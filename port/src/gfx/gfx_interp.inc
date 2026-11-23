/******************************** 60 Hz picture: the walk's deltas (S8 / P39) ********************************/

/*
 * DKR_INTERP60 (roadmap S8, engine P39; plan: proposals/3ds-interp60-plan.md). Included by gfx_rdp.c. The backend
 * draws a frame twice: the in-between picture, with every draw moved by its delta, then the real one. This works the
 * deltas out. A vertex reaches the backend in clip space with w kept (clip = v C, C the matrix the walk used), so with
 * C' the same thing's matrix half a step away, clip' = v C' = clip (C^-1 C'): one 4x4 delta D a matrix, exact also for
 * perspective (the divide comes after). C' is made from this frame's matrix and the last frame's of the same thing:
 *   interpolate (1): C' = (C + P) / 2,  extrapolate (2): C' = C + (C - P) / 2,  so D = (1 + s) I - s C^-1 P, s = -/+ 1/2.
 * "The same thing" is a key: the game tags each object's draws with the object (render_object) and the world matrix
 * (mtx_world_origin) with PORT_IKEY_TAG while the switch is on; a key is the last tag (or draw kind tag), the number of
 * matrix loads since it and how often that pair came before in the frame. Last frame's matrices are kept by key.
 *  - Matched: D from the two matrices, if it passes the sanity check (ikey_sane: no corner of the screen moves more than
 *    IKEY_MOVE_LIMIT, nothing at the near plane goes behind the camera). Each matrix its own D: the sky's matrix has
 *    no camera translation in it, so neither has its D (F-Zero X's sky jitter came from one camera delta for all).
 *  - Not matched (an object's first frame), or not sane (a teleport, a respawn): the camera's D (the world's), so the
 *    thing stays put in the world while the camera moves smoothly (30 Hz motion for it, never a jump).
 *  - Affine matrices (no perspective: the HUD, menus, billboards' offsets placed next to their anchor) keep the D in
 *    force: flat draws get the identity in the backend whatever the walk sends, and a billboard's offsets go with its
 *    anchor.
 *  - The world's own D not sane (a camera cut), no world matrix at all, or more than one viewport: the frame is not
 *    interpolated (GFX_INTERP_END_NOT: one picture, shown for two retraces, as without the switch).
 * Batches carry on across matrix loads (one draw for many bananas); a change of D cuts them (ikey_apply), and only
 * moving things change it. Vertices loaded with one matrix and drawn after another was selected take the second's D
 * (both belong to one object, so their motion is nearly the same).
 * DKR_INTERP60_CHECK: each D is checked (C D against C'), counted in the log once every 300 frames; 3 breaks the walk's
 * D on purpose (rule K6), 2 the backend's conversion (gpu_interp.inc).
 */

#define IKEY_SLOTS 1024    // Matrices kept a frame (a race has a few hundred loads)
#define IKEY_MAX_USED 768  // A table this full takes no more (the rest unmatched next frame)
#define IKEY_MOVE_LIMIT 0.4f // Screen units (-1..1) a corner may move in half a step
#define IKEY_SAME 1e-4f    // A D this close to the one in force changes nothing
#define IKEY_WORLD 0x574F524Cu

typedef struct IkeyEntry {
    u32 key;
    u32 gen;
    float m[4][4];
} IkeyEntry;

