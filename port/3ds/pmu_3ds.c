/*
 * The ARM11's performance counters (roadmap P56 K2, from kohjoh's HGSS port: src/audio/audio.c pmu_begin/pmu_read).
 * DKR_PMU=1 counts, on each measured core, the cycles (CCNT), the cycles the core waited for instructions (PMN0,
 * PERFCOUNTEREVT_CORE_STALL_BY_LACK_OF_INST: the share of time lost to instruction fetches, F-Zero's metric for its
 * I-cache layout) and instruction cache misses (PMN1); DKR_PMU=2 the same with PMN1 counting data cache read misses.
 * Debug builds only, default 0.
 *
 * The CP15 counters belong to a core, not a thread: a sample on the game's core counts everything that ran there
 * since the one before (the game, an Old 3DS's display list walk, the sound callback), and one on the renderer's core
 * everything there. So the numbers are per core.
 *
 * Unverified on retail hardware (kohjoh only saw zeros in Azahar): if the first bench rows read zeros, or core 0's
 * cycles are far from 268M a second, the feature goes (these functions become the stubs in plat_posix.c).
 */
#include <3ds.h>

#include "plat/plat.h"

#if PLAT_DEBUG_TOOLS

static int sState;                 // 0: not tried yet, 1: on, -1: not available (the system call failed)
static int sKind, sEpoch = -1;     // DKR_PMU as last read, and the switch epoch it was read in
static __thread int sThreadKind;   // The kind this thread last set its core's events for (0: not set)
static PlatPmuSample sLast[PLAT_PMU_SLOTS], sSum[PLAT_PMU_SLOTS];
static int sHaveLast[PLAT_PMU_SLOTS];
static unsigned int sFrames[PLAT_PMU_SLOTS];

static int pmu_enable(void) {
    u64 unused;
    Result r = svcControlPerformanceCounter(&unused, PERFCOUNTEROP_ENABLE, 0, 0);
    if (R_FAILED(r)) {
        plat_log("CPU counters: not available (error %08lx)", (unsigned long) r);
        return -1;
    }
    // 64-bit values: the kernel keeps counting past the 32-bit registers' overflow.
    svcControlPerformanceCounter(&unused, PERFCOUNTEROP_SET_VIRTUAL_COUNTER_ENABLED, 1, 0);
    plat_log("CPU counters: on");
    return 1;
}

int plat_pmu_on(void) {
    int epoch = gPlatSwitchEpoch;
    if (sState < 0) {
        return 0;
    }
    if (epoch != sEpoch) {
        sEpoch = epoch;
        sKind = plat_switch("DKR_PMU", 0);
        sKind = sKind == 1 || sKind == 2 ? sKind : 0;
        if (sKind != 0 && sState == 0) {
            sState = pmu_enable();
            if (sState < 0) {
                sKind = 0;
            }
        }
    }
    return sKind;
}

void plat_pmu_thread_start(void) {
    u64 unused;
    int kind = plat_pmu_on();
    if (kind == 0) {
        return;
    }
    svcControlPerformanceCounter(&unused, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_0,
                                 PERFCOUNTEREVT_CORE_STALL_BY_LACK_OF_INST);
    svcControlPerformanceCounter(&unused, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_1,
                                 kind == 2 ? PERFCOUNTEREVT_CORE_DATA_CACHE_READ_MISS
                                           : PERFCOUNTEREVT_CORE_INST_CACHE_MISS);
    svcControlPerformanceCounter(&unused, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_CYCLE_COUNTER,
                                 PERFCOUNTEREVT_CORE_CYCLE_COUNT);
    svcControlPerformanceCounter(&unused, PERFCOUNTEROP_RESET, BIT(0) | BIT(1) | BIT(2), BIT(0) | BIT(1) | BIT(2));
    sThreadKind = kind;
}

void plat_pmu_sample(PlatPmuSample *s) {
    u64 v;
    s->cycles = R_SUCCEEDED(svcControlPerformanceCounter(&v, PERFCOUNTEROP_GET_VALUE,
                                                         PERFCOUNTERREG_CORE_CYCLE_COUNTER, 0)) ? v : 0;
    s->pmn0 = R_SUCCEEDED(svcControlPerformanceCounter(&v, PERFCOUNTEROP_GET_VALUE,
                                                       PERFCOUNTERREG_CORE_COUNT_REG_0, 0)) ? v : 0;
    s->pmn1 = R_SUCCEEDED(svcControlPerformanceCounter(&v, PERFCOUNTEROP_GET_VALUE,
                                                       PERFCOUNTERREG_CORE_COUNT_REG_1, 0)) ? v : 0;
}

/* One sample a frame on a slot's thread: what its core counted since the slot's last sample goes to its totals. */
void plat_pmu_frame(int slot) {
    PlatPmuSample now;
    int kind = plat_pmu_on();
    if (kind == 0) {
        sHaveLast[slot] = 0;
        return;
    }
    if (sThreadKind != kind) {
        plat_pmu_thread_start(); // (Resets the counters: the next delta starts here.)
        sHaveLast[slot] = 0;
    }
    plat_pmu_sample(&now);
    // A delta that went backwards (another thread reset the counters, if a reset reaches every core) is dropped.
    if (sHaveLast[slot] && now.cycles >= sLast[slot].cycles && now.pmn0 >= sLast[slot].pmn0 &&
        now.pmn1 >= sLast[slot].pmn1) {
        sSum[slot].cycles += now.cycles - sLast[slot].cycles;
        sSum[slot].pmn0 += now.pmn0 - sLast[slot].pmn0;
        sSum[slot].pmn1 += now.pmn1 - sLast[slot].pmn1;
        sSum[slot].kind = kind;
        sFrames[slot]++;
    }
    sLast[slot] = now;
    sHaveLast[slot] = 1;
}

void plat_pmu_totals(int slot, PlatPmuSample *sum, unsigned int *frames) {
    *sum = sSum[slot]; // (64-bit fields written by another core may tear: a rough number for one frame at worst)
    *frames = sFrames[slot];
}

void plat_pmu_exit(void) {
    if (sState > 0) {
        u64 unused;
        svcControlPerformanceCounter(&unused, PERFCOUNTEROP_DISABLE, 0, 0); // Unlocked for the next application
        sState = 0;
    }
}

#else

int plat_pmu_on(void) {
    return 0;
}
void plat_pmu_thread_start(void) {
}
void plat_pmu_sample(PlatPmuSample *s) {
    s->cycles = s->pmn0 = s->pmn1 = 0;
    s->kind = 0;
}
void plat_pmu_frame(int slot) {
    (void) slot;
}
void plat_pmu_totals(int slot, PlatPmuSample *sum, unsigned int *frames) {
    (void) slot;
    sum->cycles = sum->pmn0 = sum->pmn1 = 0;
    sum->kind = 0;
    *frames = 0;
}
void plat_pmu_exit(void) {
}

#endif
