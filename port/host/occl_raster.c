/*
 * The hidden track batch rasteriser (occl_raster.h), moved out of occl_count.c (PR #602) so the P67 bake shares it.
 */
#include <string.h>
#include "occl_raster.h"

typedef struct {
    short x, y, z;
    unsigned char r, g, b, a;
} OcrVertex; /* structs.h Vertex */

typedef struct {
    unsigned char flags, vi0, vi1, vi2;
    unsigned int uv[3];
} OcrTriangle; /* structs.h Triangle */

void ocr_begin(OcrCtx *c, const float viewProj[16]) {
    memcpy(c->m, viewProj, sizeof(c->m));
    memset(c->depth, 0, sizeof(float) * c->w * c->h);
    if (c->mask != NULL) {
        memset(c->mask, 0, (size_t) c->w * c->h);
    }
    c->testDepth = c->depth;
}

void ocr_to_clip(const OcrCtx *c, const short *xyz, OcrClip *o) {
    float x = xyz[0], y = xyz[1], z = xyz[2];
    o->x = x * c->m[0][0] + y * c->m[1][0] + z * c->m[2][0] + c->m[3][0];
    o->y = x * c->m[0][1] + y * c->m[1][1] + z * c->m[2][1] + c->m[3][1];
    o->z = x * c->m[0][2] + y * c->m[1][2] + z * c->m[2][2] + c->m[3][2];
    o->w = x * c->m[0][3] + y * c->m[1][3] + z * c->m[2][3] + c->m[3][3];
}

/* Rasterizes one triangle (screen x, y and 1/w at its corners), as ocr_triangle's modes. */
static int raster(const OcrCtx *c, const float *sx, const float *sy, const float *iw, int mode) {
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    float minXf, maxXf, minYf, maxYf, inv, slack[3] = { 0.0f, 0.0f, 0.0f }, iwMax;
    int minX, maxX, minY, maxY, x, y, covered = 0, k;
    const int W = c->w, H = c->h;
    if (area == 0.0f) {
        return 0;
    }
    minXf = sx[0] < sx[1] ? sx[0] : sx[1]; minXf = minXf < sx[2] ? minXf : sx[2];
    maxXf = sx[0] > sx[1] ? sx[0] : sx[1]; maxXf = maxXf > sx[2] ? maxXf : sx[2];
    minYf = sy[0] < sy[1] ? sy[0] : sy[1]; minYf = minYf < sy[2] ? minYf : sy[2];
    maxYf = sy[0] > sy[1] ? sy[0] : sy[1]; maxYf = maxYf > sy[2] ? maxYf : sy[2];
    iwMax = iw[0] > iw[1] ? iw[0] : iw[1];
    iwMax = iwMax > iw[2] ? iwMax : iw[2];
    if (mode == 1 && c->widen > 0.0f) {
        // Each corner's weight may go this far below 0 (the pixel centre within widen pixels of the opposite edge)
        for (k = 0; k < 3; k++) {
            float ex = sx[(k + 2) % 3] - sx[(k + 1) % 3], ey = sy[(k + 2) % 3] - sy[(k + 1) % 3];
            float len = ex * ex + ey * ey;
            slack[k] = len > 0.0f ? c->widen * __builtin_sqrtf(len) / (area < 0.0f ? -area : area) : 0.0f;
        }
        minXf -= c->widen;
        minYf -= c->widen;
        maxXf += c->widen;
        maxYf += c->widen;
    }
    if (maxXf < 0.0f || maxYf < 0.0f || minXf > (float) W || minYf > (float) H) {
        return 0;
    }
    minX = minXf < 0.0f ? 0 : (int) minXf;
    maxX = maxXf >= (float) W ? W - 1 : (int) maxXf;
    minY = minYf < 0.0f ? 0 : (int) minYf;
    maxY = maxYf >= (float) H ? H - 1 : (int) maxYf;
    inv = 1.0f / area;
    for (y = minY; y <= maxY; y++) {
        float py = y + 0.5f;
        for (x = minX; x <= maxX; x++) {
            float px = x + 0.5f;
            float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) * inv;
            float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) * inv;
            float w2 = 1.0f - w0 - w1, d;
            float *dst;
            int inside = w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f;
            if (!inside && !(mode == 1 && c->widen > 0.0f && w0 >= -slack[0] && w1 >= -slack[1] && w2 >= -slack[2])) {
                continue;
            }
            // A widened pixel outside the triangle takes the triangle's nearest corner depth
            d = inside ? w0 * iw[0] + w1 * iw[1] + w2 * iw[2] : iwMax;
            dst = &c->depth[y * W + x];
            if (mode != 1) {
                if (c->pixels != NULL) {
                    (*c->pixels)++;
                }
                if (c->mask != NULL) {
                    c->mask[y * W + x] = 1;
                }
            }
            if (mode == 0) {
                if (d > *dst) {
                    *dst = d;
                }
            } else if (mode == 2) {
                continue;
            } else {
                covered = 1;
                if (d >= c->testDepth[y * W + x] * OCR_DEPTH_SPARE) {
                    if (c->visit == NULL) {
                        return 2;
                    }
                    if (c->visit(c->user, x, y, d)) {
                        return 2;
                    }
                }
            }
        }
    }
    return covered;
}

