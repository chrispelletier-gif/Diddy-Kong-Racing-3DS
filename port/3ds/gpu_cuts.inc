/*
 * New 3DS GPU time ("Cut New 3DS GPU time" thread, 2026-11-07). At 800 pixels with 2x anti-aliasing the GPU is what
 * holds a New 3DS back (switch bench #444: 14-16 ms a frame, the limit in every race), and the bench's cost rows put
 * about 5 ms of it outside the track, racers, objects and sky. Included once by gfx_c3d.c, after split_command_list.
 *
 *  1. The GPU timeline (DKR_GPU_TIMELINE, on): when each GPU job of a frame finished (the memory fills, each part of
 *     the command list, the copy to the screen), from the GSP's events, so a test run shows where the GPU's time
 *     goes instead of one number. One frame in GPU_KIND_EVERY also has its command list cut where the kind of draw
 *     changes (gfx_rdp.c sends the kinds), which times each kind on the GPU (DKR_GPU_KINDS, on).
 *  2. The colour clear as a memory fill (DKR_COLOUR_FILL, on by default on a New 3DS). Every frame the game clears the
 *     screen to a colour before it draws anything (rcp_dkr.c bgdraw_render: black in a race), which was drawn as two
 *     triangles over the whole target: 384,000 pixels at 800x480, as many as the sky. The GPU's memory fill unit
 *     does it instead, beside the depth clear's fill (P18), before the command list starts.
 *  3. The top screen's buffers in VRAM (DKR_FB_VRAM, on by default on a New 3DS after #451): the copy to the screen
 *     writes VRAM instead of the main memory the CPU is using (1.2 -> 0.55 ms a frame).
 *  4. The texture cache kept between draws (DKR_TEX_CACHE_KEEP, off: #451 saw no gain, shelved): citro3d's cache
 *     clear on every texture set taken out unless texture memory changed.
 *  5. Draws that can't change a pixel left out (DKR_SKIP_INVISIBLE, on): the screen fader's alpha 0 quad over the
 *     whole screen in every race frame. Also DKR_SKIP_DRAW_KINDS, for the bench's cost rows.
 */

/******************************** 1. The GPU timeline ********************************/

#define TL_EVENTS 64
#define GPU_KIND_EVERY 60 // One frame in this many is cut into parts by kind (while the profiler records)
#define GPU_KIND_PARTS 20 // At most this many parts by kind (MAX_LIST_PARTS also holds the colour probe's)
#define GPU_KIND_MIXED PROF_KINDS // Past GPU_KIND_PARTS: the rest of the frame, whatever its kinds

static volatile int sTlArmed;         // 1: the next queue run is a frame's; 2: it is running and being recorded
static volatile u64 sTlStart;         // When the frame's queue was started
static volatile u32 sTlCount;         // Jobs finished so far
static volatile u64 sTlTick[TL_EVENTS];
static volatile u8 sTlIrq[TL_EVENTS];
static u32 sTlExpected;               // Jobs in the frame's queue (timeline_expect)
static int sTimeline = -1;            // DKR_GPU_TIMELINE
static unsigned int sTexClearsKept, sTexClearsDropped; // Section 4's (DKR_TEX_CACHE_KEEP)
static unsigned int sColourFills, sColourDrawn, sColourProbes; // Section 2's (DKR_COLOUR_FILL)
static int sGpuKinds = -1;            // DKR_GPU_KINDS
static unsigned int sInvisibleSkipped; // Section 5's (DKR_SKIP_INVISIBLE)
/* Section 6's (DKR_GPU_DRAWS): a frame whose command list is cut after each of a window of draws. */
#define DP_WINDOW (MAX_LIST_PARTS - 5)
typedef struct DrawDesc {
    const void *tex0, *tex1; // The draw's textures (their GfxTexture), its key with the rest
    u16 w, h;                // tex0's size
    u8 fmt, levels;          // tex0's GPU format (GPU_TEXCOLOR) and mipmap levels
    u8 kind, blend, depth, alphaTest, fog, cycles, flat, path; // depth: test | write << 1; path: 0 indexed, 1 model, 2 meshes
    u16 tris;
} DrawDesc;
static int sDpFrame[2], sDpParts[2], sDpIndex, sDpStart, sDpCount;
static unsigned int sDpFrames; // Frames timed by draw since the last log line
static DrawDesc sDpPart[2][MAX_LIST_PARTS + 1], sDpPrev;
static void dp_take(int slot, int part, u64 ticks);
static void dp_log(void);

/* This frame's command list is cut by kind (gfxb_gpu_kind), and each part's kind, for the slot it is built in. */
static int sKindFrame[2], sKindParts[2], sKindNow;
static u8 sPartKind[2][MAX_LIST_PARTS + 1];

/* Totals since the last log line (renderer thread only). */
static unsigned long long sTlFillTicks, sTlListTicks, sTlCopyTicks, sTlOtherTicks, sTlWholeTicks;
static unsigned int sTlFrames, sTlJobs;
static unsigned long long sTlKindTicks[PROF_KINDS + 1];
static unsigned int sTlKindFrames;

/*
 * GSP thread: a GPU job finished (a memory fill, a command list, or a copy). libctru's queue has already been told
 * (gspgpu.c calls its gxCmdQueueInterrupt before the event callbacks), so this is a few microseconds late at most.
 */
