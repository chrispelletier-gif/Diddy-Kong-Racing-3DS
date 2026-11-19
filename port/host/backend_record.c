/*
 * Backend recorder (roadmap P56 package D1, the backend replay harness): with DKR_BACKEND_RECORD=path, every call the
 * renderer frontend makes into the backend (gfx_backend.h) is written to that file, with the data its pointers point
 * at, before the software backend runs it. port/host/c3d_replay/ plays the file through the 3DS backend
 * (port/3ds/gfx_c3d.c) and the real citro3d on the PC, and port/tools/backend_replay.py compares two commits' command
 * words and GPU register state draw by draw. The format is port/host/c3d_replay/record_format.h.
 *
 * The calls are caught with the linker (-Wl,--wrap=gfxb_xxx, port/Makefile), so neither the frontend nor the
 * software backend knows about the recorder; without the variable each wrapper is one test and a jump.
 * DKR_BACKEND_RECORD_EVERY=n keeps only every n-th frame's draws (default 1: all of them); texture and mesh changes
 * are always kept, since later frames use them.
 *
 * Record with DKR_SPLIT_RENDER=0 (port/tools/backend_replay.py does): the two-core renderer would make the walk's own
 * calls (gfxb_mesh_create) and the backend thread's interleave differently from run to run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx_backend.h"
#include "plat/plat.h"
#include "c3d_replay/record_format.h"

static FILE *sOut;
static int sState = -1; // -1 not decided yet, 0 off, 1 on
static int sEvery = 1;
static unsigned int sFrame, sNextId = 1;
static int sInFrame;       // Between start and end frame
static int sKeepFrame = 1; // This frame's draws are written

/* Host pointers (textures, meshes) to file ids: open addressing, deletion by backward shift. */
#define MAP_SLOTS (1 << 16)
static struct {
    const void *key;
    unsigned int id;
} sMap[MAP_SLOTS];

static unsigned int map_slot(const void *p) {
    return ((unsigned int) (unsigned long) p >> 3) * 2654435761u >> 16;
}

static unsigned int map_get(const void *p) {
    unsigned int i = map_slot(p);
    if (p == NULL) {
        return 0;
    }
    while (sMap[i].key != NULL) {
        if (sMap[i].key == p) {
            return sMap[i].id;
        }
        i = (i + 1) & (MAP_SLOTS - 1);
    }
    return 0;
}

static unsigned int map_new(const void *p) {
    unsigned int i = map_slot(p), n = 0;
    while (sMap[i].key != NULL && sMap[i].key != p) {
        i = (i + 1) & (MAP_SLOTS - 1);
        if (++n == MAP_SLOTS) {
            plat_log("backend record: too many textures and meshes");
            abort();
        }
    }
    sMap[i].key = p;
    sMap[i].id = sNextId++;
    return sMap[i].id;
}

static void map_remove(const void *p) {
    unsigned int i = map_slot(p), j, k;
    while (sMap[i].key != NULL && sMap[i].key != p) {
        i = (i + 1) & (MAP_SLOTS - 1);
    }
    if (sMap[i].key == NULL) {
        return;
    }
    sMap[i].key = NULL;
    for (j = (i + 1) & (MAP_SLOTS - 1); sMap[j].key != NULL; j = (j + 1) & (MAP_SLOTS - 1)) {
        k = map_slot(sMap[j].key);
        // Move j into the hole at i if its home slot k is not cyclically in (i, j].
        if ((j > i && (k <= i || k > j)) || (j < i && k <= i && k > j)) {
            sMap[i] = sMap[j];
            sMap[j].key = NULL;
            i = j;
        }
    }
}

static int on(void) {
    if (sState < 0) {
        const char *path = plat_getenv("DKR_BACKEND_RECORD");
        sState = 0;
        if (path != NULL && path[0] != '\0') {
            RecHeader h;
            sOut = fopen(path, "wb");
            if (sOut == NULL) {
                plat_log("backend record: can't write %s", path);
                return 0;
            }
            sEvery = plat_switch("DKR_BACKEND_RECORD_EVERY", 1);
            sEvery = sEvery < 1 ? 1 : sEvery;
            memset(&h, 0, sizeof(h));
            h.magic = REC_MAGIC;
            h.version = REC_VERSION;
            h.sizeofVertex = sizeof(GfxVertex);
            h.sizeofDrawState = sizeof(GfxDrawState);
            h.sizeofTexture = sizeof(GfxTexture);
            h.sizeofModelVertex = sizeof(GfxModelVertex);
            h.sizeofModelParams = sizeof(GfxModelParams);
            h.sizeofMeshPart = sizeof(GfxMeshPart);
            h.sizeofPointer = sizeof(void *);
            fwrite(&h, sizeof(h), 1, sOut);
            plat_log("backend record: writing %s (every %d frames)", path, sEvery);
            sState = 1;
        }
    }
    return sState;
}

