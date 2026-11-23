#ifndef PORT_GFX_BACKEND_H
#define PORT_GFX_BACKEND_H

/*
 * Interface between the F3DDKR/RDP interpreter (gfx_rdp.c) and a rendering backend.
 * Only builtin C types are used here, so backends can be written against their platform's own headers.
 *
 * Coordinate conventions:
 *  - Vertex positions are clip space coordinates for a 320x240 "N64 screen": x/w and y/w are in [-1, 1]
 *    with +y up, z/w is in [-1, 1] with -1 nearest. The N64 viewport transform is already applied.
 *  - Texture coordinates are normalised (1.0 = one full texture width/height, before wrapping).
 *  - Rectangles (scissor, fills) are in N64 screen pixels, origin top left.
 */


#define GFX_SCREEN_WIDTH 320
#define GFX_SCREEN_HEIGHT 240

/* Colour combiner inputs, matching the RDP's sources. */
enum GfxCCInput {
    GFX_CC_ZERO,
    GFX_CC_ONE,
    GFX_CC_COMBINED,
    GFX_CC_TEXEL0,
    GFX_CC_TEXEL1,
    GFX_CC_PRIM,
    GFX_CC_SHADE,
    GFX_CC_ENV,
    GFX_CC_COMBINED_ALPHA,
    GFX_CC_TEXEL0_ALPHA,
    GFX_CC_TEXEL1_ALPHA,
    GFX_CC_PRIM_ALPHA,
    GFX_CC_SHADE_ALPHA,
    GFX_CC_ENV_ALPHA,
    GFX_CC_LOD_FRACTION,
    GFX_CC_PRIM_LOD_FRACTION,
    GFX_CC_NOISE,
    GFX_CC_KEY_CENTER,
    GFX_CC_KEY_SCALE,
    GFX_CC_K4,
    GFX_CC_K5,
    GFX_CC_INPUT_COUNT
};

/* One (a - b) * c + d equation. */
typedef struct GfxCCEquation {
    unsigned char a, b, c, d;
} GfxCCEquation;

/* A full combiner: up to two cycles, separate colour and alpha equations. */
typedef struct GfxCombiner {
    GfxCCEquation rgb[2];
    GfxCCEquation alpha[2];
    unsigned char numCycles;
} GfxCombiner;

enum GfxBlendMode {
    GFX_BLEND_OPAQUE,
    GFX_BLEND_ALPHA,    /* src * a + dst * (1 - a) */
    GFX_BLEND_ADD,      /* src * a + dst */
    GFX_BLEND_ALPHA_CVG /* opaque, alpha tested (cutout) */
};

/* GfxDrawState.decal: every pixel at the farthest depth, behind whatever was drawn. Nothing sets it now; it goes with the next simplify batch. */
#define GFX_DEPTH_FAR 2

enum GfxTextureFilter { GFX_FILTER_POINT, GFX_FILTER_BILINEAR };
enum GfxTextureWrap { GFX_WRAP_REPEAT, GFX_WRAP_MIRROR, GFX_WRAP_CLAMP };

/*
 * The smallest format that holds a texture's pixels exactly, picked from the N64 format it was decoded from. The
 * 3DS renderer stores the texture on the GPU in it: 16 or 8 bits a texel instead of 32, so the GPU
 * reads less memory. RGBA5551 and LA4 channels are widened by repeating their bits, like the N64 does.
 */
enum GfxTextureFormat {
    GFX_TEXFMT_RGBA8,    /* 32 bit RGBA and anything else */
    GFX_TEXFMT_RGBA5551, /* RGBA16, and CI textures with an RGBA16 palette */
    GFX_TEXFMT_LA8,      /* IA16, IA4, I8 (intensity and alpha the same), CI4 with an IA16 palette */
    GFX_TEXFMT_LA4       /* IA8, I4 */
};

/* A texture prepared by the frontend. Pixels are RGBA8888 (R in the lowest byte). */
typedef struct GfxTexture {
    unsigned int width, height;
    const unsigned int *pixels;
    unsigned char format; /* enum GfxTextureFormat: the pixels fit it exactly */
    unsigned char filter;
    unsigned char wrapS, wrapT;
    void *backendData; /* Owned by the backend. */
    unsigned char atlas; /* A page of small textures (gfx_rdp.c's texture atlas): never given mipmaps */
} GfxTexture;

