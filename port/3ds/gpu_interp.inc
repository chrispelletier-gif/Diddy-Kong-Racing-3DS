/******************************** 60 Hz picture (S8 / engine P39) ********************************/

/*
 * DKR_INTERP60 (roadmap S8, engine P39; plan: proposals/3ds-interp60-plan.md). The game runs and builds its frames at
 * 30 Hz as always; each frame's GPU commands are run twice, a retrace apart:
 *  1. the in-between picture: every draw moved by its delta (interp.v.pica, dlt rows, ipk.x = 1), which takes where the
 *     draw is in this frame to where it was half a step earlier (1, interpolate) or will be half a step later
 *     (2, extrapolate). The walk (gfx_rdp.c, gfx_interp.c) works the deltas out from the game's matrices of this frame
 *     and the last, one per thing drawn, and sends them in order with the draws (gfxb_interp_call);
 *  2. the real picture: the same commands with ipk.x = 0, exactly what transform.v.pica draws.
 * The frame is made like a stereo frame made once (P33): its commands bind no target and set every setting they use,
 * so the two runs see the same state. Each run gets a short list first (the target and ipk). The second run is only
 * queued once the first, with its copy to the screen buffer, has finished (interp_queue_done, on the GSP's thread):
 * the GSP can run a copy and the next command list at the same time, and both use the one render target.
 * Showing them: two of the three top screen buffers (the triple buffer code). The in-between picture is presented when
 * its copy is done; the real one when its copy is done and the first has reached the screen, else at the next retrace
 * (interp_vblank). So the screen gets one new picture every retrace, and the frames still come every two.
 * The CPU builds each frame once: the cost is the GPU's second picture and a few hundred command words.
 */

#include "gfx/gfx_interp_math.h"

#define INTERP_LIST_WORDS 64 // Per slot: the two short lists (target, formats, ipk: ~25 words each)

