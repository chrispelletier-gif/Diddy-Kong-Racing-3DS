# Hand-built pixel numerals for the large number size, from bars and evenly stepped diagonals.
# W cell ink width, H rows, s vertical stroke, t horizontal stroke, cr corner cut (1 or 2)
def numerals(W, H, s=3, t=2, cr=1, face=None, bar4=None, slant=False):
    def blank(): return [[0] * W for _ in range(H)]
    def fill(c, y0, y1, x0, x1):
        for y in range(max(0, y0), min(H, y1)):
            for x in range(max(0, x0), min(W, x1)): c[y][x] = 1
    def diag(c, y0, y1, xa, xb):  # stroke s wide; left edge xa at row y0 to xb at row y1, even steps
        n = y1 - y0
        for k in range(n + 1):
            x = xa + (xb - xa) * k // n if xb < xa else xa + ((xb - xa) * k + n - 1) // n
            fill(c, y0 + k, y0 + k + 1, x, x + s)
    def down(c, y1, xa, xb, rpc=1):  # stroke s wide, left edge xa..xb (xa > xb), ending at row y1; returns its top row
        n = (xa - xb) * rpc
        y0 = y1 - n
        for k in range(n + 1):
            fill(c, y0 + k, y0 + k + 1, xa - k // rpc, xa - k // rpc + s)
        return y0
    def corners(c, pts):
        for (y, x, side) in pts:  # side: 'tl','tr','bl','br' cut cr pixels diagonally
            for k in range(cr):
                for j in range(cr - k):
                    yy = y + k if side[0] == 't' else y - k
                    xx = x + j if side[1] == 'l' else x - j
                    c[yy][xx] = 0
    R = W - s; B = H - t; M = (H - t) // 2
    G = {}
    c = blank(); fill(c, 0, t, 0, W); fill(c, B, H, 0, W); fill(c, 0, H, 0, s); fill(c, 0, H, R, W)
    corners(c, [(0, 0, 'tl'), (0, W - 1, 'tr'), (H - 1, 0, 'bl'), (H - 1, W - 1, 'br')]); G['0'] = c
    # 1: stem right of centre, a 2-px flag stepping down-left, a foot
    c = blank(); sx = (W - s + 1) // 2; fill(c, 0, B, sx, sx + s)
    fl = max(2, s - 1)  # the flag: two pixels wide, a column a row down to the left
    for k in range(1, fl + 1): fill(c, k, k + 1, sx - k - 1, sx - k + 1)
    if not slant: fill(c, B, H, 1, W - 1)  # (Barlow's 1 has no foot)
    else: fill(c, B, H, sx, sx + s)
    G['1'] = c
    # 2
    c = blank(); fill(c, 0, t, 0, W); fill(c, t, t + 1, 0, s)
    corners(c, [(0, 0, 'tl'), (0, W - 1, 'tr')])
    y0 = down(c, B - 1, R - 1, 0); fill(c, t, y0, R, W); fill(c, B, H, 0, W); G['2'] = c
    # 3
    c = blank(); fill(c, 0, t, 0, W); fill(c, M, M + t, s, W); fill(c, B, H, 0, W); fill(c, 0, H, R, W)
    corners(c, [(0, W - 1, 'tr'), (H - 1, W - 1, 'br')]); G['3'] = c
    # 4: the bar at about two thirds, the stem 3 rows below it at most
    c = blank(); b = bar4 if bar4 is not None else H - t - max(3, int(round(H * 0.19))); sx = W - s - 1
    fill(c, 0, H, sx, sx + s); fill(c, b, b + t, 0, W)
    rpc = max(1, (b - 1) // sx); y0 = down(c, sx * rpc, sx, 0, rpc); fill(c, sx * rpc, b, 0, s); G['4'] = c
    # 5
    c = blank(); fill(c, 0, t, 0, W); fill(c, 0, M + t, 0, s); fill(c, M, M + t, 0, W); fill(c, M, H, R, W)
    fill(c, B, H, 0, W); fill(c, B - 1, B, 0, s)
    corners(c, [(M, W - 1, 'tr'), (H - 1, 0, 'bl'), (H - 1, W - 1, 'br')]); G['5'] = c
    # 6
    c = blank(); fill(c, 0, t, 0, W); fill(c, 0, H, 0, s); fill(c, M, M + t, 0, W); fill(c, M, H, R, W); fill(c, B, H, 0, W)
    corners(c, [(0, 0, 'tl'), (0, W - 1, 'tr'), (M, W - 1, 'tr'), (H - 1, 0, 'bl'), (H - 1, W - 1, 'br')]); G['6'] = c
    # 7
    c = blank(); fill(c, 0, t, 0, W)
    v = H - 6 if not slant else H - 1  # the diagonal ends in a short upright (Barlow: it slants to the baseline)
    sx = (W - s) // 2
    if slant: sx = max(1, (W - s) // 2 - 2)
    rpc = max(1, (v - t - 2) // (R - 1 - sx)); y0 = down(c, v, R - 1, sx, rpc); fill(c, t, y0, R, W); fill(c, v, H, sx, sx + s); G['7'] = c
    # 8
    c = blank(); fill(c, 0, t, 0, W); fill(c, M, M + t, 0, W); fill(c, B, H, 0, W); fill(c, 0, H, 0, s); fill(c, 0, H, R, W)
    corners(c, [(0, 0, 'tl'), (0, W - 1, 'tr'), (H - 1, 0, 'bl'), (H - 1, W - 1, 'br'), (M, 0, 'tl'), (M, W - 1, 'tr'), (M + t - 1, 0, 'bl'), (M + t - 1, W - 1, 'br')])
    G['8'] = c
    # 9
    c = blank(); fill(c, 0, t, 0, W); fill(c, 0, M + t, 0, s); fill(c, M, M + t, 0, W); fill(c, 0, H, R, W); fill(c, B, H, 0, W)
    corners(c, [(0, 0, 'tl'), (0, W - 1, 'tr'), (M + t - 1, 0, 'bl'), (H - 1, 0, 'bl'), (H - 1, W - 1, 'br')]); G['9'] = c
    if slant:
        c = blank(); fill(c, 0, t, 0, W); fill(c, B, H, 0, W); fill(c, 0, M, R, W); fill(c, M + t, H, R, W)
        fill(c, t, t + max(1, t // 2), 0, s); fill(c, B - max(1, t // 2), B, 0, s)
        fill(c, M, M + t, (W - s) // 2, W - 1)
        corners(c, [(0, 0, 'tl'), (0, W - 1, 'tr'), (H - 1, 0, 'bl'), (H - 1, W - 1, 'br')]); G['3'] = c
        c = G['5']; corners(c, [(M, W - 1, 'tr')])
        c = G['6']; fill(c, t, t + max(1, t // 2), R, W)
        c = G['9']; fill(c, B - max(1, t // 2), B, 0, s)
    out = {k: [''.join('#' if v else '.' for v in r) for r in g] for k, g in G.items()}
    # the slash: the digits' height, s wide, one column every 2 rows
    sw = H // 2 + s - 1
    out['/'] = [''.join('#' if (H - 1 - j) // 2 <= i < (H - 1 - j) // 2 + s else '.' for i in range(sw)) for j in range(H)]
    # the times sign: 8x8, 2-px strokes
    out['×'] = ["##....##", "###..###", ".######.", "..####..", "..####..", ".######.", "###..###", "##....##"]
    for k, rows in FACES.get(face, {}).items():
        assert len(rows) == H and all(len(r) == W for r in rows), k
        out[k] = rows
    return out

# Barlow (Broadcast, Carbon) drawn in its own shapes rather than built from bars: condensed round bowls (two-pixel
# corners), a 1 with no foot, a 3 whose bowls meet in a notch, a 7 slanting all the way to the baseline
BARLOW = {
    "0": ["..#####..", ".#######."] + ["###...###"] * 12 + [".#######.", "..#####.."],
    "1": ["....###..", "..#####..", ".##.###.."] + ["....###.."] * 13,
    "2": ["..#####..", ".#######.", "###...###", "......###", "......###", "......###", ".....###.", "....###..",
          "...###...", "..###....", ".###.....", "###......", "###......", "###......", "#########", "#########"],
    "3": ["..#####..", ".#######.", "###...###", "......###", "......###", "......###", "......###", "...#####.",
          "...#####.", "......###", "......###", "......###", "......###", "###...###", ".#######.", "..#####.."],
    "5": ["#########", "#########", "###......", "###......", "###......", "###......", "#######..", "########.",
          "......###", "......###", "......###", "......###", "......###", "###...###", ".#######.", "..#####.."],
    "6": ["..#####..", ".#######.", "###...###", "###......", "###......", "###......", "#######..", "########."]
         + ["###...###"] * 6 + [".#######.", "..#####.."],
    "7": ["#########", "#########", "......###", "......###", ".....###.", ".....###.", ".....###.", "....###..",
          "....###..", "....###..", "...###...", "...###...", "...###...", "..###....", "..###....", "..###...."],
    "8": ["..#####..", ".#######."] + ["###...###"] * 5 + [".#######.", ".#######."] + ["###...###"] * 5
         + [".#######.", "..#####.."],
    "9": ["..#####..", ".#######."] + ["###...###"] * 6 + [".########", "..#######", "......###", "......###",
          "......###", "###...###", ".#######.", "..#####.."],
}
FACES = {"barlow": BARLOW}


def show(g):
    keys = list('0123456789/') + ['×']
    H = max(len(g[k]) for k in keys)
    for j in range(H):
        print('  '.join(g[k][j] if j < len(g[k]) else ' ' * len(g[k][0]) for k in keys))

if __name__ == '__main__':
    import sys
    a = [int(v) for v in sys.argv[1:]]
    show(numerals(*a))
