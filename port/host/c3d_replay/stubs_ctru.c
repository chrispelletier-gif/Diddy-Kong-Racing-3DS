/*
 * The pieces of libctru that citro3d and the 3DS backend (port/3ds/gfx_c3d.c) call, faked for the replay on the PC.
 * The GPU command list code (gpu.c, shaderProgram.c, shbin.c) is libctru's own, compiled from its source; everything
 * that talks to the system is here:
 *  - linear memory and VRAM come from static arenas with a simple first-fit allocator, so every address (and so every
 *    command word that holds one) is the same from run to run. Physical addresses are the arena offset plus where the
 *    3DS has that memory (FCRAM 0x20000000, VRAM 0x18000000).
 *  - command lists handed to the GPU (GX_ProcessCommandList) go to the replay's decoder (replay.c); the other GX
 *    jobs (fills, transfers, copies) are written to the log only. The GPU is always done: queues never wait.
 *  - the clock is a counter that moves on each read; sleeps, locks and events return at once; no thread is started.
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay.h"

/******** Memory ********/

typedef struct Arena {
    u8 *base;
    u32 size, phys;
    // Blocks in address order: [start, start + size), used or free.
    struct Block {
        u32 start, size;
        int used;
    } *blocks;
    int numBlocks, maxBlocks;
} Arena;

#define LINEAR_BYTES (96u << 20)
#define VRAM_BYTES (6u << 20)
static u8 sLinear[LINEAR_BYTES] __attribute__((aligned(4096)));
static u8 sVram[VRAM_BYTES] __attribute__((aligned(4096)));
static Arena sLinearArena = { sLinear, LINEAR_BYTES, 0x20000000u }, sVramArena = { sVram, VRAM_BYTES, 0x18000000u };

u32 __ctru_linear_heap = 0, __ctru_linear_heap_size = LINEAR_BYTES;

static void arena_init(Arena *a) {
    if (a->blocks == NULL) {
        a->maxBlocks = 1 << 16;
        a->blocks = calloc(a->maxBlocks, sizeof(*a->blocks));
        a->blocks[0].start = 0;
        a->blocks[0].size = a->size;
        a->numBlocks = 1;
        __ctru_linear_heap = (u32) (unsigned long) sLinear;
    }
}

/* First fit in [lo, hi) of the arena, aligned. */
static void *arena_alloc(Arena *a, u32 size, u32 align, u32 lo, u32 hi) {
    int i;
    arena_init(a);
    size = (size + 127u) & ~127u;
    align = align < 128 ? 128 : align;
    for (i = 0; i < a->numBlocks; i++) {
        struct Block *b = &a->blocks[i];
        u32 start = (b->start + align - 1) & ~(align - 1), end = b->start + b->size;
        if (b->used || start < lo || start + size > end || start + size > hi) {
            continue;
        }
        if (a->numBlocks + 2 > a->maxBlocks) {
            break;
        }
        // Split into [b->start, start) free, [start, start + size) used, [start + size, end) free.
        if (start > b->start) {
            memmove(&a->blocks[i + 1], &a->blocks[i], (a->numBlocks - i) * sizeof(*b));
            a->numBlocks++;
            a->blocks[i].size = start - a->blocks[i].start;
            i++;
            b = &a->blocks[i];
            b->start = start;
            b->size = end - start;
        }
        if (start + size < end) {
            memmove(&a->blocks[i + 1], &a->blocks[i], (a->numBlocks - i) * sizeof(*b));
            a->numBlocks++;
            a->blocks[i + 1].start = start + size;
            a->blocks[i + 1].size = end - start - size;
            a->blocks[i + 1].used = 0;
            b = &a->blocks[i];
            b->size = size;
        }
        b->used = 1;
        return a->base + start;
    }
    return NULL;
}

static int arena_has(const Arena *a, const void *p) {
    return (const u8 *) p >= a->base && (const u8 *) p < a->base + a->size;
}

static void arena_free(Arena *a, void *p) {
    u32 off;
    int i;
    if (p == NULL || !arena_has(a, p)) {
        return;
    }
    off = (u32) ((u8 *) p - a->base);
    for (i = 0; i < a->numBlocks; i++) {
        if (a->blocks[i].start == off && a->blocks[i].used) {
            a->blocks[i].used = 0;
            if (i + 1 < a->numBlocks && !a->blocks[i + 1].used) {
                a->blocks[i].size += a->blocks[i + 1].size;
                memmove(&a->blocks[i + 1], &a->blocks[i + 2], (a->numBlocks - i - 2) * sizeof(a->blocks[0]));
                a->numBlocks--;
            }
            if (i > 0 && !a->blocks[i - 1].used) {
                a->blocks[i - 1].size += a->blocks[i].size;
                memmove(&a->blocks[i], &a->blocks[i + 1], (a->numBlocks - i - 1) * sizeof(a->blocks[0]));
                a->numBlocks--;
            }
            return;
        }
    }
    replay_log("free of %p, not an allocation", p);
}

