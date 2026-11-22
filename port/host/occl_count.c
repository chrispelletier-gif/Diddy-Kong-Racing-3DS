/*
 * DKR_OCCL_COUNT=1 (host only, P64 candidate 2's first experiment, proposals/3ds-breakthrough-candidates.md): how many
 * of the track batches the game draws are hidden, so that a table baked on the PC could leave them out.
 *
 * The game picks the track segments to draw from its own visibility table and then draws every batch in them
 * (tracks.c render_level_segment). Each frame this counts, for each batch drawn, whether it shows any pixel:
 *  - no pixel: none of its triangles covers a pixel of the screen (off the screen, or only back faces),
 *  - hidden: it covers pixels, but the track's opaque batches are in front of all of them,
 *  - seen: at least one pixel is in front of (or level with) the track's nearest opaque surface there.
 * Only the track itself hides anything here (objects and karts move, so a bake could never rely on them), and a
 * batch with alpha cut-outs, decals or blending hides nothing. The test is a small software rasteriser of its own at
 * the 3DS's 400x240 (320x240 without DKR_WIDESCREEN): the depth of every opaque track batch first, then each batch's
 * pixels against it, with 0.1% of depth to spare (so a pixel level with the surface counts as seen).
 *
 * A baked table would be per cell of camera positions, not per frame. So it also keeps, per camera segment (the
 * game's own cell, gSceneActiveCamera->cameraSegmentID), which batches were ever seen from it during the run: the
 * triangles of batches never seen from the frame's camera segment are what a table with one cell per segment could
 * have left out (an upper bound for that cell size, since the run samples only the camera positions it raced).
 *
 * Logged every 300 frames and when a level ends ("occl count" lines). DKR_OCCL_COUNT_CHECK=1 (with DKR_FRAG_STATS=1)
 * proves the rasteriser draws the track where the host renderer does (the same matrix, culling and clipping): each
 * frame the renderer draws, the pixels its track kind covered are compared with those the opaque pass's batches
 * covered here, in the best of this file's last 16 frames (the renderer may draw a frame twice or skip one), as pixels
 * covered by both over pixels covered by either. A few frames always differ by a whole batch (the renderer holds some
 * cut-outs back and draws them under a later kind), so "occl check" lines give the median frame's match: FAILED under
 * 97% (the four Old 3DS bench tracks give 98-100%; the break below gives 93-96%). DKR_OCCL_BREAK=1 breaks the count on purpose (rule K6: triangles cut by the near plane are left out) to show
 * that the check notices.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat/plat.h"

#define OC_MAX_W 400
#define OC_H 240
#define OC_MAX_BATCHES 4096
#define OC_MAX_VERTS 256
#define OC_PAIRS (1 << 18) /* (camera segment, batch) pairs remembered; a power of two */
#define OC_DEPTH_SPARE 0.999f

typedef struct {
    short x, y, z;
    unsigned char r, g, b, a;
} OcVertex; /* structs.h Vertex */

typedef struct {
    unsigned char flags, vi0, vi1, vi2;
    unsigned int uv[3];
} OcTriangle; /* structs.h Triangle */

typedef struct {
    unsigned int key;
    const OcVertex *verts;
    const OcTriangle *tris;
    int numVerts, numTris;
    int occluder; /* Bit 0: hides what is behind it; bit 1: drawn in the opaque pass (the renderer's track kind) */
} OcBatch;

typedef struct {
    unsigned int key; /* (camera segment + 1) << 20 | batch key; 0: free */
    unsigned int frames;
    unsigned int tris;
    unsigned char seen;
} OcPair;

typedef struct {
    unsigned long long frames, batches, tris, offBatches, offTris, hidBatches, hidTris, seenBatches, seenTris;
    unsigned long long opaquePixels;
} OcSums;

static int sOn = -1, sCheck, sBreak, sCull = -1;
static int sW = 320;
static float sDepth[OC_MAX_W * OC_H]; /* 1/w of the nearest opaque track pixel; 0: none */
static OcBatch sBatches[OC_MAX_BATCHES];
static int sNumBatches, sFrameOpen, sCamSeg;
static float sMtx[4][4];
static const void *sLevel;
static int sLevelId = -1; /* The course (settings courseId) */
static OcSums sWindow, sLevelSums;
static OcPair *sPairs;
static unsigned int sPairCount;
#define OC_RING 16
static unsigned char sRing[OC_RING][OC_MAX_W * OC_H]; /* DKR_OCCL_COUNT_CHECK: the last frames' opaque pass pixels */
static unsigned int sRingPixels[OC_RING];
static int sRingNext;
static unsigned char *sMask; /* The slot being written (raster) */
static unsigned long long sCheckFrames, sCheckHist[101]; /* Frames compared, by their best match in % */

