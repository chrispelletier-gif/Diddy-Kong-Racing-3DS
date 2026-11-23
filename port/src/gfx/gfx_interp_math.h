#ifndef GFX_INTERP_MATH_H
#define GFX_INTERP_MATH_H
/*
 * The 60 Hz picture (DKR_INTERP60, S8 / P39): the 3DS backend's conversion of the walk's delta into the 3DS screen's
 * clip space, and its self-check. Shared so the host build (gfx_soft.c) runs the same check (DKR_INTERP60_CHECK) on
 * every delta it gets, without a 3DS.
 *
 * The walk's delta D works on gfx_backend.h's clip coordinates as a row: clip' = clip D. The 3DS backend's to_screen
 * makes pos = clip S (x' = y, y' = -s x, z' = (z - w) / 2, w' = w; s is its x scale, its z clamp is the shader's and
 * comes after the delta), so pos' = pos S^-1 D S. out[c] is column c of that (the shader's dlt[c], used with dp4).
 * breakIt (DKR_INTERP60_CHECK=2, rule K6) leaves S^-1 out, which the check must report.
 */
#include <math.h>
#include <string.h>

static inline void gfx_interp_to_screen(const float d[4][4], float s, float out[4][4], int breakIt) {
    float si[4][4] = { { 0 } }, sm[4][4] = { { 0 } }, t[4][4];
    int r, c, k;
    si[1][0] = -1.0f / s; // x = -pos1 / s
    si[0][1] = 1.0f;      // y = pos0
    si[2][2] = 2.0f;      // z = 2 pos2 + pos3
    si[3][2] = 1.0f;
    si[3][3] = 1.0f; // w = pos3
    if (breakIt) {
        memset(si, 0, sizeof(si));
        for (k = 0; k < 4; k++) {
            si[k][k] = 1.0f;
        }
    }
    sm[1][0] = 1.0f;
    sm[0][1] = -s;
    sm[2][2] = 0.5f;
    sm[3][2] = -0.5f;
    sm[3][3] = 1.0f;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            float v = 0.0f;
            for (k = 0; k < 4; k++) {
                v += si[r][k] * d[k][c];
            }
            t[r][c] = v;
        }
    }
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            float v = 0.0f;
            for (k = 0; k < 4; k++) {
                v += t[r][k] * sm[k][c];
            }
            out[c][r] = v; // Transposed: out[c] is the shader's dlt[c]
        }
    }
}

/* Three test vertices: moved by d and then put on the screen, against put on the screen and moved by rows (the
 * converted delta, as the shader uses it). Returns 1 if any differs. */
static inline int gfx_interp_screen_check(const float d[4][4], const float rows[4][4], float s) {
    static const float sTest[3][4] = { { 0.3f, -0.2f, 0.25f, 1.0f }, { -0.6f, 0.5f, -0.5f, 2.0f },
                                       { 0.1f, 0.7f, 0.9f, 4.0f } };
    int i, c, k, miss = 0;
    for (i = 0; i < 3; i++) {
        float moved[4], a[4], b, scr[4];
        for (c = 0; c < 4; c++) {
            moved[c] = 0.0f;
            for (k = 0; k < 4; k++) {
                moved[c] += sTest[i][k] * d[k][c];
            }
        }
        a[0] = moved[1];
        a[1] = -moved[0] * s;
        a[2] = 0.5f * (moved[2] - moved[3]);
        a[3] = moved[3];
        scr[0] = sTest[i][1];
        scr[1] = -sTest[i][0] * s;
        scr[2] = 0.5f * (sTest[i][2] - sTest[i][3]);
        scr[3] = sTest[i][3];
        for (c = 0; c < 4; c++) {
            b = rows[c][0] * scr[0] + rows[c][1] * scr[1] + rows[c][2] * scr[2] + rows[c][3] * scr[3];
            if (fabsf(a[c] - b) > 1e-3f * (1.0f + fabsf(a[c]))) {
                miss = 1;
            }
        }
    }
    return miss;
}
#endif
