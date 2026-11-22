/*
 * Raw GPU register words (roadmap P55 S3 / P60 F, DKR_RAW_REGS, off by default). Included by gfx_c3d.c after
 * write_settings_twice; the few hooks it needs in gfx_c3d.c are marked "DKR_RAW_REGS".
 *
 * Through citro3d a draw costs the renderer core the setters (C3D_AlphaBlend, C3D_TexBind, C3D_SetTexEnv...), then
 * C3Di_UpdateContext encoding every group they marked and scanning all 2 x 96 float uniforms, 2 x 4 integer ones and
 * the light settings for changes, then C3D_DrawElements writing the draw command by command. The F-Zero X 3DS port
 * cut its per-draw cost from 10.9 to 0.7 us by writing the PICA200's register words itself.
 *
 * Here a draw's settings (blending, alpha test, depth, culling, depth map, scissor, both texture units, the combiner
 * stages and buffer, the fog mode and colour) become one block of register words the first time a draw state is
 * seen. The block is made by citro3d itself: with every group marked (forget_gpu_state, then set_all_state), the
 * words C3Di_UpdateContext writes are the block, so the encoding is citro3d's, word for word: the same words the
 * every-setting path (DKR_STATE_CACHE=0) writes. Later draws with the same state copy the block into the command list
 * (twice where drain_before_draw wants the settings written twice), the vertex shader's uniforms are written as words
 * (only the changed ones, as before), and the draw itself is a copy of C3D_DrawElements's 15 commands with the index
 * buffer and count filled in. citro3d is still used for everything else (targets, vertex formats, the frame), and its
 * pending groups are written before a raw draw whenever any are marked.
 *
 * A block is only valid while what it was made from holds. Its textures: a block keeps, for each unit, the citro3d
 * texture it bound and that texture's six words C3Di_SetTex writes from (address, format, size, filter and wrap,
 * border, mipmap levels) and its coordinate scales, and is only reused while they are all the same (an upload, a
 * move to VRAM or back, a share or a free changes them; raw_tex_same); otherwise it is made again. And the frame's
 * settings that set_all_state reads besides the draw state (wide screen, the 3D width, render scale, colour boost,
 * mipmaps, the camera's near plane): raw_frame_start compares them and empties the cache when one changed; a switch
 * change empties it too. The fog table is not in a block: it is
 * written by itself when a fogged draw needs another one. Frames in 3D, the colour fill's clamped scissor and the
 * alpha diagnostic stay on the citro3d path.
 *
 * The raw functions are out of line and not in .text.hot, so the default path's per-draw code (the Old 3DS hot-code
 * budget, port/tools/hot_size.py) only gains the tests that call them; if the bench keeps raw words, they move into
 * the hot set in place of the citro3d calls they replace.
 *
 * DKR_RAW_REGS_CHECK=1 (not in release builds): every draw that reuses a block also makes it again through citro3d,
 * compares the two by the registers they set (the fog colour only when fog is on), logs any difference and throws
 * the second copy away. A difference means a block was used after something it depends on changed.
 */

#define RAW_BLOCKS 512         // Draw states kept
#define RAW_BUCKETS 256        // Hash chains (a power of two)
#define RAW_POOL_WORDS (RAW_BLOCKS * 176) // Their words
#define RAW_MAX_WORDS 320      // The most one block may have (a full set is ~120-170)
#define RAW_KEY_WORDS 15       // A GfxDrawState is 60 bytes; byte 37 is padding (masked off)
#define RAW_LUT_UNKNOWN 0x7FFF

_Static_assert(sizeof(GfxDrawState) == RAW_KEY_WORDS * 4 && offsetof(GfxDrawState, fogEnabled) == 36 &&
                   offsetof(GfxDrawState, fogMul) == 38,
               "the raw block key is the draw state's words, padding byte 37 masked");