static s32 sIkeyOn;           // This frame: 0, or 1 / 2 (gfxb_interp_frame)
static s32 sIkeyCheck;        // DKR_INTERP60_CHECK
static IkeyEntry *sIkeyTab[2]; // [sIkeyCur] this frame's, the other the last frame's
static s32 sIkeyCur;
static u32 sIkeyGen = 1, sIkeyUsed;
static u32 sIkeyBase, sIkeyOrd, sIkeyKind; // sIkeyKind: the draw kind and object kind the base was taken at
static s32 sIkeyWorldNext;    // The next perspective load is the world matrix (PORT_IKEY_TAG world)
static s32 sIkeyHaveCam, sIkeyBad, sIkeyWorlds; // sIkeyBad: 1 the world's D not sane or new, 2 a second viewport
static float sIkeyCam[4][4];  // The camera's D (the world matrix's), for things not matched
static float sIkeySlot[4][4][4]; // The D of each matrix index's matrix
static u8 sIkeyKeep[4];       // That index holds an affine matrix: it keeps the D in force
static float sIkeyForce[4][4]; // The D the backend has (sent last)
static float sIkeyVp[6];
static s32 sIkeyVpSeen;
static unsigned int sIkeyStats[11];  // Matched, not matched, not sane, deltas sent, frames valid, not valid, checked, wrong;
                              // frames not valid for: no world matrix, the world's D, two viewports
static const float sIkeyI[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

static inline u32 ikey_mix(u32 a, u32 b) {
    u32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h != 0 ? h : 1;
}

/* r = a b (rows). */
static void ikey_mul(const float a[4][4], const float b[4][4], float r[4][4]) {
    s32 i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
}

/* inv = m^-1; FALSE if m is (nearly) singular. */
static s32 ikey_invert(const float m[4][4], float inv[4][4]) {
    float a[4][8];
    s32 i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            a[i][j] = m[i][j];
            a[i][j + 4] = i == j ? 1.0f : 0.0f;
        }
    }
    for (i = 0; i < 4; i++) {
        s32 p = i;
        float best = fabsf(a[i][i]), d;
        for (k = i + 1; k < 4; k++) {
            if (fabsf(a[k][i]) > best) {
                best = fabsf(a[k][i]);
                p = k;
            }
        }
        if (best < 1e-12f) {
            return FALSE;
        }
        if (p != i) {
            for (j = 0; j < 8; j++) {
                float t = a[i][j];
                a[i][j] = a[p][j];
                a[p][j] = t;
            }
        }
        d = 1.0f / a[i][i];
        for (j = 0; j < 8; j++) {
            a[i][j] *= d;
        }
        for (k = 0; k < 4; k++) {
            if (k != i && a[k][i] != 0.0f) {
                float f = a[k][i];
                for (j = 0; j < 8; j++) {
                    a[k][j] -= f * a[i][j];
                }
            }
        }
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            inv[i][j] = a[i][j + 4];
        }
    }
    return TRUE;
}

/* Whether D keeps the picture together: the screen's corners and middle at three depths move at most
 * IKEY_MOVE_LIMIT, and points at the near plane stay in front of the camera (no giant triangles). */
static s32 ikey_sane(const float d[4][4]) {
    static const float sXY[5][2] = { { -0.9f, -0.9f }, { 0.9f, -0.9f }, { -0.9f, 0.9f }, { 0.9f, 0.9f }, { 0, 0 } };
    static const float sZ[4] = { -1.0f, 0.5f, 0.9f, 0.99f };
    s32 i, j, c;
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 4; j++) {
            float p[4] = { sXY[i][0], sXY[i][1], sZ[j], 1.0f }, q[4];
            for (c = 0; c < 4; c++) {
                q[c] = p[0] * d[0][c] + p[1] * d[1][c] + p[2] * d[2][c] + p[3] * d[3][c];
            }
            if (!(q[3] > 0.05f)) {
                return FALSE;
            }
            if (j > 0 && (fabsf(q[0] / q[3] - p[0]) > IKEY_MOVE_LIMIT || fabsf(q[1] / q[3] - p[1]) > IKEY_MOVE_LIMIT)) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

/* DKR_INTERP60_CHECK: C D against C' (made the plain way). */
static COLD void ikey_check(const float c[4][4], const float p[4][4], const float d[4][4], float s) {
    float cd[4][4], scale = 0.0f;
    s32 i, j, miss = 0;
    ikey_mul(c, d, cd);
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            scale = fabsf(c[i][j]) > scale ? fabsf(c[i][j]) : scale;
        }
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            float want = c[i][j] + s * (c[i][j] - p[i][j]);
            if (fabsf(cd[i][j] - want) > 1e-3f * (scale + 1e-6f)) {
                miss = 1;
            }
        }
    }
    sIkeyStats[6]++;
    sIkeyStats[7] += miss;
    plat_check_report("60 Hz picture walk", miss); /* P59 E: the race's checks_run / checks_wrong (CSV "chk" cells) */
}

