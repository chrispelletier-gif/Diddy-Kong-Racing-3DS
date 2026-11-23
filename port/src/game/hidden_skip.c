/*
 * P67 (proposals/3ds-hidden-batch-skip-plan.md): track batches left out by a table baked on the PC.
 *
 * DKR_HIDDEN_SKIP=1 (default 0): for each drawn frame, the camera's cell (camera segment, X/Z sub-cell of that
 * segment's box, yaw sector) picks a row of the level's table (hidden_tables.inc, made by port/tools/hidden_bake.py),
 * whose bits say which track batches some camera pose in that cell can see a pixel of. render_level_segment
 * (src/tracks.c) leaves out the others before it builds any command for them. The row applies only while the drawn
 * camera's pose is inside the box of poses the bake sampled for the cell (position, pitch, roll, field of view and
 * aspect no wider); any other pose (spin-outs, cutscenes, far zoom, a place never raced) gets no row and draws every
 * batch, as before. A table baked from another ROM (geometry hash, segment and batch counts) is not used.
 *
 * DKR_HIDDEN_SKIP_CHECK=1 (with the skip): after the opaque track, the batches the row left out are drawn again with
 * colour and depth writes off, counting the pixels that pass the depth test (the host renderer counts them; the 3DS's
 * stencil count is PR 2). Logged at each level's end: "hidden check: ... PASS" only with 0 pixels.
 * DKR_HIDDEN_SKIP_BREAK=1 (rule K6): the row also leaves out the biggest opaque batch of the camera's own segment that
 * it says is seen, so the check must report pixels (FAILED).
 *
 * Host only (PORT_HIDDEN_BAKE): the pose log (DKR_HIDDEN_POSES, port/host/hidden_poses.c), the bake
 * (DKR_HIDDEN_BAKE, port/host/hidden_bake.c) and the table override for the cell size evaluation
 * (DKR_HIDDEN_TABLE_DIR) are reached from here, since only game code sees the level model and the camera.
 */
#include <ultra64.h>
#include <string.h>
#include "types.h"
#include "structs.h"
#include "camera.h"
#include "tracks.h"
#include "textures_sprites.h"
#include "thread3_main.h"
#include "math_util.h"
#include "objects.h"
#include "port.h"
#include "plat/plat.h"
#include "port_hidden_skip.h"

extern LevelModel *gCurrentLevelModel;
extern s8 gCutsceneCameraActive;
extern s32 gNoCamShake;
extern f32 gCurCamFOV;
void guPerspectiveF(float mf[4][4], u16 *perspNorm, float fovy, float aspect, float near, float far, float scale);

#include "hidden_tables.inc" /* sHiddenTables[], generated */

#define HS_MAX_SEGMENTS 256
#define HS_MAX_ROW_BYTES 1024
#define HS_MAX_TABLES 8 /* the host's DKR_HIDDEN_EVAL; the game itself uses one */

typedef struct {
    u32 frames, framesRow, framesNoRow[8];
    unsigned long long skippedTris, skippedBatches;
    u32 checkFrames, checkFramesPixels, checkMost;
    unsigned long long checkPixels;
} HsStats;

/* A level's table (port_hidden_skip.h) and this frame's row from it. */
typedef struct {
    HiddenTable t;
    const u8 *row; // This viewport's row (NULL: draw everything)
    u8 breakRow[HS_MAX_ROW_BYTES];
    HsStats stats;
    char name[24];
} HsTable;

static s32 sEpoch = -1, sOn, sCheck, sBreak;
static const LevelModel *sModel;
static s32 sLevel = -1;
static u16 sFirst[HS_MAX_SEGMENTS + 1];
static s32 sNumSegments, sRowBytes;
static HsTable sTables[HS_MAX_TABLES]; // [0] is the one the frame skips with; the rest (host) are only counted
static s32 sNumTables;
static s32 sInvert;    // The check pass: draw only what the checked table's row leaves out
static s32 sCheckTable; // Which table the check pass is drawing
static u32 sFrames;
static HiddenPose sLastPose; /* the latest frame's (for the log of a frame whose check finds many pixels) */
static s32 sLastMode, sLastLogs;
static unsigned long long sWalkedTris, sWalkedBatches;

static const char *const sNoRowNames[HIDDEN_NR_COUNT] = { "segment", "cutscene", "fov", "no cell", "box",
                                                          "pitch/roll", "portrait" };