static u32 arena_space(Arena *a) {
    u32 total = 0;
    int i;
    arena_init(a);
    for (i = 0; i < a->numBlocks; i++) {
        if (!a->blocks[i].used) {
            total += a->blocks[i].size;
        }
    }
    return total;
}

void *linearMemAlign(size_t size, size_t alignment) {
    return arena_alloc(&sLinearArena, size, alignment, 0, LINEAR_BYTES);
}
void *linearAlloc(size_t size) {
    return linearMemAlign(size, 0x80);
}
void linearFree(void *mem) {
    arena_free(&sLinearArena, mem);
}
u32 linearSpaceFree(void) {
    return arena_space(&sLinearArena);
}
void *vramAlloc(size_t size) {
    return arena_alloc(&sVramArena, size, 0x80, 0, VRAM_BYTES);
}
void *vramAllocAt(size_t size, vramAllocPos pos) {
    u32 half = VRAM_BYTES / 2;
    return pos == VRAM_ALLOC_A   ? arena_alloc(&sVramArena, size, 0x80, 0, half)
           : pos == VRAM_ALLOC_B ? arena_alloc(&sVramArena, size, 0x80, half, VRAM_BYTES)
                                 : vramAlloc(size);
}
void vramFree(void *mem) {
    arena_free(&sVramArena, mem);
}
u32 vramSpaceFree(void) {
    return arena_space(&sVramArena);
}

u32 osConvertVirtToPhys(const void *vaddr) {
    if (arena_has(&sLinearArena, vaddr)) {
        return sLinearArena.phys + (u32) ((const u8 *) vaddr - sLinearArena.base);
    }
    if (arena_has(&sVramArena, vaddr)) {
        return sVramArena.phys + (u32) ((const u8 *) vaddr - sVramArena.base);
    }
    return 0; // As on the 3DS for memory the GPU can't see
}

void *replay_phys_to_virt(u32 phys, u32 bytes) {
    const Arena *a[2] = { &sLinearArena, &sVramArena };
    int i;
    for (i = 0; i < 2; i++) {
        if (phys >= a[i]->phys && phys - a[i]->phys + (unsigned long long) bytes <= a[i]->size) {
            return a[i]->base + (phys - a[i]->phys);
        }
    }
    return NULL;
}

/******** GPU jobs ********/

Result GX_ProcessCommandList(u32 *buf0a, u32 buf0s, u8 flags) {
    (void) flags;
    replay_command_list(buf0a, buf0s / 4);
    return 0;
}
Result GX_MemoryFill(u32 *buf0a, u32 buf0v, u32 *buf0e, u16 control0, u32 *buf1a, u32 buf1v, u32 *buf1e,
                     u16 control1) {
    replay_gx_job("fill", osConvertVirtToPhys(buf0a), buf0v, osConvertVirtToPhys(buf0e), control0,
                  osConvertVirtToPhys(buf1a), buf1v, osConvertVirtToPhys(buf1e), control1);
    return 0;
}
Result GX_DisplayTransfer(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 flags) {
    replay_gx_job("transfer", osConvertVirtToPhys(inadr), indim, osConvertVirtToPhys(outadr), outdim, flags, 0, 0, 0);
    return 0;
}
Result GX_TextureCopy(u32 *inadr, u32 indim, u32 *outadr, u32 outdim, u32 size, u32 flags) {
    // Textures moved to VRAM (promote_textures): the copy is made, so later frames read the same texels.
    void *out = outadr;
    if (inadr != NULL && out != NULL && size > 0) {
        memmove(out, inadr, size);
    }
    replay_gx_job("copy", osConvertVirtToPhys(inadr), indim, osConvertVirtToPhys(outadr), outdim, size, flags, 0, 0);
    return 0;
}
void GX_BindQueue(gxCmdQueue_s *queue) {
    (void) queue;
}
void gxCmdQueueRun(gxCmdQueue_s *queue) {
    (void) queue;
}
void gxCmdQueueStop(gxCmdQueue_s *queue) {
    (void) queue;
}
void gxCmdQueueClear(gxCmdQueue_s *queue) {
    if (queue != NULL) {
        queue->numEntries = 0;
        queue->curEntry = 0;
        queue->lastEntry = 0;
    }
}
bool gxCmdQueueWait(gxCmdQueue_s *queue, s64 timeout) {
    (void) queue;
    (void) timeout;
    return true;
}
Result GSPGPU_FlushDataCache(const void *adr, u32 size) {
    (void) adr;
    (void) size;
    return 0;
}
Result GSPGPU_InvalidateDataCache(const void *adr, u32 size) {
    (void) adr;
    (void) size;
    return 0;
}
Result svcFlushProcessDataCache(Handle process, u32 addr, u32 size) {
    (void) process;
    (void) addr;
    (void) size;
    return 0;
}
Result svcStoreProcessDataCache(Handle process, u32 addr, u32 size) { // P57 E step 2 (DKR_FE_FLUSH=2)
    (void) process;
    (void) addr;
    (void) size;
    return 0;
}
Result svcInvalidateProcessDataCache(Handle process, u32 addr, u32 size) {
    (void) process;
    (void) addr;
    (void) size;
    return 0;
}

