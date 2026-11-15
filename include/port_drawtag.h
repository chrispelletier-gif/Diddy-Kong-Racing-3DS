#ifndef PORT_DRAWTAG_H
#define PORT_DRAWTAG_H
/*
 * Draw mix tags (port only): a no-op command in the display list that tells the renderer what the draws after it are
 * for, so the profiler can show per kind the draws, triangles, vertices, the renderer's and the game thread's time and
 * the geometry that is wasted (off the screen, facing away, smaller than a pixel) ("draw mix" log line, the bottom
 * screen's monitor, the test runs' autorun_drawmix.csv). The renderer skips the command as the RDP would. Up to 16
 * kinds (the tag's low 4 bits); port_profile.c and gfx_rdp.c name them.
 */
#ifdef TARGET_PORT
#include "ultra64.h"
#define PORT_DRAW_TAG 0x50440000 /* "PD" */
enum {
    PORT_DRAW_OTHER,   /* Menus, text and anything untagged */
    PORT_DRAW_TRACK,   /* The level's opaque geometry */
    PORT_DRAW_RACERS,  /* Karts and their drivers */
    PORT_DRAW_OBJECTS, /* Other objects (bananas, balloons, scenery objects...) */
    PORT_DRAW_SKY,     /* The sky dome or background */
    PORT_DRAW_SEE_THROUGH, /* The level's see-through geometry and the water waves */
    PORT_DRAW_EFFECTS, /* Particles, weather, lens flare, shields and magnets */
    PORT_DRAW_SHADOWS, /* Shadows and water ripples under objects */
    PORT_DRAW_HUD,     /* The race HUD */
    PORT_DRAW_KINDS
};
/* The game thread's side: notes when each kind (and object kind, profile.h PROF_OBJ_*; -1 for none) starts, for the
 * time it spends building it (port_profile.c). */
void port_draw_kind_game(int kind, int object);
/* DKR_SKIP_GAME_KINDS (a bitmask of the kinds above, switch bench cost rows): the game doesn't build these at all, so
 * neither the game thread nor the renderer pays for them; the most a native renderer for that kind could save. */
int port_skip_game_kind(int kind);
/* v30: where the game thread's frame goes, for the profiler's "game core sections" line (port_profile.c). Each call
 * ends the section running and starts this one; only timed frames read the clock. */
enum {
    PORT_SEC_TOP,        /* The loop's start: display list set-up, input, the frame's load work */
    PORT_SEC_GAME,       /* mode_game before the object updates */
    PORT_SEC_LOGIC,      /* obj_update */
    PORT_SEC_POSTLOGIC,  /* After obj_update: state hash, native frame, cameras, AI nodes */
    PORT_SEC_WAVES,      /* waves_update */
    PORT_SEC_SHADOWS,    /* shadow_update */
    PORT_SEC_PRELUDE,    /* Colour cycles, fog, texture animations, display list state */
    PORT_SEC_SKY,        /* Camera, viewport and the sky */
    PORT_SEC_VIEW,       /* The camera matrix, environment map and wave visibility */
    PORT_SEC_SEGMENTS,   /* Finding the visible segments and their opaque geometry */
    PORT_SEC_SORT,       /* sort_objects_by_dist */
    PORT_SEC_OBJECTS,    /* The opaque and see-through object loops */
    PORT_SEC_LATE,       /* See-through geometry, the last object pass, particles */
    PORT_SEC_WEATHER,    /* Weather and lens flare */
    PORT_SEC_HUD,        /* hud_render_player */
    PORT_SEC_GAMETAIL,   /* The rest of mode_game (general HUD, race state, menus) */
    PORT_SEC_SOUND,      /* sound_update_queue */
    PORT_SEC_TAIL,       /* Dialogue, transitions, the display list's end */
    PORT_SEC_RWAIT,      /* v31: waiting for the renderer to take the frame (gfxtask_wait_ahead) */
    PORT_SEC_VSYNC,      /* v31: fb_update (the frame's swap and the wait for its retrace) */
    PORT_SEC_END_LOD,    /* v31, after the frame's end: the adaptive racer detail */
    PORT_SEC_END_NOTE,   /* port_game_note_frame */
    PORT_SEC_END_AUTORUN, /* port_autorun_frame (test runs and benches) */
    PORT_SEC_END_HUD,    /* port_race_hud_frame (the bottom screen's race HUD) */
    PORT_SEC_END_LOOP,   /* Menu sounds and the loop back to the next frame's start */
    PORT_SEC_PROFILER,   /* v32: the profiler's readout and log lines, after the frame is built (DKR_PROF_IDLE_LOG) */
    PORT_SEC_COUNT
};
void port_game_section(int section);
/*
 * v31 (DKR_OBJ_PREFETCH, Old 3DS by default): the game's loops over every object (updates, shadows, sorting, the draw
 * passes) start loading the object two places ahead (its first line, with the position and flags, and the line at
 * 0x40 with its header, behaviour and parts pointers) and the header of the next one, so the misses on objects spread
 * all over the heap overlap the work on this one. A hint only: what the loops do is unchanged.
 */