static shaderProgram_s sProgramI;
static int sProgramIUsable;
static int sUnifLocT[UNIF_COUNT], sUnifLocI[UNIF_COUNT]; // Each program's uniform registers (sUnifLoc is the bound one's)
static int sEyeUniformI = -1;
static int sIpkLoc = -1;
static int sInterpCheck;    // DKR_INTERP60_CHECK
static u32 *sInterpLists[2]; // Per frame slot: INTERP_LIST_WORDS of linear memory
static int sInterpNoLists;
static int sInterpValid;    // The walk's word for this frame: its deltas are good (gfxb_interp_call, GFX_INTERP_END)
static int sInterpIdentity = 1; // The delta in force is the identity
static float sInterpDs[4][4];   // The delta in force for 3D draws, in the 3DS screen's clip space: sInterpDs[c] is dlt[c]
static const float sIdentityRows[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
static int sInterpVblankHooked;
static unsigned int sInterpStats[6]; // Pictures in pairs, single (not valid), no buffers, Y waited a retrace, deltas, check misses
static unsigned int sInterpCheckRuns;

/* The frame on the GPU (the GSP thread's side, interp_queue_done and interp_vblank). */
typedef struct InterpRun {
    u32 *list[2];          // The two short lists: [0] the in-between picture's (ipk 1), [1] the real one's (ipk 0)
    u32 listWords[2];
    u32 *part[MAX_LIST_PARTS + 1];
    u32 partWords[MAX_LIST_PARTS + 1];
    int numParts;
    u32 *colour;           // The render target's colour buffer, its size and copy flags
    u32 dim, flags;
    int buf[2];            // The top screen buffers (sTriple) of the two pictures
    void (*citroCb)(gxCmdQueue_s *); // citro3d's own queue callback (onQueueFinish), put back and called at the end
    void *citroUser;
} InterpRun;
static InterpRun sRun;

void C3Di_RenderQueueEnableVBlank(void);

/* Binds the delta-capable program's uniform registers (or transform.v.pica's) for set_unif. */
static void interp_use_locations(int interp) {
    memcpy(sUnifLoc, interp ? sUnifLocI : sUnifLocT, sizeof(sUnifLoc));
}

/* At start-up, after init_transform: interp.v.pica (the shader blob's fifth program). */
static PLAT_COLD void init_interp(void) {
    static const char *const sNames[] = { "mtx", "camoff", "texa", "texc", "texb", "fogp", "lightv", "dlt" };
    int i;
    memcpy(sUnifLocT, sUnifLoc, sizeof(sUnifLocT));
    shaderProgramInit(&sProgramI);
    sProgramIUsable = sProgramTUsable && sShaderDvlb->numDVLE >= 5 &&
                      shaderProgramSetVsh(&sProgramI, &sShaderDvlb->DVLE[4]) == 0;
    if (sProgramIUsable) {
        sEyeUniformI = shaderInstanceGetUniformLocation(sProgramI.vertexShader, "eye");
        sIpkLoc = shaderInstanceGetUniformLocation(sProgramI.vertexShader, "ipk");
        for (i = 0; i < UNIF_COUNT; i++) {
            int name = i < UNIF_CAMOFF ? 0 : i < UNIF_DLT ? i - UNIF_CAMOFF + 1 : 7;
            int first = i < UNIF_CAMOFF ? UNIF_MTX : i < UNIF_DLT ? i : UNIF_DLT;
            int loc = shaderInstanceGetUniformLocation(sProgramI.vertexShader, sNames[name]);
            sUnifLocI[i] = loc >= 0 ? loc + (i - first) : -1;
            if (sUnifLocI[i] < 0) {
                sProgramIUsable = 0;
            }
        }
        if (sEyeUniformI < 0 || sIpkLoc < 0) {
            sProgramIUsable = 0;
        }
    }
    for (i = 0; i < 2 && sProgramIUsable; i++) {
        sInterpLists[i] = linearAlloc(INTERP_LIST_WORDS * 4);
        sInterpNoLists |= sInterpLists[i] == NULL;
    }
    plat_log("60 Hz picture: shader %s, uniforms eye %d mtx %d dlt %d ipk %d", sProgramIUsable ? "found" : "NOT usable",
             sEyeUniformI, sUnifLocI[UNIF_MTX], sUnifLocI[UNIF_DLT], sIpkLoc);
}

/* The GPU is free for the next frame: no picture of the last one still to be queued or drawn (wait_for_gpu). */
static inline int interp_gpu_free(void) {
    return sInterpChain != 1 && sInterpChain != 2;
}

/* DKR_INTERP60_CHECK: a test vertex moved by the walk's delta and put on the screen, against put on the screen and
 * moved by the converted one (gfx_interp_math.h). */
PLAT_COLD static void interp_check(const float d[4][4], const float rows[4][4], float s) {
    int miss = gfx_interp_screen_check(d, rows, s);
    sInterpCheckRuns++;
    sInterpStats[5] += miss;
    plat_check_report("60 Hz picture backend", miss); // The race's checks_run / checks_wrong (CSV "chk" cells)
}

/* The walk's calls (gfx_backend.h): a delta for the draws after it, or the frame's verdict just before its end. */
void gfxb_interp_call(void *payload) {
    const GfxInterpCall *c = (const GfxInterpCall *) payload;
    if (c->kind == GFX_INTERP_END_VALID || c->kind == GFX_INTERP_END_NOT) {
        sInterpValid = c->kind == GFX_INTERP_END_VALID;
        return;
    }
    if (!sInterpFrame) {
        return;
    }
    if (c->kind == GFX_INTERP_IDENTITY) {
        sInterpIdentity = 1;
        return;
    }
    gfx_interp_to_screen(c->d, sScale3D, sInterpDs, sInterpCheck == 2);
    sInterpIdentity = 0;
    sInterpStats[4]++;
    if (sInterpCheck) {
        interp_check(c->d, sInterpDs, sScale3D);
    }
}

/* Per draw (set_all_state), in a frame made for two runs: the delta rows, the identity for flat draws. */
static void PLAT_COLD __attribute__((noinline)) interp_draw_now(int flat) {
    const float (*d)[4] = (flat || sInterpIdentity) ? sIdentityRows : (const float (*)[4]) sInterpDs;
    int c;
    for (c = 0; c < 4; c++) {
        set_unif(UNIF_DLT + c, d[c][0], d[c][1], d[c][2], d[c][3]);
    }
}

/* The switches, with the frame's others (read_switches). */
static void interp_read_switches(void) {
    int wanted = plat_switch("DKR_INTERP60", 0);
    sInterpCheck = plat_switch("DKR_INTERP60_CHECK", 0);
    if (wanted != sInterpWanted) {
        plat_log("60 Hz picture: %s", wanted == 1 ? "on (interpolated)" : wanted == 2 ? "on (extrapolated)" : "off");
    }
    sInterpWanted = wanted == 1 || wanted == 2 ? wanted : 0;
    if (!sInterpWanted && sInterpVblankHooked && sInterpChain == 0) {
        C3Di_RenderQueueEnableVBlank(); // citro3d's own retrace counter back
        sInterpVblankHooked = 0;
    }
}

/* Whether the frame about to be built is made for two runs (gfxb_start_frame, after apply_screen_mode). */
static int interp_frame_wanted(void) {
    static int sWhy = -1;
    int why = !sInterpWanted                                           ? 0
              : !sProgramIUsable || sInterpNoLists                     ? 1
              : !(sTransformWanted && sProgramTUsable && !sTransformHung) ? 2
              : sStereo || s800                                        ? 3
              : plat_frame_retraces() != 2                             ? 4
              : sSplitMode != 0 || sRecentOn || sSnapshotPending ? 5
              : sTripleFail                                            ? 6
                                                                       : -2;
    if (why != sWhy && sInterpWanted) {
        static const char *const sWhyText[] = { "", "no shader", "GPU vertex maths off", "3D or 800 pixels",
                                                "not at the 30 fps cap", "a diagnostic is on", "no third screen buffer" };
        plat_log("60 Hz picture: %s", why == -2 ? "two pictures a frame" : sWhyText[why]);
    }
    sWhy = why;
    return why == -2;
}

/* The short list before a run: the target, its formats, and ipk. */
static u32 interp_list(u32 *at, float k, u32 **start) {
    C3D_FVec v;
    u32 *words;
    u32 n;
    bind_target(sTarget);
    bind_target_formats(sTarget);
    v.x = k;
    v.y = 0.0f;
    v.z = 0.0f;
    v.w = 0.0f;
    GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000u | (u32) sIpkLoc);
    GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, (u32 *) v.c, 4);
    GPUCMD_Split(&words, &n);
    *start = words;
    (void) at;
    return n;
}