int ocr_triangle(const OcrCtx *c, const OcrClip *clip, const unsigned char *t, int numVerts, int mode) {
    OcrClip in[3], mid[4], out[5];
    int n = 0, m, i, k, best = 0;
    float sx[5], sy[5], iw[5], area = 0.0f;
    if (t[1] >= numVerts || t[2] >= numVerts || t[3] >= numVerts) {
        return 0;
    }
    in[0] = clip[t[1]];
    in[1] = clip[t[2]];
    in[2] = clip[t[3]];
    for (i = 0; i < 3; i++) {
        const OcrClip *a = &in[i], *b = &in[(i + 1) % 3];
        float da = a->z + a->w, db = b->z + b->w;
        if (da >= 0.0f) {
            mid[n++] = *a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float s = da / (da - db);
            mid[n].x = a->x + (b->x - a->x) * s;
            mid[n].y = a->y + (b->y - a->y) * s;
            mid[n].z = a->z + (b->z - a->z) * s;
            mid[n].w = a->w + (b->w - a->w) * s;
            n++;
        }
    }
    if (n < 3 || (c->breakNear && n != 3)) {
        return 0; // (occl_count's DKR_OCCL_BREAK: triangles cut by the near plane are left out instead)
    }
    if (c->farCut) {
        // The far plane too (z = w): what lies beyond it is never drawn, so it hides nothing
        m = 0;
        for (i = 0; i < n; i++) {
            const OcrClip *a = &mid[i], *b = &mid[(i + 1) % n];
            float da = a->w - a->z, db = b->w - b->z;
            if (da >= 0.0f) {
                out[m++] = *a;
            }
            if ((da >= 0.0f) != (db >= 0.0f)) {
                float s = da / (da - db);
                out[m].x = a->x + (b->x - a->x) * s;
                out[m].y = a->y + (b->y - a->y) * s;
                out[m].z = a->z + (b->z - a->z) * s;
                out[m].w = a->w + (b->w - a->w) * s;
                m++;
            }
        }
        if (m < 3) {
            return 0;
        }
        n = m;
    } else {
        for (i = 0; i < n; i++) {
            out[i] = mid[i];
        }
    }
    for (i = 0; i < n; i++) {
        float w = out[i].w > 1e-6f ? out[i].w : 1e-6f;
        sx[i] = (out[i].x / w * 0.5f + 0.5f) * c->w;
        sy[i] = (0.5f - out[i].y / w * 0.5f) * c->h;
        iw[i] = 1.0f / w;
    }
    for (i = 0; i < n; i++) {
        k = (i + 1) % n;
        area += sx[i] * sy[k] - sx[k] * sy[i];
    }
    if (!(t[0] & 0x40) && c->cull != 0 && (area * (float) c->cull) < 0.0f) {
        return 0; // A back face
    }
    for (i = 1; i + 1 < n; i++) {
        float fx[3] = { sx[0], sx[i], sx[i + 1] }, fy[3] = { sy[0], sy[i], sy[i + 1] },
              fw[3] = { iw[0], iw[i], iw[i + 1] };
        int r = raster(c, fx, fy, fw, mode);
        if (r > best) {
            best = r;
            if (best == 2 && mode == 1) {
                break;
            }
        }
    }
    return best;
}

int ocr_batch(const OcrCtx *c, const void *verts, int numVerts, const void *tris, int numTris, int mode) {
    OcrClip clip[OCR_MAX_VERTS];
    const OcrVertex *v = verts;
    const OcrTriangle *t = tris;
    int i, best = 0;
    if (numVerts <= 0 || numVerts > OCR_MAX_VERTS) {
        return 0;
    }
    for (i = 0; i < numVerts; i++) {
        ocr_to_clip(c, &v[i].x, &clip[i]);
    }
    for (i = 0; i < numTris; i++) {
        int r = ocr_triangle(c, clip, &t[i].flags, numVerts, mode);
        if (r > best) {
            best = r;
            if (best == 2 && mode == 1) {
                break;
            }
        }
    }
    return best;
}

