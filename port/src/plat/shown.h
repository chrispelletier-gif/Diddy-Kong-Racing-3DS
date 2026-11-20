#ifndef PORT_SHOWN_H
#define PORT_SHOWN_H
/*
 * P56 K7: which pictures reached the screen, from the swaps that hand them to it (3ds/gfx_c3d.c's swap wrapper). A
 * swap made while the picture before it is still waiting to be shown replaced that one (never shown); otherwise the
 * one before was shown, and the time from the last shown picture to it is a gap between two pictures on the screen
 * (taken at their swaps: the screen takes a picture at the retrace after its swap). The longest gap is kept for each
 * of the profiler's two windows, each starting afresh at the first picture shown after it was restarted. Header only,
 * no platform calls, so the logic can be checked on a PC.
 */
typedef struct PlatShown {
    unsigned long long pendingTick, lastShownTick;
    volatile unsigned int maxUs[2], replacedStart[2];
    volatile int armed[2];
} PlatShown;

/* The picture swapped at tick was shown: the gap since the last one counts in the windows already started. */
static inline void plat_shown_picture(PlatShown *s, unsigned long long tick, unsigned int ticksPerUs) {
    int w;
    if (s->lastShownTick != 0) {
        unsigned int gapUs = (unsigned int) ((tick - s->lastShownTick) / ticksPerUs);
        for (w = 0; w < 2; w++) {
            if (s->armed[w] && gapUs > s->maxUs[w]) {
                s->maxUs[w] = gapUs;
            }
        }
    }
    for (w = 0; w < 2; w++) {
        s->armed[w] = 1;
    }
    s->lastShownTick = tick;
}

/* A swap at tick; pending: the picture before it had not reached the screen yet (it is replaced). */
static inline void plat_shown_swap(PlatShown *s, int pending, unsigned long long tick, unsigned int ticksPerUs) {
    if (!pending && s->pendingTick != 0) {
        plat_shown_picture(s, s->pendingTick, ticksPerUs);
    }
    s->pendingTick = tick;
}

/* A window's numbers: replaced since its restart (from the running count) and its longest gap; restart first if asked. */
static inline void plat_shown_read(PlatShown *s, int window, int restart, unsigned int replacedNow,
                                   unsigned int *replaced, unsigned int *longestGapUs) {
    window = window != 0;
    if (restart) {
        s->armed[window] = 0;
        s->maxUs[window] = 0;
        s->replacedStart[window] = replacedNow;
    }
    *replaced = replacedNow - s->replacedStart[window];
    *longestGapUs = s->maxUs[window];
}
#endif
