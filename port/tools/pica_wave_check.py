"""
The wave shader's check for pica_sim.py (custom shader plan P65 rank 3, DKR_WAVE_SHADER): waves.v.pica, run on the
simulated PICA200, against the game's own sums for a wave vertex (src/waves.c func_800B92F4, xlu water, no wave
generators) written out here in Python with the C types' behaviour (float32 sums, truncating casts, the >> 7).

Random tiles over the settings the game's levels use (table sizes 120-186, 2-8 subdivisions, all nine scale table
rows, every shade, colour bytes over 0-255). The shader works in the PICA's 24-bit floats and the game in 32-bit
ones, so a height whose product lands within a rounding step of a whole number can truncate the other way: a vertex
counts as differing when its stored height or a colour byte is off at all, and the check fails when more than 1% of
vertices differ, or any by more than 1 height unit / one colour step (4). Rule K6: the same check with the index
offset broken (off + 1, the step every index moves by) must report most vertices.
"""
import math
import random
import struct


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def trunc(x):
    return int(x)  # Python's int() truncates toward zero, as C's cast does


SCALE_ROWS = {
    # row: (ai, aj, bi, bj): the scale table index is min(i * ai + bi, j * aj + bj) (gfx_waves.h wave_scale_select)
    4: lambda s: (0, 0, 100, 100),
    1: lambda s: (1, 0, 0, 100),
    3: lambda s: (0, 1, 100, 0),
    5: lambda s: (0, -1, 100, s),
    7: lambda s: (-1, 0, s, 100),
    0: lambda s: (1, 1, 0, 0),
    2: lambda s: (1, -1, 0, s),
    6: lambda s: (-1, 1, s, 0),
    8: lambda s: (-1, -1, s, s),
}


