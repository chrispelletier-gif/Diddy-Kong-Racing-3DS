/*
 * The ARM11's performance counters (roadmap P56 K2, from kohjoh's HGSS port: src/audio/audio.c pmu_begin/pmu_read).
 * DKR_PMU=1 counts, on each measured core, the cycles (CCNT), the cycles the core waited for instructions (PMN0,
 * PERFCOUNTEREVT_CORE_STALL_BY_LACK_OF_INST: the share of time lost to instruction fetches, F-Zero's metric for its
 * I-cache layout) and instruction cache misses (PMN1); DKR_PMU=2 the same with PMN1 counting data cache read misses.
 * P60 A1 (proposals/3ds-native-arm-pica-candidates.md): DKR_PMU=3 counts the cycles the core waited on a data hazard
 * (PMN0, PERFCOUNTEREVT_CORE_STALL_BY_DATA_HAZARD: a load's result or a VFP result not ready yet) and the instructions
 * executed (PMN1, so cycles per instruction = CCNT / PMN1); DKR_PMU=4 the cycles the load/store unit was full (PMN0,
 * PERFCOUNTEREVT_CORE_STALL_BY_LSU_FULL: D-cache misses and the write buffer) and data cache write misses (PMN1).
 * They price the D-cache misses PMU 2 counts, and say whether the game core is bound by its instructions or its data.
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
        sKind = sKind >= 1 && sKind <= 4 ? sKind : 0;
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
    // PMN0 and PMN1 by kind (plat.h plat_pmu_on): 1 fetch stall + I-cache misses, 2 fetch stall + D-cache read misses,
    // 3 data hazard stall + instructions executed, 4 load/store unit full stall + D-cache write misses.
    static const u32 sPmn0[5] = { 0, PERFCOUNTEREVT_CORE_STALL_BY_LACK_OF_INST, PERFCOUNTEREVT_CORE_STALL_BY_LACK_OF_INST,
                                  PERFCOUNTEREVT_CORE_STALL_BY_DATA_HAZARD, PERFCOUNTEREVT_CORE_STALL_BY_LSU_FULL };
    static const u32 sPmn1[5] = { 0, PERFCOUNTEREVT_CORE_INST_CACHE_MISS, PERFCOUNTEREVT_CORE_DATA_CACHE_READ_MISS,
                                  PERFCOUNTEREVT_CORE_INST_EXECUTED, PERFCOUNTEREVT_CORE_DATA_CACHE_WRITE_MISS };
    svcControlPerformanceCounter(&unused, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_0, sPmn0[kind]);
    svcControlPerformanceCounter(&unused, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_1, sPmn1[kind]);
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

/*
 * P60 A2 (proposals/3ds-native-arm-pica-candidates.md): the VFP's RunFast mode. The ARM11's VFP hands denormal inputs
 * and results, and NaN handling, to the kernel's support code (hundreds of cycles each) unless the FPSCR has
 * flush-to-zero (FZ, bit 24) and default NaN (DN, bit 25) set and every exception trap off (bits 8-12 and 15). Nothing
 * in the port sets them; whether the kernel starts threads that way was never checked. The first call on each thread
 * logs its FPSCR ("VFP mode"); with DKR_VFP_RUNFAST=1 (default 0) the thread runs with FZ and DN set and the traps off,
 * re-read at each switch change. Called once a frame by the game thread, the walk and the backend.
 */
#define FPSCR_FZ (1u << 24)
#define FPSCR_DN (1u << 25)
#define FPSCR_TRAPS (0x1Fu << 8 | 1u << 15)

static __thread int sVfpEpoch = -1;
static __thread int sVfpSeen;
static __thread u32 sVfpBase; // The thread's FPSCR as the kernel started it

void plat_vfp_frame(const char *thread) {
    u32 now, want;
    int epoch = gPlatSwitchEpoch;
    if (__builtin_expect(epoch == sVfpEpoch, 1)) {
        return;
    }
    sVfpEpoch = epoch;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(now));
    if (!sVfpSeen) {
        sVfpSeen = 1;
        sVfpBase = now;
        plat_log("VFP mode, %s thread: FPSCR %08lx, flush-to-zero %s, default NaN %s, traps %s: RunFast %s", thread,
                 (unsigned long) now, now & FPSCR_FZ ? "on" : "off", now & FPSCR_DN ? "on" : "off",
                 now & FPSCR_TRAPS ? "on" : "off",
                 (now & (FPSCR_FZ | FPSCR_DN)) == (FPSCR_FZ | FPSCR_DN) && !(now & FPSCR_TRAPS) ? "yes" : "no");
    }
    want = plat_switch("DKR_VFP_RUNFAST", 0) ? (sVfpBase | FPSCR_FZ | FPSCR_DN) & ~FPSCR_TRAPS : sVfpBase;
    if (want != now) {
        __asm__ volatile("vmsr fpscr, %0" : : "r"(want));
    }
}
