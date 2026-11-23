/*
 * DKR_HIDDEN_BAKE=<pose file> (host only, P67 proposals/3ds-hidden-batch-skip-plan.md section 3.3): bakes the hidden
 * track batch tables of every level in the pose file (port/data/hidden_poses.txt, made by DKR_HIDDEN_POSES and merged
 * by port/tools/hidden_bake.py) as the game loads it (DKR_WARP visits them). hidden_skip.c hands over the level model
 * (batches, segment boxes, Rare's segment rows) and the game's own view matrix code.
 *
 * Cells: (camera segment, X/Z sub-cell of the segment's box split N x N, yaw sector of S). A cell exists where the
 * pose log has at least DKR_HIDDEN_MIN_FRAMES (30) frames for it. Its gates: the logged position box grown by 32
 * units in X/Z and 16 in Y, the pitch range grown by 2 degrees, the roll range by 1 degree; the table's field of view
 * and aspect are the widest logged.
 *
 * Sampling. Which surfaces a camera can see from a point does not depend on where it looks: turning only changes what
 * is inside the frame. So instead of drawing every sampled direction at the screen's size (the plan's yaw x pitch x
 * roll loop), the bake draws, at each sample position, the whole sphere once (a cube of six 90 degree views,
 * DKR_HIDDEN_BAKE_RES pixels square, default 512) and then asks, for each pixel where a batch shows, whether that
 * direction lies inside the frame of some camera pose the cell allows (a direction mask per cell, 128 x 128 a face,
 * made from the game's own view-projection over the cell's yaw sector +-2 degrees, pitch and roll ranges in 1 degree
 * steps, at the widest field of view and aspect, widened by 6% to cover the steps and the mask's own pixels; each bin
 * keeps how far outside every such frame it is, so a pixel up to eps / distance outside still counts). The
 * near and far planes stay safe for every pose: the occluders are cut at near / cos(55 degrees) and far * cos(55
 * degrees) (nothing a real view cuts away can hide anything), the tested batches at near * 0.5 (nothing a real view
 * keeps is cut away). As in the plan: occluders are the opaque-pass batches without cut-outs, decals, blending, vertex
 * alpha or water of the segments Rare's row allows from the cell's segment (a segment the frame never draws can only
 * hold surfaces outside the frame, which hide nothing in it); every batch of those segments is tested; a pixel counts
 * as seen when it is in front of the shrunk occluders (below) with 0.1% of depth to spare, test coverage widened by
 * one pixel.
 *
 * Sample positions: a lattice (16 units in X/Z, 8 in Y; both doubled while there are more than
 * DKR_HIDDEN_BAKE_MAX_POINTS) over the gates of the finest logged boxes the cell is made of (32 units in X/Z, 16 in Y,
 * plus half a step), so every eye a table lets through is within eps (the lattice's half diagonal) of a sample. One
 * sample stands for all of them because the occluders are shrunk by eps first (occluder shrinking, Wonka et al. 2000;
 * ocr_shrink does it in image space: each pixel's depth becomes the farthest within eps / its distance). Without it
 * the host check found cliff tops missed over silhouettes in up to 5% of frames. Then each cell's row is joined with those of the same segment's same-sector cells whose
 * gates overlap its own (the plan's neighbour union).
 *
 * Every cell size of DKR_HIDDEN_BAKE_CONFIGS (default "1x8,2x8,4x8,1x16,2x16,4x16") is baked in the same pass (one cube
 * a sample position serves them all), each to <DKR_HIDDEN_BAKE_OUT>/<NxS>/hidden_<level>.bin. Threads:
 * DKR_HIDDEN_BAKE_THREADS (default 4); the result does not depend on them. When every level of the pose file is
 * done, the game exits.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "plat/plat.h"
#include "port_hidden_skip.h"
#include "occl_raster.h"

void port_hidden_exit_log(void);

#define HB_MAX_CONFIGS 8
#define HB_BIN 128
#define HB_BINS (6 * HB_BIN * HB_BIN)
#define HB_MARGIN_XZ 32
#define HB_MARGIN_Y 16
#define HB_STEP_XZ 16
#define HB_STEP_Y 8
#define HB_PITCH_MARGIN 364 /* 2 degrees */
#define HB_ROLL_MARGIN 182  /* 1 degree */
#define HB_YAW_MARGIN 364
#define HB_STEP_ANGLE 182 /* 1 degree */
#define HB_WIDEN 1.06f

/* ---- The pose file ---- */

typedef struct {
    int level, seg, sub, sector, frames;
    int box[6];
    int pmin, pmax, rmin, rmax, fov100, aspect1000;
} HbKey;

static HbKey *sKeys;
static int sNumKeys, sLoaded;
static int sLevelsLeft[256], sNumLevelsLeft;

static void load_poses(void) {
    const char *path = getenv("DKR_HIDDEN_BAKE");
    FILE *f;
    HbKey k;
    int cap = 0;
    sLoaded = 1;
    f = path != NULL ? fopen(path, "r") : NULL;
    if (f == NULL) {
        plat_log("hidden bake: can't read the pose file %s", path != NULL ? path : "(none)");
        return;
    }
    while (fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &k.level, &k.seg, &k.sub, &k.sector,
                  &k.frames, &k.box[0], &k.box[1], &k.box[2], &k.box[3], &k.box[4], &k.box[5], &k.pmin, &k.pmax,
                  &k.rmin, &k.rmax, &k.fov100, &k.aspect1000) == 17) {
        if (k.level < 0 || k.level >= 256) {
            continue;
        }
        if (sNumKeys == cap) {
            cap = cap ? cap * 2 : 1024;
            sKeys = realloc(sKeys, sizeof(HbKey) * cap);
        }
        sKeys[sNumKeys++] = k;
        if (!sLevelsLeft[k.level]) {
            sLevelsLeft[k.level] = 1;
            sNumLevelsLeft++;
        }
    }
    fclose(f);
    plat_log("hidden bake: %d pose keys, %d levels in %s", sNumKeys, sNumLevelsLeft, path);
}

