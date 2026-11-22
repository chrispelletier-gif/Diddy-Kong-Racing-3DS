/*
 * Backend replay (roadmap P56 package D2): plays a record file of the host build's backend calls
 * (port/host/backend_record.c) through the 3DS backend, port/3ds/gfx_c3d.c, and the real citro3d, compiled for the
 * PC, and writes down what the GPU would have been given.
 *
 *   c3d_replay RECORD [--words FILE] [--state FILE] [--frames N]
 *
 * Every command list the backend hands to the GPU is decoded (PICA200 command headers: register, byte mask, extra
 * parameters, consecutive registers) into a model of the GPU's registers, and of the memories behind its data
 * ports (vertex shader uniforms, code and operand descriptors, the fog, lighting and procedural texture tables, the
 * fixed vertex attributes). At each draw (a write of GPUREG_DRAWARRAYS or GPUREG_DRAWELEMENTS):
 *  --words: one line, "frame draw: words... | hash", the command words written since the previous draw (in the order
 *    the GPU reads them) and a hash of the vertex and index data the draw reads (found from the registers). GPU jobs
 *    that are not command lists (fills, transfers, copies) get a line of their own. Two commits' backends replaying
 *    one record are compared with this (port/tools/backend_replay.py, the "words" check).
 *  --state: the registers and port memory words whose value changed since the previous draw, binary: per draw u32
 *    frame, u32 draw, u32 count, then count (u32 key, u32 value) pairs; a key is a register number (below 0x400) or
 *    0x10000 * (port + 1) + the word's index in that port's memory. Two runs of one backend with different switches
 *    are compared with this (the "state" check, e.g. DKR_STATE_CACHE=0 against the default): fewer words by design,
 *    but the same registers at every draw.
 * Registers that start work rather than hold a setting (draws, flushes, the end of a list) and the data ports
 * themselves are left out of the state; so is GPUREG_TEXUNIT_CONFIG's "clear texture cache" bit.
 *
 * What this does not model: timing and ordering against the GPU (which register a draw still reads while the next
 * list is written: the Crescent Island hang was that kind of bug, texture registers written right after a draw),
 * the pictures themselves, and stereo 3D (the host build never asks for it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "gfx/gfx_backend.h"
#include "record_format.h"
#include "replay.h"

/*
 * DKR_WALK_VERTS (P54 step 2, PR #563): with REPLAY_WALK=1 each frame's draws go through the walk's side as on the
 * 3DS's two-core renderer: gfxb_walk_frame_start after the frame's start, then gfxb_walk_draw_indexed for each
 * indexed draw (its record runs at once, stubs_plat.c gfx_split_call_*); a draw it leaves goes to gfxb_draw_indexed.
 * Weak: a backend without them (before #563) replays as before. gfxb_walk_stats gives the self-check's counts.
 */
extern void gfxb_walk_frame_start(void) __attribute__((weak));
extern int gfxb_walk_draw_indexed(const GfxDrawState *state, const GfxVertex *verts, int numVerts,
                                  const unsigned short *indices, int numTriangles) __attribute__((weak));
extern void gfxb_walk_stats(unsigned int *drawn, unsigned int *checked, unsigned int *differ) __attribute__((weak));
/* DKR_RAW_REGS (P55 S3): the raw path's counts, with DKR_RAW_REGS_CHECK's. Weak: older backends have none. */
extern void gfxb_raw_stats(unsigned int *draws, unsigned int *hits, unsigned int *checked, unsigned int *differ)
    __attribute__((weak));
static int sWalk; // REPLAY_WALK=1 and the backend has the walk's side

/******** GPU register model ********/

