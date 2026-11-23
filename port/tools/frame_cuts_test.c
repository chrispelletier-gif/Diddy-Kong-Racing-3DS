/*
 * P57 E step 2 host test (CI step "Frame end cuts check", rule K6): the logic of DKR_FE_FLUSH's merged cache flush and
 * of its self-check DKR_FE_FLUSH_CHECK, and of DKR_FE_QUEUE_CHECK (port/src/gfx/gfx_frame_cuts.h).
 *  - parts inside the slot's command buffer merge into one range, which covers them all (check: 0 missing);
 *  - a part outside the buffer (a stereo eye list) gets its own range, still covered;
 *  - the check broken on purpose (a flushed range left out) must report the lists that range covered;
 *  - a merged range one word short must be caught;
 *  - the queue check: a finished queue passes, an unfinished one fails, and the broken check fails a finished one.
 * Exit 1 on any failure.
 */
#include <stdio.h>
#include "../src/gfx/gfx_frame_cuts.h"

static int sFailed;

static void expect(int ok, const char *what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        sFailed = 1;
    }
}

int main(void) {
    const unsigned long blo = 0x10000, bhi = 0x10000 + 0x40000;
    GfxFcRange parts[5] = { { 0x10000, 0x10400 }, { 0x10400, 0x12000 }, { 0x12000, 0x12010 }, { 0x12010, 0x12010 },
                            { 0x12010, 0x13800 } };
    GfxFcRange out[8], eye[3] = { { 0x10000, 0x10400 }, { 0x90000, 0x90100 }, { 0x10400, 0x10800 } };
    int n, missing;

    n = gfx_fc_merge(parts, 5, blo, bhi, out, 8);
    expect(n == 1 && out[0].lo == 0x10000 && out[0].hi == 0x13800, "parts in the buffer merge into one range");
    missing = gfx_fc_uncovered(out, n, parts, 5, -1);
    expect(missing == 0, "the merged range covers every part (check: 0 missing)");
    missing = gfx_fc_uncovered(out, n, parts, 5, 0);
    printf("check broken on purpose (merged range left out): %d of 5 lists not covered\n", missing);
    expect(missing == 4, "the check broken on purpose reports the 4 non-empty parts");

    n = gfx_fc_merge(eye, 3, blo, bhi, out, 8);
    expect(n == 2 && out[0].lo == 0x10000 && out[0].hi == 0x10800 && out[1].lo == 0x90000,
           "a list outside the buffer gets its own range");
    expect(gfx_fc_uncovered(out, n, eye, 3, -1) == 0, "both ranges cover all three lists");
    expect(gfx_fc_uncovered(out, n, eye, 3, 1) == 1, "leaving the outside list's range out is caught");

    n = gfx_fc_merge(parts, 5, blo, bhi, out, 8);
    out[0].hi -= 4; // One word short at the end
    expect(gfx_fc_uncovered(out, n, parts, 5, -1) == 1, "a merged range one word short is caught");

    n = gfx_fc_merge(eye, 3, blo, bhi, out, 1);
    expect(gfx_fc_uncovered(out, n, eye, 3, -1) == 1, "a list with no room for its range is caught");

    expect(!gfx_fc_clear_wrong(7, 7, 0), "queue check: a finished queue passes");
    expect(gfx_fc_clear_wrong(5, 7, 0), "queue check: a queue cleared before it finished fails");
    expect(!gfx_fc_clear_wrong(0, 0, 1), "queue check broken: an empty queue still passes");
    expect(gfx_fc_clear_wrong(7, 7, 1), "queue check broken on purpose: a finished queue is reported");
    return sFailed;
}
