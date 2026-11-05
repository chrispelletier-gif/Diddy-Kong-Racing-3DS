/*
 * C translations of the hand written MIPS routines in src/hasm/obj_animate.s and src/hasm/obj_shade_fast.s.
 * They follow the assembly instruction by instruction, including its integer wrap-around behaviour.
 *
 * Object animation data is kept in the ROM's big endian byte order (see port/src/game/asset_swap.c), so it is
 * read with explicit big endian loads here.
 */
#include <ultra64.h>
#include "types.h"
#include "structs.h"
#include "objects.h"
#include "camera.h"
#include "math_util.h"
#include "port.h"
#include "plat/plat.h"
#include "port_drawtag.h"

extern s32 D_8011D644; // Scratch buffer of interpolated animation offsets, 3 s16 per animated vertex.

#define ANIM_S16(p) ((s16) port_read_be16(p))

/* Writes a vertex colour the way the assembly does: two big endian halfword stores. */
static inline void store_shade(Vertex *v, s32 value) {
    u16 first = (u16) (value | (value << 8));
    u16 second = (u16) (first | 0xFF);
    v->r = first >> 8;
    v->g = first & 0xFF;
    v->b = second >> 8;
    v->a = second & 0xFF;
}

#ifdef PICA_ALL
/*
 * GPU vertex lighting (off-GitHub "everything on the PICA200" test build, DKR_GPU_LIGHT): the two simple shading
 * routines below store each vertex's normal (signed bytes, 127 = 1) in its colour instead of the lit grey, and note
 * the light (direction scaled so that n . L + ambient is the grey over 255) for that vertex buffer. render_mesh then
 * tags the buffer's lit batches with it (PORT_LIGHT_TAG) and the renderer lights the vertices: the GPU's vertex shader
 * for GPU-transformed draws, the CPU (gfx_light_shade) otherwise. A buffer is only marked while there is room in the
 * table; otherwise it is shaded on the CPU as before.
 */
#define GPU_LIGHTS 512
#define GPU_LIGHT_PROBES 8
typedef struct GpuLight {
    const Vertex *verts;
    const ObjectModel *model;
    unsigned int level;
    float l[4];
    float scale; // Normal units a stored byte step is worth (8192 / 127 for unit normals)
    u8 fresh;    // The colours don't hold this model's normals yet
    s32 check[2];   // Two stored vertices (first and last lit) and what their colours held, to catch a buffer
    u32 checkRgba[2]; // reused by another object of the same model
} GpuLight;

static u32 vertex_rgba(const Vertex *v) {
    return ((u32) v->r << 24) | ((u32) v->g << 16) | ((u32) v->b << 8) | v->a;
}
static GpuLight sGpuLights[GPU_LIGHTS];
static unsigned int sGpuLightLevel = 0xFFFFFFFFu;
static int sGpuLightOn;

static GpuLight *gpu_light_find(const Vertex *verts, int add) {
    unsigned int h = (unsigned int) (((size_t) verts >> 3) * 2654435761u) % GPU_LIGHTS, i;
    GpuLight *freeSlot = NULL;
    if (sGpuLightLevel != gPortLevelLoads) {
        __builtin_memset(sGpuLights, 0, sizeof(sGpuLights));
        sGpuLightLevel = gPortLevelLoads;
        sGpuLightOn = plat_switch("DKR_GPU_LIGHT", 0) != 0;
    }
    for (i = 0; i < GPU_LIGHT_PROBES; i++) {
        GpuLight *e = &sGpuLights[(h + i) % GPU_LIGHTS];
        if (e->verts == verts) {
            return e;
        }
        if (e->verts == NULL && freeSlot == NULL) {
            freeSlot = e;
        }
    }
    if (add && freeSlot != NULL) {
        freeSlot->verts = verts;
        return freeSlot;
    }
    return NULL;
}

/* The light slot for this object's vertices if they are to be lit on the GPU, else NULL (shade on the CPU). */
static GpuLight *gpu_light_take(Object *obj, ObjectModel *model) {
    GpuLight *e;
    (void) gpu_light_find(NULL, 0); // Level / switch check
    if (!sGpuLightOn) {
        return NULL;
    }
    e = gpu_light_find(obj->curVertData, 1);
    if (e != NULL) {
        // The colours keep the normals from frame to frame (animation only moves the positions), so they are only
        // stored when the buffer is new to the table or its model changed.
        e->fresh = e->model != model || e->level != gPortLevelLoads ||
                   vertex_rgba(&obj->curVertData[e->check[0]]) != e->checkRgba[0] ||
                   vertex_rgba(&obj->curVertData[e->check[1]]) != e->checkRgba[1];
        e->model = model;
        e->level = gPortLevelLoads;
    }
    return e;
}

