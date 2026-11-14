/*
 * The walk census, host side (DKR_WALK_CENSUS=1, see port/src/gfx/gfx_census.h): the game's display list calls that
 * the native renderer still walks (vertex slots, texture loads, fixed matrices, list calls), each named by the
 * source line that wrote it, counted per level, draw kind and class, and written as census.csv when the run ends.
 *
 * Sites. With the census on, the game's gbi macros (include/port_nops.h, PORT_CENSUS_SITES) call port_census_site
 * with the address of the command they just wrote and their __FILE__:__LINE__. The walk then finds the site by the
 * command's address (port_census_hit). A command with no site is in a list that holds constants (the game's gs*
 * tables, a texture's load list): it is named by the list ("static@offset" in the program, or "heap list") and the
 * site of the call that ran it. A command's site stays in the table until another write to its address replaces it,
 * since the game builds some lists once and replays them (fonts, menu panels).
 *
 * Output: DKR_CENSUS_FILE, else $DKR_DATA_DIR/census.csv, else ./census.csv:
 *   scenario,level,phase,kind,class,op,site,total,frames,per_frame
 * scenario is DKR_CENSUS_SCENARIO. phase is "load" for the first 90 frames after a level load, else "steady".
 * per_frame is the total divided by the frames of that level and phase. Rows with op "material" in the op column
 * (suffix "+material") are texture and list ops that ran inside a material capture (the first use of a material).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "gfx/gfx_census.h"

int gPortCensusSites; /* port_nops.h: the game side records sites while this is nonzero */
const char *gPortCensusFile; /* A caller's site (port_drawtag.h's wrappers), for the records obj_asm.c writes */
int gPortCensusLine;

extern char __executable_start;
extern char end;

#define SITE_SLOTS (1u << 20)
#define MAX_SITES 20000
#define ROWS (1u << 17)
#define MAX_LEVELS 128
#define LOAD_FRAMES 90
#define PENDING_MAX 512
#define DEPTH_MAX 16

typedef struct SiteEntry {
    unsigned int key; /* The command's address (0: empty) */
    unsigned int stamp; /* The walk's frame when written (not used to expire entries) */
    unsigned int site; /* Index in sSites */
} SiteEntry;

typedef struct Row {
    unsigned long long key; /* 0: empty */
    unsigned long long count;
} Row;

static SiteEntry sSiteTable[SITE_SLOTS];
static char *sSites[MAX_SITES + 1]; /* Interned site names, from 1 */
static unsigned int sNumSites;
static unsigned int sSiteHash[1u << 15]; /* name hash -> site id */
static Row sRows[ROWS];
static unsigned int sNumRows;
static unsigned long long sFrames[MAX_LEVELS][2];
static unsigned int sStamp; /* The walk's frame number */
static int sLevel, sPhase;
static unsigned int sLoads = 0xFFFFFFFFu, sSinceLoad;
static unsigned int sCaller[DEPTH_MAX + 2]; /* The site of the call that runs the list at each depth */
static unsigned long long sPending[PENDING_MAX];
static unsigned int sNumPending;
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static int sAtExit, sWarned;
static const char *sScenario = "default";

static const char *kKinds[] = {"other", "track", "racers", "objects", "sky", "see_through", "effects", "shadows", "hud"};
static const char *kClasses[] = {"record",   "state", "vertex_slot",  "tmem",         "matrix",
                                 "list_call", "raw_n64", "rect", "record_slots", "record_dropped"};
static const char *kOps[] = {"NDRAW",     "NDRAW_DYNAMIC", "SPRITE",     "NRECT",         "MTXF",       "NATIVE_MESH",
                             "NATIVE_TRACK", "NMAT",       "KIND_TAG",   "LIGHT_TAG",     "STATE",      "VIEWPORT",
                             "VTX",       "TRI",           "TILE",       "TILESIZE",      "LOADBLOCK",  "LOADTILE",
                             "LOADTLUT",  "IMAGE_TEX",     "MTX",        "CALL",          "TEXRECT",    "FILLRECT",
                             "SLOTS_DYNAMIC", "SLOTS_BILLBOARD", "SLOTS_SPRITE", "SLOTS_DECLINED", "DROP_MATERIAL",
                             "DROP_TEXTURE"};

static unsigned int intern_site(const char *name) {
    unsigned int h = 5381, i;
    const char *p;
    for (p = name; *p; p++) {
        h = h * 33 + (unsigned char) *p;
    }
    h &= (1u << 15) - 1;
    for (i = 0; i < (1u << 15); i++, h = (h + 1) & ((1u << 15) - 1)) {
        unsigned int id = sSiteHash[h];
        if (id == 0) {
            if (sNumSites >= MAX_SITES) {
                return 1;
            }
            sSites[++sNumSites] = strdup(name);
            sSiteHash[h] = sNumSites;
            return sNumSites;
        }
        if (strcmp(sSites[id], name) == 0) {
            return id;
        }
    }
    return 1;
}

static void census_write(void);

