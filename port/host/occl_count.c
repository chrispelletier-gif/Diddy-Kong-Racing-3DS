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
 * 97% (the four Old 3DS bench tracks give 98-100%; the break below gives 93-96%). DKR_OCCL_BREAK=1 breaks the
 * count on purpose (rule K6: triangles cut by the near plane are left out) to show that the check notices.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat/plat.h"
#include "occl_raster.h"

#define OC_MAX_W 400
#define OC_H 240
#define OC_MAX_BATCHES 4096
#define OC_MAX_VERTS 256
#define OC_PAIRS (1 << 18) /* (camera segment, batch) pairs remembered; a power of two */

typedef struct {
    unsigned int key;
    const void *verts; /* Vertex */
    const void *tris;  /* Triangle */
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
static OcrCtx sCtx; /* occl_raster.c: sDepth at sW x OC_H, the renderer's culling */
static const void *sLevel;
static int sLevelId = -1; /* The course (settings courseId) */
static OcSums sWindow, sLevelSums;
static OcPair *sPairs;
static unsigned int sPairCount;
#define OC_RING 16
static unsigned char sRing[OC_RING][OC_MAX_W * OC_H]; /* DKR_OCCL_COUNT_CHECK: the last frames' opaque pass pixels */
static unsigned int sRingPixels[OC_RING];
static int sRingNext;
static unsigned char *sMask; /* The slot being written (frame_close) */
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

/* The frame's batches: the depth of the opaque ones, then each batch tested against it (occl_raster.c). */
static void frame_close(void) {
    int b;
    if (!sFrameOpen) {
        return;
    }
    sFrameOpen = 0;
    sCtx.w = sW;
    sCtx.h = OC_H;
    sCtx.depth = sDepth;
    sCtx.cull = sCull;
    sCtx.breakNear = sBreak;
    sMask = sRing[sRingNext];
    memset(sMask, 0, sizeof(sRing[0]));
    sCtx.mask = sMask;
    sCtx.pixels = &sWindow.opaquePixels;
    sCtx.testDepth = sDepth;
    memset(sDepth, 0, sizeof(float) * sW * OC_H);
    for (b = 0; b < sNumBatches; b++) {
        OcBatch *bt = &sBatches[b];
        if (!(bt->occluder & 2)) {
            continue; // Not in the opaque pass
        }
        // Opaque pass batches that hide nothing: only their pixels, for the check (mode 2)
        ocr_batch(&sCtx, bt->verts, bt->numVerts, bt->tris, bt->numTris, (bt->occluder & 1) ? 0 : 2);
    }
    sCtx.mask = NULL;
    sCtx.pixels = NULL;
    if (sCheck) {
        unsigned int unique = 0;
        int i;
        for (i = 0; i < sW * OC_H; i++) {
            unique += sMask[i];
        }
        sRingPixels[sRingNext] = unique; // Pixels, not fragments (opaquePixels counts overdraw too)
    }
    sRingNext = (sRingNext + 1) % OC_RING;
    sWindow.frames++;
    for (b = 0; b < sNumBatches; b++) {
        OcBatch *bt = &sBatches[b];
        int best = ocr_batch(&sCtx, bt->verts, bt->numVerts, bt->tris, bt->numTris, 1);
        OcPair *p;
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
    memcpy(sCtx.m, viewProj, sizeof(sCtx.m));
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