static void switches(void) {
    if (sEpoch != gPlatSwitchEpoch) {
        sEpoch = gPlatSwitchEpoch;
        sOn = plat_switch("DKR_HIDDEN_SKIP", 0) != 0;
        sCheck = sOn && plat_switch("DKR_HIDDEN_SKIP_CHECK", 0) != 0;
        sBreak = sOn && plat_switch("DKR_HIDDEN_SKIP_BREAK", 0) != 0;
    }
}

/* FNV-1a over the level's track geometry: each batch's texture, offsets and (but for water, whose vertices the waves
 * move) its vertex positions, and every triangle's flags and corners. Ties a table to the model it was baked from. */
static u32 fnv(u32 h, u32 v, s32 bytes) {
    s32 i;
    for (i = 0; i < bytes; i++) {
        h ^= (v >> (i * 8)) & 0xFF;
        h *= 16777619u;
    }
    return h;
}

u32 port_hidden_geom_hash(const LevelModel *model) {
    u32 h = 2166136261u;
    s32 s, b, v, t;
    for (s = 0; s < model->numberOfSegments; s++) {
        const LevelModelSegment *seg = &model->segments[s];
        h = fnv(h, (u32) seg->numberOfBatches, 2);
        for (b = 0; b < seg->numberOfBatches; b++) {
            const TriangleBatchInfo *bi = &seg->batches[b];
            h = fnv(h, bi->textureIndex, 1);
            h = fnv(h, (u16) bi->verticesOffset, 2);
            h = fnv(h, (u16) bi->facesOffset, 2);
            if (!(bi->flags & RENDER_WATER)) {
                for (v = bi->verticesOffset; v < bi[1].verticesOffset; v++) {
                    h = fnv(h, (u16) seg->vertices[v].x, 2);
                    h = fnv(h, (u16) seg->vertices[v].y, 2);
                    h = fnv(h, (u16) seg->vertices[v].z, 2);
                }
            }
            for (t = bi->facesOffset; t < bi[1].facesOffset; t++) {
                const Triangle *tri = &seg->triangles[t];
                h = fnv(h, tri->flags, 1);
                h = fnv(h, tri->vi0, 1);
                h = fnv(h, tri->vi1, 1);
                h = fnv(h, tri->vi2, 1);
            }
        }
    }
    return h;
}

static void log_level_stats(void) {
    char reasons[160];
    s32 i, t, n;
    if (sFrames == 0) {
        return;
    }
    for (t = 0; t < sNumTables; t++) {
        HsStats *s = &sTables[t].stats;
        n = 0;
        reasons[0] = '\0';
        for (i = 0; i < HIDDEN_NR_COUNT; i++) {
            if (s->framesNoRow[i] > 0) {
                n += plat_snprintf(reasons + n, sizeof(reasons) - n, "%s%s %u", n > 0 ? ", " : "", sNoRowNames[i],
                                   (unsigned) s->framesNoRow[i]);
            }
        }
        plat_log("hidden skip%s%s: level %d, %u frames, %u with a row (%.1f%%), walked tris %.0f a frame, skipped "
                 "%.0f (%.1f%%), batches skipped %.1f of %.1f a frame; no row: %s",
                 sTables[t].name[0] ? " " : "", sTables[t].name, (int) sLevel, (unsigned) sFrames,
                 (unsigned) s->framesRow, 100.0 * s->framesRow / sFrames, (double) sWalkedTris / sFrames,
                 (double) s->skippedTris / sFrames, sWalkedTris > 0 ? 100.0 * s->skippedTris / sWalkedTris : 0.0,
                 (double) s->skippedBatches / sFrames, (double) sWalkedBatches / sFrames, n > 0 ? reasons : "none");
        if (s->checkFrames > 0) {
            plat_log("hidden check%s%s: level %d, frames %u, frames with pixels %u, most pixels %u, pixels %llu, %s%s",
                     sTables[t].name[0] ? " " : "", sTables[t].name, (int) sLevel, (unsigned) s->checkFrames,
                     (unsigned) s->checkFramesPixels, (unsigned) s->checkMost, s->checkPixels,
                     s->checkPixels == 0 ? "PASS" : "FAILED", sBreak ? " (broken on purpose)" : "");
        }
        __builtin_memset(s, 0, sizeof(*s));
    }
    sFrames = 0;
    sWalkedTris = sWalkedBatches = 0;
}