typedef struct RawBlock {
    u32 key[RAW_KEY_WORDS];
    u32 pool;  // First word in sRawPool
    u16 words; // Its length
    s16 next;  // Next in the hash chain, -1 for none
    u8 depthOnly;
    u8 fogPixel; // The fog table is used (written separately, raw_fog_lut)
    float su0, sv0, su1, sv1; // bind_texture's texture coordinate scales
    const C3D_Tex *tex[2];    // The textures bound (the white one for none)
    u32 texWords[2][6];       // Their words C3Di_SetTex reads, when the block was made
} RawBlock;
_Static_assert(offsetof(C3D_Tex, lodParam) == 20, "C3D_Tex's first six words are what C3Di_SetTex reads");

static RawBlock *sRawBlocks;
static u32 *sRawPool;
static s16 sRawHeads[RAW_BUCKETS];
static int sRawUsed;          // Blocks made
static u32 sRawPoolUsed;      // Words used
static int sRawCapturing;     // set_all_state is making a block (or the check's copy): no raw path inside it
static int sRawNoRoom;        // Allocation failed: raw stays off
static int sRawCheck;         // DKR_RAW_REGS_CHECK
static const u32 *sRawAgain;  // The settings words just written, for raw_again (a block, or a block too big to keep)
static u32 sRawAgainWords;    // Their count, 0 when the draw's settings went through citro3d
static u32 sRawDrawWords[30]; // C3D_DrawElements's commands for triangles (raw_draw_elements fills two in)

static struct {
    float scale3D, near;
    int wide, renderScale, scaleX, boost, mipUsed, mipShown;
    C3D_RenderTarget *target;
} sRawSig;

static struct {
    unsigned int draws, hits, made, uncached, declined, emitted, words, flushes, lut, unifs, checked,
        differ, stale;
} sRawStats;
static unsigned int sRawLogFrame;

/* The citro3d texture a unit binds for a draw (as bind_texture), with its coordinate scales. */
static inline const C3D_Tex *raw_bound(const GfxTexture *tex, float *scaleU, float *scaleV) {
    const C3DTexture *t = tex != NULL ? (const C3DTexture *) tex->backendData : NULL;
    if (t == NULL) {
        *scaleU = 1.0f;
        *scaleV = 1.0f;
        return &sWhiteTex;
    }
    *scaleU = t->scaleU;
    *scaleV = t->scaleV;
    return &t->tex;
}

/* Whether a block's textures are still what it was made with: the same citro3d textures, words and scales. */
static inline int raw_tex_same(const RawBlock *b, const GfxDrawState *state) {
    int u;
    for (u = 0; u < 2; u++) {
        float su, sv;
        const C3D_Tex *t = raw_bound(state->textures[u], &su, &sv);
        if (t != b->tex[u] || !same_words(t, b->texWords[u], 6) || su != (u ? b->su1 : b->su0) ||
            sv != (u ? b->sv1 : b->sv0)) {
            return 0;
        }
    }
    return 1;
}

static void raw_flush(void) {
    int i;
    for (i = 0; i < RAW_BUCKETS; i++) {
        sRawHeads[i] = -1;
    }
    sRawUsed = 0;
    sRawPoolUsed = 0;
    sRawLast = NULL;
    sRawStats.flushes++;
}