void port_gpu_light_clear(const Vertex *verts) {
    GpuLight *e = gpu_light_find(verts, 0);
    if (e != NULL && verts != NULL) {
        e->verts = NULL;
        e->model = NULL;
    }
}

int port_gpu_light_get(const Vertex *verts, const void *model, float out[4]) {
    GpuLight *e;
    if (verts == NULL) {
        return 0;
    }
    e = gpu_light_find(verts, 0);
    if (e == NULL || e->model != model || !sGpuLightOn) {
        return 0;
    }
    out[0] = e->l[0];
    out[1] = e->l[1];
    out[2] = e->l[2];
    out[3] = e->l[3];
    return 1;
}

/*
 * Native draws (port_drawtag.h PORT_NDRAW_TAG): the game side's table of materials, keyed by what material_set takes
 * and the two settings it reads. Emptied with each level (the renderer's captures go with it: a key's first use in a
 * level is always a capture) or when full.
 */
#define NMAT_KEYS 1024
typedef struct NMatKey {
    const void *tex;
    s32 flags, texOffset, blocked;
    s16 dirLight;
    u8 used;
} NMatKey;
extern s32 gBlockedRenderFlags;
extern s16 gUseDirectionalLighting;
static NMatKey sNMatKeys[NMAT_KEYS];
static u32 sNMatCount;
static unsigned int sNMatLevel = 0xFFFFFFFFu;
static int sNativeDraw;
volatile unsigned char gPortNMatBad[NMAT_KEYS];

static void nmat_level_check(void) {
    if (sNMatLevel != gPortLevelLoads) {
        sNMatLevel = gPortLevelLoads;
        __builtin_memset(sNMatKeys, 0, sizeof(sNMatKeys));
        sNMatCount = 0;
        sNativeDraw = plat_switch("DKR_NATIVE_DRAW", 0) != 0 && !plat_switch("DKR_OLD_RENDERER", 0);
#ifdef PORT_NATIVE_OPS
        // The game's draw-state calls write native ops with native draws (include/port_nops.h; DKR_NATIVE_OPS=0: N64
        // commands with native draws still on).
        gPortNativeOps = sNativeDraw && plat_switch("DKR_NATIVE_OPS", 1) != 0;
#endif
    }
}

int port_native_draw(void) {
    nmat_level_check();
    return sNativeDraw;
}

/* The game freed texture memory [start, end): the materials made with it are caught again on their next use, as the
 * memory may hold another texture by then (or the same one loaded again at the same place, which the renderer has
 * forgotten). */
void port_nmat_texture_freed(const void *start, const void *end) {
    u32 i;
    if (!sNativeDraw) {
        return;
    }
    for (i = 0; i < NMAT_KEYS; i++) {
        NMatKey *k = &sNMatKeys[i];
        if (k->used && (const u8 *) k->tex >= (const u8 *) start && (const u8 *) k->tex < (const u8 *) end) {
            gPortNMatBad[i] = 1;
        }
    }
}

int port_nmat_key(const void *texture, int flags, int texOffset, int *capture) {
    u32 h = (u32) ((((size_t) texture >> 3) ^ (u32) flags * 31u ^ (u32) texOffset * 131u ^
                    (u32) gBlockedRenderFlags * 7u ^ (u32) gUseDirectionalLighting) * 2654435761u) % NMAT_KEYS, i;
    nmat_level_check();
    for (i = 0; i < 16; i++) {
        u32 id = (h + i) % NMAT_KEYS;
        NMatKey *k = &sNMatKeys[id];
        if (!k->used) {
            if (sNMatCount >= NMAT_KEYS * 3 / 4) {
                break;
            }
            k->used = 1;
            k->tex = texture;
            k->flags = flags;
            k->texOffset = texOffset;
            k->blocked = gBlockedRenderFlags;
            k->dirLight = gUseDirectionalLighting;
            sNMatCount++;
            gPortNMatBad[id] = 0;
            *capture = 1;
            return (int) id;
        }
        if (k->tex == texture && k->flags == flags && k->texOffset == texOffset && k->blocked == gBlockedRenderFlags &&
            k->dirLight == gUseDirectionalLighting) {
            *capture = gPortNMatBad[id] != 0;
            gPortNMatBad[id] = 0;
            return (int) id;
        }
    }
    // Full: start again (every key is captured afresh).
    __builtin_memset(sNMatKeys, 0, sizeof(sNMatKeys));
    sNMatCount = 0;
    return port_nmat_key(texture, flags, texOffset, capture);
}