/* The last level's lines (the host calls this at exit). */
void port_hidden_exit_log(void) {
    log_level_stats();
}

/* Reads and checks one block against the loaded model (whose batch numbering is in sFirst). */
static s32 table_open(HsTable *t, const u8 *b, s32 size, s32 courseId, u32 hash) {
    static const char *const sWhy[4] = { "", "does not match this ROM", "is damaged",
                                         "batch numbering does not match" };
    s32 err = hidden_table_open(&t->t, b, size, courseId, hash, sNumSegments, sFirst);
    if (err == 0 && t->t.rowBytes > HS_MAX_ROW_BYTES) {
        err = 2;
    }
    if (err != 0) {
        plat_log("hidden skip: level %d table %s, off", (int) courseId, sWhy[err]);
        return FALSE;
    }
    sRowBytes = t->t.rowBytes;
    plat_log("hidden skip%s%s: level %d table %d bytes, %d cells, %d rows (%dx%d sub-cells, %d sectors)",
             t->name[0] ? " " : "", t->name, (int) courseId, (int) size, (int) t->t.numCells, (int) t->t.numRows,
             (int) t->t.subN, (int) t->t.subN, (int) t->t.sectors);
    return TRUE;
}

/* Finds the level's block (the generated list; on the host DKR_HIDDEN_TABLE_DIR, or the DKR_HIDDEN_EVAL list, first)
 * and checks it against the loaded model. */
void port_hidden_level_load(LevelModel *model, s32 courseId) {
    const u8 *b = NULL;
    s32 size = 0, i, s, total;
    u32 hash;
    log_level_stats();
    sModel = model;
    sLevel = courseId;
    sNumTables = 0;
    sInvert = 0;
    for (i = 0; i < HS_MAX_TABLES; i++) {
        sTables[i].t.block = NULL;
        sTables[i].row = NULL;
    }
    if (model == NULL || model->numberOfSegments <= 0 || model->numberOfSegments > HS_MAX_SEGMENTS) {
        return;
    }
    total = 0;
    for (s = 0; s < model->numberOfSegments; s++) {
        sFirst[s] = (u16) total;
        total += model->segments[s].numberOfBatches;
    }
    sFirst[s] = (u16) total;
    sNumSegments = model->numberOfSegments;
    hash = 0;
#ifdef PORT_HIDDEN_BAKE
    {
        // The cell size evaluation (port/tools/hidden_bake.py evaluate): several tables at once; the first one skips,
        // every one is counted and checked.
        int n = hidden_host_eval_count(), k, hostSize;
        if (n > 0) {
            hash = port_hidden_geom_hash(model);
            for (k = 0; k < n && sNumTables < HS_MAX_TABLES; k++) {
                HsTable *t = &sTables[sNumTables];
                b = hidden_host_eval_table(k, courseId, &hostSize, t->name, sizeof(t->name));
                if (b != NULL && table_open(t, b, hostSize, courseId, hash)) {
                    sNumTables++;
                } else {
                    sNumTables = 0; // all or none, so the counts line up
                    return;
                }
            }
            return;
        }
        b = hidden_host_table(courseId, &hostSize);
        size = hostSize;
    }
#endif
    for (i = 0; b == NULL && i < (s32) ARRAY_COUNT(sHiddenTables); i++) {
        if (sHiddenTables[i].level == courseId) {
            b = sHiddenTables[i].data;
            size = sHiddenTables[i].size;
        }
    }
    if (b == NULL) {
        return;
    }
    sTables[0].name[0] = '\0';
    if (table_open(&sTables[0], b, size, courseId, hash != 0 ? hash : port_hidden_geom_hash(model))) {
        sNumTables = 1;
    }
}

/* The drawn camera's pose, read as func_80067D3C reads it. */
static void read_pose(const Camera *cam, HiddenPose *p) {
    const LevelModelSegmentBoundingBox *bb;
    p->level = sLevel;
    p->seg = cam->cameraSegmentID;
    p->x = cam->trans.x_position;
    p->y = cam->trans.y_position + (gNoCamShake ? cam->shakeMagnitude : 0.0f);
    p->z = cam->trans.z_position;
    p->yaw = (u16) cam->trans.rotation.y_rotation;
    p->pitch = (s16) (cam->trans.rotation.x_rotation + cam->pitch);
    p->roll = cam->trans.rotation.z_rotation;
    p->fov100 = (int) (CAMERA_FOV(gCurCamFOV) * 100.0f + 0.5f);
    p->aspect1000 = (int) (CAMERA_ASPECT * 1000.0f + 0.5f);
    if (p->seg >= 0 && p->seg < gCurrentLevelModel->numberOfSegments) {
        bb = &gCurrentLevelModel->segmentsBoundingBoxes[p->seg];
        p->segBox[0] = bb->x1;
        p->segBox[1] = bb->y1;
        p->segBox[2] = bb->z1;
        p->segBox[3] = bb->x2;
        p->segBox[4] = bb->y2;
        p->segBox[5] = bb->z2;
    }
}