/* frag_stats.inc, at the end of each frame the host renderer draws: its track kind's pixels against the best of ours. */
void port_occl_renderer_frame(const unsigned char *mask, int w, int h) {
    unsigned int theirs = 0, i;
    int k;
    double best = 0.0;
    if (sOn <= 0 || !sCheck || w != sW || h != OC_H) {
        return;
    }
    for (i = 0; i < (unsigned int) (w * h); i++) {
        theirs += mask[i] != 0;
    }
    if (theirs < 1000) {
        return; // Not a race frame (menus), or too little track to tell
    }
    for (k = 0; k < OC_RING; k++) {
        unsigned int both = 0, either = 0;
        const unsigned char *m = sRing[k];
        if (sRingPixels[k] * 10u < theirs * 7u || sRingPixels[k] * 7u > theirs * 10u) {
            continue;
        }
        for (i = 0; i < (unsigned int) (w * h); i++) {
            both += m[i] & (mask[i] != 0);
            either += m[i] | (mask[i] != 0);
        }
        if ((double) both / either > best) {
            best = (double) both / either;
        }
    }
    sCheckFrames++;
    sCheckHist[(int) (best * 100.0 + 0.5)]++;
}

typedef struct {
    float x, y, z, w;
} OcClip;

static int on(void) {
    if (sOn < 0) {
        const char *e = getenv("DKR_OCCL_COUNT");
        sOn = e != NULL && atoi(e) != 0;
        e = getenv("DKR_OCCL_COUNT_CHECK");
        sCheck = e != NULL ? atoi(e) : 0;
        e = getenv("DKR_OCCL_BREAK");
        sBreak = e != NULL && atoi(e) != 0;
        e = getenv("DKR_OCCL_CULL"); /* The sign of the faces kept (-1, matching the renderer; 1; 0 keeps both) */
        if (e != NULL) {
            sCull = atoi(e);
        }
        e = getenv("DKR_WIDESCREEN");
        sW = (e != NULL && atoi(e) != 0) ? 400 : 320;
        if (sOn) {
            sPairs = calloc(OC_PAIRS, sizeof(OcPair));
        }
    }
    return sOn;
}

static OcPair *pair_get(unsigned int key) {
    unsigned int h = (key * 2654435761u) & (OC_PAIRS - 1);
    while (sPairs[h].key != 0 && sPairs[h].key != key) {
        h = (h + 1) & (OC_PAIRS - 1);
    }
    if (sPairs[h].key == 0) {
        if (sPairCount >= OC_PAIRS * 3 / 4) {
            return NULL;
        }
        sPairs[h].key = key;
        sPairCount++;
    }
    return &sPairs[h];
}

static void to_clip(const OcVertex *v, OcClip *c) {
    float x = v->x, y = v->y, z = v->z;
    c->x = x * sMtx[0][0] + y * sMtx[1][0] + z * sMtx[2][0] + sMtx[3][0];
    c->y = x * sMtx[0][1] + y * sMtx[1][1] + z * sMtx[2][1] + sMtx[3][1];
    c->z = x * sMtx[0][2] + y * sMtx[1][2] + z * sMtx[2][2] + sMtx[3][2];
    c->w = x * sMtx[0][3] + y * sMtx[1][3] + z * sMtx[2][3] + sMtx[3][3];
}

/* Rasterizes one triangle (screen x, y and 1/w at its corners). mode 0 writes the depth (nearest 1/w), mode 2 writes
 * nothing; with pixels, both count its pixels and mark them in sMask. mode 1 returns 2 at its first pixel in front of
 * the depth, else 1 if it covered any pixel, else 0. */
