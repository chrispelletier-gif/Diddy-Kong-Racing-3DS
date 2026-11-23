#ifndef GFX_FRAME_CUTS_H
#define GFX_FRAME_CUTS_H

/*
 * P57 E step 2 (proposals/3ds-old3ds-frame-start-end-plan.md): the logic of the 3DS backend's frame end cuts that the
 * host test (port/tools/frame_cuts_test.c) can run without a console. port/3ds/gfx_c3d.c uses it.
 *
 * DKR_FE_FLUSH: the frame's command list parts (and the rest of the list, split off by the backend itself) are written
 * out of the CPU's data cache with one system call over the span they share in the slot's command buffer, instead of
 * one call for each part and a flush by the GSP service for the rest. gfx_fc_merge works out the ranges to flush;
 * DKR_FE_FLUSH_CHECK checks with gfx_fc_uncovered that every list handed to the GPU lies inside one of them.
 */
typedef struct GfxFcRange {
    unsigned long lo, hi; // Bytes [lo, hi)
} GfxFcRange;

/*
 * The ranges to flush for n lists: one range spanning every list inside the buffer [blo, bhi) (the slot's command
 * buffer), then one for each list outside it, as they come. Empty lists are skipped. Returns the number of ranges
 * written to out (at most max; a list that finds no room is left out, which the check then reports).
 */
static inline int gfx_fc_merge(const GfxFcRange *lists, int n, unsigned long blo, unsigned long bhi, GfxFcRange *out,
                               int max) {
    unsigned long lo = 0, hi = 0;
    int k, count = 0, inside = 0;
    for (k = 0; k < n; k++) {
        if (lists[k].hi <= lists[k].lo) {
            continue;
        }
        if (lists[k].lo >= blo && lists[k].hi <= bhi) {
            if (!inside || lists[k].lo < lo) {
                lo = lists[k].lo;
            }
            if (!inside || lists[k].hi > hi) {
                hi = lists[k].hi;
            }
            inside = 1;
        }
    }
    if (inside && count < max) {
        out[count].lo = lo;
        out[count].hi = hi;
        count++;
    }
    for (k = 0; k < n; k++) {
        if (lists[k].hi > lists[k].lo && !(lists[k].lo >= blo && lists[k].hi <= bhi) && count < max) {
            out[count++] = lists[k];
        }
    }
    return count;
}

/*
 * How many of the n lists are not wholly inside one of the flushed ranges. drop >= 0 leaves that flushed range out
 * (the check broken on purpose, rule K6: the GPU would read stale words from the lists it held).
 */
static inline int gfx_fc_uncovered(const GfxFcRange *flushed, int nFlushed, const GfxFcRange *lists, int n, int drop) {
    int k, f, missing = 0;
    for (k = 0; k < n; k++) {
        int covered = lists[k].hi <= lists[k].lo;
        for (f = 0; f < nFlushed && !covered; f++) {
            if (f != drop && lists[k].lo >= flushed[f].lo && lists[k].hi <= flushed[f].hi) {
                covered = 1;
            }
        }
        missing += !covered;
    }
    return missing;
}

/*
 * DKR_FE_QUEUE_HELPER: the GPU queue's start, handed to a thread on the game core. The queue may only be waited on,
 * stopped or cleared once it has been started; DKR_FE_QUEUE_CHECK checks at each clear that every job the queue held
 * was finished (lastEntry == numEntries). broken (rule K6) counts one job more than it held, so every clear of a queue
 * that held jobs counts as wrong. Returns 1 when the clear is wrong.
 */
static inline int gfx_fc_clear_wrong(unsigned lastEntry, unsigned numEntries, int broken) {
    unsigned held = numEntries + (broken && numEntries > 0 ? 1u : 0u);
    return lastEntry != held;
}

#endif