#define NUM_REGS 0x400
enum {
    REG_FINALIZE = 0x010,
    REG_TEXUNIT_CONFIG = 0x080,
    REG_FB_INVALIDATE = 0x110,
    REG_FB_FLUSH = 0x111,
    REG_ATTRIBBUFFERS_LOC = 0x200,
    REG_ATTRIBBUFFERS_FORMAT_HIGH = 0x202,
    REG_ATTRIBBUFFER0_OFFSET = 0x203,
    REG_INDEXBUFFER_CONFIG = 0x227,
    REG_NUMVERTICES = 0x228,
    REG_VERTEX_OFFSET = 0x22A,
    REG_DRAWARRAYS = 0x22E,
    REG_DRAWELEMENTS = 0x22F,
    REG_VTX_FUNC = 0x231,
    REG_START_DRAW_FUNC0 = 0x245,
    REG_VSH_CODETRANSFER_END = 0x2BF,
};

/* A data port: writes to its index register set where the next data write goes; each data write fills one word. */
typedef struct Port {
    const char *name;
    unsigned int indexReg, dataFirst, dataLast, size;
    unsigned int ptr;
    uint32_t *mem;
} Port;

static Port sPorts[] = {
    { "fog table", 0x0E6, 0x0E8, 0x0EF, 128 },
    { "proctex table", 0x0AF, 0x0B0, 0x0B7, 16 * 256 },
    { "light table", 0x1C5, 0x1C8, 0x1CF, 32 * 256 },
    { "fixed attributes", 0x232, 0x233, 0x235, 16 * 3 },
    { "gs uniforms", 0x290, 0x291, 0x298, 96 * 4 },
    { "gs code", 0x29B, 0x29C, 0x2A3, 4096 },
    { "gs opdesc", 0x2A5, 0x2A6, 0x2AD, 128 },
    { "vs uniforms", 0x2C0, 0x2C1, 0x2C8, 96 * 4 },
    { "vs code", 0x2CB, 0x2CC, 0x2D3, 4096 },
    { "vs opdesc", 0x2D5, 0x2D6, 0x2DD, 128 },
};
#define NUM_PORTS (int) (sizeof(sPorts) / sizeof(sPorts[0]))

static uint32_t sRegs[NUM_REGS];
static signed char sPortOfReg[NUM_REGS]; // port + 1 for a data register, -(port + 1) for an index register
static unsigned char sNotState[NUM_REGS]; // Left out of the state: triggers

/* Keys changed since the last draw, for --state: a list and a mark per key. */
#define MAX_DIRTY 65536
static uint32_t sDirty[MAX_DIRTY];
static int sNumDirty;
static unsigned char sDirtyReg[NUM_REGS];
static unsigned char *sDirtyPort[NUM_PORTS];
static uint32_t sShadowRegs[NUM_REGS]; // Each key's value at the last draw written out
static uint32_t *sShadowPort[NUM_PORTS];

static FILE *sWordsOut, *sStateOut;
static unsigned int sFrame, sDrawInFrame;
static unsigned long long sDraws, sWordsTotal, sJobs;
static uint32_t *sPending; // The words since the last draw
static unsigned int sNumPending, sMaxPending;

static void model_init(void) {
    int p;
    unsigned int r;
    for (p = 0; p < NUM_PORTS; p++) {
        sPorts[p].mem = calloc(sPorts[p].size, 4);
        sShadowPort[p] = calloc(sPorts[p].size, 4);
        sDirtyPort[p] = calloc(sPorts[p].size, 1);
        sPortOfReg[sPorts[p].indexReg] = (signed char) -(p + 1);
        for (r = sPorts[p].dataFirst; r <= sPorts[p].dataLast; r++) {
            sPortOfReg[r] = (signed char) (p + 1);
            sNotState[r] = 1;
        }
    }
    sNotState[REG_FINALIZE] = sNotState[REG_FB_INVALIDATE] = sNotState[REG_FB_FLUSH] = 1;
    sNotState[REG_DRAWARRAYS] = sNotState[REG_DRAWELEMENTS] = sNotState[REG_VTX_FUNC] = 1;
    sNotState[REG_VSH_CODETRANSFER_END] = 1;
    sNotState[0x28F] = 1; // GPUREG_GSH_CODETRANSFER_END
}

static void mark_dirty(uint32_t key) {
    if (sNumDirty < MAX_DIRTY) {
        sDirty[sNumDirty++] = key;
    } else {
        replay_log("too many changes between two draws");
        exit(3);
    }
}

