/*
 * P65 rank 4: a check of the GS hang ladder's step markers (port/3ds/gs_ladder.h) on a PC, run by CI (Port.yml):
 *   gcc -Iport/3ds -o gs_ladder_test port/tools/gs_ladder_test.c && ./gs_ladder_test
 * Built with -DGS_LADDER_BREAK (rule K6: the "testing" comparison dropped) it must report a failure.
 */
#include <stdio.h>
#include "gs_ladder.h"

static int sFailed;

static void expect(const char *text, int rerun, int want, const char *what) {
    int got = gs_ladder_marker_action(text, GS_LADDER_VERSION, rerun);
    if (got != want) {
        printf("gs_ladder_test: %s: got %d, expected %d\n", what, got, want);
        sFailed++;
    }
}

int main(void) {
    expect(NULL, 0, GSL_RUN, "no marker runs the step");
    expect("testing 1\n", 0, GSL_LOCKED_NOW, "a step left testing locked the console");
    expect("testing 1\n", 1, GSL_LOCKED_NOW, "a lock is recorded even when re-running");
    expect("passed 1\n", 0, GSL_PASSED, "a passed step is skipped");
    expect("passed 1\n", 1, GSL_RUN, "re-running runs a passed step");
    expect("hung 1\n", 0, GSL_HUNG, "a hung step is skipped");
    expect("gpu-hung 1\n", 0, GSL_HUNG, "a GPU-hung step is skipped");
    expect("hung 1\n", 1, GSL_RUN, "re-running runs a hung step");
    expect("testing 0\n", 0, GSL_RUN, "another version's lock runs the step again");
    expect("passed 0\n", 0, GSL_RUN, "another version's pass runs the step again");
    expect("garbage", 0, GSL_RUN, "an unreadable marker runs the step");
    if (sFailed) {
        printf("gs_ladder_test: FAILED (%d)\n", sFailed);
        return 1;
    }
    printf("gs_ladder_test: all 11 cases passed\n");
    return 0;
}