static void timeline_event(void *irq) {
    if (sTlArmed == 2) {
        u32 n = sTlCount;
        if (n < TL_EVENTS) {
            sTlTick[n] = svcGetSystemTick();
            sTlIrq[n] = (u8) (u32) irq;
            __sync_synchronize();
            sTlCount = n + 1;
        }
    }
}

/* At start-up: the GSP's events for the jobs libctru's queue counts (PSC1 ends with PSC0 in a two-buffer fill). */
static void PLAT_COLD timeline_init(void) {
    gspSetEventCallback(GSPGPU_EVENT_PSC0, timeline_event, (void *) (u32) GSPGPU_EVENT_PSC0, false);
    gspSetEventCallback(GSPGPU_EVENT_PPF, timeline_event, (void *) (u32) GSPGPU_EVENT_PPF, false);
    gspSetEventCallback(GSPGPU_EVENT_P3D, timeline_event, (void *) (u32) GSPGPU_EVENT_P3D, false);
}

void __real_gxCmdQueueRun(gxCmdQueue_s *queue);
/* citro3d's C3D_FrameEnd starts the frame's queue here. */
__attribute__((used)) void __wrap_gxCmdQueueRun(gxCmdQueue_s *queue) {
    if (sTlArmed == 1) {
        sTlCount = 0;
        sTlStart = svcGetSystemTick();
        __sync_synchronize();
        sTlArmed = 2;
    }
    __real_gxCmdQueueRun(queue);
}

static void PLAT_COLD read_timeline_switches(void) {
    sTimeline = plat_switch("DKR_GPU_TIMELINE", 1) != 0;
    // v32: off by default on the Old 3DS. Its cut frame (one in GPU_KIND_EVERY) was itself a hitch there: 24 of the 53
    // renderer hitches of bench #456 landed on it (the GPU drawing 30-60 ms instead of ~13).
    sGpuKinds = plat_switch("DKR_GPU_KINDS", !plat_old_3ds()) != 0;
}

/* Renderer: just before C3D_FrameEnd hands a frame over. */
static inline void timeline_arm(void) {
    static int sInit;
    if (__builtin_expect(sTimeline < 0, 0)) {
        read_timeline_switches();
    }
    if (__builtin_expect(!sInit, 0)) {
        sInit = 1;
        timeline_init();
    }
    if (sTimeline && prof_recording()) {
        sTlArmed = 1;
    }
}

/* Renderer: just after C3D_FrameEnd, the jobs the frame's queue holds. */
static inline void timeline_expect(void) {
    if (sTlArmed) {
        sTlExpected = __C3D_Context.gxQueue.numEntries;
    }
}

static inline double ticks_ms(unsigned long long ticks) {
    return ticks * 1000.0 / SYSCLOCK_ARM11;
}

static void PLAT_COLD timeline_log(void) {
    char line[512];
    int k, at = 0;
    double f = 1.0 / sTlFrames;
    plat_log("GPU timeline: %.2f ms a frame from start to finish: memory fills %.2f, command lists %.2f, copies to the "
             "screen %.2f, other %.2f ms (%.1f jobs a frame)",
             ticks_ms(sTlWholeTicks) * f, ticks_ms(sTlFillTicks) * f, ticks_ms(sTlListTicks) * f,
             ticks_ms(sTlCopyTicks) * f, ticks_ms(sTlOtherTicks) * f, (double) sTlJobs * f);
    if (sTlKindFrames > 0) {
        line[0] = 0;
        for (k = 0; k <= PROF_KINDS && at < (int) sizeof(line) - 40; k++) {
            if (sTlKindTicks[k] != 0) {
                at += snprintf(line + at, sizeof(line) - at, "%s%s %.2f", at ? ", " : "",
                               k == GPU_KIND_MIXED ? "past the cuts" : prof_kind_name(k),
                               ticks_ms(sTlKindTicks[k]) / sTlKindFrames);
            }
        }
        plat_log("GPU command lists by kind (%u frames cut at each kind, ms): %s", sTlKindFrames, line);
    }
    dp_log();
    plat_log("GPU cuts: colour clear filled %u times (edges drawn %u, colours read back %u), texture cache clears %u "
             "kept, %u taken out, %u draws left out (they couldn't change a pixel)", sColourFills, sColourDrawn,
             sColourProbes, sTexClearsKept, sTexClearsDropped, sInvisibleSkipped);
    sColourFills = sColourDrawn = sColourProbes = sTexClearsKept = sTexClearsDropped = sInvisibleSkipped = 0;
    sTlFillTicks = sTlListTicks = sTlCopyTicks = sTlOtherTicks = sTlWholeTicks = 0;
    sTlFrames = sTlJobs = 0;
    memset(sTlKindTicks, 0, sizeof(sTlKindTicks));
    sTlKindFrames = 0;
}