typedef struct GfxVertex {
    float x, y, z, w;
    float u0, v0;
    float u1, v1;
    unsigned char r, g, b, a; /* Shade colour. When fog is on, a holds the fog factor (0 = no fog). */
} GfxVertex;

typedef struct GfxDrawState {
    GfxCombiner combiner;
    unsigned char prim[4];
    unsigned char env[4];
    unsigned char fog[4];
    unsigned char primLodFrac;
    unsigned char blend;            /* enum GfxBlendMode */
    unsigned char alphaTest;        /* Discard pixels with alpha below alphaRef */
    unsigned char alphaRef;
    unsigned char depthTest;
    unsigned char depthWrite;
    unsigned char decal;            /* Pull towards the camera to win depth fights (ZMODE_DEC), or GFX_DEPTH_FAR */
    unsigned char fogEnabled;       /* Blend towards fog colour using vertex alpha */
    short fogMul, fogOffset;        /* Fog factor = clamp(z / w * fogMul + fogOffset, 0, 255), for per pixel fog */
    unsigned char cullMode;         /* 0 none, 1 back faces, 2 front faces (in screen space, CCW front) */
    unsigned char flat;             /* Drawn flat on the screen: rectangles and orthographic triangles (gfx_flat_mode) */
    GfxTexture *textures[2];
    short scissor[4];       /* x0, y0, x1, y1 (exclusive) */
} GfxDrawState;

/*
 * Widescreen. The game's camera is widened to the shape of the screen (plat_aspect_scale), so 3D
 * draws fill it. What is drawn flat on the screen (the HUD, text, menus: GfxDrawState.flat) keeps its 4:3 shape in
 * the middle, except where it reaches an edge of the N64 screen: fades, backgrounds and full screen fills are
 * stretched out to the edges of the wide one. gfx_flat_x takes a flat vertex's clip x (or a rectangle edge's, with
 * w = 1) to clip x on the wide screen, the 4:3 picture being `narrow` of its width (0.8 for 400 pixels).
 * The `mode`:
 *  - GFX_FLAT_STRETCH (untextured draws) also stretches what lies past an edge, as the shapes of screen wipes do.
 *  - GFX_FLAT_KEEP (textured triangles, full width textured rectangles) keeps its shape past an edge, as the Save
 *    Options boxes sliding off the screen must (the wide screen shows a little more of them).
 *  - GFX_FLAT_PLACED (textured rectangles narrower than the screen: letters, icons, background tiles) never
 *    stretches: a letter that happens to end at the edge, or an icon the game cut off there, stays its own size.
 */
#define GFX_FLAT_STRETCH 1
#define GFX_FLAT_KEEP 2
#define GFX_FLAT_PLACED 3
static inline float gfx_flat_x(float x, float w, float narrow, int mode) {
    // Within a pixel and a half of the edge: the game's scissor often ends at x = 319, and a full screen draw
    // clipped to it must still reach the edge (the race finish backgrounds left the old picture at the right).
    float aw = w < 0.0f ? -w : w, ax = x < 0.0f ? -x : x;
    return (mode != GFX_FLAT_PLACED && ax >= 0.99f * aw && (mode == GFX_FLAT_STRETCH || ax <= 1.01f * aw)) ? x
                                                                                                         : x * narrow;
}

/* The flat mode of a draw for gfx_flat_x: 0 when not flat. GfxDrawState.flat is 1, or GFX_FLAT_PLACED. */
static inline int gfx_flat_mode(const GfxDrawState *state) {
    return !state->flat                         ? 0
           : state->flat == GFX_FLAT_PLACED     ? GFX_FLAT_PLACED
           : state->textures[0] != NULL         ? GFX_FLAT_KEEP
                                                : GFX_FLAT_STRETCH;
}

/* The same for an x in N64 screen pixels, returned in pixels of a screen `width` wide. */
static inline float gfx_flat_px(float x, float narrow, float width) {
    return (gfx_flat_x(x / (GFX_SCREEN_WIDTH / 2.0f) - 1.0f, 1.0f, narrow, GFX_FLAT_STRETCH) + 1.0f) * (width / 2.0f);
}