static void PLAT_COLD raw_build_draw_words(void) {
    static const struct {
        u16 reg;
        u8 mask;
        u32 value;
    } sCmds[15] = {
        { GPUREG_PRIMITIVE_CONFIG, 2, GPU_GEOMETRY_PRIM }, { GPUREG_RESTART_PRIMITIVE, 0xF, 1 },
        { GPUREG_INDEXBUFFER_CONFIG, 0xF, 0 }, { GPUREG_NUMVERTICES, 0xF, 0 }, { GPUREG_VERTEX_OFFSET, 0xF, 0 },
        { GPUREG_GEOSTAGE_CONFIG, 2, 0x100 }, { GPUREG_GEOSTAGE_CONFIG2, 2, 0x100 }, { GPUREG_START_DRAW_FUNC0, 1, 0 },
        { GPUREG_DRAWELEMENTS, 0xF, 1 }, { GPUREG_START_DRAW_FUNC0, 1, 1 }, { GPUREG_GEOSTAGE_CONFIG, 2, 0 },
        { GPUREG_GEOSTAGE_CONFIG2, 2, 0 }, { GPUREG_VTX_FUNC, 0xF, 1 }, { GPUREG_PRIMITIVE_CONFIG, 8, 0 },
        { GPUREG_PRIMITIVE_CONFIG, 8, 0 },
    };
    int i;
    for (i = 0; i < 15; i++) {
        sRawDrawWords[i * 2] = sCmds[i].value;
        sRawDrawWords[i * 2 + 1] = GPUCMD_HEADER(0, sCmds[i].mask, sCmds[i].reg);
    }
}

/* Read with the other switches (read_switches): whether raw words are wanted, and the check. */
static void PLAT_COLD raw_read_switches(void) {
    // On by default on an Old 3DS (bench run 50 on 62ac36e: renderer core -4.3 ms on every track; with the karts'
    // vertex maths on the GPU, DKR_NATIVE_GPU 1, game core -4.3 ms and renderer core -3.6 ms). Off in stereo 3D.
    sRawWanted = plat_switch("DKR_RAW_REGS", plat_old_3ds()) != 0;
#ifndef PORT_RELEASE
    sRawCheck = plat_switch("DKR_RAW_REGS_CHECK", 0) != 0;
#endif
    if (sRawWanted && sRawBlocks == NULL && !sRawNoRoom) {
        sRawBlocks = malloc(sizeof(RawBlock) * RAW_BLOCKS);
        sRawPool = malloc(sizeof(u32) * RAW_POOL_WORDS);
        if (sRawBlocks == NULL || sRawPool == NULL) {
            free(sRawBlocks);
            free(sRawPool);
            sRawBlocks = NULL;
            sRawPool = NULL;
            sRawNoRoom = 1;
            plat_log("raw GPU words: no memory for the block cache, left off");
        } else {
            raw_build_draw_words();
        }
    }
    if (sRawBlocks != NULL) {
        raw_flush(); // A switch may change what set_all_state makes of a state
    }
    plat_log("switches: GPU settings as %s%s", sRawWanted && !sRawNoRoom ? "raw register words" : "citro3d calls",
             sRawWanted && sRawCheck ? " (each reuse checked against citro3d)" : "");
}

static void PLAT_COLD raw_log_stats(void) {
    plat_log("raw GPU words: %u draws, %u block reuses, %u made, %u not kept, %u on citro3d, %u blocks written "
             "(%u words), %u fog tables, %u uniform writes, cache emptied %u times, %u remade (textures changed); "
             "checked %u, %u differ",
             sRawStats.draws, sRawStats.hits, sRawStats.made, sRawStats.uncached, sRawStats.declined,
             sRawStats.emitted, sRawStats.words, sRawStats.lut, sRawStats.unifs, sRawStats.flushes,
             sRawStats.stale, sRawStats.checked, sRawStats.differ);
}