/* Renderer, with the GPU idle again (wait_for_gpu): the frame that just finished, slot that built it. */
static void timeline_take(int slot) {
    unsigned long long fill = 0, list = 0, copy = 0, other = 0;
    u64 prev;
    u32 n, i;
    int part = 0, kinds = slot >= 0 && sKindFrame[slot];
    if (slot >= 0) {
        sKindFrame[slot] = 0;
    }
    if (sTlArmed != 2) {
        sTlArmed = 0;
        if (slot >= 0) {
            sDpFrame[slot] = 0;
        }
        return;
    }
    // libctru's queue says it is done before the GSP thread gets to the last job's event callback: wait for it.
    for (i = 0; i < 40 && sTlCount < sTlExpected; i++) {
        svcSleepThread(50 * 1000);
    }
    sTlArmed = 0;
    __sync_synchronize();
    n = sTlCount;
    prev = sTlStart;
    for (i = 0; i < n; i++) {
        u64 d = sTlTick[i] - prev;
        prev = sTlTick[i];
        switch (sTlIrq[i]) {
            case GSPGPU_EVENT_PSC0:
                fill += d;
                break;
            case GSPGPU_EVENT_P3D:
                list += d;
                if (slot >= 0 && sDpFrame[slot]) {
                    dp_take(slot, part, d);
                }
                if (kinds) {
                    int k = part < sKindParts[slot] ? sPartKind[slot][part] : GPU_KIND_MIXED;
                    sTlKindTicks[k] += d;
                }
                part++;
                break;
            case GSPGPU_EVENT_PPF:
                copy += d;
                break;
            default:
                other += d;
                break;
        }
    }
    if (kinds) {
        sTlKindFrames++;
    }
    if (slot >= 0 && sDpFrame[slot]) {
        sDpFrame[slot] = 0;
        sDpFrames++;
    }
    if (n == 0) {
        return;
    }
    sTlFillTicks += fill;
    sTlListTicks += list;
    sTlCopyTicks += copy;
    sTlOtherTicks += other;
    sTlWholeTicks += prev - sTlStart;
    sTlJobs += n;
    prof_add(PROF_GPU_FILL, (unsigned long long) (ticks_ms(fill) * 1e6));
    prof_add(PROF_GPU_LISTS, (unsigned long long) (ticks_ms(list) * 1e6));
    prof_add(PROF_GPU_COPY, (unsigned long long) (ticks_ms(copy) * 1e6));
    if (++sTlFrames == 600) {
        timeline_log();
    }
}

static int sSkipKinds = -1; // DKR_SKIP_DRAW_KINDS (section 5; read_switches resets it)
static int sKindCur;        // The kind of the draws now coming (section 5)

/* Walk (any thread): whether this frame's draws are to be told by kind (1) and cut by kind (2). */
int gfxb_gpu_kinds_frame(int frame) {
    if (__builtin_expect(sGpuKinds < 0, 0)) {
        read_timeline_switches();
    }
    if (__builtin_expect(sSkipKinds < 0, 0)) {
        sSkipKinds = plat_switch("DKR_SKIP_DRAW_KINDS", 0);
    }
    if (sGpuKinds && sTimeline && !sStereo && prof_recording() && frame % GPU_KIND_EVERY == GPU_KIND_EVERY / 2) {
        return 2;
    }
    return sSkipKinds != 0;
}

/*
 * Backend: the draws from here on are of this kind (port_drawtag.h). The first call of a frame starts the cutting;
 * each change of kind after it ends the part of the command list so far. Only in frames gfxb_gpu_kinds_frame chose.
 */
void gfxb_gpu_kind(int value) {
    FrameSlot *slot = &sSlots[sRec];
    int kind = value & 0xFF;
    sKindCur = kind;
    if (!(value & GFX_KIND_CUT)) {
        return;
    }
    if (!sKindFrame[sRec]) {
        sKindFrame[sRec] = 1;
        sKindParts[sRec] = 0;
        sKindNow = kind;
        return;
    }
    if (kind == sKindNow) {
        return;
    }
    if (sKindParts[sRec] < GPU_KIND_PARTS && slot->numParts < MAX_LIST_PARTS - 2 && sVboUsed > 0) {
        int before = slot->numParts;
        flush_new_vertices(); // The GPU reads the vertices so far when it runs this part
        split_command_list();
        if (slot->numParts > before) {
            sPartKind[sRec][sKindParts[sRec]++] = (u8) sKindNow;
        }
    } else if (sKindParts[sRec] >= GPU_KIND_PARTS) {
        sKindNow = GPU_KIND_MIXED;
        return;
    }
    sKindNow = kind;
}

/* The walk's OP_CALL record (gfx_split_call_reserve): gfxb_gpu_kind on the backend thread. */
void gfxb_gpu_kind_call(void *payload) {
    gfxb_gpu_kind(*(int *) payload);
}

/* Backend, at the frame's end: the last part's kind (the rest of the command list, which C3D_FrameEnd ends). */
static inline void gpu_kind_end_frame(void) {
    if (sKindFrame[sRec] && sKindParts[sRec] <= MAX_LIST_PARTS) {
        sPartKind[sRec][sKindParts[sRec]++] = (u8) sKindNow;
    }
}

/******************************** 2. The colour clear as a memory fill ********************************/

/*
 * The fill writes whole lines of the target (lines of the target run along the screen's height), so it can only do a
 * clear that covers the target's full height, and across it only a band of whole tile rows (8 lines): the band of the
 * clear's lines whose pixel centres lie inside its triangles (with a hundredth of a pixel to spare) and its scissor,
 * made symmetric about the
 * middle (the framebuffer's lines may be stored top to bottom or bottom to top: a symmetric band is the same lines
 * either way). The lines of the clear outside the band (only at the 4:3 edges of a narrow picture, where the band's
 * tile rows don't meet them) are still drawn, with the scissor narrowed to them.
 * The game's clear stops a line short of the screen's bottom row (its scissor ends at 239: rcp_dkr.c), so the fill
 * also clears that row, which every 3D frame draws over anyway.
 *
 * The colour the fill writes must be what the draw would have left, colour boost (G2) and all, which is the
 * combiner's sum in the GPU's own rounding. So it is read back from the GPU: the first clear of each colour (and boost
 * level) is drawn as before, with the command list cut after it and one 8x8 tile of the target copied to main memory
 * between the parts (the probe); once that frame is done, the tile's colour is kept and used from then on.
 */