/* Sites are recorded from the start of the program: the game builds some lists once, at load, and replays them. */
__attribute__((constructor)) static void census_early(void) {
    const char *e = getenv("DKR_WALK_CENSUS");
    gPortCensusSites = (e != NULL && e[0] == '1');
}

static void census_init(void) {
    const char *e = getenv("DKR_CENSUS_SCENARIO");
    if (e != NULL && e[0] != 0) {
        sScenario = e;
    }
    intern_site("unknown"); /* Site 1 */
    if (getenv("DKR_LOCKSTEP") == NULL) {
        fprintf(stderr, "census: not a DKR_LOCKSTEP run, so most sites will be unknown\n");
    }
    atexit(census_write);
    sAtExit = 1;
}

static const char *base_name(const char *file) {
    /* From the last "src/" or "include/" on, so the lines read as the repository's paths */
    const char *p = strstr(file, "/src/"), *q = file, *r;
    while ((r = strstr(q, "/src/")) != NULL || (r = strstr(q, "/include/")) != NULL) {
        p = r;
        q = r + 1;
        if (strstr(q, "/src/") == NULL && strstr(q, "/include/") == NULL) {
            break;
        }
    }
    return (p != NULL) ? p + 1 : file;
}

void port_census_site(const void *cmd, const char *file, int line) {
    unsigned int key = (unsigned int) (unsigned long) cmd, h, i, id;
    char name[160];
    if (!sAtExit) {
        census_init();
    }
    if (gPortCensusFile != NULL && strstr(file, "obj_asm.c") != NULL) {
        /* A record written by port_native_batch and friends: the line that called it */
        file = gPortCensusFile;
        line = gPortCensusLine;
    }
    snprintf(name, sizeof(name), "%s:%d", base_name(file), line);
    pthread_mutex_lock(&sLock);
    id = intern_site(name);
    h = (key >> 3) & (SITE_SLOTS - 1);
    for (i = 0; i < 64; i++, h = (h + 1) & (SITE_SLOTS - 1)) {
        if (sSiteTable[h].key == key || sSiteTable[h].key == 0 || i == 63) {
            sSiteTable[h].key = key;
            sSiteTable[h].stamp = sStamp;
            sSiteTable[h].site = id;
            break;
        }
    }
    pthread_mutex_unlock(&sLock);
}

void port_census_caller(const char *file, int line) {
    gPortCensusFile = file;
    gPortCensusLine = line;
}

static unsigned int site_of(const void *cmd) {
    unsigned int key = (unsigned int) (unsigned long) cmd, h = (key >> 3) & (SITE_SLOTS - 1), i;
    for (i = 0; i < 64; i++, h = (h + 1) & (SITE_SLOTS - 1)) {
        if (sSiteTable[h].key == key) {
            return sSiteTable[h].site;
        }
        if (sSiteTable[h].key == 0) {
            break;
        }
    }
    return 0;
}

static unsigned long long make_key(int level, int phase, int kind, int cls, int op, int material, unsigned int site) {
    return 1ull << 62 | (unsigned long long) (level & 0xFF) << 40 | (unsigned long long) (phase & 1) << 39 |
           (unsigned long long) (kind & 15) << 35 | (unsigned long long) (cls & 15) << 31 |
           (unsigned long long) (op & 0x1FF) << 22 | (unsigned long long) (material & 1) << 21 | (site & 0x1FFFFF);
}

static void add_row(unsigned long long key, long long delta) {
    unsigned int h = (unsigned int) ((key * 0x9E3779B97F4A7C15ull) >> 47) & (ROWS - 1), i;
    for (i = 0; i < ROWS; i++, h = (h + 1) & (ROWS - 1)) {
        if (sRows[h].key == key) {
            sRows[h].count += delta;
            return;
        }
        if (sRows[h].key == 0) {
            if (sNumRows * 2 > ROWS) {
                return;
            }
            sRows[h].key = key;
            sRows[h].count = delta;
            sNumRows++;
            return;
        }
    }
}

void port_census_frame_begin(int level, unsigned int levelLoads) {
    if (!sAtExit) {
        census_init();
    }
    if (levelLoads != sLoads) {
        sLoads = levelLoads;
        sSinceLoad = 0;
    }
    sLevel = level < 0 ? 0 : (level >= MAX_LEVELS ? MAX_LEVELS - 1 : level);
    sPhase = sSinceLoad < LOAD_FRAMES ? 1 : 0;
    sFrames[sLevel][sPhase]++;
    sNumPending = 0;
    sCaller[0] = 1;
}

void port_census_frame_end(void) {
    sSinceLoad++;
    sStamp++;
    if (sStamp % 3000 == 0) {
        census_write();
    }
}

void port_census_material_captured(void) {
    unsigned int i;
    for (i = 0; i < sNumPending; i++) {
        unsigned long long key = sPending[i];
        add_row(key, -1);
        add_row(key | 1ull << 21, 1);
    }
    sNumPending = 0;
}