/* At the end of gfxb_start_frame: whether this frame's draws are raw, and whether the blocks still hold. */
static void raw_frame_start(void) {
    float nearPlane;
    sRawDraw = sRawWanted && sRawBlocks != NULL && !sStereo;
    if (!sRawDraw) {
        return;
    }
    nearPlane = plat_camera_near();
    if (sRawSig.scale3D != sScale3D || sRawSig.near != nearPlane || sRawSig.wide != sWide ||
        sRawSig.renderScale != sRenderScale || sRawSig.scaleX != sScaleX || sRawSig.boost != sColourBoost ||
        sRawSig.mipUsed != sMipmapsUsed || sRawSig.mipShown != sMipmapsShown || sRawSig.target != sTarget) {
        sRawSig.scale3D = sScale3D;
        sRawSig.near = nearPlane;
        sRawSig.wide = sWide;
        sRawSig.renderScale = sRenderScale;
        sRawSig.scaleX = sScaleX;
        sRawSig.boost = sColourBoost;
        sRawSig.mipUsed = sMipmapsUsed;
        sRawSig.mipShown = sMipmapsShown;
        sRawSig.target = sTarget;
        raw_flush();
    }
    // The GPU's state isn't assumed across frames (the HOME menu, another frame's citro3d groups): the first draw
    // writes its block, the fog table and the uniforms again, and citro3d's pending groups are written first.
    sRawLast = NULL;
    sRawLutMul = RAW_LUT_UNKNOWN;
    sUnifKnown = 0;
    sRawSynced = 0;
    if (++sRawLogFrame % 600 == 0) {
        raw_log_stats();
    }
}

/* Writes citro3d's pending groups (and its uniform scan) when there may be any, so that they land before the raw
 * words that follow and never after them. */
static inline void raw_sync(void) {
    if (__builtin_expect(!sRawSynced || (__C3D_Context.flags & ~(u32) (C3D_FLAG_ACTIVE | C3D_FLAG_DRAW_USED)), 0)) {
        C3Di_UpdateContext();
        sRawSynced = 1;
    }
}

static inline u32 *raw_words(u32 n) {
    u32 *p = gpuCmdBuf + gpuCmdBufOffset;
    gpuCmdBufOffset += n;
    return p;
}

static inline void raw_emit(const RawBlock *b) {
    memcpy(raw_words(b->words), sRawPool + b->pool, b->words * 4);
    sRawStats.emitted++;
    sRawStats.words += b->words;
}

/* The fog table for a draw with per-pixel fog, written by itself when the GPU has another one. */
static void raw_fog_lut(const GfxDrawState *state) {
    u32 *p;
    int i;
    if (state->fogMul == sRawLutMul && state->fogOffset == sRawLutOffset) {
        return;
    }
    if (state->fogMul != sLutMul || state->fogOffset != sLutOffset) {
        bind_fog_lut(state);
    }
    __C3D_Context.flags &= ~C3D_FLAG_FOG_LUT; // Written here, not by citro3d
    // As citro3d writes it: the index, then the 128 entries to the data port (one command, 128 parameters).
    p = raw_words(132);
    p[0] = 0;
    p[1] = GPUCMD_HEADER(0, 0xF, GPUREG_FOG_LUT_INDEX);
    p[2] = sFogLutData.data[0];
    p[3] = GPUCMD_HEADER(0, 0xF, GPUREG_FOG_LUT_DATA0) | (127u << 20);
    for (i = 1; i < 128; i++) {
        p[3 + i] = sFogLutData.data[i];
    }
    p[131] = 0; // Padding to an even count
    sRawLutMul = state->fogMul;
    sRawLutOffset = state->fogOffset;
    sRawStats.lut++;
}

static inline void raw_key(const GfxDrawState *state, u32 *key, u32 *hash) {
    const u32 *w = (const u32 *) state;
    u32 h = sDepthOnly;
    int i;
    for (i = 0; i < RAW_KEY_WORDS; i++) {
        u32 v = w[i];
        if (i == 9) {
            v &= 0xFFFF00FFu; // Byte 37: padding after fogEnabled
        }
        key[i] = v;
        h = (h ^ v) * 0x9E3779B1u;
    }
    *hash = (h ^ (h >> 15)) & (RAW_BUCKETS - 1);
}

/*
 * Makes a block: every group marked and set through citro3d, written by C3Di_UpdateContext straight into the command
 * list (so this draw is drawn with it already), and the words kept. Returns NULL if it couldn't be kept; the draw's
 * settings are written either way.
 */