#define FILL_COLOURS 8
static struct {
    u32 key;     // The clear's colour (RGBA, as gfxb_clear_color is given it)
    int boost;   // and the colour boost level
    u32 value;   // The colour as the target stores it (the memory fill's value)
    int used;
} sFillColours[FILL_COLOURS];
static int sFillNext;
static int sColourFill = -1;            // DKR_COLOUR_FILL
static int sColourFillPending;          // This frame's colour clear is a memory fill (gfxb_end_frame)
static u32 sColourFillValue;
static int sColourFillFrom, sColourFillTo; // The band, in lines of the target
static C3D_RenderTarget *sColourFillTarget;
static int sScissorClamp, sScissorClampFrom, sScissorClampTo; // set_scissor keeps draws within these lines

/* The probe of the slot being built: after which part of its command list the tile is copied, from where, its key. */
static struct {
    int after;      // -1: none
    const u32 *src;
    u32 key;
    int boost;
} sProbe[2] = { { -1, NULL, 0, 0 }, { -1, NULL, 0, 0 } };
static u32 *sProbeTile[2]; // 8x8 RGBA8 pixels in linear memory, one per slot

static int PLAT_COLD colour_fill_setting(void) {
    // Read again for each switch bench race (read_switches resets it).
    return plat_switch("DKR_COLOUR_FILL", !plat_old_3ds()) != 0;
}

static int fill_find(u32 key, int boost) {
    int i;
    for (i = 0; i < FILL_COLOURS; i++) {
        if (sFillColours[i].used && sFillColours[i].key == key && sFillColours[i].boost == boost) {
            return i;
        }
    }
    return -1;
}

/* The screen x of a flat draw's clip x, as to_screen_flat places it, in lines of the target. */
static float flat_line(float x) {
    float clipX = sWide ? gfx_flat_x(x, 1.0f, NARROW, GFX_FLAT_STRETCH) : x * NARROW;
    // GPU y = -clip x (to_screen_flat), and the viewport takes [-1, 1] to [0, lines].
    return (1.0f - clipX) * 0.5f * (float) sTarget->frameBuf.height;
}

/*
 * Backend: the colour clear of rect, before anything else is drawn this frame. Returns whether it was taken (filled,
 * with any edges drawn); 0 leaves it to be drawn as before.
 */
static int colour_fill(const short rect[4], u32 rgba) {
    float x0, x1, triFrom, triTo;
    int lines = sTarget->frameBuf.height, width = sTarget->frameBuf.width;
    int scFrom, scTo, from, to, i;
    u32 key;
    float scale;

    if (sColourFill < 0) {
        sColourFill = colour_fill_setting();
    }
    if (!sColourFill || !sFrameFirstDraw || sStereo || sOnceFrame || sClearing || sNumSlots < 1 ||
        sTarget->frameBuf.colorFmt != GPU_RB_RGBA8) {
        return 0;
    }
    // Across the target (the screen's height): the whole of it, but for the bottom row the game leaves out.
    if (rect[1] > 0 || rect[3] < GFX_SCREEN_HEIGHT - 1) {
        return 0;
    }
    // Along it (the screen's width): the clear's lines, from its triangles and its scissor (set_scissor's sums).
    x0 = rect[0] / (GFX_SCREEN_WIDTH / 2.0f) - 1.0f;
    x1 = rect[2] / (GFX_SCREEN_WIDTH / 2.0f) - 1.0f;
    triFrom = flat_line(x1);
    triTo = flat_line(x0);
    if (sWide) {
        scFrom = TOP_WIDTH - (int) gfx_flat_px(rect[2], NARROW, TOP_WIDTH);
        scTo = TOP_WIDTH - (int) gfx_flat_px(rect[0], NARROW, TOP_WIDTH);
    } else {
        scale = NARROW * ((float) TOP_WIDTH / GFX_SCREEN_WIDTH);
        scFrom = TOP_WIDTH - (int) (TOP_WIDTH / 2 + (rect[2] - GFX_SCREEN_WIDTH / 2) * scale);
        scTo = TOP_WIDTH - (int) (TOP_WIDTH / 2 + (rect[0] - GFX_SCREEN_WIDTH / 2) * scale);
    }
    if (scFrom < 0) scFrom = 0;
    if (scTo > TOP_WIDTH) scTo = TOP_WIDTH;
    scFrom *= sScaleX;
    scTo *= sScaleX;
    {
        // The lines whose pixel centres lie inside the triangles with room to spare, and inside the scissor.
        int lo = (int) ceilf(triFrom - 0.49f), hi = (int) floorf(triTo - 0.51f) + 1;
        if (lo < scFrom) lo = scFrom;
        if (hi > scTo) hi = scTo;
        if (lo < 0) lo = 0;
        if (hi > lines) hi = lines;
        from = (lo + 7) & ~7; // Whole tile rows inside [lo, hi)
        to = hi & ~7;
        if (from < lines - to) from = lines - to; // Symmetric about the middle
        to = lines - from;
    }
    if (to - from < 16) {
        return 0;
    }
    key = rgba;
    i = fill_find(key, sColourBoost);
    if (i < 0) {
        // The probe: drawn as before, and one tile of it read back once the frame is done.
        FrameSlot *slot = &sSlots[sRec];
        if (sProbe[sRec].after >= 0 || slot->numParts >= MAX_LIST_PARTS - 2) {
            return 0;
        }
        if (sProbeTile[sRec] == NULL) {
            sProbeTile[sRec] = linearMemAlign(64 * 4, 64);
            if (sProbeTile[sRec] == NULL) {
                return 0;
            }
        }
        draw_clear_rect(rect, rgba, 0);
        flush_new_vertices();
        split_command_list();
        if (slot->numParts == 0) {
            return 1; // No split made: drawn, nothing read
        }
        sProbe[sRec].after = slot->numParts - 1;
        // The first tile row of the band, a middle tile of it (inside the clear whichever way the lines run).
        sProbe[sRec].src = (const u32 *) sTarget->frameBuf.colorBuf + ((from / 8) * (width / 8) + width / 16) * 64;
        sProbe[sRec].key = key;
        sProbe[sRec].boost = sColourBoost;
        sColourProbes++;
        return 1;
    }
    sColourFillPending = 1;
    sColourFillValue = sFillColours[i].value;
    sColourFillFrom = from;
    sColourFillTo = to;
    sColourFillTarget = sTarget;
    sColourFills++;
    // The clear's lines outside the band, drawn as before but only there.
    if (scFrom < from || to < scTo) {
        sScissorClamp = 1;
        if (scFrom < from) {
            sScissorClampFrom = scFrom;
            sScissorClampTo = from;
            forget_gpu_state();
            draw_clear_rect(rect, rgba, 0);
        }
        if (to < scTo) {
            sScissorClampFrom = to;
            sScissorClampTo = scTo;
            forget_gpu_state();
            draw_clear_rect(rect, rgba, 0);
        }
        sScissorClamp = 0;
        forget_gpu_state();
        sColourDrawn++;
        // The fill still runs before them: they are the frame's first draws, as the clear was.
    }
    return 1;
}