/******** Screens ********/

static u8 *sFramebuffer[2];
static bool sWide, s3d;

u8 *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height) {
    int s = screen == GFX_TOP ? 0 : 1;
    (void) side;
    if (sFramebuffer[s] == NULL) {
        sFramebuffer[s] = linearAlloc(800 * 240 * 4);
    }
    if (width != NULL) {
        *width = 240;
    }
    if (height != NULL) {
        *height = s == 0 ? (sWide ? 800 : 400) : 320;
    }
    return sFramebuffer[s];
}
GSPGPU_FramebufferFormat gfxGetScreenFormat(gfxScreen_t screen) {
    (void) screen;
    return GSP_BGR8_OES;
}
void gfxScreenSwapBuffers(gfxScreen_t scr, bool hasStereo) {
    (void) scr;
    (void) hasStereo;
}
bool gfxIsWide(void) {
    return sWide;
}
void gfxSetWide(bool enable) {
    sWide = enable;
}
bool gfxIs3D(void) {
    return s3d;
}
void gfxSet3D(bool enable) {
    s3d = enable;
}
bool gspIsPresentPending(unsigned screen) {
    (void) screen;
    return false;
}
bool gspPresentBuffer(unsigned screen, unsigned swap, const void *fb_a, const void *fb_b, u32 stride, u32 mode) {
    (void) screen;
    (void) swap;
    (void) fb_a;
    (void) fb_b;
    (void) stride;
    (void) mode;
    return true;
}
void gspSetEventCallback(GSPGPU_Event id, ThreadFunc cb, void *data, bool oneShot) {
    (void) id;
    (void) cb;
    (void) data;
    (void) oneShot;
}
GSPGPU_Event gspWaitForAnyEvent(void) {
    return GSPGPU_EVENT_VBlank0;
}
void gspWaitForEvent(GSPGPU_Event id, bool nextEvent) {
    (void) id;
    (void) nextEvent;
}

/******** System ********/

static u64 sTicks;
u64 svcGetSystemTick(void) {
    sTicks += 1000; // About 3.6 us of the 3DS's 268 MHz clock a read
    return sTicks;
}
double osTickCounterRead(const TickCounter *cnt) {
    return cnt->elapsed * (1000.0 / SYSCLOCK_ARM11);
}
s32 svcGetProcessorID(void) {
    return 1; // The backend's core
}
void svcSleepThread(s64 ns) {
    (void) ns;
}
void svcBreak(UserBreakType breakReason) {
    replay_log("svcBreak(%d)", (int) breakReason);
    abort();
}
Result APT_CheckNew3DS(bool *out) {
    *out = !replay_old_3ds();
    return 0;
}
Result CFGU_GetSystemModel(u8 *model) {
    *model = replay_old_3ds() ? 0 : 2; // CFG_MODEL_3DS, CFG_MODEL_N3DS
    return 0;
}
Result cfguInit(void) {
    return 0;
}
void cfguExit(void) {
}
void aptHook(aptHookCookie *cookie, aptHookFn callback, void *param) {
    (void) cookie;
    (void) callback;
    (void) param;
}
void aptUnhook(aptHookCookie *cookie) {
    (void) cookie;
}
Thread threadCreate(ThreadFunc entrypoint, void *arg, size_t stack_size, int prio, int core_id, bool detached) {
    (void) entrypoint;
    (void) arg;
    (void) stack_size;
    (void) prio;
    (void) core_id;
    (void) detached;
    return NULL; // The backend's own workers (frame history) are test build extras: it carries on without them
}
void LightLock_Init(LightLock *lock) {
    *lock = 0;
}
void LightLock_Lock(LightLock *lock) {
    (void) lock;
}
void LightLock_Unlock(LightLock *lock) {
    (void) lock;
}
void LightEvent_Init(LightEvent *event, ResetType reset_type) {
    memset(event, 0, sizeof(*event));
    (void) reset_type;
}
void LightEvent_Signal(LightEvent *event) {
    (void) event;
}
void LightEvent_Wait(LightEvent *event) {
    (void) event;
}

/* citro3d's Tex3DS loader (tex3ds.c) is linked but never used by the backend. */
bool decompressV(const decompressIOVec *iov, size_t iovcnt, decompressCallback callback, void *userdata,
                 size_t insize) {
    (void) iov;
    (void) iovcnt;
    (void) callback;
    (void) userdata;
    (void) insize;
    return false;
}
ssize_t decompressCallback_FD(void *userdata, void *buffer, size_t size) {
    (void) userdata;
    (void) buffer;
    (void) size;
    return -1;
}
ssize_t decompressCallback_Stdio(void *userdata, void *buffer, size_t size) {
    (void) userdata;
    (void) buffer;
    (void) size;
    return -1;
}

/* P59 C (gpu_hang.txt's state section): no GPU registers on the PC, so they print as n/a */
Result GSPGPU_ReadHWRegs(u32 regAddr, u32 *data, u8 size) {
    (void) regAddr;
    (void) data;
    (void) size;
    return -1;
}
