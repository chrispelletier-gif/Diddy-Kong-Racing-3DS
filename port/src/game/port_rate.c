/*
 * 60 fps steps (port.h): the main loop (src/thread3_main.c) tells this file how many retraces each logic frame took,
 * once a frame, and the PORT_* macros (include/port_step.h) in the racer, object, HUD and menu code read the two flags
 * it sets.
 */
#include <stdlib.h>

#include "plat/plat.h"
#include "port.h"

s32 gPortHalfStep;
s32 gPortSkipTick;
s32 gPortLogicStep;

static s32 sRateFix = -1; // -1 until DKR_RATE_FIX is read.

void port_set_logic_rate(s32 updateRate) {
    gPortLogicStep = updateRate;
    if (sRateFix < 0) {
        const char *env = plat_getenv("DKR_RATE_FIX");
        sRateFix = env == NULL || atoi(env) != 0;
        port_log("60 fps physics fix: %s\n", sRateFix ? "on" : "off");
    }
    // Only with the 60 fps cap: at 30 fps the odd 1-retrace frame (after a load, say) runs as it always did.
    if (sRateFix && updateRate == 1 && plat_frame_retraces() == 1) {
        gPortHalfStep = TRUE;
        gPortSkipTick = !gPortSkipTick;
    } else {
        gPortHalfStep = FALSE;
        gPortSkipTick = FALSE;
    }
}

/*
 * The kart stuck on two wheels (0.2 alpha report): update_racer_collision tilts a kart only from wheels 0 and 1 (roll)
 * and 0 and 2 (pitch). When those are all in the air and the kart rests on the others, say reversing and turning up
 * a ramp's edge, nothing ever changes its tilt, so it drives on one or two wheels until it falls off something.
 * On (the default), a kart car on one or two wheels eases its roll and pitch toward level, as the game already does
 * for its other vehicles (racer.c, func_80046524), until enough wheels land to tilt it from the ground again.
 */
s32 port_car_settle(void) {
    return plat_switch("DKR_CAR_SETTLE", 1) != 0;
}
