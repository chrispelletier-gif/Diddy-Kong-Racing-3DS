#!/usr/bin/env python3
"""Rasterises the bottom screen's looks' fonts to port/src/plat/ui_font_data.h, typeset as a word processor would.

Usage: make_ui_font.py OUT.h NAME:FONT:SIZE:BASE:VARIATION:CHARS:STYLE ...
  SIZE      pixel size, or capN for the size whose capital H is N pixels tall
  BASE      baseline pixels under the text's top
  VARIATION a named instance of a variable font (may be empty)
  CHARS     only these characters get pixels (may be empty: all)
  STYLE     empty, or "outline": glyphs the renderer outlines and shadows (Classic)

Every font gets, from the font file itself (OpenType, through libraqm):
  - advances in 1/64 px, so a line keeps its true width instead of collecting rounding;
  - tabular figures (the tnum feature; a font without one gets them made: every digit the widest digit's advance,
    its ink centred), so digits, points and colons line up from row to row;
  - kerning: each pair's adjustment (GPOS and kern tables), as a word processor applies it;
  - two extra glyphs: 0x7F the multiplication sign (counts: "x3"), 0x80 the minus sign (gaps: "-0.61").
The fonts are SIL Open Font License 1.1 (see NOTICE.md).
"""
import sys
from PIL import ImageFont

EXTRA = {0x7F: "×", 0x80: "−"}
# 0x81-0x8A: the digits again in their proportional forms (no tnum), for digits in running text and paths ("36%",
# "sdmc:/3ds"); the tabular ones (0x30-0x39) are for numbers that line up in columns
PROP = {0x81 + i: str(i) for i in range(10)}
EXTRA.update(PROP)
CODES = list(range(0x21, 0x8B))
NG = len(CODES)  # glyphs per pen phase

def char_of(code):
    return EXTRA.get(code, chr(code))

def load(path, size, variation):
    if size.startswith("cap"):
        want = int(size[3:])
        for s in range(6, 120):
            f = ImageFont.truetype(path, s)
            if variation:
                f.set_variation_by_name(variation.encode())
            b = f.getbbox("H", anchor="ls")
            if b[3] - b[1] >= want:
                return f
    f = ImageFont.truetype(path, int(size))
    if variation:
        f.set_variation_by_name(variation.encode())
    return f

OVER = 8

# The vertical zones of the font being rasterised (None: none): breakpoints (source, target) in pixels above the
# baseline, as a hinter snaps them. Set by zones() for each non-outlined font.
ZONES = None

def zmap(u):
    z = ZONES
    if u <= z[0][0]:
        return z[0][1] + (u - z[0][0])
    for (s0, t0), (s1, t1) in zip(z, z[1:]):
        if u <= s1:
            return t0 + (u - s0) * (t1 - t0) / (s1 - s0) if s1 > s0 else t1
    return z[-1][1] + (u - z[-1][0])

def zinv(t):
    z = ZONES
    if t <= z[0][1]:
        return z[0][0] + (t - z[0][1])
    for (s0, t0), (s1, t1) in zip(z, z[1:]):
        if t0 < t1 and t <= t1:
            return s0 + (t - t0) * (s1 - s0) / (t1 - t0)
    return z[-1][0] + (t - z[-1][1])

def zones(big):
    """Snaps the font's zones to even rows: the capitals' height (digits, round capitals and ascenders end on the same
    row), the lowercase's, and the descenders' depth, each the nearest even number of pixels. A label's ink is then an
    even number of rows whatever its letters (capitals, lowercase only, with or without descenders), so it sits exactly
    in the middle of the grid's even boxes: never a pixel more above than below."""
    def top(c):
        b = big.getbbox(c, anchor="ls")
        return -b[1] / OVER
    def bot(c):
        b = big.getbbox(c, anchor="ls")
        return b[3] / OVER
    Hs, xs = top("H"), top("x")
    hi = max(top(c) for c in "0123456789OCGQSbdfhklij!?/")
    xo = max(top(c) for c in "oecsa")
    ov0 = max(0.0, max(bot(c) for c in "abcdehiklmnorstuvwxzBCDGJOQSU0123456789/"))
    ps = bot("p")
    gs = max(bot(c) for c in "gjpqy")
    even = lambda v: max(2, 2 * int(round(v / 2.0)))
    H, X, D = even(Hs), even(xs), even(ps)
    return [(-gs - 20, -D - 20), (-gs, -D), (-ps, -D), (-ov0, 0), (0, 0), (xs, X), (max(xs, xo), X), (Hs, H),
            (max(Hs, hi), H), (max(Hs, hi) + 20, H + 20)]

# Glyphs redrawn hinted (STYLE "fix=SIZE/CHARS"): the few letters the zone resampling below breaks at these sizes
# (Barlow's Q read as an O, its g closed up, f and t blurred; Inter's Q lost its tail) are FreeType's own light-hinted
# outlines at SIZE px instead: heights and bars snapped to whole rows, each letter's true shape across. Their rows are
# made to stand exactly on the font's zones (see zones()) by repeating or dropping one whole row where the letters
# change least from the row beside it, and their coverage takes the same three levels as every other glyph.
HFACE = None
HCAP = 0  # the capitals' height after the row map: no letter but a descender's reaches past it or under the baseline
HROWS = None  # source row (above the baseline) -> destination rows

TNUM = {}  # character -> its tabular form's glyph name (the font's tnum feature, read from GSUB)

def hload(path, size):
    import freetype
    global TNUM
    f = freetype.Face(path)
    f.set_char_size(int(round(size * 64)), 0, 72, 72)
    TNUM = {}
    try:
        from fontTools.ttLib import TTFont
        t = TTFont(path)
        cmap = t.getBestCmap()
        g = t["GSUB"].table
        m = {}
        for fr in g.FeatureList.FeatureRecord:
            if fr.FeatureTag == "tnum":
                for li in fr.Feature.LookupListIndex:
                    for st in g.LookupList.Lookup[li].SubTable:
                        st = getattr(st, "ExtSubTable", st)
                        if hasattr(st, "mapping"):
                            m.update(st.mapping)
        for d in "0123456789":
            if ord(d) in cmap and cmap[ord(d)] in m:
                TNUM[d] = m[cmap[ord(d)]]
    except (ImportError, KeyError):
        pass
    return f

HFLAGS = None

def hglyph(ch, stretch=1.0, tnum=False, shift=0):
    """The hinted bitmap: (rows of coverage, width, bitmap_left, bitmap_top, advance px). The advance is the hinter's
    (whole pixels); hdelta() gives the pair corrections FreeType's own layout makes with it."""
    import freetype
    f = HFACE
    if stretch != 1.0 or shift:
        # (a pen phase: light hinting leaves the letter's place across alone, so it is drawn that far right)
        f.set_transform(freetype.Matrix(int(stretch * 0x10000), 0, 0, 0x10000), freetype.Vector(int(shift), 0))
    fl = freetype.FT_LOAD_FORCE_AUTOHINT | (freetype.FT_LOAD_TARGET_LIGHT if HLIGHT else freetype.FT_LOAD_TARGET_NORMAL) | freetype.FT_LOAD_RENDER
    if tnum and ch in TNUM:
        f.load_glyph(f.get_name_index(TNUM[ch].encode()), fl)
    else:
        f.load_char(ch, fl)
    if stretch != 1.0 or shift:
        f.set_transform(freetype.Matrix(0x10000, 0, 0, 0x10000), freetype.Vector(0, 0))
    g = f.glyph
    bm = g.bitmap
    rows = [[bm.buffer[j * bm.pitch + i] for i in range(bm.width)] for j in range(bm.rows)]
    raw = g._FT_GlyphSlot.contents
    HDELTA[(ch, tnum)] = (raw.lsb_delta, raw.rsb_delta)
    HLIN[(ch, tnum)] = g.linearHoriAdvance / 65536.0
    return rows, bm.width, g.bitmap_left, g.bitmap_top, int(round(g.advance.x / 64.0))