static RawBlock *__attribute__((noinline)) raw_make(const GfxDrawState *state, const u32 *key, u32 hash, float *su0,
                                                     float *sv0, float *su1, float *sv1) {
    RawBlock *b;
    u32 from, words;
    int i;
    raw_sync(); // Whatever citro3d has pending goes before the block, not into it
    forget_gpu_state();
    sRawCapturing = 1;
    set_all_state(state, su0, sv0, su1, sv1);
    sRawCapturing = 0;
    __C3D_Context.flags &= ~C3D_FLAG_FOG_LUT; // The fog table is written by itself (raw_fog_lut)
    from = gpuCmdBufOffset;
    C3Di_UpdateContext();
    words = gpuCmdBufOffset - from;
    if (state->fogEnabled && fog_mode() == FOG_PIXEL) {
        raw_fog_lut(state);
    }
    sGpu.known = 0; // sGpu describes citro3d's copy, which later blocks leave behind
    sRawAgain = gpuCmdBuf + from; // Still there for raw_again, whether kept or not
    sRawAgainWords = words;
    if (words == 0 || words > RAW_MAX_WORDS || (words & 1)) {
        sRawStats.uncached++;
        return NULL;
    }
    if (sRawUsed == RAW_BLOCKS || sRawPoolUsed + words > RAW_POOL_WORDS) {
        raw_flush();
    }
    b = &sRawBlocks[sRawUsed];
    memcpy(b->key, key, sizeof(b->key));
    b->pool = sRawPoolUsed;
    b->words = (u16) words;
    b->depthOnly = (u8) sDepthOnly;
    b->fogPixel = state->fogEnabled && fog_mode() == FOG_PIXEL;
    b->su0 = *su0;
    b->sv0 = *sv0;
    b->su1 = *su1;
    b->sv1 = *sv1;
    for (i = 0; i < 2; i++) {
        float su, sv;
        b->tex[i] = raw_bound(state->textures[i], &su, &sv);
        memcpy(b->texWords[i], b->tex[i], sizeof(b->texWords[i])); // After set_all_state (bind_texture's mipmaps)
    }
    memcpy(sRawPool + sRawPoolUsed, gpuCmdBuf + from, words * 4);
    sRawPoolUsed += words;
    b->next = sRawHeads[hash];
    sRawHeads[hash] = (s16) sRawUsed;
    sRawUsed++;
    sRawStats.made++;
    sRawStats.emitted++;
    sRawStats.words += words;
    return b;
}

#ifndef PORT_RELEASE
/* The registers a run of command words sets, for raw_check (as port/host/c3d_replay reads them). */
static u32 sRawRegA[0x400], sRawRegB[0x400];
static u8 sRawSetA[0x400], sRawSetB[0x400];

static void PLAT_COLD raw_regs(const u32 *w, u32 n, u32 *regs, u8 *set) {
    u32 i = 0;
    memset(set, 0, 0x400);
    while (i + 1 < n) {
        u32 header = w[i + 1], reg = header & 0x3FF, mask = (header >> 16) & 0xF, extra = (header >> 20) & 0xFF, k;
        u32 bytes = (mask & 1 ? 0xFFu : 0) | (mask & 2 ? 0xFF00u : 0) | (mask & 4 ? 0xFF0000u : 0) |
                    (mask & 8 ? 0xFF000000u : 0);
        for (k = 0; k <= extra; k++) {
            u32 r = header >> 31 ? (reg + k) & 0x3FF : reg, v = k == 0 ? w[i] : w[i + 1 + k];
            regs[r] = (regs[r] & ~bytes) | (v & bytes);
            set[r] = 1;
        }
        i += 2 + extra + (extra & 1);
    }
}

