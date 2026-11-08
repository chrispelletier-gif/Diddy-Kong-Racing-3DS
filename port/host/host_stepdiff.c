/*
 * 60 fps step check (host only, DKR_STEPDIFF=N): every Nth race frame at 30 fps, runs the frame's game logic twice in
 * forked copies of the game, once as one 30 fps step and once as two 60 fps half steps, and writes every game word
 * that ends up different to DKR_STEPDIFF_OUT (default stepdiff.bin) with its value before and after each. A value that
 * moves twice as far at 60 fps is per-frame logic the 60 fps fixes (port.h PORT_*) don't cover yet.
 * port/tools/stepdiff.py reads the file. The real game carries on exactly as if this weren't here.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "plat/plat.h"

extern char __data_start[], _end[];
extern int gPortHalfStep, gPortSkipTick;
int port_list_objects(unsigned int *out, int max);
extern unsigned char gPortMainPool[];

static unsigned int sObjStart[1024], sObjEnd[1024];
static int sNumSorted;

static int cmp_u32(const void *a, const void *b) {
    unsigned int x = *(const unsigned int *) a, y = *(const unsigned int *) b;
    return x < y ? -1 : x > y;
}

/* Whether a heap word belongs to an object (the object and the behaviour data right after it). */
static int in_object(unsigned int addr) {
    int lo = 0, hi = sNumSorted - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (addr < sObjStart[mid]) {
            hi = mid - 1;
        } else if (addr >= sObjEnd[mid]) {
            lo = mid + 1;
        } else {
            return 1;
        }
    }
    return 0;
}

#define MAX_OBJS 1024
#define MAX_RECORDS 200000

static int sEvery = -1;
static int sFrame, sSample, sContext;
static const char *sOut;
static char sBeforePath[256], sAfterPath[256];

static void dump(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(__data_start, 1, _end - __data_start, f);
        fclose(f);
    }
}

static unsigned char *load(const char *path, size_t size) {
    unsigned char *buf = malloc(size);
    FILE *f = fopen(path, "rb");
    if (buf == NULL || f == NULL || fread(buf, 1, size, f) != size) {
        exit(3);
    }
    fclose(f);
    return buf;
}

static void run_child(int sixty, void (*step)(long), void (*reinput)(long)) {
    alarm(20);
    if (sixty) {
        gPortHalfStep = 1;
        gPortSkipTick = 1;
        step(1);
        reinput(1);
        gPortSkipTick = 0;
        step(1);
        gPortHalfStep = 0;
    } else {
        gPortHalfStep = 0;
        gPortSkipTick = 0;
        step(2);
    }
    if (!sixty) {
        dump(sAfterPath);
        _exit(0);
    }
    {
        size_t size = _end - __data_start;
        unsigned char *before = load(sBeforePath, size), *after30 = load(sAfterPath, size);
        unsigned char *now = (unsigned char *) __data_start;
        static unsigned int objs[MAX_OBJS * 3];
        int numObjs = port_list_objects(objs, MAX_OBJS), i, records = 0;
        FILE *f = fopen(sOut, "ab");
        unsigned int hdr[4] = { 0x46494453, (unsigned) sSample, (unsigned) sContext, (unsigned) numObjs };
        unsigned int poolStart = (unsigned) (uintptr_t) gPortMainPool, poolEnd = poolStart + 0x800000;
        if (f == NULL) {
            _exit(4);
        }
        for (i = 0; i < numObjs; i++) {
            sObjStart[i] = objs[i * 3];
        }
        sNumSorted = numObjs;
        qsort(sObjStart, numObjs, 4, cmp_u32);
        for (i = 0; i < numObjs; i++) {
            unsigned int end = sObjStart[i] + 0x800;
            sObjEnd[i] = i + 1 < numObjs && sObjStart[i + 1] < end ? sObjStart[i + 1] : end;
        }
        // The objects as they were before the step (both copies share the start).
        fwrite(hdr, 4, 4, f);
        fwrite(objs, 4, numObjs * 3, f);
        {
            unsigned int base = (unsigned) (uintptr_t) __data_start; // Where the data was loaded, for nm's addresses.
            fwrite(&base, 4, 1, f);
        }
        for (i = 0; i + 4 <= (int) size; i += 4) {
            unsigned int b, a3, a6;
            memcpy(&a3, after30 + i, 4);
            memcpy(&a6, now + i, 4);
            if (a3 == a6) {
                continue;
            }
            {
                unsigned int addr = (unsigned) (uintptr_t) (__data_start + i);
                if (addr >= poolStart && addr < poolEnd && !in_object(addr)) {
                    continue;
                }
            }
            memcpy(&b, before + i, 4);
            {
                unsigned int rec[4] = { (unsigned) (uintptr_t) (__data_start + i), b, a3, a6 };
                fwrite(rec, 4, 4, f);
            }
            if (++records >= MAX_RECORDS) {
                break;
            }
        }
        {
            unsigned int end[4] = { 0, 0, 0, 0 };
            fwrite(end, 4, 4, f);
        }
        fclose(f);
        _exit(0);
    }
}

void plat_step_diff(int updateRate, void (*step)(long), void (*reinput)(long), int context) {
    pid_t pid;
    int status;
    if (sEvery < 0) {
        const char *env = getenv("DKR_STEPDIFF");
        sEvery = env != NULL ? atoi(env) : 0;
        sOut = getenv("DKR_STEPDIFF_OUT") != NULL ? getenv("DKR_STEPDIFF_OUT") : "stepdiff.bin";
        snprintf(sBeforePath, sizeof(sBeforePath), "%s.before", sOut);
        snprintf(sAfterPath, sizeof(sAfterPath), "%s.after30", sOut);
    }
    if (sEvery <= 0 || updateRate != 2 || plat_frame_retraces() != 2 || gPortHalfStep) {
        return;
    }
    if (++sFrame % sEvery != 0) {
        return;
    }
    sSample++;
    sContext = context;
    fflush(NULL);
    dump(sBeforePath);
    pid = fork();
    if (pid == 0) {
        run_child(0, step, reinput);
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return;
    }
    pid = fork();
    if (pid == 0) {
        run_child(1, step, reinput);
    }
    waitpid(pid, &status, 0);
}