def scale_table(sub):
    """func_800BC6C8's sp34 (sins_f taken as the exact sine), padded with 1.0 to 12 entries."""
    sp = [f32(math.sin((i * (0x4000 // sub)) / 65536.0 * 2 * math.pi)) for i in range(sub)]
    sp[0] = 0.0
    sp.append(1.0)
    while len(sp) < 12:
        sp.append(1.0)
    return sp


def scale_rows_c(sub, sp):
    """D_800E304C's nine rows as func_800BC6C8 fills them (the game's own loops)."""
    n = sub + 1
    rows = [[0.0] * (n * n) for _ in range(9)]
    for i in range(n):
        for j in range(n):
            rows[4][i * n + j] = 1.0
            rows[1][i * n + j] = sp[i]
            rows[3][i * n + j] = sp[j]
            rows[5][i * n + j] = sp[sub - j]
            rows[7][i * n + j] = sp[sub - i]
    for i in range(n):
        for j in range(i, n):
            rows[0][i * n + j] = sp[i]
            rows[0][j * n + i] = sp[i]
            rows[2][i * n + sub - j] = sp[i]
            rows[2][j * n + sub - i] = sp[i]
            rows[6][sub * n - i * n + j] = sp[i]
            rows[6][sub * n - j * n + i] = sp[i]
            rows[8][sub * n - i * n + sub - j] = sp[i]
            rows[8][sub * n - j * n + sub - i] = sp[i]
    return rows


def game_vertex(T, i0, i1, mag, scale, c, unk44, temp, unk48, shade):
    """One vertex as func_800B92F4 does it: (stored height, (r, g, b, a))."""
    y = f32(f32(T[i0] + T[i1]) * mag)
    y = f32(y * scale)
    v = c << 1
    if v < 255:
        y = f32(y * f32(unk44 + f32(v * temp)))
    v += trunc(f32(y * unk48))
    v = 255 if v > 255 else 0 if v < 0 else v
    v += ((255 - v) * shade) >> 7
    rgb = 255 if v < 192 else ((255 - v) * 4) & 255
    a = ((v * 4) & 255) if v < 64 else 255
    return trunc(y), (rgb, rgb, rgb, a)


def run(sim, argv):
    wave_i = None
    for i, d in enumerate(sim.dvles):
        if not d.geo and "wavea" in d.uniforms:
            wave_i = i
    if wave_i is None:
        print("wave shader: not in the blob")
        return True
    ok = True
    for broken in (False, True):
        rng = random.Random(77)
        total = differ = worst_y = worst_c = 0
        for tile in range(60):
            S = rng.choice([120, 130, 156, 178, 186])
            sub = rng.choice([2, 3, 4, 6, 8])
            k1, k2 = rng.choice([1, 2, 4]), rng.choice([1, 2])
            h1, h2 = rng.uniform(2, 20), rng.uniform(2, 20)
            dd = rng.random() < 0.5
            T = []
            for n in range(S):
                t = f32(f32(math.sin(((k1 << 16) // S) * n / 65536.0 * 2 * math.pi)) * h1 +
                        f32(h2 * math.sin(((k2 << 16) // S) * n / 65536.0 * 2 * math.pi)))
                T.append(f32(t * 2.0) if dd else t)
            off = rng.randrange(S)
            mag = f32(rng.uniform(0.3, 2.0))
            unk44 = f32(rng.uniform(0.0, 1.0))
            temp = f32((1.0 - unk44) / 255.0)
            unk48 = f32(rng.uniform(0.5, 12.0))
            shade = rng.randrange(0, 129)
            row = rng.randrange(9)
            sp = scale_table(sub)
            rows = scale_rows_c(sub, sp)
            ai, aj, bi, bj = SCALE_ROWS[row](sub)
            wtab = [T[4 * r:4 * r + 4] + [0.0] * (4 - len(T[4 * r:4 * r + 4])) for r in range(47)]
            wsc = [sp[0:4], sp[4:8], sp[8:12]]
            uniforms = {
                "eye": (0, 0, 0, 0),
                "mtx": [(1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1)],
                "camoff": (0, 0, 0, 0),
                "texa": (1, 1, 1, 1), "texb": (0, 0, 0, 0),
                "wavea": ((off + 1) % S if broken else off, S, 1.0 / S, mag),
                "waveb": (unk44, temp, unk48, shade / 128.0),
                "wavec": (ai, aj, bi, bj),
                "waved": (rng.randrange(0, 4096), rng.randrange(0, 4096), rng.randrange(1, 900), rng.randrange(1, 900)),
                "stab": wtab + wsc + [(0.0, 0.0, 0.0, 0.0)] * (75 - 47 - 3),
            }
            for i in range(sub + 1):
                for j in range(sub + 1):
                    a0, a1 = rng.randrange(S), rng.randrange(S)
                    c = rng.randrange(256) if rng.random() < 0.3 else rng.randrange(128)
                    y, rgba = game_vertex(T, (a0 + off) % S, (a1 + off) % S, mag, rows[row][i * (sub + 1) + j], c,
                                          unk44, temp, unk48, shade)
                    x, z = j * 37, i * 41
                    out = sim.run(wave_i, {0: (x, a0, z, a1), 1: (j, i, 0, 0), 2: (0, 0, 0, 0), 3: (c, 0, 0, 0)},
                                  uniforms)
                    pos, col = out[0], out[2]
                    sy = int(round(pos[1]))
                    scol = tuple(int(round(v * 255.0)) for v in col)
                    uv = out[3]
                    want_u = uniforms["waved"][0] + j * uniforms["waved"][2]
                    want_v = uniforms["waved"][1] + i * uniforms["waved"][3]
                    total += 1
                    dy = abs(sy - y)
                    dc = max(abs(scol[k] - rgba[k]) for k in range(4))
                    if pos[0] != x or abs(uv[0] - want_u) > 0.5 or abs(uv[1] - want_v) > 0.5:
                        dc = max(dc, 999)
                    if dy or dc:
                        differ += 1
                        worst_y = max(worst_y, dy)
                        worst_c = max(worst_c, dc)
                        if differ <= 3 and not broken:
                            print("  differs: table %d sub %d row %d c %d: game y %d %s, shader y %s %s" %
                                  (S, sub, row, c, y, rgba, pos[1], scol))
        share = differ / float(total)
        if broken:
            print("  broken on purpose (K6: index offset + 1): %d of %d vertices differ" % (differ, total))
            ok &= share > 0.2
        else:
            print("wave shader (DVLE %d) against the game's sums: %d vertices, %d differ (%.2f%%), worst height %d, "
                  "worst colour %d" % (wave_i, total, differ, 100.0 * share, worst_y, worst_c))
            ok &= share <= 0.01 and worst_y <= 1 and worst_c <= 4
    return ok
