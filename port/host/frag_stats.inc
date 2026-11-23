/*
 * DKR_FRAG_STATS=1 (host only, a measuring tool for the New 3DS GPU work, gpu_cuts.inc): the pixels each kind of draw
 * (port_drawtag.h) rasterizes, a frame on average, logged every 300 frames. On a New 3DS at 800 pixels with 2x
 * anti-aliasing the GPU is limited by the pixels it rasterizes (bench #451: about 1.5 ms a screen of them), whether or
 * not they pass the depth test, so these say where its time goes and what could be left out:
 *  - rasterized: inside a triangle and the scissor (what the GPU pays for)
 *  - passed: also through the depth and alpha tests
 *  - hidden: written, then written over by a later opaque pixel in the same frame (wasted)
 *  - blended, no depth test: of the rasterized
 * DKR_FRAG_STATS=2 also lists the sky's draws (each with its pixels and settings) once a second.
 * DKR_FRAG_STATS=3 also writes every frame's pixels and triangles by kind to frag_frames.csv (in the data directory),
 * to find the frames whose GPU work spikes (Old 3DS Whale Bay, quick test #475: 17-24 ms GPU frames against 10).
 * DKR_FRAG_STATS=4 (P62 M4, 2026-11-22) also lists the frame's first FRAG_FIRST_DRAWS draws once a second: each one's
 * pixels, kind, settings, colours, box and the code that drew it (the call stack, for addr2line), to say what the
 * New 3DS's costly frame-start flat draws are (bench #570, Whale Bay: "menus/other" #0-#2, 1.2 / 1.0 / 1.9 ms).
 */
#define FRAG_FIRST_DRAWS 4
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
static int sFragDrawNo; // This frame's draws so far (DKR_FRAG_STATS=4)
static int sFragStats = -1, sFragKind;
static uint8_t sFragLast[MAX_W * H]; // The kind that last wrote each pixel this frame, +1 (0: the clear)
static uint8_t sFragSky[MAX_W * H];  // Sky fragments rasterized at each pixel this frame
static unsigned long long sOracleBand[4], sOracleRect; // Sky fragments a scissor could leave out (bands 1,4,16,64; one rect)
static unsigned long long sFragRaster[PROF_KINDS + 2], sFragPass[PROF_KINDS + 2], sFragHidden[PROF_KINDS + 2],
    sFragBlend[PROF_KINDS + 2], sFragNoDepth[PROF_KINDS + 2], sFragTris[PROF_KINDS + 2], sFragDraws[PROF_KINDS + 2];
static unsigned long long sFragClear, sFragFrames, sFragCovered;
static unsigned long long sDrawRaster; // This draw's (DKR_FRAG_STATS=2)
#ifdef PORT_OCCL_COUNT // Host only (gfx_ref.c on the 3DS includes this file too)
static uint8_t sFragTrackMask[MAX_W * H]; // This frame's pixels the track kind rasterized (occl_count.c's check)
void port_occl_renderer_frame(const uint8_t *mask, int w, int h);
#endif
#define FRAG_CLEAR_KIND (PROF_KINDS + 1)
#define PORT_DRAW_SKY_KIND 4 // port_drawtag.h PORT_DRAW_SKY

static int frag_on(void) {
    if (sFragStats < 0) {
        const char *e = getenv("DKR_FRAG_STATS");
        sFragStats = e != NULL ? atoi(e) : 0;
    }
    return sFragStats;
}

