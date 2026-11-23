#ifndef GFX_FRAME_PARTS_H
#define GFX_FRAME_PARTS_H

/*
 * P57 E (DKR_FRAME_PARTS_CHECK): the self-check of the backend's frame start/end parts (gfx_backend.h GFXB_FPART_*).
 * The parts are timed by marks inside gfxb_start_frame and gfxb_end_frame; gfx_split.c times the same two calls as one
 * bucket of its own ("frame start/end"). On the same frames their sums must agree: what lies outside the parts is
 * only the records' few lines around the two calls. A gap means a path the marks miss (an early return, a part left
 * out), and parts larger than the bucket mean one counted twice.
 * drop >= 0 leaves that part out of the sum (the check broken on purpose, rule K6). Returns TRUE if they agree;
 * *outside gets the bucket's ticks less the parts' (negative: the parts are larger).
 */
static inline int gfx_frame_parts_agree(unsigned long long bucket, const unsigned long long *parts, int count, int drop,
                                        long long *outside) {
    unsigned long long sum = 0;
    long long gap, size;
    int k;
    for (k = 0; k < count; k++) {
        if (k != drop) {
            sum += parts[k];
        }
    }
    gap = (long long) bucket - (long long) sum;
    *outside = gap;
    size = gap < 0 ? -gap : gap;
    return (unsigned long long) size <= bucket / 20; // Within 5% of the bucket (the records' own lines)
}

/* The largest part (where the check's break leaves one out, so the break always shows). */
static inline int gfx_frame_parts_largest(const unsigned long long *parts, int count) {
    int k, best = 0;
    for (k = 1; k < count; k++) {
        if (parts[k] > parts[best]) {
            best = k;
        }
    }
    return best;
}

#endif