#ifdef PORT_HIDDEN_BAKE
/* The game's view-projection for a rotation alone (the camera at the origin): func_80067D3C's maths. */
void port_hidden_view_proj(unsigned short yaw, short pitch, short roll, float fovy, float aspect, float near,
                           float far, float out[16]) {
    ObjectTransform t;
    MtxF view, proj, vp;
    u16 norm;
    __builtin_memset(&t, 0, sizeof(t));
    t.rotation.y_rotation = (s16) (0x8000 + yaw);
    t.rotation.x_rotation = pitch;
    t.rotation.z_rotation = roll;
    t.scale = 1.0f;
    mtxf_from_inverse_transform(&view, &t);
    guPerspectiveF(proj, &norm, fovy, aspect, near, far, 1.0f);
    mtxf_mul(&view, &proj, &vp);
    memcpy(out, vp, sizeof(float) * 16);
}

/* DKR_HIDDEN_BAKE: hands the level to the bake (port/host/hidden_bake.c) once. */
static void bake_level(void) {
    static s32 sBaked[256];
    static HiddenBatch *sBatches;
    static int *sFirstInt;
    static short *sBoxes;
    static u8 *sAllowed;
    HiddenLevel lv;
    s32 s, b, k, total = sFirst[sNumSegments];
    if (sLevel < 0 || sLevel >= 256 || sBaked[sLevel]) {
        return;
    }
    sBaked[sLevel] = 1;
    plat_free(sBatches);
    plat_free(sFirstInt);
    plat_free(sBoxes);
    plat_free(sAllowed);
    sBatches = plat_malloc(sizeof(HiddenBatch) * (total + 1));
    sFirstInt = plat_malloc(sizeof(int) * (sNumSegments + 1));
    sBoxes = plat_malloc(sizeof(short) * 6 * sNumSegments);
    sAllowed = plat_malloc(sNumSegments * sNumSegments);
    if (sBatches == NULL || sFirstInt == NULL || sBoxes == NULL || sAllowed == NULL) {
        plat_log("hidden bake: out of memory");
        return;
    }
    for (s = 0; s < sNumSegments; s++) {
        const LevelModelSegment *seg = &gCurrentLevelModel->segments[s];
        const LevelModelSegmentBoundingBox *bb = &gCurrentLevelModel->segmentsBoundingBoxes[s];
        s32 row = seg->unk28;
        sFirstInt[s] = sFirst[s];
        sBoxes[s * 6 + 0] = bb->x1;
        sBoxes[s * 6 + 1] = bb->y1;
        sBoxes[s * 6 + 2] = bb->z1;
        sBoxes[s * 6 + 3] = bb->x2;
        sBoxes[s * 6 + 4] = bb->y2;
        sBoxes[s * 6 + 5] = bb->z2;
        for (k = 0; k < sNumSegments; k++) {
            // add_segment_to_order: Rare's row for a camera in segment s (gSceneStartSegment = unk28; -1: all)
            sAllowed[s * sNumSegments + k] =
                row == -1 ? 1 : (gCurrentLevelModel->segmentsBitfields[row + (k >> 3)] >> (k & 7)) & 1;
        }
        for (b = 0; b < seg->numberOfBatches; b++) {
            const TriangleBatchInfo *bi = &seg->batches[b];
            HiddenBatch *hb = &sBatches[sFirst[s] + b];
            s32 batchFlags = bi->flags, textureFlags = 0, opaque;
            if (bi->textureIndex != 0xFF) {
                textureFlags = gCurrentLevelModel->textures[bi->textureIndex].texture->flags;
            }
            // render_level_segment's pass rule, and occl_count's occluder rule (PR #602)
            opaque = (!(textureFlags & RENDER_SEMI_TRANSPARENT) && !(batchFlags & RENDER_WATER)) ||
                     (batchFlags & RENDER_DECAL);
            hb->verts = &seg->vertices[bi->verticesOffset];
            hb->tris = &seg->triangles[bi->facesOffset];
            hb->numVerts = bi[1].verticesOffset - bi->verticesOffset;
            hb->numTris = bi[1].facesOffset - bi->facesOffset;
            hb->kind = (opaque ? HIDDEN_BATCH_OPAQUE : 0) |
                       ((opaque && !((batchFlags | textureFlags) & (RENDER_CUTOUT | RENDER_DECAL |
                                                                    RENDER_SEMI_TRANSPARENT | RENDER_VTX_ALPHA |
                                                                    RENDER_WATER)))
                            ? HIDDEN_BATCH_OCCLUDER
                            : 0) |
                       ((batchFlags & (RENDER_HIDDEN | RENDER_WATER)) ? HIDDEN_BATCH_ALWAYS : 0);
        }
    }
    sFirstInt[sNumSegments] = total;
    lv.level = sLevel;
    lv.numSegments = sNumSegments;
    lv.totalBatches = total;
    lv.geomHash = port_hidden_geom_hash(gCurrentLevelModel);
    lv.segBoxes = sBoxes;
    lv.firstBatch = sFirstInt;
    lv.batches = sBatches;
    lv.allowed = sAllowed;
    lv.near = CAMERA_NEAR;
    lv.far = CAMERA_FAR;
    hidden_bake_level(&lv);
}
#endif