/* Backend, frame end, with the GPU idle: the frame's fills ahead of its command list (the depth clear's too). */
static int colour_fill_queue(void) {
    C3D_FrameBuf *fb;
    u32 *colour, *colourEnd, *depth = NULL, *depthEnd = NULL;
    u32 lineBytes;
    static const u8 sDepthBytes[] = { 2, 2, 3, 4 };
    if (!sColourFillPending) {
        return 0;
    }
    sColourFillPending = 0;
    fb = &sColourFillTarget->frameBuf;
    lineBytes = (u32) fb->width * 4;
    colour = (u32 *) ((u8 *) fb->colorBuf + (u32) sColourFillFrom * lineBytes);
    colourEnd = (u32 *) ((u8 *) fb->colorBuf + (u32) sColourFillTo * lineBytes);
    if (sDepthFillPending) {
        depth = (u32 *) fb->depthBuf;
        depthEnd = (u32 *) ((u8 *) fb->depthBuf + (u32) fb->width * fb->height * sDepthBytes[fb->depthFmt & 3]);
        sDepthFillPending = 0;
    }
    // Both in one job, on the two fill units at once (as C3D_RenderTargetClear does a colour and depth clear).
    GX_MemoryFill(colour, sColourFillValue, colourEnd, BIT(0) | (2 << 8), depth, 0, depthEnd,
                  depth != NULL ? (BIT(0) | ((fb->depthFmt == GPU_RB_DEPTH16 ? 0 : fb->depthFmt == GPU_RB_DEPTH24 ? 1 : 2)
                                             << 8))
                                : 0);
    return 1;
}

/* Backend, frame end: after the probe's part of the command list, its tile copied out. */
static inline void probe_after_part(int part) {
    if (sProbe[sRec].after == part) {
        GX_TextureCopy((u32 *) sProbe[sRec].src, 0, sProbeTile[sRec], 0, 64 * 4, GX_TRANSFER_RAW_COPY(1));
    }
}

/* Renderer, with the GPU idle: the probe of the frame that just finished, slot that built it. */
static void probe_take(int slot) {
    const u32 *tile;
    int i;
    if (slot < 0 || sProbe[slot].after < 0) {
        return;
    }
    sProbe[slot].after = -1;
    tile = sProbeTile[slot];
    svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32) tile, 64 * 4);
    for (i = 1; i < 64; i++) {
        if (tile[i] != tile[0]) {
            plat_log("colour fill: the probe's tile isn't one colour (%08lx, %08lx at %d): drawn as before",
                     (unsigned long) tile[0], (unsigned long) tile[i], i);
            return;
        }
    }
    if (fill_find(sProbe[slot].key, sProbe[slot].boost) < 0) {
        i = sFillNext++ % FILL_COLOURS;
        sFillColours[i].key = sProbe[slot].key;
        sFillColours[i].boost = sProbe[slot].boost;
        sFillColours[i].value = tile[0];
        sFillColours[i].used = 1;
        plat_log("colour fill: clear colour %08lx (boost %d) is %08lx in the target", (unsigned long) sProbe[slot].key,
                 sProbe[slot].boost, (unsigned long) tile[0]);
    }
}

