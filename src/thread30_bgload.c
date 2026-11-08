#include "thread30_bgload.h"
#include "game.h"
#include "macros.h"
#include "stacks.h"
#include "thread3_main.h"
#include <ultra64.h>
#include "port_loadtime.h"
#ifdef TARGET_PORT
#include "plat/plat.h"
#endif
#ifdef PICA_ALL
// Native wake-up (DKR_NATIVE_BGLOAD, off in lockstep runs): a semaphore instead of the message queue.
#include "port.h"
static PlatSem *sBgLoadSem;
#endif

/************ .data ************/

s32 gThread30NeedToLoadLevel = FALSE;
s32 gThread30LevelIdToLoad = -1;
s32 gThread30CutsceneIdToLoad = -1;
s32 gThread30LoadDelay = 0;

/*******************************/

/************ .bss ************/

OSThread gThread30;
OSMesgQueue gThread30MesgQueue;
OSMesg gThread30Message[2];
u64 gThread30Stack[STACKSIZE(STACK_BGLOAD)];

/*****************************/

/**
 * Initalizes and starts thread30()
 */
void bgload_init(void) {
#ifdef PICA_ALL
    if (plat_switch("DKR_NATIVE_BGLOAD", 1) && !port_lockstep_enabled()) {
        sBgLoadSem = plat_sem_create(0);
        plat_log("bgload: native wake-up (no message queue)");
    }
#endif
    osCreateMesgQueue(&gThread30MesgQueue, gThread30Message, ARRAY_COUNT(gThread30Message));
    osCreateThread(&gThread30, 30, &thread30_bgload, NULL, &gThread30Stack[STACKSIZE(STACK_BGLOAD)], 8);
    osStartThread(&gThread30);
}

/**
 * Stop thread30()
 * Official Name: amStop
 */
void bgload_kill(void) {
    osStopThread(&gThread30);
}

/**
 * Returns the value in gThread30NeedToLoadLevel.
 */
s32 bgload_active(void) {
    return gThread30NeedToLoadLevel;
}

/**
 * If gThread30NeedToLoadLevel is set, then decrement gThread30LoadDelay. If gThread30LoadDelay is 0,
 * then signal thread30 to load a level.
 */
void bgload_tick(void) {
    if (gThread30NeedToLoadLevel && gThread30LoadDelay > 0) {
        gThread30LoadDelay--;
        if (gThread30LoadDelay == 0) {
            // Signal thread30 that the level needs to load.
#ifdef PICA_ALL
            if (sBgLoadSem != NULL) {
                plat_sem_post(sBgLoadSem);
                return;
            }
#endif
            osSendMesg(&gThread30MesgQueue, (OSMesg *) OS_MESG_TYPE_LOOPBACK, OS_MESG_NOBLOCK);
        }
    }
}

/**
 * Returns the value in gThread30LoadDelay.
 */
UNUSED s32 bgload_timer(void) {
    return gThread30LoadDelay;
}

/**
 * Sets a level id and cutscene id to be loaded. Used in the tracks menu & credits menu.
 * Returns 1 if successful, or 0 if gThread30NeedToLoadLevel was already set.
 */
s32 bgload_start(s32 levelId, s32 cutsceneId) {
    if (!gThread30NeedToLoadLevel) {
        PORT_LOAD_BG_QUEUED();
        gThread30LoadDelay = 4;
        gThread30LevelIdToLoad = levelId;
        gThread30CutsceneIdToLoad = cutsceneId;
        gThread30NeedToLoadLevel = TRUE;
        return TRUE;
    }
    return FALSE;
}

/**
 * Waits for a signal from the main thread to load a level asynchronously.
 */
void thread30_bgload(UNUSED void *arg) {
    OSMesg mesg = 0;
    while (TRUE) {
        // Wait for a signal from the main thread
#ifdef PICA_ALL
        if (sBgLoadSem != NULL) {
            plat_sem_wait(sBgLoadSem);
            load_level_for_menu(gThread30LevelIdToLoad, -1, gThread30CutsceneIdToLoad);
            gThread30NeedToLoadLevel = FALSE;
            continue;
        }
#endif
        osRecvMesg(&gThread30MesgQueue, &mesg, OS_MESG_BLOCK);
        if (mesg != (OSMesg) OS_MESG_TYPE_LOOPBACK) {
            continue;
        }
        // -1 means there won't be any racers loaded.
        load_level_for_menu(gThread30LevelIdToLoad, -1, gThread30CutsceneIdToLoad);
        gThread30NeedToLoadLevel = FALSE;
    }
}