/*
 * A draw that can't change a pixel (New 3DS GPU work, gpu_cuts.inc): blended (alpha or additive), writing no depth,
 * and its combiner's alpha 0 whatever the pixel, from constants alone (prim, env, one, zero). The screen fader is one:
 * a whole screen of prim alpha 0 drawn in every race frame once the fade is over (PC pixel count, DKR_FRAG_STATS),
 * which costs a New 3DS at 800 pixels with anti-aliasing as much GPU time as the sky. Blending with alpha 0 leaves
 * every pixel as it was, so leaving the draw out keeps frames identical. -1 in gfx_cc_alpha_const: not a constant.
 */
static inline float gfx_cc_alpha_input(const GfxDrawState *st, int input, float combined) {
    switch (input) {
        case GFX_CC_ZERO: return 0.0f;
        case GFX_CC_ONE: return 1.0f;
        case GFX_CC_COMBINED:
        case GFX_CC_COMBINED_ALPHA: return combined;
        case GFX_CC_PRIM:
        case GFX_CC_PRIM_ALPHA: return st->prim[3] / 255.0f;
        case GFX_CC_ENV:
        case GFX_CC_ENV_ALPHA: return st->env[3] / 255.0f;
        case GFX_CC_PRIM_LOD_FRACTION: return st->primLodFrac / 255.0f;
        default: return -1.0f; // Texels, shade, noise: per pixel
    }
}
static inline float gfx_cc_alpha_const(const GfxDrawState *st) {
    float combined = -1.0f;
    int c;
    for (c = 0; c < st->combiner.numCycles && c < 2; c++) {
        const GfxCCEquation *eq = &st->combiner.alpha[c];
        float a = gfx_cc_alpha_input(st, eq->a, combined), b = gfx_cc_alpha_input(st, eq->b, combined);
        float m = gfx_cc_alpha_input(st, eq->c, combined), d = gfx_cc_alpha_input(st, eq->d, combined);
        float v;
        if (d < 0.0f) {
            return -1.0f;
        }
        if (m == 0.0f || eq->a == eq->b) {
            v = d; // (a - b) * 0, or (x - x) * c: whatever a, b and c are
        } else if (a < 0.0f || b < 0.0f || m < 0.0f) {
            return -1.0f;
        } else {
            v = (a - b) * m + d;
        }
        combined = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    return combined;
}
static inline int gfx_draw_invisible(const GfxDrawState *st) {
    return (st->blend == GFX_BLEND_ALPHA || st->blend == GFX_BLEND_ADD) && !st->depthWrite &&
           st->combiner.numCycles >= 1 && gfx_cc_alpha_const(st) == 0.0f;
}

/* Backend entry points. */
void gfxb_init(void) __attribute__((cold));
void gfxb_start_frame(void);
void gfxb_end_frame(void);
void gfxb_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void gfxb_clear_depth(const short rect[4]);
/*
 * DKR_WALK_VERTS (P54 step 2; the 3DS backend, a no-op elsewhere): with the two-core renderer, the walk's thread makes
 * a draw's GPU vertices itself and queues them (gfx_split.c). gfxb_walk_frame_start is called at each queued frame's
 * start, after its start record; gfxb_walk_draw_indexed returns 0 when the draw is to be queued as before.
 */
void gfxb_walk_frame_start(void);
/* Its coverage so far, for the bench results: frames walked, draws the walk made, draws it left to the backend and
 * draws left out for room (counts[0..3]); all zeros where the walk never makes vertices. */
void gfxb_walk_coverage(unsigned int counts[4]) __attribute__((cold));
int gfxb_walk_draw_indexed(const GfxDrawState *state, const GfxVertex *verts, int numVerts,
                           const unsigned short *indices, int numTriangles);
void gfxb_draw_triangles(const GfxDrawState *state, const GfxVertex *verts, int numTriangles);
/*
 * The same with the triangles' corners as indices into verts, three a triangle, so that a vertex several triangles
 * share is handed over (and transformed) once. The frontend batches at most GFX_MAX_BATCH_TRIANGLES triangles with
 * GFX_MAX_BATCH_VERTICES vertices in one call.
 */
void gfxb_draw_indexed(const GfxDrawState *state, const GfxVertex *verts, int numVerts, const unsigned short *indices,
                       int numTriangles);
#define GFX_MAX_BATCH_TRIANGLES 256
#define GFX_MAX_BATCH_VERTICES (GFX_MAX_BATCH_TRIANGLES * 3)
/*
 * Sprite points (roadmap P60 D, DKR_SPRITE_GS, default off). A sprite tile of the 3D world (two triangles forming a
 * parallelogram, one texture, the same shade colour at each corner but for its alpha) handed over as one point
 * instead of four vertices and six indices. Its corners are p, q = p + e1, r = p + e2 and d = p + e1 + e2, all as a
 * GfxVertex would hold them (the N64 viewport folded in); the triangles are (p, q, r) and (d, r, q), so both keep the
 * winding the game gave them. gfx_sprite_expand makes the four vertices and six indices; the 3DS backend can instead
 * let the GPU's geometry shader make them (gfx_c3d.c, sprite.g.pica). Never flat (the walk only makes points for the
 * 3D world) and never with a second texture.
 */
typedef struct GfxSpritePoint {
    float p[4], e1[4], e2[4];  /* x, y, z, w */
    float uv[6];               /* p's u0, v0, then e1's and e2's */
    unsigned char rgba[4];     /* p's colour (with fog on, a is its fog factor, as GfxVertex.a) */
    unsigned char alpha[4];    /* q's, r's and d's alpha, then 0 */
} GfxSpritePoint;
#define GFX_MAX_SPRITE_POINTS (GFX_MAX_BATCH_TRIANGLES / 2)

/* The two triangles of each point, as gfxb_draw_indexed takes them: 4 vertices and 6 indices a point. */
static inline void gfx_sprite_expand(const GfxSpritePoint *pts, int n, GfxVertex *verts, unsigned short *indices) {
    int i, k;
    for (i = 0; i < n; i++) {
        const GfxSpritePoint *s = &pts[i];
        GfxVertex *v = &verts[i * 4];
        unsigned short *t = &indices[i * 6], b = (unsigned short) (i * 4);
        for (k = 0; k < 4; k++) {
            int a = k & 1, c = k >> 1; // p (0, 0), q (1, 0), r (0, 1), d (1, 1)
            float x = s->p[0], y = s->p[1], z = s->p[2], w = s->p[3], u = s->uv[0], vv = s->uv[1];
            if (a) {
                x += s->e1[0], y += s->e1[1], z += s->e1[2], w += s->e1[3], u += s->uv[2], vv += s->uv[3];
            }
            if (c) {
                x += s->e2[0], y += s->e2[1], z += s->e2[2], w += s->e2[3], u += s->uv[4], vv += s->uv[5];
            }
            v[k].x = x, v[k].y = y, v[k].z = z, v[k].w = w;
            v[k].u0 = u, v[k].v0 = vv;
            v[k].u1 = 0.0f, v[k].v1 = 0.0f;
            v[k].r = s->rgba[0], v[k].g = s->rgba[1], v[k].b = s->rgba[2];
            v[k].a = k == 0 ? s->rgba[3] : s->alpha[k - 1];
        }
        t[0] = b, t[1] = (unsigned short) (b + 1), t[2] = (unsigned short) (b + 2);
        t[3] = (unsigned short) (b + 3), t[4] = (unsigned short) (b + 2), t[5] = (unsigned short) (b + 1);
    }
}
/* Whether the backend takes sprite points itself (gfxb_draw_sprites); 0: the frontend expands them and draws them as
 * gfxb_draw_indexed (gfx_split_draw_sprites). Read once a frame. */
int gfxb_sprite_points(void);
void gfxb_draw_sprites(const GfxDrawState *state, const GfxSpritePoint *pts, int numPoints);
/*
 * GPU vertex maths. While the backend says so (gfxb_gpu_transform, at the start of each frame), the
 * frontend hands triangles over in the game's own model space where it can, with the draw's matrix, and the GPU
 * transforms them; everything else (billboards, flat draws, triangles whose box may cross the guard band) still comes
 * as GfxVertex. A model vertex is one of the game's vertices with one triangle corner's texture coordinates.
 */
typedef struct GfxModelVertex {
    short x, y, z, one; // Model space; one is 1
    short u, v;         // The corner's texture coordinates, in the game's units (s10.5 texels, before the tile shift)
    unsigned char r, g, b, a;
} GfxModelVertex;

typedef struct GfxModelParams {
    float mtx[4][4];        // Model to clip space as gfx_backend.h has it: clip[c] = x m[0][c] + y m[1][c] + z m[2][c] + m[3][c]
    float texScale[4];      // Normalised texture coordinates (u0, v0, u1, v1) = (u, v, u, v) * texScale + texOffset
    float texOffset[4];
    short fogMul, fogOffset; // With fog, the alpha is the fog factor clamp(z / w * fogMul + fogOffset, 0, 255)
    unsigned char fog;
    unsigned char pad[3];   // pad[0]: lit (GPU vertex lighting): the colours are normals, lit with light
    float light[4];         // Light direction (x, y, z, over 127-unit normals) and ambient, as gfx_light_shade takes them
} GfxModelParams;

/* GPU vertex lighting: the grey of a vertex whose colour holds its normal (signed bytes). */
static inline unsigned char gfx_light_shade(unsigned char r, unsigned char g, unsigned char b, const float l[4]) {
    float d = (signed char) r * l[0] + (signed char) g * l[1] + (signed char) b * l[2];
    float v = l[3] + (d > 0.0f ? d : 0.0f);
    v = v > 1.0f ? 1.0f : v;
    return (unsigned char) (v * 255.0f);
}

/* What gfxb_model_check says about a model draw whose model space box is boxMin..boxMax. */
enum { GFX_MODEL_CPU, GFX_MODEL_GPU, GFX_MODEL_HIDDEN };
int gfxb_gpu_transform(void);
/* Whether the walk may cull back faces itself this frame (DKR_CPU_CULL): in 3D only with DKR_STEREO_CULL. */
int gfxb_cpu_cull_ok(void);
/* With it, how far (in clip x) an eye may move a vertex this frame: the walk drops a triangle only if it faces away
 * in both eyes, and leaves the GPU's culling on one that faces away in one. 0: no 3D, the plain test. */
float gfxb_cpu_cull_margin(void);
int gfxb_model_check(const float mtx[4][4], const short boxMin[3], const short boxMax[3]);

/*
 * The GPU timeline's draw kinds (3DS gpu_cuts.inc): whether the walk is to say, in this frame, what kind of draws
 * (port_drawtag.h) come next whenever that changes (0 no, 1 yes, 2 yes and the backend times each kind: the walk adds
 * GFX_KIND_CUT to the kind it passes), and the backend's end of it (gfxb_gpu_kind, or gfxb_gpu_kind_call with the
 * kind as an OP_CALL record's payload on the two-core renderer). The kinds are also what DKR_SKIP_DRAW_KINDS leaves
 * out (the switch bench's cost rows).
 */
#define GFX_KIND_CUT 0x100
#define GFX_KIND_DRAWS 0x200 /* 3: this frame is timed draw by draw (gpu_cuts.inc section 6), told its kinds */
int gfxb_gpu_kinds_frame(int frame);
void gfxb_gpu_kind(int kind);
void gfxb_gpu_kind_call(void *payload);
void gfxb_draw_model(const GfxDrawState *state, const GfxModelParams *params, const GfxModelVertex *verts, int numVerts,
                     const unsigned short *indices, int numTriangles);
/*
 * P61 M3: the backend's model and mesh draws split into their parts (the 3DS backend; the PC's reports none), timed on
 * the backend mix's timed frames (DKR_BACKEND_MIX: gfx_split.c sets gGfxbModelSplitOn at each frame's start). Totals
 * since start-up, in clock ticks, with the draws counted and the clock reads made (whose cost the reader takes out).
 */
enum {
    GFXB_MSPLIT_DRAIN,    // drain_before_draw (and the hang report's start)
    GFXB_MSPLIT_STATE,    // set_all_state
    GFXB_MSPLIT_TWICE,    // write_settings_twice
    GFXB_MSPLIT_UNIFORMS, // set_model_uniforms and unif_write_twice
    GFXB_MSPLIT_COPY,     // The vertex copy into the frame's vertex buffer (models) or the parts' index copies (meshes)
    GFXB_MSPLIT_INDICES,  // The index rebase loop (models)
    GFXB_MSPLIT_DRAW,     // draw_prims
    GFXB_MSPLIT_OTHER,    // The rest: checks, vertex format, near depth note, bookkeeping
    GFXB_MSPLIT_COUNT
};
extern volatile int gGfxbModelSplitOn;
void gfxb_model_split_totals(unsigned long long ticks[GFXB_MSPLIT_COUNT], unsigned int *draws, unsigned int *reads);
/*
 * Meshes: model vertices copied once to memory the GPU reads, and drawn from there frame
 * after frame. gfxb_mesh_create returns NULL when out of room; a freed mesh goes once the GPU has drawn the frames
 * that use it. gfxb_draw_meshes draws parts of several meshes that share a state and parameters as one draw (a draw
 * costs the CPU about as much as a hundred triangles, so one per game command would undo the saving).
 */
typedef struct GfxMeshPart {
    void *mesh;
    int firstIndex, numTriangles;
} GfxMeshPart;
#define GFX_MAX_MESH_PARTS 64
void *gfxb_mesh_create(const GfxModelVertex *verts, int numVerts, const unsigned short *indices, int numIndices);
void gfxb_mesh_free(void *mesh);
void gfxb_draw_meshes(const GfxDrawState *state, const GfxModelParams *params, const GfxMeshPart *parts,
                      int numParts);

/* A model vertex the way the CPU path makes it, for backends without GPU vertex maths and for comparisons. */
static inline void gfx_model_to_vertex(GfxVertex *out, const GfxModelVertex *in, const GfxModelParams *p) {
    float x = in->x, y = in->y, z = in->z;
    out->x = x * p->mtx[0][0] + y * p->mtx[1][0] + z * p->mtx[2][0] + p->mtx[3][0];
    out->y = x * p->mtx[0][1] + y * p->mtx[1][1] + z * p->mtx[2][1] + p->mtx[3][1];
    out->z = x * p->mtx[0][2] + y * p->mtx[1][2] + z * p->mtx[2][2] + p->mtx[3][2];
    out->w = x * p->mtx[0][3] + y * p->mtx[1][3] + z * p->mtx[2][3] + p->mtx[3][3];
    out->u0 = in->u * p->texScale[0] + p->texOffset[0];
    out->v0 = in->v * p->texScale[1] + p->texOffset[1];
    out->u1 = in->u * p->texScale[2] + p->texOffset[2];
    out->v1 = in->v * p->texScale[3] + p->texOffset[3];
    out->r = in->r;
    out->g = in->g;
    out->b = in->b;
    out->a = in->a;
    if (p->pad[0]) {
        out->r = out->g = out->b = gfx_light_shade(in->r, in->g, in->b, p->light);
    }
    if (p->fog) {
        float w = (out->w != 0.0f) ? out->w : 0.0001f;
        float fog = (out->z / w) * p->fogMul + p->fogOffset;
        out->a = (fog < 0.0f) ? 0 : (fog > 255.0f) ? 255 : (unsigned char) fog;
    }
}

void gfxb_texture_upload(GfxTexture *tex);
void gfxb_texture_free(GfxTexture *tex);
/* Makes dst (a copy of src with another filter) draw from src's GPU copy instead of uploading its own. Both are freed
 * with gfxb_texture_free, together. */
void gfxb_texture_share(GfxTexture *dst, const GfxTexture *src);

/* A pixel (RGBA8888, R in the lowest byte) in a texture format's bits, and back the way the 3DS GPU widens them. */
static inline unsigned int gfx_texel_encode(unsigned int c, int format) {
    unsigned int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
    switch (format) {
        case GFX_TEXFMT_RGBA5551:
            return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7);
        case GFX_TEXFMT_LA8:
            return (r << 8) | a;
        case GFX_TEXFMT_LA4:
            return ((r >> 4) << 4) | (a >> 4);
        default:
            return (r << 24) | (g << 16) | (b << 8) | a; /* The GPU's RGBA8 has R in the highest byte */
    }
}