/* gfxb_end_frame, where a stereo frame made once makes its eyes' lists: the rest of the frame becomes a part, and the
 * two short lists are made. Returns 0 if they couldn't be (the frame is then drawn once, with the real picture's list
 * if there is one). */
static int interp_lists(FrameSlot *slot) {
    u32 *body, bodyWords, *saved, savedSize;
    u32 *lists = sInterpLists[slot - sSlots];
    if (lists == NULL || slot->numParts + 1 > MAX_LIST_PARTS || !C3Di_SplitFrame(&body, &bodyWords)) {
        return 0;
    }
    slot->partAddr[slot->numParts] = body;
    slot->partWords[slot->numParts++] = bodyWords;
    saved = gpuCmdBuf;
    savedSize = gpuCmdBufSize;
    GPUCMD_SetBuffer(lists, INTERP_LIST_WORDS, 0);
    sRun.listWords[0] = interp_list(lists, 1.0f, &sRun.list[0]);
    sRun.listWords[1] = interp_list(lists, 0.0f, &sRun.list[1]);
    GPUCMD_SetBuffer(saved, savedSize, 0);
    return 1;
}

/* GSP thread: what libctru's swap does for the top screen (the pictures shown count), then buffer i presented. */
static void interp_present(int i) {
    u64 now = svcGetSystemTick();
    int pending = gspIsPresentPending(GSP_SCREEN_TOP);
    if (pending) {
        sSwapReplaced++;
    }
    plat_shown_swap(&sShown, pending, now, SYSCLOCK_ARM11 / 1000000);
    sSwapGapUs = sLastSwapTick != 0 ? (unsigned int) ((now - sLastSwapTick) / (SYSCLOCK_ARM11 / 1000000)) : 0;
    sLastSwapTick = now;
    sSwapCount++;
    triple_present_buf(i, false);
}

/* GSP thread: the queue has finished a run. */
static void interp_queue_done(gxCmdQueue_s *queue) {
    int i;
    if (sInterpChain == 1) {
        // The in-between picture is in its screen buffer: shown at the next retrace. The real picture's run now.
        interp_present(sRun.buf[0]);
        sInterpChain = 2;
        GX_ProcessCommandList(sRun.list[1], sRun.listWords[1] * 4, 0); // Flushed with the first run
        for (i = 0; i < sRun.numParts; i++) {
            GX_ProcessCommandList(sRun.part[i], sRun.partWords[i] * 4, 0);
        }
        GX_DisplayTransfer(sRun.colour, sRun.dim, (u32 *) sTriple[sRun.buf[1]], sRun.dim, sRun.flags);
        return;
    }
    if (sInterpChain == 2) {
        queue->callback = sRun.citroCb;
        queue->user = sRun.citroUser;
        if (!gspIsPresentPending(GSP_SCREEN_TOP)) {
            interp_present(sRun.buf[1]);
            sInterpChain = 0;
        } else {
            sInterpChain = 3; // interp_vblank, once the in-between picture is on the screen
        }
        if (sRun.citroCb != NULL) {
            sRun.citroCb(queue); // citro3d's: the GPU time of both runs
        }
    }
}

/* GSP thread, every retrace while the 60 Hz picture is on: the real picture, if it waits for the in-between one. */
static void interp_vblank(void *unused) {
    (void) unused;
    if (sInterpChain == 3 && !gspIsPresentPending(GSP_SCREEN_TOP)) {
        sInterpStats[3]++;
        interp_present(sRun.buf[1]);
        sInterpChain = 0;
    }
}