static int raster(const float *sx, const float *sy, const float *iw, int mode, unsigned long long *pixels) {
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    float minXf, maxXf, minYf, maxYf, inv;
    int minX, maxX, minY, maxY, x, y, covered = 0;
    if (area == 0.0f) {
        return 0;
    }
    minXf = sx[0] < sx[1] ? sx[0] : sx[1]; minXf = minXf < sx[2] ? minXf : sx[2];
    maxXf = sx[0] > sx[1] ? sx[0] : sx[1]; maxXf = maxXf > sx[2] ? maxXf : sx[2];
    minYf = sy[0] < sy[1] ? sy[0] : sy[1]; minYf = minYf < sy[2] ? minYf : sy[2];
    maxYf = sy[0] > sy[1] ? sy[0] : sy[1]; maxYf = maxYf > sy[2] ? maxYf : sy[2];
    if (maxXf < 0.0f || maxYf < 0.0f || minXf > (float) sW || minYf > (float) OC_H) {
        return 0;
    }
    minX = minXf < 0.0f ? 0 : (int) minXf;
    maxX = maxXf >= (float) sW ? sW - 1 : (int) maxXf;
    minY = minYf < 0.0f ? 0 : (int) minYf;
    maxY = maxYf >= (float) OC_H ? OC_H - 1 : (int) maxYf;
    inv = 1.0f / area;
    for (y = minY; y <= maxY; y++) {
        float py = y + 0.5f;
        for (x = minX; x <= maxX; x++) {
            float px = x + 0.5f;
            float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) * inv;
            float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) * inv;
            float w2 = 1.0f - w0 - w1, d;
            float *dst;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                continue;
            }
            d = w0 * iw[0] + w1 * iw[1] + w2 * iw[2];
            dst = &sDepth[y * sW + x];
            if (pixels != NULL) {
                (*pixels)++;
                sMask[y * sW + x] = 1;
            }
            if (mode == 0) {
                if (d > *dst) {
                    *dst = d;
                }
            } else if (mode == 2) {
                continue;
            } else {
                covered = 1;
                if (d >= *dst * OC_DEPTH_SPARE) {
                    return 2;
                }
            }
        }
    }
    return covered;
}

/* One of the game's triangles: kept if it faces the camera (or is double sided), cut at the near plane (z = -w), then
 * rasterized as a fan. Returns as raster does (the highest of its pieces). */
static int triangle(const OcClip *c, const OcTriangle *t, int numVerts, int mode, unsigned long long *pixels) {
    OcClip in[3], out[4];
    int n = 0, i, k, best = 0;
    float sx[4], sy[4], iw[4], area = 0.0f;
    if (t->vi0 >= numVerts || t->vi1 >= numVerts || t->vi2 >= numVerts) {
        return 0;
    }
    in[0] = c[t->vi0];
    in[1] = c[t->vi1];
    in[2] = c[t->vi2];
    for (i = 0; i < 3; i++) {
        const OcClip *a = &in[i], *b = &in[(i + 1) % 3];
        float da = a->z + a->w, db = b->z + b->w;
        if (da >= 0.0f) {
            out[n++] = *a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float s = da / (da - db);
            out[n].x = a->x + (b->x - a->x) * s;
            out[n].y = a->y + (b->y - a->y) * s;
            out[n].z = a->z + (b->z - a->z) * s;
            out[n].w = a->w + (b->w - a->w) * s;
            n++;
        }
    }
    if (n < 3 || (sBreak && n != 3)) {
        return 0; // (DKR_OCCL_BREAK: triangles cut by the near plane are left out instead)
    }
    for (i = 0; i < n; i++) {
        float w = out[i].w > 1e-6f ? out[i].w : 1e-6f;
        sx[i] = (out[i].x / w * 0.5f + 0.5f) * sW;
        sy[i] = (0.5f - out[i].y / w * 0.5f) * OC_H;
        iw[i] = 1.0f / w;
    }
    for (i = 0; i < n; i++) {
        k = (i + 1) % n;
        area += sx[i] * sy[k] - sx[k] * sy[i];
    }
    if (!(t->flags & 0x40) && sCull != 0 && (area * (float) sCull) < 0.0f) {
        return 0; // A back face
    }
    for (i = 1; i + 1 < n; i++) {
        float fx[3] = { sx[0], sx[i], sx[i + 1] }, fy[3] = { sy[0], sy[i], sy[i + 1] },
              fw[3] = { iw[0], iw[i], iw[i + 1] };
        int r = raster(fx, fy, fw, mode, pixels);
        if (r > best) {
            best = r;
            if (best == 2 && mode == 1) {
                break;
            }
        }
    }
    return best;
}

