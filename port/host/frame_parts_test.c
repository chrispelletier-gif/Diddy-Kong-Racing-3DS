/*
 * P57 E: host test of the frame start/end parts' self-check (port/src/gfx/gfx_frame_parts.h), run by CI. The parts
 * only exist on the 3DS backend, so the check's arithmetic is tested here on run 54's shape (3.9 ms in 15 parts):
 * parts that add up pass; the check broken on purpose (rule K6: the largest part left out) and a part counted twice
 * must both fail. Exit status 0 when all three come out as they should.
 */
#include <stdio.h>
#include "../src/gfx/gfx_frame_parts.h"

int main(void) {
    const unsigned long long ms = 268111ULL; // Old 3DS clock ticks a millisecond
    unsigned long long parts[15] = { 1, 2, 40, 1, 3, 20, 30, 1, 7, 1, 60, 20, 10, 180, 6 }, sum = 0, bucket;
    long long outside;
    int k, ok, broken, twice, bad = 0;
    for (k = 0; k < 15; k++) {
        parts[k] *= ms / 100;
        sum += parts[k];
    }
    bucket = sum + ms * 3 / 100; // 0.03 ms of the records' own lines outside the parts
    ok = gfx_frame_parts_agree(bucket, parts, 15, -1, &outside);
    printf("frame parts check: %.2f of %.2f ms outside the parts, %s\n", (double) outside / ms, (double) bucket / ms,
           ok ? "ok" : "WRONG");
    broken = gfx_frame_parts_agree(bucket, parts, 15, gfx_frame_parts_largest(parts, 15), &outside);
    printf("frame parts check (broken on purpose): %.2f ms outside the parts, %s\n", (double) outside / ms,
           broken ? "ok" : "WRONG");
    parts[13] *= 2;
    twice = gfx_frame_parts_agree(bucket, parts, 15, -1, &outside);
    printf("frame parts check (a part counted twice): %.2f ms outside the parts, %s\n", (double) outside / ms,
           twice ? "ok" : "WRONG");
    if (!ok) {
        printf("FAIL: parts that add up were reported wrong\n");
        bad = 1;
    }
    if (broken) {
        printf("FAIL: the check broken on purpose (largest part left out) reported nothing\n");
        bad = 1;
    }
    if (twice) {
        printf("FAIL: a part counted twice was not reported\n");
        bad = 1;
    }
    return bad;
}