extern s16 gForceFlags;
extern TextureHeader *gCurrentTextureHeader;
void material_set(Gfx **dList, TextureHeader *texhead, s32 flags, s32 texOffset);

/* One batch as a native draw record (the material's commands first when the renderer hasn't kept them). dynamic:
 * vertices or triangles made anew each frame (drawn through the renderer's vertex slots). Returns 0 with native draws
 * off, for the caller's own commands. */
int port_native_batch(Gfx **dl, void *texture, int flags, int texOffset, const void *verts, int numVerts,
                      const void *tris, int numTris, int textured, int dynamic) {
    int capture;
    s32 id;
    if (!port_native_draw() || numVerts <= 0 || numVerts > 64 || numTris <= 0) {
        return 0;
    }
    id = port_nmat_key(texture, flags, texOffset, &capture);
    if (capture) {
        gForceFlags = TRUE;
        gCurrentTextureHeader = NULL;
        material_set(dl, (TextureHeader *) texture, flags, texOffset);
        gDPNoOpTag((*dl)++, PORT_NMAT_TAG | id);
    }
    port_native_record(dl, id, verts, numVerts, tris, numTris, textured, dynamic);
    return 1;
}

/* The draw record itself, for a material id from port_native_batch's key or port_native_material_here. */
void port_native_record(Gfx **dl, int id, const void *verts, int numVerts, const void *tris, int numTris, int textured,
                        int dynamic) {
    Gfx *g;
    gDPNoOpTag((*dl)++, PORT_NDRAW_TAG | id);
    g = (*dl)++;
    g->words.w0 = OS_K0_TO_PHYSICAL(verts);
    g->words.w1 = OS_K0_TO_PHYSICAL(tris);
    g = (*dl)++;
    g->words.w0 = (u32) numVerts | ((u32) numVerts << 8) | ((u32) (textured != 0) << 16) | ((u32) (dynamic != 0) << 17);
    g->words.w1 = (u32) numTris;
}

/* The settings the caller has just sent, kept as a material (caught again every time: for code that sets its own
 * settings once and then draws many batches, like the water). Returns the id, or -1 with native draws off. */
int port_native_material_here(Gfx **dl, const void *key, int keyFlags) {
    int capture;
    s32 id;
    if (!port_native_draw()) {
        return -1;
    }
    id = port_nmat_key(key, (int) (0x20000000u | (u32) keyFlags), 0, &capture);
    gDPNoOpTag((*dl)++, PORT_NMAT_TAG | id);
    return id;
}

void gfx_init_basic_xlu(Gfx **dList, u32 index, u32 primitiveColor, u32 environmentColor);

/* A batch drawn with gfx_init_basic_xlu settings and a texture's own load commands (sky flash, rain): the settings are
 * kept with the material; only the two colours go in each frame. */
int port_native_batch_xlu(Gfx **dl, int index, u32 prim, u32 env, void *texture, const void *verts, int numVerts,
                          const void *tris, int numTris) {
    int capture;
    s32 id;
    TextureHeader *tex = (TextureHeader *) texture;
    if (!port_native_draw() || tex == NULL || numVerts <= 0 || numVerts > 64 || numTris <= 0) {
        return 0;
    }
    id = port_nmat_key(texture, (int) (0x40000000u | (u32) index), 0, &capture);
    if (capture) {
        gfx_init_basic_xlu(dl, (u32) index, prim, env);
        gDkrDmaDisplayList((*dl)++, OS_K0_TO_PHYSICAL(tex->cmd), tex->numberOfCommands);
        gDPNoOpTag((*dl)++, PORT_NMAT_TAG | id);
    } else {
        gDPSetPrimColorRGBA((*dl)++, prim);
        gDPSetEnvColorRGBA((*dl)++, env);
    }
    port_native_record(dl, id, verts, numVerts, tris, numTris, 1, 1);
    return 1;
}