static void log_sums(const char *what, const OcSums *s) {
    double f = s->frames > 0 ? 1.0 / s->frames : 0.0, t = s->tris > 0 ? 100.0 / s->tris : 0.0;
    plat_log("occl count %s (%llu frames, %dx%d): a frame walks %.1f batches / %.0f tris; no pixel %.1f / %.0f "
             "(%.1f%% of tris), hidden behind track %.1f / %.0f (%.1f%%), seen %.1f / %.0f (%.1f%%)",
             what, s->frames, sW, OC_H, s->batches * f, s->tris * f, s->offBatches * f, s->offTris * f, s->offTris * t,
             s->hidBatches * f, s->hidTris * f, s->hidTris * t, s->seenBatches * f, s->seenTris * f, s->seenTris * t);
}

static void log_pairs(const char *what) {
    unsigned long long walked = 0, never = 0;
    unsigned int i, cells = 0;
    for (i = 0; i < OC_PAIRS; i++) {
        if (sPairs[i].key == 0) {
            continue;
        }
        walked += (unsigned long long) sPairs[i].frames * sPairs[i].tris;
        if (!sPairs[i].seen) {
            never += (unsigned long long) sPairs[i].frames * sPairs[i].tris;
        }
    }
    {
        // Cells: camera segments with any pair
        static unsigned char sCells[4096];
        memset(sCells, 0, sizeof(sCells));
        for (i = 0; i < OC_PAIRS; i++) {
            if (sPairs[i].key != 0) {
                unsigned int c = sPairs[i].key >> 20;
                if (c < sizeof(sCells) && !sCells[c]) {
                    sCells[c] = 1;
                    cells++;
                }
            }
        }
    }
    plat_log("occl count %s: per camera segment (%u cells, %u batch pairs): tris never seen from the frame's camera "
             "segment over the run %.1f%% of walked tris%s",
             what, cells, sPairCount, walked > 0 ? 100.0 * never / walked : 0.0,
             sPairCount >= OC_PAIRS * 3 / 4 ? " (pair table full: undercounted)" : "");
}

static void log_check(const char *what) {
    unsigned long long below = 0;
    int median = 0, k;
    if (!sCheck) {
        return;
    }
    for (k = 0; k <= 100; k++) {
        below += sCheckHist[k];
        if (below * 2 >= sCheckFrames) {
            median = k;
            break;
        }
    }
    plat_log("occl check %s: %llu renderer frames, median match %d%% (pixels both / pixels either), %.1f%% of frames "
             "at 99%% or more%s",
             what, sCheckFrames, median, sCheckFrames > 0 ? 100.0 * (sCheckHist[99] + sCheckHist[100]) / sCheckFrames : 0.0,
             sCheckFrames == 0 ? " (nothing compared: run with DKR_FRAG_STATS=1 and the same DKR_WIDESCREEN)"
             : median < 97 ? " FAILED" : " ok");
    sCheckFrames = 0;
    memset(sCheckHist, 0, sizeof(sCheckHist));
}

static void window_to_level(void) {
    sLevelSums.frames += sWindow.frames;
    sLevelSums.batches += sWindow.batches;
    sLevelSums.tris += sWindow.tris;
    sLevelSums.offBatches += sWindow.offBatches;
    sLevelSums.offTris += sWindow.offTris;
    sLevelSums.hidBatches += sWindow.hidBatches;
    sLevelSums.hidTris += sWindow.hidTris;
    sLevelSums.seenBatches += sWindow.seenBatches;
    sLevelSums.seenTris += sWindow.seenTris;
    sLevelSums.opaquePixels += sWindow.opaquePixels;
    memset(&sWindow, 0, sizeof(sWindow));
}

