/*
 * The GS hang ladder's step markers (roadmap P65 rank 4, DKR_GS_LADDER, gfx_c3d.c gs_ladder): what a step's file
 * (gs_ladder_N.txt in the data directory) says it should do at this start. Kept free of the 3DS headers so that a PC
 * test (port/tools/gs_ladder_test.c, run by CI) checks it.
 *
 * A step writes "testing V" before its first draw and "passed V" after its last frame; the GPU watchdog writes
 * "gpu-hung V" when the GPU stops while the console still runs. A start that finds "testing V" knows the whole console
 * locked during that step (nothing could write after it), and records "hung V". V is GS_LADDER_VERSION: a marker of
 * another version is run again.
 */
#ifndef GS_LADDER_H
#define GS_LADDER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GS_LADDER_VERSION 1

enum {
    GSL_RUN,         // Run the step
    GSL_PASSED,      // Ran 30 frames before: skip it
    GSL_HUNG,        // Locked the console before (or the GPU, gpu-hung): skip it
    GSL_LOCKED_NOW,  // The last start locked the console during it: write "hung V", then skip it
};

/* text: the step's file (NUL-terminated), or NULL if there is none. rerun: DKR_GS_LADDER=2, run the passed and hung
 * steps again (a "testing" marker is still recorded as a lock first). */
static inline int gs_ladder_marker_action(const char *text, int version, int rerun) {
    if (text == NULL) {
        return GSL_RUN;
    }
#ifndef GS_LADDER_BREAK
    if (strncmp(text, "testing ", 8) == 0 && atoi(text + 8) == version) {
        return GSL_LOCKED_NOW;
    }
#endif
    if (rerun) {
        return GSL_RUN;
    }
    if (strncmp(text, "passed ", 7) == 0 && atoi(text + 7) == version) {
        return GSL_PASSED;
    }
    if ((strncmp(text, "hung ", 5) == 0 && atoi(text + 5) == version) ||
        (strncmp(text, "gpu-hung ", 9) == 0 && atoi(text + 9) == version)) {
        return GSL_HUNG;
    }
    return GSL_RUN;
}

/*
 * GS freeze test 2, step 10 (gfx_c3d.c gfxb_draw_sprites, DKR_SPRITE_GS_CAP): the marker file of a cap, in the data
 * directory. Each cap (each Old 3DS bench row) has its own, so a lock in one row doesn't leave the others off; 0
 * (every batch) keeps P60 D's sprite_gs.txt. -DGS_CAP_BREAK gives every cap the same file (K6).
 */
static inline void sprite_gs_marker_name(char *out, size_t size, int cap) {
#ifndef GS_CAP_BREAK
    if (cap > 0) {
        snprintf(out, size, "sprite_gs_cap%d.txt", cap);
        return;
    }
#endif
    snprintf(out, size, "sprite_gs.txt");
}

/* Whether the sprite geometry shader stays off for a cap: its marker (NULL: none) says the console locked in it
 * ("testing V", left by the run that locked) or that it hung before ("hung V", "gpu-hung V"). */
static inline int sprite_gs_marker_off(const char *text, int version) {
    int action = gs_ladder_marker_action(text, version, 0);
    return action == GSL_LOCKED_NOW || action == GSL_HUNG;
}

#endif