/******************************** 3. The top screen's buffers in VRAM ********************************/

/*
 * Two sets of the three buffers (triple_choose): in main memory as before, and in VRAM (DKR_FB_VRAM), made when first
 * wanted and kept, so the switch bench can change it between races. A change of set happens while a buffer of the
 * other set is on screen (or waiting to be), so all three of the new set are free to be written.
 */
static int sFbVram = -1; // DKR_FB_VRAM (switch bench row)
static int sTripleSetFailed[2]; // sTripleSets and sTripleSetNow: before triple_choose

static int fb_vram_wanted(void) {
    if (sFbVram < 0) {
        // On by default on a New 3DS after bench #451: the copy to the screen 1.2 -> 0.55 ms on all three tracks.
        // The debug build's frame history (bug reports' flash finder) can't read VRAM, so it stops while they are
        // there: DKR_FB_VRAM=0 brings it back.
        sFbVram = plat_switch("DKR_FB_VRAM", 1) != 0 && !plat_old_3ds();
    }
    return sFbVram;
}

/* Makes set vram (0 main memory, 1 VRAM) the one frames go to; returns 0 if it can't be made. */
static int triple_use_set(int vram) {
    int i;
    if (sTripleSets[vram][0] == NULL && !sTripleSetFailed[vram]) {
        for (i = 0; i < 3; i++) {
            sTripleSets[vram][i] = vram ? vramAlloc(TRIPLE_BYTES) : linearAlloc(TRIPLE_BYTES);
            if (sTripleSets[vram][i] == NULL) {
                plat_log("top screen: no %s for a third buffer", vram ? "VRAM" : "memory");
                sTripleSetFailed[vram] = 1;
                for (i = 0; i < 3; i++) {
                    if (sTripleSets[vram][i] != NULL) {
                        if (vram) {
                            vramFree(sTripleSets[vram][i]);
                        } else {
                            linearFree(sTripleSets[vram][i]);
                        }
                        sTripleSets[vram][i] = NULL;
                    }
                }
                return 0;
            }
        }
        if (vram) {
            plat_log("top screen: its three buffers in VRAM, %lu KB of VRAM left", (unsigned long) (vramSpaceFree() / 1024));
        }
    }
    if (sTripleSets[vram][0] == NULL) {
        return 0;
    }
    if (sTripleSetNow != vram) {
        if (sTripleSetNow >= 0) {
            sTripleShown = sTripleMaybe = sTriplePresented = -1; // Those were the other set's
        }
        for (i = 0; i < 3; i++) {
            sTriple[i] = sTripleSets[vram][i];
        }
        sTripleSetNow = vram;
    }
    return 1;
}

/******************************** 4. The texture cache kept between draws ********************************/

/*
 * citro3d clears the GPU's texture cache whenever a texture unit is set (C3Di_UpdateContext, base.c), and since every
 * draw's settings are written twice on a New 3DS (DKR_REWRITE=2), that is twice a draw, about 300 times a frame: each
 * draw starts reading its textures from main memory again, and the GPU may have to finish the draw before for each
 * clear. The cache only needs clearing when texture memory changed since the GPU last read it: a texture made,
 * changed, shared or freed, or a new frame (anything the CPU wrote while the last frame was drawn).
 * DKR_TEX_CACHE_KEEP=1 keeps the first clear after such a change and takes the clear bit out of the others, in the
 * commands C3Di_UpdateContext has just written (-Wl,--wrap=C3Di_UpdateContext; citro3d's draws call it from their
 * own files, so they reach the wrapper too). Shelved, off: bench #451 raced it with no gain (GPU within 0.1 ms on
 * Crescent Island and Jungle Falls, worse on Haunted Woods) and no wrong texels, so the clears cost the GPU nothing.
 */
static int sTexCacheKeep = -1;      // DKR_TEX_CACHE_KEEP (read_switches resets it)
static int sTexClearAllowed = 1;    // Texture memory changed since the last clear (or a frame began)

void __real_C3Di_UpdateContext(void);
__attribute__((used)) void __wrap_C3Di_UpdateContext(void) {
    u32 *buf = gpuCmdBuf, from = gpuCmdBufOffset, i, end;
    __real_C3Di_UpdateContext();
    if (__builtin_expect(sTexCacheKeep < 0, 0)) {
        sTexCacheKeep = plat_switch("DKR_TEX_CACHE_KEEP", 0) != 0;
    }
    if (!sTexCacheKeep || buf != gpuCmdBuf) {
        return;
    }
    end = gpuCmdBufOffset;
    // Each command: its first parameter, its header (extra parameters in bits 20-27), the rest, padded to 8 bytes.
    for (i = from; i + 1 < end;) {
        u32 header = buf[i + 1], extra = (header >> 20) & 0xFF;
        if ((header & ~(0xFFu << 20)) == GPUCMD_HEADER(0, 0x4, GPUREG_TEXUNIT_CONFIG) && extra == 0 &&
            (buf[i] & BIT(16))) {
            if (sTexClearAllowed) {
                sTexClearAllowed = 0;
                sTexClearsKept++;
            } else {
                buf[i] &= ~BIT(16); // The same write, without the clear
                sTexClearsDropped++;
            }
        }
        i += 2 + extra + (extra & 1);
    }
}