extern s32 gSpriteOpaque;
void material_load_simple(Gfx **dList, s32 flags);

/* A sprite frame (sprite_init_frame's list: per tile its texture's load commands, every fifth tile a vertex load, two
 * triangles) as one draw record per tile, each tile's settings kept as a material. The anchor, matrix and billboard
 * mode are the caller's, as for the frame's list. whitePrim: the primitive colour set to white after the settings, as
 * the game does. */
int port_native_sprite(Gfx **dl, const Gfx *frame, int flags, int whitePrim) {
    const Gfx *c, *dma = NULL;
    u32 vtxAddr = 0, vtxCount = 0;
    s32 n;
    if (!port_native_draw() || frame == NULL) {
        return 0;
    }
    for (c = frame, n = 0; n < 256; c++, n++) {
        u32 op = c->words.w0 >> 24;
        if (op == (u8) G_DMADL) {
            dma = c;
        } else if (op == (u8) G_VTX) {
            vtxAddr = c->words.w1;
            vtxCount = ((c->words.w0 >> 19) & 0x1F) + 1;
        } else if (op == (u8) G_TRIN) {
            int capture;
            s32 id = port_nmat_key(dma != NULL ? (const void *) (uintptr_t) dma->words.w1 : NULL,
                                   (int) (0x10000000u | ((u32) (gSpriteOpaque != 0) << 27) | (u32) flags), 0, &capture);
            Gfx *g;
            if (capture) {
                gForceFlags = TRUE;
                material_load_simple(dl, flags);
                if (dma != NULL) {
                    *(*dl)++ = *dma;
                }
                gDPNoOpTag((*dl)++, PORT_NMAT_TAG | id);
            }
            if (whitePrim) {
                gDPSetPrimColor((*dl)++, 0, 0, 255, 255, 255, 255);
                whitePrim = FALSE;
            }
            gDPNoOpTag((*dl)++, PORT_NDRAW_TAG | id);
            g = (*dl)++;
            g->words.w0 = vtxAddr;
            g->words.w1 = c->words.w1;
            g = (*dl)++;
            g->words.w0 = vtxCount | ((c->words.w0 >> 16) & 1) << 16 | (1u << 17) | (1u << 18);
            g->words.w1 = (c->words.w0 >> 4) & 0xFFF;
            vtxAddr = 0;
            vtxCount = 0;
        } else if (op == (u8) G_ENDDL) {
            break;
        }
    }
    port_native_end();
    return 1;
}

/* A texture rectangle's texture as a native material: on first use its load commands, then PORT_NRECT_TAG; the
 * caller's G_TEXRECT follows. Returns the id, or -1 with native draws off (the caller sends the load itself). */
int port_native_rect(Gfx **dl, const void *texture) {
    const TextureHeader *tex = (const TextureHeader *) texture;
    int capture;
    s32 id;
    if (!port_native_draw() || tex == NULL) {
        return -1;
    }
    id = port_nmat_key(texture, 0x08000000, 0, &capture);
    if (capture) {
        gDkrDmaDisplayList((*dl)++, OS_K0_TO_PHYSICAL(tex->cmd), tex->numberOfCommands);
        gDPNoOpTag((*dl)++, PORT_NMAT_TAG | id);
    }
    gDPNoOpTag((*dl)++, PORT_NRECT_TAG | id);
    return id;
}

/* After native batches: the renderer's settings are the last captured material's, so the next material_set sets
 * everything again. */
void port_native_end(void) {
    gForceFlags = TRUE;
    gCurrentTextureHeader = NULL;
}

static inline s32 normal_byte(s32 n, float k) {
    float f = n * k;
    return (s32) (f >= 0.0f ? f + 0.5f : f - 0.5f);
}

/* Normals into the colours of the batches the routines light (as their loops walk them), scaled so the largest
 * component is 127 (not all models' normals are unit length); gl->scale gets the scale's inverse. */
