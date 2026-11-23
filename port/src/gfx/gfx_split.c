/*
 * The two-core renderer (DKR_SPLIT_RENDER). The renderer has two halves of about the same size: the display list
 * walk (gfx_rdp.c: the RSP's vertex maths, texture decodes, batching) and the backend that turns its draws into GPU
 * commands (gfx_c3d.c on the 3DS). With the switch on, the walk queues its backend calls here instead of making them,
 * and a second renderer thread on the other core (plat.h PLAT_THREAD_RCP2) makes them in the same order: the walk of
 * frame N+1 runs while the backend builds frame N. An Old 3DS, whose one core held the whole renderer and the game,
 * has the system core for the other half. On by default on both consoles (bench numbers: docs/bench-history.md).
 *
 * The queue is a byte ring of records, each a backend call with its arguments copied (the walk reuses its batch
 * buffers as soon as a call returns). One thread writes it (the walk's, sHead) and one reads it (sTail). The walk is
 * at most SPLIT_FRAMES_AHEAD frames ahead of the backend. Textures: an upload, share or free is a record like a draw,
 * so it reaches the backend in order; but a texture cache entry is reused as soon as it is freed, and draws still
 * queued point at it, so a free first waits until the backend has caught up (gfx_split_drain): frees are rare (level
 * loads and the cache's eviction), draws are not.
 *
 * Frames the walk must see drawn at once (snapshots, traced frames, the reference renderer's) are made directly,
 * after a drain; so is everything while the switch is off.
 */
#include <string.h>
#include "port.h"
#include "plat/plat.h"
#include "gfx_backend.h"
#include "gfx_split.h"
#include "gfx_frame_parts.h"
#include "plat/profile.h"

/* The queue's size, a power of two (the ring's offsets wrap with the unsigned counters): 2 MB on a New 3DS, 1 MB on
 * an Old 3DS, where a crowded frame queues about 200 KB and the heap is tight. Set before the queue is allocated. */
static unsigned int sRingBytes = 2u * 1024u * 1024u;
#define SPLIT_RING_BYTES sRingBytes
/*
 * DKR_SPLIT_MASK (default 1): a position in the ring as head & (size - 1) instead of head % size. sRingBytes is a
 * variable (1 or 2 MB by console), so the compiler made every % a call to the ARM11's software divide
 * (__aeabi_uidivmod, ~30-50 cycles), once per record on the walk and twice on the backend
 * (proposals/3ds-arm11-c-tips-audit.md item 1; estimated -0.05..-0.25 ms Old 3DS game core). The ring is always a
 * power of two, so both give the same position. Read at each frame's start on the walk's thread; the backend uses
 * whichever it last saw, which is safe because the two agree.
 */
static unsigned int sRingMask;          // sRingBytes - 1
static volatile int sRingMaskOn;        // DKR_SPLIT_MASK
#define RING_AT(pos) (sRingMaskOn ? (pos) & sRingMask : (pos) % SPLIT_RING_BYTES)
#define SPLIT_FRAMES_AHEAD 2

enum {
    OP_START,
    OP_END,
    OP_CLEAR_COLOR,
    OP_CLEAR_DEPTH,
    OP_TRIANGLES,
    OP_INDEXED,
    OP_TEX_UPLOAD,
    OP_TEX_FREE,
    OP_TEX_SHARE,
    OP_MODEL,
    OP_MESHES,
    OP_MESH_FREE,
    OP_CALL, // v28: a function of the walk's run on the backend thread with the record's bytes (gfx_split_call_reserve)
    OP_INDEXED_REF, // P63 DKR_SPLIT_REF: an indexed draw whose arrays stay the walk's (gfx_split_draw_indexed_ref)
    OP_MODEL2, // P68 DKR_KART_BATCH bit 2: one model vertex array, two cull groups' indices (gfx_split_model2_begin)
    OP_WRAP // The rest of the ring up to its end is unused: the next record is at the start.
};

typedef struct SplitRecord {
    unsigned int op, bytes; // bytes: of the whole record, header included, a multiple of 8
} SplitRecord;

/* OP_START's payload: the walk's DKR_SPLIT_LINES settings for the backend, in step with the records. */
typedef struct SplitStart {
    int lines, check, reset; // reset: the check's counts start again with this record
    unsigned int publishBytes;
} SplitStart;

/* OP_END's payload: the frame's step, and (DKR_SPLIT_LINES_CHECK) the walk's record count and sum up to this record. */
typedef struct SplitEnd {
    int step;
    unsigned int records, sum;
} SplitEnd;

typedef struct SplitClear {
    short rect[4];
    unsigned char rgba[4];
} SplitClear;

typedef struct SplitDraw {
    GfxDrawState state;
    int numVerts, numTriangles; // OP_TRIANGLES: numVerts is 3 * numTriangles and there are no indices
} SplitDraw;

/* OP_INDEXED_REF: the arrays by pointer; with check set, followed by a copy of them (numVerts vertices, then the
 * indices) to compare them with (DKR_SPLIT_REF_CHECK). */
typedef struct SplitDrawRef {
    GfxDrawState state;
    const GfxVertex *verts;
    const unsigned short *indices;
    int numVerts, numTriangles;
    unsigned int ticket, check;
} SplitDrawRef;

/* P63: the last draw ticket given out (walk) and the last one the backend has drawn; the self-check's totals. */
static unsigned int sRefTicket;
static volatile unsigned int sRefDone, sRefCompared, sRefDiffer;

/* OP_MODEL: followed by numVerts model vertices and 3 * numTriangles indices. OP_MESHES: by numParts parts. */
typedef struct SplitModel {
    GfxDrawState state;
    GfxModelParams params;
    int numVerts, numTriangles;
} SplitModel;

/*
 * OP_MODEL2 (P68): followed by the params (all of them, or with full clear only the texture factors, texScale then
 * texOffset: the rest are the last OP_MODEL2's), then with check set the walk's whole params again (the self-check's
 * copy), then numVerts model vertices, then n[0] triangles' indices drawn with no culling and n[1] drawn with cull1.
 */
typedef struct SplitModel2 {
    GfxDrawState state;
    int numVerts;
    unsigned short n[2];
    unsigned char cull1, full, check, pad; // check: 1 compare, 2 compare and leave the texture factors stale (K6)
} SplitModel2;
_Static_assert(sizeof(SplitModel2) % 4 == 0 && sizeof(GfxModelParams) % 4 == 0 && sizeof(GfxModelVertex) == 16,
               "OP_MODEL2's parts stay 4-byte aligned");
#define MODEL2_TEX_OFFSET __builtin_offsetof(GfxModelParams, texScale)
#define MODEL2_TEX_BYTES (sizeof(float) * 8)
_Static_assert(__builtin_offsetof(GfxModelParams, texOffset) == __builtin_offsetof(GfxModelParams, texScale) + 16,
               "texScale then texOffset");

/* The walk's last OP_MODEL2 params of this frame (gfx_split_model2_begin) and the backend's (the record's replay). */
static GfxModelParams sWalkModel2Last, sBackendModel2Last;
static int sWalkModel2LastOk;
static void *sWalkModel2Open;
static int sWalkModel2BreakArmed = 1;
static volatile unsigned int sModel2Records, sModel2Slim, sModel2Compared, sModel2Wrong;

typedef struct SplitTexture {
    GfxTexture *tex;
    const GfxTexture *src;
    unsigned int ticket; // v33 OP_TEX_FREE from gfx_split_texture_free_later: done when sFreeDone reaches it
} SplitTexture;

/* v33: the last free ticket given out (walk) and the last one the backend has done. */
static unsigned int sFreeTicket;
static volatile unsigned int sFreeDone;

/* OP_CALL: followed by the caller's bytes. */
typedef struct SplitCall {
    void (*fn)(void *);
    void *pad;
} SplitCall;

/* v28 (DKR_NATIVE_REMOTE): the walk's work on this thread may keep a batch open across calls of sKeepFn; any other
 * record closes it first (sCloseFn), so it is drawn where the walk would have drawn it. */
static void (*sCloseFn)(void);
static void (*sKeepFn)(void *);

static unsigned char *sRing;
/*
 * P66 DKR_SPLIT_LINES (proposals/3ds-perf-guide-audit.md 1.1): the counters the two cores share sit on 32-byte cache
 * lines of their own (the ARM11's line), so a write by one core no longer takes the other's line away from it:
 * the walk's line holds what the walk writes and the backend reads (the head it hands over, its waiting flag), the
 * backend's line what the backend writes and the walk reads (the tail, its idle flag), and the walk's own counters
 * (its write position, its copy of the tail) a third line that the backend never touches. The layout is always so;
 * the switch changes how often each side writes and reads the other's line (see commit and backend_thread).
 */