static void frag_frame_end(void) {
    static const char *const sNames[] = { "other", "track", "racers", "objects", "sky", "see-through", "effects",
                                          "shadows", "HUD" };
    int k, i;
    if (!frag_on()) {
        return;
    }
    for (i = 0; i < W * H; i++) {
        sFragCovered += sFragLast[i] != 0;
    }
    {
        // Oracle: per band of columns, the sky rasterized below the lowest pixel where the sky is seen (y down)
        // and, for one rectangle, outside the box of all the pixels where it is seen.
        static const int sBands[4] = { 1, 4, 16, 64 };
        int b, x, y, minX = W, maxX = -1, maxYAll = -1;
        for (b = 0; b < 4; b++) {
            int bands = sBands[b], band;
            for (band = 0; band < bands; band++) {
                int x0 = W * band / bands, x1 = W * (band + 1) / bands, lowest = -1;
                for (y = 0; y < H; y++) {
                    for (x = x0; x < x1; x++) {
                        if (sFragLast[y * W + x] == PORT_DRAW_SKY_KIND + 1) {
                            lowest = y;
                            if (b == 0) {
                                if (x < minX) minX = x;
                                if (x > maxX) maxX = x;
                                maxYAll = y;
                            }
                        }
                    }
                }
                for (y = lowest + 1; y < H; y++) {
                    for (x = x0; x < x1; x++) {
                        sOracleBand[b] += sFragSky[y * W + x];
                    }
                }
            }
        }
        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                if (y > maxYAll || x < minX || x > maxX) {
                    sOracleRect += sFragSky[y * W + x];
                }
            }
        }
    }
    if (sFragStats == 3) {
        // This frame's share of the running sums: rasterized screens and triangles by kind.
        static FILE *sCsv;
        static unsigned long long sPrevRaster[PROF_KINDS + 2], sPrevTris[PROF_KINDS + 2], sPrevBlend[PROF_KINDS + 2],
            sPrevDraws[PROF_KINDS + 2];
        static unsigned long long sFrameNo;
        if (sCsv == NULL) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frag_frames.csv", plat_data_dir());
            sCsv = fopen(path, "w");
            if (sCsv != NULL) {
                fprintf(sCsv, "frame,raster,blend,tris");
                for (k = 0; k < 9; k++) {
                    fprintf(sCsv, ",raster_%d,tris_%d,draws_%d", k, k, k);
                }
                fprintf(sCsv, "\n");
            }
        }
        if (sCsv != NULL) {
            double screen = (double) W * H, raster = 0, blend = 0, tris = 0;
            for (k = 0; k < 9; k++) {
                raster += (sFragRaster[k] - sPrevRaster[k]) / screen;
                blend += (sFragBlend[k] - sPrevBlend[k]) / screen;
                tris += (double) (sFragTris[k] - sPrevTris[k]);
            }
            fprintf(sCsv, "%llu,%.3f,%.3f,%.0f", sFrameNo, raster, blend, tris);
            for (k = 0; k < 9; k++) {
                fprintf(sCsv, ",%.3f,%llu,%llu", (sFragRaster[k] - sPrevRaster[k]) / screen, sFragTris[k] - sPrevTris[k],
                        sFragDraws[k] - sPrevDraws[k]);
            }
            fprintf(sCsv, "\n");
        }
        sFrameNo++;
        memcpy(sPrevRaster, sFragRaster, sizeof(sPrevRaster));
        memcpy(sPrevTris, sFragTris, sizeof(sPrevTris));
        memcpy(sPrevBlend, sFragBlend, sizeof(sPrevBlend));
        memcpy(sPrevDraws, sFragDraws, sizeof(sPrevDraws));
        if ((sFragFrames + 1) % 300 == 0) {
            memset(sPrevRaster, 0, sizeof(sPrevRaster)); // The sums restart below
            memset(sPrevTris, 0, sizeof(sPrevTris));
            memset(sPrevBlend, 0, sizeof(sPrevBlend));
            memset(sPrevDraws, 0, sizeof(sPrevDraws));
        }
    }
#ifdef PORT_OCCL_COUNT
    port_occl_renderer_frame(sFragTrackMask, W, H);
    memset(sFragTrackMask, 0, sizeof(sFragTrackMask));
