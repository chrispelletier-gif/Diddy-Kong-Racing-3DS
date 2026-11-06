/*
 * DKR_STATE_HASH=file: after each game logic step (obj_update), writes a 64-bit hash of the game's whole heap (the
 * N64 "RAM" pool every object, racer and track structure lives in) to the file, one line a frame. Two builds that
 * give the same file kept the game state bit for bit identical; the first differing line is the first frame that
 * changed. Host only, for checking native rewrites of game code (DKR_LOCKSTEP=1 makes runs repeat exactly).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

extern char __executable_start[], _end[];

extern unsigned char gPortMainPool[];
#define POOL_SIZE (8 * 1024 * 1024)

static FILE *sFile;
static int sInit;
static unsigned sFrame;

void port_state_hash(void) {
    if (!sInit) {
        const char *name = getenv("DKR_STATE_HASH");
        sInit = 1;
        if (name != NULL && name[0] != '\0') {
            sFile = fopen(name, "w");
        }
    }
    if (sFile == NULL) {
        return;
    }
    // Pointers differ between two builds (the binary's layout moves), so they are hashed position-free: a pointer into
    // the pool as its offset, one into the program's image or the C heap as a marker.
    const uint32_t *p = (const uint32_t *) gPortMainPool;
    uint32_t poolLo = (uint32_t) (uintptr_t) gPortMainPool, poolHi = poolLo + POOL_SIZE;
    uint32_t imgLo = (uint32_t) (uintptr_t) __executable_start, imgHi = (uint32_t) (uintptr_t) sbrk(0);
    uint64_t h1 = 0x9E3779B97F4A7C15ull, h2 = 0xC2B2AE3D27D4EB4Full;
    for (size_t i = 0; i < POOL_SIZE / 4; i += 2) {
        uint32_t a = p[i], b = p[i + 1];
        if (a - poolLo < POOL_SIZE) a = (a - poolLo) ^ 0xA0000000u; else if (a - imgLo < imgHi - imgLo) a = 0xB0000000u;
        if (b - poolLo < POOL_SIZE) b = (b - poolLo) ^ 0xA0000000u; else if (b - imgLo < imgHi - imgLo) b = 0xB0000000u;
        h1 = (h1 ^ a) * 0x100000001B3ull;
        h2 = (h2 ^ b) * 0x100000001B3ull;
    }
    (void) poolHi;
    {
        const char *dump = getenv("DKR_STATE_DUMP");
        if (dump != NULL && (unsigned) atoi(dump) == sFrame) {
            char name[256];
            snprintf(name, sizeof(name), "%s.dump", getenv("DKR_STATE_HASH"));
            FILE *f = fopen(name, "wb");
            fwrite(&poolLo, 4, 1, f); fwrite(&imgLo, 4, 1, f); fwrite(&imgHi, 4, 1, f);
            fwrite(gPortMainPool, 1, POOL_SIZE, f);
            fclose(f);
        }
    }
    fprintf(sFile, "%u %016llx%016llx\n", sFrame++, (unsigned long long) h1, (unsigned long long) h2);
    fflush(sFile);
}