/* Texture memory changed (an upload, share or free, or a new frame): the next clear is kept. */
static inline void tex_cache_dirty(void) {
    sTexClearAllowed = 1;
}

/******************************** 5. Draws that can't change a pixel ********************************/

/*
 * gfx_draw_invisible (gfx_backend.h): blended, no depth written, alpha 0 from constants. The screen fader's prim
 * alpha 0 quad over the whole screen in every race frame was one screen of the GPU's pixels (PC count, DKR_FRAG_STATS;
 * #451's "menus/other" 0.8-1.2 ms). Left out with DKR_SKIP_INVISIBLE (on); frames identical.
 * Also here: DKR_SKIP_DRAW_KINDS, a bitmask of kinds (port_drawtag.h) whose draws are left out, for the switch
 * bench's cost rows (what a kind costs the GPU and the backend; the walk still does its part).
 */
static int sSkipInvisible = -1; // DKR_SKIP_INVISIBLE (read_switches resets it)

static inline int skip_invisible(const GfxDrawState *state) {
    if (__builtin_expect(sSkipInvisible < 0, 0)) {
        sSkipInvisible = plat_switch("DKR_SKIP_INVISIBLE", 1) != 0;
    }
    if (__builtin_expect(sSkipKinds > 0, 0) && ((sSkipKinds >> sKindCur) & 1)) {
        return 1;
    }
    if (sSkipInvisible && __builtin_expect(gfx_draw_invisible(state), 0)) {
        sInvisibleSkipped++;
        return 1;
    }
    return 0;
}

/******************************** 6. Each draw's GPU time ********************************/

/*
 * DKR_GPU_DRAWS (on, while the profiler records, not in 3D; New 3DS): one frame in GPU_DRAW_EVERY has its command list
 * cut before each draw of a window of DP_WINDOW draws (the window moves on each time, so every draw of a race frame
 * gets its turn), and the GPU timeline times each part: what each draw costs the GPU, with what it was drawn with.
 * The timeline's split by kind (section 1) says which kinds cost; this says which draws, and why (their texture's
 * size and format, blending, depth, how many triangles). Draws are added up by their textures and settings; the
 * costliest are logged with the timeline ("GPU draws ..."). Each cut adds the GPU's finishing and restarting of a
 * list part (some microseconds), so the cheapest draws read a little high.
 */
#define GPU_DRAW_EVERY 60
#define DP_KEYS 96
static int sGpuDraws = -1; // DKR_GPU_DRAWS
static unsigned int sDpFrameNo;
static struct {
    DrawDesc d;
    u64 ticks;
    unsigned int count;
    unsigned long tris;
} sDpKeys[DP_KEYS];
static int sDpKeysUsed;
static unsigned long long sDpTicksAll;
static unsigned int sDpTimed;

/* Backend, frame start: whether this frame is timed by draw. */
static void dp_frame_start(void) {
    int i;
    if (__builtin_expect(sGpuDraws < 0, 0)) {
        sGpuDraws = plat_switch("DKR_GPU_DRAWS", !plat_old_3ds()) != 0;
    }
    sDpFrame[sRec] = 0;
    if (++sDpFrameNo % GPU_DRAW_EVERY != 0 || !sGpuDraws || !sTimeline || sStereo || !prof_recording()) {
        return;
    }
    sDpFrame[sRec] = 1;
    sDpParts[sRec] = MAX_LIST_PARTS + 1;
    for (i = 0; i <= MAX_LIST_PARTS; i++) {
        sDpPart[sRec][i].kind = 0xFF; // Parts outside the window are not timed
    }
    sDpIndex = 0;
    if (sDpStart >= sDpCount) {
        sDpStart = 0; // Round again from the frame's first draw
    }
}

static void dp_describe(DrawDesc *d, const GfxDrawState *st, int tris, int path) {
    const GfxTexture *t = st->textures[0];
    memset(d, 0, sizeof(*d));
    d->tex0 = t;
    d->tex1 = st->textures[1];
    if (t != NULL && t->backendData != NULL) {
        const C3DTexture *ct = (const C3DTexture *) t->backendData;
        d->w = ct->tex.width;
        d->h = ct->tex.height;
        d->fmt = (u8) ct->tex.fmt;
        d->levels = (u8) ct->levels;
    }
    d->kind = (u8) sKindCur;
    d->blend = st->blend;
    d->depth = (u8) ((st->depthTest ? 1 : 0) | (st->depthWrite ? 2 : 0));
    d->alphaTest = st->alphaTest;
    d->fog = st->fogEnabled;
    d->cycles = st->combiner.numCycles;
    d->flat = st->flat;
    d->path = (u8) path;
    d->tris = (u16) (tris > 65535 ? 65535 : tris);
}

/* Backend, at a draw's start (after it is known to be drawn): in the window, the list is cut before it, so the part
 * that ends there holds the draw before (sDpPrev). */