static void store_normals(ObjectModel *model, Object *obj, GpuLight *gl) {
    TriangleBatchInfo *batch, *batchEnd = model->batches + model->numberOfBatches;
    Vec3s *normals;
    Vertex *verts;
    s32 count, pass, biggest = 1, first = -1, last = 0;
    float k = 0.0f;
    for (pass = 0; pass < 2; pass++) {
    batch = model->batches;
    normals = model->normals;
    verts = obj->curVertData;
    do {
        count = batch[1].verticesOffset - batch->verticesOffset;
        if (batch->miscData == BATCH_VTX_COL) {
            verts += count;
            if (batch->flags & 0x8000) {
                normals += count;
            }
        } else {
            while (count-- > 0) {
                if (pass == 0) {
                    s32 c[3] = { normals->x, normals->y, normals->z }, q;
                    for (q = 0; q < 3; q++) {
                        s32 m = c[q] < 0 ? -c[q] : c[q];
                        biggest = m > biggest ? m : biggest;
                    }
                } else {
                    verts->r = (u8) (s8) normal_byte(normals->x, k);
                    verts->g = (u8) (s8) normal_byte(normals->y, k);
                    verts->b = (u8) (s8) normal_byte(normals->z, k);
                    verts->a = 0xFF;
                    last = verts - obj->curVertData;
                    first = first < 0 ? last : first;
                }
                verts++;
                normals++;
            }
        }
        batch++;
    } while (batch < batchEnd);
    k = 127.0f / (float) biggest;
    }
    gl->scale = (float) biggest / 127.0f;
    gl->check[0] = first < 0 ? 0 : first;
    gl->check[1] = last;
    gl->checkRgba[0] = vertex_rgba(&obj->curVertData[gl->check[0]]);
    gl->checkRgba[1] = vertex_rgba(&obj->curVertData[gl->check[1]]);
}

/* The light, from the routine's direction and factor c (grey = ambient + (n . dir) * c over 255). */
static void gpu_light_set(ObjectModel *model, Object *obj, GpuLight *gl, s32 dirX, s32 dirY, s32 dirZ, float c,
                          float ambient) {
    if (gl->fresh) {
        store_normals(model, obj, gl);
    }
    c *= gl->scale / 255.0f;
    gl->l[0] = dirX * c;
    gl->l[1] = dirY * c;
    gl->l[2] = dirZ * c;
    gl->l[3] = ambient / 255.0f;
}
#endif

/**
 * Updates the vertices of an animated object model for the object's current animation and frame.
 * Returns 1 if the vertices were updated, 0 if nothing changed.
 */