HDELTA = {}
# light hinting (FreeType's "light" target): heights, bars and baselines snapped to whole rows; across, each letter
# keeps its true shape and width (the full hinter squeezed Barlow's f and t into 1 and l)
HLIGHT = True
HLIN = {}  # the unhinted advances

def hdelta(a, b):
    """FreeType's hinted spacing fix for a pair: the hinter moved a's right edge and b's left edge, and when they moved
    apart (or together) by more than half a pixel between them the pair is set a pixel closer (or wider)."""
    hglyph(a)
    hglyph(b)
    d = HDELTA[(a, False)][1] - HDELTA[(b, False)][0]
    return -64 if d > 32 else 64 if d < -31 else 0

def hadv(ch, tnum=False):
    return hglyph(ch, 1.0, tnum)[4]

def hzone_rows(chars, th=96):
    """Ink rows above the baseline (top, bottom) of these letters, as hinted (no row map)."""
    top, bot = -99, 99
    for c in chars:
        rows, w, x0, t, _ = hglyph(c)
        ink = [j for j in range(len(rows)) if any(v >= th for v in rows[j])]
        if ink:
            top = max(top, t - ink[0])
            bot = min(bot, t - ink[-1] - 1)
    return top, bot

def hplan(old, digits_only=False):
    """The row map: each zone (capitals above the lowercase, lowercase, descenders) made even, toward the font's
    previous height (old: (H, X, D)) when it is odd, by one row repeated or dropped where it changes least."""
    global HROWS
    cap = hzone_rows("0123456789" if digits_only else "HEIZ")[0]
    x = hzone_rows("xzvw")[0]
    desc = -hzone_rows("gpqy")[1]
    lo = "abcdefghijklmnopqrstuvwxyz"
    up = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    glyphs = {c: hglyph(c) for c in lo + up}
    def change(r, chars):
        # how much the letters' row r differs from the row below it (lower is better to repeat or drop)
        tot = 0
        for c in chars:
            rows, w, x0, t, _ = glyphs[c]
            def row(rr):
                j = t - 1 - rr
                return rows[j] if 0 <= j < len(rows) else [0] * w
            a, b = row(r), row(r - 1)
            tot += sum(abs(u - v) for u, v in zip(a, b))
        return tot
    count = {}
    plan = []
    def gap(r):
        # a row some letter leaves clear between ink above and below it (the dot of an i or j over its stem): dropping
        # it would join them
        n = 0
        for c in lo + up:
            rows, w, x0, t, _ = glyphs[c]
            def has(rr):
                j = t - 1 - rr
                return 0 <= j < len(rows) and any(v >= 64 for v in rows[j])
            if not has(r) and has(r + 1) and has(r - 1):
                n += 1
        return n
    def fix(v, want, band, chars, name):
        if v % 2 == 0:
            return v
        d = 1 if want > v else -1
        r = min(band, key=lambda rr: (gap(rr) if d < 0 else 0, change(rr, chars),
                                      abs(rr - (band[0] + band[-1]) / 2.0)))
        count[r] = 2 if d > 0 else 0
        plan.append("%s %d->%d at row %d" % (name, v, v + d, r))
        return v + d
    H, X, D = old
    global HCAP
    dx = 0
    if not digits_only:
        dx = fix(x, X, list(range(1, x - 1)), lo, "x") - x
    # (a row added or dropped in the lowercase moves the capitals' top with it)
    HCAP = fix(cap + dx, H, list(range(x, cap - 1)) or [x], up + "bdfhijklt", "cap")
    if not digits_only:
        fix(desc, D, list(range(-desc + 1, -1)) or [-1], "gjpqy", "desc")
    HROWS = {}
    d = 0
    for r in range(0, 120):
        c = count.get(r, 1)
        HROWS[r] = list(range(d, d + c))
        d += c
    d = -1
    for r in range(-1, -60, -1):
        c = count.get(r, 1)
        HROWS[r] = list(range(d, d - c, -1))
        d -= c
    return cap, x, desc, plan

HFIX = ""  # the letters drawn hinted