typedef struct __attribute__((aligned(32))) {
    volatile unsigned int head; // Bytes handed over to the backend so far (the head and tail wrap together)
    volatile int walkWaiting;   // The walk waits on sSpace for room (or for the ring to empty)
} SplitWalkShared;
typedef struct __attribute__((aligned(32))) {
    volatile unsigned int tail; // Bytes the backend is done with
    volatile int backendIdle;   // The backend thread caught up and waits (or is about to) on sData
} SplitBackendShared;
typedef struct __attribute__((aligned(32))) {
    unsigned int head;       // Bytes written, handed over or not (sHead lags it while DKR_SPLIT_LINES is on)
    unsigned int cachedTail; // The tail when the walk last read it (DKR_SPLIT_LINES: read again only when short of room)
    unsigned int refBytes;   // P63's by-pointer bytes since the head was last handed over (count toward handing over)
    unsigned int records, sum; // DKR_SPLIT_LINES_CHECK: records written and a sum of their ops and sizes
} SplitWalkOwn;
static SplitWalkShared sWalkShared;
static SplitBackendShared sBackendShared;
static SplitWalkOwn sWalk;
#define sHead sWalkShared.head
#define sTail sBackendShared.tail
#define sWalkWaiting sWalkShared.walkWaiting
#define sBackendIdle sBackendShared.backendIdle
static int sLines;                // DKR_SPLIT_LINES (read at each frame's start on the walk's thread)
static unsigned int sPublishBytes; // DKR_SPLIT_PUBLISH_KB: unhanded bytes that make the walk hand the head over
static int sLinesCheck;           // DKR_SPLIT_LINES_CHECK: 1 the check, 2 and 3 broken on purpose (rule K6)
static int sLinesBreakArmed;      // The break (modes 2 and 3) happens once
static volatile unsigned int sLinesFrames, sLinesDiffer; // The backend's frames compared and those that differed
static unsigned int sLinesUnpublished, sLinesDifferSeen, sLinesLoggedFrame, sLinesFrame;
static PlatSem *sData;                     // A record (or a wake-up) for the backend thread (the owner's)
static PlatSem *sDataSems[2];               // Each backend thread's: [0] on the game's core, [1] on the system core
static volatile int sOwner;                 // Which backend thread takes the queue (gfx_split_frame_start)
static volatile int sParked[2];             // That thread saw it isn't the owner and waits to be one again
static int sMade[2];                        // That thread exists
static PlatSem *sSpace;                     // The backend thread moved on while the walk waited for it
static PlatSem *sFrameSlots;                // Frames the walk may still start ahead of the backend
static volatile int sStarted, sActive;      // The backend thread exists; this frame's calls are queued
static unsigned int sWakeBytes;             // Queued bytes that wake an idle backend thread (DKR_SPLIT_WAKE_KB)
static int sFailed;

/* The profiler's view (gfx_split_take_times): the walk's waits for the backend, the backend's busy time (frame start
 * to frame end, less its waits) and how often it was woken. Clocks are read only around waits, which block anyway. */
static unsigned long long sWalkWaitTotalNs, sWalkWaitTakenNs; // Never reset, for the draw mix (gfx_split_walk_waited_ns)
static unsigned long long sWalkWaitNs, sBackendBusyNs, sBackendFrameStart, sBackendWaitNs;
static unsigned int sBackendFrames, sBackendWakes;
static volatile unsigned long long sBackendBusyTotalNs; // Never reset (gfx_split_backend_busy_ns, the uncapped bench)

static void backend_thread(void *arg);

/*
 * P54 2b-A (DKR_BACKEND_MIX, default 1 while the profiler is on): the backend's time by record type, on timed frames
 * (an Old 3DS's 1 in 8, as the game sections; every frame elsewhere), to find where the renderer core's 22-27 ms a
 * frame go. Two clock reads a record on timed frames only. Totals never reset; each reader keeps where it last read.
 */
enum { MIX_PIECES, MIX_DRAWS, MIX_MODELS, MIX_TEXTURES, MIX_FRAME, MIX_CLOSE, MIX_OTHER, MIX_COUNT };
static const char *const sMixNames[MIX_COUNT] = { "native pieces", "draws", "model/mesh", "textures",
                                                  "frame start/end", "batch close", "other" };
static volatile unsigned long long sMixTicks[MIX_COUNT];
static volatile unsigned int sMixFrames;
static unsigned long long sMixReadTicks[2][MIX_COUNT];
static unsigned int sMixReadFrames[2], sMixCount;
static const char *const sMsplitNames[GFXB_MSPLIT_COUNT] = { "drain", "state", "settings twice", "uniforms", "copies",
                                                             "indices", "draw", "other" };
static unsigned long long sMsplitRead[2][GFXB_MSPLIT_COUNT];
static unsigned int sMsplitReadDraws[2], sMsplitReadReads[2];
static int sMixOn; // This frame is timed (decided at its OP_START)

static int mix_kind(unsigned int op, const void *p) {
    switch (op) {
        case OP_CALL:
            return ((const SplitCall *) p)->fn == sKeepFn ? MIX_PIECES : MIX_OTHER;
        case OP_CLEAR_COLOR:
        case OP_CLEAR_DEPTH:
        case OP_TRIANGLES:
        case OP_INDEXED:
        case OP_INDEXED_REF:
            return MIX_DRAWS;
        case OP_MODEL:
        case OP_MODEL2:
        case OP_MESHES:
        case OP_MESH_FREE:
            return MIX_MODELS;
        case OP_TEX_UPLOAD:
        case OP_TEX_FREE:
        case OP_TEX_SHARE:
            return MIX_TEXTURES;
        case OP_START:
        case OP_END:
            return MIX_FRAME;
        default:
            return MIX_OTHER;
    }
}