/* The D of a matched matrix C (last frame's P); FALSE if C can't be inverted. */
static s32 ikey_delta(const float c[4][4], const float p[4][4], float d[4][4]) {
    float inv[4][4], q[4][4];
    float s = sIkeyOn == 2 ? 0.5f : -0.5f;
    s32 i, j;
    if (!ikey_invert(c, inv)) {
        return FALSE;
    }
    ikey_mul(inv, p, q);
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            // DKR_INTERP60_CHECK=3 (rule K6): Q transposed, which the check must report.
            float qv = sIkeyCheck == 3 ? q[j][i] : q[i][j];
            d[i][j] = (i == j ? 1.0f + s : 0.0f) - s * qv;
        }
    }
    if (UNLIKELY(sIkeyCheck)) {
        ikey_check(c, p, d, s);
    }
    return TRUE;
}

/* Last frame's matrix for key, or NULL. */
static const IkeyEntry *ikey_find_prev(u32 key) {
    const IkeyEntry *t = sIkeyTab[sIkeyCur ^ 1];
    u32 i, h = key & (IKEY_SLOTS - 1);
    for (i = 0; i < 32; i++, h = (h + 1) & (IKEY_SLOTS - 1)) {
        if (t[h].gen != sIkeyGen - 1) {
            return NULL;
        }
        if (t[h].key == key) {
            return &t[h];
        }
    }
    return NULL;
}

/* This frame's entry for key (a key already used this frame gets the next occurrence), or NULL if the table is full. */
static IkeyEntry *ikey_insert(u32 *key) {
    IkeyEntry *t = sIkeyTab[sIkeyCur];
    u32 i, h, salt;
    if (sIkeyUsed >= IKEY_MAX_USED) {
        return NULL;
    }
    for (salt = 0; salt < 16; salt++) {
        u32 k = salt == 0 ? *key : ikey_mix(*key, salt);
        h = k & (IKEY_SLOTS - 1);
        for (i = 0; i < 32; i++, h = (h + 1) & (IKEY_SLOTS - 1)) {
            if (t[h].gen != sIkeyGen) {
                t[h].gen = sIkeyGen;
                t[h].key = k;
                sIkeyUsed++;
                *key = k;
                return &t[h];
            }
            if (t[h].key == k) {
                break; // Used this frame already: the next occurrence
            }
        }
    }
    return NULL;
}

static void ikey_send(s32 kind, const float d[4][4]) {
    GfxInterpCall local, *c = (GfxInterpCall *) gfx_split_call_reserve(gfxb_interp_call, sizeof(GfxInterpCall));
    GfxInterpCall *to = c != NULL ? c : &local;
    to->kind = kind;
    to->pad = 0;
    __builtin_memcpy(to->d, d, sizeof(to->d));
    if (c != NULL) {
        gfx_split_call_commit(c);
    } else {
        gfxb_interp_call(&local); // Not queued this frame: the backend's calls are made at once
    }
}

/* The draws from here on take D: the batch so far is drawn first with the D before. */
static void ikey_apply(const float d[4][4]) {
    s32 i, j, same = TRUE, ident = TRUE;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            same &= fabsf(d[i][j] - sIkeyForce[i][j]) < IKEY_SAME;
            ident &= fabsf(d[i][j] - sIkeyI[i][j]) < IKEY_SAME;
        }
    }
    if (same) {
        return;
    }
    flush_batch_for_atlas(); // (Out of line: flush_batch's inlining in the hot walk stays as it was)
    if (sTrackPending) {
        track_flush(); // The native track's waiting pieces were made with the D before
    }
    __builtin_memcpy(sIkeyForce, d, sizeof(sIkeyForce));
    ikey_send(ident ? GFX_INTERP_IDENTITY : GFX_INTERP_DELTA, d);
    sIkeyStats[3]++;
}

