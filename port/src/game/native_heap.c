/*
 * Native big buffers (roadmap NC11, DKR_NATIVE_HEAP, default 0).
 *
 * The game carves its whole heap out of one pool the size of the N64's RAM (8 MB). Most of that is empty: the textures
 * live outside it (decoded at level load), so even the heaviest levels use a few MB. With the switch on:
 *   - the big buffers (the display lists, the vertex, matrix and triangle heaps, the particle, wave, snow, shadow and
 *     void buffers: anything of mempool_alloc_big's size and up) are native allocations of exactly the size asked, taken
 *     from the C heap and given back with the usual mempool_free;
 *   - the pool itself is only as big as what is left (DKR_NATIVE_HEAP_POOL_KB, default NATIVE_POOL_KB_DEFAULT), so the
 *     rest of the old 8 MB stays free for textures and the GPU's copies.
 * It is read once, at boot, because the pool's size follows it: changing it needs a restart.
 */
#include <ultra64.h>
#include <string.h>
#include "types.h"
#include "port.h"
#include "plat/plat.h"

#define NATIVE_BLOCKS 128
#define NATIVE_ALIGN 64
/* Smaller allocations stay in the pool: a native block costs a registry slot and its own heap header. */
#define NATIVE_MIN_SIZE (8 * 1024)
/* The pool's size with the switch on, in KB. Chosen from a pool-use survey of every level (see the PR). */
#define NATIVE_POOL_KB_DEFAULT 5120

typedef struct {
    u8 *raw;
    u8 *data;
    u32 size;
} NativeBlock;

static NativeBlock sBlocks[NATIVE_BLOCKS];
static s32 sCount;
static u32 sBytes, sPeakBytes, sPeakCount;
static s32 sOn = -1;

s32 port_native_heap_on(void) {
    if (sOn < 0) {
        sOn = plat_switch("DKR_NATIVE_HEAP", 0) != 0;
    }
    return sOn;
}

/* The pool's size in bytes for this run. */
u32 port_native_heap_pool_size(void) {
    s32 kb;
    if (!port_native_heap_on()) {
        return PORT_MAIN_POOL_SIZE;
    }
    kb = plat_switch("DKR_NATIVE_HEAP_POOL_KB", NATIVE_POOL_KB_DEFAULT);
    if (kb < 1024) {
        kb = 1024;
    }
    if (kb > (s32) (PORT_MAIN_POOL_SIZE / 1024)) {
        kb = PORT_MAIN_POOL_SIZE / 1024;
    }
    return (u32) kb * 1024;
}

/* A zeroed native block, or NULL (switch off, too small, no registry slot or no memory: the caller uses the pool). */
void *port_native_heap_alloc(s32 size) {
    s32 i;
    u8 *raw;
    if (!port_native_heap_on() || size < NATIVE_MIN_SIZE || sCount >= NATIVE_BLOCKS) {
        return NULL;
    }
    raw = (u8 *) plat_malloc((size_t) size + NATIVE_ALIGN);
    if (raw == NULL) {
        plat_log("native heap: no memory for %d KB, using the pool", (int) (size / 1024));
        return NULL;
    }
    for (i = 0; sBlocks[i].raw != NULL; i++) {
    }
    sBlocks[i].raw = raw;
    sBlocks[i].data = (u8 *) (((uintptr_t) raw + NATIVE_ALIGN - 1) & ~(uintptr_t) (NATIVE_ALIGN - 1));
    sBlocks[i].size = (u32) size;
    bzero(sBlocks[i].data, (size_t) size);
    sCount++;
    sBytes += (u32) size;
    if (sBytes > sPeakBytes) {
        sPeakBytes = sBytes;
    }
    if ((u32) sCount > sPeakCount) {
        sPeakCount = (u32) sCount;
    }
    return sBlocks[i].data;
}

/* Frees the native block that starts at address, if there is one. Returns whether there was. */
s32 port_native_heap_free(const void *address) {
    s32 i;
    if (sCount == 0 || address == NULL) {
        return FALSE;
    }
    for (i = 0; i < NATIVE_BLOCKS; i++) {
        if (sBlocks[i].raw != NULL && sBlocks[i].data == (const u8 *) address) {
            sBytes -= sBlocks[i].size;
            sCount--;
            plat_free(sBlocks[i].raw);
            bzero(&sBlocks[i], sizeof(sBlocks[i]));
            return TRUE;
        }
    }
    return FALSE;
}

/* For the load log line: native blocks live now, their size, and the most there have been at once. */
void port_native_heap_stats(u32 *blocks, u32 *bytes, u32 *peakBytes, u32 *peakBlocks) {
    *blocks = (u32) sCount;
    *bytes = sBytes;
    *peakBytes = sPeakBytes;
    *peakBlocks = sPeakCount;
}
