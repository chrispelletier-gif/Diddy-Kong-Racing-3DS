/*
 * Emulation of the signal processor (RSP) and display processor (RDP).
 * Tasks handed to osSpTaskStartGo are executed on a dedicated thread, which then raises the same SP and DP
 * interrupts the hardware would.
 */
#include <ultra64.h>
#include "types.h"
#include "port.h"
#include "plat/plat.h"
#include "plat/profile.h"
int port_frames_native(void);
void port_frames_gfx_done(void);

#define RCP_QUEUE_SIZE 8

/*
 * Two workers stand in for the RCP. Graphics tasks go to the "RDP" worker; since the RSP's share of a graphics task
 * is quick on the N64, the SP interrupt is raised as soon as the worker picks the task up and only the DP interrupt waits
 * for the frame to be drawn. That leaves the scheduler free to start audio tasks while a frame renders, which run on
 * the second worker, so a slow frame never starves the audio.
 */
/* A task for a worker: an OSTask (the scheduler's way), or (R5, DKR_FRAME_DIRECT) a frame's record buffer alone. */
typedef struct RcpItem {
    OSTask *task;
    const void *dl;
    s32 solo; // The frame runs with the two-core renderer's calls made directly (placement 2)
    s32 texOnly; // P53 (DKR_LOAD_PREWARM): no frame, the textures queued so far decoded between frames
    s32 drain;   // texOnly: also wait for the backend to upload them
} RcpItem;

typedef struct RcpWorker {
    RcpItem queue[RCP_QUEUE_SIZE];
    volatile u32 read;
    volatile u32 write;
    PlatSem *sem;
} RcpWorker;

static OSTask *sLoadedTask;
static RcpWorker sGfxWorker;
static RcpWorker sAudioWorker;
static RcpWorker sSoloWorker; // R5 placement 2 on an Old 3DS: the "rdp solo" thread, off the game's core
static s32 sSoloStarted;
static s32 sRcpStarted;
static volatile s32 sGfxTasksPending;
// Graphics tasks handed over and finished so far (kernel lock), for waiting on the ones queued at a given moment.
static volatile u32 sGfxTasksSubmitted, sGfxTasksFinished;
/* Each display list's logic step (gPortLogicStep), by the order they were handed over; they are drawn in that order. */
#define GFX_STEP_RING 8
static u8 sGfxSteps[GFX_STEP_RING];
static unsigned int sGfxTexHeads[GFX_STEP_RING]; // R3: the texture queue's head as each frame was handed over
/* Tasks handed to the workers whose completion has not been raised yet (kernel lock). */
static s32 sRcpTasksBusy;
/* P53: the worker that took the last frame (a textures-only item goes there, after it, never beside a frame), and
 * the textures-only items asked for and done (kernel lock). */
static RcpWorker *sLastFrameWorker = &sGfxWorker;
static volatile u32 sTexOnlyAsked, sTexOnlyDone, sTexOnlyTarget;
/* What the audio worker is doing, for the 3DS watchdog's stall report. */
static const char *volatile sAudioStage = "not started";

static RcpItem worker_next(RcpWorker *worker) {
    RcpItem item;
    plat_sem_wait(worker->sem);
    item = worker->queue[worker->read % RCP_QUEUE_SIZE];
    worker->read++;
    return item;
}

/* The game thread (P53's textures-only items) and the scheduler's both hand items to the "rdp" worker: the slot is
 * taken and filled under the kernel lock. */
static void worker_enqueue(RcpWorker *worker, const RcpItem *src) {
    plat_klock();
    sRcpTasksBusy++;
    worker->queue[worker->write % RCP_QUEUE_SIZE] = *src;
    __sync_synchronize();
    worker->write++;
    if (worker != &sAudioWorker && !src->texOnly) {
        sLastFrameWorker = worker;
    }
    plat_kunlock();
    plat_sem_post(worker->sem);
}