static void put(const void *p, size_t n) {
    if (n > 0 && fwrite(p, 1, n, sOut) != n) {
        plat_log("backend record: write failed");
        abort();
    }
}

static void put_op(int op) {
    unsigned char b = (unsigned char) op;
    put(&b, 1);
}

static void put_s32(int v) {
    put(&v, 4);
}

/* The draw's state with its texture pointers as ids. */
static void put_state(const GfxDrawState *state) {
    GfxDrawState s = *state;
    int i;
    for (i = 0; i < 2; i++) {
        unsigned int id = map_get(state->textures[i]);
        if (state->textures[i] != NULL && id == 0) {
            plat_log("backend record: a draw uses a texture never uploaded");
        }
        s.textures[i] = (GfxTexture *) (unsigned long) id;
    }
    put(&s, sizeof(s));
}

static void put_texture(const GfxTexture *tex) {
    GfxTexture t = *tex;
    t.pixels = NULL;
    t.backendData = NULL;
    put(&t, sizeof(t));
}

static int draws_kept(void) {
    return on() && sKeepFrame;
}

void __real_gfxb_init(void);
void __wrap_gfxb_init(void) {
    if (on()) {
        put_op(REC_INIT);
    }
    __real_gfxb_init();
}

void __real_gfxb_start_frame(void);
void __wrap_gfxb_start_frame(void) {
    if (on()) {
        sKeepFrame = sFrame % (unsigned int) sEvery == 0;
        sInFrame = 1;
        if (sKeepFrame) {
            put_op(REC_START_FRAME);
            put_s32((int) sFrame);
        }
    }
    __real_gfxb_start_frame();
}

void __real_gfxb_end_frame(void);
void __wrap_gfxb_end_frame(void) {
    __real_gfxb_end_frame();
    if (on()) {
        if (sKeepFrame) {
            put_op(REC_END_FRAME);
        }
        sFrame++;
        sInFrame = 0;
        sKeepFrame = 1;
        fflush(sOut);
    }
}

void __real_gfxb_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void __wrap_gfxb_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    if (draws_kept()) {
        unsigned char rgba[4] = { r, g, b, a };
        put_op(REC_CLEAR_COLOR);
        put(rect, 8);
        put(rgba, 4);
    }
    __real_gfxb_clear_color(rect, r, g, b, a);
}

void __real_gfxb_clear_depth(const short rect[4]);
void __wrap_gfxb_clear_depth(const short rect[4]) {
    if (draws_kept()) {
        put_op(REC_CLEAR_DEPTH);
        put(rect, 8);
    }
    __real_gfxb_clear_depth(rect);
}

void __real_gfxb_draw_triangles(const GfxDrawState *state, const GfxVertex *verts, int numTriangles);
void __wrap_gfxb_draw_triangles(const GfxDrawState *state, const GfxVertex *verts, int numTriangles) {
    if (draws_kept()) {
        put_op(REC_DRAW_TRIANGLES);
        put_state(state);
        put_s32(numTriangles);
        put(verts, sizeof(GfxVertex) * 3 * (size_t) numTriangles);
    }
    __real_gfxb_draw_triangles(state, verts, numTriangles);
}

void __real_gfxb_draw_indexed(const GfxDrawState *state, const GfxVertex *verts, int numVerts,
                              const unsigned short *indices, int numTriangles);
void __wrap_gfxb_draw_indexed(const GfxDrawState *state, const GfxVertex *verts, int numVerts,
                              const unsigned short *indices, int numTriangles) {
    if (draws_kept()) {
        put_op(REC_DRAW_INDEXED);
        put_state(state);
        put_s32(numVerts);
        put(verts, sizeof(GfxVertex) * (size_t) numVerts);
        put_s32(numTriangles);
        put(indices, 6 * (size_t) numTriangles);
    }
    __real_gfxb_draw_indexed(state, verts, numVerts, indices, numTriangles);
}

void __real_gfxb_draw_model(const GfxDrawState *state, const GfxModelParams *params, const GfxModelVertex *verts,
                            int numVerts, const unsigned short *indices, int numTriangles);
void __wrap_gfxb_draw_model(const GfxDrawState *state, const GfxModelParams *params, const GfxModelVertex *verts,
                            int numVerts, const unsigned short *indices, int numTriangles) {
    if (draws_kept()) {
        put_op(REC_DRAW_MODEL);
        put_state(state);
        put(params, sizeof(*params));
        put_s32(numVerts);
        put(verts, sizeof(GfxModelVertex) * (size_t) numVerts);
        put_s32(numTriangles);
        put(indices, 6 * (size_t) numTriangles);
    }
    __real_gfxb_draw_model(state, params, verts, numVerts, indices, numTriangles);
}