void port_census_hit(const void *cmd, int depth, int kind, int cls, int op, const void *list) {
    unsigned int site;
    unsigned long long key;
    int idx = depth < 0 ? 0 : (depth > DEPTH_MAX ? DEPTH_MAX : depth);
    pthread_mutex_lock(&sLock);
    site = (cls == CENSUS_STATE) ? 0 : site_of(cmd);
    if (cls == CENSUS_STATE) {
        site = intern_site("-"); /* State ops are only counted, not named */
    } else if (site == 0) {
        /* A command of a constant list: the list and the call that ran it */
        char name[200];
        const char *via = sSites[sCaller[idx] != 0 ? sCaller[idx] : 1];
        const char *ls = idx == 0 ? "frame list (no site)" : "heap list";
        char buf[48];
        if (list != NULL && (const char *) list >= &__executable_start && (const char *) list < &end) {
            snprintf(buf, sizeof(buf), "static@+0x%lx", (unsigned long) ((const char *) list - &__executable_start));
            ls = buf;
        }
        if (idx == 0 && ls[0] == 'f') {
            snprintf(name, sizeof(name), "%s", ls);
        } else {
            snprintf(name, sizeof(name), "%s via %s", ls, via);
        }
        site = intern_site(name);
    }
    if (cls == CENSUS_LIST_CALL && idx + 1 <= DEPTH_MAX + 1) {
        /* The called list's commands are named for this call (its own site, or the site that called this list) */
        unsigned int own = site_of(cmd);
        sCaller[idx + 1] = own != 0 ? own : sCaller[idx];
    }
    if (getenv("DKR_CENSUS_DEBUG") != NULL && strstr(sSites[site], "via unknown") != NULL && cls == CENSUS_TMEM) {
        static int sShown;
        if (sShown++ < 40) {
            const unsigned int *w = (const unsigned int *) cmd;
            fprintf(stderr, "census debug: %p depth %d kind %d class %d op %d words %08x %08x, previous %08x %08x\n", cmd,
                    depth, kind, cls, op, w[0], w[1], w[-2], w[-1]);
        }
    }
    key = make_key(sLevel, sPhase, kind, cls, op, 0, site);
    add_row(key, 1);
    if (cls == CENSUS_TMEM || cls == CENSUS_LIST_CALL || cls == CENSUS_STATE) {
        if (sNumPending < PENDING_MAX) {
            sPending[sNumPending++] = key;
        }
    } else if (cls == CENSUS_VERTEX_SLOT || cls == CENSUS_RECT || cls == CENSUS_RECORD) {
        if (op != CENSUS_OP_MTXF && op != CENSUS_OP_NMAT) {
            sNumPending = 0;
        }
    }
    pthread_mutex_unlock(&sLock);
}

static const char *op_name(int op, char *buf, size_t size) {
    if (op >= CENSUS_OP_RAW) {
        snprintf(buf, size, "raw_%02X", op - CENSUS_OP_RAW);
        return buf;
    }
    return op < CENSUS_OP_COUNT ? kOps[op] : "?";
}

static void census_write(void) {
    const char *path = getenv("DKR_CENSUS_FILE"), *dir = getenv("DKR_DATA_DIR");
    char full[512];
    FILE *f;
    unsigned int i;
    if (path == NULL) {
        snprintf(full, sizeof(full), "%s%scensus.csv", dir != NULL ? dir : "", dir != NULL ? "/" : "");
        path = full;
    }
    f = fopen(path, "w");
    if (f == NULL) {
        if (!sWarned) {
            fprintf(stderr, "census: cannot write %s\n", path);
            sWarned = 1;
        }
        return;
    }
    fprintf(f, "scenario,level,phase,kind,class,op,site,total,frames,per_frame\n");
    for (i = 0; i < ROWS; i++) {
        unsigned long long k = sRows[i].key;
        int level, phase, kind, cls, op, material;
        unsigned int site;
        char buf[32], opbuf[48];
        double frames;
        if (k == 0 || sRows[i].count == 0) {
            continue;
        }
        level = (int) (k >> 40 & 0xFF);
        phase = (int) (k >> 39 & 1);
        kind = (int) (k >> 35 & 15);
        cls = (int) (k >> 31 & 15);
        op = (int) (k >> 22 & 0x1FF);
        material = (int) (k >> 21 & 1);
        site = (unsigned int) (k & 0x1FFFFF);
        frames = (double) sFrames[level][phase];
        snprintf(opbuf, sizeof(opbuf), "%s%s", op_name(op, buf, sizeof(buf)), material ? "+material" : "");
        fprintf(f, "%s,%d,%s,%s,%s,%s,\"%s\",%llu,%llu,%.3f\n", sScenario, level, phase ? "load" : "steady",
                kind < 9 ? kKinds[kind] : "?", cls < 10 ? kClasses[cls] : "?", opbuf, sSites[site],
                sRows[i].count, sFrames[level][phase], frames > 0 ? (double) sRows[i].count / frames : 0.0);
    }
    fclose(f);
}