/* A matrix index selected (G_MW_MVPMATRIX, NOPX_MVP, after a load): its D is the one in force. */
static COLD NOINLINE void ikey_select(void) {
    s32 cur = g.curMatrix & 3;
    if (!sIkeyKeep[cur]) {
        ikey_apply(sIkeySlot[cur]);
    }
}

/* The viewport of each world matrix load: a second one in a frame (split screen) leaves the frame drawn once. */
static COLD NOINLINE void ikey_viewport(void) {
    float vp[6] = { g.vpScale[0], g.vpScale[1], g.vpScale[2], g.vpTrans[0], g.vpTrans[1], g.vpTrans[2] };
    if (!sIkeyVpSeen) {
        __builtin_memcpy(sIkeyVp, vp, sizeof(vp));
        sIkeyVpSeen = TRUE;
    } else if (__builtin_memcmp(sIkeyVp, vp, sizeof(vp)) != 0) {
        sIkeyBad |= 2;
    }
}

/* A matrix loaded into index (and selected, as every load does): its key and D. */
static COLD NOINLINE void ikey_load(s32 index) {
    float(*m)[4] = g.matrices[index & 3];
    const float(*d)[4];
    float made[4][4];
    s32 world = sIkeyWorldNext, ok = FALSE;
    const IkeyEntry *prev;
    IkeyEntry *e;
    u32 key;
    index &= 3;
    if (m[0][3] == 0.0f && m[1][3] == 0.0f && m[2][3] == 0.0f) {
        sIkeyKeep[index] = 1; // Affine: keeps the D in force
        return;
    }
    sIkeyKeep[index] = 0;
    sIkeyWorldNext = FALSE;
    if (!world && (u32) (sDrawKind | sDrawObj << 8 | 0x10000) != sIkeyKind) {
        // A new draw kind (or object kind) since the base was taken: untagged loads are keyed by it
        sIkeyKind = (u32) (sDrawKind | sDrawObj << 8 | 0x10000);
        sIkeyBase = ikey_mix(sIkeyKind, 0x4B494E44u);
        sIkeyOrd = 0;
    }
    key = world ? ikey_mix(IKEY_WORLD, sIkeyWorlds++) : ikey_mix(sIkeyBase, sIkeyOrd++);
    e = ikey_insert(&key);
    if (e != NULL) {
        __builtin_memcpy(e->m, m, sizeof(e->m));
    }
    prev = e != NULL ? ikey_find_prev(key) : NULL;
    if (prev != NULL && ikey_delta(m, prev->m, made)) {
        ok = ikey_sane(made);
        sIkeyStats[ok ? 0 : 2]++;
    } else {
        sIkeyStats[1]++;
    }
    if (world) {
        ikey_viewport();
        if (!ok) {
            sIkeyBad |= 1; // A camera cut, the first frame of a race...
        } else if (!sIkeyHaveCam) {
            __builtin_memcpy(sIkeyCam, made, sizeof(sIkeyCam));
            sIkeyHaveCam = TRUE;
        }
    }
    d = ok ? (const float(*)[4]) made : sIkeyHaveCam ? (const float(*)[4]) sIkeyCam : sIkeyI;
    __builtin_memcpy(sIkeySlot[index], d, sizeof(sIkeySlot[index]));
    ikey_apply(d);
}

/* PORT_IKEY_TAG (port_drawtag.h): the next loads are an object's (bit 0, a hash of it in bits 1-15), or the next one
 * is the world's. */
static COLD NOINLINE void ikey_tag(u32 w1) {
    if (w1 & 1) {
        sIkeyBase = ikey_mix(w1 & 0xFFFF, 0x4F424A31u);
        sIkeyOrd = 0;
        sIkeyKind = (u32) (sDrawKind | sDrawObj << 8 | 0x10000); // The object's tag comes after its kind tag
    } else {
        sIkeyWorldNext = TRUE;
    }
}


