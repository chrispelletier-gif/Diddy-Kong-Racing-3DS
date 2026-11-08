/*
 * The native frame queue (PICA_ALL test build, DKR_NATIVE_FRAMES, on by default there; off in lockstep runs).
 *
 * Replaces the N64 scheduler's part in getting a frame to the screen. On the N64 (and in the port until now) the game
 * sent its display list task as a message to the scheduler, which queued it in its RSP/RDP state machine, started it
 * on the emulated RCP, was told by SP and DP interrupt messages when it was done, sent the game a done message (held
 * back until the frame cap's retraces had passed), and sent the video and audio threads a message at every retrace.
 *
 * Here: the game hands the task to this queue, which starts it on the renderer as soon as the renderer is free and no
 * screen swap is pending; the renderer reports back with a call; the game and its audio thread wait on counters of
 * finished frames and retraces under the port's own lock and condition variable. The frame cap hold, the one held
 * frame slot (render ahead) and the done message's screen status are the scheduler's, so the pacing is the same.
 */
#include <ultra64.h>
#include <PR/sched.h>
#include "types.h"
#include "port.h"
#include "plat/plat.h"
#include "plat/recorder.h"

#ifdef PICA_ALL

extern s32 D_800DE730[];      /* the scheduler's usual done message: swap the buffer */
extern s32 gRetraceCounter32;
extern u64 gRetraceCounter64;
void port_audio_run_task(void *taskPtr);

#define QUEUE_SIZE 8
#define DONE_SIZE 8  /* the game's done queue held 8 messages */
#define TICKS_MAX 8  /* and its video and audio retrace queues 8 each */

static s32 sNative = -1;
static OSScTask *sQueue[QUEUE_SIZE];
static s32 sQueueHead, sQueueCount;
static OSScTask *sRunning; /* on the renderer */
static OSScTask *sHeld;    /* a finished frame held back for the frame cap */
static s32 sFrameCount;    /* retraces since the last frame was let through */
static OSMesg sDone[DONE_SIZE];
static s32 sDoneHead, sDoneCount;
static s32 sVideoTicks, sAudioTicks;
static u32 sRetraces;
static u32 sOverflows;

/*
 * v36: how late the game ran after the retrace that let its frame end. v35 put the GSP event thread above the walk,
 * and lean quick test #465 had no late retraces left, but the 38-39 ms frames stayed ("waiting 22 ms for the
 * retrace"): so the retrace came on time and the game thread, below the walk on an Old 3DS's game core, ran later.
 * Each retrace keeps when it came and what the walk was doing then; a game thread that waited for it and ran more
 * than 3 ms later counts a late wake (late_wakes in the test run's results) and says in the flight recorder how much
 * of the gap the walk had.
 */
extern volatile unsigned long long gPortWalkTotalNs, gPortWalkStartNs;
static unsigned long long sRetraceNs, sRetraceWalkNs, sRetraceWalkStartNs;
static const char *sRetraceStage;

static unsigned long long walk_ns_at(unsigned long long now, unsigned long long *start) {
    unsigned long long walkStart = gPortWalkStartNs;
    *start = walkStart;
    return gPortWalkTotalNs + (walkStart != 0 && now > walkStart ? now - walkStart : 0);
}

int port_frames_native(void) {
    if (sNative < 0) {
        sNative = plat_switch("DKR_NATIVE_FRAMES", 1) != 0 && !port_lockstep_enabled();
        if (sNative) {
            plat_log("frames: native frame queue (no scheduler)");
        }
    }
    return sNative;
}

/* Kernel lock held. */
static void reply_locked(OSScTask *t) {
    OSMesg msg = (t->unk68 || t->msg) ? t->msg : (OSMesg) D_800DE730;
    if (sDoneCount == DONE_SIZE) {
        sOverflows++;
        return;
    }
    sDone[(sDoneHead + sDoneCount) % DONE_SIZE] = msg;
    sDoneCount++;
}

/* The scheduler's __scTaskComplete for a finished task. Kernel lock held. */
static void complete_locked(OSScTask *t) {
    if (t->msgQ == NULL) {
        return;
    }
    if (t->flags & OS_SC_LAST_TASK) {
        if (sHeld != NULL) {
            // A frame already waits for the cap: a second one finished before it (render ahead), so it goes now.
            reply_locked(sHeld);
            sHeld = NULL;
        }
        if (sFrameCount < plat_frame_retraces()) {
            sHeld = t;
            return;
        }
        reply_locked(t);
        sFrameCount = 0;
        return;
    }
    reply_locked(t);
}

/* The next task to start, if the renderer is free and no swap waits for a retrace (__scTaskReady). Kernel lock held. */
static OSScTask *take_ready_locked(void) {
    OSScTask *t;
    if (sRunning != NULL || sQueueCount == 0 || osViGetCurrentFramebuffer() != osViGetNextFramebuffer()) {
        return NULL;
    }
    t = sQueue[sQueueHead];
    sQueueHead = (sQueueHead + 1) % QUEUE_SIZE;
    sQueueCount--;
    sRunning = t;
    return t;
}

static void start(OSScTask *t) {
    if (t != NULL) {
        osSpTaskLoad(&t->list);
        osSpTaskStartGo(&t->list);
    }
}

