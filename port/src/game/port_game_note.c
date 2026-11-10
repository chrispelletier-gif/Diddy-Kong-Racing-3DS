/*
 * What the player's kart is doing (lap, boost, item, paused, ...), set once a frame for the port's other threads: bug
 * reports and the profiler show it, and tilt steering (3ds/input_3ds.c) only steers while it says racing. It is in its
 * own file because a player's build leaves port_autorun.c out and tilt steering must still work there.
 */
#include <ultra64.h>
#include "types.h"
#include "structs.h"
#include "game.h"
#include "thread3_main.h"
#include "menu.h"
#include "objects.h"
#include "port.h"
#include "plat/plat.h"

void port_game_note_frame(void) {
    Object *obj = (get_game_mode() == GAMEMODE_INGAME) ? get_racer_object_by_port(PLAYER_ONE) : NULL;
    Object_Racer *racer = (obj != NULL) ? obj->racer : NULL;
    PlatGameNote note = { 0 };
    if (racer != NULL) {
        note.racing = 1;
        note.lap = racer->lap + 1;
        note.checkpoint = racer->nextCheckpoint;
        note.bananas = racer->bananas;
        note.boost = racer->boostTimer;
        note.itemType = racer->balloon_type;
        note.itemCount = racer->balloon_quantity;
        note.shield = racer->shieldTimer;
        note.magnet = racer->magnetTimer;
        note.hit = racer->attackType;
        note.paused = is_game_paused() != 0;
    }
    // Not racing, paused, or finished: the buttons steer a menu (input_3ds.c keeps A, B and the D-Pad standard then).
    note.menu = (racer == NULL || note.paused || racer->raceFinished) ? 1 : 0;
    plat_set_game_note(&note);
}