static inline unsigned int gfx_texel_decode(unsigned int v, int format) {
    unsigned int r, g, b, a;
    switch (format) {
        case GFX_TEXFMT_RGBA5551:
            r = (v >> 11) & 0x1F;
            g = (v >> 6) & 0x1F;
            b = (v >> 1) & 0x1F;
            r = (r << 3) | (r >> 2);
            g = (g << 3) | (g >> 2);
            b = (b << 3) | (b >> 2);
            a = (v & 1) ? 255 : 0;
            break;
        case GFX_TEXFMT_LA8:
            r = g = b = (v >> 8) & 0xFF;
            a = v & 0xFF;
            break;
        case GFX_TEXFMT_LA4:
            r = g = b = ((v >> 4) & 0xF) * 17;
            a = (v & 0xF) * 17;
            break;
        default:
            r = v >> 24;
            g = (v >> 16) & 0xFF;
            b = (v >> 8) & 0xFF;
            a = v & 0xFF;
            break;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* Bytes per texel. */
static inline unsigned int gfx_texel_bytes(int format) {
    return (format == GFX_TEXFMT_RGBA8) ? 4 : (format == GFX_TEXFMT_LA4) ? 1 : 2;
}

/*
 * Whether a texture gets mipmaps: bilinear, at least 16x16, and either opaque or repeating on both
 * axes. A texture with transparency that is clamped on an axis is drawn once along it, often on a polygon much bigger
 * than its shape (kart shadows, the start line, track borders): its shape fades out before the edge texels that
 * the clamp stretches over the rest of the polygon. Smaller levels average the shape into those edge texels, so the
 * GPU spread it over the whole polygon: shadows became dark rectangles and start lines red and yellow stripes.
 */
static inline int gfx_wants_mipmaps(const GfxTexture *tex) {
    unsigned int i, n = tex->width * tex->height;
    if (tex->filter == GFX_FILTER_POINT || tex->width < 16 || tex->height < 16 || tex->atlas) {
        return 0;
    }
    if (tex->wrapS != GFX_WRAP_CLAMP && tex->wrapT != GFX_WRAP_CLAMP) {
        return 1;
    }
    for (i = 0; i < n; i++) {
        if ((tex->pixels[i] >> 24) != 0xFF) {
            return 0;
        }
    }
    return 1;
}

/*
 * The mipmap level bias of a texture that gets mipmaps, in levels: negative reads sharper ones. A
 * texture clamped on both axes is drawn once across its polygon: the signs with track names, door numbers, banners,
 * faces and eyes. Text there turned to mush with mipmaps, especially at an angle, where the GPU picks the level for
 * the more squashed direction, and such textures barely shimmer anyway as they don't repeat into the distance. So they
 * switch to each smaller level `signBias` levels later (the backends' setting, -1 by default); textures that repeat
 * (ground, walls, water), where shimmer is what mipmaps are for, keep the plain choice.
 */
static inline float gfx_mip_lod_bias(const GfxTexture *tex, float signBias) {
    return (tex->wrapS == GFX_WRAP_CLAMP && tex->wrapT == GFX_WRAP_CLAMP) ? signBias : 0.0f;
}

/*
 * The next smaller mipmap level: each 2x2 block of src (w x h, RGBA8888) averaged into one pixel of
 * dst ((w / 2) x (h / 2), which may be src itself). Colours are weighted by alpha, so the colour hidden under
 * transparent texels (often black) doesn't darken the edges of cutouts such as trees and fences.
 */
static inline void gfx_mip_downsample(const unsigned int *src, unsigned int w, unsigned int h, unsigned int *dst) {
    unsigned int x, y, i;
    for (y = 0; y < h / 2; y++) {
        for (x = 0; x < w / 2; x++) {
            unsigned int c[4], sum[3] = { 0, 0, 0 }, plain[3] = { 0, 0, 0 }, alpha = 0, k;
            c[0] = src[(2 * y) * w + 2 * x];
            c[1] = src[(2 * y) * w + 2 * x + 1];
            c[2] = src[(2 * y + 1) * w + 2 * x];
            c[3] = src[(2 * y + 1) * w + 2 * x + 1];
            for (i = 0; i < 4; i++) {
                unsigned int a = c[i] >> 24;
                for (k = 0; k < 3; k++) {
                    unsigned int v = (c[i] >> (8 * k)) & 0xFF;
                    sum[k] += v * a;
                    plain[k] += v;
                }
                alpha += a;
            }
            for (k = 0; k < 3; k++) {
                sum[k] = alpha > 0 ? (sum[k] + alpha / 2) / alpha : (plain[k] + 2) / 4;
            }
            dst[y * (w / 2) + x] = sum[0] | (sum[1] << 8) | (sum[2] << 16) | (((alpha + 2) / 4) << 24);
        }
    }
}

#endif