static void worker_push(RcpWorker *worker, OSTask *task, const void *dl, s32 solo) {
    RcpItem item;
    item.task = task;
    item.dl = dl;
    item.solo = solo;
    item.texOnly = FALSE;
    item.drain = FALSE;
    worker_enqueue(worker, &item);
}

/* Called after a task's completion event is raised, so lockstep never sees the RCP idle before it. */
static void task_done(void) {
    plat_klock();
    sRcpTasksBusy--;
    plat_kbroadcast();
    plat_kunlock();
}

void port_describe_rcp(char *buffer, s32 size) {
    plat_snprintf(buffer, size, "rcp: gfx frames %u taken of %u, solo frames %u taken of %u, audio tasks %u taken of %u, "
                  "%d busy, %d gfx pending, audio worker %s", (unsigned) sGfxWorker.read, (unsigned) sGfxWorker.write,
                  (unsigned) sSoloWorker.read, (unsigned) sSoloWorker.write, (unsigned) sAudioWorker.read,
                  (unsigned) sAudioWorker.write, (int) sRcpTasksBusy, (int) sGfxTasksPending, sAudioStage);
}

int port_rcp_idle(void) {
    return sRcpTasksBusy == 0;
}

/*
 * The render ahead the game thread works with: the setting, taken up only at the end of a frame (gfxtask_wait_ahead),
 * and going from 2 down to 1 only once no frame is in flight. A frame still being drawn may be using any of the three
 * per-frame geometry copies, and with two copies the next one could be its copy.
 */
static s32 sGameAhead;

s32 port_render_ahead(void) {
    if (sGameAhead == 0) {
        sGameAhead = plat_render_ahead();
    }
    return sGameAhead;
}

/* Game thread, at the end of a frame: whether the setting can be taken up now (FALSE: drain the frames in flight). */
s32 port_render_ahead_update(s32 framesInFlight) {
    s32 want = plat_render_ahead();
    if (want >= port_render_ahead() || framesInFlight == 0) {
        sGameAhead = want;
        return TRUE;
    }
    return FALSE;
}

/* The next of the game's per-frame geometry copies (FRAME_FLIP): two in turn as on the N64, three when two frames may be in flight. */
int port_frame_flip(int flip) {
    return (flip + 1) % (port_render_ahead() + 1);
}

/* v31: the walk's time so far and the start of the one running (0: none), for the profiler's sections, which count
 * what of each section the walk took from the game thread on the same core. */
volatile unsigned long long gPortWalkTotalNs, gPortWalkStartNs;

/*
 * A renderer thread: "rdp" (sGfxWorker) and, for R5's placement 2 on an Old 3DS, "rdp solo" (sSoloWorker). A frame comes
 * as an OSTask (osSpTaskStartGo, the scheduler's way) or, with DKR_FRAME_DIRECT, as its record buffer alone
 * (port_rcp_frame_start): then no SP or DP event exists for it, only the native frame queue's call.
 */
static void frame_thread(void *arg) {
    RcpWorker *worker = (RcpWorker *) arg;
    // The walk's clock for the profiler's shared core time (sec_other_now): only a thread on the game's core.
    s32 gameCore = worker == &sGfxWorker;

    port_int_thread_init();
    port_gfx_init();
    for (;;) {
        unsigned long long start;
        RcpItem item;
        const void *dl;
        gPlatGfxStage = "waiting for a display list";
        item = worker_next(worker);
        if (item.texOnly) {
            // P53: no frame, so no SP or DP event, no frame count and no profiler walk time.
            gPlatGfxStage = "decoding textures during a load";
            port_gfx_textures_between_frames(item.drain);
            plat_klock();
            sTexOnlyDone++;
            plat_kbroadcast();
            plat_kunlock();
            task_done();
            continue;
        }
        if (item.task != NULL && !port_frames_native()) { // the native frame queue has no RSP to free
            port_raise_event(OS_EVENT_SP);
        }
        dl = item.task != NULL ? (const void *) item.task->t.data_ptr : item.dl;
        plat_set_frame_step(sGfxSteps[sGfxTasksFinished % GFX_STEP_RING]);
        port_gfx_frame_textures(sGfxTexHeads[sGfxTasksFinished % GFX_STEP_RING]);
        start = plat_time_ns();
        if (gameCore) {
            gPortWalkStartNs = start;
        }
        port_gfx_run_frame(dl, item.solo);
        {
            unsigned long long spent = plat_time_ns() - start;
            if (gameCore) {
                gPortWalkTotalNs += spent;
                gPortWalkStartNs = 0;
            }
            prof_render_task(spent);
        }
        port_load_frame_done();
        gPlatCounters[PLAT_COUNTER_GFX_DONE]++;
        gPlatGfxStage = "raising the DP interrupt";
        plat_klock();
        sGfxTasksPending--;
        sGfxTasksFinished++;
        plat_kbroadcast();
        plat_kunlock();
        if (port_frames_native()) {
            port_frames_gfx_done(); // the native frame queue (frames.c)
        } else {
            port_raise_event(OS_EVENT_DP);
        }
        task_done();
    }
}