/* gfxtask_run_xbus: the game's display list task. */
void port_frames_submit(void *task) {
    OSScTask *t = (OSScTask *) task, *go;
    t->next = NULL;
    t->state = t->flags & OS_SC_RCP_MASK;
    plat_klock();
    if (sQueueCount == QUEUE_SIZE) {
        sOverflows++;
        plat_kunlock();
        return;
    }
    sQueue[(sQueueHead + sQueueCount) % QUEUE_SIZE] = t;
    sQueueCount++;
    go = take_ready_locked();
    plat_kunlock();
    start(go);
}

/* The renderer finished the task it was given (os_sp.c, instead of raising the DP interrupt). */
void port_frames_gfx_done(void) {
    OSScTask *go;
    plat_klock();
    if (sRunning != NULL) {
        sRunning->state = 0;
        complete_locked(sRunning);
        sRunning = NULL;
    }
    go = take_ready_locked();
    plat_kbroadcast();
    plat_kunlock();
    start(go);
}

/* A retrace (os_vi.c, instead of the scheduler's retrace message). */
void port_frames_retrace(void) {
    OSScTask *go;
    unsigned long long now = plat_time_ns(), walkStart;
    unsigned long long walkNs = walk_ns_at(now, &walkStart);
    plat_klock();
    sRetraceNs = now;
    sRetraceWalkNs = walkNs;
    sRetraceWalkStartNs = walkStart;
    sRetraceStage = gPlatGfxStage;
    sRetraces++;
    gRetraceCounter64++;
    gRetraceCounter32++;
    go = take_ready_locked(); // the swap may have happened at this retrace
    sFrameCount++;
    if (sHeld != NULL && sFrameCount >= plat_frame_retraces()) {
        reply_locked(sHeld);
        sHeld = NULL;
        sFrameCount = 0;
    }
    if (sVideoTicks < TICKS_MAX) {
        sVideoTicks++;
    }
    if ((sRetraces % 2) == 0 && sAudioTicks < TICKS_MAX) {
        sAudioTicks++; // the audio thread ran every other retrace, as on the N64
    }
    plat_kbroadcast();
    plat_kunlock();
    start(go);
}

/* gfxtask_wait: the next finished frame's done message (the screen status is its second word). */
OSMesg port_frames_wait_done(void) {
    OSMesg msg;
    plat_klock();
    while (sDoneCount == 0) {
        plat_kwait();
    }
    msg = sDone[sDoneHead];
    sDoneHead = (sDoneHead + 1) % DONE_SIZE;
    sDoneCount--;
    plat_kunlock();
    return msg;
}

/* fb_update: the retraces since it last looked, without waiting. */
s32 port_frames_video_take(void) {
    s32 n;
    plat_klock();
    n = sVideoTicks;
    sVideoTicks = 0;
    plat_kunlock();
    return n;
}

/* fb_update: waits for the next retrace. */
void port_frames_video_wait(void) {
    unsigned long long retraceNs = 0, retraceWalkNs = 0, retraceWalkStartNs = 0, now, walkNs, walkStart;
    const char *stage = NULL;
    int waited = 0;
    plat_klock();
    while (sVideoTicks == 0) {
        plat_kwait();
        waited = 1;
    }
    sVideoTicks--;
    if (waited) {
        retraceNs = sRetraceNs;
        retraceWalkNs = sRetraceWalkNs;
        retraceWalkStartNs = sRetraceWalkStartNs;
        stage = sRetraceStage;
    }
    plat_kunlock();
    if (!waited) {
        return;
    }
    now = plat_time_ns();
    if (now - retraceNs > 3000000ULL) {
        walkNs = walk_ns_at(now, &walkStart);
        gPlatCounters[PLAT_COUNTER_LATE_WAKES]++;
        rec_event("late wake %.1f ms, walk %.1f ms of it; at the retrace %s, %s",
                  (now - retraceNs) / 1e6, (walkNs - retraceWalkNs) / 1e6, stage != NULL ? stage : "-",
                  retraceWalkStartNs != 0 ? "walking" : "no walk");
        (void) walkNs; // (Unused where the recorder is left out: release builds)
        (void) stage;
        (void) retraceWalkStartNs;
        (void) retraceWalkNs;
    }
}

/* The audio thread's beat: every other retrace. */
void port_frames_audio_wait(void) {
    plat_klock();
    while (sAudioTicks == 0) {
        plat_kwait();
    }
    sAudioTicks--;
    plat_kunlock();
}

/* An audio task (the CPU mixer's command list, only when the DSP voices are off): mixed on the calling thread. */
void port_frames_run_audio(void *task) {
    OSScTask *t = (OSScTask *) task;
    port_audio_run_task(&t->list);
    if (t->msgQ != NULL) {
        osSendMesg(t->msgQ, t->msg, OS_MESG_NOBLOCK);
    }
}

void port_describe_frames(char *buffer, s32 size) {
    plat_snprintf(buffer, size, "frames: %s, %d queued, %s, %s held, %d done waiting, video %d audio %d, %u overflows",
                  sNative > 0 ? "native" : "scheduler", (int) sQueueCount, sRunning ? "drawing" : "idle",
                  sHeld ? "one" : "none", (int) sDoneCount, (int) sVideoTicks, (int) sAudioTicks,
                  (unsigned) sOverflows);
}

#endif