/* DKR_RAW_REGS_CHECK: the block about to be reused against what citro3d makes of the state now. */
static void PLAT_COLD __attribute__((noinline)) raw_check(const RawBlock *b, const GfxDrawState *state) {
    u32 from, r, fogA, fogB;
    float su0, sv0, su1, sv1;
    int differ = 0;
    raw_sync();
    forget_gpu_state();
    sRawCapturing = 1;
    set_all_state(state, &su0, &sv0, &su1, &sv1);
    sRawCapturing = 0;
    __C3D_Context.flags &= ~C3D_FLAG_FOG_LUT;
    from = gpuCmdBufOffset;
    C3Di_UpdateContext();
    raw_regs(sRawPool + b->pool, b->words, sRawRegA, sRawSetA);
    raw_regs(gpuCmdBuf + from, gpuCmdBufOffset - from, sRawRegB, sRawSetB);
    gpuCmdBufOffset = from; // Thrown away: the block is written instead
    sGpu.known = 0;
    sRawStats.checked++;
    fogA = sRawSetA[GPUREG_TEXENV_UPDATE_BUFFER] ? sRawRegA[GPUREG_TEXENV_UPDATE_BUFFER] & 7 : 0;
    fogB = sRawSetB[GPUREG_TEXENV_UPDATE_BUFFER] ? sRawRegB[GPUREG_TEXENV_UPDATE_BUFFER] & 7 : 0;
    for (r = 0; r < 0x400; r++) {
        if (!sRawSetA[r] && !sRawSetB[r]) {
            continue;
        }
        if (r == GPUREG_FOG_COLOR && fogA == GPU_NO_FOG && fogB == GPU_NO_FOG) {
            continue; // citro3d writes its last fog colour with fog off too: no effect
        }
        if (sRawSetA[r] != sRawSetB[r] || sRawRegA[r] != sRawRegB[r]) {
            if (sRawStats.differ + differ < 8) {
                plat_log("raw GPU words check: frame %d draw %u: register 0x%03x block %s0x%08lx, citro3d %s0x%08lx",
                         sFrameCount, sRawStats.draws, (unsigned) r, sRawSetA[r] ? "" : "(unset) ",
                         (unsigned long) sRawRegA[r], sRawSetB[r] ? "" : "(unset) ", (unsigned long) sRawRegB[r]);
            }
            differ = 1;
        }
    }
    if (differ) {
        sRawStats.differ++;
    }
    if (su0 != b->su0 || sv0 != b->sv0 || su1 != b->su1 || sv1 != b->sv1) {
        if (sRawStats.differ < 8) {
            plat_log("raw GPU words check: frame %d draw %u: texture scales differ", sFrameCount, sRawStats.draws);
        }
        sRawStats.differ += !differ;
        differ = 1;
    }
    plat_check_report("raw GPU words", differ); /* the race's checks_run / checks_wrong (CSV "chk" cells) */
}
#endif

/*
 * set_all_state's raw way (DKR_RAW_REGS). Returns 0 to leave the draw to citro3d (inside a block being made, the
 * colour fill's clamped scissor, the alpha diagnostic). A block is written unless it is the one the GPU has from the
 * last draw (forget_gpu_state, as for a draw whose settings are written twice, makes it write again).
 */