static inline void dp_draw(const GfxDrawState *st, int tris, int path) {
    int idx;
    FrameSlot *slot;
    if (__builtin_expect(!sDpFrame[sRec], 1)) {
        return;
    }
    if (sKindFrame[sRec]) {
        sDpFrame[sRec] = 0; // Being cut by kind this frame
        return;
    }
    slot = &sSlots[sRec];
    idx = sDpIndex++;
    if (idx >= sDpStart && idx <= sDpStart + DP_WINDOW && slot->numParts < MAX_LIST_PARTS - 3 && sVboUsed > 0) {
        int before = slot->numParts;
        flush_new_vertices();
        split_command_list();
        // The part that ended here (numbered as the timeline numbers the frame's list parts) holds the draw before;
        // the one before the window's first draw is the frame's start, not timed (kind 0xFF).
        if (slot->numParts > before && idx > sDpStart) {
            sDpPart[sRec][before] = sDpPrev;
        }
    }
    dp_describe(&sDpPrev, st, tris, path);
}

/* Backend, frame end: the draw count (for the window's next turn) and the window moved on. */
static inline void dp_frame_end(void) {
    if (!sDpFrame[sRec]) {
        return;
    }
    if (sDpIndex > sDpStart && sDpIndex <= sDpStart + DP_WINDOW && sSlots[sRec].numParts <= MAX_LIST_PARTS) {
        sDpPart[sRec][sSlots[sRec].numParts] = sDpPrev; // The window's last draw is the frame's last: the list's rest
    }
    sDpCount = sDpIndex;
    sDpStart += DP_WINDOW;
}

static int dp_same(const DrawDesc *a, const DrawDesc *b) {
    return a->tex0 == b->tex0 && a->tex1 == b->tex1 && a->kind == b->kind && a->blend == b->blend &&
           a->depth == b->depth && a->alphaTest == b->alphaTest && a->path == b->path && a->cycles == b->cycles;
}

/* Renderer (timeline_take): part `part` of the frame slot built took `ticks` on the GPU. */
static void dp_take(int slot, int part, u64 ticks) {
    const DrawDesc *d;
    int i;
    if (part >= sDpParts[slot]) {
        return;
    }
    d = &sDpPart[slot][part];
    if (d->kind == 0xFF) {
        return; // Before the window
    }
    sDpTicksAll += ticks;
    sDpTimed++;
    for (i = 0; i < sDpKeysUsed; i++) {
        if (dp_same(&sDpKeys[i].d, d)) {
            break;
        }
    }
    if (i == sDpKeysUsed) {
        if (sDpKeysUsed == DP_KEYS) {
            // Full: the cheapest so far makes room
            int j, low = 0;
            for (j = 1; j < DP_KEYS; j++) {
                if (sDpKeys[j].ticks < sDpKeys[low].ticks) {
                    low = j;
                }
            }
            i = low;
        } else {
            sDpKeysUsed++;
        }
        sDpKeys[i].d = *d;
        sDpKeys[i].ticks = 0;
        sDpKeys[i].count = 0;
        sDpKeys[i].tris = 0;
    }
    sDpKeys[i].ticks += ticks;
    sDpKeys[i].count++;
    sDpKeys[i].tris += d->tris;
}

static void PLAT_COLD dp_log(void) {
    static const char *const sFmt[] = { "RGBA8", "RGB8", "RGBA5551", "RGB565", "RGBA4", "LA8", "HILO8", "L8",
                                        "A8", "LA4", "L4", "A4", "ETC1", "ETC1A4" };
    int shown, i, j;
    double perFrame;
    if (sDpFrames == 0 || sDpTimed == 0) {
        return;
    }
    // The window covers DP_WINDOW of the frame's draws, so a draw's time a frame is its sampled time spread over the
    // frames in which its index was in the window.
    perFrame = (double) sDpCount / DP_WINDOW;
    plat_log("GPU draws (%u frames timed by draw, %u draws timed, %.2f ms of GPU in them; about %d draws a frame): the "
             "costliest, ms a frame (est.) / ms each, how many, triangles each, texture, settings:",
             sDpFrames, sDpTimed, ticks_ms(sDpTicksAll), sDpCount);
    for (shown = 0; shown < 14 && sDpKeysUsed > 0; shown++) {
        int best = 0;
        for (j = 1; j < sDpKeysUsed; j++) {
            if (sDpKeys[j].ticks > sDpKeys[best].ticks) {
                best = j;
            }
        }
        {
            const DrawDesc *d = &sDpKeys[best].d;
            double each = ticks_ms(sDpKeys[best].ticks) / sDpKeys[best].count;
            plat_log("  %.3f / %.3f ms x%u, %lu tris, %s: tex %dx%d %s%s mips %d, blend %d depth %s alpha test %d fog %d "
                     "cycles %d flat %d %s",
                     ticks_ms(sDpKeys[best].ticks) / sDpFrames * perFrame, each, sDpKeys[best].count,
                     sDpKeys[best].tris / sDpKeys[best].count, d->kind < PROF_KINDS ? prof_kind_name(d->kind) : "?",
                     d->w, d->h, d->tex0 != NULL && d->fmt < 14 ? sFmt[d->fmt] : "none", d->tex1 != NULL ? " +2nd" : "",
                     d->levels, d->blend,
                     d->depth == 3 ? "test+write" : d->depth == 1 ? "test" : d->depth == 2 ? "write" : "off",
                     d->alphaTest, d->fog, d->cycles, d->flat,
                     d->path == 0 ? "triangles" : d->path == 1 ? "model" : "meshes");
        }
        sDpKeys[best].ticks = 0; // Shown (the table is emptied below)
    }
    sDpKeysUsed = 0;
    sDpTicksAll = 0;
    sDpTimed = 0;
    sDpFrames = 0;
    (void) i;
}
