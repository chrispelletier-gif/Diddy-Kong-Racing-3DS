/*
 * The sound lock (PICA_ALL test build, DKR_NATIVE_AUDIO_LOCK, on by default there; off in lockstep runs).
 *
 * The sound code (libultra's synthesizer, event queues and sound player, and the game's sound effect player) guards
 * the state it shares between the game thread and the sound thread by "disabling interrupts" (osSetIntMask). The port
 * emulates that with one global lock, which also holds back every emulated interrupt: the retrace and, before the
 * native frame queue, the renderer's done signals. So whenever the sound thread ran its players (up to a few ms a
 * frame, with the lock held for the whole handler), retraces and finished frames waited for it.
 *
 * Here those sections take a lock of their own, with the same rules (a thread that already holds it goes straight on,
 * and the mask value returned says whether it was held), so sound only waits for sound.
 */
#include <ultra64.h>
#include "types.h"
#include "port.h"
#include "plat/plat.h"

#ifdef PICA_ALL

static PlatSem *volatile sLock;
static s32 sNative = -1;
static __thread int sHeld;

static int native(void) {
    if (sNative < 0) {
        s32 on = plat_switch("DKR_NATIVE_AUDIO_LOCK", 1) != 0 && !port_lockstep_enabled();
        if (on && sLock == NULL) {
            PlatSem *lock = plat_sem_create(1);
            PlatSem *expected = NULL;
            if (!__atomic_compare_exchange_n(&sLock, &expected, lock, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                // Another thread made it first; this spare semaphore is never used.
            }
        }
        if (on) {
            plat_log("audio: own sound lock (no interrupt mask)");
        }
        sNative = on;
    }
    return sNative;
}

/* osSetIntMask for the sound code. */
OSIntMask port_audio_set_mask(OSIntMask mask) {
    OSIntMask prev;
    if (!native()) {
        return osSetIntMask(mask);
    }
    prev = sHeld ? OS_IM_NONE : OS_IM_ALL;
    if ((mask & OS_IM_CPU) == 0 || mask == OS_IM_NONE) {
        if (!sHeld) {
            plat_sem_wait(sLock);
            sHeld = TRUE;
        }
    } else if (sHeld) {
        sHeld = FALSE;
        plat_sem_post(sLock);
    }
    return prev;
}

#endif