static void set_port_word(int p, unsigned int i, uint32_t value) {
    Port *port = &sPorts[p];
    if (i >= port->size) {
        return;
    }
    port->mem[i] = value;
    if (!sDirtyPort[p][i]) {
        sDirtyPort[p][i] = 1;
        mark_dirty(0x10000u * (unsigned int) (p + 1) + i);
    }
}

static void port_index(int p, uint32_t v) {
    Port *port = &sPorts[p];
    switch (port->indexReg) {
        case 0x0AF: port->ptr = (v & 0xFF) + ((v >> 8) & 0xF) * 256; break;
        case 0x1C5: port->ptr = (v & 0xFF) + ((v >> 8) & 0x1F) * 256; break;
        case 0x232: port->ptr = (v & 0xF) * 3; break;
        case 0x290:
        case 0x2C0: port->ptr = (v & 0xFF) * 4; break; // f32 uniforms take 4 words, f24 3: either way from there
        default: port->ptr = v & (port->size - 1); break;
    }
}

static void reg_write(unsigned int reg, uint32_t mask, uint32_t value) {
    int port = sPortOfReg[reg];
    uint32_t v = (sRegs[reg] & ~mask) | (value & mask);
    if (port > 0) {
        set_port_word(port - 1, sPorts[port - 1].ptr++, value);
        return;
    }
    sRegs[reg] = v;
    if (port < 0) {
        port_index(-port - 1, v);
    }
    if (!sNotState[reg] && !sDirtyReg[reg]) {
        sDirtyReg[reg] = 1;
        mark_dirty(reg);
    }
}

/* FNV-1a, 64 bits. */
static uint64_t hash_bytes(uint64_t h, const void *p, uint32_t n) {
    const unsigned char *b = p;
    uint32_t i;
    for (i = 0; i < n; i++) {
        h = (h ^ b[i]) * 0x100000001b3ULL;
    }
    return h;
}

/* The vertex and index data the draw reads, as the registers point at it. */
static uint64_t draw_data_hash(int elements) {
    uint64_t h = 0xcbf29ce484222325ULL;
    uint32_t base = (sRegs[REG_ATTRIBBUFFERS_LOC] & 0x1FFFFFFF) << 3, count = sRegs[REG_NUMVERTICES], first = 0,
             last = 0, i;
    int b;
    if (count == 0) {
        return h;
    }
    if (elements) {
        uint32_t cfg = sRegs[REG_INDEXBUFFER_CONFIG], shorts = cfg >> 31, bytes = count << shorts;
        const void *idx = replay_phys_to_virt(base + (cfg & 0x0FFFFFFF), bytes);
        if (idx == NULL) {
            return 0; // Points outside the GPU's memory: shows up as a hash of 0
        }
        h = hash_bytes(h, idx, bytes);
        first = 0xFFFFFFFF;
        for (i = 0; i < count; i++) {
            uint32_t v = shorts ? ((const uint16_t *) idx)[i] : ((const uint8_t *) idx)[i];
            first = v < first ? v : first;
            last = v > last ? v : last;
        }
    } else {
        first = sRegs[REG_VERTEX_OFFSET];
        last = first + count - 1;
    }
    for (b = 0; b < 12; b++) {
        uint32_t off = sRegs[REG_ATTRIBBUFFER0_OFFSET + b * 3], cfg2 = sRegs[REG_ATTRIBBUFFER0_OFFSET + b * 3 + 2];
        uint32_t stride = (cfg2 >> 16) & 0xFF, comps = cfg2 >> 28;
        const void *v;
        if (comps == 0 || stride == 0) {
            continue;
        }
        v = replay_phys_to_virt(base + off + first * stride, (last - first + 1) * stride);
        if (v == NULL) {
            return 0;
        }
        h = hash_bytes(h, v, (last - first + 1) * stride);
    }
    return h;
}