extern int gPortObjPrefetch;
#define PORT_PREFETCH_OBJS(list, i, end)                                                                              \
    do {                                                                                                              \
        if (gPortObjPrefetch) {                                                                                       \
            if ((i) + 2 < (end)) {                                                                                    \
                const char *pfObj_ = (const char *) (list)[(i) + 2];                                                  \
                __builtin_prefetch(pfObj_);                                                                           \
                __builtin_prefetch(pfObj_ + 0x40);                                                                    \
            }                                                                                                         \
            if ((i) + 1 < (end) && (list)[(i) + 1] != NULL) {                                                                                    \
                __builtin_prefetch((list)[(i) + 1]->header);                                                          \
            }                                                                                                         \
        }                                                                                                             \
    } while (0)
/* The same going down the list. */
#define PORT_PREFETCH_OBJS_DOWN(list, i, start)                                                                       \
    do {                                                                                                              \
        if (gPortObjPrefetch) {                                                                                       \
            if ((i) - 2 >= (start)) {                                                                                 \
                const char *pfObj_ = (const char *) (list)[(i) - 2];                                                  \
                __builtin_prefetch(pfObj_);                                                                           \
                __builtin_prefetch(pfObj_ + 0x40);                                                                    \
            }                                                                                                         \
            if ((i) - 1 >= (start) && (list)[(i) - 1] != NULL) {                                                                                 \
                __builtin_prefetch((list)[(i) - 1]->header);                                                          \
            }                                                                                                         \
        }                                                                                                             \
    } while (0)
/*
 * Native model meshes (roadmap NR1, DKR_NATIVE_MESH, a bitmask: 1 the racers, 2 other objects' 3D models): render_mesh
 * puts this tag before each batch's vertex loads and triangles. The renderer then draws the batch from its own tables
 * (gfx_rdp.c native_mesh: each model's corners and indices worked out once, every vertex transformed once) instead of
 * replaying the N64 vertex slots; when it can't (billboards, flat matrices, the profiler's audit frames) it ignores the
 * tag and the commands after it draw the batch as before.
 */
#define PORT_NATIVE_TAG 0x504E0000 /* "PN" */
int port_native_mesh_for(int isRacer);
/*
 * Native track (roadmap NR4, DKR_NATIVE_TRACK): render_level_segment puts this tag before each batch's vertex load and
 * triangles. The renderer then draws the batch from a copy in GPU memory made the first time it is seen (the track
 * never changes during a level), transformed by the GPU's vertex shader with the camera's matrix, and draws the
 * level's opaque batches sorted by material, one draw for each material instead of one for each batch. Batches near
 * the camera, whose triangles the GPU can't draw that big, and anything unusual leave the tag unused, and the commands
 * after it draw the batch as before. gPortLevelLoads counts the levels loaded, so a level loaded at the same address
 * as the last one isn't drawn from the last one's copies.
 */
#define PORT_TRACK_TAG 0x50540000 /* "PT" */
int port_native_track(void);
int port_shadows_first(void); // DKR_SHADOWS_FIRST: solid objects' shadows drawn together first (tracks.c)
extern unsigned int gPortLevelLoads;
/*
 * GPU vertex lighting (off-GitHub PICA_ALL test build, DKR_GPU_LIGHT): render_mesh puts PORT_LIGHT_TAG | 1 and then
 * four no-ops whose second words are the light's floats (x, y, z, ambient) before a batch whose vertex colours hold
 * normals (obj_asm.c), and PORT_LIGHT_TAG alone after it.
 */
#define PORT_LIGHT_TAG 0x504C0000 /* "PL" */
/*
 * Native draws (PICA_ALL, DKR_NATIVE_DRAW): render_mesh records a batch as PORT_NDRAW_TAG | material followed by two
 * data words (vertices and triangles; vertex count | split << 8 | textured << 16 and the triangle count) instead of its
 * N64 commands. A material's first use (or after gPortNMatBad) emits its commands in full, then PORT_NMAT_TAG |
 * material, for the renderer to keep their state.
 */
#define PORT_NDRAW_TAG 0x50420000 /* "PB" */
#define PORT_NMAT_TAG 0x504D0000  /* "PM" */
/* A matrix load as floats (PICA_ALL native draws): PORT_MTXF_TAG | index, then a word whose w0 is the address of 16
 * floats (an MtxF written where the game's fixed point Mtx would have been). */