/* Two free top screen buffers for the pictures, waiting up to a retrace for the last real picture to be shown. */
static int interp_pick_buffers(int *a, int *b) {
    int i, k, n = 0, got[2] = { -1, -1 };
    for (k = 0; k < 40 && sInterpChain != 0; k++) {
        svcSleepThread(500 * 1000); // The last real picture still waits for its retrace
    }
    if (sInterpChain != 0 || !triple_alloc()) {
        return 0;
    }
    for (k = 0; k < 40; k++) {
        if (sTriplePresented >= 0 && !gspIsPresentPending(GSP_SCREEN_TOP)) {
            sTripleShown = sTriplePresented;
            sTripleMaybe = -1;
        }
        n = 0;
        for (i = 0; i < 3 && n < 2; i++) {
            if (i != sTripleShown && i != sTripleMaybe && i != sTriplePresented) {
                got[n++] = i;
            }
        }
        if (n == 2) {
            break;
        }
        svcSleepThread(500 * 1000);
    }
    if (n < 2) {
        return 0;
    }
    *a = got[0];
    *b = got[1];
    return 1;
}

/*
 * gfxb_end_frame's hand-over of a frame made for two runs (after pace_frame). Returns 1 if it queued both pictures
 * (and ended citro3d's frame); 0: the frame is to be handed over as one picture, which the caller does after this
 * queued the real picture's short list.
 */
static int interp_hand_over(FrameSlot *slot, int listsMade) {
    int i, a, b;
    gxCmdQueue_s *queue = &__C3D_Context.gxQueue;
    if (!listsMade) {
        return 0;
    }
    if (!sInterpValid || !interp_pick_buffers(&a, &b)) {
        sInterpStats[sInterpValid ? 2 : 1]++;
        GX_ProcessCommandList(sRun.list[1], sRun.listWords[1] * 4, GX_CMDLIST_FLUSH);
        return 0;
    }
    if (!sInterpVblankHooked) {
        gspSetEventCallback(GSPGPU_EVENT_VBlank0, interp_vblank, NULL, false); // citro3d's retrace counter is unused
        sInterpVblankHooked = 1;
    }
    sRun.numParts = slot->numParts;
    for (i = 0; i < slot->numParts; i++) {
        sRun.part[i] = slot->partAddr[i];
        sRun.partWords[i] = slot->partWords[i];
    }
    sRun.colour = (u32 *) sTarget->frameBuf.colorBuf;
    sRun.dim = GX_BUFFER_DIM((u32) sTarget->frameBuf.width, (u32) sTarget->frameBuf.height);
    sRun.flags = sTarget->transferFlags;
    sRun.buf[0] = a;
    sRun.buf[1] = b;
    sRun.citroCb = queue->callback;
    sRun.citroUser = queue->user;
    // The first run: its list, the frame, and the copy to its screen buffer. interp_queue_done queues the second.
    GX_ProcessCommandList(sRun.list[0], sRun.listWords[0] * 4, GX_CMDLIST_FLUSH);
    for (i = 0; i < sRun.numParts; i++) {
        GX_ProcessCommandList(sRun.part[i], sRun.partWords[i] * 4, GX_CMDLIST_FLUSH);
    }
    GX_DisplayTransfer(sRun.colour, sRun.dim, (u32 *) sTriple[a], sRun.dim, sRun.flags);
    sInterpChain = 1;
    queue->callback = interp_queue_done;
    queue->user = NULL;
    sTarget->used = false; // citro3d copies and swaps nothing itself
    sTripleFrame = 0;
    sTripleQueued = sTripleSwaps;
    sInterpStats[0]++;
    C3D_FrameEnd(GX_CMDLIST_FLUSH); // Runs the queue
    if ((sInterpStats[0] + sInterpStats[1] + sInterpStats[2]) % 600 == 0) {
        plat_log("60 Hz picture: %u frames as two pictures, %u as one (camera cut or not valid), %u without free "
                 "buffers, %u real pictures waited a retrace, %u deltas%s",
                 sInterpStats[0], sInterpStats[1], sInterpStats[2], sInterpStats[3], sInterpStats[4], "");
        if (sInterpCheck) {
            plat_log("60 Hz picture check: %u deltas checked, %u wrong", sInterpCheckRuns, sInterpStats[5]);
        }
    }
    return 1;
}

/* For the bench results and the self-check line: deltas checked and wrong. */
void gfxb_interp_check_counts(unsigned int *checked, unsigned int *wrong) {
    *checked = sInterpCheckRuns;
    *wrong = sInterpStats[5];
}

/* The walk, at its frame start: whether to work deltas out this frame (gfx_backend.h). Read on the walk's thread; a
 * change of a switch lands a frame late on one side at most, and a frame the backend doesn't make for two runs just
 * ignores the deltas. */
int gfxb_interp_frame(void) {
    return sInterpWanted && sProgramIUsable && !sInterpNoLists && !sStereo && !s800 ? sInterpWanted : 0;
}