/* read_switches_now: the walk's own settings DKR_INTERP60 turns off. Draws keep their order (the opaque sort would draw
 * them after the deltas they belong with were replaced), and sprites stay triangles (the sprite program has no
 * delta). */
static COLD NOINLINE void ikey_switches(void) {
    if (plat_switch("DKR_INTERP60", 0) != 0) {
        sOpaqueSortSwitch = 0;
        sSpriteGs = 0;
    }
}

/* The frame's start (after the backend's frame started): whether this frame works deltas out. */
static COLD NOINLINE void ikey_frame_start(void) {
    s32 on = REF_ACTIVE() || UNLIKELY(sTracing) ? 0 : gfxb_interp_frame();
    s32 i;
    if (on && sIkeyTab[0] == NULL) {
        sIkeyTab[0] = (IkeyEntry *) plat_malloc(IKEY_SLOTS * 2 * sizeof(IkeyEntry));
        if (sIkeyTab[0] != NULL) {
            __builtin_memset(sIkeyTab[0], 0, IKEY_SLOTS * 2 * sizeof(IkeyEntry));
        }
        sIkeyTab[1] = sIkeyTab[0] != NULL ? sIkeyTab[0] + IKEY_SLOTS : NULL;
        plat_log("60 Hz picture: walk's matrix table %s", sIkeyTab[0] != NULL ? "made" : "NOT made (no memory)");
    }
    if (sIkeyTab[0] == NULL) {
        on = 0;
    }
    if (on && !sIkeyOn) {
        sIkeyGen += 2; // Nothing from before the switch matches
    }
    sIkeyOn = on;
    if (!on) {
        return;
    }
    sIkeyCheck = plat_switch("DKR_INTERP60_CHECK", 0);
    sIkeyCur ^= 1;
    sIkeyGen++;
    sIkeyUsed = 0;
    sIkeyBase = 0;
    sIkeyOrd = 0;
    sIkeyKind = 0;
    sIkeyWorldNext = FALSE;
    sIkeyHaveCam = FALSE;
    sIkeyBad = FALSE;
    sIkeyWorlds = 0;
    sIkeyVpSeen = FALSE;
    for (i = 0; i < 4; i++) {
        __builtin_memcpy(sIkeySlot[i], sIkeyI, sizeof(sIkeyI));
        sIkeyKeep[i] = 0;
    }
    __builtin_memcpy(sIkeyForce, sIkeyI, sizeof(sIkeyI)); // The backend starts each frame with the identity
}

/* The frame's end, its batches all drawn: the verdict. */
static COLD NOINLINE void ikey_frame_end(void) {
    s32 valid;
    valid = sIkeyHaveCam && !sIkeyBad;
    sIkeyStats[valid ? 4 : 5]++;
    if (!valid) {
        sIkeyStats[(sIkeyBad & 2) ? 10 : (sIkeyBad & 1) ? 9 : 8]++;
    }
    ikey_send(valid ? GFX_INTERP_END_VALID : GFX_INTERP_END_NOT, sIkeyI);
    if (sFrameCount % 300 == 0) {
        plat_log("60 Hz picture walk: frames %u valid, %u not (%u no world matrix, %u camera cut, %u two viewports); "
                 "matrices %u matched, %u new, %u not sane; %u deltas sent",
                 sIkeyStats[4], sIkeyStats[5], sIkeyStats[8], sIkeyStats[9], sIkeyStats[10], sIkeyStats[0],
                 sIkeyStats[1], sIkeyStats[2], sIkeyStats[3]);
        if (sIkeyCheck) {
            plat_log("60 Hz picture walk check: %u deltas checked, %u wrong", sIkeyStats[6], sIkeyStats[7]);
        }
    }
}

/* For the bench results and the host test: the walk check's counts and frames valid / not. */
void gfx_interp_walk_counts(unsigned int out[4]) {
    out[0] = sIkeyStats[6];
    out[1] = sIkeyStats[7];
    out[2] = sIkeyStats[4];
    out[3] = sIkeyStats[5];
}