/* DKR_HIDDEN_SKIP_BREAK: the row with the biggest opaque batch of the camera's segment that it marks seen cleared. */
static const u8 *break_row(HsTable *t, const u8 *row, s32 seg) {
    const LevelModelSegment *s = &gCurrentLevelModel->segments[seg];
    s32 b, best = -1, bestTris = 0;
    __builtin_memcpy(t->breakRow, row, sRowBytes);
    for (b = 0; b < s->numberofOpaqueBatches; b++) {
        s32 bit = sFirst[seg] + b, tris = s->batches[b + 1].facesOffset - s->batches[b].facesOffset;
        if (hidden_row_bit(row, bit) && !(s->batches[b].flags & (RENDER_HIDDEN | RENDER_WATER)) &&
            tris > bestTris) {
            best = bit;
            bestTris = tris;
        }
    }
    if (best >= 0) {
        t->breakRow[best >> 3] &= (u8) ~(1u << (best & 7));
    }
    return t->breakRow;
}

/*
 * render_level_geometry_and_objects, once a viewport a frame (after the segments are picked): whether batches are
 * tested this frame (the switch is on). Picks the row for the drawn camera.
 */
s32 port_hidden_frame(const Camera *cam) {
    HiddenPose p;
    s32 reason = -1, t;
    switches();
    sInvert = 0;
    for (t = 0; t < sNumTables; t++) {
        sTables[t].row = NULL;
    }
#ifdef PORT_HIDDEN_BAKE
    {
        static s32 sPoses = -1, sBake = -1;
        if (sPoses < 0) {
            sPoses = plat_getenv("DKR_HIDDEN_POSES") != NULL && plat_getenv("DKR_HIDDEN_POSES")[0] != '0';
            sBake = plat_getenv("DKR_HIDDEN_BAKE") != NULL;
        }
        if (sBake && sModel == gCurrentLevelModel) {
            bake_level();
        }
        if (sPoses && !gCutsceneCameraActive && cam->mode != CAMERA_FINISH_RACE && get_race_countdown() == 0 &&
            cam->cameraSegmentID >= 0 && cam->cameraSegmentID < gCurrentLevelModel->numberOfSegments) {
            read_pose(cam, &p);
            hidden_poses_frame(&p);
        }
    }
#endif
    if (!sOn) {
        return FALSE;
    }
    sFrames++;
    if (sNumTables == 0 || sModel != gCurrentLevelModel) {
        return TRUE; // No table for this level: every batch is drawn
    }
    read_pose(cam, &p);
    sLastPose = p;
    sLastMode = cam->mode;
    if (port_portrait_active()) {
        reason = HIDDEN_NR_PORTRAIT;
    } else if (gCutsceneCameraActive) {
        reason = HIDDEN_NR_CUTSCENE;
    }
    for (t = 0; t < sNumTables; t++) {
        HsTable *tb = &sTables[t];
        s32 why = reason;
        if (why < 0) {
            tb->row = hidden_table_row(&tb->t, &p, (int *) &why);
            if (tb->row != NULL && sBreak) {
                tb->row = break_row(tb, tb->row, p.seg);
            }
        }
        if (tb->row != NULL) {
            tb->stats.framesRow++;
        } else {
            tb->stats.framesNoRow[why]++;
        }
    }
#ifdef PORT_HIDDEN_BAKE
    {
        // DKR_HIDDEN_TRACE=1 (host): table 0's pose, key and zero bits every frame, to find a cell that misses
        static s32 sTrace = -1, sTraceFrame;
        if (sTrace < 0) {
            sTrace = plat_getenv("DKR_HIDDEN_TRACE") != NULL;
        }
        sTraceFrame++;
        if (sTrace && sNumTables > 0) {
            const HiddenTable *ht = &sTables[0].t;
            s32 zeros = 0, b;
            if (sTables[0].row != NULL) {
                for (b = 0; b < ht->totalBatches; b++) {
                    zeros += !hidden_row_bit(sTables[0].row, b);
                }
            }
            plat_log("hidden trace %d: seg %d pos %.0f %.0f %.0f yaw %u pitch %d roll %d sub %d sector %d row %s zeros %d",
                     (int) sTraceFrame, p.seg, p.x, p.y, p.z, p.yaw, p.pitch, p.roll,
                     p.seg >= 0 ? hidden_sub_cell(p.segBox, p.x, p.z, ht->subN) : -1, hidden_sector(p.yaw, ht->sectors),
                     sTables[0].row != NULL ? "yes" : "no", (int) zeros);
        }
    }
#endif
    return TRUE;
}