def hraster(ch, stretch=1.0, tnum=False, shift=0):
    rows, w, x0, t, _ = hglyph(ch, stretch, tnum, shift)
    out = {}
    for j, row in enumerate(rows):
        for dr in HROWS[t - 1 - j]:
            out[dr] = row
    if not out or w == 0:
        return [], 0, 0, 0, 0
    # nothing taller than the capitals or under the baseline but descenders and brackets (Outfit's ascenders and
    # Inter's slash stood a row out, so a label with them was an odd number of rows and sat half a pixel off): a short
    # mark (` ^ ') moved in, a taller glyph losing the row of its body most like the one under it
    def trim(lo, hi):
        rows = sorted(out, reverse=True)
        n = (rows[0] - hi + 1 if rows[0] >= hi else 0) + (lo - rows[-1] if rows[-1] < lo else 0)
        if n <= 0:
            return
        if len(rows) <= 4:
            dy = -(rows[0] - hi + 1) if rows[0] >= hi else lo - rows[-1]
            moved = {r + dy: v for r, v in out.items()}
            out.clear()
            out.update(moved)
            return
        for _ in range(n):
            rows = sorted(out, reverse=True)
            above = rows[0] >= hi
            pick = [r for r in rows[1:-1] if (r >= HCAP // 2 if above else True)] or rows[1:-1]
            r = min(pick, key=lambda rr: (sum(abs(a - b) for a, b in zip(out[rr], out.get(rr - 1, [0] * w))), -rr if above else rr))
            new = {}
            for rr, v in out.items():
                if rr == r:
                    continue
                new[rr - 1 if (above and rr > r) else rr + 1 if (not above and rr < r) else rr] = v
            out.clear()
            out.update(new)
    if HCAP and ch not in "gjpqy,;()[]{}|_Q@$":
        trim(0, HCAP)
    elif HCAP and ch in "gjpqy;,Q":
        trim(-99, HCAP)
    # coverage under 24 dropped: a fringe the eye can't see but a check would count as ink
    top = max(out)
    bot = min(out)
    full = [[(v if v >= 24 else 0) for v in out.get(r, [0] * w)] for r in range(top, bot - 1, -1)]
    cols = [i for i in range(w) if any(rw[i] for rw in full)]
    rws = [j for j in range(len(full)) if any(full[j])]
    if not cols:
        return [], 0, 0, 0, 0
    c0, c1, r0, r1 = cols[0], cols[-1] + 1, rws[0], rws[-1] + 1
    px = [full[j][i] for j in range(r0, r1) for i in range(c0, c1)]
    # the three levels every other glyph is drawn in (see raster())
    px = [0 if v < 56 else 112 if v < 184 else 255 for v in px]
    return px, c1 - c0, r1 - r0, x0 + c0, -(top + 1) + r0

# The key caps' and value chips' lone letters, drawn by hand for each body font: solid pixels only (no half-covered
# edge, so every cap letter in a look is drawn alike), 2 px stems and 1 row bars, mirror exact where the letter is
# symmetric (A, X: left to right; X top to bottom too), each in two widths a column apart so whichever cap's room is
# even or odd, the letter sits exactly in its middle. font -> letter -> (narrow, wide); "#" solid, "." clear.
def _bar(w, l, r, n=1):
    return ["#" * l + "." * (w - l - r) + "#" * r] * n
KEYCAP = {
    "Barlow": {  # 10 rows
        "A": (["..##..", ".####.", ".####.", "##..##", "##..##", "##..##", "######", "##..##", "##..##", "##..##"],
              ["..###..", ".#####.", "##...##", "##...##", "##...##", "##...##", "#######", "##...##", "##...##", "##...##"]),
        "B": (["#####."] + _bar(6, 2, 2, 3) + ["#####."] + _bar(6, 2, 2, 4) + ["#####."],
              ["######."] + _bar(7, 2, 2, 3) + ["######."] + _bar(7, 2, 2, 4) + ["######."]),
        "L": (["##...."] * 9 + ["######"], ["##....."] * 9 + ["#######"]),
        "R": (["#####."] + _bar(6, 2, 2, 3) + ["#####.", "##.##."] + _bar(6, 2, 2, 4),
              ["######."] + _bar(7, 2, 2, 3) + ["######.", "##.##..", "##..##."] + _bar(7, 2, 2, 3)),
        "X": (["##..##", "##..##", ".####.", ".####.", "..##..", "..##..", ".####.", ".####.", "##..##", "##..##"],
              ["##...##", "##...##", ".##.##.", ".##.##.", "..###..", "..###..", ".##.##.", ".##.##.", "##...##", "##...##"]),
    },
    "Outfit": {  # 10 rows
        "A": (["...##...", "..####..", "..####..", ".##..##.", ".##..##.", "##....##", "########", "##....##", "##....##", "##....##"],
              ["...###...", "...###...", "..##.##..", "..##.##..", ".##...##.", ".##...##.", ".#######.", "##.....##", "##.....##", "##.....##"]),
        "B": (["######."] + _bar(7, 2, 2, 3) + ["######."] + _bar(7, 2, 2, 4) + ["######."],
              ["#######."] + _bar(8, 2, 2, 3) + ["#######."] + _bar(8, 2, 2, 4) + ["#######."]),
        "L": (["##...."] * 9 + ["######"], ["##....."] * 9 + ["#######"]),
        "R": (["######."] + _bar(7, 2, 2, 3) + ["######.", "##.##..", "##..##."] + _bar(7, 2, 2, 3),
              ["#######."] + _bar(8, 2, 2, 3) + ["#######.", "##..##..", "##...##."] + _bar(8, 2, 2, 3)),
        "X": (["##....##", "##....##", ".##..##.", "..####..", "...##...", "...##...", "..####..", ".##..##.", "##....##", "##....##"],
              ["##.....##", "##.....##", ".##...##.", "..##.##..", "...###...", "...###...", "..##.##..", ".##...##.", "##.....##", "##.....##"]),
    },
    "Glass": {  # 8 rows
        "A": (["..###..", ".##.##.", ".##.##.", "##...##", "##...##", "#######", "##...##", "##...##"],
              ["...##...", "..####..", ".##..##.", ".##..##.", "##....##", "########", "##....##", "##....##"]),
        "B": (["#####."] + _bar(6, 2, 2, 2) + ["#####."] + _bar(6, 2, 2, 3) + ["#####."],
              ["######."] + _bar(7, 2, 2, 2) + ["######."] + _bar(7, 2, 2, 3) + ["######."]),
        "L": (["##..."] * 7 + ["#####"], ["##...."] * 7 + ["######"]),
        "R": (["#####."] + _bar(6, 2, 2, 2) + ["#####.", "##.##."] + _bar(6, 2, 2, 3),
              ["######."] + _bar(7, 2, 2, 2) + ["######.", "##..##."] + _bar(7, 2, 2, 3)),
        "X": (["##...##", "##...##", ".##.##.", "..###..", "..###..", ".##.##.", "##...##", "##...##"],
              ["##....##", ".##..##.", "..####..", "...##...", "...##...", "..####..", ".##..##.", "##....##"]),
    },
}

# capitals drawn mirror symmetric: their wide forms must be too
SYMMETRIC = "AHIMOTUVWXY"

# Classic's capitals one column wider than its own (see main(): a lone letter centred in its key cap), 8 rows
CLASSIC_WIDE = {
    "A": ["...##...", "..####..", "..####..", ".######.", ".##..##.", ".######.", "###..###", "##....##"],
    "B": ["#####.", "######", "##..##", "#####.", "######", "##..##", "######", "#####."],
    "L": ["##...", "##...", "##...", "##...", "##...", "##...", "#####", "#####"],
    "X": ["##....##", "###..###", ".######.", "..####..", "..####..", ".######.", "###..###", "##....##"],
    "Y": ["##...##", "###.###", ".#####.", "..###..", "..###..", "..###..", "..###..", "..###.."],
    "R": ["#####.", "######", "##..##", "######", "#####.", "######", "##..##", "##..##"],
    "Z": ["######", "######", "...###", "..###.", ".###..", "###...", "######", "######"],
}

def raster(font, big, ch, feats, phase, stretch=1.0):
    """A glyph's coverage as (pixels, w, h, x0, y0) for a pen phase/4 px right of a whole pixel. Outlined fonts
    (big None) keep FreeType's hinted bitmap. The others are drawn unhinted at OVER times the size and box-filtered
    down, as a word processor positions text: hinting would snap every glyph back to whole pixels (and its own
    widths), and the quarter-pixel pen would gain nothing."""
    if HFACE is not None and big is not None and ch in HFIX:
        return hraster(ch, stretch, False, int(round(phase * 16)))
    if big is None:
        m, (x0, y0) = font.getmask2(ch, mode="L", anchor="ls", features=feats)
        w, h = m.size
        return [m[j * w + i] for j in range(h) for i in range(w)], w, h, x0, y0
    m, (bx, by) = big.getmask2(ch, mode="L", anchor="ls", features=feats, start=(phase * OVER / 4.0, 0))
    bw, bh = m.size
    if bw == 0 or bh == 0:
        return [], 0, 0, 0, 0
    if stretch != 1.0:
        # the letter drawn wider from its outline (a width variant, as a font's wider instance), about its middle
        from PIL import Image as _I
        nbw = int(round(bw * stretch))
        img = _I.frombytes("L", (bw, bh), bytes(m)).resize((nbw, bh), _I.BILINEAR)
        bx -= (nbw - bw) // 2
        m, bw = list(img.tobytes()), nbw
    X0 = bx // OVER
    X1 = -((-(bx + bw)) // OVER)
    w = X1 - X0
    if ZONES is None:
        Y0 = by // OVER
        Y1 = -((-(by + bh)) // OVER)
        h = Y1 - Y0
        acc = [0] * (w * h)
        for j in range(bh):
            row = ((by + j) // OVER - Y0) * w
            for i in range(bw):
                v = m[j * bw + i]
                if v:
                    acc[row + (bx + i) // OVER - X0] += v
    else:
        # the rows resampled through the font's zones (see zones()): capitals, lowercase and descenders end on whole,
        # even pixel rows, so every label's ink is an even number of rows and centres exactly in the grid's boxes
        import math
        Y0 = int(math.floor(-zmap(-by / OVER)))
        Y1 = int(math.ceil(-zmap(-(by + bh) / OVER)))
        h = max(1, Y1 - Y0)
        acc = [0] * (w * h)
        for r in range(h):
            for sub in range(OVER):
                u = zinv(-(Y0 + r + (sub + 0.5) / OVER))
                j = int(math.floor(-u * OVER - by))
                if 0 <= j < bh:
                    base = j * bw
                    for i in range(bw):
                        v = m[base + i]
                        if v:
                            acc[r * w + (bx + i) // OVER - X0] += v
    # levels 16..224: faint fringe drops out and near-full edge pixels go full, so stems read as sharp as hinted ones
    # at 1:1 (a box filter alone leaves more half-covered pixels than FreeType's hinter does)
    px = [max(0, min(255, ((a + OVER * OVER // 2) // (OVER * OVER) - 16) * 255 // 208)) for a in acc]
    # three levels of coverage: clear, half (112) and solid. A label's ink then measures the same at any threshold an
    # eye or a check picks: its edge (half and solid) or its core (solid alone); gtext_ex centres it on both, so it is
    # centred whichever the eye counts
    px = [0 if v < 56 else 112 if v < 184 else 255 for v in px]
    # trim empty edges
    cols = [i for i in range(w) if any(px[j * w + i] for j in range(h))]
    rows = [j for j in range(h) if any(px[j * w + i] for i in range(w))]
    if not cols:
        return [], 0, 0, 0, 0
    c0, c1, r0, r1 = cols[0], cols[-1] + 1, rows[0], rows[-1] + 1
    out = [px[j * w + i] for j in range(r0, r1) for i in range(c0, c1)]
    return out, c1 - c0, r1 - r0, X0 + c0, Y0 + r0

def optical_kerns(glyphs, data, adv64):
    """Classic is set by ink (each face one pixel of shared outline after the last). At these sizes Lilita's letters
    are near-solid shapes with soft edges, so that leaves uneven gaps ("Ba na na s", "Isla nd", "Ta j"). Each pair is
    moved, whole pixels at a time, to make its average visible gap (edge pixels from coverage 40) one pixel on the rows
    both letters share, and never so close that two faces (coverage 80) come nearer than one outline pixel in any
    direction (rows next to each other count, so T's bar never touches the a under it)."""
    def profile(code, th):
        a64, x0, y, w, h, off = glyphs[code - 0x21]
        left, right = {}, {}
        for j in range(h):
            xs = [x0 + i for i in range(w) if data[off + j * w + i] >= th]
            if xs:
                left[y + j], right[y + j] = min(xs), max(xs)
        return left, right
    live = [c for c in CODES if adv64[c] and glyphs[c - 0x21][3] and not chr(c).isdigit() and c not in EXTRA]
    figs = [c for c in range(ord("0"), ord("9") + 1) if adv64[c] and glyphs[c - 0x21][3]]
    solid = {c: profile(c, 80) for c in live + figs}
    seen = {c: profile(c, 40) for c in live + figs}
    def near(a, b, shift):
        ra, lb, n = solid[a][1], solid[b][0], 99
        for ya, xa in ra.items():
            if ya in lb:
                n = min(n, lb[ya] + shift - xa)
            for dy in (-1, 1):
                if ya + dy in lb:
                    # a row above or below may come one column nearer (corners meet diagonally, as a descender's
                    # hook under the letter before it: "Banjo"), never under or over it
                    n = min(n, lb[ya + dy] + shift - xa + 1)
        return n
    def mean(a, b, shift):
        ra, lb = seen[a][1], seen[b][0]
        gaps = [lb[y] + shift - ra[y] - 1 for y in ra if y in lb]
        return sum(gaps) / len(gaps) if gaps else None
    out = []
    for a in live:
        for b in live:
            base = adv64[a] // 64
            m0 = mean(a, b, base)
            if m0 is None:
                continue
            best, bestd = 0, abs(m0 - 1.0)
            for k in (-3, -2, -1, 1, 2):
                if k < 0 and near(a, b, base + k) < 2:
                    continue
                d = abs(mean(a, b, base + k) - 1.0)
                if d < bestd - 0.3:  # move only for a clear gain: most pairs stay as set by ink
                    best, bestd = k, d
            # never so close that the outlines touch: faces one clear pixel apart at least, side by side or corner to
            # corner ("NEXT": the X's arm met the E's)
            def corner(shift):
                # faces corner to corner: a row above or below counted as near as the same row (an X's arm tip
                # diagonally under an E's arm shares its outline pixel)
                ra, lb, n = solid[a][1], solid[b][0], 99
                for ya, xa in ra.items():
                    for dy in (-1, 0, 1):
                        if ya + dy in lb:
                            n = min(n, lb[ya + dy] + shift - xa)
                return n
            while (near(a, b, base + best) < 2 or corner(base + best) < 2) and best < 3:
                best += 1
            if best:
                out.append((a, b, best * 64))
    # a fraction's slash set close to its digits ("8/8", not "8 / 8"), but never so close that its outline touches
    # theirs (the tabular cells leave room round the ink, so most pairs move in)
    sl = ord("/")
    if sl in live:
        for a, b in [(d, sl) for d in figs] + [(sl, d) for d in figs]:
            base = adv64[a] // 64
            if mean(a, b, base) is None:
                continue
            # the closest that leaves a clear pixel between the two outlines (faces 3 apart at least)
            ok = [k for k in (-4, -3, -2, -1, 0, 1, 2) if near(a, b, base + k) >= 3]
            best = min(ok) if ok else 2
            if best:
                out.append((a, b, best * 64))
    return out

def tabular(code):
    """Digits and the minus sign: one width, so signed numbers line up from row to row (and "--:--" placeholders
    typeset with the minus sit where the digits will)."""
    return chr(code).isdigit() or code == 0x80 or code == 0x7F

def centre_marks(glyphs, data, crisp, phases):
    """The multiplication and minus signs centred on the digits' height (fonts put them on the maths axis, a pixel or
    two above the middle of a figure: "4x" read as a raised mark). Outlined fonts (Classic) get a drawn x: two crisp
    diagonals, as the font's own is a soft blob at these sizes."""
    z = glyphs[ord("0") - 0x21]
    a64, zx, zy, zw, zh, zoff = z
    rows = [j for j in range(zh) if any(data[zoff + j * zw + i] >= 80 for i in range(zw))]
    if not rows:
        return
    dc2 = (zy + rows[0]) + (zy + rows[-1])  # twice the digits' ink centre row
    for p in range(phases):
        for code in (0x7F, 0x80, 0x2B):  # and the plus of a signed number
            idx = p * NG + code - 0x21
            a64, x0, y, w, h, off = glyphs[idx]
            if not a64 or not w:
                continue
            if crisp and code == 0x7F:
                n = max(5, int(round(len(rows) * 0.55)))
                # the same parity as the digits' ink rows, so the x sits exactly on their middle (not half a pixel high)
                n += (len(rows) - n) % 2
                cell = a64 // 64 - 1
                off = len(data)
                w = n if n >= 7 else n + 1
                for j in range(n):
                    for i in range(w):
                        # two pixels wide at the small sizes, as the counts' drawn cross (a 1 px stroke inside its
                        # outline read as a hollow mark beside a solid digit: "4x"), three from 7 px up
                        if n >= 7:
                            on = abs(i - j) <= 1 or abs(i - (n - 1 - j)) <= 1
                        else:
                            on = i in (j, j + 1) or i in (n - 1 - j, n - j)
                        data.append(255 if on else 0)
                glyphs[idx] = (a64, (cell - w + 1) // 2, (dc2 - (n - 1)) // 2, w, n, off)
                continue
            ink = [j for j in range(h) if any(data[off + j * w + i] >= 60 for i in range(w))]
            if ink:
                c2 = (y + ink[0]) + (y + ink[-1])
                glyphs[idx] = (a64, x0, y + int(round((dc2 - c2) / 2.0)), w, h, off)

def char_feats(ch, feats):
    """Tabular forms only for figures and the marks inside numbers: some fonts' tnum also widens the hyphen (Inter's
    to a digit's width), which a word processor never applies to words ("Anti-aliasing")."""
    return feats if (ch.isdigit() or ch in ".:,+" or ch in EXTRA.values()) else [f for f in feats if f != "tnum"]

def main():
    out = sys.argv[1]
    lines = ["/* Generated by port/tools/ui_font/make_ui_font.py: do not edit. */", ""]
    names = []
    for spec in sys.argv[2:]:
        parts = (spec.split(":") + [""] * 7)[:7]
        name, path, size, base, variation, chars, style = parts
        base = int(base)
        font = load(path, size, variation)
        big = None
        global ZONES, HFACE, HFIX, HCAP
        ZONES = None
        HFACE, HFIX, HCAP = None, "", 0
        if not style.startswith(("outline", "draw=")):
            big = ImageFont.truetype(path, font.size * OVER)
            if variation:
                big.set_variation_by_name(variation.encode())
            ZONES = zones(big)
        if style.startswith("fix="):
            hsize, HFIX = style[4:].split("/")
            HFACE = hload(path, float(hsize))
            old = (ZONES[7][1], ZONES[5][1], -ZONES[1][1])
            got = hplan(old)
            # the redrawn letters must stand on exactly the zones the others do
            def hrows(c):
                pix, w, h, x0, y0 = hraster(c)
                return -y0, h + y0
            H_, X_, D_ = hrows("H")[0], hrows("x")[0], hrows("p")[1]
            sys.stderr.write("%s: %s redrawn hinted at %s px: cap %d x %d desc %d (zones %d %d %d)%s\n" % (
                name, HFIX, hsize, H_, X_, D_, old[0], old[1], old[2], "" if (H_, X_, D_) == old else "  MISMATCH"))
        bigx, xshift = big, 0
        bigp, pshift = big, 0
        if big is not None:
            # a times sign drawn as tall as a lowercase x less a row (as drawn at 1:1): some fonts' (Barlow's) is a small mark of a raised
            # maths sign, 4 px beside a 13 px digit; it is drawn from a larger size, its ink kept centred where it was
            def inkbox(f, c):
                m, (ox, oy) = f.getmask2(c, mode="L", anchor="ls")
                from PIL import Image as _I
                b = _I.frombytes("L", m.size, bytes(m)).point(lambda v: 255 if v >= 128 else 0).getbbox() or (0, 0, 0, 0)
                return (ox + b[0], oy + b[1], ox + b[2], oy + b[3])
            def rows1x(f, c):
                # ink rows at 1:1, as drawn (the box filter and levels thin a diagonal's ends): coverage 128 and up
                pix, w, h, _, _ = raster(font, f, c, char_feats(c, ["tnum", "kern"]), 0)
                return len([j for j in range(h) if any(pix[j * w + i] >= 128 for i in range(w))])
            want = rows1x(big, "x") - 1
            def enlarged(ch):
                # the sign drawn from a larger size until it stands as tall as a lowercase x less a row; its ink centre
                # kept where the font put it in its cell
                tb, f, scale = inkbox(big, ch), big, 1.0
                while rows1x(f, ch) < want and scale < 1.5:
                    scale += 0.05
                    f = ImageFont.truetype(path, int(round(font.size * OVER * scale)))
                    if variation:
                        f.set_variation_by_name(variation.encode())
                if f is big:
                    return big, 0
                lb = inkbox(f, ch)
                adv_s, adv_l = big.getlength(ch), f.getlength(ch)
                return f, int(round(((tb[0] + tb[2]) / 2.0 - adv_s / 2.0 - ((lb[0] + lb[2]) / 2.0 - adv_l / 2.0)) / OVER))
            bigx, xshift = enlarged("×")
            # the plus of a signed number too ("2+ late", "+1.41"): Barlow's is a small raised mark beside its digit
            bigp, pshift = enlarged("+")
            pshift = 0  # its advance grows with it instead (no shift)
        feats = ["tnum", "kern"]
        # the times sign ("4x" anti-aliasing, "x3" a count) centred top to bottom on the digits' ink, as a maths sign
        # sits between figures; fonts put theirs on the lowercase (Outfit, Lexend: low beside the digit)
        _zp, _zw, _zh, _zx, _zy = raster(font, big, "0", ["tnum"], 0)
        def times_y(h, y0):
            return y0 if _zh == 0 or h == 0 else _zy + (_zh - h) // 2
        # and, at body and small sizes (digits 11 px or less), drawn rather than taken from the font: two strokes as heavy
        # as the 0's side, crossing in a square 2/3 of the digits' height, so it reads as a sign, not a small x ("4x");
        # the large sizes ("x3" items) and the outlined Classic keep the font's own, centred the same way
        _stem = 0
        if _zh:
            _mid = _zh // 2
            while _stem < _zw and _zp[_mid * _zw + _stem] >= 128:
                _stem += 1
        def times_pix():
            n = int(round(_zh * 2 / 3.0))
            n += (_zh - n) % 2  # the digits' parity, so it sits exactly on their middle
            half = max(0.75, min(1.25, _stem / 2.0)) * 0.95
            pix = []
            for j in range(n):
                for i in range(n):
                    best = 0.0
                    for k in range(16):  # 4x4 samples per pixel
                        u, v = i + (k % 4 + 0.5) / 4, j + (k // 4 + 0.5) / 4
                        d1 = abs(u - v) / 1.41421356
                        d2 = abs(u + v - n) / 1.41421356
                        # the strokes end square with the box (no ink beyond it)
                        best += 1.0 if min(d1, d2) <= half else 0.0
                    pix.append(min(255, int(round(best / 16 * 255))))
            return pix, n, n
        has_tnum = len({round(font.getlength(d, features=feats), 3) for d in "0123456789"}) == 1
        digit_adv = max(font.getlength(d, features=feats) for d in "0123456789")
        # crisp: whole pixels, set by ink (Classic, outlined; the drawn sets of the other looks, not outlined)
        outlined = style.startswith("outline")
        crisp = outlined or style.startswith("draw=")
        # Classic's own lettering drawn by hand (STYLE "outline+pix8" or "outline+pix9": see classic_pixels.py); the
        # font file is then only for the few marks not drawn
        design = {}
        if style.startswith("draw="):
            # the other looks' lettering drawn by hand (STYLE "draw=FACE/ASC/X/DESC": the rows its size gains above the
            # x-height, in it and below it; see drawn_pixels.py)
            import drawn_pixels as _dp
            face, g_asc, g_x, g_d = style[5:].split("/")
            g_asc, g_x, g_d = int(g_asc), int(g_x), int(g_d)
            prows = 8 + g_asc + g_x
            for k, gl in _dp.FACES[face].items():
                design[k] = _dp.grow(k, gl, g_asc, g_x, g_d)
        if "+pix" in style:
            import classic_pixels as _cp
            prows = int(style.split("+pix")[1])
            for k, gl in _cp.GLYPHS.items():
                design[k] = gl if prows == _cp.CAP else _cp.head(gl)
        def designed(ch):
            # (the proportional figures and the minus are the same drawings)
            return design.get(ch)
        data, glyphs = [], []
        adv64 = {}
        for code in CODES:
            ch = char_of(code)
            if chars and ch not in chars:
                glyphs.append((0, 0, 0, 0, 0, 0))
                adv64[code] = 0
                continue
            cf = [f for f in feats if f != "tnum"] if code in PROP else char_feats(ch, feats)
            adv = font.getlength(ch, features=cf)
            if ch == "+" and bigp is not big:
                adv = bigp.getlength(ch, features=cf) / OVER  # the larger plus keeps its own side bearings
            pix, w, h, x0, y0 = raster(font, bigx if ch == "×" else bigp if ch == "+" else big, ch, cf, 0)
            dg = designed(ch)
            if dg is not None:
                top, rws = dg
                w, h = len(rws[0]), len(rws)
                pix = [255 if c == "#" else 0 for r in rws for c in r]
                x0, y0 = 0, top - prows
            if ch == "×":
                x0 += xshift
                if not crisp and 0 < _zh <= 11:
                    pix, w, h = times_pix()
                    x0, adv = 1, w + 2  # 1 px clear of the digit each side, as a word space is not
                y0 = times_y(h, y0)
            elif ch == "+":
                x0 += pshift
            if tabular(code) and abs(adv - digit_adv) > 0.01:
                # tabular figures made (a font without tnum; the minus in every font): the widest digit's advance,
                # this glyph's ink centred in it
                x0 = int(round(x0 + (digit_adv - adv) / 2))
                adv = digit_adv
            mask = None
            if crisp and w > 0:
                from PIL import Image as _I
                mask = _I.frombytes("L", (w, h), bytes(pix))
                if ch == "1" and code not in PROP and dg is None:
                    # a timer-face 1 drawn to fill the 0's cell: a stem as heavy as the 0's sides, centred, a flag to the
                    # left at the top and a foot the 0's full width, so the tabular 1 neither leaves a hole ("00:3 1:00")
                    # nor reads thin beside its neighbours ("7 1%")
                    zm, (zx, zy) = font.getmask2("0", mode="L", anchor="ls")
                    zero = _I.frombytes("L", zm.size, bytes(zm))
                    zb = zero.point(lambda v: 255 if v >= 80 else 0).getbbox()
                    zw, zh = zb[2] - zb[0], zb[3] - zb[1]
                    # the 0's side: solid run on its middle row
                    mid = zb[1] + zh // 2
                    side = 0
                    while zb[0] + side < zb[2] and zero.getpixel((zb[0] + side, mid)) >= 80:
                        side += 1
                    sw = max(2, min(side, zw - 2))
                    sl = (zw - sw + 1) // 2
                    fh = max(1, int(round(zh * 0.18)))
                    fl = max(1, int(round(zh * 0.22)))
                    canvas = _I.new("L", (zw, zh), 0)
                    for yy in range(zh):
                        for xx in range(sl, sl + sw):
                            canvas.putpixel((xx, yy), 255)
                    for r in range(fl + 1):  # the flag: an arm down to the left from the stem's top
                        for xx in range(max(0, sl - 1 - r), sl):
                            canvas.putpixel((xx, r), 255)
                    for yy in range(zh - fh, zh):
                        for xx in range(zw):
                            canvas.putpixel((xx, yy), 255)
                    mask, w, h = canvas, zw, zh
                    x0, y0 = zx + zb[0], zy + zb[1]
                    pix = list(canvas.tobytes())
            if crisp and ch in "ij" and w > 0 and dg is None:
                # the dot no higher than the capitals (the hinter sets it a row above them): its top row dropped, so a
                # word's ink is the capitals' height ("Medium" an even number of rows, as "MEDIUM")
                hb = font.getmask2("H", mode="L", anchor="ls")
                hm, (hx, hy) = hb
                hrows = [j for j in range(hm.size[1]) if any(hm[j * hm.size[0] + i] >= 80 for i in range(hm.size[0]))]
                cut = (hy + hrows[0]) - y0 if hrows else 0
                if cut > 0:
                    pix = pix[cut * w:]
                    h -= cut
                    y0 += cut
            if outlined and ch == "/" and w > 0 and dg is None:
                # Classic's big slash (Lilita's): exactly the figures' height and on their rows, so "2/3" laps have it
                # centred on the digits (Lilita's stands a row over them and two under); a stroke as heavy as theirs
                zrows = [j for j in range(_zh) if any(_zp[j * _zw + i] >= 80 for i in range(_zw))]
                n = len(zrows)
                sw = 3 if n >= 12 else 2
                sw2 = n // 2 + sw - 1  # its width: half its height, plus the stroke
                pix = []
                for j in range(n):
                    left = (n - 1 - j) * (sw2 - sw) // max(1, n - 1)
                    pix.extend(255 if left <= i < left + sw else 0 for i in range(sw2))
                w, h = sw2, n
                x0, y0 = 0, _zy + zrows[0]
                from PIL import Image as _I
                mask = _I.frombytes("L", (w, h), bytes(pix))
            if crisp and ch == "-" and w > 0 and dg is None:
                # Classic's hyphen: Lilita's bar is an en dash wide at this size ("Anti- aliasing"); a hyphen is a short
                # bar, 4 px of solid ink, its rows as the font draws them
                sb = mask.point(lambda v: 255 if v >= 80 else 0).getbbox()
                if sb and sb[2] - sb[0] > 4:
                    canvas = _I.new("L", (4, h), 0)
                    for yy in range(sb[1], sb[3]):
                        for xx in range(4):
                            canvas.putpixel((xx, yy), 255)
                    x0 += sb[0] + (sb[2] - sb[0] - 4) // 2
                    mask, w = canvas, 4
                    pix = list(canvas.tobytes())
            off = len(data)
            data.extend(pix)
            if crisp and w > 0:
                # outlined (Classic, as in the approved mock): set by ink, each face 1 px after the last one's (one dark pixel of shared outline between them, as the game's own lettering), so the
                # outlines never merge at this size (the font's own spacing assumes no outline); digits tabular
                solid_th = 112 if (ch.isdigit() or code in PROP or ch in "×/-" or code == 0x80) else 80
                def solid_box(m, th=None):
                    # the ink the renderer outlines (coverage 80 and up; 112 for the glyphs it draws solid, the figures
                    # and their signs, whose fainter pixels it drops): faint edge pixels don't count as letter width
                    t = solid_th if th is None else th
                    return m.point(lambda v: 255 if v >= t else 0).getbbox()
                def as_img(m):
                    return _I.frombytes("L", m.size, bytes(m))
                bb = solid_box(mask)
                if bb:
                    ink_w = bb[2] - bb[0]
                    if tabular(code):
                        if designed("0") is not None:
                            boxes = [(0, 0, len(designed(d)[1][0]), 1) for d in "0123456789"]
                        else:
                            boxes = [solid_box(as_img(font.getmask2(d, mode="L", anchor="ls", features=feats)[0])) for d in "0123456789"]
                        ink_w_max = max(b[2] - b[0] for b in boxes if b)
                        x0 = -bb[0] + (ink_w_max - ink_w) // 2
                        adv = ink_w_max + 1
                    else:
                        x0 = -bb[0]
                        adv = ink_w + 1
            a64 = int(round(adv * 64))
            adv64[code] = a64
            glyphs.append((a64, x0, base + y0, w, h, off))
        # subpixel positions: the pen lands on quarter pixels, so each glyph (not outlined) is also rasterised a
        # quarter, a half and three quarters of a pixel to the right; these follow the 96 whole-pixel glyphs
        phases = 1 if crisp else 4
        for p in range(1, phases):
            for idx, code in enumerate(CODES):
                ch = char_of(code)
                a64, x0p, yb, w0, h0, off0 = glyphs[idx]
                if chars and ch not in chars:
                    glyphs.append((0, 0, 0, 0, 0, 0))
                    continue
                cf = [f for f in feats if f != "tnum"] if code in PROP else char_feats(ch, feats)
                pix, w, h, x0, y0 = raster(font, bigx if ch == "×" else bigp if ch == "+" else big, ch, cf, p)
                if ch == "×":
                    x0 += xshift
                    if not crisp and 0 < _zh <= 11:
                        pix, w, h = times_pix()
                        x0 = 1
                    y0 = times_y(h, y0)
                elif ch == "+":
                    x0 += pshift
                adv = font.getlength(ch, features=cf)
                if tabular(code) and abs(adv - digit_adv) > 0.01:
                    x0 = int(round(x0 + (digit_adv - adv) / 2))
                off = len(data)
                data.extend(pix)
                glyphs.append((a64, x0, base + y0, w, h, off))
        # kerning: the pair's length less its glyphs' own advances (digits are tabular: never kerned)
        kerns = []
        live = [] if crisp else [c for c in CODES if adv64[c] and not char_of(c).isdigit()]
        kf = [f for f in feats if f != "tnum"]
        single = {c: font.getlength(char_of(c), features=kf) for c in live}
        for a in live:
            for b in live:
                k = font.getlength(char_of(a) + char_of(b), features=kf) - single[a] - single[b]
                k64 = int(round(k * 64))
                if abs(k64) >= 8:
                    kerns.append((a, b, k64))
        centre_marks(glyphs, data, crisp, phases)
        # a capital alone in a key cap or chip (A, B, X) is centred to the pixel: its ink is an even or odd number of
        # columns as its cap's room is, and a column of caps holds letters of both kinds. Two more sets follow the pen
        # phases' glyphs, at the whole pixel (zeros: none). The first: the key cap letters (KEYCAP) in their wide form,
        # Classic's (CLASSIC_WIDE) a column wider than its own, and any other capital drawn from its outline a few
        # percent wider (never a column repeated), mirror exact where the letter is symmetric. The second: the key cap
        # letters in their narrow form. A lone capital with both forms is always drawn in one of them
        for extra in (1, 0):
            for idx, code in enumerate(CODES):
                ch = char_of(code)
                a64, gx0, gy, gw, gh, goff = glyphs[idx]
                if not ("A" <= ch <= "Z") or not gw:
                    glyphs.append((0, 0, 0, 0, 0, 0))
                    continue
                if outlined and extra == 0:
                    glyphs.append((0, 0, 0, 0, 0, 0))
                    continue
                if outlined:
                    # the outlined font is FreeType's hinted bitmap, which can't be drawn wider: the key caps' letters are
                    # drawn by hand at its 8 px capitals, in its own chunky shapes (solid, as a boxed Classic label draws)
                    hand = CLASSIC_WIDE.get(ch)
                    rows0 = [j for j in range(gh) if any(data[goff + j * gw + i] >= 80 for i in range(gw))]
                    if hand is None or len(rows0) != len(hand):
                        glyphs.append((0, 0, 0, 0, 0, 0))
                        continue
                    w, h = len(hand[0]), len(hand)
                    off = len(data)
                    data.extend(255 if c == "#" else 0 for r in hand for c in r)
                    glyphs.append((a64, 0, gy + rows0[0], w, h, off))
                    continue
                hand = KEYCAP.get(name, {}).get(ch)
                hand = hand and hand[extra]
                if not hand and extra == 0:
                    glyphs.append((0, 0, 0, 0, 0, 0))  # (the narrow set: drawn letters only)
                    continue
                if hand:
                    rows0 = [j for j in range(gh) if any(data[goff + j * gw + i] for i in range(gw))]
                    assert len(rows0) == len(hand), (name, ch)
                    off = len(data)
                    data.extend(255 if c == "#" else 0 for r in hand for c in r)
                    glyphs.append((a64, 0, gy + rows0[0], len(hand[0]), len(hand), off))
                    continue
                cf = char_feats(ch, feats)
                def cols_of(pix, w, h, th):
                    return [i for i in range(w) if any(pix[j * w + i] >= th for j in range(h))]
                def span(c):
                    return c[-1] - c[0] + 1 if c else 0
                want = span(cols_of(data[goff:goff + gw * gh], gw, gh, 1)) + extra
                # (the variant stands on exactly the rows the letter does)
                rows0 = [gy + j for j in range(gh) if any(data[goff + j * gw + i] for i in range(gw))]
                best = None
                # tried at every stretch and every eighth of a pixel of pen: a letter drawn symmetric (A, X) must come out
                # mirror symmetric to the pixel; any letter's solid ink must sit in the middle of its edge; of those, the
                # one with the fewest half-covered pixels (a clean stroke, no light pixel in a solid one)
                for k in range(1, 40):
                    for e in range(8):
                        pix, w, h, x0, y0 = raster(font, big, ch, cf, e / 2.0, 1.0 + k * 0.01)
                        c = cols_of(pix, w, h, 1)
                        if span(c) != want:
                            continue
                        rows = [base + y0 + j for j in range(h) if any(pix[j * w + i] for i in range(w))]
                        if rows != rows0:
                            continue
                        sc = cols_of(pix, w, h, 184)
                        if not sc or sc[0] - c[0] != c[-1] - sc[-1]:
                            continue
                        if ch in SYMMETRIC and any(pix[j * w + c[0] + i] != pix[j * w + c[-1] - i]
                                                   for j in range(h) for i in range(len(c))):
                            continue
                        half = sum(1 for v in pix if 0 < v < 255)
                        if best is None or half < best[0]:
                            best = (half, pix, w, h, x0, y0)
                if best is None:
                    glyphs.append((0, 0, 0, 0, 0, 0))
                    continue
                _, pix, w, h, x0, y0 = best
                off = len(data)
                data.extend(pix)
                glyphs.append((a64, x0, base + y0, w, h, off))
        if crisp:
            kerns += optical_kerns(glyphs, data, adv64)
        # a sign hugs its number: the minus (a digit wide, for "--:--" placeholders) is kerned against each digit so its
        # ink sits as close to the number as the plus's does ("+1.41", "-0.61")
        gp, gm = glyphs[ord("+") - 0x21], glyphs[0x80 - 0x21]
        if gp[0] and gm[0] and gp[3] and gm[3]:
            rsb = lambda g: g[0] - (g[1] + g[3]) * 64
            k64 = rsb(gp) - rsb(gm)
            if k64 != 0:
                kerns += [(0x80, ord(d), k64) for d in "0123456789"]
        # a count's sign sits as close to a narrow digit as to a wide one: x kerned against each digit so the ink gap
        # (x's last inked column to the digit's first, any row) is the same for every figure ("x1" read with a hole
        # beside "x7"); outlined fonts leave 3 columns, so the x's outline and the digit's are apart by a clear pixel
        def ink_cols(g):
            a64, x0, y, w, h, off = g
            return [max(data[off + j * w + i] for j in range(h)) / 255.0 for i in range(w)]
        def left_edge(g, th=0.5):
            c = ink_cols(g)
            for i, v in enumerate(c):
                if v >= th:
                    return g[1] + i - (c[i - 1] if i else 0.0)
            return None
        def right_edge(g, th=0.5):
            c = ink_cols(g)
            for i in range(len(c) - 1, -1, -1):
                if c[i] >= th:
                    return g[1] + i + (c[i + 1] if i + 1 < len(c) else 0.0)
            return None
        gt, g0 = glyphs[0x7F - 0x21], glyphs[ord("0") - 0x21]
        if gt[0] and gt[3] and g0[3]:
            if crisp:
                r = right_edge(gt, 0.3)
                for d in "0123456789":
                    gd = glyphs[ord(d) - 0x21]
                    if gd[3]:
                        k = (r + 4 - gt[0] / 64.0 - left_edge(gd, 0.3)) * 64
                        k64 = int(round((gt[0] + k) / 64.0)) * 64 - gt[0]
                        if k64:
                            kerns.append((0x7F, ord(d), k64))
            else:
                # set as drawn: the x at a whole pixel, the digit at the quarter-pixel copy its pen lands on; the gap is
                # the columns between the two inks (half coverage and up), each counted by how empty it is
                tc = ink_cols(gt)
                tr = gt[1] + max(i for i, v in enumerate(tc) if v >= 0.5)
                def gap(d, k):
                    q = (gt[0] + k + 8) >> 4
                    gd = glyphs[(q & 3) * NG + ord(d) - 0x21]
                    dc = ink_cols(gd)
                    x0 = (q >> 2) + gd[1]
                    first = min(i for i, v in enumerate(dc) if v >= 0.5)
                    empty = 0.0
                    for x in range(tr + 1, x0 + first):
                        i = x - x0
                        empty += 1.0 - (dc[i] if 0 <= i < len(dc) else 0.0)
                    return empty
                target = max(2, round(gap("0", 0)))  # never under 2 columns: a sign set tight reads as one glyph
                for d in "0123456789":
                    if not glyphs[ord(d) - 0x21][3]:
                        continue
                    k64 = min(range(-128, 129, 16), key=lambda k: (round(abs(gap(d, k) - target), 2), abs(k)))
                    if k64:
                        kerns.append((0x7F, ord(d), k64))
        # words are spaced by the font's own space glyph and side bearings, as a word processor sets them: no pair is
        # kerned against the space, and no pair is opened beyond the font's own kerning. Classic (a bitmap set with one
        # fixed space) is the exception: its tabular digits sit in cells wider than their ink, so a space beside a digit
        # is kerned by the cell's extra bearing (whole pixels), and "budget 33" reads as one space, not two
        if crisp:
            lows = [glyphs[c - 0x21] for c in range(ord("a"), ord("z") + 1) if glyphs[c - 0x21][0] and glyphs[c - 0x21][3]]
            ls = sorted(left_edge(g) for g in lows)
            rs = sorted(g[0] / 64.0 - right_edge(g) for g in lows)
            if ls:
                lref, rref = ls[len(ls) // 2], rs[len(rs) // 2]
                for d in "0123456789":
                    g = glyphs[ord(d) - 0x21]
                    if not g[0] or not g[3]:
                        continue
                    kb = int(round(lref - left_edge(g))) * 64
                    ka = int(round(rref - (g[0] / 64.0 - right_edge(g)))) * 64
                    if kb < 0:
                        kerns.append((0x20, ord(d), kb))
                    if ka < 0:
                        kerns.append((ord(d), 0x20, ka))
        merged = {}
        for a, b, k in kerns:
            merged[(a, b)] = merged.get((a, b), 0) + k
        kerns = sorted((a, b, k) for (a, b), k in merged.items() if k)
        space64 = int(round(font.getlength(" ", features=feats) * 64))
        if crisp:
            hh = font.getbbox("H", anchor="ls")
            space64 = ((hh[3] - hh[1]) // 2 - 1) * 64  # a word space about 2 px wider than a letter gap
            if design:
                # the drawn letters: a word gap 2 px wider than a letter gap (3 px), 3 px wider in Classic's 10 row heading
                # and the 12 row one (4 px)
                space64 = (2 if prows < 10 or (prows == 10 and not outlined) else 3) * 64
        lines.append("static const unsigned char k%sPixels[%d] = {" % (name, max(1, len(data))))
        for i in range(0, max(1, len(data)), 24):
            lines.append("    " + ",".join(str(v) for v in (data or [0])[i:i + 24]) + ",")
        lines.append("};")
        lines.append("static const UiGlyph k%sGlyphs[%d] = {" % (name, len(glyphs)))
        for g in glyphs:
            lines.append("    { %d, %d, %d, %d, %d, %d }," % g)
        lines.append("};")
        lines.append("static const UiKern k%sKerns[%d] = {" % (name, max(1, len(kerns))))
        for a, b, k in (kerns or [(0, 0, 0)]):
            lines.append("    { %d, %d, %d }," % (a, b, k))
        lines.append("};")
        lines.append("")
        names.append((name, space64, base, len(kerns), 1 if outlined else 0, phases))
    for name, space64, base, nk, st, ph in names:
        lines.append("static const UiFont k%s = { %d, %d, k%sGlyphs, k%sPixels, k%sKerns, %d, %d, %d };" % (name, space64, base, name, name, name, nk, st, ph))
    open(out, "w").write("\n".join(lines) + "\n")

main()