/* ---- Cells ---- */

typedef struct {
    int seg, sub, sector, frames;
    int raw[6];  /* the logged box (union of its keys) */
    short gate[6];
    short pmin, pmax, rmin, rmax;
    int fov100, aspect1000;
    unsigned char *mask; /* HB_BINS bytes: how far outside every allowed pose's frame (cell_mask) */
    unsigned char *seen; /* totalBatches bytes */
    int row;
} HbCell;

typedef struct {
    int n, s;
    HbCell *cells;
    int numCells;
    int fovMax100, aspectMax1000;
} HbConfig;

static HbConfig sConfigs[HB_MAX_CONFIGS];
static int sNumConfigs;
static const HiddenLevel *sLv;
static int sMinFrames;

static int coarse_key(const HbConfig *cf, int seg, int sub4, int sector16) {
    int sx = (sub4 % 4) * cf->n / 4, sz = (sub4 / 4) * cf->n / 4;
    return (seg * cf->n * cf->n + sz * cf->n + sx) * cf->s + sector16 * cf->s / 16;
}

static short clamp16(int v) {
    return (short) (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* ---- Directions ---- */

static float sFace[6][16]; /* the cube's views (camera at the origin) */
static float sBinDir[HB_BINS][3];

/* The direction through screen point (nx, ny) of a view-projection (camera at the origin). */
static void view_dir(const float *vp, float nx, float ny, float d[3]) {
    float a[3], b[3], c3[3];
    int i;
    for (i = 0; i < 3; i++) {
        c3[i] = vp[i * 4 + 3];
        a[i] = vp[i * 4 + 0] - nx * c3[i];
        b[i] = vp[i * 4 + 1] - ny * c3[i];
    }
    d[0] = a[1] * b[2] - a[2] * b[1];
    d[1] = a[2] * b[0] - a[0] * b[2];
    d[2] = a[0] * b[1] - a[1] * b[0];
    if (d[0] * c3[0] + d[1] * c3[1] + d[2] * c3[2] < 0.0f) {
        d[0] = -d[0];
        d[1] = -d[1];
        d[2] = -d[2];
    }
    {
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        d[0] /= l;
        d[1] /= l;
        d[2] /= l;
    }
}

static const unsigned short sFaceYaw[6] = { 0, 0x4000, 0x8000, 0xC000, 0, 0 };
static const short sFacePitch[6] = { 0, 0, 0, 0, 0x4000, -0x4000 };

static void cube_setup(void) {
    int f, x, y;
    for (f = 0; f < 6; f++) {
        port_hidden_view_proj(sFaceYaw[f], sFacePitch[f], 0, 90.0f, 1.0f, 1.0f, 100.0f, sFace[f]);
        for (y = 0; y < HB_BIN; y++) {
            for (x = 0; x < HB_BIN; x++) {
                float nx = (x + 0.5f) / HB_BIN * 2.0f - 1.0f, ny = 1.0f - (y + 0.5f) / HB_BIN * 2.0f;
                view_dir(sFace[f], nx, ny, sBinDir[(f * HB_BIN + y) * HB_BIN + x]);
            }
        }
    }
}

/* How far outside the frames (widened by HB_WIDEN) of a set of views a direction is, as an angle in tenths of a
 * degree (0: inside one; 255: 25.5 degrees or more, or behind all). The nearest so far is kept with the limits on
 * |x/w| and |y/w| that a view must beat, so most views cost two compares and no arc tangent. */
typedef struct {
    float tanH, tanV, cH, cV; /* the frame's half widths (tangents), and the widened frame's edges (angles) */
    int best;
    float limX, limY;
} HbReach;

static void reach_limits(HbReach *r) {
    float a = r->best * (3.14159265f / 1800.0f), h = r->cH + a, v = r->cV + a;
    r->limX = h < 1.55f ? tanf(h) / r->tanH : 1e30f;
    r->limY = v < 1.55f ? tanf(v) / r->tanV : 1e30f;
}

static void reach_begin(HbReach *r, float tanH, float tanV) {
    r->tanH = tanH;
    r->tanV = tanV;
    r->cH = atanf(HB_WIDEN * tanH);
    r->cV = atanf(HB_WIDEN * tanV);
    r->best = 255;
    reach_limits(r);
}

static void reach_view(HbReach *r, const float *vp, const float *d) {
    float x = d[0] * vp[0] + d[1] * vp[4] + d[2] * vp[8];
    float y = d[0] * vp[1] + d[1] * vp[5] + d[2] * vp[9];
    float w = d[0] * vp[3] + d[1] * vp[7] + d[2] * vp[11], ax, ay, a;
    int t;
    if (w <= 0.0f) {
        return;
    }
    x = fabsf(x) / w;
    y = fabsf(y) / w;
    if (x >= r->limX || y >= r->limY) {
        return; // no nearer than the best so far
    }
    if (x <= HB_WIDEN && y <= HB_WIDEN) {
        r->best = 0;
        reach_limits(r);
        return;
    }
    ax = x > HB_WIDEN ? atanf(x * r->tanH) - r->cH : 0.0f;
    ay = y > HB_WIDEN ? atanf(y * r->tanV) - r->cV : 0.0f;
    a = (ax > ay ? ax : ay) * (1800.0f / 3.14159265f);
    t = a >= 254.0f ? 255 : (int) a + 1;
    if (t < r->best) {
        r->best = t;
        reach_limits(r);
    }
}

/* The yaw (u16) whose forward direction has horizontal angle psi: the game's yaw maps linearly to it. */
static float sPsi0, sPsiPerUnit;

static void yaw_setup(const HbConfig *cf) {
    float vp[16], d0[3], d1[3];
    (void) cf;
    port_hidden_view_proj(0, 0, 0, 60.0f, 1.0f, 1.0f, 100.0f, vp);
    view_dir(vp, 0.0f, 0.0f, d0);
    port_hidden_view_proj(0x1000, 0, 0, 60.0f, 1.0f, 1.0f, 100.0f, vp);
    view_dir(vp, 0.0f, 0.0f, d1);
    sPsi0 = atan2f(d0[0], d0[2]);
    {
        float a = atan2f(d1[0], d1[2]) - sPsi0;
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        sPsiPerUnit = a / 4096.0f;
    }
}

/* The cell's direction mask: for each bin, how far its centre lies outside the frame of the nearest pose the cell
 * allows (tenths of a degree, 0 inside): a pixel seen from a sample counts for the cell if the shift an eye up to eps
 * away makes (eps / its distance) can bring it into a frame. */
static void cell_mask(HbCell *c, const HbConfig *cf) {
    float fov = cf->fovMax100 / 100.0f, aspect = cf->aspectMax1000 / 1000.0f;
    int yaw0 = c->sector * (65536 / cf->s) - HB_YAW_MARGIN, yaw1 = (c->sector + 1) * (65536 / cf->s) + HB_YAW_MARGIN;
    int np = 1 + (c->pmax - c->pmin + HB_STEP_ANGLE - 1) / HB_STEP_ANGLE;
    int nr = 1 + (c->rmax - c->rmin + HB_STEP_ANGLE - 1) / HB_STEP_ANGLE;
    int ny = 1 + (yaw1 - yaw0 + HB_STEP_ANGLE - 1) / HB_STEP_ANGLE;
    float *vps = malloc(sizeof(float) * 16 * np * nr * ny);
    float tanV = tanf(fov * 3.14159265f / 360.0f), tanH = tanV * aspect;
    int b, i, j, k;
    c->mask = malloc(HB_BINS);
    for (k = 0; k < ny; k++) {
        int yaw = yaw0 + (yaw1 - yaw0) * k / (ny - 1 > 0 ? ny - 1 : 1);
        for (i = 0; i < np; i++) {
            int pitch = c->pmin + (np > 1 ? (c->pmax - c->pmin) * i / (np - 1) : 0);
            for (j = 0; j < nr; j++) {
                int roll = c->rmin + (nr > 1 ? (c->rmax - c->rmin) * j / (nr - 1) : 0);
                port_hidden_view_proj((unsigned short) yaw, (short) pitch, (short) roll, fov, aspect, 1.0f, 100.0f,
                                      &vps[((k * np + i) * nr + j) * 16]);
            }
        }
    }
    for (b = 0; b < HB_BINS; b++) {
        const float *d = sBinDir[b];
        float psi = atan2f(d[0], d[2]), u;
        int best, dk;
        HbReach rc;
        // The yaw that points straight at this direction, kept inside the sector's window; then its neighbours
        u = (psi - sPsi0) / sPsiPerUnit; // in yaw units, modulo 65536
        u = fmodf(u - yaw0, 65536.0f);
        if (u < 0.0f) u += 65536.0f;
        if (u > (yaw1 - yaw0) + (65536.0f - (yaw1 - yaw0)) / 2) {
            best = 0; // nearer the window's start going round the other way
        } else if (u > yaw1 - yaw0) {
            best = ny - 1;
        } else {
            best = (int) (u / HB_STEP_ANGLE + 0.5f);
            best = best >= ny ? ny - 1 : best;
        }
        reach_begin(&rc, tanH, tanV);
        for (dk = 0; dk <= 3 && rc.best > 0; dk++) {
            int side;
            for (side = -1; side <= 1 && rc.best > 0; side += 2) {
                int kk = best + dk * side;
                if (kk < 0 || kk >= ny || (dk == 0 && side == 1)) {
                    continue;
                }
                for (i = 0; i < np * nr && rc.best > 0; i++) {
                    reach_view(&rc, &vps[(kk * np * nr + i) * 16], d);
                }
            }
        }
        if (rc.best > 0 && fabsf(d[1]) > 0.7f) {
            // Steep directions: the nearest yaw means little there; try them all
            for (k = 0; k < ny * np * nr && rc.best > 0; k++) {
                reach_view(&rc, &vps[k * 16], d);
            }
        }
        c->mask[b] = (unsigned char) rc.best;
    }
    free(vps);
}

/* ---- Sample positions and the cube ---- */

typedef struct {
    int x, y, z, seg;
    int numCells, firstCell; /* its cells: sCellRefs[firstCell ...] */
    HbCell **cells;
} HbPoint;

static HbPoint *sPoints;
static HbCell **sCellRefs;
static int sNumCellRefs, sCellRefsCap;
static int sNumPoints, sNextPoint, sRes = 512;
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long sTested, sVisited;
static time_t sStart;

static float sEps; /* the farthest any eye a table allows is from its nearest sample position (units) */

typedef struct {
    OcrCtx occ, test;
    float *depth, *eroded, *tab;
    HbPoint *pt;
    int face, batch;
} HbWork;

static float sOccFace[6][16], sTestFace[6][16];

static int visit(void *user, int x, int y, float depth) {
    HbWork *w = user;
    int bx = x * HB_BIN / sRes, by = y * HB_BIN / sRes, bin = (w->face * HB_BIN + by) * HB_BIN + bx, i, left = 0;
    // An eye eps away sees this pixel up to eps / distance off (tenths of a degree), plus the bin's own half width
    float reach = sEps * depth * (1800.0f / 3.14159265f) + 4.0f;
    for (i = 0; i < w->pt->numCells; i++) {
        HbCell *c = w->pt->cells[i];
        if (__atomic_load_n(&c->seen[w->batch], __ATOMIC_RELAXED)) {
            continue;
        }
        if (c->mask[bin] <= reach) {
            __atomic_store_n(&c->seen[w->batch], 1, __ATOMIC_RELAXED);
        } else {
            left++;
        }
    }
    return left == 0; // Every cell here already sees it: stop this batch
}

/* A view through face f with the camera at p: the face's rotation, then the translation. */
static void face_at(const float *rot, const HbPoint *p, float out[16]) {
    int c;
    memcpy(out, rot, sizeof(float) * 16);
    for (c = 0; c < 4; c++) {
        out[12 + c] = rot[12 + c] - (p->x * rot[c] + p->y * rot[4 + c] + p->z * rot[8 + c]);
    }
}

static void do_point(HbWork *w, HbPoint *p) {
    const HiddenLevel *lv = sLv;
    int f, s, b, i;
    w->pt = p;
    for (f = 0; f < 6; f++) {
        float vp[16];
        face_at(sOccFace[f], p, vp);
        ocr_begin(&w->occ, vp);
        for (s = 0; s < lv->numSegments; s++) {
            if (!lv->allowed[p->seg * lv->numSegments + s]) {
                continue;
            }
            for (b = lv->firstBatch[s]; b < lv->firstBatch[s + 1]; b++) {
                const HiddenBatch *hb = &lv->batches[b];
                if (hb->kind & HIDDEN_BATCH_OCCLUDER) {
                    ocr_batch(&w->occ, hb->verts, hb->numVerts, hb->tris, hb->numTris, 0);
                }
            }
        }
        ocr_shrink(&w->occ, w->eroded, w->tab, sEps);
        face_at(sTestFace[f], p, vp);
        memcpy(w->test.m, vp, sizeof(w->test.m));
        w->test.testDepth = w->eroded;
        w->face = f;
        for (s = 0; s < lv->numSegments; s++) {
            if (!lv->allowed[p->seg * lv->numSegments + s]) {
                continue;
            }
            for (b = lv->firstBatch[s]; b < lv->firstBatch[s + 1]; b++) {
                const HiddenBatch *hb = &lv->batches[b];
                int all = 1;
                if (hb->kind & HIDDEN_BATCH_ALWAYS) {
                    continue;
                }
                for (i = 0; i < p->numCells && all; i++) {
                    all = __atomic_load_n(&p->cells[i]->seen[b], __ATOMIC_RELAXED);
                }
                if (all) {
                    continue;
                }
                w->batch = b;
                __atomic_add_fetch(&sTested, 1, __ATOMIC_RELAXED);
                ocr_batch(&w->test, hb->verts, hb->numVerts, hb->tris, hb->numTris, 1);
            }
        }
    }
}

static void *worker(void *arg) {
    HbWork *w = arg;
    for (;;) {
        int i;
        pthread_mutex_lock(&sLock);
        i = sNextPoint++;
        pthread_mutex_unlock(&sLock);
        if (i >= sNumPoints) {
            break;
        }
        do_point(w, &sPoints[i]);
        if (__atomic_add_fetch(&sVisited, 1, __ATOMIC_RELAXED) % 5000 == 0) {
            plat_log("hidden bake: %llu of %d positions (%ld s)", sVisited, sNumPoints, (long) (time(NULL) - sStart));
        }
    }
    return NULL;
}

static HbCell *find_cell(HbConfig *cf, int key) {
    int lo = 0, hi = cf->numCells - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        HbCell *c = &cf->cells[mid];
        int k = (c->seg * cf->n * cf->n + c->sub) * cf->s + c->sector;
        if (k == key) {
            return c;
        }
        if (k < key) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static int cmp_cell(const void *a, const void *b) {
    const HbCell *x = a, *y = b;
    if (x->seg != y->seg) return x->seg - y->seg;
    if (x->sub != y->sub) return x->sub - y->sub;
    return x->sector - y->sector;
}

/* Merges the level's finest keys into the config's cells (sorted by key). */
static void build_cells(HbConfig *cf, int level) {
    int i, j, cap = 0;
    HbCell *all = NULL;
    int numAll = 0;
    for (i = 0; i < sNumKeys; i++) {
        const HbKey *k = &sKeys[i];
        int key, found = -1;
        if (k->level != level || k->seg < 0 || k->seg >= sLv->numSegments) {
            continue;
        }
        key = coarse_key(cf, k->seg, k->sub, k->sector);
        for (j = 0; j < numAll; j++) {
            HbCell *c = &all[j];
            if ((c->seg * cf->n * cf->n + c->sub) * cf->s + c->sector == key) {
                found = j;
                break;
            }
        }
        if (found < 0) {
            if (numAll == cap) {
                cap = cap ? cap * 2 : 256;
                all = realloc(all, sizeof(HbCell) * cap);
            }
            found = numAll++;
            memset(&all[found], 0, sizeof(HbCell));
            all[found].seg = k->seg;
            all[found].sub = (key / cf->s) % (cf->n * cf->n);
            all[found].sector = key % cf->s;
            memcpy(all[found].raw, k->box, sizeof(k->box));
            all[found].pmin = k->pmin;
            all[found].pmax = k->pmax;
            all[found].rmin = k->rmin;
            all[found].rmax = k->rmax;
        }
        {
            HbCell *c = &all[found];
            c->frames += k->frames;
            for (j = 0; j < 3; j++) {
                c->raw[j] = k->box[j] < c->raw[j] ? k->box[j] : c->raw[j];
                c->raw[j + 3] = k->box[j + 3] > c->raw[j + 3] ? k->box[j + 3] : c->raw[j + 3];
            }
            c->pmin = k->pmin < c->pmin ? k->pmin : c->pmin;
            c->pmax = k->pmax > c->pmax ? k->pmax : c->pmax;
            c->rmin = k->rmin < c->rmin ? k->rmin : c->rmin;
            c->rmax = k->rmax > c->rmax ? k->rmax : c->rmax;
            c->fov100 = k->fov100 > c->fov100 ? k->fov100 : c->fov100;
            c->aspect1000 = k->aspect1000 > c->aspect1000 ? k->aspect1000 : c->aspect1000;
        }
    }
    cf->numCells = 0;
    cf->cells = malloc(sizeof(HbCell) * (numAll + 1));
    cf->fovMax100 = cf->aspectMax1000 = 0;
    for (i = 0; i < numAll; i++) {
        HbCell *c = &all[i];
        if (c->frames < sMinFrames) {
            continue;
        }
        c->gate[0] = clamp16(c->raw[0] - HB_MARGIN_XZ);
        c->gate[1] = clamp16(c->raw[1] - HB_MARGIN_Y);
        c->gate[2] = clamp16(c->raw[2] - HB_MARGIN_XZ);
        c->gate[3] = clamp16(c->raw[3] + HB_MARGIN_XZ);
        c->gate[4] = clamp16(c->raw[4] + HB_MARGIN_Y);
        c->gate[5] = clamp16(c->raw[5] + HB_MARGIN_XZ);
        c->pmin = clamp16(c->pmin - HB_PITCH_MARGIN);
        c->pmax = clamp16(c->pmax + HB_PITCH_MARGIN);
        c->rmin = clamp16(c->rmin - HB_ROLL_MARGIN);
        c->rmax = clamp16(c->rmax + HB_ROLL_MARGIN);
        cf->fovMax100 = c->fov100 > cf->fovMax100 ? c->fov100 : cf->fovMax100;
        cf->aspectMax1000 = c->aspect1000 > cf->aspectMax1000 ? c->aspect1000 : cf->aspectMax1000;
        cf->cells[cf->numCells++] = *c;
    }
    free(all);
    qsort(cf->cells, cf->numCells, sizeof(HbCell), cmp_cell);
    for (i = 0; i < cf->numCells; i++) {
        cf->cells[i].seen = calloc(sLv->totalBatches, 1);
    }
}

/* The sample positions: lattice points near the finest logged boxes, each with the cells (of every config) it serves. */
static void build_points(int level, int stepXZ, int stepY) {
    // Every eye inside a gate (the logged box plus its margins) is within half a step on each axis of a sample
    int cap = 0, seg, i, nearXZ = HB_MARGIN_XZ + stepXZ / 2, nearY = HB_MARGIN_Y + stepY / 2;
    sNumPoints = 0;
    sNumCellRefs = 0;
    for (seg = 0; seg < sLv->numSegments; seg++) {
        int lo[3] = { 1 << 30, 1 << 30, 1 << 30 }, hi[3] = { -(1 << 30), -(1 << 30), -(1 << 30) }, any = 0;
        int x, y, z;
        for (i = 0; i < sNumKeys; i++) {
            const HbKey *k = &sKeys[i];
            if (k->level != level || k->seg != seg) {
                continue;
            }
            any = 1;
            lo[0] = k->box[0] - nearXZ < lo[0] ? k->box[0] - nearXZ : lo[0];
            lo[1] = k->box[1] - nearY < lo[1] ? k->box[1] - nearY : lo[1];
            lo[2] = k->box[2] - nearXZ < lo[2] ? k->box[2] - nearXZ : lo[2];
            hi[0] = k->box[3] + nearXZ > hi[0] ? k->box[3] + nearXZ : hi[0];
            hi[1] = k->box[4] + nearY > hi[1] ? k->box[4] + nearY : hi[1];
            hi[2] = k->box[5] + nearXZ > hi[2] ? k->box[5] + nearXZ : hi[2];
        }
        if (!any) {
            continue;
        }
#define LATTICE_LO(v, s) ((int) floorf((float) (v) / (s)) * (s))
        for (x = LATTICE_LO(lo[0], stepXZ); x <= hi[0]; x += stepXZ) {
            for (z = LATTICE_LO(lo[2], stepXZ); z <= hi[2]; z += stepXZ) {
                for (y = LATTICE_LO(lo[1], stepY); y <= hi[1]; y += stepY) {
                    HbPoint pt;
                    HbCell *found[HB_MAX_CONFIGS * 32];
                    int c;
                    memset(&pt, 0, sizeof(pt));
                    pt.x = x;
                    pt.y = y;
                    pt.z = z;
                    pt.seg = seg;
                    for (i = 0; i < sNumKeys; i++) {
                        const HbKey *k = &sKeys[i];
                        if (k->level != level || k->seg != seg || x < k->box[0] - nearXZ ||
                            x > k->box[3] + nearXZ || z < k->box[2] - nearXZ || z > k->box[5] + nearXZ ||
                            y < k->box[1] - nearY || y > k->box[4] + nearY) {
                            continue;
                        }
                        for (c = 0; c < sNumConfigs; c++) {
                            HbCell *cell = find_cell(&sConfigs[c], coarse_key(&sConfigs[c], seg, k->sub, k->sector));
                            int j, dup = 0;
                            if (cell == NULL) {
                                continue; // too few frames there
                            }
                            for (j = 0; j < pt.numCells; j++) {
                                dup |= found[j] == cell;
                            }
                            if (!dup && pt.numCells < (int) (sizeof(found) / sizeof(found[0]))) {
                                found[pt.numCells++] = cell;
                            }
                        }
                    }
                    if (pt.numCells == 0) {
                        continue;
                    }
                    if (sNumCellRefs + pt.numCells > sCellRefsCap) {
                        sCellRefsCap = (sCellRefsCap + pt.numCells) * 2;
                        sCellRefs = realloc(sCellRefs, sizeof(HbCell *) * sCellRefsCap);
                    }
                    pt.firstCell = sNumCellRefs;
                    memcpy(sCellRefs + sNumCellRefs, found, sizeof(HbCell *) * pt.numCells);
                    sNumCellRefs += pt.numCells;
                    if (sNumPoints == cap) {
                        cap = cap ? cap * 2 : 4096;
                        sPoints = realloc(sPoints, sizeof(HbPoint) * cap);
                    }
                    sPoints[sNumPoints++] = pt;
                }
            }
        }
    }
    for (i = 0; i < sNumPoints; i++) {
        sPoints[i].cells = sCellRefs + sPoints[i].firstCell;
    }
}

static int boxes_overlap(const short *a, const short *b) {
    return a[0] <= b[3] && b[0] <= a[3] && a[1] <= b[4] && b[1] <= a[4] && a[2] <= b[5] && b[2] <= a[5];
}

static void put16(unsigned char **p, int v) {
    (*p)[0] = (unsigned char) (v & 0xFF);
    (*p)[1] = (unsigned char) ((v >> 8) & 0xFF);
    *p += 2;
}

/* Rows (seen bits, the batches the table never decides, the segments the cell's segment never draws), the neighbour
 * union, then the block. */
static void write_config(HbConfig *cf, const char *outDir) {
    const HiddenLevel *lv = sLv;
    int rowBytes = (lv->totalBatches + 7) / 8, i, j, s, b, numRows = 0, slots;
    unsigned char *rows = calloc((size_t) cf->numCells + 1, rowBytes), *uniq = calloc((size_t) cf->numCells + 1, rowBytes);
    unsigned char *block, *p;
    unsigned short *cellIndex;
    char path[512];
    FILE *f;
    unsigned long long hidTris = 0, allTris = 0;
    for (i = 0; i < cf->numCells; i++) {
        HbCell *c = &cf->cells[i];
        unsigned char *r = rows + (size_t) i * rowBytes;
        for (s = 0; s < lv->numSegments; s++) {
            int allowed = lv->allowed[c->seg * lv->numSegments + s];
            for (b = lv->firstBatch[s]; b < lv->firstBatch[s + 1]; b++) {
                int on = !allowed || (lv->batches[b].kind & HIDDEN_BATCH_ALWAYS) || c->seen[b];
                for (j = 0; j < cf->numCells && !on; j++) {
                    // Neighbour union: the same segment's same-sector cells whose gates overlap this one's
                    const HbCell *o = &cf->cells[j];
                    on = o != c && o->seg == c->seg && o->sector == c->sector && boxes_overlap(o->gate, c->gate) &&
                         o->seen[b];
                }
                if (on) {
                    r[b >> 3] |= (unsigned char) (1u << (b & 7));
                } else if (allowed) {
                    hidTris += lv->batches[b].numTris;
                }
                if (allowed) {
                    allTris += lv->batches[b].numTris;
                }
            }
        }
        c->row = -1;
        for (j = 0; j < numRows && c->row < 0; j++) {
            if (memcmp(uniq + (size_t) j * rowBytes, r, rowBytes) == 0) {
                c->row = j;
            }
        }
        if (c->row < 0) {
            memcpy(uniq + (size_t) numRows * rowBytes, r, rowBytes);
            c->row = numRows++;
        }
    }
    slots = lv->numSegments * cf->n * cf->n * cf->s;
    cellIndex = malloc(sizeof(unsigned short) * slots);
    for (i = 0; i < slots; i++) {
        cellIndex[i] = HIDDEN_NO_CELL;
    }
    for (i = 0; i < cf->numCells; i++) {
        HbCell *c = &cf->cells[i];
        cellIndex[(c->seg * cf->n * cf->n + c->sub) * cf->s + c->sector] = (unsigned short) i;
    }
    block = calloc(28 + lv->numSegments * 2 + slots * 2 + cf->numCells * HIDDEN_CELL_BYTES + numRows * rowBytes, 1);
    p = block;
    put16(&p, HIDDEN_MAGIC & 0xFFFF);
    put16(&p, HIDDEN_MAGIC >> 16);
    put16(&p, HIDDEN_VERSION);
    put16(&p, lv->level);
    put16(&p, (int) (lv->geomHash & 0xFFFF));
    put16(&p, (int) (lv->geomHash >> 16));
    put16(&p, lv->numSegments);
    put16(&p, lv->totalBatches);
    *p++ = (unsigned char) cf->n;
    *p++ = (unsigned char) cf->s;
    put16(&p, rowBytes);
    put16(&p, cf->fovMax100);
    put16(&p, cf->aspectMax1000);
    put16(&p, cf->numCells);
    put16(&p, numRows);
    for (s = 0; s < lv->numSegments; s++) {
        put16(&p, lv->firstBatch[s]);
    }
    for (i = 0; i < slots; i++) {
        put16(&p, cellIndex[i]);
    }
    for (i = 0; i < cf->numCells; i++) {
        HbCell *c = &cf->cells[i];
        for (j = 0; j < 6; j++) {
            put16(&p, c->gate[j]);
        }
        put16(&p, c->pmin);
        put16(&p, c->pmax);
        put16(&p, c->rmin);
        put16(&p, c->rmax);
        put16(&p, c->row);
    }
    memcpy(p, uniq, (size_t) numRows * rowBytes);
    p += numRows * rowBytes;
    snprintf(path, sizeof(path), "%s/%dx%d", outDir, cf->n, cf->s);
    mkdir(outDir, 0777);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/%dx%d/hidden_%d.bin", outDir, cf->n, cf->s, lv->level);
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(block, 1, p - block, f);
        fclose(f);
    }
    plat_log("hidden bake: level %d %dx%d: %d cells, %d rows, %d bytes; a cell leaves out %.1f%% of the triangles of "
             "the segments its row allows (average) -> %s",
             lv->level, cf->n, cf->s, cf->numCells, numRows, (int) (p - block),
             allTris > 0 ? 100.0 * hidTris / allTris : 0.0, path);
    free(block);
    free(cellIndex);
    free(rows);
    free(uniq);
}

int hidden_bake_level(const HiddenLevel *lv) {
    const char *e;
    const char *outDir = getenv("DKR_HIDDEN_BAKE_OUT") ? getenv("DKR_HIDDEN_BAKE_OUT") : ".";
    int i, c, threads = 4, stepXZ = HB_STEP_XZ, stepY = HB_STEP_Y, maxPoints = 400000;
    float nearOcc, farOcc, nearTest;
    pthread_t tid[32];
    HbWork *work;
    time_t t0 = time(NULL);
    sStart = t0;
    if (!sLoaded) {
        load_poses();
    }
    if (lv->level < 0 || lv->level >= 256 || !sLevelsLeft[lv->level]) {
        return 0;
    }
    sLv = lv;
    e = getenv("DKR_HIDDEN_MIN_FRAMES");
    sMinFrames = e != NULL ? atoi(e) : 30;
    e = getenv("DKR_HIDDEN_BAKE_THREADS");
    threads = e != NULL ? atoi(e) : 4;
    threads = threads < 1 ? 1 : threads > 32 ? 32 : threads;
    e = getenv("DKR_HIDDEN_BAKE_RES");
    sRes = e != NULL ? atoi(e) : 512;
    e = getenv("DKR_HIDDEN_BAKE_MAX_POINTS");
    maxPoints = e != NULL ? atoi(e) : maxPoints;
    e = getenv("DKR_HIDDEN_BAKE_CONFIGS");
    if (e == NULL) {
        e = "1x8,2x8,4x8,1x16,2x16,4x16";
    }
    sNumConfigs = 0;
    while (*e && sNumConfigs < HB_MAX_CONFIGS) {
        int n = atoi(e), s;
        const char *x = strchr(e, 'x');
        if (x == NULL) {
            break;
        }
        s = atoi(x + 1);
        if ((n == 1 || n == 2 || n == 4) && (s == 4 || s == 8 || s == 16)) {
            memset(&sConfigs[sNumConfigs], 0, sizeof(HbConfig));
            sConfigs[sNumConfigs].n = n;
            sConfigs[sNumConfigs].s = s;
            sNumConfigs++;
        }
        e = strchr(e, ',');
        if (e == NULL) {
            break;
        }
        e++;
    }
    cube_setup();
    yaw_setup(&sConfigs[0]);
    for (c = 0; c < sNumConfigs; c++) {
        build_cells(&sConfigs[c], lv->level);
        for (i = 0; i < sConfigs[c].numCells; i++) {
            cell_mask(&sConfigs[c].cells[i], &sConfigs[c]);
        }
    }
    plat_log("hidden bake: level %d, cell masks done (%ld s)", lv->level, (long) (time(NULL) - t0));
    for (;;) {
        build_points(lv->level, stepXZ, stepY);
        if (sNumPoints <= maxPoints) {
            break;
        }
        plat_log("hidden bake: level %d, %d sample positions at %d/%d units: steps doubled", lv->level, sNumPoints,
                 stepXZ, stepY);
        stepXZ *= 2;
        stepY *= 2;
    }
    // The cube's six views: occluders cut where every real view would cut them, tests where none would (the real
    // views' corners are within 55 degrees of their centre; the cube's faces' within 55 too)
    {
        // The widest real view's corner angle (its cosine), and the cube face's (cos 54.7 degrees = 0.577)
        float tanV = 0.0f, cosReal;
        int aspect = 0;
        for (c = 0; c < sNumConfigs; c++) {
            float t = tanf(sConfigs[c].fovMax100 / 100.0f * 3.14159265f / 360.0f);
            tanV = t > tanV ? t : tanV;
            aspect = sConfigs[c].aspectMax1000 > aspect ? sConfigs[c].aspectMax1000 : aspect;
        }
        cosReal = 1.0f / sqrtf(1.0f + tanV * tanV * (1.0f + (aspect / 1000.0f) * (aspect / 1000.0f)));
        cosReal = cosReal < 0.57f ? cosReal : 0.57f;
        // Occluders from the farthest near plane the game uses (10 units, 2.5 in widescreen), tests from the nearest
        nearOcc = 10.0f / cosReal;
        farOcc = lv->far * 0.57f;
        nearTest = 2.5f * 0.5f;
    }
    for (i = 0; i < 6; i++) {
        port_hidden_view_proj(sFaceYaw[i], sFacePitch[i], 0, 90.0f, 1.0f, nearOcc, farOcc, sOccFace[i]);
        port_hidden_view_proj(sFaceYaw[i], sFacePitch[i], 0, 90.0f, 1.0f, nearTest, lv->far * 4.0f, sTestFace[i]);
    }
    work = calloc(threads, sizeof(HbWork));
    for (i = 0; i < threads; i++) {
        HbWork *w = &work[i];
        w->depth = malloc(sizeof(float) * sRes * sRes);
        w->eroded = malloc(sizeof(float) * sRes * sRes);
        w->tab = malloc(sizeof(float) * sRes * sRes * (OCR_SHRINK_LEVELS - 1));
        w->occ.w = w->occ.h = w->test.w = w->test.h = sRes;
        w->occ.depth = w->test.depth = w->depth;
        w->occ.cull = w->test.cull = -1;
        w->occ.farCut = 1;
        w->test.widen = 1.0f;
        w->test.visit = visit;
        w->test.user = w;
    }
    // Samples on a lattice that reaches the gates: any allowed eye is within half a step on each axis of one
    sEps = sqrtf(2.0f * (stepXZ * 0.5f) * (stepXZ * 0.5f) + (stepY * 0.5f) * (stepY * 0.5f)) + 1.0f;
    sNextPoint = 0;
    sTested = sVisited = 0;
    plat_log("hidden bake: level %d, %d sample positions (%d/%d units, occluders shrunk %.1f), %d threads, cube %d px a "
             "face", lv->level, sNumPoints, stepXZ, stepY, sEps, threads, sRes);
    for (i = 0; i < threads; i++) {
        pthread_create(&tid[i], NULL, worker, &work[i]);
    }
    for (i = 0; i < threads; i++) {
        pthread_join(tid[i], NULL);
    }
    plat_log("hidden bake: level %d, sampled in %ld s (%llu batch tests)", lv->level, (long) (time(NULL) - t0),
             sTested);
    for (c = 0; c < sNumConfigs; c++) {
        write_config(&sConfigs[c], outDir);
        for (i = 0; i < sConfigs[c].numCells; i++) {
            free(sConfigs[c].cells[i].mask);
            free(sConfigs[c].cells[i].seen);
        }
        free(sConfigs[c].cells);
    }
    for (i = 0; i < threads; i++) {
        free(work[i].depth);
        free(work[i].eroded);
        free(work[i].tab);
    }
    free(work);
    sLevelsLeft[lv->level] = 0;
    if (--sNumLevelsLeft == 0) {
        plat_log("hidden bake: every level done");
        port_hidden_exit_log();
        exit(0);
    }
    return 1;
}

/* ---- The host's table override (DKR_HIDDEN_TABLE_DIR) ---- */

static void exit_log(void) {
    port_hidden_exit_log();
}

const unsigned char *hidden_host_table(int level, int *size) {
    static int sAtExit;
    static unsigned char *sData;
    char path[512];
    const char *dir = getenv("DKR_HIDDEN_TABLE_DIR");
    FILE *f;
    long n;
    if (!sAtExit) {
        sAtExit = 1;
        atexit(exit_log);
    }
    *size = 0;
    if (dir == NULL) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/hidden_%d.bin", dir, level);
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(sData);
    sData = malloc(n > 0 ? n : 1);
    if (sData == NULL || fread(sData, 1, n, f) != (size_t) n) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (int) n;
    return sData;
}

/* ---- The cell size evaluation (DKR_HIDDEN_EVAL=<dir>,<dir>,...) ---- */

static char sEvalDirs[8][256];
static int sNumEval = -1;
static unsigned char *sEvalData[8];

int hidden_host_eval_count(void) {
    if (sNumEval < 0) {
        const char *e = getenv("DKR_HIDDEN_EVAL");
        sNumEval = 0;
        while (e != NULL && *e && sNumEval < 8) {
            const char *comma = strchr(e, ',');
            int n = comma != NULL ? (int) (comma - e) : (int) strlen(e);
            if (n > 0 && n < 256) {
                memcpy(sEvalDirs[sNumEval], e, n);
                sEvalDirs[sNumEval][n] = '\0';
                sNumEval++;
            }
            e = comma != NULL ? comma + 1 : NULL;
        }
    }
    return sNumEval;
}

const unsigned char *hidden_host_eval_table(int k, int level, int *size, char *name, int nameSize) {
    char path[512];
    const char *slash;
    FILE *f;
    long n;
    int dummy;
    hidden_host_table(level, &dummy); /* (registers the exit log) */
    *size = 0;
    if (k < 0 || k >= hidden_host_eval_count()) {
        return NULL;
    }
    slash = strrchr(sEvalDirs[k], '/');
    snprintf(name, nameSize, "%s", slash != NULL ? slash + 1 : sEvalDirs[k]);
    snprintf(path, sizeof(path), "%s/hidden_%d.bin", sEvalDirs[k], level);
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(sEvalData[k]);
    sEvalData[k] = malloc(n > 0 ? n : 1);
    if (sEvalData[k] == NULL || fread(sEvalData[k], 1, n, f) != (size_t) n) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (int) n;
    return sEvalData[k];
}
