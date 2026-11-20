/*
 * P56 K7: a check of the pictures-shown logic (port/src/plat/shown.h) on a PC, run by CI (Port.yml, host job):
 *   gcc -Iport/src -o shown_test port/tools/shown_test.c && ./shown_test
 * One tick is a microsecond. A picture is swapped every 33 ms, and the seventh swap finds the sixth still waiting for
 * the screen, so the sixth is replaced: one replaced, and 66 ms between the fifth and the seventh on the screen.
 */
#include <stdio.h>
#include "plat/shown.h"

int main(void) {
    PlatShown s = { 0 };
    unsigned int replacedCount = 0, replaced, gap;
    unsigned long long tick = 1000;
    int i;
    plat_shown_read(&s, 1, 1, replacedCount, &replaced, &gap);
    for (i = 0; i < 10; i++) {
        int pending = i == 6;
        replacedCount += pending;
        plat_shown_swap(&s, pending, tick, 1);
        tick += 33000;
    }
    plat_shown_read(&s, 1, 0, replacedCount, &replaced, &gap);
    printf("shown_test: replaced %u, longest gap %u us (expected 1 and 66000)\n", replaced, gap);
    if (replaced != 1 || gap != 66000) {
        printf("shown_test: FAILED\n");
        return 1;
    }
    /* A restart starts the window afresh: no gap until two pictures are shown after it. */
    plat_shown_read(&s, 1, 1, replacedCount, &replaced, &gap);
    plat_shown_swap(&s, 0, tick, 1);
    plat_shown_read(&s, 1, 0, replacedCount, &replaced, &gap);
    printf("shown_test: after a restart and one swap: replaced %u, longest gap %u us (expected 0 and 0)\n", replaced, gap);
    if (replaced != 0 || gap != 0) {
        printf("shown_test: FAILED\n");
        return 1;
    }
    printf("shown_test: ok\n");
    return 0;
}