#endif
    memset(sFragLast, 0, sizeof(sFragLast));
    memset(sFragSky, 0, sizeof(sFragSky));
    sFragDrawNo = 0;
    if (++sFragFrames % 300 != 0) {
        return;
    }
    {
        double f = 1.0 / sFragFrames, screen = (double) W * H;
        plat_log("frag stats (%dx%d, %llu frames; screens a frame: rasterized/passed/hidden later/blended/no depth "
                 "test, triangles, draws): clear %.2f, covered by draws %.2f",
                 W, H, sFragFrames, sFragClear * f / screen, sFragCovered * f / screen);
        for (k = 0; k < PROF_KINDS; k++) {
            if (sFragRaster[k] == 0 && sFragTris[k] == 0) {
                continue;
            }
            plat_log("  %-12s %.3f / %.3f / %.3f / %.3f / %.3f, %.0f tris, %.1f draws", k < 9 ? sNames[k] : "?",
                     sFragRaster[k] * f / screen, sFragPass[k] * f / screen, sFragHidden[k] * f / screen,
                     sFragBlend[k] * f / screen, sFragNoDepth[k] * f / screen, sFragTris[k] * f, sFragDraws[k] * f);
        }
        plat_log("  clear hidden by draws %.3f; sky a scissor could leave out: below the lowest seen sky in 1/4/16/64 "
                 "bands %.3f/%.3f/%.3f/%.3f, outside the seen sky's box %.3f screens", sFragHidden[FRAG_CLEAR_KIND] * f / screen,
                 sOracleBand[0] * f / screen, sOracleBand[1] * f / screen, sOracleBand[2] * f / screen,
                 sOracleBand[3] * f / screen, sOracleRect * f / screen);
        memset(sOracleBand, 0, sizeof(sOracleBand));
        sOracleRect = 0;
    }
    memset(sFragRaster, 0, sizeof(sFragRaster));
    memset(sFragPass, 0, sizeof(sFragPass));
    memset(sFragHidden, 0, sizeof(sFragHidden));
    memset(sFragBlend, 0, sizeof(sFragBlend));
    memset(sFragNoDepth, 0, sizeof(sFragNoDepth));
    memset(sFragTris, 0, sizeof(sFragTris));
    memset(sFragDraws, 0, sizeof(sFragDraws));
    sFragClear = sFragFrames = sFragCovered = 0;
}

/* DKR_FRAG_STATS=4: after one of the frame's first FRAG_FIRST_DRAWS draws, once a second, what it was and who drew it. */
static void frag_first_draw(const GfxDrawState *st, const GfxVertex *v, int numVerts, int numTriangles) {
    int no = sFragDrawNo++;
    if (sFragStats != 4 || no >= FRAG_FIRST_DRAWS || sFramesRendered % 60 != 0) {
        return;
    }
    {
        static const char *const sNames[] = { "other", "track", "racers", "objects", "sky", "see-through", "effects",
                                              "shadows", "HUD" };
        const GfxTexture *t = st->textures[0];
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        int i;
        void *stack[12];
        char **names;
        int depth;
        for (i = 0; i < numVerts && i < 64; i++) {
            float w = v[i].w != 0.0f ? v[i].w : 1.0f, x = v[i].x / w, y = v[i].y / w;
            x0 = x < x0 ? x : x0;
            x1 = x > x1 ? x : x1;
            y0 = y < y0 ? y : y0;
            y1 = y > y1 ? y : y1;
        }
        plat_log("frag first draw #%d (frame %d): %s, %d tris, %.3f screens rasterized, tex %dx%d fmt %d, blend %d depth "
                 "%d/%d alpha test %d fog %d cycles %d flat %d, box x %.2f..%.2f y %.2f..%.2f, scissor %d,%d-%d,%d, "
                 "vertex 0 rgba %d,%d,%d,%d prim %d,%d,%d,%d env %d,%d,%d,%d, colour (%d-%d)*%d+%d alpha (%d-%d)*%d+%d",
                 no, sFramesRendered, sFragKind < 9 ? sNames[sFragKind] : "?", numTriangles,
                 (double) sDrawRaster / (W * H), t ? (int) t->width : 0, t ? (int) t->height : 0,
                 t ? (int) t->format : -1, st->blend, st->depthTest, st->depthWrite, st->alphaTest, st->fogEnabled,
                 st->combiner.numCycles, st->flat, x0, x1, y0, y1, st->scissor[0], st->scissor[1], st->scissor[2],
                 st->scissor[3], v[0].r, v[0].g, v[0].b, v[0].a, st->prim[0], st->prim[1], st->prim[2], st->prim[3],
                 st->env[0], st->env[1], st->env[2], st->env[3], st->combiner.rgb[0].a, st->combiner.rgb[0].b,
                 st->combiner.rgb[0].c, st->combiner.rgb[0].d, st->combiner.alpha[0].a, st->combiner.alpha[0].b,
                 st->combiner.alpha[0].c, st->combiner.alpha[0].d);
#if defined(__GLIBC__)
        depth = backtrace(stack, 12);
        names = backtrace_symbols(stack, depth);
        if (names != NULL) {
            for (i = 1; i < depth; i++) {
                plat_log("    at %s", names[i]);
            }
            free(names);
        }
#else
        (void) stack;
        (void) names;
        (void) depth;
#endif
    }
}
