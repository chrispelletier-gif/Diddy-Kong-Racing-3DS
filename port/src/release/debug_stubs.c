/*
 * A player's build (make RELEASE=1) leaves the unattended test runs (port/src/game/port_autorun.c) out; these stand in
 * for what the game and the rest of the port call. No test run is ever on.
 */
#include <ultra64.h>
#include "types.h"
#include "port.h"

s32 port_autorun_enabled(void) {
    return FALSE;
}

s32 port_autorun_active(void) {
    return FALSE;
}

s32 port_autorun_title(void) {
    return FALSE;
}

s32 port_autorun_drives(void) {
    return FALSE;
}

s32 port_autorun_time_trial(void) {
    return FALSE;
}

s32 port_autorun_quit_race(void) {
    return FALSE;
}

void port_autorun_request(s32 start) {
    (void) start;
}

s32 port_autorun_switch_count(void) {
    return 0;
}

s32 port_autorun_shelved_count(void) {
    return 0;
}

void port_autorun_frame(s32 updateRate) {
    (void) updateRate;
}

int port_autorun_input(unsigned short *buttons, signed char *stickX, signed char *stickY) {
    (void) buttons;
    (void) stickX;
    (void) stickY;
    return 0;
}

/* The host renderer's clipping sweep. */
s32 port_sweep_level_vertex(const void *vertices) {
    (void) vertices;
    return FALSE;
}

/* The profiler's draw kinds (port_drawtag.h): nothing is timed. */
void port_draw_kind_game(int kind, int object) {
    (void) kind;
    (void) object;
}