int gfx_split_backend_mix(int reader, char *out, int size) {
    double perTick = 1e3 / (double) plat_ticks_per_second(), ms[MIX_COUNT];
    unsigned int frames = sMixFrames - sMixReadFrames[reader], k;
    int at;
    for (k = 0; k < MIX_COUNT; k++) {
        unsigned long long now = sMixTicks[k];
        ms[k] = frames ? (now - sMixReadTicks[reader][k]) * perTick / frames : 0.0;
        sMixReadTicks[reader][k] = now;
    }
    sMixReadFrames[reader] += frames;
    if (frames == 0 || size <= 0) {
        if (size > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    at = plat_snprintf(out, size, "backend records a frame (ms):");
    for (k = 0; k < MIX_COUNT && at < size; k++) {
        at += plat_snprintf(out + at, size - at, "%s %s %.2f", k ? "," : "", sMixNames[k], ms[k]);
    }
    if (at < size) {
        at += plat_snprintf(out + at, size - at, " (%u frames timed)", frames);
    }
    {
        // P61 M3: the model and mesh draws' parts (gfx_c3d.c), less the clock reads that timed them.
        unsigned long long ticks[GFXB_MSPLIT_COUNT], readTicks;
        unsigned int draws, reads, k2;
        double msplit[GFXB_MSPLIT_COUNT], all = 0.0;
        gfxb_model_split_totals(ticks, &draws, &reads);
        readTicks = (unsigned long long) (reads - sMsplitReadReads[reader]) * prof_clock_read_cost() / 256;
        for (k2 = 0; k2 < GFXB_MSPLIT_COUNT; k2++) {
            msplit[k2] = (ticks[k2] - sMsplitRead[reader][k2]) * perTick;
            all += msplit[k2];
            sMsplitRead[reader][k2] = ticks[k2];
        }
        if (draws != sMsplitReadDraws[reader] && at < size) {
            // The clock reads' cost comes off each part by its share.
            double scale = all > 0.0 ? (all - readTicks * perTick) / all : 1.0;
            scale = scale > 0.0 ? scale : 0.0;
            at += plat_snprintf(out + at, size - at, "; backend model/mesh draws: %.1f a frame,",
                                (double) (draws - sMsplitReadDraws[reader]) / frames);
            for (k2 = 0; k2 < GFXB_MSPLIT_COUNT && at < size; k2++) {
                at += plat_snprintf(out + at, size - at, "%s %s %.2f", k2 ? "," : "", sMsplitNames[k2],
                                    msplit[k2] * scale / frames);
            }
            if (at < size) {
                at += plat_snprintf(out + at, size - at, " ms");
            }
        }
        sMsplitReadDraws[reader] = draws;
        sMsplitReadReads[reader] = reads;
    }
    return at < size ? at : size - 1;
}
/*
 * P57 E: the frame start/end bucket's parts (gfx_backend.h GFXB_FPART_*), ms a frame on the timed frames since this
 * reader last read, the clock reads' cost taken off each part by its share; and the self-check (gfx_frame_parts.h):
 * the parts' sum against the bucket's own time on the same frames (DKR_FRAME_PARTS_CHECK: 1 on, 2 broken on purpose
 * by leaving the largest part out, rule K6; 0 off). Returns the length written, 0 if no frame was timed.
 */
static const char *const sFpartNames[GFXB_FPART_COUNT] = {
    "capture/screen", "start GPU wait", "set-up", "texture promote", "start rest", "end tallies", "vertex flush",
    "logs", "end GPU wait", "fills", "command lists", "present", "screen watch", "C3D_FrameEnd", "after"};
static unsigned long long sFpartRead[2][GFXB_FPART_COUNT], sFpartBucketRead[2];
static unsigned int sFpartReadFrames[2], sFpartReadReads[2], sFpartReadLists[2];

int gfx_split_frame_parts(int reader, char *out, int size) {
    unsigned long long ticks[GFXB_FPART_COUNT], delta[GFXB_FPART_COUNT], bucket, all = 0, readTicks;
    unsigned int frames, reads, lists, k;
    double perTick = 1e3 / (double) plat_ticks_per_second(), scale;
    long long outside;
    int at, check, agree, drop = -1;
    gfxb_frame_parts_totals(ticks, &frames, &reads, &lists);
    bucket = sMixTicks[MIX_FRAME] - sFpartBucketRead[reader];
    sFpartBucketRead[reader] = sMixTicks[MIX_FRAME];
    for (k = 0; k < GFXB_FPART_COUNT; k++) {
        delta[k] = ticks[k] - sFpartRead[reader][k];
        all += delta[k];
        sFpartRead[reader][k] = ticks[k];
    }
    readTicks = (unsigned long long) (reads - sFpartReadReads[reader]) * prof_clock_read_cost() / 256;
    lists -= sFpartReadLists[reader];
    frames -= sFpartReadFrames[reader];
    sFpartReadFrames[reader] += frames;
    sFpartReadReads[reader] = reads;
    sFpartReadLists[reader] += lists;
    if (size <= 0) {
        return 0;
    }
    out[0] = '\0';
    if (frames == 0) {
        return 0;
    }
    scale = all > 0 ? (double) (all > readTicks ? all - readTicks : 0) / (double) all : 1.0;
    at = plat_snprintf(out, size, "backend frame start/end (ms a frame):");
    for (k = 0; k < GFXB_FPART_COUNT && at < size; k++) {
        at += plat_snprintf(out + at, size - at, "%s %s %.2f", k ? "," : "", sFpartNames[k],
                            delta[k] * scale * perTick / frames);
    }
    check = plat_switch("DKR_FRAME_PARTS_CHECK", 1);
    if (check != 0 && at < size) {
        if (check == 2) {
            drop = gfx_frame_parts_largest(delta, GFXB_FPART_COUNT); // Broken on purpose (rule K6)
        }
        agree = gfx_frame_parts_agree(bucket, delta, GFXB_FPART_COUNT, drop, &outside);
        if (reader == 1) {
            plat_check_report("frame parts", !agree); // Once a race (the core window): the results' chk columns
        }
        at += plat_snprintf(out + at, size - at, "; %.1f list parts; check%s: %.2f of %.2f ms outside the parts, %s",
                            (double) lists / frames, drop >= 0 ? " (broken on purpose)" : "",
                            outside * perTick / frames, bucket * perTick / frames, agree ? "ok" : "WRONG");
    }
    if (at < size) {
        at += plat_snprintf(out + at, size - at, " (%u frames timed)", frames);
    }
    return at < size ? at : size - 1;
}

static int sBackendPrefetch; // v31 DKR_BACKEND_PREFETCH (read at each frame's start on the backend thread)

static inline unsigned int round8(unsigned int n) {
    return (n + 7u) & ~7u;
}

/* DKR_SPLIT_LINES_CHECK: the record sum both sides keep (an op and a size per record). */
static inline unsigned int lines_sum(unsigned int sum, unsigned int op, unsigned int bytes) {
    return sum * 31u + (op << 24) + bytes;
}

/*
 * Hands what the walk has written over to the backend thread: the record bytes reach memory before the head that
 * covers them, and the head before the walk looks at the backend's idle flag (the backend sets the flag before it
 * looks at the head, so one of them sees the other).
 */
static inline void head_publish(void) {
    __sync_synchronize(); // The records are written before the head moves past them.
    sHead = sWalk.head;
    sWalk.refBytes = 0;
    __sync_synchronize();
}

/*
 * Waking the backend is a system call on both cores, and the backend, being quicker, would sleep and be woken for
 * nearly every draw; so an idle backend is woken only once sWakeBytes are queued, or at the frame's end
 * (DKR_SPLIT_WAKE_KB=0 = every record, as before).
 */
/* P63: bytes the walk's OP_INDEXED_REF records hand over by pointer since the backend was last woken, counted as
 * queued for the wake-up (a by-pointer draw would otherwise leave the backend asleep, and the walk's blocks unreturned,
 * until 16 KB of other records came). */
static unsigned int sRefWakeBytes;

static inline void wake_backend(unsigned int op) {
    if (sBackendIdle && (op == OP_END || sHead - sTail + sRefWakeBytes >= sWakeBytes)) {
        sBackendIdle = 0;
        sRefWakeBytes = 0;
        plat_sem_post(sData);
    }
}

/* Waits until the ring has room for bytes more (SPLIT_RING_BYTES: until it is empty). */
static void wait_for_room(unsigned int bytes) {
    if (sLines && SPLIT_RING_BYTES - (sWalk.head - sWalk.cachedTail) >= bytes) {
        return; // DKR_SPLIT_LINES: room by the tail last read, without reading the backend's line
    }
    sWalk.cachedTail = sTail;
    if (SPLIT_RING_BYTES - (sWalk.head - sWalk.cachedTail) >= bytes) {
        return;
    }
    if (sHead != sWalk.head) {
        // The backend can only make room out of what it was given: everything goes over before the walk waits.
        head_publish();
        wake_backend(OP_END);
    }
    while (SPLIT_RING_BYTES - (sWalk.head - sTail) < bytes) {
        sWalkWaiting = 1;
        __sync_synchronize();
        if (SPLIT_RING_BYTES - (sWalk.head - sTail) >= bytes) {
            sWalkWaiting = 0;
            break;
        }
        plat_sem_post(sData); // Makes sure the backend thread looks at the flag after this
        {
            unsigned long long start = plat_time_ns();
            plat_sem_wait(sSpace);
            sWalkWaitNs += plat_time_ns() - start;
            sWalkWaitTotalNs = sWalkWaitNs + sWalkWaitTakenNs;
        }
    }
    sWalk.cachedTail = sTail;
}

/* Room for a record of payload bytes, at a position that doesn't wrap; its payload. */
static void *reserve(unsigned int op, unsigned int payload) {
    unsigned int bytes = sizeof(SplitRecord) + round8(payload);
    unsigned int at = RING_AT(sWalk.head);
    SplitRecord *rec;
    if (at + bytes > SPLIT_RING_BYTES) {
        unsigned int rest = SPLIT_RING_BYTES - at;
        wait_for_room(rest);
        rec = (SplitRecord *) (sRing + at);
        rec->op = OP_WRAP;
        rec->bytes = rest;
        sWalk.head += rest;
        if (sLinesCheck) {
            sWalk.records++;
            sWalk.sum = lines_sum(sWalk.sum, OP_WRAP, rest);
        }
        if (!sLines) {
            __sync_synchronize();
            sHead = sWalk.head; // (DKR_SPLIT_LINES: handed over with the records after it)
        }
        at = 0;
    }
    wait_for_room(bytes);
    rec = (SplitRecord *) (sRing + at);
    rec->op = op;
    rec->bytes = bytes;
    return rec + 1;
}

/*
 * Hands the record reserve gave over to the backend thread. With DKR_SPLIT_LINES the head is handed over (a barrier,
 * the store to the walk's shared line and a look at the backend's) only once DKR_SPLIT_PUBLISH_KB are written, at the
 * frame's end, before the walk waits for room (wait_for_room) and on a drain or kick; else for every record, as before.
 */
static void commit(void *payload) {
    SplitRecord *rec = (SplitRecord *) payload - 1;
    unsigned int op = rec->op;
    if (sLinesCheck) {
        if (op == OP_END) {
            SplitEnd *e = payload;
            e->records = sWalk.records; // The records before this one
            e->sum = sWalk.sum;
        }
        sWalk.records++;
        sWalk.sum = lines_sum(sWalk.sum, op, rec->bytes);
        if (sLinesBreakArmed && sLinesCheck == 2 && (op == OP_TRIANGLES || op == OP_INDEXED)) {
            // Rule K6's break (mode 2): a draw counted but lost, once (the next record is written over it).
            sLinesBreakArmed = 0;
            return;
        }
    }
    sWalk.head += rec->bytes;
    if (sLines && op != OP_END && sWalk.head - sHead + sWalk.refBytes < sPublishBytes) {
        return;
    }
    if (sLines && op == OP_END && sLinesBreakArmed && sLinesCheck == 3) {
        sLinesBreakArmed = 0; // Rule K6's break (mode 3): a frame's end not handed over, once
        return;
    }
    head_publish();
    wake_backend(op);
}

/* OP_MODEL2 on the backend thread: the params made whole again from the last record's, then one model draw for each
 * cull group with triangles (as gfx_split_draw_model would have made them, the vertices sent with each). */
static void model2_replay(const void *payload) {
    const SplitModel2 *m = payload;
    const unsigned char *at = (const unsigned char *) (m + 1);
    const GfxModelVertex *verts;
    const unsigned short *idx;
    GfxDrawState state = m->state;
    if (m->full) {
        memcpy(&sBackendModel2Last, at, sizeof(GfxModelParams));
        at += sizeof(GfxModelParams);
    } else {
        if (m->check != 2) { // (2: rule K6's break, the texture factors left as the last record's)
            memcpy((unsigned char *) &sBackendModel2Last + MODEL2_TEX_OFFSET, at, MODEL2_TEX_BYTES);
        }
        at += MODEL2_TEX_BYTES;
    }
    if (m->check) {
        int wrong = __builtin_memcmp(&sBackendModel2Last, at, sizeof(GfxModelParams)) != 0;
        sModel2Wrong += (unsigned int) wrong;
        sModel2Compared++;
        plat_check_report("kart batch params", wrong); /* the race's checks_run / checks_wrong (CSV "chk" cells) */
        at += sizeof(GfxModelParams);
    }
    verts = (const GfxModelVertex *) at;
    idx = (const unsigned short *) (verts + m->numVerts);
    if (m->n[0] > 0) {
        state.cullMode = 0;
        gfxb_draw_model(&state, &sBackendModel2Last, verts, m->numVerts, idx, m->n[0]);
    }
    if (m->n[1] > 0) {
        state.cullMode = m->cull1;
        gfxb_draw_model(&state, &sBackendModel2Last, verts, m->numVerts, idx + 3 * m->n[0], m->n[1]);
    }
}

/* The backend's DKR_SPLIT_LINES settings (from the OP_START records) and the check's records run and their sum: shared
 * by both backend threads, as the queue (and the counting) is handed from one to the other (choose_backend). */
static int sBackendLines, sBackendCheck;
static unsigned int sBackendPublishBytes, sBackendRecords, sBackendSum;

static void backend_thread(void *arg) {
    int self = (int) (long) arg;
    // P66 DKR_SPLIT_LINES (as the walk's OP_START records set it): the backend reads the walk's head again only when it
    // has run everything up to the copy it last read, and hands its tail back once DKR_SPLIT_PUBLISH_KB are done, at a
    // frame's end, when the walk waits, and before it reads the head again or sleeps; else both for every record.
    for (;;) {
        unsigned int tail, head;
        if (sOwner != self) {
            // Another backend thread has the queue (it was handed over with the queue empty): wait to have it again.
            sParked[self] = 1;
            __sync_synchronize();
            plat_sem_wait(sDataSems[self]);
            sParked[self] = 0;
            continue;
        }
        sBackendIdle = 1;
        __sync_synchronize(); // The flag is set before the head is looked at (see commit).
        if (sTail == sHead) {
            unsigned long long start = plat_time_ns();
            plat_sem_wait(sDataSems[self]);
            sBackendWaitNs += plat_time_ns() - start;
            sBackendWakes++;
        }
        sBackendIdle = 0;
        tail = sTail; // (the tail is handed back before the backend sleeps or reads the head again)
        while (tail != (head = sHead)) {
          __sync_synchronize(); // The records are read after the head that covers them.
          do {
            SplitRecord *rec;
            unsigned int op, bytes;
            unsigned long long mixStart;
            int mixKind = MIX_OTHER;
            void *p;
            rec = (SplitRecord *) (sRing + RING_AT(tail));
            op = rec->op;
            bytes = rec->bytes;
            p = rec + 1;
            if (op == OP_START) {
                sMixOn = plat_switch("DKR_BACKEND_MIX", 1) && prof_enabled() &&
                         (!plat_old_3ds() || ++sMixCount % 8 == 0);
                gGfxbModelSplitOn = sMixOn; // P61 M3: the model draws' parts too
                gGfxbFramePartsOn = sMixOn; // P57 E: the frame start and end's parts
            }
            mixStart = sMixOn ? plat_ticks() : 0;
            if (sBackendPrefetch && tail + bytes != head) {
                // v31 (DKR_BACKEND_PREFETCH): the next record's first 256 bytes (a native piece's header: state,
                // matrices, axes) start coming over from the walk's core under this record's work. A hint only.
                const unsigned char *next = (const unsigned char *) sRing + RING_AT(tail + bytes);
                int k;
                for (k = 0; k < 256; k += 32) {
                    __builtin_prefetch(next + k);
                }
            }
            if (sCloseFn != NULL && (op != OP_CALL || ((SplitCall *) p)->fn != sKeepFn) && op != OP_WRAP) {
                sCloseFn();
                if (mixStart != 0) {
                    unsigned long long now = plat_ticks();
                    sMixTicks[MIX_CLOSE] += now - mixStart;
                    mixStart = now;
                }
            }
            if (mixStart != 0) {
                mixKind = mix_kind(op, p); // (before the record runs: an OP_CALL's payload is the walk's after)
            }
            switch (op) {
                case OP_START: {
                    const SplitStart *st = p;
                    sBackendLines = st->lines;
                    sBackendCheck = st->check;
                    sBackendPublishBytes = st->publishBytes;
                    if (st->reset) {
                        sBackendRecords = 0;
                        sBackendSum = 0;
                    }
                }
                    sBackendPrefetch = plat_switch("DKR_BACKEND_PREFETCH", plat_old_3ds()) != 0;
                    sBackendFrameStart = plat_time_ns();
                    sBackendWaitNs = 0;
                    gfxb_start_frame();
                    break;
                case OP_END: {
                    unsigned long long now;
                    const SplitEnd *e = p;
                    if (sBackendCheck) {
                        if (e->records != sBackendRecords || e->sum != sBackendSum) {
                            sLinesDiffer++;
                            sBackendRecords = e->records; // In step again, so one loss counts once
                            sBackendSum = e->sum;
                        }
                        sLinesFrames++;
                    }
                    plat_set_frame_step(((SplitEnd *) p)->step);
                    gfxb_end_frame();
                    now = plat_time_ns();
                    sBackendBusyNs += now - sBackendFrameStart - sBackendWaitNs;
                    sBackendBusyTotalNs += now - sBackendFrameStart - sBackendWaitNs;
                    sBackendFrames++;
                    break;
                }
                case OP_CLEAR_COLOR: {
                    SplitClear *c = p;
                    gfxb_clear_color(c->rect, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                    break;
                }
                case OP_CLEAR_DEPTH:
                    gfxb_clear_depth(((SplitClear *) p)->rect);
                    break;
                case OP_TRIANGLES:
                case OP_INDEXED: {
                    SplitDraw *d = p;
                    const GfxVertex *verts = (const GfxVertex *) (d + 1);
                    if (op == OP_TRIANGLES) {
                        gfxb_draw_triangles(&d->state, verts, d->numTriangles);
                    } else {
                        gfxb_draw_indexed(&d->state, verts, d->numVerts, (const unsigned short *) (verts + d->numVerts),
                                          d->numTriangles);
                    }
                    break;
                }
                case OP_INDEXED_REF: {
                    SplitDrawRef *r = p;
                    const GfxVertex *verts = r->verts;
                    const unsigned short *indices = r->indices;
                    if (r->check) {
                        // The walk must not have touched the arrays since it queued them (a block reused too early).
                        // A difference is counted and the queued copy drawn instead, so a broken pool shows in the
                        // counts rather than as a crash on a reused block's indices.
                        const GfxVertex *copy = (const GfxVertex *) (r + 1);
                        const unsigned short *copyIndices = (const unsigned short *) (copy + r->numVerts);
                        if (__builtin_memcmp(copy, verts, (size_t) r->numVerts * sizeof(GfxVertex)) != 0 ||
                            __builtin_memcmp(copyIndices, indices, (size_t) r->numTriangles * 3u * sizeof(short)) != 0) {
                            sRefDiffer++;
                            verts = copy;
                            indices = copyIndices;
                        }
                        sRefCompared++;
                    }
                    gfxb_draw_indexed(&r->state, verts, r->numVerts, indices, r->numTriangles);
                    // Only now may the walk reuse the arrays: the backend has made its own copy (the GPU's VBO).
                    __atomic_store_n(&sRefDone, r->ticket, __ATOMIC_RELEASE);
                    break;
                }
                case OP_MODEL: {
                    SplitModel *m = p;
                    const GfxModelVertex *verts = (const GfxModelVertex *) (m + 1);
                    gfxb_draw_model(&m->state, &m->params, verts, m->numVerts,
                                    (const unsigned short *) (verts + m->numVerts), m->numTriangles);
                    break;
                }
                case OP_MODEL2:
                    model2_replay(p);
                    break;
                case OP_MESHES: {
                    SplitModel *m = p;
                    gfxb_draw_meshes(&m->state, &m->params, (const GfxMeshPart *) (m + 1), m->numVerts);
                    break;
                }
                case OP_MESH_FREE:
                    gfxb_mesh_free(((SplitTexture *) p)->tex);
                    break;
                case OP_CALL:
                    ((SplitCall *) p)->fn((SplitCall *) p + 1);
                    break;
                case OP_TEX_UPLOAD:
                    gfxb_texture_upload(((SplitTexture *) p)->tex);
                    break;
                case OP_TEX_FREE:
                    gfxb_texture_free(((SplitTexture *) p)->tex);
                    if (((SplitTexture *) p)->ticket != 0) {
                        __atomic_store_n(&sFreeDone, ((SplitTexture *) p)->ticket, __ATOMIC_RELEASE);
                    }
                    break;
                case OP_TEX_SHARE:
                    gfxb_texture_share(((SplitTexture *) p)->tex, ((SplitTexture *) p)->src);
                    break;
                default:
                    break;
            }
            if (mixStart != 0) {
                sMixTicks[mixKind] += plat_ticks() - mixStart;
                if (op == OP_END) {
                    sMixFrames++;
                    sMixOn = 0;
                    gGfxbFramePartsOn = 0;
                }
            }
            if (sBackendCheck) {
                sBackendRecords++;
                sBackendSum = lines_sum(sBackendSum, op, bytes);
            }
            tail += bytes;
            if (!sBackendLines || tail == head || op == OP_END || tail - sTail >= sBackendPublishBytes || sWalkWaiting) {
                // Done with the records before the walk may write over them (op and bytes were read before, as locals).
                __sync_synchronize();
                sTail = tail;
            }
            if (op == OP_END) {
                plat_sem_post(sFrameSlots);
            }
            if (sWalkWaiting) {
                sWalkWaiting = 0;
                plat_sem_post(sSpace);
            }
          } while (sBackendLines && tail != head);
        }
        if (sWalkWaiting) {
            sWalkWaiting = 0;
            plat_sem_post(sSpace);
        }
    }
}

/* Starts the backend thread the first time the switch is on; FALSE if it can't (then everything stays direct). */
static int start_backend(void) {
    if (sStarted) {
        return TRUE;
    }
    if (sFailed) {
        return FALSE;
    }
    sRingBytes = plat_old_3ds() ? 1024u * 1024u : 2u * 1024u * 1024u;
    sRingMask = sRingBytes - 1u; // Used only while sRingBytes is a power of two (split_frame_start)
    sRing = plat_malloc(SPLIT_RING_BYTES);
    if (sRing == NULL) {
        plat_log("two-core renderer: no memory for its queue");
        sFailed = TRUE;
        return FALSE;
    }
    sDataSems[0] = plat_sem_create(0);
    sDataSems[1] = plat_sem_create(0);
    sData = sDataSems[0];
    sOwner = 0;
    sSpace = plat_sem_create(0);
    sFrameSlots = plat_sem_create(SPLIT_FRAMES_AHEAD);
    plat_thread_create(backend_thread, (void *) 0L, 0, PLAT_THREAD_RCP2, "rdp2");
    sMade[0] = TRUE;
    sStarted = TRUE;
    return TRUE;
}

/*
 * A New 3DS can also run the backend on the system core (DKR_SPLIT_SYSCORE=1), with its share raised, instead of on
 * the game's core, which the game and the backend together fill in a busy race. Each core has a thread of its own;
 * the queue is handed over empty, at a frame's start.
 */
static void choose_backend(void) {
    int want = plat_split_syscore_possible() && plat_switch("DKR_SPLIT_SYSCORE", 0) ? 1 : 0;
    int old = sOwner;
    if (want == old) {
        return;
    }
    if (want == 1 && !plat_split_syscore_share(TRUE)) {
        return;
    }
    gfx_split_drain();
    sOwner = want;
    __sync_synchronize();
    plat_sem_post(sDataSems[old]); // So it sees it isn't the owner, if it waits for records
    while (!sParked[old]) {
        plat_sleep_ns(1000000ULL);
    }
    sData = sDataSems[want];
    if (!sMade[want]) {
        plat_thread_create(backend_thread, (void *) (long) want, 0, want ? PLAT_THREAD_RCP2_SYS : PLAT_THREAD_RCP2,
                           want ? "rdp2 system core" : "rdp2");
        sMade[want] = TRUE;
    } else {
        plat_sem_post(sDataSems[want]);
    }
    if (want == 0) {
        plat_split_syscore_share(FALSE);
    }
    plat_log("two-core renderer: its second thread on the %s core", want ? "system" : "game's");
}

static void split_noop(void *p) {
    (void) p;
}

/* P57 A1: drains (each a cross-core wait for the backend) and their wait, counted for the stall finder. */
static unsigned int sDrains;
static unsigned long long sDrainNs;

void gfx_split_drain_counts(unsigned int *drains, unsigned long long *ns) {
    *drains = sDrains;
    *ns = sDrainNs;
}

void gfx_split_drain(void) {
    if (sActive && sCloseFn != NULL) {
        // A record that isn't sKeepFn's, so a batch the backend holds open is drawn before the queue counts as empty.
        SplitCall *c = reserve(OP_CALL, sizeof(SplitCall));
        c->fn = split_noop;
        commit(c);
    }
    sDrains++;
    if (sStarted && sWalk.head != sTail) {
        unsigned long long start = plat_time_ns();
        wait_for_room(SPLIT_RING_BYTES);
        sDrainNs += plat_time_ns() - start;
    }
}

/* P53: between frames (a level load's texture uploads), wakes an idle backend thread for what is queued, which
 * commit would otherwise leave until 16 KB or a frame's end are queued. Records outside a frame are run in order. */
void gfx_split_kick(void) {
    if (sStarted && sHead != sWalk.head) {
        head_publish(); // DKR_SPLIT_LINES: what the walk wrote and hasn't handed over yet
    }
    if (sStarted && sBackendIdle && sHead != sTail) {
        sBackendIdle = 0;
        plat_sem_post(sData);
    }
}

static int sCoresHeld; // A solo frame (holdCores) keeps the system core's share raised

/*
 * P66 DKR_SPLIT_LINES at each queued frame's start (the walk's thread; the backend reads its own copy at OP_START):
 * the switches, and DKR_SPLIT_LINES_CHECK's result for the race's checks (chk cell "split lines") and a log line every
 * 600 frames. The check: the backend counts the records it runs and sums their ops and sizes, and at each OP_END
 * compares them with the walk's, which the record carries (a lost, torn or doubled record differs); and the walk
 * counts frame ends it left without handing over (the backend would draw that frame late).
 */
static int sLinesReset;

static void lines_frame_start(void) {
    int check = plat_switch("DKR_SPLIT_LINES_CHECK", 0);
    unsigned int differ;
    if (check != sLinesCheck) {
        sLinesBreakArmed = check >= 2;
        sLinesReset = check != 0 && sLinesCheck == 0; // Both sides start counting at this frame's OP_START
        sLinesCheck = check;
    }
    sLines = check != 0 || plat_switch("DKR_SPLIT_LINES", 0) != 0;
    sRingMaskOn = (sRingBytes & (sRingBytes - 1u)) == 0 && plat_switch("DKR_SPLIT_MASK", 1) != 0;
    sPublishBytes = (unsigned int) plat_switch("DKR_SPLIT_PUBLISH_KB", 2) * 1024u;
    if (!sLinesCheck) {
        return;
    }
    differ = sLinesDiffer;
    plat_check_report("split lines", differ + sLinesUnpublished != sLinesDifferSeen);
    if (differ + sLinesUnpublished != 0 && sLinesDifferSeen == 0) {
        plat_log("split lines: first difference by frame %u (%u frames differ, %u frame ends not handed over)",
                 sLinesFrame, differ, sLinesUnpublished);
    }
    sLinesDifferSeen = differ + sLinesUnpublished;
    if (sLinesFrame++ - sLinesLoggedFrame >= 600) {
        sLinesLoggedFrame = sLinesFrame;
        plat_log("split lines: %u frames compared, %u differ, %u frame ends not handed over (publish every %u KB)",
                 sLinesFrames, differ, sLinesUnpublished, sPublishBytes / 1024u);
    }
}

int gfx_split_frame_start(int direct, int holdCores) {
    int want = !direct && plat_switch("DKR_SPLIT_RENDER", 1) != 0;
    if (holdCores) {
        // R5 placement 2: this frame runs whole on the system core. The queue empties first (a frame queued before
        // may still be drawing on the second thread), then the share stays (refreshed, as DKR_SPLIT_SHARE may change).
        gfx_split_drain();
        sActive = FALSE;
        sCoresHeld = TRUE;
        plat_split_render_cores(TRUE);
        gfxb_start_frame();
        return FALSE;
    }
    if (want && !sActive) {
        // The other core's share comes first: without it the second thread can't be made there.
        plat_split_render_cores(TRUE);
        want = start_backend();
        if (!want) {
            plat_split_render_cores(FALSE);
        }
    }
    if (!want) {
        gfx_split_drain();
        if (sActive || sCoresHeld) {
            plat_split_render_cores(FALSE);
        }
        sActive = FALSE;
        sCoresHeld = FALSE;
        gfxb_start_frame();
        return FALSE;
    }
    if (!sActive) {
        plat_log("two-core renderer: on (the walk and the GPU commands on separate cores)");
    } else {
        plat_split_render_cores(TRUE); // Follows a change of the system core's share (the switch bench's)
    }
    sActive = TRUE;
    sCoresHeld = FALSE;
    choose_backend();
    plat_split_backend_priority();
    sWakeBytes = (unsigned int) plat_switch("DKR_SPLIT_WAKE_KB", 16) * 1024u;
    lines_frame_start();
    if (!plat_sem_try_wait(sFrameSlots)) {
        unsigned long long start = plat_time_ns();
        plat_sem_wait(sFrameSlots);
        sWalkWaitNs += plat_time_ns() - start;
            sWalkWaitTotalNs = sWalkWaitNs + sWalkWaitTakenNs;
    }
    sWalkModel2LastOk = 0; // P68: each frame's first OP_MODEL2 carries all its params
    {
        SplitStart *st = reserve(OP_START, sizeof(SplitStart));
        st->lines = sLines;
        st->check = sLinesCheck != 0;
        st->reset = sLinesReset;
        st->publishBytes = sPublishBytes;
        if (sLinesReset) {
            sWalk.records = 0; // (after a wrap reserve may have written, before this record is counted)
            sWalk.sum = 0;
            sLinesReset = 0;
        }
        commit(st);
    }
    gfxb_walk_frame_start(); // DKR_WALK_VERTS: the walk's copy of the frame's settings (after the start record)
    return TRUE;
}

void gfx_split_frame_end(void) {
    SplitEnd *e;
    if (!sActive) {
        gfxb_end_frame();
        return;
    }
    e = reserve(OP_END, sizeof(SplitEnd));
    e->step = plat_frame_step();
    commit(e);
    if (sLinesCheck && sHead != sWalk.head) {
        sLinesUnpublished++; // DKR_SPLIT_LINES_CHECK: the backend would not see this frame's end until more came
    }
}

int gfx_split_active(void) {
    return sActive;
}

unsigned long long gfx_split_backend_busy_ns(void) {
    return sBackendBusyTotalNs;
}

unsigned long long gfx_split_walk_waited_ns(void) {
    return sWalkWaitTotalNs;
}

void gfx_split_take_times(unsigned long long *walkWaitNs, unsigned long long *backendBusyNs,
                          unsigned int *backendFrames, unsigned int *backendWakes) {
    *walkWaitNs = sWalkWaitNs;
    *backendBusyNs = sBackendBusyNs;
    *backendFrames = sBackendFrames;
    *backendWakes = sBackendWakes;
    sWalkWaitTakenNs += sWalkWaitNs;
    sWalkWaitNs = sBackendBusyNs = 0; // Read without a lock: a frame caught mid-update is off by a little
    sBackendFrames = sBackendWakes = 0;
}

void gfx_split_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    SplitClear *c;
    if (!sActive) {
        gfxb_clear_color(rect, r, g, b, a);
        return;
    }
    c = reserve(OP_CLEAR_COLOR, sizeof(SplitClear));
    memcpy(c->rect, rect, sizeof(c->rect));
    c->rgba[0] = r;
    c->rgba[1] = g;
    c->rgba[2] = b;
    c->rgba[3] = a;
    commit(c);
}

void gfx_split_clear_depth(const short rect[4]) {
    SplitClear *c;
    if (!sActive) {
        gfxb_clear_depth(rect);
        return;
    }
    c = reserve(OP_CLEAR_DEPTH, sizeof(SplitClear));
    memcpy(c->rect, rect, sizeof(c->rect));
    commit(c);
}

void gfx_split_draw_triangles(const GfxDrawState *state, const GfxVertex *verts, int numTriangles) {
    SplitDraw *d;
    unsigned int vertBytes = (unsigned int) numTriangles * 3u * sizeof(GfxVertex);
    if (!sActive || sizeof(SplitDraw) + vertBytes + 64u > SPLIT_RING_BYTES / 4u) {
        gfx_split_drain();
        gfxb_draw_triangles(state, verts, numTriangles);
        return;
    }
    d = reserve(OP_TRIANGLES, sizeof(SplitDraw) + vertBytes);
    d->state = *state;
    d->numVerts = numTriangles * 3;
    d->numTriangles = numTriangles;
    memcpy(d + 1, verts, vertBytes);
    commit(d);
}

/* Out of line (P61 M2): with the walk's step marks around its two callers, LTO otherwise copied it into both. */
PLAT_HOT_GAME PLAT_NOINLINE void gfx_split_draw_indexed(const GfxDrawState *state, const GfxVertex *verts,
                                                        int numVerts, const unsigned short *indices, int numTriangles) {
    SplitDraw *d;
    unsigned int vertBytes = (unsigned int) numVerts * sizeof(GfxVertex);
    unsigned int indexBytes = (unsigned int) numTriangles * 3u * sizeof(unsigned short);
    if (!sActive || sizeof(SplitDraw) + vertBytes + indexBytes + 64u > SPLIT_RING_BYTES / 4u) {
        gfx_split_drain();
        gfxb_draw_indexed(state, verts, numVerts, indices, numTriangles);
        return;
    }
    if (gfxb_walk_draw_indexed(state, verts, numVerts, indices, numTriangles)) {
        return; // DKR_WALK_VERTS: the walk made the GPU's vertices and queued them (a call record)
    }
    d = reserve(OP_INDEXED, sizeof(SplitDraw) + vertBytes + indexBytes);
    d->state = *state;
    d->numVerts = numVerts;
    d->numTriangles = numTriangles;
    memcpy(d + 1, verts, vertBytes);
    memcpy((unsigned char *) (d + 1) + vertBytes, indices, indexBytes);
    commit(d);
}

/* P60 D (DKR_SPRITE_GS): a batch of sprite points, as a call of the backend's in the queue, or expanded here into an
 * indexed draw for a backend that doesn't take points (the host's). */
typedef struct SplitSprites {
    GfxDrawState state;
    int numPoints, pad;
} SplitSprites;

static void sprites_call(void *payload) {
    const SplitSprites *s = (const SplitSprites *) payload;
    gfxb_draw_sprites(&s->state, (const GfxSpritePoint *) (s + 1), s->numPoints);
}

void gfx_split_draw_sprites(const GfxDrawState *state, const GfxSpritePoint *pts, int numPoints) {
    static GfxVertex sVerts[GFX_MAX_SPRITE_POINTS * 4];
    static unsigned short sIndices[GFX_MAX_SPRITE_POINTS * 6];
    SplitSprites *s;
    if (numPoints <= 0) {
        return;
    }
    if (numPoints > GFX_MAX_SPRITE_POINTS) {
        numPoints = GFX_MAX_SPRITE_POINTS;
    }
    if (!gfxb_sprite_points()) {
        gfx_sprite_expand(pts, numPoints, sVerts, sIndices);
        gfx_split_draw_indexed(state, sVerts, numPoints * 4, sIndices, numPoints * 2);
        return;
    }
    s = gfx_split_call_reserve(sprites_call, sizeof(SplitSprites) + (unsigned int) numPoints * sizeof(GfxSpritePoint));
    if (s == NULL) {
        gfx_split_drain();
        gfxb_draw_sprites(state, pts, numPoints);
        return;
    }
    s->state = *state;
    s->numPoints = numPoints;
    s->pad = 0;
    memcpy(s + 1, pts, (unsigned int) numPoints * sizeof(GfxSpritePoint));
    gfx_split_call_commit(s);
}

/* P65 rank 1 (DKR_SPRITE_TABLE): a sprite table batch, as a call of the backend's in the queue (its groups, then its
 * vertices), or expanded here into an indexed draw for a backend that doesn't take tables (the host's). */
typedef struct SplitTable {
    GfxDrawState state;
    float fogMul, fogOffset;
    int numGroups, numVerts;
} SplitTable;

static void table_call(void *payload) {
    const SplitTable *s = (const SplitTable *) payload;
    const GfxTabGroup *groups = (const GfxTabGroup *) (s + 1);
    gfxb_draw_sprite_table(&s->state, groups, s->numGroups, s->fogMul, s->fogOffset,
                           (const GfxTabVertex *) (groups + s->numGroups), s->numVerts);
}

void gfx_split_draw_sprite_table(const GfxDrawState *state, const GfxTabGroup *groups, int numGroups, float fogMul,
                                 float fogOffset, const GfxTabVertex *verts, int numVerts) {
    static GfxVertex sVerts[GFX_TAB_MAX_VERTS];
    static unsigned short sIndices[GFX_TAB_MAX_VERTS];
    unsigned int groupBytes, vertBytes;
    SplitTable *s;
    if (numVerts <= 0 || numGroups <= 0) {
        return;
    }
    if (numVerts > GFX_TAB_MAX_VERTS) {
        numVerts = GFX_TAB_MAX_VERTS - GFX_TAB_MAX_VERTS % 3;
    }
    if (!gfxb_sprite_table()) {
        gfx_tab_expand(groups, fogMul, fogOffset, verts, numVerts, sVerts, sIndices);
        gfx_split_draw_indexed(state, sVerts, numVerts, sIndices, numVerts / 3);
        return;
    }
    groupBytes = (unsigned int) numGroups * sizeof(GfxTabGroup);
    vertBytes = (unsigned int) numVerts * sizeof(GfxTabVertex);
    s = gfx_split_call_reserve(table_call, sizeof(SplitTable) + groupBytes + vertBytes);
    if (s == NULL) {
        gfx_split_drain();
        gfxb_draw_sprite_table(state, groups, numGroups, fogMul, fogOffset, verts, numVerts);
        return;
    }
    s->state = *state;
    s->fogMul = fogMul;
    s->fogOffset = fogOffset;
    s->numGroups = numGroups;
    s->numVerts = numVerts;
    memcpy(s + 1, groups, groupBytes);
    memcpy((unsigned char *) (s + 1) + groupBytes, verts, vertBytes);
    gfx_split_call_commit(s);
}

/*
 * P63 DKR_SPLIT_REF (gfx_split.h): the same as gfx_split_draw_indexed but for the copy, which the walk's bucket draws
 * spent ~1.2 ms a frame on in the Old 3DS's game core (run 50). The backend reads the walk's arrays inside
 * gfxb_draw_indexed and keeps no pointer to them, so a done ticket after that call frees them again.
 */
PLAT_HOT_GAME PLAT_NOINLINE int gfx_split_draw_indexed_ref(const GfxDrawState *state, const GfxVertex *verts,
                                                           int numVerts, const unsigned short *indices,
                                                           int numTriangles, int check, unsigned int *ticket) {
    SplitDrawRef *r;
    unsigned int copyBytes = 0;
    if (!sActive) {
        return SPLIT_REF_NOT_QUEUED;
    }
    if (check) {
        copyBytes = (unsigned int) numVerts * sizeof(GfxVertex) + (unsigned int) numTriangles * 3u * sizeof(short);
        if (sizeof(SplitDrawRef) + copyBytes + 64u > SPLIT_RING_BYTES / 4u) {
            return SPLIT_REF_NOT_QUEUED;
        }
    }
    if (gfxb_walk_draw_indexed(state, verts, numVerts, indices, numTriangles)) {
        return SPLIT_REF_DRAWN; // DKR_WALK_VERTS: the walk made the GPU's vertices and queued them (a call record)
    }
    r = reserve(OP_INDEXED_REF, sizeof(SplitDrawRef) + copyBytes);
    r->state = *state;
    r->verts = verts;
    r->indices = indices;
    r->numVerts = numVerts;
    r->numTriangles = numTriangles;
    if (++sRefTicket == 0) {
        sRefTicket = 1;
    }
    r->ticket = sRefTicket;
    r->check = check != 0;
    if (check) {
        memcpy(r + 1, verts, (size_t) numVerts * sizeof(GfxVertex));
        memcpy((GfxVertex *) (r + 1) + numVerts, indices, (size_t) numTriangles * 3u * sizeof(short));
    }
    {
        unsigned int refBytes = (unsigned int) numVerts * sizeof(GfxVertex) + (unsigned int) numTriangles * 3u * sizeof(short);
        sRefWakeBytes += refBytes;
        sWalk.refBytes += refBytes; // DKR_SPLIT_LINES: the block comes back sooner if its draw is handed over sooner
    }
    commit(r);
    *ticket = sRefTicket;
    return SPLIT_REF_QUEUED;
}

int gfx_split_ref_done(unsigned int ticket) {
    return (int) (__atomic_load_n(&sRefDone, __ATOMIC_ACQUIRE) - ticket) >= 0;
}

void gfx_split_ref_counts(unsigned int *compared, unsigned int *differ) {
    *compared = sRefCompared;
    *differ = sRefDiffer;
}

static void texture_op(unsigned int op, GfxTexture *tex, const GfxTexture *src) {
    SplitTexture *t = reserve(op, sizeof(SplitTexture));
    t->tex = tex;
    t->src = src;
    t->ticket = 0;
    commit(t);
}

/*
 * A free that goes through the queue after the draws that still use the texture, as a mesh's
 * does, instead of the walk waiting for the backend to catch up (gfx_split_texture_free). The caller must not reuse
 * tex (its cache entry) before gfx_split_free_done says the ticket is done. Returns 0 when it was freed now. On an
 * Old 3DS a mid-race free otherwise made the walk wait for the whole queue.
 */
unsigned int gfx_split_texture_free_later(GfxTexture *tex) {
    SplitTexture *t;
    if (!sActive) {
        gfxb_texture_free(tex);
        return 0;
    }
    t = reserve(OP_TEX_FREE, sizeof(SplitTexture));
    t->tex = tex;
    t->src = NULL;
    if (++sFreeTicket == 0) {
        sFreeTicket = 1;
    }
    t->ticket = sFreeTicket;
    commit(t);
    return sFreeTicket;
}

int gfx_split_free_done(unsigned int ticket) {
    return (int) (__atomic_load_n(&sFreeDone, __ATOMIC_ACQUIRE) - ticket) >= 0;
}

void gfx_split_texture_upload(GfxTexture *tex) {
    if (!sActive) {
        gfxb_texture_upload(tex);
        return;
    }
    texture_op(OP_TEX_UPLOAD, tex, NULL);
}

void gfx_split_texture_share(GfxTexture *dst, const GfxTexture *src) {
    if (!sActive) {
        gfxb_texture_share(dst, src);
        return;
    }
    texture_op(OP_TEX_SHARE, dst, src);
}

void gfx_split_texture_free(GfxTexture *tex) {
    if (!sActive) {
        gfxb_texture_free(tex);
        return;
    }
    // Draws still queued may point at the texture, whose cache entry is reused once this returns: the backend
    // catches up first, then the free is made here, with the backend idle.
    gfx_split_drain();
    gfxb_texture_free(tex);
}

/*
 * GPU vertex maths (P30) and meshes (P31, P34) on two cores: model draws are queued like the others, with their
 * matrix. The walk creates meshes itself (gfxb_mesh_create may be called from either thread); a mesh freed goes
 * through the queue, after the draws that still use it.
 */
void gfx_split_draw_model(const GfxDrawState *state, const GfxModelParams *params, const GfxModelVertex *verts,
                          int numVerts, const unsigned short *indices, int numTriangles) {
    SplitModel *m;
    unsigned int vertBytes = (unsigned int) numVerts * sizeof(GfxModelVertex);
    unsigned int indexBytes = (unsigned int) numTriangles * 3u * sizeof(unsigned short);
    if (!sActive || sizeof(SplitModel) + vertBytes + indexBytes + 64u > SPLIT_RING_BYTES / 4u) {
        gfx_split_drain();
        gfxb_draw_model(state, params, verts, numVerts, indices, numTriangles);
        return;
    }
    m = reserve(OP_MODEL, sizeof(SplitModel) + vertBytes + indexBytes);
    m->state = *state;
    m->params = *params;
    m->numVerts = numVerts;
    m->numTriangles = numTriangles;
    memcpy(m + 1, verts, vertBytes);
    memcpy((unsigned char *) (m + 1) + vertBytes, indices, indexBytes);
    commit(m);
}

/*
 * P68 (DKR_KART_BATCH bit 2): an OP_MODEL2 record reserved in the queue, for the walk to write its vertices and indices
 * into (gfx_split_model2_end hands it over). The params are queued whole only when the matrix, fog or light differ
 * from the frame's last OP_MODEL2 (a kart's batches share them): else only the texture factors. NULL when this frame's
 * calls aren't queued or the record is too big for the ring: the caller draws as before (gfx_split_draw_model).
 * check: 0 none, 1 the backend compares the params it made with the walk's, 3 the same, and once the backend leaves
 * a record's texture factors stale (rule K6's break, which the compare must report).
 */
GfxModelVertex *gfx_split_model2_begin(const GfxDrawState *state, const GfxModelParams *params, int numVerts, int n0,
                                       int n1, unsigned char cull1, int check, unsigned short **indices) {
    SplitModel2 *m;
    unsigned char *at;
    int full;
    unsigned int bytes;
    if (!sActive || sWalkModel2Open != NULL) {
        return NULL;
    }
    full = !sWalkModel2LastOk || __builtin_memcmp(params->mtx, sWalkModel2Last.mtx, sizeof(params->mtx)) != 0 ||
           __builtin_memcmp((const unsigned char *) params + MODEL2_TEX_OFFSET + MODEL2_TEX_BYTES,
                  (const unsigned char *) &sWalkModel2Last + MODEL2_TEX_OFFSET + MODEL2_TEX_BYTES,
                  sizeof(GfxModelParams) - MODEL2_TEX_OFFSET - MODEL2_TEX_BYTES) != 0;
    bytes = sizeof(SplitModel2) + (full ? sizeof(GfxModelParams) : MODEL2_TEX_BYTES) +
            (check ? sizeof(GfxModelParams) : 0) + (unsigned int) numVerts * sizeof(GfxModelVertex) +
            (unsigned int) (n0 + n1) * 3u * sizeof(unsigned short);
    if (bytes + 64u > SPLIT_RING_BYTES / 4u) {
        return NULL;
    }
    m = reserve(OP_MODEL2, bytes);
    m->state = *state;
    m->numVerts = numVerts;
    m->n[0] = (unsigned short) n0;
    m->n[1] = (unsigned short) n1;
    m->cull1 = cull1;
    m->full = (unsigned char) full;
    m->check = check ? 1 : 0;
    m->pad = 0;
    at = (unsigned char *) (m + 1);
    if (full) {
        memcpy(at, params, sizeof(GfxModelParams));
        at += sizeof(GfxModelParams);
    } else {
        if (check == 3 && sWalkModel2BreakArmed &&
            __builtin_memcmp(params->texScale, sWalkModel2Last.texScale, MODEL2_TEX_BYTES) != 0) {
            sWalkModel2BreakArmed = 0;
            m->check = 2; // Stale factors that differ from these: the compare must see it
        }
        memcpy(at, params->texScale, MODEL2_TEX_BYTES);
        at += MODEL2_TEX_BYTES;
        sModel2Slim++;
    }
    if (check) {
        memcpy(at, params, sizeof(GfxModelParams));
        at += sizeof(GfxModelParams);
    }
    sWalkModel2Last = *params;
    sWalkModel2LastOk = 1;
    sWalkModel2Open = m;
    sModel2Records++;
    *indices = (unsigned short *) ((GfxModelVertex *) at + numVerts);
    return (GfxModelVertex *) at;
}

void gfx_split_model2_end(void) {
    void *m = sWalkModel2Open;
    if (m != NULL) {
        sWalkModel2Open = NULL;
        commit(m);
    }
}

void gfx_split_model2_counts(unsigned int *records, unsigned int *slim, unsigned int *compared, unsigned int *wrong) {
    *records = sModel2Records;
    *slim = sModel2Slim;
    *compared = sModel2Compared;
    *wrong = sModel2Wrong;
}

void gfx_split_draw_meshes(const GfxDrawState *state, const GfxModelParams *params, const GfxMeshPart *parts,
                           int numParts) {
    SplitModel *m;
    unsigned int partBytes = (unsigned int) numParts * sizeof(GfxMeshPart);
    if (!sActive) {
        gfxb_draw_meshes(state, params, parts, numParts);
        return;
    }
    m = reserve(OP_MESHES, sizeof(SplitModel) + partBytes);
    m->state = *state;
    m->params = *params;
    m->numVerts = numParts;
    m->numTriangles = 0;
    memcpy(m + 1, parts, partBytes);
    commit(m);
}

void gfx_split_mesh_free(void *mesh) {
    if (!sActive) {
        gfxb_mesh_free(mesh);
        return;
    }
    texture_op(OP_MESH_FREE, mesh, NULL);
}

/*
 * The walk's side of the queue (gfx_split_draw_indexed, gfx_split_call_reserve/commit) runs on the game core every
 * draw: PLAT_HOT_GAME (plat.h, P57 D).
 *
 * v28 (DKR_NATIVE_REMOTE): work of the walk's own run on the backend thread, in order with the draws: room
 * for bytes of the caller's (8-aligned), or NULL when the calls aren't queued this frame or the record is too big for
 * the ring (the caller then drains the queue and makes the call itself).
 */
PLAT_HOT_GAME void *gfx_split_call_reserve(void (*fn)(void *), unsigned int bytes) {
    SplitCall *c;
    if (!sActive || sizeof(SplitCall) + bytes + 64u > SPLIT_RING_BYTES / 4u) {
        return NULL;
    }
    c = reserve(OP_CALL, sizeof(SplitCall) + bytes);
    c->fn = fn;
    return c + 1;
}

PLAT_HOT_GAME void gfx_split_call_commit(void *payload) {
    commit((SplitCall *) payload - 1);
}

/* The same for a record that turned out to need only bytes of the room reserved (no more than that): the rest of the
 * reservation is left for the next record. */
void gfx_split_call_commit_shrink(void *payload, unsigned int bytes) {
    SplitRecord *rec = (SplitRecord *) ((SplitCall *) payload - 1) - 1;
    unsigned int total = sizeof(SplitRecord) + round8(sizeof(SplitCall) + bytes);
    if (total < rec->bytes) {
        rec->bytes = total;
    }
    commit((SplitCall *) payload - 1);
}

void gfx_split_set_hooks(void (*closeFn)(void), void (*keepFn)(void *)) {
    gfx_split_drain();
    sCloseFn = closeFn;
    sKeepFn = keepFn;
}