static void write_draw(int elements) {
    unsigned int i;
    if (sWordsOut != NULL) {
        fprintf(sWordsOut, "%u %u:", sFrame, sDrawInFrame);
        for (i = 0; i < sNumPending; i++) {
            fprintf(sWordsOut, " %08x", sPending[i]);
        }
        fprintf(sWordsOut, " | %016llx\n", (unsigned long long) draw_data_hash(elements));
    }
    if (sStateOut != NULL) {
        uint32_t head[3] = { sFrame, sDrawInFrame, 0 };
        long at = ftell(sStateOut);
        int k;
        fwrite(head, 4, 3, sStateOut);
        for (k = 0; k < sNumDirty; k++) {
            uint32_t key = sDirty[k], value;
            if (key < NUM_REGS) {
                value = sRegs[key];
                if (key == REG_TEXUNIT_CONFIG) {
                    value &= ~(1u << 16); // The texture cache clear: an action, not a setting
                }
                sDirtyReg[key] = 0;
                if (value == sShadowRegs[key]) {
                    continue;
                }
                sShadowRegs[key] = value;
            } else {
                int p = (int) (key >> 16) - 1;
                unsigned int w = key & 0xFFFF;
                value = sPorts[p].mem[w];
                sDirtyPort[p][w] = 0;
                if (value == sShadowPort[p][w]) {
                    continue;
                }
                sShadowPort[p][w] = value;
            }
            fwrite(&key, 4, 1, sStateOut);
            fwrite(&value, 4, 1, sStateOut);
            head[2]++;
        }
        fseek(sStateOut, at + 8, SEEK_SET);
        fwrite(&head[2], 4, 1, sStateOut);
        fseek(sStateOut, 0, SEEK_END);
    }
    sNumDirty = 0;
    sWordsTotal += sNumPending;
    sNumPending = 0;
    sDraws++;
    sDrawInFrame++;
}

static void pending_add(const uint32_t *w, unsigned int n) {
    if (sNumPending + n > sMaxPending) {
        sMaxPending = (sNumPending + n) * 2 + 1024;
        sPending = realloc(sPending, sMaxPending * 4);
    }
    memcpy(sPending + sNumPending, w, n * 4);
    sNumPending += n;
}

void replay_command_list(const uint32_t *words, uint32_t count) {
    uint32_t i = 0;
    if (words == NULL) {
        replay_log("frame %u: a command list outside the GPU's memory", sFrame);
        return;
    }
    while (i + 2 <= count) {
        uint32_t header = words[i + 1], reg = header & 0x3FF, extra = (header >> 20) & 0xFF, k, n;
        uint32_t m = (header >> 16) & 0xF, mask = ((m & 1) ? 0xFFu : 0) | ((m & 2) ? 0xFF00u : 0) |
                                                  ((m & 4) ? 0xFF0000u : 0) | ((m & 8) ? 0xFF000000u : 0);
        int consecutive = (header >> 31) != 0;
        n = 2 + extra + (extra & 1);
        if (i + n > count) {
            replay_log("frame %u: a command runs past the end of its list", sFrame);
            n = count - i;
            extra = n - 2;
        }
        pending_add(words + i, n);
        for (k = 0; k <= extra; k++) {
            uint32_t r = consecutive ? (reg + k) & 0x3FF : reg, v = k == 0 ? words[i] : words[i + 1 + k];
            reg_write(r, mask, v);
            if ((r == REG_DRAWARRAYS || r == REG_DRAWELEMENTS) && (mask & 1)) {
                write_draw(r == REG_DRAWELEMENTS);
            }
        }
        i += n;
    }
}

void replay_gx_job(const char *what, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
                   uint32_t h) {
    sJobs++;
    if (sWordsOut != NULL) {
        fprintf(sWordsOut, "%u job %s %08x %08x %08x %08x %08x %08x %08x %08x\n", sFrame, what, a, b, c, d, e, f, g, h);
    }
}

/******** Record file ********/

static FILE *sIn;
static const char *sInName;

static void read_bytes(void *p, size_t n) {
    if (n > 0 && fread(p, 1, n, sIn) != n) {
        replay_log("%s: ends in the middle of a record", sInName);
        exit(2);
    }
}

static int read_s32(void) {
    int v;
    read_bytes(&v, 4);
    return v;
}

