#ifndef OCCL_RASTER_H
#define OCCL_RASTER_H
/*
 * The small software rasteriser of the hidden track batch work (host only): the game's track triangles (Vertex and
 * Triangle as in structs.h) through a view-projection, kept facing the camera (or double sided) with the renderer's
 * back-face sign, cut at the near plane, rasterised as fans. Depth is 1/w (nearer is larger; 0: nothing there).
 * Used by the hidden batch count (occl_count.c, DKR_OCCL_COUNT, PR #602) and the P67 bake (hidden_bake.c). Moved out of
 * occl_count.c unchanged; the bake's additions (far cut, eroded depth, widened test coverage, a callback per pixel)
 * are off unless a context asks for them. Each context has its own buffers, so threads can use one each.
 */
#define OCR_DEPTH_SPARE 0.999f

typedef struct {
    float x, y, z, w;
} OcrClip;

typedef struct OcrCtx {
    int w, h;
    float *depth;          /* w * h: 1/w of the nearest occluder pixel; 0: none */
    const float *testDepth; /* the depth tests compare against (depth, or ocr_erode's) */
    unsigned char *mask;   /* optional: pixels drawn (mode 0 and 2) are set to 1 */
    unsigned long long *pixels; /* optional: pixels drawn (mode 0 and 2) are counted */
    float m[4][4];         /* view-projection, row vector convention (as gViewProjMatrixF) */
    int cull;              /* sign of the faces kept: -1 as the renderer, 1, 0 keeps both */
    int breakNear;         /* occl_count's K6 break: triangles cut by the near plane are left out */
    int farCut;            /* also cut at z = w (the bake's occluders) */
    float widen;           /* tests: a pixel counts if its centre is within this many pixels outside the triangle */
    /* tests: called for each pixel in front of the depth (with its 1/w); returning nonzero stops the batch */
    int (*visit)(void *user, int x, int y, float depth);
    void *user;
} OcrCtx;

/* Clears the depth (and the mask) and sets the matrix. */
void ocr_begin(OcrCtx *c, const float viewProj[16]);
void ocr_to_clip(const OcrCtx *c, const short *xyz, OcrClip *out);
/*
 * One triangle (flags, vi0, vi1, vi2; clip-space corners of its batch). mode 0 writes depth (nearest 1/w), mode 2 only
 * marks its pixels, mode 1 tests: returns 2 at its first pixel in front of the test depth (with OCR_DEPTH_SPARE; with
 * a visit callback, each such pixel is visited and 2 means the callback stopped it), else 1 if it covered any pixel,
 * else 0.
 */
int ocr_triangle(const OcrCtx *c, const OcrClip *clip, const unsigned char *tri, int numVerts, int mode);
/* A whole batch (verts: Vertex, 10 bytes each; tris: Triangle, 16 bytes each). Returns the highest of its triangles
 * (mode 1 stops at the first 2). numVerts at most OCR_MAX_VERTS. */
#define OCR_MAX_VERTS 256
int ocr_batch(const OcrCtx *c, const void *verts, int numVerts, const void *tris, int numTris, int mode);
/* The bake's margin: each pixel's test depth becomes the farthest of its 3x3 neighbourhood's (empty if any is), so a
 * pixel next to an occluder's edge counts as uncovered. Writes into eroded (w * h) and makes it the test depth. */
void ocr_erode(OcrCtx *c, float *eroded);
/*
 * The bake's occluder shrinking, so that one sample position stands for every eye within eps units of it (Wonka et
 * al. 2000: occluders shrunk by eps): each pixel's test depth becomes the farthest within the angle an edge eps units
 * away moves by at the pixel's own depth (eps / distance, as pixels of a 90 degree face; at least 1, at most
 * OCR_SHRINK_MAX). The square is kept inside the face (moved in at the edges). tab: (OCR_SHRINK_LEVELS - 1) * w * h
 * floats of scratch. Writes into eroded and makes it the test depth.
 */
#define OCR_SHRINK_LEVELS 8
#define OCR_SHRINK_MAX 127 /* squares of at most 255, two 128 tiles */
void ocr_shrink(OcrCtx *c, float *eroded, float *tab, float eps);

#endif