#define PORT_MTXF_TAG 0x50580000 /* "PX" */
/* A billboard's anchor (G3, DKR_NATIVE_CAMERA): PORT_ANCHOR_TAG, then a word whose w0 is the address of one Vertex. It
 * stands for the game's own one vertex load into slot 0 (the sprite's position, before the billboard matrix), which
 * the renderer makes at once instead of through the RSP's lazy loads. */
#define PORT_ANCHOR_TAG 0x50410000 /* "PA" */
int port_native_camera(void);
/* A texture rectangle without its texture load (PICA_ALL native draws): PORT_NRECT_TAG | material, then the game's
 * G_TEXRECT and its two G_RDPHALF words. */
#define PORT_NRECT_TAG 0x50520000 /* "PR" */
int port_native_rect(Gfx **dl, const void *tex);
int port_native_draw(void);
/* DKR_NATIVE_LEFTOVERS (needs native draws): the menus' and HUD's sprite frames and the pulsing-light track batches are
 * draw records instead of display lists. */
int port_native_leftovers(void);

int port_native_batch(Gfx **dl, void *texture, int flags, int texOffset, const void *verts, int numVerts,
                      const void *tris, int numTris, int textured, int dynamic);
void port_native_end(void);
void port_nmat_texture_freed(const void *start, const void *end);
int port_native_sprite(Gfx **dl, const Gfx *frame, int flags, int whitePrim);
void port_native_record(Gfx **dl, int id, const void *verts, int numVerts, const void *tris, int numTris, int textured,
                        int dynamic);
int port_native_material_here(Gfx **dl, const void *key, int keyFlags);
int port_native_batch_xlu(Gfx **dl, int index, u32 prim, u32 env, void *texture, const void *verts, int numVerts,
                          const void *tris, int numTris);
int port_nmat_key(const void *texture, int flags, int texOffset, int *capture);

/* Host builds with the walk census (PORT_CENSUS_SITES): the records obj_asm.c writes are named by the line that
 * called its functions (port/host/census_sites.c). obj_asm.c undefines these before defining the functions. */
#ifdef PORT_CENSUS_SITES
void port_census_caller(const char *file, int line);
#define PORT_CENSUS_CALL(call) (port_census_caller(__FILE__, __LINE__), call)
#define port_native_batch(...) PORT_CENSUS_CALL(port_native_batch(__VA_ARGS__))
#define port_native_batch_xlu(...) PORT_CENSUS_CALL(port_native_batch_xlu(__VA_ARGS__))
#define port_native_sprite(...) PORT_CENSUS_CALL(port_native_sprite(__VA_ARGS__))
#define port_native_rect(...) PORT_CENSUS_CALL(port_native_rect(__VA_ARGS__))
#define port_native_record(...) PORT_CENSUS_CALL(port_native_record(__VA_ARGS__))
#endif
/* v29 (DKR_TRIS_SKIP): the level and model triangle lists that game code changed in place (texture scrollers,
 * animators, model UV maps) go in this ring after their changes, and the renderer compares those batches' triangles
 * with its copies only then (see native_tris_take). Written by the game thread alone. */
#define PORT_TRIS_RING 2048
extern const void *volatile gPortTrisRing[PORT_TRIS_RING];
extern volatile unsigned int gPortTrisHead;
extern volatile unsigned int gPortTrisEpoch; /* Moved on when a model's memory is freed */
static inline void port_tris_changed(const void *tris) {
    unsigned int head = gPortTrisHead;
    gPortTrisRing[head % PORT_TRIS_RING] = tris;
    __sync_synchronize(); // The triangles and the entry before the head
    gPortTrisHead = head + 1;
}
struct Vertex;
int port_gpu_light_get(const struct Vertex *verts, const void *model, float out[4]);
void port_gpu_light_clear(const struct Vertex *verts);
#define port_draw_tag(dl, kind)                                                                                        \
    do {                                                                                                               \
        gDPNoOpTag(dl, PORT_DRAW_TAG | (kind));                                                                        \
        port_draw_kind_game(kind, -1);                                                                                 \
    } while (0)
/* An object's draws: also its kind (its behaviour, or particles) in bits 4-11, plus one (0 is no object). */
#define port_draw_tag_object(dl, kind, object)                                                                         \
    do {                                                                                                               \
        gDPNoOpTag(dl, PORT_DRAW_TAG | (((object) + 1) << 4) | (kind));                                                \
        port_draw_kind_game(kind, object);                                                                             \
    } while (0)
#else
#define port_draw_tag(dl, kind)
#define port_draw_tag_object(dl, kind, object)
#define port_game_section(section) ((void) 0)
#define PORT_PREFETCH_OBJS(list, i, end) ((void) 0)
#define PORT_PREFETCH_OBJS_DOWN(list, i, start) ((void) 0)
#endif
#endif
