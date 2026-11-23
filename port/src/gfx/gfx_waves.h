/*
 * Wave tiles worked out by the GPU's vertex shader (custom shader plan P65 rank 3, DKR_WAVE_SHADER; port/3ds/waves.v.pica).
 * Shared by the walk (gfx_rdp.c native_wave and its self-check), the PC renderer (host/gfx_soft.c, which does the
 * shader's sums in C) and the game side (waves.c port_wave_tile_mesh).
 *
 * A wave vertex in the mesh is a GfxModelVertex (so it lives in the backend's mesh arena with the model vertex format)
 * holding, instead of a position, texture coordinates and a colour:
 *   x, y, z, one = the grid x, the first height index (as the level started), the grid z, the second height index
 *   u, v = the vertex's column j and row i in the tile
 *   r = the colour table's byte; g, b, a = 0
 * The per tile values (PortWaveTile.unif, the shader's wavea..waved):
 *   [0..3]  off, S, 1 / S, magnitude
 *   [4..7]  unk44, (1 - unk44) / 255, unk48, shade / 128
 *   [8..11] ai, aj, bi, bj: the scale table index is min(i * ai + bi, j * aj + bj), clamped to 0..11
 *   [12..15] u base, v base, u step, v step (the game's texture units)
 */
#ifndef GFX_WAVES_H
#define GFX_WAVES_H

#define GFX_WAVE_TABLE_MAX 188 // Height table entries: stab rows 0..46 in waves.v.pica
#define GFX_WAVE_SCALES 12     // Scale table entries: stab rows 47..49

/* The scale table row selections of func_800BC6C8's nine rows (D_800E304C[row]), as (ai, aj, bi, bj); 100 stands for
 * "no limit" (clamped to 11, where the table holds 1.0). */
static inline void gfx_wave_scale_select(int row, int sub, float out[4]) {
    static const signed char sA[9][2] = { { 1, 1 }, { 1, 0 }, { 1, -1 }, { 0, 1 }, { 0, 0 },
                                          { 0, -1 }, { -1, 1 }, { -1, 0 }, { -1, -1 } };
    int ai = sA[row][0], aj = sA[row][1];
    out[0] = (float) ai;
    out[1] = (float) aj;
    out[2] = ai == 0 ? 100.0f : ai > 0 ? 0.0f : (float) sub;
    out[3] = aj == 0 ? 100.0f : aj > 0 ? 0.0f : (float) sub;
}

/*
 * One wave vertex as waves.v.pica works it out, in 32-bit floats and the game's own order of sums (src/waves.c
 * func_800B92F4), from its mesh vertex, the tile's values and the level's tables: its stored height (an s16, as the
 * game's Vertex.y) and colour. The game's result for the same vertex is the same bit for bit when the mesh and values
 * are right (the walk's self-check, DKR_WAVE_SHADER_CHECK); the GPU's 24-bit floats can land one unit or colour step
 * away now and then (port/tools/pica_wave_check.py).
 */
static inline void gfx_wave_eval(short x, short a0, short a1, int j, int i, int c, const float *u,
                                 const float *heights, const float *scales, short *y, unsigned char rgba[4]) {
    int S = (int) u[1], off = (int) u[0], v, k, idx;
    float vy, ri, rj;
    (void) x;
    k = a0 + off;
    k -= k >= S ? S : 0;
    idx = a1 + off;
    idx -= idx >= S ? S : 0;
    vy = (heights[k] + heights[idx]) * u[3];
    ri = (float) i * u[8] + u[10];
    rj = (float) j * u[9] + u[11];
    ri = ri < rj ? ri : rj;
    idx = ri < 0.0f ? 0 : ri > 11.0f ? 11 : (int) ri;
    vy *= scales[idx];
    v = c << 1;
    if (v < 255) {
        vy *= u[4] + (float) v * u[5];
    }
    v += (int) (vy * u[6]);
    v = v > 255 ? 255 : v < 0 ? 0 : v;
    v += ((255 - v) * (int) (u[7] * 128.0f + 0.5f)) >> 7;
    *y = (short) vy;
    rgba[0] = rgba[1] = rgba[2] = (unsigned char) (v < 192 ? 255 : ((255 - v) * 4) & 255);
    rgba[3] = (unsigned char) (v < 64 ? (v * 4) & 255 : 255);
}

#endif