static void *scratch(int which, size_t bytes) {
    static void *sBuf[4];
    static size_t sSize[4];
    if (bytes > sSize[which]) {
        sSize[which] = bytes * 2;
        sBuf[which] = realloc(sBuf[which], sSize[which]);
    }
    return sBuf[which];
}

/* Textures and meshes by the file's id. */
typedef struct Thing {
    void *p;
    unsigned int *pixels; // A texture's
} Thing;
static Thing *sThings;
static unsigned int sNumThings;

static Thing *thing(unsigned int id) {
    if (id >= sNumThings) {
        unsigned int n = id * 2 + 256;
        sThings = realloc(sThings, n * sizeof(Thing));
        memset(sThings + sNumThings, 0, (n - sNumThings) * sizeof(Thing));
        sNumThings = n;
    }
    return &sThings[id];
}

static void read_state(GfxDrawState *s) {
    int i;
    read_bytes(s, sizeof(*s));
    for (i = 0; i < 2; i++) {
        unsigned int id = (unsigned int) (unsigned long) s->textures[i];
        s->textures[i] = id != 0 ? thing(id)->p : NULL;
        if (id != 0 && s->textures[i] == NULL) {
            replay_log("frame %u: a draw uses texture %u, never uploaded", sFrame, id);
        }
    }
}

static GfxTexture *read_texture(unsigned int id) {
    Thing *t = thing(id);
    GfxTexture in, *tex = t->p;
    read_bytes(&in, sizeof(in));
    if (tex == NULL) {
        tex = t->p = calloc(1, sizeof(GfxTexture));
    }
    in.backendData = tex->backendData; // A texture uploaded again keeps the backend's copy
    in.pixels = tex->pixels;
    *tex = in;
    return tex;
}

