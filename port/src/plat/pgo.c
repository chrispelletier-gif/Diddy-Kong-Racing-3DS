/*
 * Profile-guided optimisation, the training build (make PGO=gen; Old 3DS game-core work). Every function counts its
 * branches; the compiler puts each file's counters' description in the gcov_info section (-fprofile-info-section)
 * instead of registering an exit-time writer, which a 3DS application never reaches. At the end of each part of the
 * whole game run (port_autorun_section_done) all counts so far are written to one file in the data folder,
 * pgo_counts.bin, which the test run's report attaches. port/tools/pgo_unpack.py turns it back into the .gcda files
 * next to the objects, and make PGO=use builds with them.
 *
 * pgo_counts.bin: "DKRP", then for each file a u32 name length, the name (the .gcda path the compiler chose: the
 * object's), a u32 data length and the .gcda data, all little endian.
 */
#ifdef PORT_PGO_GEN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat/plat.h"

struct gcov_info;
extern void __gcov_info_to_gcda(const struct gcov_info *info, void (*filename_fn)(const char *, void *),
                                void (*dump_fn)(const void *, unsigned, void *),
                                void *(*allocate_fn)(unsigned, void *), void *arg);
extern const struct gcov_info *const __start_gcov_info[];
extern const struct gcov_info *const __stop_gcov_info[];

typedef struct {
    unsigned char *data;
    unsigned int size, capacity;
    unsigned int lengthAt; // Where the current file's data length goes
    int failed;
} PgoBuffer;

static void put(PgoBuffer *b, const void *data, unsigned int size) {
    if (b->failed) {
        return;
    }
    if (b->size + size > b->capacity) {
        unsigned int capacity = b->capacity ? b->capacity * 2 : 256 * 1024;
        unsigned char *grown;
        while (capacity < b->size + size) {
            capacity *= 2;
        }
        grown = realloc(b->data, capacity);
        if (grown == NULL) {
            b->failed = 1;
            return;
        }
        b->data = grown;
        b->capacity = capacity;
    }
    memcpy(b->data + b->size, data, size);
    b->size += size;
}

static void put_u32(PgoBuffer *b, unsigned int v) {
    unsigned char bytes[4] = { (unsigned char) v, (unsigned char) (v >> 8), (unsigned char) (v >> 16),
                               (unsigned char) (v >> 24) };
    put(b, bytes, 4);
}

static void on_filename(const char *name, void *arg) {
    PgoBuffer *b = arg;
    unsigned int length = (unsigned int) strlen(name);
    put_u32(b, length);
    put(b, name, length);
    b->lengthAt = b->size;
    put_u32(b, 0); // Filled in once the file's data is in
}

static void on_data(const void *data, unsigned size, void *arg) {
    put(arg, data, size);
}

static void *on_allocate(unsigned size, void *arg) {
    (void) arg;
    return malloc(size); // Small and rare (value profiles' lists); not freed, as libgcov's own writer doesn't
}

/* Writes every count so far to pgo_counts.bin. */
void pgo_dump(const char *why) {
    static PgoBuffer b;
    const struct gcov_info *const *info;
    char path[256];
    int files = 0;
    unsigned long long start = plat_time_ns();

    b.size = 0;
    b.failed = 0;
    put(&b, "DKRP", 4);
    for (info = __start_gcov_info; info < __stop_gcov_info; info++) {
        unsigned int before;
        if (*info == NULL) {
            continue;
        }
        __gcov_info_to_gcda(*info, on_filename, on_data, on_allocate, &b);
        if (b.failed) {
            break;
        }
        before = b.size - b.lengthAt - 4;
        b.data[b.lengthAt] = (unsigned char) before;
        b.data[b.lengthAt + 1] = (unsigned char) (before >> 8);
        b.data[b.lengthAt + 2] = (unsigned char) (before >> 16);
        b.data[b.lengthAt + 3] = (unsigned char) (before >> 24);
        files++;
    }
    if (b.failed) {
        plat_log("profile counts (%s): out of memory at %u bytes", why, b.size);
        return;
    }
    snprintf(path, sizeof(path), "%s/pgo_counts.bin", plat_data_dir());
    if (!plat_save_file(path, b.data, b.size)) {
        plat_log("profile counts (%s): could not write %s", why, path);
        return;
    }
    plat_log("profile counts (%s): %d files, %u KB, %llu ms", why, files, b.size / 1024,
             (plat_time_ns() - start) / 1000000ULL);
}

void port_autorun_section_done(const char *section) {
    pgo_dump(section);
}
#endif