void __real_gfxb_draw_meshes(const GfxDrawState *state, const GfxModelParams *params, const GfxMeshPart *parts,
                             int numParts);
void __wrap_gfxb_draw_meshes(const GfxDrawState *state, const GfxModelParams *params, const GfxMeshPart *parts,
                             int numParts) {
    if (draws_kept()) {
        int i;
        put_op(REC_DRAW_MESHES);
        put_state(state);
        put(params, sizeof(*params));
        put_s32(numParts);
        for (i = 0; i < numParts; i++) {
            GfxMeshPart p = parts[i];
            p.mesh = (void *) (unsigned long) map_get(parts[i].mesh);
            put(&p, sizeof(p));
        }
    }
    __real_gfxb_draw_meshes(state, params, parts, numParts);
}

void *__real_gfxb_mesh_create(const GfxModelVertex *verts, int numVerts, const unsigned short *indices,
                              int numIndices);
void *__wrap_gfxb_mesh_create(const GfxModelVertex *verts, int numVerts, const unsigned short *indices,
                              int numIndices) {
    void *mesh = __real_gfxb_mesh_create(verts, numVerts, indices, numIndices);
    if (on()) {
        put_op(REC_MESH_CREATE);
        put_s32(mesh != NULL ? (int) map_new(mesh) : 0);
        put_s32(numVerts);
        put(verts, sizeof(GfxModelVertex) * (size_t) numVerts);
        put_s32(numIndices);
        put(indices, 2 * (size_t) numIndices);
    }
    return mesh;
}

void __real_gfxb_mesh_free(void *mesh);
void __wrap_gfxb_mesh_free(void *mesh) {
    if (on() && mesh != NULL) {
        put_op(REC_MESH_FREE);
        put_s32((int) map_get(mesh));
        map_remove(mesh);
    }
    __real_gfxb_mesh_free(mesh);
}

void __real_gfxb_texture_upload(GfxTexture *tex);
void __wrap_gfxb_texture_upload(GfxTexture *tex) {
    if (on()) {
        unsigned int id = map_get(tex);
        put_op(REC_TEXTURE_UPLOAD);
        put_s32((int) (id != 0 ? id : map_new(tex)));
        put_texture(tex);
        put(tex->pixels, 4 * (size_t) tex->width * tex->height);
    }
    __real_gfxb_texture_upload(tex);
}

void __real_gfxb_texture_free(GfxTexture *tex);
void __wrap_gfxb_texture_free(GfxTexture *tex) {
    if (on()) {
        unsigned int id = map_get(tex);
        if (id != 0) {
            put_op(REC_TEXTURE_FREE);
            put_s32((int) id);
            map_remove(tex);
        }
    }
    __real_gfxb_texture_free(tex);
}

void __real_gfxb_texture_share(GfxTexture *dst, const GfxTexture *src);
void __wrap_gfxb_texture_share(GfxTexture *dst, const GfxTexture *src) {
    if (on()) {
        unsigned int id = map_get(dst);
        put_op(REC_TEXTURE_SHARE);
        put_s32((int) (id != 0 ? id : map_new(dst)));
        put_texture(dst);
        put_s32((int) map_get(src));
    }
    __real_gfxb_texture_share(dst, src);
}

void __real_gfxb_gpu_kind(int kind);
void __wrap_gfxb_gpu_kind(int kind) {
    if (draws_kept()) {
        put_op(REC_GPU_KIND);
        put_s32(kind);
    }
    __real_gfxb_gpu_kind(kind);
}

void __real_gfxb_gpu_kind_call(void *payload);
void __wrap_gfxb_gpu_kind_call(void *payload) {
    if (draws_kept()) {
        put_op(REC_GPU_KIND);
        put_s32(*(int *) payload);
    }
    __real_gfxb_gpu_kind_call(payload);
}

int __real_gfxb_gpu_kinds_frame(int frame);
int __wrap_gfxb_gpu_kinds_frame(int frame) {
    if (draws_kept()) {
        put_op(REC_GPU_KINDS_FRAME);
        put_s32(frame);
    }
    return __real_gfxb_gpu_kinds_frame(frame);
}

int __real_gfxb_gpu_transform(void);
int __wrap_gfxb_gpu_transform(void) {
    if (draws_kept()) {
        put_op(REC_GPU_TRANSFORM);
    }
    return __real_gfxb_gpu_transform();
}