static int __attribute__((noinline)) raw_set_all_state(const GfxDrawState *state, float *su0, float *sv0, float *su1, float *sv1) {
    u32 key[RAW_KEY_WORDS], hash;
    int i;
    RawBlock *b = NULL;

    if (sRawCapturing) {
        return 0;
    }
    if (__builtin_expect(sScissorClamp || sDiagAlpha != 0, 0)) {
        sRawLast = NULL;
        sRawAgainWords = 0;
        sRawStats.declined++;
        return 0;
    }
    sRawStats.draws++;
    raw_key(state, key, &hash);
    for (i = sRawHeads[hash]; i >= 0; i = sRawBlocks[i].next) {
        RawBlock *c = &sRawBlocks[i];
        if (c->depthOnly == sDepthOnly && same_words(c->key, key, RAW_KEY_WORDS)) {
            if (!raw_tex_same(c, state)) {
                c->depthOnly = 2; // Stale: never matches again; made anew below
                sRawStats.stale++;
                if (sRawLast == c) {
                    sRawLast = NULL;
                }
                break;
            }
            b = c;
            break;
        }
    }
    if (b == NULL) {
        sRawLast = raw_make(state, key, hash, su0, sv0, su1, sv1);
        return 1;
    }
    sRawStats.hits++;
#ifndef PORT_RELEASE
    if (__builtin_expect(sRawCheck, 0)) {
        raw_check(b, state);
    }
#endif
    *su0 = b->su0;
    *sv0 = b->sv0;
    *su1 = b->su1;
    *sv1 = b->sv1;
    sRawAgain = sRawPool + b->pool;
    sRawAgainWords = b->words;
    if (b != sRawLast) {
        raw_sync();
        raw_emit(b);
        sRawLast = b;
        sGpu.known = 0; // citro3d's copy no longer matches the GPU (a later draw on citro3d sets everything)
    }
    if (b->fogPixel) {
        raw_fog_lut(state);
    }
    return 1;
}

/* write_settings_twice's raw way: the block just written, once more. Returns 0 if the draw went through citro3d. */
static int __attribute__((noinline)) raw_again(void) {
    if (sRawAgainWords == 0) {
        return 0;
    }
    memcpy(raw_words(sRawAgainWords), sRawAgain, sRawAgainWords * 4);
    sRawStats.emitted++;
    sRawStats.words += sRawAgainWords;
    return 1;
}

/* The changed vertex shader uniforms as words (unif_write_twice_now's raw way): twice unless DKR_UNIF_TWICE=0, as
 * citro3d would write them once now and once with the draw. Their values are in citro3d's copy (set_unif). */
static void raw_emit_uniforms(unsigned changed) {
    int t, u;
    raw_sync();
    for (t = sUnifTwice ? 2 : 1; t > 0; t--) {
        for (u = 0; u < UNIF_COUNT; u++) {
            if (changed & (1u << u)) {
                const u32 *v = (const u32 *) C3D_FVUnif[GPU_VERTEX_SHADER][sUnifLoc[u]].c;
                u32 *p = raw_words(8);
                p[0] = 0x80000000u | (u32) sUnifLoc[u];
                p[1] = GPUCMD_HEADER(0, 0xF, GPUREG_VSH_FLOATUNIFORM_CONFIG);
                p[2] = v[0];
                p[3] = GPUCMD_HEADER(0, 0xF, GPUREG_VSH_FLOATUNIFORM_DATA) | (3u << 20);
                p[4] = v[1];
                p[5] = v[2];
                p[6] = v[3];
                p[7] = 0;
                sRawStats.unifs++;
            }
        }
    }
}

/* C3D_DrawElements for 16-bit indexed triangles, as words. */
static void __attribute__((noinline)) raw_draw_elements(int count, const u16 *indices) {
    u32 pa = osConvertVirtToPhys(indices), *p;
    if (pa < 0x18000000u) { // citro3d's vertex buffer base (BufInfo_Init), as C3D_DrawElements checks
        return;
    }
    raw_sync();
    p = raw_words(30);
    memcpy(p, sRawDrawWords, sizeof(sRawDrawWords));
    p[4] = (pa - 0x18000000u) | (1u << 31); // GPUREG_INDEXBUFFER_CONFIG: offset, 16-bit indices
    p[6] = (u32) count;                     // GPUREG_NUMVERTICES
    __C3D_Context.flags |= C3D_FLAG_DRAW_USED;
}

/* For the backend replay (port/host/c3d_replay): the raw path's counts. */
void gfxb_raw_stats(unsigned int *draws, unsigned int *hits, unsigned int *checked, unsigned int *differ) {
    *draws = sRawStats.draws;
    *hits = sRawStats.hits;
    *checked = sRawStats.checked;
    *differ = sRawStats.differ;
}