/* The frame's batches: the depth of the opaque ones, then each batch tested against it. */
static void frame_close(void) {
    static OcClip sClip[OC_MAX_VERTS];
    int b, i;
    if (!sFrameOpen) {
        return;
    }
    sFrameOpen = 0;
    memset(sDepth, 0, sizeof(float) * sW * OC_H);
    sMask = sRing[sRingNext];
    memset(sMask, 0, sizeof(sRing[0]));
    for (b = 0; b < sNumBatches; b++) {
        OcBatch *bt = &sBatches[b];
        if (!(bt->occluder & 2)) {
            continue; // Not in the opaque pass
        }
        for (i = 0; i < bt->numVerts; i++) {
            to_clip(&bt->verts[i], &sClip[i]);
        }
        for (i = 0; i < bt->numTris; i++) {
            // Opaque pass batches that hide nothing: only their pixels, for the check (mode 2)
            triangle(sClip, &bt->tris[i], bt->numVerts, (bt->occluder & 1) ? 0 : 2, &sWindow.opaquePixels);
        }
    }
    if (sCheck) {
        unsigned int unique = 0;
        for (i = 0; i < sW * OC_H; i++) {
            unique += sMask[i];
        }
        sRingPixels[sRingNext] = unique; // Pixels, not fragments (opaquePixels counts overdraw too)
    }
    sRingNext = (sRingNext + 1) % OC_RING;
    sWindow.frames++;
    for (b = 0; b < sNumBatches; b++) {
        OcBatch *bt = &sBatches[b];
        int best = 0;
        OcPair *p;
        for (i = 0; i < bt->numVerts; i++) {
            to_clip(&bt->verts[i], &sClip[i]);
        }
        for (i = 0; i < bt->numTris && best < 2; i++) {
            int r = triangle(sClip, &bt->tris[i], bt->numVerts, 1, NULL);
            best = r > best ? r : best;
        }
        sWindow.batches++;
        sWindow.tris += bt->numTris;
        if (best == 0) {
            sWindow.offBatches++;
            sWindow.offTris += bt->numTris;
        } else if (best == 1) {
            sWindow.hidBatches++;
            sWindow.hidTris += bt->numTris;
        } else {
            sWindow.seenBatches++;
            sWindow.seenTris += bt->numTris;
        }
        p = pair_get(((unsigned int) (sCamSeg + 1) << 20) | bt->key);
        if (p != NULL) {
            p->frames++;
            p->tris = bt->numTris;
            p->seen |= best == 2;
        }
    }
    sNumBatches = 0;
    if (sWindow.frames % 300 == 0) {
        log_sums("window", &sWindow);
        log_check("window");
        window_to_level();
    }
}

static void level_end(void) {
    frame_close();
    window_to_level(); // The last part window too
    if (sLevelSums.frames > 0) {
        char what[32];
        snprintf(what, sizeof(what), "level %d total", sLevelId);
        log_sums(what, &sLevelSums);
        log_pairs(what);
        log_check(what);
    }
    memset(&sLevelSums, 0, sizeof(sLevelSums));
    memset(sPairs, 0, sizeof(OcPair) * OC_PAIRS);
    sPairCount = 0;
}

static void at_exit(void) {
    level_end();
}

/* tracks.c render_level_geometry_and_objects, before the segments are drawn: a new frame (and maybe a new level, told
 * apart by its model and its course number, since a later level's model can land at the same address). */
void port_occl_frame(const float *viewProj, int cameraSegment, const void *level, int levelId) {
    static int sAtExit;
    if (!on()) {
        return;
    }
    if (!sAtExit) {
        atexit(at_exit);
        sAtExit = 1;
    }
    frame_close();
    if (level != sLevel || levelId != sLevelId) {
        level_end();
        sLevel = level;
        sLevelId = levelId;
    }
    memcpy(sMtx, viewProj, sizeof(sMtx));
    sCamSeg = cameraSegment < 0 ? -1 : cameraSegment;
    sNumBatches = 0;
    sFrameOpen = 1;
}

/* tracks.c render_level_segment: a batch the game draws this frame (occluder: OcBatch.occluder). */
void port_occl_batch(int segment, int batch, const void *verts, int numVerts, const void *tris, int numTris,
                     int occluder) {
    OcBatch *bt;
    if (!on() || !sFrameOpen || sNumBatches >= OC_MAX_BATCHES || numVerts <= 0 || numVerts > OC_MAX_VERTS) {
        return;
    }
    bt = &sBatches[sNumBatches++];
    bt->key = ((unsigned int) segment << 10) | ((unsigned int) batch & 0x3FF);
    bt->verts = verts;
    bt->tris = tris;
    bt->numVerts = numVerts;
    bt->numTris = numTris;
    bt->occluder = occluder;
}
