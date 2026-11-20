#ifndef PORT_GFX_SPLIT_H
#define PORT_GFX_SPLIT_H

#include "gfx_backend.h"

/*
 * The two-core renderer (roadmap P21, gfx_split.c): the display list walk's backend calls, queued for a second
 * renderer thread while DKR_SPLIT_RENDER is on, else made at once. gfx_split_frame_start starts the frame either
 * way (direct: this frame is made at once whatever the switch says) and says whether its calls are queued.
 */
/* holdCores (R5's DKR_FRAME_DIRECT 2): a direct frame made on the system core keeps the share of it the two-core
 * renderer raised, instead of giving it back. */
int gfx_split_frame_start(int direct, int holdCores);
void gfx_split_frame_end(void);
int gfx_split_active(void);
void gfx_split_drain(void);
void gfx_split_kick(void);
void gfx_split_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void gfx_split_clear_depth(const short rect[4]);
void gfx_split_draw_triangles(const GfxDrawState *state, const GfxVertex *verts, int numTriangles);
void gfx_split_draw_indexed(const GfxDrawState *state, const GfxVertex *verts, int numVerts,
                            const unsigned short *indices, int numTriangles);
void gfx_split_texture_upload(GfxTexture *tex);
void gfx_split_texture_share(GfxTexture *dst, const GfxTexture *src);
void gfx_split_texture_free(GfxTexture *tex);
unsigned int gfx_split_texture_free_later(GfxTexture *tex);
int gfx_split_free_done(unsigned int ticket);
void gfx_split_draw_model(const GfxDrawState *state, const GfxModelParams *params, const GfxModelVertex *verts,
                          int numVerts, const unsigned short *indices, int numTriangles);
void gfx_split_draw_meshes(const GfxDrawState *state, const GfxModelParams *params, const GfxMeshPart *parts,
                           int numParts);
void gfx_split_mesh_free(void *mesh);
void *gfx_split_call_reserve(void (*fn)(void *), unsigned int bytes);
void gfx_split_call_commit(void *payload);
void gfx_split_call_commit_shrink(void *payload, unsigned int bytes);
/* closeFn is run on the backend thread before any record but a call of keepFn (see gfx_split.c). */
void gfx_split_set_hooks(void (*closeFn)(void), void (*keepFn)(void *));

/* Since the last call: the walk's time waiting for the backend, the backend's busy time and frames, and how often the
 * backend was woken (the profiler's "two-core renderer" line). */
/* The walk's waits for the other core since start-up, in ns (the draw mix leaves them out of each kind's time). */
unsigned long long gfx_split_walk_waited_ns(void);
/* The backend's busy time since start-up, in ns (never reset; the profiler's per-frame core times read its changes). */
unsigned long long gfx_split_backend_busy_ns(void);
/* P54 DKR_BACKEND_MIX: the backend's ms a timed frame by record type since this reader (0 the log, 1 the core window)
 * last asked, as one line; returns its length (0: no frame timed). */
int gfx_split_backend_mix(int reader, char *out, int size);
void gfx_split_take_times(unsigned long long *walkWaitNs, unsigned long long *backendBusyNs,
                          unsigned int *backendFrames, unsigned int *backendWakes);

#endif