/* render_level_segment, for each batch it would draw while port_hidden_frame said so: TRUE leaves the batch out. */
s32 port_hidden_batch(s32 bit, s32 numTris) {
    s32 t, out = FALSE;
    if (sInvert) {
        const u8 *row = sTables[sCheckTable].row;
        return row == NULL || hidden_row_bit(row, bit); // The check pass draws only what was left out
    }
    sWalkedTris += numTris;
    sWalkedBatches++;
    for (t = 0; t < sNumTables; t++) {
        const u8 *row = sTables[t].row;
        if (row != NULL && !hidden_row_bit(row, bit)) {
            sTables[t].stats.skippedTris += numTris;
            sTables[t].stats.skippedBatches++;
            out |= t == 0;
        }
    }
    return out;
}

s32 port_hidden_first_batch(s32 seg) {
    return seg >= 0 && seg <= sNumSegments ? sFirst[seg] : 0;
}

/*
 * The self-check passes (DKR_HIDDEN_SKIP_CHECK), one a table: -1 when there are no more; 0 to leave table k out this
 * frame (it has no row); 1 when render_level_segment is to draw only the batches table k left out, until
 * port_hidden_check_end.
 */
s32 port_hidden_check_begin(s32 k) {
    if (!sCheck || k >= sNumTables) {
        return -1;
    }
    if (sTables[k].row == NULL) {
        return 0;
    }
    sInvert = 1;
    sCheckTable = k;
    return 1;
}

void port_hidden_check_end(void) {
    sInvert = 0;
}

/* The renderer's count for one frame's check pass of table k (host gfx_soft.c; the 3DS's stencil count in PR 2). */
void port_hidden_check_pixels(int k, unsigned int pixels) {
    HsStats *s;
    if (k < 0 || k >= HS_MAX_TABLES) {
        return;
    }
    s = &sTables[k].stats;
    s->checkFrames++;
    if (pixels >= 1000 && sLastLogs < 20) {
        // A big miss: where the camera was (the latest pose; the renderer may be a frame behind)
        sLastLogs++;
        plat_log("hidden check%s%s: %u pixels in one frame; latest pose seg %d at %.0f %.0f %.0f yaw %u pitch %d roll %d "
                 "camera mode %d",
                 sTables[k].name[0] ? " " : "", sTables[k].name, pixels, sLastPose.seg, sLastPose.x, sLastPose.y,
                 sLastPose.z, sLastPose.yaw, sLastPose.pitch, sLastPose.roll, (int) sLastMode);
    }
    if (pixels > 0) {
        s->checkFramesPixels++;
        s->checkPixels += pixels;
        if (pixels > s->checkMost) {
            s->checkMost = pixels;
        }
    }
}