static void audio_thread(void *arg) {
    OSTask *task;

    (void) arg;
    port_int_thread_init();
    for (;;) {
        sAudioStage = "waiting for a task";
        task = worker_next(&sAudioWorker).task;
        sAudioStage = "mixing";
        if (task->t.type == M_AUDTASK) {
            unsigned long long start = plat_time_ns();
            port_audio_run_task(task);
            prof_add(PROF_AUDIO, plat_time_ns() - start);
            gPlatCounters[PLAT_COUNTER_AUDIO]++;
        }
        sAudioStage = "raising the SP interrupt";
        port_raise_event(OS_EVENT_SP);
        task_done();
    }
}

void port_rcp_init(void) {
    if (!sRcpStarted) {
        sRcpStarted = TRUE;
        sGfxWorker.sem = plat_sem_create(0);
        sAudioWorker.sem = plat_sem_create(0);
        plat_thread_create(frame_thread, &sGfxWorker, 0, PLAT_THREAD_RCP, "rdp");
        plat_thread_create(audio_thread, NULL, 0, PLAT_THREAD_AUDIO, "rsp");
    }
}

void osSpTaskLoad(OSTask *task) {
    sLoadedTask = task;
}

void osSpTaskStartGo(OSTask *task) {
    if (task == NULL) {
        task = sLoadedTask;
    }
    port_rcp_init();
    if (task->t.type == M_GFXTASK) {
        worker_push(&sGfxWorker, task, NULL, FALSE);
    } else {
        worker_push(&sAudioWorker, task, NULL, FALSE);
    }
    if (port_lockstep_enabled()) {
        // Lockstep: the task finishes, and raises its interrupts, before the caller goes on.
        int wasDisabled = port_int_release();
        plat_klock();
        while (sRcpTasksBusy > 0) {
            plat_kwait();
        }
        plat_kunlock();
        port_int_restore(wasDisabled);
    }
}

/*
 * R5 (DKR_FRAME_DIRECT, frames.c): a frame's record buffer straight to a renderer thread, without an OSTask. Called by
 * the native frame queue only with no frame on the renderer, so changing threads between frames is safe.
 * direct 1: the "rdp" thread, as today. direct 2: the frame (dispatcher and GPU commands) on one thread off the game's
 * core: "rdp solo" on the system core where "rdp" shares the game's (an Old 3DS), else "rdp" with the two-core
 * renderer's calls made directly.
 */
void port_rcp_frame_start(const void *dl, s32 direct) {
    s32 solo = direct == 2;
    port_rcp_init();
    if (solo && plat_frame_solo_thread()) {
        // The system core's share comes first, as for the two-core renderer's second thread: without it a thread
        // can't be made there. Its frames keep it (gfx_split_frame_start's holdCores).
        plat_split_render_cores(TRUE);
        if (!sSoloStarted) {
            sSoloStarted = TRUE;
            sSoloWorker.sem = plat_sem_create(0);
            plat_thread_create(frame_thread, &sSoloWorker, 0, PLAT_THREAD_RCP_SOLO, "rdp solo");
            plat_log("frames: the whole frame on its own thread (rdp solo), off the game's core");
        }
        worker_push(&sSoloWorker, NULL, dl, TRUE);
        return;
    }
    worker_push(&sGfxWorker, NULL, dl, solo);
}