void ocr_erode(OcrCtx *c, float *eroded) {
    const int W = c->w, H = c->h;
    int x, y, dx, dy;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            float d = c->depth[y * W + x];
            for (dy = -1; dy <= 1 && d > 0.0f; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    float e = (xx < 0 || yy < 0 || xx >= W || yy >= H) ? d : c->depth[yy * W + xx];
                    d = e < d ? e : d;
                }
            }
            eroded[y * W + x] = d;
        }
    }
    c->testDepth = eroded;
}

/* The radius (pixels) eps moves an edge by at pixel (x, y) of depth d (ocr_shrink's rule). */
static inline int shrink_radius(float eps, float d, float u, float v, int W) {
    float rf = eps * d * (1.0f + u * u + v * v) * (float) W * 0.5f;
    return rf >= OCR_SHRINK_MAX ? OCR_SHRINK_MAX : (int) rf + 1;
}

void ocr_shrink(OcrCtx *c, float *eroded, float *tab, float eps) {
    const int W = c->w, H = c->h;
    const float *L[OCR_SHRINK_LEVELS];
    int k, x, y, maxN = 1, levels;
    // Only the pyramid levels some pixel needs (the bake's faces of open sky or far scenery need few)
    for (y = 0; y < H && maxN < 2 * OCR_SHRINK_MAX + 1; y++) {
        float v = 1.0f - (y + 0.5f) * 2.0f / H;
        for (x = 0; x < W; x++) {
            float d = c->depth[y * W + x];
            if (d > 0.0f) {
                int n = 2 * shrink_radius(eps, d, (x + 0.5f) * 2.0f / W - 1.0f, v, W) + 1;
                maxN = n > maxN ? n : maxN;
            }
        }
    }
    for (levels = 1; (2 << (levels - 1)) <= maxN && levels < OCR_SHRINK_LEVELS; levels++) {
    }
    L[0] = c->depth;
    // L[k] (x, y): the farthest (smallest 1/w) of the 2^k square from (x, y), clamped at the far edges. Built as a
    // row pass then a column pass in place (row y only reads rows y and y + st, not yet written); same values as the
    // four-corner minimum.
    for (k = 1; k < levels; k++) {
        float *o = tab + (size_t) (k - 1) * W * H;
        const float *in = L[k - 1];
        int st = 1 << (k - 1);
        for (y = 0; y < H; y++) {
            const float *r = in + (size_t) y * W;
            float *q = o + (size_t) y * W;
            for (x = 0; x + st < W; x++) {
                q[x] = r[x + st] < r[x] ? r[x + st] : r[x];
            }
            for (; x < W; x++) {
                q[x] = r[W - 1] < r[x] ? r[W - 1] : r[x];
            }
        }
        for (y = 0; y < H; y++) {
            float *q = o + (size_t) y * W;
            const float *q2 = o + (size_t) (y + st < H ? y + st : H - 1) * W;
            for (x = 0; x < W; x++) {
                q[x] = q2[x] < q[x] ? q2[x] : q[x];
            }
        }
        L[k] = o;
    }
    for (y = 0; y < H; y++) {
        float v = 1.0f - (y + 0.5f) * 2.0f / H;
        for (x = 0; x < W; x++) {
            float u = (x + 0.5f) * 2.0f / W - 1.0f, d = c->depth[y * W + x], m;
            int r, n, x0, y0, s;
            if (d <= 0.0f) {
                eroded[y * W + x] = 0.0f;
                continue;
            }
            // The angle eps moves an edge by at this depth, over the smallest angle a pixel takes here
            r = shrink_radius(eps, d, u, v, W);
            n = 2 * r + 1;
            x0 = x - r;
            y0 = y - r;
            x0 = x0 < 0 ? 0 : x0 + n > W ? W - n : x0;
            y0 = y0 < 0 ? 0 : y0 + n > H ? H - n : y0;
            for (k = 0; (2 << k) <= n && k + 1 < OCR_SHRINK_LEVELS; k++) {
            }
            s = n - (1 << k);
            {
                const float *t = L[k];
                float a = t[y0 * W + x0], b = t[y0 * W + x0 + s], e = t[(y0 + s) * W + x0], f = t[(y0 + s) * W + x0 + s];
                m = a < b ? a : b;
                m = e < m ? e : m;
                m = f < m ? f : m;
            }
            eroded[y * W + x] = m;
        }
    }
    c->testDepth = eroded;
}