int main(int argc, char **argv) {
    RecHeader h;
    int i, frames = -1, keptFrames = 0, op;
    const char *wordsPath = NULL, *statePath = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--words") == 0 && i + 1 < argc) {
            wordsPath = argv[++i];
        } else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            statePath = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (sInName == NULL && argv[i][0] != '-') {
            sInName = argv[i];
        } else {
            fprintf(stderr, "usage: c3d_replay RECORD [--words FILE] [--state FILE] [--frames N]\n");
            return 2;
        }
    }
    if (sInName == NULL || (sIn = fopen(sInName, "rb")) == NULL) {
        fprintf(stderr, "c3d_replay: can't read %s\n", sInName != NULL ? sInName : "(no record file)");
        return 2;
    }
    read_bytes(&h, sizeof(h));
    if (h.magic != REC_MAGIC || h.version != REC_VERSION || h.sizeofVertex != sizeof(GfxVertex) ||
        h.sizeofDrawState != sizeof(GfxDrawState) || h.sizeofTexture != sizeof(GfxTexture) ||
        h.sizeofModelVertex != sizeof(GfxModelVertex) || h.sizeofModelParams != sizeof(GfxModelParams) ||
        h.sizeofMeshPart != sizeof(GfxMeshPart) || h.sizeofPointer != sizeof(void *)) {
        fprintf(stderr, "c3d_replay: %s was recorded with other structs (version %u; vertex %u/%u, state %u/%u, "
                "texture %u/%u, model vertex %u/%u, params %u/%u, part %u/%u): record it again with this build\n",
                sInName, h.version, h.sizeofVertex, (unsigned) sizeof(GfxVertex), h.sizeofDrawState,
                (unsigned) sizeof(GfxDrawState), h.sizeofTexture, (unsigned) sizeof(GfxTexture), h.sizeofModelVertex,
                (unsigned) sizeof(GfxModelVertex), h.sizeofModelParams, (unsigned) sizeof(GfxModelParams),
                h.sizeofMeshPart, (unsigned) sizeof(GfxMeshPart));
        return 4;
    }
    if (wordsPath != NULL && (sWordsOut = fopen(wordsPath, "w")) == NULL) {
        fprintf(stderr, "c3d_replay: can't write %s\n", wordsPath);
        return 2;
    }
    if (statePath != NULL && (sStateOut = fopen(statePath, "wb")) == NULL) {
        fprintf(stderr, "c3d_replay: can't write %s\n", statePath);
        return 2;
    }
    setenv("DKR_NO_GPU_TEST", "1", 0); // Its frames wait on the GPU's results
    // The 3DS's shared system pages, which libctru's inline functions read at fixed addresses (osGet3DSliderState:
    // the 3D slider is down, so no 3D).
    if (mmap((void *) 0x1FF80000, 0x2000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
             0) != (void *) 0x1FF80000) {
        fprintf(stderr, "c3d_replay: can't map the 3DS's shared pages at 0x1FF80000\n");
        return 2;
    }
    model_init();
    {
        const char *walk = getenv("REPLAY_WALK");
        sWalk = walk != NULL && atoi(walk) != 0 && gfxb_walk_frame_start != NULL && gfxb_walk_draw_indexed != NULL;
        if (walk != NULL && atoi(walk) != 0 && !sWalk) {
            printf("walk_none 1 ");
        }
    }

    while ((op = fgetc(sIn)) != EOF) {
        GfxDrawState state;
        GfxModelParams params;
        int n, m;
        switch (op) {
            case REC_INIT:
                gfxb_init();
                break;
            case REC_START_FRAME:
                if (frames >= 0 && keptFrames >= frames) {
                    goto done;
                }
                sFrame = (unsigned int) read_s32();
                sDrawInFrame = 0;
                gfxb_start_frame();
                if (sWalk) {
                    gfxb_walk_frame_start();
                }
                break;
            case REC_END_FRAME:
                gfxb_end_frame();
                keptFrames++;
                break;
            case REC_CLEAR_COLOR: {
                short rect[4];
                unsigned char rgba[4];
                read_bytes(rect, 8);
                read_bytes(rgba, 4);
                gfxb_clear_color(rect, rgba[0], rgba[1], rgba[2], rgba[3]);
                break;
            }
            case REC_CLEAR_DEPTH: {
                short rect[4];
                read_bytes(rect, 8);
                gfxb_clear_depth(rect);
                break;
            }
            case REC_DRAW_TRIANGLES: {
                GfxVertex *v;
                read_state(&state);
                n = read_s32();
                v = scratch(0, sizeof(GfxVertex) * 3 * (size_t) n);
                read_bytes(v, sizeof(GfxVertex) * 3 * (size_t) n);
                gfxb_draw_triangles(&state, v, n);
                break;
            }
            case REC_DRAW_INDEXED: {
                GfxVertex *v;
                unsigned short *idx;
                read_state(&state);
                n = read_s32();
                v = scratch(0, sizeof(GfxVertex) * (size_t) n);
                read_bytes(v, sizeof(GfxVertex) * (size_t) n);
                m = read_s32();
                idx = scratch(1, 6 * (size_t) m);
                read_bytes(idx, 6 * (size_t) m);
                if (!sWalk || !gfxb_walk_draw_indexed(&state, v, n, idx, m)) {
                    gfxb_draw_indexed(&state, v, n, idx, m);
                }
                break;
            }
            case REC_DRAW_MODEL: {
                GfxModelVertex *v;
                unsigned short *idx;
                read_state(&state);
                read_bytes(&params, sizeof(params));
                n = read_s32();
                v = scratch(0, sizeof(GfxModelVertex) * (size_t) n);
                read_bytes(v, sizeof(GfxModelVertex) * (size_t) n);
                m = read_s32();
                idx = scratch(1, 6 * (size_t) m);
                read_bytes(idx, 6 * (size_t) m);
                gfxb_draw_model(&state, &params, v, n, idx, m);
                break;
            }
            case REC_DRAW_MESHES: {
                GfxMeshPart *parts;
                int k, kept = 0;
                read_state(&state);
                read_bytes(&params, sizeof(params));
                n = read_s32();
                parts = scratch(0, sizeof(GfxMeshPart) * (size_t) (n > 0 ? n : 1));
                read_bytes(parts, sizeof(GfxMeshPart) * (size_t) n);
                for (k = 0; k < n; k++) {
                    unsigned int id = (unsigned int) (unsigned long) parts[k].mesh;
                    parts[kept] = parts[k];
                    parts[kept].mesh = id != 0 ? thing(id)->p : NULL;
                    kept += parts[kept].mesh != NULL; // A mesh the 3DS backend had no room for is left out
                }
                if (kept > 0) {
                    gfxb_draw_meshes(&state, &params, parts, kept);
                }
                break;
            }
            case REC_MESH_CREATE: {
                unsigned int id = (unsigned int) read_s32();
                GfxModelVertex *v;
                unsigned short *idx;
                n = read_s32();
                v = scratch(0, sizeof(GfxModelVertex) * (size_t) n);
                read_bytes(v, sizeof(GfxModelVertex) * (size_t) n);
                m = read_s32();
                idx = scratch(1, 2 * (size_t) m);
                read_bytes(idx, 2 * (size_t) m);
                if (id != 0) {
                    thing(id)->p = gfxb_mesh_create(v, n, idx, m);
                    if (thing(id)->p == NULL) {
                        replay_log("frame %u: no room for mesh %u", sFrame, id);
                    }
                }
                break;
            }
            case REC_MESH_FREE: {
                Thing *t = thing((unsigned int) read_s32());
                if (t->p != NULL) {
                    gfxb_mesh_free(t->p);
                    t->p = NULL;
                }
                break;
            }
            case REC_TEXTURE_UPLOAD: {
                unsigned int id = (unsigned int) read_s32();
                GfxTexture *tex = read_texture(id);
                unsigned int *old = thing(id)->pixels, *px = malloc(4 * (size_t) tex->width * tex->height + 4);
                read_bytes(px, 4 * (size_t) tex->width * tex->height);
                tex->pixels = thing(id)->pixels = px;
                gfxb_texture_upload(tex);
                free(old);
                break;
            }
            case REC_TEXTURE_FREE: {
                Thing *t = thing((unsigned int) read_s32());
                if (t->p != NULL) {
                    gfxb_texture_free(t->p);
                    free(t->p);
                    free(t->pixels);
                    t->p = NULL;
                    t->pixels = NULL;
                }
                break;
            }
            case REC_TEXTURE_SHARE: {
                unsigned int id = (unsigned int) read_s32();
                GfxTexture *dst = read_texture(id);
                Thing *src = thing((unsigned int) read_s32());
                if (src->p != NULL) {
                    dst->pixels = ((GfxTexture *) src->p)->pixels;
                    gfxb_texture_share(dst, src->p);
                } else {
                    replay_log("frame %u: texture %u shares one never uploaded", sFrame, id);
                }
                break;
            }
            case REC_GPU_KIND:
                gfxb_gpu_kind(read_s32());
                break;
            case REC_GPU_KINDS_FRAME:
                gfxb_gpu_kinds_frame(read_s32());
                break;
            case REC_GPU_TRANSFORM:
                gfxb_gpu_transform();
                break;
            default:
                replay_log("%s: unknown record %d", sInName, op);
                return 2;
        }
    }
done:
    if (sWalk && gfxb_walk_stats != NULL) {
        unsigned int drawn, checked, differ;
        gfxb_walk_stats(&drawn, &checked, &differ);
        printf("walk_drawn %u walk_checked %u walk_differ %u ", drawn, checked, differ);
    }
    if (gfxb_raw_stats != NULL) {
        unsigned int draws, hits, checked, differ;
        gfxb_raw_stats(&draws, &hits, &checked, &differ);
        printf("raw_draws %u raw_hits %u raw_checked %u raw_differ %u ", draws, hits, checked, differ);
    }
    printf("frames %d draws %llu words %llu words/draw %.2f jobs %llu\n", keptFrames, sDraws, sWordsTotal,
           sDraws ? (double) sWordsTotal / (double) sDraws : 0.0, sJobs);
    if (sWordsOut != NULL) {
        fclose(sWordsOut);
    }
    if (sStateOut != NULL) {
        fclose(sStateOut);
    }
    return 0;
}
