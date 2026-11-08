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
 */
static int sFragStats = -1, sFragKind;
static uint8_t sFragLast[MAX_W * H]; // The kind that last wrote each pixel this frame, +1 (0: the clear)
static uint8_t sFragSky[MAX_W * H];  // Sky fragments rasterized at each pixel this frame
static unsigned long long sOracleBand[4], sOracleRect; // Sky fragments a scissor could leave out (bands 1,4,16,64; one rect)
static unsigned long long sFragRaster[PROF_KINDS + 2], sFragPass[PROF_KINDS + 2], sFragHidden[PROF_KINDS + 2],
    sFragBlend[PROF_KINDS + 2], sFragNoDepth[PROF_KINDS + 2], sFragTris[PROF_KINDS + 2], sFragDraws[PROF_KINDS + 2];
static unsigned long long sFragClear, sFragFrames, sFragCovered;
static unsigned long long sDrawRaster; // This draw's (DKR_FRAG_STATS=2)
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
        static unsigned long long sPrevRaster[PROF_KINDS + 2], sPrevTris[PROF_KINDS + 2], sPrevBlend[PROF_KINDS + 2];
        static unsigned long long sFrameNo;
        if (sCsv == NULL) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frag_frames.csv", plat_data_dir());
            sCsv = fopen(path, "w");
            if (sCsv != NULL) {
                fprintf(sCsv, "frame,raster,blend,tris");
                for (k = 0; k < 9; k++) {
                    fprintf(sCsv, ",raster_%d,tris_%d", k, k);
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
                fprintf(sCsv, ",%.3f,%llu", (sFragRaster[k] - sPrevRaster[k]) / screen, sFragTris[k] - sPrevTris[k]);
            }
            fprintf(sCsv, "\n");
        }
        sFrameNo++;
        memcpy(sPrevRaster, sFragRaster, sizeof(sPrevRaster));
        memcpy(sPrevTris, sFragTris, sizeof(sPrevTris));
        memcpy(sPrevBlend, sFragBlend, sizeof(sPrevBlend));
        if ((sFragFrames + 1) % 300 == 0) {
            memset(sPrevRaster, 0, sizeof(sPrevRaster)); // The sums restart below
            memset(sPrevTris, 0, sizeof(sPrevTris));
            memset(sPrevBlend, 0, sizeof(sPrevBlend));
        }
    }
    memset(sFragLast, 0, sizeof(sFragLast));
    memset(sFragSky, 0, sizeof(sFragSky));
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