s32 obj_animate(Object *obj) {
    s32 modelIndex;
    ModelInstance *inst;
    ObjectModel *model;
    s32 frame;   // t9
    s32 animID;  // a3
    s32 keyframe; // a0
    s32 lastLength; // t2
    s32 prevKeyframe; // s0
    s32 frac; // t8
    s32 stride; // t4
    s32 numAnimVerts;
    s16 *work; // s1
    s16 *deltas; // D_8011D644
    s16 *vertIndices;
    s16 *vertIndicesEnd;
    Vertex *baseVerts;
    Vertex *outVerts;
    u8 *anim;
    u8 *src;
    s16 *dst;
    s16 *dstEnd;
    s32 x, y, z, tilt;
    s32 idx;

    modelIndex = obj->modelIndex;
    if (modelIndex < 0) {
        modelIndex = 0;
    }
    if (!(modelIndex < obj->header->numberOfModelIds)) {
        modelIndex = obj->header->numberOfModelIds;
    }
    inst = obj->modelInstances[modelIndex];
    model = inst->objModel;
    if (model->animations == NULL) {
        return 0;
    }
    frame = obj->animFrame;
    animID = obj->animationID;
    if (frame == inst->animationFrameCount && animID == inst->animationID) {
        return 0;
    }
    if (animID < 0) {
        animID = 0;
    }
    if (!(animID < model->numberOfAnimations)) {
        animID = model->numberOfAnimations - 1;
    }
    lastLength = 0;
    if (model->numberOfAnimations > 0) {
        lastLength = model->animations[animID].animLength - 2;
    }
    keyframe = (u32) frame >> 4;
    if (lastLength < keyframe) {
        frame = 0;
        keyframe = 0;
        inst->animationID = -1;
    }
    work = (s16 *) inst->vertices[2];
    if (animID == inst->animationID) {
        prevKeyframe = inst->animationFrame;
    } else {
        prevKeyframe = -1;
    }
    inst->animationID = animID;
    inst->animationFrameCount = frame;
    inst->animationFrame = keyframe;
    frac = frame & 0xF;
    anim = model->animations[animID].animData;
    numAnimVerts = model->numberOfAnimatedVertices;
    vertIndices = (s16 *) model->animatedVertexIndices;
    vertIndicesEnd = vertIndices + model->numberOfVertices;

    if (keyframe == 0 || prevKeyframe == -1) {
        // Rebuild the work buffer from the first keyframe's absolute positions.
        u8 *positions = anim + 0xC;
        s16 *indexPtr = vertIndices;
        baseVerts = model->vertices;
        while (indexPtr < vertIndicesEnd) {
            idx = *indexPtr;
            if (idx != -1) {
                dst = work + idx * 3;
                src = positions + idx * 6;
                dst[0] = baseVerts->x + ANIM_S16(src + 0);
                dst[1] = baseVerts->y + ANIM_S16(src + 2);
                dst[2] = baseVerts->z + ANIM_S16(src + 4);
            }
            indexPtr++;
            baseVerts++;
        }
        prevKeyframe = 0;
    }
    stride = numAnimVerts * 3 + 12;

    // Step forwards through the keyframe deltas.
    if (prevKeyframe < keyframe) {
        u8 *frameData = anim + stride * (prevKeyframe + 2);
        do {
            src = frameData;
            frameData += stride;
            dst = work;
            dstEnd = work + numAnimVerts * 3;
            do {
                dst[0] += (s8) src[0];
                dst[1] += (s8) src[1];
                dst[2] += (s8) src[2];
                dst += 3;
                src += 3;
            } while (dst < dstEnd);
            prevKeyframe++;
        } while (prevKeyframe < keyframe);
    }
    // Or backwards.
    if (keyframe < prevKeyframe) {
        u8 *frameData = anim + stride * (prevKeyframe + 2);
        do {
            frameData -= stride;
            prevKeyframe--;
            src = frameData;
            dst = work;
            dstEnd = work + numAnimVerts * 3;
            do {
                dst[0] -= (s8) src[0];
                dst[1] -= (s8) src[1];
                dst[2] -= (s8) src[2];
                dst += 3;
                src += 3;
            } while (dst < dstEnd);
        } while (keyframe < prevKeyframe);
    }

    // Interpolate towards the next keyframe.
    src = anim + stride * (prevKeyframe + 2);
    deltas = (s16 *) D_8011D644;
    dst = deltas;
    dstEnd = deltas + numAnimVerts * 3;
    do {
        dst[0] = (s16) ((u32) ((s8) src[0] * frac) >> 4);
        dst[1] = (s16) ((u32) ((s8) src[1] * frac) >> 4);
        dst[2] = (s16) ((u32) ((s8) src[2] * frac) >> 4);
        dst += 3;
        src += 3;
    } while (dst < dstEnd);

    // Interpolate the model offset and head tilt stored in each keyframe's header.
    src = anim;
    if (keyframe != 0) {
        src = anim + stride * (keyframe + 1) - 12;
    }
    x = ANIM_S16(src + 0);
    y = ANIM_S16(src + 2);
    z = ANIM_S16(src + 4);
    tilt = ANIM_S16(src + 0xA);
    src += stride;
    if (keyframe == 0) {
        src += stride - 12;
    }
    inst->offsetX = x + (s32) ((u32) ((ANIM_S16(src + 0) - x) * frac) >> 4);
    inst->offsetY = y + (s32) ((u32) ((ANIM_S16(src + 2) - y) * frac) >> 4);
    inst->offsetZ = z + (s32) ((u32) ((ANIM_S16(src + 4) - z) * frac) >> 4);
    inst->headTilt = tilt + (s32) ((u32) ((ANIM_S16(src + 0xA) - tilt) * frac) >> 4);

    // Write the final vertex positions into the other vertex buffer.
    inst->animationTaskNum ^= 1;
    outVerts = inst->vertices[(u8) inst->animationTaskNum];
    while (vertIndices < vertIndicesEnd) {
        idx = *vertIndices++;
        if (idx != -1) {
            s16 *a = work + idx * 3;
            s16 *b = deltas + idx * 3;
            outVerts->x = a[0] + b[0];
            outVerts->y = a[1] + b[1];
            outVerts->z = a[2] + b[2];
        }
        outVerts++;
    }
    return 1;
}

/**
 * Applies simple directional shading to an object's vertex colours.
 */