/*
 * P53 (DKR_LOAD_PREWARM, port_loadtime.c): game thread, during a level load. The renderer thread that took the last
 * frame decodes the textures the load has queued so far (and with drain set, waits for the backend to upload them),
 * after that frame, so the first frames after the load don't. Waits until it is done: the decodes read the texture
 * data out of the game's memory, which the load must not reuse meanwhile. Nothing to do before the first frame.
 */
static int texonly_idle(void) {
    return (s32) (sTexOnlyDone - sTexOnlyTarget) >= 0;
}

void port_rcp_textures_now(s32 drain) {
    RcpItem item;
    int wasDisabled;
    if (!sRcpStarted) {
        return;
    }
    // The frames handed over so far finish first, so no frame (R5's native queue starts the next one from the
    // renderer thread, maybe on the other worker) can run beside the item.
    port_rcp_wait_idle();
    plat_klock();
    sTexOnlyTarget = ++sTexOnlyAsked;
    plat_kunlock();
    item.task = NULL;
    item.dl = NULL;
    item.solo = FALSE;
    item.texOnly = TRUE;
    item.drain = drain;
    worker_enqueue(sLastFrameWorker, &item);
    // As port_rcp_wait_idle: frames ahead of the item finish only if the VI and RCP threads can raise their events.
    wasDisabled = port_int_release();
    plat_klock();
    port_kwait_until(texonly_idle);
    plat_kunlock();
    port_int_restore(wasDisabled);
}

void osSpTaskYield(void) {
    // Emulated tasks always run to completion.
}

OSYieldResult osSpTaskYielded(OSTask *task) {
    (void) task;
    return 0;
}

/* Called by the game when it hands a display list to the scheduler. */
void port_rcp_gfx_submitted(void) {
    gPlatCounters[PLAT_COUNTER_GFX_QUEUED]++;
    plat_klock();
    sGfxTasksPending++;
    sGfxSteps[sGfxTasksSubmitted % GFX_STEP_RING] = (u8) gPortLogicStep;
    sGfxTexHeads[sGfxTasksSubmitted % GFX_STEP_RING] = port_gfx_texture_queue_head();
    sGfxTasksSubmitted++;
    plat_kunlock();
}

/*
 * Waits until the graphics tasks queued or running now have finished. The game sometimes frees memory that the
 * display list being drawn still uses, relying on the RCP having finished long before. This makes that safe however
 * slow rendering is. Tasks handed over later can't use memory freed now, so they aren't waited for: the tracks
 * menu's background loader frees a lot while the game thread keeps queueing menu frames, and waiting for no task at
 * all could take seconds, or forever with frames rendered ahead.
 */
static volatile u32 sGfxWaitTarget; // The newest task any waiter needs finished (kernel lock).

static int gfx_idle(void) {
    return (s32) (sGfxTasksFinished - sGfxWaitTarget) >= 0;
}

void port_rcp_wait_idle(void) {
    int wasDisabled;
    if (sGfxTasksPending <= 0) {
        return;
    }
    // Callers may have "interrupts" disabled, but the frame can only finish if the VI and RCP threads can still
    // raise their events, so let them in while waiting (as the OS does for any blocking call).
    wasDisabled = port_int_release();
    plat_klock();
    if ((s32) (sGfxTasksSubmitted - sGfxWaitTarget) > 0) {
        sGfxWaitTarget = sGfxTasksSubmitted;
    }
    port_kwait_until(gfx_idle);
    plat_kunlock();
    port_int_restore(wasDisabled);
}
