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

typedef struct {
    const char *swName, *logLine;
    PlatSem *volatile lock;
    s32 native;
} OwnLock;

static OwnLock sSoundLock = { "DKR_NATIVE_AUDIO_LOCK", "audio: own sound lock (no interrupt mask)", NULL, -1 };
static OwnLock sHeapLock = { "DKR_NATIVE_HEAP_LOCK", "memory: own heap lock (no interrupt mask)", NULL, -1 };
static __thread int sSoundHeld, sHeapHeld;

static int lock_native(OwnLock *l) {
    if (l->native < 0) {
        s32 on = plat_switch(l->swName, 1) != 0 && !port_lockstep_enabled();
        if (on && l->lock == NULL) {
            PlatSem *lock = plat_sem_create(1);
            PlatSem *expected = NULL;
            // Another thread may make it first; the spare semaphore is then never used.
            __atomic_compare_exchange_n(&l->lock, &expected, lock, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
        if (on) {
            plat_log("%s", l->logLine);
        }
        l->native = on;
    }
    return l->native;
}

/* osSetIntMask semantics on an own lock: a mask without the CPU interrupts takes it (once per thread), any other
   releases it; the value returned says whether this thread held it before. */
static OSIntMask lock_set(OwnLock *l, int *held, OSIntMask mask) {
    OSIntMask prev = *held ? OS_IM_NONE : OS_IM_ALL;
    if ((mask & OS_IM_CPU) == 0 || mask == OS_IM_NONE) {
        if (!*held) {
            plat_sem_wait(l->lock);
            *held = TRUE;
        }
    } else if (*held) {
        *held = FALSE;
        plat_sem_post(l->lock);
    }
    return prev;
}

/* osSetIntMask for the sound code. */
OSIntMask port_audio_set_mask(OSIntMask mask) {
    if (!lock_native(&sSoundLock)) {
        return osSetIntMask(mask);
    }
    return lock_set(&sSoundLock, &sSoundHeld, mask);
}

/*
 * The heap lock (DKR_NATIVE_HEAP_LOCK, on by default; off in lockstep runs). The game's memory pools (memory.c) guard
 * their slot tables with interrupts_disable()/interrupts_enable(), which in the port is the same global lock as above,
 * so every allocation or free by the game thread or the background level loader also held back retraces. The pools
 * take no other lock inside these sections, so this lock is a leaf: it can't deadlock with the sound lock.
 */
u32 port_heap_lock(void) {
    extern u32 interrupts_disable(void);
    if (!lock_native(&sHeapLock)) {
        return interrupts_disable();
    }
    return lock_set(&sHeapLock, &sHeapHeld, OS_IM_NONE);
}

void port_heap_unlock(u32 flags) {
    extern void interrupts_enable(u32 flags);
    if (!lock_native(&sHeapLock)) {
        interrupts_enable(flags);
        return;
    }
    if (flags != OS_IM_NONE) {
        lock_set(&sHeapLock, &sHeapHeld, OS_IM_ALL);
    }
}

#endif