void obj_shade_fast(ObjectModel *model, Object *obj, f32 intensity) {
    ShadeProperties *shading = obj->shading;
    TriangleBatchInfo *batch, *batchEnd;
    Vec3s *normals;
    Vertex *verts;
    s32 base, dirX, dirY, dirZ, count, dot, value;

    if (shading == NULL) {
        return;
    }
    base = (s32) (shading->unk0 * intensity * 160.0f);
    dirX = shading->shadowDirX;
    dirY = shading->shadowDirY;
    dirZ = shading->shadowDirZ;
#ifdef PICA_ALL
    {
        GpuLight *gl = gpu_light_take(obj, model);
        if (gl != NULL) {
            // value = base + (n . dir) * base / 2^27
            gpu_light_set(model, obj, gl, dirX, dirY, dirZ, (float) base / 134217728.0f, (float) base);
            return;
        }
    }
#endif
    normals = model->normals;
    verts = obj->curVertData;
    batch = model->batches;
    batchEnd = batch + model->numberOfBatches;
    do {
        count = batch[1].verticesOffset - batch->verticesOffset;
        if (batch->miscData == BATCH_VTX_COL) {
            verts += count;
            if (batch->flags & 0x8000) {
                normals += count;
            }
        } else {
            while (count-- > 0) {
                dot = (normals->x * dirX + normals->y * dirY + normals->z * dirZ) >> 11;
                normals++;
                if (dot <= 0) {
                    value = base;
                } else {
                    value = ((u32) (dot * base) >> 16) + base;
                    if (value >= 0x100) {
                        value = 0xFF;
                    }
                }
                store_shade(verts, value);
                verts++;
            }
        }
        batch++;
    } while (batch < batchEnd);
}

/**
 * Lights an object's vertices using the shadow direction relative to the object's rotation.
 */
void calc_dynamic_lighting_for_object_2(Object *obj, ObjectModel *model, s16 useProjection, f32 intensity) {
    ShadeProperties *shading = obj->shading;
    TriangleBatchInfo *batch, *batchEnd;
    ObjectTransform trans;
    MtxF mtx;
    Vec3f dir;
    Vec3s *normals;
    Vertex *verts;
    f32 scale;
    s32 ambient, diffuse, dirX, dirY, dirZ, count, dot, value;

    if (shading == NULL) {
        return;
    }
    dir.f[0] = (f32) (shading->shadowDirX << 2);
    dir.f[1] = (f32) (shading->shadowDirY << 2);
    dir.f[2] = (f32) (shading->shadowDirZ << 2);
    if (useProjection) {
        mtxf_transform_dir(get_projection_matrix_f32(), &dir, &dir);
    }
    trans.rotation.x_rotation = -obj->trans.rotation.x_rotation;
    trans.rotation.y_rotation = -obj->trans.rotation.y_rotation;
    trans.rotation.z_rotation = -obj->trans.rotation.z_rotation;
    trans.scale = 1.0f;
    trans.x_position = 0.0f;
    trans.y_position = 0.0f;
    trans.z_position = 0.0f;
    mtxf_from_inverse_transform(&mtx, &trans);
    mtxf_transform_dir(&mtx, &dir, &dir);

    scale = shading->unk0 * intensity * 255.0f;
    ambient = (s32) (shading->ambient * scale);
    diffuse = (s32) (shading->diffuse * scale);
    dirX = (s32) dir.f[0];
    dirY = (s32) dir.f[1];
    dirZ = (s32) dir.f[2];
#ifdef PICA_ALL
    {
        GpuLight *gl = gpu_light_take(obj, model);
        if (gl != NULL) {
            // value = ambient + (n . dir) * diffuse / 2^28
            gpu_light_set(model, obj, gl, dirX, dirY, dirZ, (float) diffuse / 268435456.0f, (float) ambient);
            return;
        }
    }
#endif
    normals = model->normals;
    verts = obj->curVertData;
    batch = model->batches;
    batchEnd = batch + model->numberOfBatches;
    do {
        count = batch[1].verticesOffset - batch->verticesOffset;
        if (batch->miscData == BATCH_VTX_COL) {
            verts += count;
            if (batch->flags & 0x8000) {
                normals += count;
            }
        } else {
            while (count-- > 0) {
                dot = (normals->x * dirX + normals->y * dirY + normals->z * dirZ) >> 7;
                normals++;
                if (dot <= 0) {
                    value = ambient;
                } else {
                    value = ((u32) (dot * diffuse) >> 21) + ambient;
                    if (value >= 0x100) {
                        value = 0xFF;
                    }
                }
                store_shade(verts, value);
                verts++;
            }
        }
        batch++;
    } while (batch < batchEnd);
}
