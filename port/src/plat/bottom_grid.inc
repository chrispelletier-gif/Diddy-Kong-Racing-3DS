/*
 * O1h redo (scratch, design only): one spacing system for every screen in every look.
 *
 *   frame    x 6..314 (side margins 6), content y 4..208, button bar y 212..236, bottom margin 4
 *   columns  4 x 74 with 4 px gaps (6..80, 84..158, 162..236, 240..314); 2 x 152 (6..158, 162..314);
 *            3 x 100 (6..106, 110..210, 214..314)
 *   rows     8 slots of 22 with 4 px gaps: slot k is y 4+26k .. 26+26k
 *   in a box 6 px padding; text centred on the band of its capitals (descenders hang below), numbers on their
 *            digits' band, pictures on their ink; numbers drawn tabular so decimal points and colons line up
 *
 * Every box spans whole columns and whole slots, so edges line up across screens and with the buttons.
 * Included by bottom_screen.c; switched on with DKR_BOTTOM_GRID (host preview) or bs_set_grid().
 */

#define GX0 8
#define GX1 312
#define GY0 8
#define GY1 196
#define GGAP 8  /* between columns */
#define GGAPY 4 /* between rows */
#define GCOLW 70
#define GSLOT 20
#define GPITCH 24
#define GPAD 6
#define GBTN_Y0 200
#define GBTN_Y1 232
/* Touch rows (Settings): 24 tall on a 28 pitch, so each meets the stylus minimum (24 seen, 28 hit) */
#define GTSLOT 24
#define GTPITCH 28


static int gy0(int k) {
    return GY0 + k * GPITCH;
}
static int gy1(int k) {
    return GY0 + k * GPITCH + GSLOT;
}
static int gc0(int k) {
    return GX0 + k * (GCOLW + GGAP);
}
static int gc1(int k) {
    return GX0 + k * (GCOLW + GGAP) + GCOLW;
}
static int gty0(int k) {
    return GY0 + k * GTPITCH;
}
static int gty1(int k) {
    return GY0 + k * GTPITCH + GTSLOT;
}

/* Box i of n equal boxes from x0 to x1 with GGAP between; logs an error when they can't be equal. */
static void gsplit(int x0, int x1, int n, int i, int *a, int *b) {
    int w = (x1 - x0 - GGAP * (n - 1)) / n;
    if (n * w + GGAP * (n - 1) != x1 - x0) {
        log_rec('E', x0, 0, x1, 0, n, "unequal-split");
    }
    *a = x0 + i * (w + GGAP);
    *b = *a + w;
}

/* ---- Measuring what a drawing call puts on screen (its ink), without drawing it ---- */
typedef struct Ink {
    int x0, y0, x1, y1; /* x1, y1 exclusive; x1 <= x0 when nothing */
} Ink;



#define MEASURE(expr, out)                                                                                            \
    do {                                                                                                              \
        int mTop_ = sClipTop, mEnd_ = sClipEnd, mQuiet_ = sQuiet, mDry_ = sDryInk, mTrack_ = sTrack;                 \
        int mX0_ = sInkX0, mY0_ = sInkY0, mX1_ = sInkX1, mY1_ = sInkY1;                                              \
        sClipTop = -100000;                                                                                           \
        sClipEnd = 100000;                                                                                            \
        sDryInk = 1;                                                                                                  \
        sQuiet = 1;                                                                                                   \
        sInkX0 = sInkY0 = 99999;                                                                                      \
        sInkX1 = sInkY1 = -99999;                                                                                     \
        sTrack = 1;                                                                                                   \
        (void) (expr);                                                                                                \
        (out).x0 = sInkX0;                                                                                            \
        (out).y0 = sInkY0;                                                                                            \
        (out).x1 = sInkX1 + 1;                                                                                        \
        (out).y1 = sInkY1 + 1;                                                                                        \
        sTrack = mTrack_;                                                                                             \
        sDryInk = mDry_;                                                                                              \
        sQuiet = mQuiet_;                                                                                             \
        sClipTop = mTop_;                                                                                             \
        sClipEnd = mEnd_;                                                                                             \
        sInkX0 = mX0_;                                                                                                \
        sInkY0 = mY0_;                                                                                                \
        sInkX1 = mX1_;                                                                                                \
        sInkY1 = mY1_;                                                                                                \
    } while (0)

#define QUIET(expr)                                                                                                   \
    do {                                                                                                              \
        int q_ = sQuiet;                                                                                              \
        sQuiet = 1;                                                                                                   \
        (void) (expr);                                                                                                \
        sQuiet = q_;                                                                                                  \
    } while (0)

/* ---- Fonts: the look's, and the band their capitals and digits sit in ---- */
enum { GF_BODY, GF_HEAD, GF_SM, GF_LG, GF_XL };
static int fun_kind(int kind) {
    return kind == GF_HEAD || kind == GF_LG || kind == GF_XL;
}
static void gband(int kind, int digits, int *top, int *h);
static int sFlatMode; /* Classic fun font: 2 draws a disabled label (no outline, no shadow) */
static int sReadText; /* set while drawing text that is read, not glanced at (debug pages, status lines) */
static int sTabSmall; /* set while drawing a debug build's 10 tabs: Classic's small type is Glass's small size there */

static const UiFont *gfont(int kind) {
    if (sStyle == 2 && sReadText && kind == GF_BODY) {
        /* Broadcast and Carbon: Barlow's body size is 14 rows from cap top to descender, the debug pages' whole pitch
           (no leading: "rdp" touched the line under it); its small size has Glass's 9 px capitals and 2 px of leading */
        return ui_font_kind(UI_SM);
    }
    switch (kind) {
        case GF_HEAD:
            return ui_font_kind(UI_HEAD);
        case GF_SM:
            return ui_font_kind(UI_SM);
        case GF_LG:
            return ui_font_kind(UI_LG);
        case GF_XL:
            return ui_font_kind(UI_XL);
        default:
            return ui_font_kind(UI_BODY);
    }
}

/*
 * Typesetting: a count's "x" before its number becomes the font's multiplication sign ("x3": the sign centred on the
 * digits, not a letter on the baseline), and a "-" that starts a number its minus sign (a hyphen sits low and short
 * beside digits; "2026-10-23" keeps its hyphens).
 */
static void gtypeset(const char *s, char *out, int size) {
    int i, n = 0;
    for (i = 0; s[i] != '\0' && n < size - 1; i++) {
        unsigned char c = (unsigned char) s[i];
        int starts = i == 0 || s[i - 1] == ' ' || s[i - 1] == '(';
        int digit = s[i + 1] >= '0' && s[i + 1] <= '9';
        if (c == 'x' && ((starts && digit) || (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9' &&
                                                (s[i + 1] == '\0' || s[i + 1] == ' ')))) {
            c = UI_TIMES; /* a count ("x3") or a factor ("4x": anti-aliasing) */
        } else if (c == '-' && starts && digit) {
            c = UI_MINUS;
        } else if (c == '-') {
            /* a time not set yet ("--'--\"--"): the minus, a digit wide, so the dashes stand where the digits will */
            int a = i, b = i, only = 1;
            while (a > 0 && s[a - 1] != ' ') {
                a--;
            }
            while (s[b] != '\0' && s[b] != ' ') {
                b++;
            }
            for (; a < b; a++) {
                only = only && (s[a] == '-' || s[a] == ':' || s[a] == '.' || s[a] == '\'' || s[a] == '"');
            }
            if (only) {
                c = UI_MINUS;
            }
        }
        out[n++] = (char) c;
    }
    out[n] = '\0';
}

/* Classic has no look fonts: the game's small font for body text, its fun font for headings. */
#define GFUN_GAP sFunGap
static int sFunGap = 1; /* gdraw_tight sets 0 */
#define GFUN_SPACE 6
static int gdraw_raw(int kind, int x, int y, const char *s, Colour c) {
    const UiFont *u = gfont(kind);
    if (u != NULL) {
        char t[256];
        int i;
        gtypeset(s, t, sizeof(t));
        /* words with digits in them take the proportional digits ("sdmc:/3ds", "v0.3"); a number standing as a word of
           its own ("12.1", "36%", "10-26 23:45") keeps the tabular ones, whose 1 has its full side bearings */
        for (i = 0; t[i] != '\0'; i++) {
            int w0 = i, w1 = i, letters = 0, j;
            if (t[i] < '0' || t[i] > '9') {
                continue;
            }
            while (w0 > 0 && t[w0 - 1] != ' ') {
                w0--;
            }
            while (t[w1] != '\0' && t[w1] != ' ') {
                w1++;
            }
            for (j = w0; j < w1; j++) {
                unsigned char ch = (unsigned char) t[j];
                letters |= (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch >= 0xC0;
            }
            if (letters || u->outlined) { /* (Classic: its figures set by ink, 1 px apart, in numbers too) */
                t[i] = (char) (UI_PROP0 + (t[i] - '0'));
            }
        }
        return ui_draw(u, x, y, t, c, 0, 100000);
    }
    if (!fun_kind(kind) && strchr(s, '\'') != NULL) {
        /* the game's small font draws "'" as a backtick: a straight 1 x 3 tick in its place, on the capitals' top */
        char part[128];
        const char *q = strchr(s, '\'');
        int n = (int) (q - s), r, top, h, j;
        snprintf(part, sizeof(part), "%.*s", n < 127 ? n : 127, s);
        r = n > 0 ? gdraw_raw(kind, x, y, part, c) : x;
        gband(kind, 0, &top, &h);
        for (j = 0; j < 3; j++) {
            plot_c(r + 1, y + top + j, c, 255);
        }
        return gdraw_raw(kind, r + 3, y, q + 1, c);
    }
    if ((kind == GF_HEAD || kind == GF_LG || kind == GF_XL) && s[0] != '\0' && s[1] != '\0') {
        /* the fun font's side bearings are uneven ("NE XT", "PR EV"): set it a letter at a time, each letter's ink
           GFUN_GAP px after the last one's, and a word space a fixed GFUN_SPACE */
        int ink = x, any = 0, last = x;
        for (; *s != '\0'; s++) {
            char one[2] = { *s, 0 };
            Ink k;
            if (*s == ' ') {
                ink += GFUN_SPACE - GFUN_GAP;
                continue;
            }
            MEASURE(gdraw_raw(kind, 0, 0, one, c), k);
            if (k.x0 > k.x1) {
                continue;
            }
            last = gdraw_raw(kind, ink - k.x0, y, one, c);
            ink += k.x1 - k.x0 + GFUN_GAP;
            any = 1;
        }
        return any ? last : x;
    }
    {
        /* Classic's fun font in one ink with its outline (its red-yellow fill was hard to read); a disabled label flat */
        int fun = kind == GF_HEAD || kind == GF_LG || kind == GF_XL, r, f = sFlatInk;
        sFlatInk = fun ? (sFlatMode ? sFlatMode : 1) : 0;
        r = text_clipped_as_is_impl(fun ? FONT_FUN : FONT_SMALL, x, y, s, c,
                                    (kind == GF_HEAD || kind == GF_XL) && sFlatMode != 2, 100000);
        sFlatInk = f;
        return r;
    }
}

/* Was: letters 1 px closer. Text is never squeezed now: it is set as the font sets it. */
static int gdraw_tight(int kind, int x, int y, const char *s, Colour c) {
    return gdraw_raw(kind, x, y, s, c);
}

/* The widest digit's advance: every digit gets a cell this wide (tabular figures). */
static int gdigit_cell(int kind) {
    int best = 0, d;
    for (d = 0; d < 10; d++) {
        char one[2] = { (char) ('0' + d), 0 };
        Ink k;
        int a;
        const UiFont *u = gfont(kind);
        if (u != NULL) {
            a = ui_advance(u, (unsigned char) one[0]);
        } else {
            MEASURE(gdraw_raw(kind, 0, 0, one, WHITE), k);
            a = k.x1 - k.x0 + 1;
        }
        best = a > best ? a : best;
    }
    return best;
}

static void gband(int kind, int digits, int *top, int *h);

/* The dot size of drawn points and colons: the digits' stroke (2 px from a 7 px digit band up, else 1). */
static int gmark_dot(int kind) {
    int top, h;
    gband(kind, 1, &top, &h);
    return h >= 7 ? 2 : 1;
}

/* Fonts whose points and colons are drawn by gdraw_tab: all but the game's fun font (it has its own outline). */
static int gdrawn_marks(int kind) {
    return gfont(kind) != NULL || kind == GF_BODY || kind == GF_SM;
}

/* The cell of a point or colon (the widest of their inks + 1 px each side), or of a sign (the wider of + and -, + 2 px). */
static int gpunct_cell(int kind, char ch) {
    static const char *const sPoint = ".:,", *const sSign = "+-";
    const char *set = ch == '+' || ch == '-' ? sSign : sPoint;
    int best = 0;
    for (; *set != '\0'; set++) {
        char one[2] = { *set, 0 };
        Ink k;
        MEASURE(gdraw_raw(kind, 0, 0, one, WHITE), k);
        best = k.x1 - k.x0 > best ? k.x1 - k.x0 : best;
    }
    return best + 2; /* 1 px each side; the digit cells add their own side bearing */
}

static int sPtN, sPtX0[8], sPtX1[8]; /* the last gdraw_tab's points and colons (ink left/right), for the log */

/* Draws text at 0,0 seeing only rows r0..r1-1 (inside MEASURE, which restores the rows): a glyph's ink on a band. */
static int gdraw_rows(int kind, const char *s, int r0, int r1) {
    sClipTop = r0;
    sClipEnd = r1;
    return gdraw_raw(kind, 0, 0, s, WHITE);
}

/* The ink left of digit d drawn in its cell at x, seen on rows r0..r1-1 of the line (a sign hugs what is beside it:
   a footed 1's foot is far left of its stem, so the sign keeps its gap to the stem, not the foot). */
static int gdigit_ink_left(int kind, int x, int cell, char d, int r0, int r1) {
    Ink k, kr;
    char nd[2] = { d, 0 };
    MEASURE(gdraw_raw(kind, 0, 0, nd, WHITE), k);
    MEASURE(gdraw_rows(kind, nd, r0, r1), kr);
    if (kr.x0 > kr.x1) {
        kr.x0 = k.x0;
    }
    return x + (cell - (k.x1 - k.x0)) / 2 - k.x0 + kr.x0;
}

/*
 * Classic numbers: the game's own race-timer digits (TIMERNUMBERSMALL, yellow with a dark drop shadow) instead of the
 * thin menu font, tinted to the number's ink (white keeps their yellow), with a colon from the same sprites, a point
 * from the colon's lower dot, and signs and dashes drawn as bars with the same shadow.
 */
/* Classic numbers are the game's timer sprites: the small set for text-size numbers, the big set for counts (GF_LG) */
static int sSprBig;
#define GDIG (sSprBig ? sDigits : sSmallDigits)
#define GCOLON (sSprBig ? &sColon : &sSmallColon)
static int gsprite_nums(int kind) {
    if (sStyle != 0 || sReadText || (kind != GF_BODY && kind != GF_LG) || gfont(kind) != NULL) {
        return 0; /* Classic's numbers are set in its font now (tabular figures), like every look's */
    }
    sSprBig = 0; /* one upright set for every Classic number: the big set is italic, and its 1 read as broken ("x1") */
    return GDIG[0].rgba != NULL && GCOLON->rgba != NULL;
}

static void gsprite_ink(const Picture *p, int *x0, int *y0, int *x1, int *y1) {
    int x, y;
    *x0 = *y0 = 1000;
    *x1 = *y1 = -1;
    for (y = 0; y < p->height; y++) {
        for (x = 0; x < p->width; x++) {
            if (p->rgba[(y * p->width + x) * 4 + 3] != 0) {
                *x0 = x < *x0 ? x : *x0;
                *y0 = y < *y0 ? y : *y0;
                *x1 = x + 1 > *x1 ? x + 1 : *x1;
                *y1 = y + 1 > *y1 ? y + 1 : *y1;
            }
        }
    }
}

/*
 * The timer font's "1" is a bare stem, narrower than its cell ("00:3 1:0 1" left a gap in it): on the grid it gets a
 * foot like the other looks' footed 1 (rule 20). The sprite's own stem and flag, moved so the stem stands on the middle
 * of the 0's bottom; its last two face rows widened to the 0's face there, with the sprite's dark edge round the foot and
 * its shadow rows under it. Built once per set from the sprites themselves (their colours row by row).
 */
static const Picture *gdig(int d) {
    static Picture sFooted[2];
    static unsigned char sPix[2][24 * 24 * 4];
    int k = sSprBig;
    const Picture *one = &GDIG[1], *zero = &GDIG[0];
    if (d == 4) {
        /* the 4 is a column wider than the 0: without its bar's middle column it fits the 0's cell like every digit */
        static Picture sNarrow[2];
        static unsigned char sNPix[2][24 * 24 * 4];
        const Picture *four = &GDIG[4];
        int fx0, fy0, fx1, fy1, zx0, zy0, zx1, zy1, x, y, cut;
        if (sNarrow[k].rgba == NULL && four->rgba != NULL && four->width <= 24 && four->height <= 24) {
            gsprite_ink(four, &fx0, &fy0, &fx1, &fy1);
            gsprite_ink(zero, &zx0, &zy0, &zx1, &zy1);
            if (fx1 - fx0 <= zx1 - zx0) {
                return four;
            }
            cut = fx0 + 4; /* between the left stroke and the stem */
            memset(sNPix[k], 0, sizeof(sNPix[k]));
            for (y = 0; y < four->height; y++) {
                for (x = 0; x < four->width; x++) {
                    int nx = x < cut ? x : x - 1;
                    if (x == cut) {
                        continue;
                    }
                    memcpy(sNPix[k] + (y * four->width + nx) * 4, four->rgba + (y * four->width + x) * 4, 4);
                }
            }
            sNarrow[k] = *four;
            sNarrow[k].rgba = sNPix[k];
        }
        return sNarrow[k].rgba != NULL ? &sNarrow[k] : four;
    }
    if (d != 1) {
        return &GDIG[d];
    }
    if (sFooted[k].rgba == NULL && one->rgba != NULL && zero->width <= 24 && zero->height <= 24 &&
        one->height == zero->height) {
        int w = zero->width, h = zero->height, x, y, last = -1, zl = -1, zr = -1, sl = -1, sr = -1, dx;
        unsigned char *px = sPix[k], edge[4] = { 16, 16, 16, 255 };
#define GFACE(p, xx, yy) ((p)->rgba[((yy) * (p)->width + (xx)) * 4 + 3] != 0 && \
                          ((p)->rgba[((yy) * (p)->width + (xx)) * 4] * 3 + (p)->rgba[((yy) * (p)->width + (xx)) * 4 + 1] * 6 + \
                           (p)->rgba[((yy) * (p)->width + (xx)) * 4 + 2]) / 10 >= 90)
        for (y = 0; y < h; y++) {
            for (x = 0; x < one->width; x++) {
                if (GFACE(one, x, y)) {
                    last = y;
                }
            }
        }
        if (last < 3) {
            return &GDIG[1];
        }
        /* the 0's face and the 1's stem on the foot's upper row */
        for (x = 0; x < w; x++) {
            if (GFACE(zero, x, last - 1)) {
                zl = zl < 0 ? x : zl;
                zr = x;
            }
        }
        for (x = 0; x < one->width; x++) {
            if (GFACE(one, x, last - 1)) {
                sl = sl < 0 ? x : sl;
                sr = x;
            }
            if (one->rgba[((last + 1) * one->width + x) * 4 + 3] != 0 && last + 1 < h) {
                memcpy(edge, one->rgba + ((last + 1) * one->width + x) * 4, 4);
            }
        }
        if (zl < 0 || sl < 0) {
            return &GDIG[1];
        }
        dx = (zl + zr) / 2 - (sl + sr) / 2;
        memset(px, 0, (size_t) w * h * 4);
        for (y = 0; y < h; y++) {
            for (x = 0; x < one->width; x++) {
                int nx = x + dx;
                if (nx >= 0 && nx < w && one->rgba[(y * one->width + x) * 4 + 3] != 0) {
                    memcpy(px + (y * w + nx) * 4, one->rgba + (y * one->width + x) * 4, 4);
                }
            }
        }
        for (y = last - 1; y <= last; y++) {
            /* the face's colour on this row: the stem's own */
            unsigned char face[4];
            memcpy(face, one->rgba + (y * one->width + sl) * 4, 4);
            for (x = zl - 1; x <= zr + 1; x++) {
                if (x >= 0 && x < w) {
                    memcpy(px + (y * w + x) * 4, x < zl || x > zr ? edge : face, 4);
                }
            }
        }
        /* the edge over the foot (where the stem is not), and the shadow rows under it */
        for (x = zl - 1; x <= zr + 1; x++) {
            if (x >= 0 && x < w && px[((last - 2) * w + x) * 4 + 3] == 0) {
                memcpy(px + ((last - 2) * w + x) * 4, edge, 4);
            }
            for (y = last + 1; y < h; y++) {
                if (x >= 0 && x < w) {
                    memcpy(px + (y * w + x) * 4, edge, 4);
                }
            }
        }
#undef GFACE
        sFooted[k] = (Picture){ w, h, px };
    }
    return sFooted[k].rgba != NULL ? &sFooted[k] : &GDIG[1];
}

/* The digits' cell (the 0's ink + 1 px; the 4's extra column falls in its shadow side) and band (all ten digits' ink rows, from the sprite's top). */
static void gsprite_metrics(int *cell, int *top, int *h) {
    static int sCell[2], sTop[2], sH[2];
    int k = sSprBig;
    if (sCell[k] == 0) {
        int d, x0, y0, x1, y1, t = 1000, b = -1;
        for (d = 0; d < 10; d++) {
            gsprite_ink(&GDIG[d], &x0, &y0, &x1, &y1);
            /* the cell is the common digit's ink (face + its outline) + 1: every digit 1 px clear of the next, as the
               other looks track (the 4, a column wider, is drawn a column narrower: gdig) */
            sCell[k] = d == 0 ? x1 - x0 + 1 : sCell[k];
            t = y0 < t ? y0 : t;
            b = y1 > b ? y1 : b;
        }
        sTop[k] = t;
        sH[k] = b - t;
    }
    *cell = sCell[k];
    *top = sTop[k];
    *h = sH[k];
}

/* A sprite pixel by pixel: its dark shadow as it is, its face in the ink (white: the sprite's own colour). */
static void gsprite_draw(const Picture *p, int sx0, int sy0, int sx1, int sy1, int x, int y, Colour ink) {
    int i, j;
    int white = ink.r > 240 && ink.g > 240 && ink.b > 240;
    for (j = sy0; j < sy1; j++) {
        for (i = sx0; i < sx1; i++) {
            const unsigned char *t = p->rgba + (j * p->width + i) * 4;
            int lum = (t[0] * 3 + t[1] * 6 + t[2]) / 10;
            Colour c;
            if (t[3] == 0) {
                continue;
            }
            if (white || lum < 90) {
                c = (Colour){ t[0], t[1], t[2], t[3] };
            } else {
                int f = 150 + 105 * lum / 255;
                c = (Colour){ (unsigned char) (ink.r * f / 255), (unsigned char) (ink.g * f / 255),
                              (unsigned char) (ink.b * f / 255), t[3] };
            }
            plot_c(x + i - sx0, y + j - sy0, c, 255);
        }
    }
}

static int gsprite_shadow_only, gsprite_face_only; /* gsprite_bar: one layer only (a plus's two strokes) */

/* A bar in the digits' style: the face 2 px tall in the ink (white: the digits' yellow), a 1 px shadow below and right. */
static void gsprite_bar(int x, int y, int w, int hgt, Colour ink) {
    static const Colour shadow = { 16, 16, 16, 255 }, yellow = { 248, 216, 32, 255 };
    Colour face = ink.r > 240 && ink.g > 240 && ink.b > 240 ? yellow : ink;
    int i, j;
    for (j = 0; j < hgt && !gsprite_face_only; j++) {
        for (i = 0; i < w; i++) {
            plot_c(x + i + 1, y + j + 1, shadow, 255);
        }
    }
    for (j = 0; j < hgt && !gsprite_shadow_only; j++) {
        for (i = 0; i < w; i++) {
            plot_c(x + i, y + j, face, 255);
        }
    }
}

static int gdraw_tab_sprites(int kind, int x, int y, const char *s, Colour c) {
    int cell, top, h, mid, cx0, cy0, cx1, cy1;
    (void) kind;
    gsprite_metrics(&cell, &top, &h);
    mid = y + top + h / 2 - 1;
    gsprite_ink(GCOLON, &cx0, &cy0, &cx1, &cy1);
    for (; *s != '\0'; s++) {
        if (*s >= '0' && *s <= '9') {
            /* every digit centred in its fixed cell, so points and colons line up from row to row (the 1 is footed to
               fill its cell like the others: gdig) */
            const Picture *p = gdig(*s - '0');
            int x0, y0, x1, y1;
            gsprite_ink(p, &x0, &y0, &x1, &y1);
            gsprite_draw(p, x0, 0, x1, p->height, x + (cell - (x1 - x0)) / 2, y, c);
            x += cell;
        } else if (*s == ':' || *s == '.') {
            /* the colon sprite, or its lower dot alone for a point */
            int from = *s == ':' ? cy0 : (cy0 + cy1) / 2;
            gsprite_draw(GCOLON, cx0, from, cx1, cy1, x + 1, y + from, c);
            if (sPtN < 8) {
                sPtX0[sPtN] = x + 1;
                sPtX1[sPtN++] = x + 1 + (cx1 - cx0);
            }
            x += cx1 - cx0 + 2; /* 1 px clear each side of the colon's ink (shadow included) */
        } else if (*s == '-' && !(s[1] >= '0' && s[1] <= '9')) {
            gsprite_bar(x + 1, mid, cell - 3, 2, c); /* a placeholder dash fills a digit's cell */
            x += cell;
        } else if (*s == '-' || *s == '+') {
            /* a sign hugs its number's first digit (2 px clear), wherever the packed run puts it */
            int w = 6, at = x;
            if (*s == '+') {
                /* both strokes' shadows first, then both faces, so neither shadow cuts the other stroke */
                gsprite_shadow_only = 1;
                gsprite_bar(at, mid, w, 2, c);
                gsprite_bar(at + 2, mid - 2, 2, 6, c);
                gsprite_shadow_only = 0;
                gsprite_face_only = 1;
                gsprite_bar(at, mid, w, 2, c);
                gsprite_bar(at + 2, mid - 2, 2, 6, c);
                gsprite_face_only = 0;
            } else {
                gsprite_bar(at, mid, w, 2, c);
            }
            x += w + 1; /* the digit's cell adds its own pixel: 2 px clear, as between digits */
        } else if (*s == 'x' && (s[1] >= '0' && s[1] <= '9')) {
            /* a count's "x": a cross in the digits' style (the italic letter leaned away from its number, "x 1") */
            int n = h * 5 / 8, i, t;
            int cy = y + top + (h - n) / 2;
            for (t = 0; t < 2; t++) {
                gsprite_shadow_only = t == 0;
                gsprite_face_only = t == 1;
                for (i = 0; i < n; i++) {
                    gsprite_bar(x + i * (n - 2) / (n - 1), cy + i, 2, 1, c);
                    gsprite_bar(x + (n - 1 - i) * (n - 2) / (n - 1), cy + i, 2, 1, c);
                }
            }
            gsprite_shadow_only = gsprite_face_only = 0;
            x += n; /* its shadow column touches the number's cell: the italic digits lean clear of it */
        } else if (*s == '/') {
            /* all of the slash's shadow first, then its face: drawn row by row, each shadow darkened the row below */
            int i;
            gsprite_shadow_only = 1;
            for (i = 0; i < h; i++) {
                gsprite_bar(x + 1 + (h - 1 - i) * 4 / h, y + top + i, 2, 1, c);
            }
            gsprite_shadow_only = 0;
            gsprite_face_only = 1;
            for (i = 0; i < h; i++) {
                gsprite_bar(x + 1 + (h - 1 - i) * 4 / h, y + top + i, 2, 1, c);
            }
            gsprite_face_only = 0;
            x += 8;
        } else {
            char one[2] = { *s, 0 };
            Ink ko;
            MEASURE(gdraw_raw(kind, 0, 0, one, c), ko);
            /* a letter (the "x" of a count) sits on the digits' baseline, 1 px clear of them */
            gdraw_raw(kind, x - ko.x0, y + top + h - ko.y1, one, c);
            x += ko.x1 - ko.x0 + 1;
        }
    }
    return x;
}

/* Text with tabular digits (each in a cell of the widest digit's advance), so columns of numbers line up. */
static int gdraw_tab(int kind, int x, int y, const char *s, Colour c) {
    int cell = gdigit_cell(kind);
    const UiFont *uf = gfont(kind);
    sPtN = 0;
    if (uf != NULL) {
        /* the font's own tabular figures, kerning and points: the number is typeset as one line, not placed digit by
           digit; the points' and colons' ink is noted for the log */
        char t[256];
        int xs[UI_LINE_MAX], ph[UI_LINE_MAX], i;
        gtypeset(s, t, sizeof(t));
        ui_layout_ph(uf, t, xs, ph);
        for (i = 0; t[i] != '\0'; i++) {
            if ((t[i] == '.' || t[i] == ':' || t[i] == '\'' || t[i] == '"') && sPtN < 8) {
                const unsigned char *px;
                const UiGlyph *g = ui_glyph(uf, (unsigned char) t[i], ph[i], &px);
                sPtX0[sPtN] = x + xs[i] + g->x;
                sPtX1[sPtN++] = x + xs[i] + g->x + g->width;
            }
        }
        (void) cell;
        return ui_draw(uf, x, y, t, c, 0, 100000);
    }
    if (gsprite_nums(kind)) {
        return gdraw_tab_sprites(kind, x, y, s, c);
    }
    for (; *s != '\0'; s++) {
        char one[2] = { *s, 0 };
        if (*s >= '0' && *s <= '9') {
            Ink k;
            MEASURE(gdraw_raw(kind, 0, 0, one, c), k);
            gdraw_raw(kind, x + (cell - (k.x1 - k.x0)) / 2 - k.x0, y, one, c);
            x += cell;
        } else if (*s == '/' && gfont(kind) == NULL && kind != GF_BODY && kind != GF_SM) {
            /* The game's fun font has no slash: draw one on the digits' band, in its yellow with its dark edge. */
            int top, h, i, t, w;
            static const Colour yellow = { 248, 212, 40, 255 }, edge = { 8, 24, 16, 255 };
            /* the font draws white as its own yellow, any other ink as that ink */
            Colour fun = c.r > 240 && c.g > 240 && c.b > 240 ? yellow : c;
            gband(kind, 1, &top, &h);
            w = h / 2;
            for (t = 0; t < 2; t++) {
                for (i = 0; i <= h; i++) {
                    int px = x + 2 + w * (h - i) / h, py = y + top + i;
                    if (t == 0) {
                        int dx, dy;
                        for (dy = -1; dy <= 1; dy++) {
                            for (dx = -1; dx <= 2; dx++) {
                                plot_c(px + dx, py + dy, edge, 255);
                            }
                        }
                    } else {
                        plot_c(px, py, fun, 255);
                        plot_c(px + 1, py, fun, 255);
                    }
                }
            }
            x += w + 6;
        } else if ((*s == '.' || *s == ':' || *s == '-') && gdrawn_marks(kind)) {
            /*
             * Points, colons and placeholder dashes are drawn here, not taken from the font: the fonts' own are uneven
             * (one colon sits high, one is a single dot). Square dots of the digits' stroke on the digits' band: a
             * point on the baseline, a colon's two dots either side of the band's middle, a dash across a digit cell
             * at the middle. Points and colons get a cell of the dot plus 1 px each side; a dash takes a digit's cell.
             */
            int top, h, d = gmark_dot(kind), mid, w, i, j, gap;
            gband(kind, 1, &top, &h);
            mid = y + top + h / 2;
            if (*s == '-' && !(s[1] >= '0' && s[1] <= '9')) {
                for (j = 0; j < d; j++) {
                    for (i = 1; i < cell - 1; i++) {
                        plot_c(x + i, mid - d / 2 + j, c, 255);
                    }
                }
                x += cell;
            } else if (*s == '-') {
                /* a sign: a bar the width of a dot-heavy dash, hugging the first digit's ink */
                int bar = h / 2 + 1, nx;
                /* the next digit's ink left on the bar's rows */
                nx = gdigit_ink_left(kind, x + gpunct_cell(kind, '+'), cell, s[1], top + h / 2 - d / 2, top + h / 2 - d / 2 + d);
                for (j = 0; j < d; j++) {
                    for (i = 0; i < bar; i++) {
                        plot_c(nx - d - bar + i, mid - d / 2 + j, c, 255);
                    }
                }
                x += gpunct_cell(kind, '+');
            } else {
                w = d + 2;
                gap = h / 3 > 2 ? h / 3 : 2;
                for (j = 0; j < d; j++) {
                    for (i = 0; i < d; i++) {
                        if (*s == '.') {
                            plot_c(x + 1 + i, y + top + h - d + j, c, 255);
                        } else {
                            plot_c(x + 1 + i, mid - gap / 2 - d + j, c, 255);
                            plot_c(x + 1 + i, mid + (gap + 1) / 2 + j, c, 255);
                        }
                    }
                }
                if (sPtN < 8) {
                    sPtX0[sPtN] = x + 1;
                    sPtX1[sPtN++] = x + 1 + d;
                }
                x += w;
            }
        } else if (*s == '+' && s[1] >= '0' && s[1] <= '9') {
            /* a plus sign from the font, hugging the first digit's ink like the minus */
            Ink kp;
            int nx, gapS = gmark_dot(kind), top, h;
            gband(kind, 1, &top, &h);
            MEASURE(gdraw_raw(kind, 0, 0, one, c), kp);
            /* the next digit's ink left on the rows of the plus's arm */
            nx = gdigit_ink_left(kind, x + gpunct_cell(kind, '+'), cell, s[1], top + h / 2 - 1, top + h / 2 + 1);
            gdraw_raw(kind, nx - gapS - (kp.x1 - kp.x0) - kp.x0, y, one, c);
            x += gpunct_cell(kind, '+');
        } else {
            const UiFont *u = gfont(kind);
            int a;
            if (u != NULL) {
                a = ui_advance(u, (unsigned char) *s);
            } else {
                a = text_width_as_is(kind == GF_HEAD || kind == GF_LG || kind == GF_XL ? FONT_FUN : FONT_SMALL, one);
            }
            gdraw_raw(kind, x, y, one, c);
            x += a;
        }
    }
    return x;
}

/* The band (top offset from the draw y, height) of the font's capitals, or of its digits. */
static void gband(int kind, int digits, int *top, int *h) {
    Ink k;
    MEASURE(gdraw_raw(kind, 0, 0, digits ? "0" : "H", WHITE), k);
    *top = k.y0;
    *h = k.y1 - k.y0;
}

enum { GA_LEFT, GA_CENTRE, GA_RIGHT };

/*
 * Text placed in the box x0..x1, y0..y1: its capitals' band centred top to bottom, its ink at the left edge, centred,
 * or at the right edge. tab: digits tabular (numbers). Returns the ink's left and right in *inkL, *inkR (may be NULL).
 */
/*
 * A whole-pixel label (Classic and the drawn sets) whose room leaves an odd column: where that column goes, every letter
 * drawn as it is everywhere else (not one of them a column wider: see ui_widen_on). Tried in turn: a word space a
 * pixel wider, a button's icon gap (iconW) a pixel wider, every gap between the letters a pixel wider (spread; not
 * Classic's buttons: a row of them would be spaced unalike, so those sit a pixel left). (Not Classic's
 * drop shadow: it joins the outline, so the dark ink would sit a pixel off the middle.)
 * 1: found (*ink the ink to centre; *track, *space and *iconGap to draw with), 0: none.
 */
static int ui_whole_fit(int kind, const char *s, Colour c, int room, int iconW, int iconGap0, int track0, Ink *ink, int *track,
                        int *space, int *iconGap, int spread) {
    Ink kq;
#define FITS_(k) (!((room - ((k).x1 - (k).x0) - (iconW ? iconW + *iconGap : 0)) & 1))
    *track = track0, *space = 0, *iconGap = iconGap0;
    sUiTrack = track0;
    sUiSpaceExtra = 0;
    MEASURE(gdraw_raw(kind, 0, 0, s, c), kq);
    if (strchr(s, ' ') != NULL) {
        Ink kw;
        sUiSpaceExtra = 64;
        MEASURE(gdraw_raw(kind, 0, 0, s, c), kw);
        sUiSpaceExtra = 0;
        if (FITS_(kw)) {
            *space = 64, *ink = kw;
            return 1;
        }
    }
    if (iconW) {
        *iconGap = iconGap0 + 1;
        if (FITS_(kq)) {
            *ink = kq;
            return 1;
        }
        *iconGap = iconGap0;
    }
    if (!spread) {
        return 0;
    }
    sUiTrack = track0 + 64;
    MEASURE(gdraw_raw(kind, 0, 0, s, c), kq);
    sUiTrack = track0;
    if (FITS_(kq)) {
        *track = track0 + 64, *ink = kq;
        return 1;
    }
    return 0;
#undef FITS_
}
static int sFaceY0, sFaceY1; /* gtext_ex: the face's rows (top to baseline) at y 0, for the log */

/*
 * The last box that holds one label (a tab, a chip, a pill, a button): text set inside it is centred by its own ink,
 * top (capitals, ascenders, digits) to baseline, as the eye measures the gaps above and below it. Text in rows and
 * panels keeps the font's capitals band, so a row's label and value share one baseline.
 */
static int sInkBox[4] = { 0, 0, -1, -1 };
static void gink_box(int x0, int y0, int x1, int y1) {
    sInkBox[0] = x0, sInkBox[1] = y0, sInkBox[2] = x1, sInkBox[3] = y1;
}
static int gink_box_has(int x0, int y0, int y1) {
    return x0 >= sInkBox[0] && x0 < sInkBox[2] && y0 >= sInkBox[1] && y1 <= sInkBox[3];
}
#ifndef __3DS__
/* host checks only: DKR_BOTTOM_NOINK=1 leaves out text and pictures, so a check can tell ink from background */
static int gnoink(void) {
    static int v = -1;
    if (v < 0) {
        v = getenv("DKR_BOTTOM_NOINK") != NULL;
    }
    return v;
}
#else
#define gnoink() 0
#endif
static int sPenTrack; /* gtext_ex: the tracking to restore (+1), when a label's letters were spread */
static int sPenLeft; /* gtext_ex: a left-aligned label's pen phase (+1), its first column solid */
static int sOptical; /* set: one-line labels with descenders are lifted (the buttons) */

static const char *glabel(const char *s, char *buf, int size);
static int gbox_baseline(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = plat_getenv("DKR_UI_BASELINE");
        on = e == NULL || atoi(e) != 0;
    }
    return on;
}
static void gtext_ex(int kind, int x0, int y0, int x1, int y1, const char *s, Colour c, int align, int tab, int *inkL,
                     int *inkR) {
    int top, h, y, x, inkX0 = 0, inkX1 = 0, crisp = sUiCrispAll;
    Ink k;
    if (s == NULL || s[0] == '\0') {
        return;
    }
    if (gink_box_has(x0, y0, y1) && gfont(kind) != NULL && gfont(kind)->outlined) {
        sUiCrispAll = 1; /* Classic in a box: every letter solid, as its figures, so the face sits in the middle of its outline */
    }
    if (tab && gsprite_nums(kind)) {
        int cell;
        gsprite_metrics(&cell, &top, &h); /* the timer digits' own band */
    } else {
        gband(kind, tab && s[0] >= '0' && s[0] <= '9', &top, &h);
    }
    y = y0 + (y1 - y0 - h) / 2 - top;
    {
        /*
         * Centred by the ink as the eye sees it, not the font's band: its top (capitals, ascenders or digits; the
         * lowercase's when that is all there is) to its baseline (descenders hang below). An outlined font's face
         * alone: its dark outline and shadow vanish on a dark box and would leave the face high.
         */
        Ink f;
        const UiFont *uf = gfont(kind);
        int flat = sUiFlat, fy0, fy1;
        sUiFlat = uf != NULL && uf->outlined ? 1 : flat;
        if (gink_box_has(x0, y0, y1)) {
            sUiFlat = flat; /* in a box: all the ink the eye sees, outline, shadow and descenders included */
        }
        MEASURE(tab ? gdraw_tab(kind, 0, 0, s, c) : gdraw_raw(kind, 0, 0, s, c), f);
        if (gink_box_has(x0, y0, y1) && !tab && gbox_baseline() && f.y1 > f.y0) {
            /*
             * in a box: centred on a capital's ink (H, outline and shadow included), so every label in boxes of one
             * height shares one baseline whatever its letters: Settings' g hangs below, its round S overshoots above,
             * and neither moves it off Pause's line. DKR_UI_BASELINE=0 in env.txt: centred on all its ink as before.
             */
            Ink hk;
            MEASURE(gdraw_raw(kind, 0, 0, "H", c), hk);
            if (hk.y1 > hk.y0) {
                f.y0 = hk.y0, f.y1 = hk.y1;
            }
        }
        sUiFlat = flat;
        if (f.y1 > f.y0 && !(tab && gsprite_nums(kind))) {
            fy0 = f.y0;
            fy1 = gink_box_has(x0, y0, y1) || f.y1 < top + h ? f.y1 : top + h;
            if (fy1 > fy0) {
                if (gink_box_has(x0, y0, y1)) {
                    y = y0 + (y1 - y0 - (fy1 - fy0)) / 2 - fy0;
                } else {
                    fy0 = top, fy1 = top + h; /* (a row's lines share the band: the audit measures what was centred) */
                }
                sFaceY0 = fy0, sFaceY1 = fy1;
            }
        }
    }
    if (tab) {
        int adv = 0;
        MEASURE(adv = gdraw_tab(kind, 0, 0, s, c), k);
        inkX0 = k.x0;
        inkX1 = k.x1;
        /*
         * Numbers align on their cells, not their ink: a trailing "1" is narrower than its cell, and aligning its ink
         * would shift the whole number (and its point) by a pixel against the row above.
         */
        k.x0 = 0;
        k.x1 = adv;
    } else {
        MEASURE(gdraw_raw(kind, 0, 0, s, c), k);
        if (align == GA_LEFT && !gink_box_has(x0, y0, y1) && gfont(kind) != NULL && gfont(kind)->phases > 1) {
            /*
             * left-aligned labels in a column start on one exact column, faint edge and solid ink alike: a round first
             * letter (C, S) drawn at the whole pixel begins with a half-covered column, its solid ink a pixel after
             * a straight one's. The pen is moved a quarter pixel over until the first column is solid (a label in a
             * box is placed by its box's own centring instead)
             */
            int q;
            for (q = 0; q < 64; q += 16) {
                Ink kq;
                sPen0 = q;
                sSolidX0 = 99999, sSolidX1 = -99999;
                MEASURE(gdraw_raw(kind, 0, 0, s, c), kq);
                if (sSolidX0 == kq.x0) {
                    k = kq;
                    sPenLeft = q + 1; /* drawn so, then reset */
                    break;
                }
            }
            sPen0 = 0;
        }
        inkX0 = k.x0;
        inkX1 = k.x1;
    }
    if (sOptical && k.y1 > top + h + 1) {
        /* a label with descenders (Settings, Prev) reads low when its capitals are centred: lift it by a quarter of them */
        y -= (k.y1 - (top + h) + 2) / 4;
    }
    x = align == GA_LEFT ? x0 - k.x0 : align == GA_CENTRE ? x0 + (x1 - x0 - (k.x1 - k.x0)) / 2 - k.x0 : x1 - k.x1;
    if (align == GA_LEFT && !tab && gfont(kind) != NULL && gfont(kind)->outlined && !gink_box_has(x0, y0, y1)) {
        /* Classic: left-aligned labels line up on their faces. A label drawn as its face alone (the lit row's)
           starts where the others' faces do, a pixel inside where their outline starts */
        Ink kf;
        int flat = sUiFlat;
        sUiFlat = 1;
        MEASURE(gdraw_raw(kind, 0, 0, s, c), kf);
        sUiFlat = flat;
        if (kf.x1 > kf.x0) {
            x = x0 + 1 - kf.x0;
        }
    }
    if (align == GA_CENTRE && !tab && gink_box_has(x0, y0, y1) && gfont(kind) != NULL) {
        /*
         * a label alone in its box: the same gap left and right of its ink, to the pixel, for its faint edge and its
         * solid ink alike. When the room left over is odd (or the solid ink sits off the faint ink's middle), its
         * letters are spread or drawn together by a fraction of a pixel (unseen), the pen moved a quarter pixel
         * over, until both are exactly in the middle (a lone capital: drawn in its variant a column wider if need be)
         */
        int t, sg, q, wd, fw = 0, base = sUiTrack, done = 0, chipSpace = 0, alone = s[0] >= 'A' && s[0] <= 'Z' && s[1] == '\0', fq = 0, ft = base;
        Ink fk = k;
        /* the least spread first: every pen phase at each tracking before a wider one; a lone capital: its own
           glyph, then its variant a column wider (then, drawn by hand, its narrow form) */
        /* a lone capital drawn by hand for the key caps (both forms there): only those, so every cap letter of a look
           is drawn alike */
        int handDrawn = alone && gfont(kind)->glyphs[(gfont(kind)->phases + 1) * UI_NG + s[0] - 0x21].width > 0;
        if (gfont(kind)->phases == 1 && !alone && ((x1 - x0 - (k.x1 - k.x0)) & 1) && !ui_widen_on()) {
            /* whole pixels: the odd column taken up where it isn't seen (ui_whole_fit), else the label a pixel left */
            int sp = 0, ig = 0;
            if (ui_whole_fit(kind, s, c, x1 - x0, 0, 0, base, &fk, &ft, &sp, &ig, 1)) {
                chipSpace = sp;
                done = 2;
            } else {
                fk = k, ft = base;
                done = 1;
            }
            fq = 0, fw = 0;
        } else if (gfont(kind)->phases == 1 && !alone && ((x1 - x0 - (k.x1 - k.x0)) & 1)) {
            /* Classic: one letter a column wider (see sUiWiden), not its letters spread */
            Ink kq;
            sUiWiden = 1;
            MEASURE(gdraw_raw(kind, 0, 0, s, c), kq);
            if (!((x1 - x0 - (kq.x1 - kq.x0)) & 1)) {
                fk = kq, fq = 0, ft = base, fw = 0;
                done = 2;
            } else {
                sUiWiden = 0;
            }
        }
        for (wd = handDrawn ? 1 : 0; wd <= (alone ? 2 : 0) && done < 2 && !(gfont(kind)->phases == 1 && !alone && !ui_widen_on()); wd++) {
            for (t = 0; t <= (alone ? 0 : 64) && done < 2; t += 2) {
                for (sg = 1; sg >= -1 && done < 2; sg -= 2) {
                    for (q = 0; q < 64 && done < 2; q += 16) {
                        Ink kq;
                        if (t == 0 && sg < 0) {
                            continue;
                        }
                        sUiTrack = base + sg * t;
                        sPen0 = q;
                        sUiWide = wd;
                        sSolidX0 = 99999, sSolidX1 = -99999;
                        MEASURE(gdraw_raw(kind, 0, 0, s, c), kq);
                        if (!((x1 - x0 - (kq.x1 - kq.x0)) & 1)) {
                            int solid = sSolidX1 < sSolidX0 || sSolidX0 - kq.x0 == kq.x1 - (sSolidX1 + 1);
                            if (done == 0 || solid) {
                                fk = kq, fq = q, ft = sUiTrack, fw = wd;
                                done = solid ? 2 : 1;
                            }
                        }
                    }
                }
            }
        }
        sUiWide = done ? fw : 0;
        if (sUiWide) {
            log_rec('W', x0, y0, x1, y1, sUiWide, s); /* (the letter drawn in a variant: 1 wider, 2 re-centred) */
        }
        sUiTrack = done ? ft : base;
        sPen0 = done ? fq : 0;
        if (done) {
            k = fk;
            inkX0 = k.x0;
            inkX1 = k.x1;
            x = x0 + (x1 - x0 - (k.x1 - k.x0)) / 2 - k.x0;
        }
        if (done == 0 || (done == 1 && gfont(kind)->phases == 1 && !alone && ((x1 - x0 - (k.x1 - k.x0)) & 1))) {
            /* (a lone "B": its solid stem and half-covered bowl can't both be centred; its ink is) */
            log_rec('E', x0, y0, x1, y1, done, "uneven-label");
        }
        sUiSpaceExtra = chipSpace;
        sPenTrack = base + 1; /* restored after drawing */
    }
    if (sPenLeft) {
        sPen0 = sPenLeft - 1;
        sPenLeft = 0;
    }
    if (!gnoink()) {
        QUIET(tab ? gdraw_tab(kind, x, y, s, c) : gdraw_raw(kind, x, y, s, c));
    }
    if (sPenTrack) {
        sUiTrack = sPenTrack - 1;
        sPenTrack = 0;
    }
    sUiSpaceExtra = 0;
    sPen0 = 0;
    sUiWide = 0;
    sUiWiden = 0;
    sUiCrispAll = crisp;
    log_rec('C', x + inkX0, y + top, x + inkX1, y + top + h, kind, s); /* the ink, as the eye sees the text's place */
    if (sFaceY1 > sFaceY0) {
        log_rec('F', x + inkX0, y + sFaceY0, x + inkX1, y + sFaceY1, kind, s); /* its face, top to baseline: the audit's gaps */
    }
    sFaceY0 = sFaceY1 = 0;
    if (tab) {
        int i;
        for (i = 0; i < sPtN; i++) {
            log_rec('D', sPtX0[i], y + top, sPtX1[i], y + top + h, kind, s); /* a point's column, for the audit */
        }
    }
    if (inkL != NULL) {
        *inkL = x + k.x0;
    }
    if (inkR != NULL) {
        *inkR = x + k.x1;
    }
}

static void gtext(int kind, int x0, int y0, int x1, int y1, const char *s, Colour c, int align) {
    gtext_ex(kind, x0, y0, x1, y1, port_text(s), c, align, 0, NULL, NULL);
}

static void gnum(int kind, int x0, int y0, int x1, int y1, const char *s, Colour c, int align) {
    gtext_ex(kind, x0, y0, x1, y1, s, c, align, 1, NULL, NULL);
}

/*
 * The button bar's bottom: 24 px, or 25 when the label font's capitals would leave an odd number of rows inside the
 * 1 px edges (9 px capitals in 22 rows sit half a pixel high; in 23 they sit exactly in the middle).
 */
static int gbtn_y1(void) {
    int t, h;
    gband(sStyle == 3 ? GF_BODY : GF_HEAD, 0, &t, &h);
    return GBTN_Y1 + ((GBTN_Y1 - GBTN_Y0 - 2 - h) & 1);
}

static int gtext_width(int kind, const char *s, int tab) {
    Ink k;
    if (tab) {
        int adv = 0;
        MEASURE(adv = gdraw_tab(kind, 0, 0, s, WHITE), k);
        return adv; /* numbers take their cells (see gtext_ex) */
    }
    MEASURE(gdraw_raw(kind, 0, 0, port_text(s), WHITE), k);
    return k.x1 > k.x0 ? k.x1 - k.x0 : 0;
}

/* A picture scaled to fit w x h (keeping its shape), its ink centred in the box x0..x1, y0..y1. */
static int sPicX, sPicY; /* gpic_fit: where it drew the last picture's top left corner */
static void gpic_fit(const Picture *p, int x0, int y0, int x1, int y1, int maxW, int maxH, int dim) {
    Ink k;
    int w, h, x, y;
    if (p == NULL || p->rgba == NULL) {
        return;
    }
    w = maxW;
    h = p->height * maxW / (p->width > 0 ? p->width : 1);
    if (h > maxH) {
        h = maxH;
        w = p->width * maxH / (p->height > 0 ? p->height : 1);
    }
    if (p == &sLogo || p == &sLogoClean || p == &sSmallLogo) {
        /* the logo: the vector's own rendering at this size, 1:1, never a resize of another size */
        /* the largest rendering that fits the box (the logo bitmaps' own shapes differ from the vector's by a pixel,
           so matching their scaled size could miss and fall back to a muddy resize) */
        int i;
        for (i = 0; i < 4; i++) {
            if (sLogoVector[i].rgba != NULL && sLogoVector[i].width <= maxW && sLogoVector[i].height <= maxH) {
                p = &sLogoVector[i];
                w = p->width;
                h = p->height;
                break;
            }
        }
    }
    if (w == p->width && h == p->height) {
        MEASURE(dim ? picture_dim(p, 0, 0) : picture(p, 0, 0), k);
    } else {
        MEASURE(picture_fit(p, 0, 0, w, h), k);
    }
    x = x0 + (x1 - x0 - (k.x1 - k.x0)) / 2 - k.x0;
    y = y0 + (y1 - y0 - (k.y1 - k.y0)) / 2 - k.y0;
    sPicX = x, sPicY = y;
    if (gnoink()) {
    } else if (w == p->width && h == p->height) {
        QUIET(dim ? picture_dim(p, x, y) : picture(p, x, y));
    } else {
        QUIET(picture_fit(p, x, y, w, h));
    }
    log_rec('P', x + k.x0, y + k.y0, x + k.x1, y + k.y1, 0, "pic");
}

/*
 * A picture mirror symmetric top to bottom (a stepper's arrow), drawn at (x, y): its lower half's pixels made its
 * upper half's as they came out on the screen, upside down. A chip shaded top to bottom, or lit by the focused row,
 * shows through the arrow's translucent edge a shade differently on each row; copied after blending, the two halves
 * match pixel for pixel.
 */
static void gmirror_rows(const Picture *p, int x, int y) {
    int n = p->height / 2, mx, my;
    if (sDryInk || gnoink() || p->rgba == NULL) {
        return;
    }
    for (my = 0; my < n; my++) {
        for (mx = 0; mx < p->width; mx++) {
            int sx = x + mx, sy = y + my, dy = y + p->height - 1 - my;
            if (p->rgba[(my * p->width + mx) * 4 + 3] == 0 || sx < 0 || sx >= BS_WIDTH || sy < sClipTop || sy < 0 ||
                dy >= sClipEnd || dy >= BS_HEIGHT) {
                continue;
            }
            sImage[dy * BS_WIDTH + sx] = sImage[sy * BS_WIDTH + sx];
        }
    }
}

/* A picture at its own size, its ink's left at x and centred top to bottom in y0..y1; returns its ink's right. */
static int gpic_left(const Picture *p, int x, int y0, int y1, int dim) {
    Ink k;
    int px, py;
    if (p == NULL || p->rgba == NULL) {
        return x;
    }
    MEASURE(dim ? picture_dim(p, 0, 0) : picture(p, 0, 0), k);
    px = x - k.x0;
    py = y0 + (y1 - y0 - (k.y1 - k.y0)) / 2 - k.y0;
    QUIET(dim ? picture_dim(p, px, py) : picture(p, px, py));
    log_rec('P', px + k.x0, py + k.y0, px + k.x1, py + k.y1, 0, "pic");
    return px + k.x1;
}

/* ---- The looks' skins: what a panel, row, lit row, chip, card and tab look like, and the inks ---- */
static int gfamily(void) {
    return sStyle; /* 0 Classic, 1 Glass (Sunset, Daylight), 2 Broadcast (Carbon), 3 Dock (Jungle) */
}
static int gcarbon(void) {
    return sStyle == 2 && sVariant == 1;
}
static int gbroadcast(void) {
    return sStyle == 2 && sVariant != 1;
}
static int gslant(void) {
    return 0; /* Broadcast's slabs are rectangles on the grid too */
}

static Colour gink_label(void) {
    if (gcarbon()) {
        return (Colour){ 150, 162, 188, 255 };
    }
    return LABEL_BLUE;
}
static Colour gink_dim(void) {
    if (gcarbon()) {
        return (Colour){ 138, 144, 166, 255 };
    }
    return DIM;
}
static Colour gink_accent(void) {
    return GOLD; /* every look's accent (Broadcast's was white, so its current lap and titles had no mark) */
}
static Colour gink_lit(void) {
    if (sStyle == 1) {
        return (Colour){ 70, 30, 0, 255 }; /* dark ink on the gold row */
    }
    return WHITE;
}
/* Ink for headings drawn straight on the background (not on a panel). */
static Colour gink_onbg(void) {
    return sVariant == 4 ? (Colour){ 11, 32, 80, 255 } : gink_accent();
}

static void gfill_edge(int x0, int y0, int x1, int y1, Colour fillc, Colour edge) {
    fill(x0, y0, x1, y1, fillc);
    if (edge.a > 0) {
        fill(x0, y0, x1, y0 + 1, edge);
        fill(x0, y1 - 1, x1, y1, edge);
        fill(x0, y0 + 1, x0 + 1, y1 - 1, edge);
        fill(x1 - 1, y0 + 1, x1, y1 - 1, edge);
    }
}

static const char *const sSkinNames[] = { "panel", "row", "lit", "chip", "card", "tab", "tablit", "cur" };

static void gbox(int x0, int y0, int x1, int y1, int kind) {
    Colour pt, pb, pe;
    if (kind == SK_CHIP || kind == SK_TAB || kind == SK_TAB_LIT) {
        gink_box(x0, y0, x1, y1);
    } else {
        gink_box(0, 0, -1, -1);
    }
    if (kind == SK_TAB_LIT || kind == SK_CUR) {
        /*
         * the selected tab and the current item (the lap being driven): their own box lit with an accent fill, no
         * frame (in every look, Dock's tabs too), so the screen never shows two yellow frames: the focused row keeps
         * the only frame (or the solid fill, in Glass, Sunset, Daylight and Broadcast)
         */
        int q = sQuiet;
        Colour a = gink_accent();
        gbox(x0, y0, x1, y1, kind == SK_TAB_LIT ? SK_TAB : SK_ROW);
        sQuiet = 1;
        fill(x0 + 1, y0 + 1, x1 - 1, y1 - 1, (Colour){ a.r, a.g, a.b, 84 });
        sQuiet = q;
        log_rec('B', x0, y0, x1, y1, kind, sSkinNames[kind]);
        return;
    }
    int h = y1 - y0;
    int q = sQuiet;
    sQuiet = 1;
    switch (sStyle) {
        case 0: /* Classic: the game's dialogue boxes */
            if (kind == SK_LIT || kind == SK_TAB_LIT) {
                /* a bright blue under the gold edge: the timer digits are yellow, and gold under them lost them */
                box_classic(x0, y0, x1, y1, (Colour){ 40, 100, 230, 255 }, PICKED_EDGE); /* (opaque: the checkerboard showed through as a faint square behind a portrait) */
            } else if (kind == SK_CHIP) {
                box_classic(x0, y0, x1, y1, (Colour){ 0, 0, 20, 110 }, (Colour){ 140, 180, 255, 110 });
            } else if (kind == SK_ROW || kind == SK_TAB) {
                box_classic(x0, y0, x1, y1, (Colour){ 0, 8, 40, 170 }, (Colour){ 140, 180, 255, 80 });
            } else {
                /* nearly opaque: the checkerboard showed through as squares behind pictures (the egg) and text */
                box_classic(x0, y0, x1, y1, (Colour){ 0, 8, 40, 215 }, BOX_EDGE);
            }
            break;
        case 1: /* Glass: frosted, one corner radius everywhere */
            palette_panel(0, &pt, &pb, &pe);
            if (kind == SK_LIT || kind == SK_TAB_LIT) {
                smooth_box(x0, y0, x1, y1, 6, (Colour){ 255, 226, 96, 240 }, (Colour){ 236, 156, 24, 240 },
                           (Colour){ 255, 250, 200, 200 }, 1);
            } else if (kind == SK_CHIP) {
                smooth_box(x0, y0, x1, y1, 5, (Colour){ 0, 0, 20, 110 }, (Colour){ 0, 0, 20, 110 },
                           (Colour){ 255, 255, 255, 50 }, 1);
            } else {
                smooth_box(x0, y0, x1, y1, 6, pt, pb, pe, 1);
            }
            break;
        case 2:
            if (gcarbon()) {
                if (kind == SK_LIT || kind == SK_TAB_LIT) {
                    gfill_edge(x0, y0, x1, y1, (Colour){ 0, 112, 122, 255 }, (Colour){ 120, 240, 240, 255 });
                } else if (kind == SK_CHIP) {
                    gfill_edge(x0, y0, x1, y1, (Colour){ 12, 14, 18, 255 }, (Colour){ 0, 0, 0, 0 });
                } else {
                    gfill_edge(x0, y0, x1, y1, (Colour){ 28, 31, 38, 245 }, (Colour){ 74, 82, 98, 255 });
                }
            } else {
                /* Broadcast: square slabs on the grid; the lit one in the logo's red to yellow */
                int sl = h > 30 ? 8 : 5;
                if (kind == SK_TAB_LIT) {
                    slab(x0, y0, x1, y1, sl, (Colour){ 235, 40, 40, 255 }, (Colour){ 255, 150, 20, 255 });
                } else if (kind == SK_LIT) {
                    /* the lit slab: a brighter blue, marked by the logo's red to yellow stripe on its left edge */
                    slab(x0, y0, x1, y1, sl, (Colour){ 60, 92, 206, 250 }, (Colour){ 44, 68, 168, 250 });
                    slab(x0, y0, x0 + 4, y1, sl, (Colour){ 255, 196, 30, 255 }, (Colour){ 235, 40, 40, 255 });
                } else if (kind == SK_CHIP) {
                    slab(x0, y0, x1, y1, 4, (Colour){ 0, 0, 20, 140 }, (Colour){ 0, 0, 20, 140 });
                } else if (kind == SK_CARD) {
                    slab(x0, y0, x1, y1, sl, (Colour){ 36, 54, 130, 235 }, (Colour){ 36, 54, 130, 235 });
                } else {
                    slab(x0, y0, x1, y1, sl, (Colour){ 26, 38, 96, 235 }, (Colour){ 26, 38, 96, 235 });
                }
            }
            break;
        default: /* Dock: panels are a whisper, rows have none, the lit one a gold ring */
            palette_panel(1, &pt, &pb, &pe);
            if (kind == SK_LIT || kind == SK_TAB_LIT) {
                smooth_box(x0, y0, x1, y1, 8, (Colour){ 255, 255, 255, 40 }, (Colour){ 255, 255, 255, 40 },
                           (Colour){ 255, 214, 40, 255 }, 1);
            } else if (kind == SK_CHIP) {
                smooth_box(x0, y0, x1, y1, 6, (Colour){ 0, 0, 0, 70 }, (Colour){ 0, 0, 0, 70 }, (Colour){ 0, 0, 0, 0 }, 1);
            } else if (kind == SK_ROW || kind == SK_TAB) {
                smooth_box(x0, y0, x1, y1, 8, (Colour){ 255, 255, 255, 14 }, (Colour){ 255, 255, 255, 14 },
                           (Colour){ 0, 0, 0, 0 }, 1);
            } else {
                /* flat, with a faint rim: the old fade to nothing lost every panel's bottom edge */
                (void) pb;
                smooth_box(x0, y0, x1, y1, 8, pt, pt, (Colour){ 255, 255, 255, 36 }, 1);
            }
            break;
    }
    sQuiet = q;
    log_rec('B', x0, y0, x1, y1, kind, sSkinNames[kind]);
}

/* How far text must stay from a box's left and right edges: the padding, plus the slant of Broadcast's slabs. */
static int gpad_x(int kind) {
    (void) kind;
    return GPAD + gslant();
}

/* ---- Icons: drawn smooth in a 12 px square, ink centred in the given box ---- */
/*
 * The button icons as exact pixel grids ('#' ink, '.' clear, so the button shows through the holes). Every one mirrors
 * left to right pixel for pixel (but the chevrons), and the pause bars, the gear, the d-pad, the X and the chevrons
 * also mirror top to bottom.
 */
typedef struct {
    const char *name;
    int w, h;
    const char *rows[13];
} GIcon;

static const GIcon sGIcons[] = {
    /* (every icon an even number of rows, as the type's capitals: so it sits exactly in the middle of its button) */
    { "Pause", 8, 10, { "###..###", "###..###", "###..###", "###..###", "###..###", "###..###", "###..###", "###..###",
                        "###..###", "###..###" } },
    { "Settings", 12, 12, { ".....##.....", ".##..##..##.", ".##########.", "..########..", "..###..###..",
                            "####....####", "####....####", "..###..###..", "..########..", ".##########.",
                            ".##..##..##.", ".....##....." } },
    /* the D-pad: 10 px so CONTROLS and its icon keep 8 px clear in a three-column button */
    { "Controls", 10, 10, { "...####...", "...####...", "...####...", "##########", "####..####", "####..####",
                            "##########", "...####...", "...####...", "...####..." } },
    { "Updates", 10, 12, { "....##....", "....##....", "....##....", "....##....", ".########.", "..######..",
                           "...####...", "....##....", "..........", "..........", "##########", "##########" } },
    { "Close", 10, 10, { "##......##", "###....###", ".###..###.", "..######..", "...####...", "...####...",
                         "..######..", ".###..###.", "###....###", "##......##" } },
    /* a stop square, the size of the D-pad */
    { "Stop test", 10, 10, { "##########", "##########", "##########", "##########", "##########", "##########",
                             "##########", "##########", "##########", "##########" } },
    { "Cancel", 10, 10, { "##......##", "###....###", ".###..###.", "..######..", "...####...", "...####...",
                          "..######..", ".###..###.", "###....###", "##......##" } },
    { "Next", 6, 10, { "##....", "###...", ".###..", "..###.", "...###", "...###", "..###.", ".###..", "###...", "##...." } },
    { "Prev", 6, 10, { "....##", "...###", "..###.", ".###..", "###...", "###...", ".###..", "..###.", "...###", "....##" } },
};

static const GIcon *gicon_find(const char *label) {
    unsigned i;
    for (i = 0; i < sizeof(sGIcons) / sizeof(sGIcons[0]); i++) {
        if (strcasecmp(label, sGIcons[i].name) == 0) {
            return &sGIcons[i];
        }
    }
    return NULL;
}

#define GICON_GAP 4
/* Classic's button labels: 2 px more between letters, so each letter keeps its own outline (one shared outline fused
   "CLOSE" into one dark mass) */
#define GBTN_TRACK (2 * 64)
static void gicon_raw(const char *label, int cx, int cy, Colour c, Colour hole) {
    const GIcon *g = gicon_find(label);
    int x, y, x0, y0;
    (void) hole;
    if (g == NULL || (gnoink() && !sDryInk)) { /* (host checks: no icons either) */
        return;
    }
    x0 = cx - g->w / 2;
    y0 = cy - g->h / 2;
    if (sStyle == 0 && c.r + c.g + c.b >= 3 * 110 && !sUiFlat) {
        /* Classic: the icon outlined and shadowed as its label's letters are (drop shadow, then 1 px of dark round it) */
        int pass, dx, dy;
        for (pass = 0; pass < 2; pass++) {
            for (y = 0; y < g->h; y++) {
                for (x = 0; x < g->w; x++) {
                    if (g->rows[y][x] != '#') {
                        continue;
                    }
                    for (dy = pass ? -1 : 1; dy <= (pass ? 1 : 3); dy++) { /* (the shadow 2 px down, as the letters') */
                        for (dx = pass ? -1 : 0; dx <= (pass ? 1 : 2); dx++) {
                            if (pass) {
                                plot(x0 + x + dx, y0 + y + dy, 16, 18, 34, 255);
                            } else {
                                plot(x0 + x + dx, y0 + y + dy, 8, 10, 24, 255);
                            }
                        }
                    }
                }
            }
        }
    }
    for (y = 0; y < g->h; y++) {
        for (x = 0; x < g->w; x++) {
            if (g->rows[y][x] == '#') {
                plot_c(x0 + x, y0 + y, c, 255);
            }
        }
    }
}
static int gicon_known(const char *label) {
    static const char *const sKnown[] = { "Pause", "Settings", "Controls", "Updates", "Close", "Prev", "Next", "Stop test", "Cancel" };
    unsigned i;
    for (i = 0; i < sizeof(sKnown) / sizeof(sKnown[0]); i++) {
        if (strcasecmp(label, sKnown[i]) == 0) { /* "cancel" is the confirm bar's own spelling */
            return 1;
        }
    }
    return 0;
}

/* Does every known button label plus its icon fit a three-column bar button with 8 px clear each side? */
static int gicons_fit(void) {
    static const char *const sAll[] = { "Pause", "Settings", "Controls", "Updates", "Close", "Prev", "Next" };
    int w = (GX1 - GX0 - GGAP * 2) / 3, worst = 999;
    unsigned i;
    for (i = 0; i < sizeof(sAll) / sizeof(sAll[0]); i++) {
        char up[40];
        Ink k;
        int need;
        if (sStyle == 3) {
            snprintf(up, sizeof(up), "%s", port_text(sAll[i]));
        } else {
            upper_case(port_text(sAll[i]), up, sizeof(up));
        }
        MEASURE(gicon_raw(sAll[i], 0, 0, WHITE, WHITE), k);
        need = gtext_width(sStyle == 3 ? GF_BODY : GF_HEAD, up, 0) + (k.x1 - k.x0) + GICON_GAP;
        if (getenv("DKR_ICON_DEBUG")) {
            fprintf(stderr, "  %s text %d icon %d pad %d\n", up, gtext_width(sStyle == 3 ? GF_BODY : GF_HEAD, up, 0), k.x1 - k.x0, (w - need) / 2);
        }
        if ((w - need) / 2 < worst) {
            worst = (w - need) / 2;
        }
    }
    if (getenv("DKR_ICON_DEBUG")) {
        fprintf(stderr, "style %d var %d: worst side pad %d\n", sStyle, sVariant, worst);
    }
    /* (Classic keeps its plain labels, as with its earlier lettering: its hand-drawn capitals are narrower and would
       otherwise just make room) */
    return sStyle != 0 && worst >= 8;
}

/* Classic's letter tracking on buttons: GBTN_TRACK where every label and its icon still keep 8 px clear of a
   three-column button, else the most that does (in whole pixels), decided once per look so a label never changes
   its spacing between screens or states */
static int gbtn_track(void) {
    static const char *const sAll[] = { "Pause", "Settings", "Controls", "Updates", "Close", "Prev", "Next" };
    int w = (GX1 - GX0 - GGAP * 2) / 3, trk;
    unsigned i;
    if (sStyle != 0) {
        return 0;
    }
    for (trk = GBTN_TRACK; trk > 0; trk -= 64) { /* whole pixels: every gap between letters the same */
        int worst = 999;
        for (i = 0; i < sizeof(sAll) / sizeof(sAll[0]); i++) {
            char up[40];
            Ink k;
            int need;
            upper_case(port_text(sAll[i]), up, sizeof(up));
            MEASURE(gicon_raw(sAll[i], 0, 0, WHITE, WHITE), k);
            sUiTrack = trk;
            need = gtext_width(GF_HEAD, up, 0) + (k.x1 - k.x0) + GICON_GAP;
            sUiTrack = 0;
            if ((w - need) / 2 < worst) {
                worst = (w - need) / 2;
            }
        }
        if (worst >= 8) {
            break;
        }
    }
    return trk;
}

/* ---- The button bar: equal buttons on the 2- or 3-column grid, icon and label centred as one group ---- */
/* Where each bar button was last drawn, for the touch screen (bs_grid_button_hit): -1 where none is shown */
static volatile short sGBtnX0[8], sGBtnX1[8];
static volatile int sGBtnCount;

static void gbuttons(const char (*buttons)[40], int count) {
    int i, shownCount = 0, slot = 0;
    for (i = 0; i < 8; i++) {
        sGBtnX0[i] = sGBtnX1[i] = -1;
    }
    sGBtnCount = count < 8 ? count : 8;
    /* a lone "-" only keeps two buttons apart: the buttons shown share the bar equally instead */
    int noIcons = 0;
    for (i = 0; i < count; i++) {
        shownCount += !(buttons[i][0] == '-' && buttons[i][1] == '\0');
    }
    /* icons on every button of every bar in this look, or on none: decided once per look from the widest bar
       (three buttons), so the same button never gains or loses its icon between screens */
    noIcons = !gicons_fit();
    for (i = 0; i < count && !noIcons; i++) {
        /* and within a bar, all or none: a bar with a button that has no icon (MASTER, NEWEST) shows none */
        const char *t = buttons[i] + (buttons[i][0] == '-' || buttons[i][0] == '+' || buttons[i][0] == '*' || buttons[i][0] == '^');
        if (buttons[i][0] != '\0' && buttons[i][0] != '-' && !gicon_known(t)) {
            noIcons = 1;
        }
    }
    for (i = 0; i < count; i++) {
        const char *label = buttons[i];
        /* "-" disabled, "+" on, "*" pressed (preview of the touch feedback) */
        int grey = label[0] == '-', on = label[0] == '+', pressed = label[0] == '*', focus = label[0] == '^', x0, x1,
            y0 = GBTN_Y0, y1 = gbtn_y1();
        const char *t = label + (grey || on || pressed || focus);
        char shown[40];
        Colour ink = grey ? (Colour){ 150, 155, 178, 255 } : WHITE, top, bottom, edge, hole;
        int kind = sStyle == 3 ? GF_BODY : GF_HEAD, iconW = 0, textW, textX0 = 0, groupW, gx, trk = gbtn_track(), btnTrack = 0, btnPen = 0, btnWiden = 0, iconSolidL = 0, iconGap = GICON_GAP, btnSpace = 0;
        Ink ik;
        /* disabled differs by more than colour: it loses its icon and its fill goes flat */
        if (gicon_known(t)) { /* disabled keeps its icon, greyed with its label */
            MEASURE(gicon_raw(t, 0, 0, WHITE, WHITE), ik);
            iconW = ik.x1 - ik.x0;
        }
        if (grey && label[1] == '\0') {
            continue;
        }
        if (sBtnLastThird) {
            gsplit(GX0, GX1, 3, 2, &x0, &x1); /* beside a progress panel: the bar's last third */
        } else {
            gsplit(GX0, GX1, shownCount, slot++, &x0, &x1);
        }
        if (i < 8) {
            sGBtnX0[i] = (short) x0;
            sGBtnX1[i] = (short) x1;
        }
        /* The button. */
        top = (Colour){ 52, 104, 216, 255 };
        bottom = (Colour){ 30, 68, 196, 255 };
        edge = (Colour){ 255, 255, 255, 150 };
        if (sVariant == 3) {
            top = (Colour){ 196, 78, 70, 255 };
            bottom = (Colour){ 160, 48, 64, 255 };
        } else if (sVariant == 4) {
            top = (Colour){ 42, 92, 200, 255 };
            bottom = (Colour){ 30, 68, 176, 255 };
        }
        if (on) {
            top = (Colour){ 60, 170, 80, 255 };
            bottom = (Colour){ 30, 130, 50, 255 };
        }
        if (grey) {
            top = (Colour){ 96, 100, 124, 255 };
            bottom = (Colour){ 78, 82, 104, 255 };
            edge = (Colour){ 255, 255, 255, 60 };
        }
        if (grey) {
            bottom = top; /* flat: no gradient, no bevel */
        }
        if (pressed) {
            /* pushed in: the gradient turned over and darkened, the label 1 px lower */
            Colour t0 = top;
            top = (Colour){ bottom.r * 3 / 4, bottom.g * 3 / 4, bottom.b * 3 / 4, bottom.a };
            bottom = (Colour){ t0.r * 3 / 4, t0.g * 3 / 4, t0.b * 3 / 4, t0.a };
        }
        hole = top;
        {
            int q = sQuiet;
            sQuiet = 1;
            switch (sStyle) {
                case 0:
                    if (grey) {
                        top = bottom = (Colour){ 70, 70, 84, 255 }; /* dark flat fill: the dimmed label keeps 5:1 */
                    } else if (pressed) {
                        /* pressed: lit, a pale fill under its rim and the label in navy, as the other looks show a tap
                           (the darker blue it had read as one more resting button) */
                        top = (Colour){ 232, 240, 255, 255 };
                        bottom = (Colour){ 176, 200, 250, 255 };
                        ink = (Colour){ 16, 32, 110, 255 };
                    } else if (!on) {
                        top = (Colour){ 70, 130, 255, 255 };
                        bottom = (Colour){ 20, 50, 170, 255 };
                    }
                    /* a light blue edge: the game's yellow edge read as "focused", and every button had it */
                    /* on: a light green edge on its green fill (gold read as the selected box) */
                    box_classic(x0, y0, x1, y1, (Colour){ 0, 0, 0, 0 },
                                grey ? (Colour){ 190, 190, 200, 255 } : on ? (Colour){ 160, 230, 170, 255 } : (Colour){ 150, 190, 255, 255 });
                    {
                        int y;
                        for (y = y0 + 1; y < y1 - 1; y++) {
                            fill(x0 + 1, y, x1 - 1, y + 1, mix(top, bottom, y - y0 - 1, y1 - y0 - 3));
                        }
                    }
                    hole = mix(top, bottom, 1, 2);
                    break;
                case 1:
                    if (pressed) {
                        /* pressed: lit (a pale glass, the label dark), not the resting gradient darkened a quarter */
                        top = (Colour){ 236, 242, 255, 240 };
                        bottom = (Colour){ 196, 212, 250, 240 };
                        ink = (Colour){ 16, 36, 110, 255 };
                    }
                    smooth_box(x0, y0, x1, y1, 6, top, bottom, edge, 1);
                    hole = mix(top, bottom, 1, 2);
                    break;
                case 2:
                    if (gcarbon()) {
                        Colour f = grey ? (Colour){ 40, 44, 52, 255 } : on ? (Colour){ 40, 140, 70, 255 } : pressed ? (Colour){ 210, 216, 226, 255 } : (Colour){ 28, 31, 38, 255 };
                        /* pressed: lit (a light fill, the label dark), so a tap shows on a dark bar: the darker fill it
                           had read as the rest state */
                        gfill_edge(x0, y0, x1, y1, f, grey ? (Colour){ 60, 66, 80, 255 } : (Colour){ 120, 130, 150, 255 });
                        hole = f;
                        if (pressed) {
                            ink = (Colour){ 16, 18, 24, 255 };
                        }
                        if (grey) {
                            ink = (Colour){ 120, 126, 146, 255 };
                        }
                    } else {
                        Colour bt = grey ? (Colour){ 62, 66, 92, 255 } : on ? (Colour){ 40, 150, 70, 255 } : pressed ? (Colour){ 236, 240, 252, 255 } : (Colour){ 52, 74, 168, 255 };
                        Colour bb = grey ? (Colour){ 62, 66, 92, 255 } : on ? (Colour){ 30, 120, 56, 255 } : pressed ? (Colour){ 200, 210, 240, 255 } : (Colour){ 36, 54, 134, 255 };
                        /* pressed: lit, the label in the bar's navy (a darker blue slab barely differed from rest) */
                        slab(x0, y0, x1, y1, 6, bt, bb);
                        hole = mix(bt, bb, 1, 2);
                        if (pressed) {
                            ink = (Colour){ 20, 32, 96, 255 };
                        }
                    }
                    break;
                default: {
                    Colour f = pressed ? (Colour){ 255, 255, 255, 215 } : grey ? (Colour){ 0, 0, 0, 60 } : on ? (Colour){ 40, 150, 80, 230 }
                                                                                                : (Colour){ 255, 255, 255, 20 };
                    /* disabled: a dark fill and no rim, its label greyed; pressed: lit (a light fill under its rim, the
                       label in the backdrop's dark, 1 px lower), so it can't be taken for rest or disabled */
                    Colour deep = sVariant == 2 ? (Colour){ 8, 60, 56, 255 } : sVariant == 3 ? (Colour){ 70, 24, 80, 255 }
                                                                                       : (Colour){ 14, 30, 90, 255 };
                    smooth_box(x0, y0, x1, y1, 8, f, f, (Colour){ 255, 255, 255, grey ? 0 : 110 }, 1);
                    hole = pressed ? (Colour){ 222, 226, 236, 255 } : deep;
                    if (pressed) {
                        ink = deep;
                    }
                    if (on) {
                        ink = WHITE; /* on: a green fill, as the other looks', not green letters alone */
                        hole = f;
                    }
                    break;
                }
            }
            sQuiet = q;
        }
        if (pressed) {
            /* pushed in: a shadow right under its top edge (inside the rim, full width), so a tap never reads as a
               lit button; set 2 px lower it left a light strip above it that read as a stray dash */
            int in = sStyle == 2 && !gcarbon() ? 0 : 1;
            QUIET(fill(x0 + in, y0 + in, x1 - in, y0 + in + 2, (Colour){ 0, 0, 0, 70 }));
        }
        if (focus) {
            /* selected (the D-pad's focus, not a tap): the resting button inside a 2 px gold box, 1 px clear of it */
            Colour g = GOLD;
            QUIET(fill(x0 - 3, y0 - 3, x1 + 3, y0 - 1, g));
            QUIET(fill(x0 - 3, y1 + 1, x1 + 3, y1 + 3, g));
            QUIET(fill(x0 - 3, y0 - 1, x0 - 1, y1 + 1, g));
            QUIET(fill(x1 + 1, y0 - 1, x1 + 3, y1 + 1, g));
            log_rec('B', x0 - 3, y0 - 3, x1 + 3, y1 + 3, 0, "focus");
        }
        log_rec('B', x0, y0, x1, y1, 9, "button");
        if (pressed) {
            log_rec('B', x0, y0 + 2, x1, y1, 0, "pressface"); /* the face below the press shadow, where its label centres */
        }
        gink_box(x0, y0, x1, y1 + 1); /* (y1 + 1: a pressed label sits 1 px lower) */
        /* The label: capitals in the heading font (Dock: title case in its body font), with its icon. */
        if (sStyle == 3) {
            glabel(t, shown, sizeof(shown)); /* Dock and Jungle: title case ("Stop Test") */
        } else {
            upper_case(port_text(t), shown, sizeof(shown));
        }
        if (noIcons) {
            iconW = 0;
        }
        /* a label wider than its button less 8 px each side: without its icon, then a size smaller (never clipped or
           over the next box; logged so the checks catch any label still too wide) */
        if (gtext_width(kind, shown, 0) + (iconW ? iconW + GICON_GAP : 0) > x1 - x0 - 16) {
            iconW = 0;
            while (kind != GF_SM && gtext_width(kind, shown, 0) > x1 - x0 - 16) {
                kind = kind == GF_HEAD ? GF_BODY : GF_SM;
            }
            if (gtext_width(kind, shown, 0) > x1 - x0 - 16) {
                log_rec('E', x0, y0, x1, y1, 0, "label-too-wide");
            }
        }
        if (iconW) {
            /* the icon as it is drawn here (a dark icon on a lit key has no outline): the gap is from its own ink */
            Ink ki;
            sSolidX0 = 99999, sSolidX1 = -99999;
            MEASURE(gicon_raw(t, 0, 0, ink, hole), ki);
            iconW = ki.x1 - ki.x0;
            iconSolidL = sSolidX1 < sSolidX0 ? 0 : sSolidX0 - ki.x0; /* (a smooth icon's faint left edge) */
        }
        {
            /*
             * centred on the label's ink (Classic's fun font carries a 1 px shadow its advance doesn't show), icon and
             * label as one group: the same gap left and right to the pixel, for the faint edge and the solid ink
             * alike. The letters are spread or drawn together by a fraction of a pixel, or the pen moved a quarter
             * pixel over, until the group leaves an even number of columns and its solid ink sits in its middle
             */
            Ink kt;
            int q, tt, sg, done = 0, fq = 0, ft = trk, crisp = sUiCrispAll;
            sFlatMode = grey && sStyle == 0 ? 2 : 0;
            sUiFlat = grey && sStyle == 0;
            if (gfont(kind) != NULL && gfont(kind)->phases == 1) { /* (whole-pixel fonts: Classic and the drawn sets) */
                sUiCrispAll = 1; /* measured as gtext_ex draws a boxed Classic label: every letter solid */
                Ink kq;
                int gw;
                sUiTrack = trk;
                sPen0 = 0;
                MEASURE(gdraw_raw(kind, 0, 0, shown, ink), kq);
                gw = kq.x1 - kq.x0 + (iconW ? iconW + GICON_GAP : 0);
                if (((x1 - x0 - gw) & 1) && !ui_widen_on()) {
                    /* the odd column taken up where it isn't seen (ui_whole_fit), else the label a pixel left */
                    int tr = trk, sp = 0;
                    if (ui_whole_fit(kind, shown, ink, x1 - x0, iconW, GICON_GAP, trk, &kq, &tr, &sp, &iconGap, !gfont(kind)->outlined)) {
                        kt = kq, fq = 0, ft = tr;
                        btnSpace = sp;
                    } else {
                        kt = kq, fq = 0, ft = trk;
                        log_rec('E', x0, y0, x1, y1, 1, "uneven-button");
                    }
                    done = 2;
                } else if ((x1 - x0 - gw) & 1) {
                    /* one letter a column wider (see sUiWiden), not the letters spread */
                    sUiWiden = 1;
                    sUiTrack = trk;
                    sPen0 = 0;
                    MEASURE(gdraw_raw(kind, 0, 0, shown, ink), kq);
                    gw = kq.x1 - kq.x0 + (iconW ? iconW + GICON_GAP : 0);
                    sUiWiden = 0;
                    if (!((x1 - x0 - gw) & 1)) {
                        kt = kq, fq = 0, ft = trk;
                        done = 2;
                        btnWiden = 1;
                    }
                }
            }
            for (q = 0; q < 64 && done < 2; q += 16) {
                for (tt = 0; tt <= 64 && done < 2; tt += 2) {
                    for (sg = 1; sg >= -1 && done < 2; sg -= 2) {
                        Ink kq;
                        int gw, l, r;
                        if (tt == 0 && sg < 0) {
                            continue;
                        }
                        sUiTrack = trk + sg * tt;
                        sPen0 = q;
                        sSolidX0 = 99999, sSolidX1 = -99999;
                        MEASURE(gdraw_raw(kind, 0, 0, shown, ink), kq); /* (as gtext_ex draws it, moved) */
                        gw = kq.x1 - kq.x0 + (iconW ? iconW + GICON_GAP : 0);
                        l = iconW ? iconSolidL : sSolidX1 < sSolidX0 ? 0 : sSolidX0 - kq.x0;
                        r = sSolidX1 < sSolidX0 ? 0 : kq.x1 - (sSolidX1 + 1);
                        if (!((x1 - x0 - gw) & 1) && (done == 0 || l == r)) {
                            kt = kq, fq = q, ft = sUiTrack;
                            done = l == r ? 2 : 1;
                        }
                    }
                }
            }
            if (!done) {
                sUiTrack = trk;
                sPen0 = 0;
                MEASURE(gtext_ex(kind, 0, y0, 1000, y1, shown, ink, GA_LEFT, 0, NULL, NULL), kt);
            }
            if (done == 0) {
                log_rec('E', x0, y0, x1, y1, done, "uneven-button");
            }
            sUiCrispAll = crisp;
            btnTrack = done ? ft : trk;
            btnPen = done ? fq : 0;
            sPen0 = 0;
            sUiTrack = 0;
            sFlatMode = 0;
            sUiFlat = 0;
            textW = kt.x1 - kt.x0;
            textX0 = 0; /* (gtext_ex sets a left-aligned label's ink at its x) */
        }
        groupW = textW + (iconW ? iconW + iconGap : 0);
        gx = x0 + (x1 - x0 - groupW) / 2;
        if (iconW) {
            Ink k;
            int cy = (y0 + y1) / 2, ix, iy;
            MEASURE(gicon_raw(t, 0, 0, ink, hole), k);
            ix = gx - k.x0;
            iy = y0 + (y1 - y0 - (k.y1 - k.y0)) / 2 - k.y0 + pressed;
            (void) cy;
            sUiFlat = grey; /* a disabled icon flat, as its label */
            QUIET(gicon_raw(t, ix, iy, ink, hole));
            sUiFlat = 0;
            log_rec('P', ix + k.x0, iy + k.y0, ix + k.x1, iy + k.y1, 0, "icon");
            gx += iconW + iconGap;
        }
        /* centred on the capitals' band only, so labels side by side share one baseline (descenders hang below) */
        /* Classic disabled: the label's face alone (no outline, no shadow) in the other looks' grey, as dim as theirs */
        sFlatMode = grey && sStyle == 0 ? 2 : 0;
        sUiFlat = grey && sStyle == 0;
        sUiTrack = btnTrack; /* (the tracking and pen the label was centred with, above) */
        sPen0 = btnPen;
        sUiWiden = btnWiden;
        sUiSpaceExtra = btnSpace;
        gtext_ex(kind, gx - textX0, y0 + pressed, gx - textX0 + 1000, y1 + pressed, shown, ink, GA_LEFT, 0, NULL, NULL);
        sUiSpaceExtra = 0;
        sUiTrack = 0;
        sFlatMode = 0;
        sUiFlat = 0;
    }
}

/* ---- Small shared pieces ---- */

/* A portrait at 18 px (the 20 px art less its outer pixel ring), centred in the cell: 2 px clear of a 22 px row. */
/* 16 px: the icon module of the 8 px grid, 2 px clear inside a 20 px row */
#define GPORT 16
static void gportrait(const RaceHudRacer *r, int x0, int y0, int x1, int y1) {
    const Picture *p = NULL;
    int px = x0 + (x1 - x0 - GPORT) / 2, py = y0 + (y1 - y0 - GPORT) / 2, cut = (20 - GPORT) / 2;
    if (r->special == RACE_HUD_NOT_SPECIAL && r->character >= 0 && r->character < 10) {
        p = &sPortraits[r->character];
    } else if (r->special > RACE_HUD_NOT_SPECIAL && r->special < RACE_HUD_SPECIALS && sSpecialPortraits[r->special].rgba != NULL) {
        p = &sSpecialPortraits[r->special];
    }
    if (p != NULL && p->rgba != NULL && p->width >= 20 && p->height >= 20 && p->width <= 64 && p->height <= 64) {
        /* the art's faint background pixels (alpha under half) dropped: drawn, they tinted a box behind the face on a
           lit row */
        static const Picture *sFrom[24];
        static Picture sClean[24];
        static unsigned char sPx[24][64 * 64 * 4];
        int k, i;
        for (k = 0; k < 24 && sFrom[k] != NULL && sFrom[k] != p; k++) {
        }
        if (k < 24) {
            if (sFrom[k] == NULL) {
                sFrom[k] = p;
                sClean[k].width = p->width;
                sClean[k].height = p->height;
                sClean[k].rgba = sPx[k];
                memcpy(sPx[k], p->rgba, (size_t) p->width * p->height * 4);
                for (i = 0; i < p->width * p->height; i++) {
                    if (sPx[k][i * 4 + 3] < 128) {
                        sPx[k][i * 4 + 3] = 0;
                    }
                }
            }
            p = &sClean[k];
        }
        QUIET(picture_part(p, cut, cut, GPORT, GPORT, px, py, WHITE));
    } else {
        QUIET(fill(px, py, px + GPORT, py + GPORT, (Colour){ 0, 20, 90, 255 }));
    }
    log_rec('P', px, py, px + GPORT, py + GPORT, 0, "portrait");
}

static void gformat_time(int frames, char *out, int size) {
    int hundredths, seconds, minutes;
    if (frames < 0) {
        frames = 0;
    }
    hundredths = (frames % 60) * 100 / 60;
    seconds = (frames / 60) % 60;
    minutes = frames / 3600;
    snprintf(out, size, "%02d'%02d\"%02d", minutes % 100, seconds, hundredths); /* the game's own 00'31"00 */
}

/* The place ("3" with "RD" raised to the digit's top), centred in the box: 4 clear columns from the figure's ink box to the
 * suffix's (at 2 and 3 the 3's right side read as touching the R) */
static void gplace(const RaceHud *hud, int x0, int y0, int x1, int y1) {
    static const char *const sSuffix[4] = { "ST", "ND", "RD", "TH" };
    char digit[4];
    int top, h, stop, sh, wD, wS, x, y, gap = 4;
    Ink kd, ks;
    if (hud->position < 1 || hud->position > 8) {
        return;
    }
    if (sStyle == 0) {
        /* Classic: the game's own place sprites, as one group */
        MEASURE(place_row(hud->position, 0, 0), kd);
        x = x0 + (x1 - x0 - (kd.x1 - kd.x0)) / 2 - kd.x0;
        y = y0 + (y1 - y0 - (kd.y1 - kd.y0)) / 2 - kd.y0;
        QUIET(place_row(hud->position, x, y));
        log_rec('P', x + kd.x0, y + kd.y0, x + kd.x1, y + kd.y1, 0, "place");
        return;
    }
    snprintf(digit, sizeof(digit), "%d", hud->position);
    gband(GF_XL, 1, &top, &h);
    gband(GF_BODY, 0, &stop, &sh);
    MEASURE(gdraw_raw(GF_XL, 0, 0, digit, GOLD), kd);
    MEASURE(gdraw_raw(GF_BODY, 0, 0, sSuffix[hud->position <= 3 ? hud->position - 1 : 3], GOLD), ks);
    wD = kd.x1 - kd.x0;
    wS = ks.x1 - ks.x0;
    x = x0 + (x1 - x0 - (wD + gap + wS)) / 2;
    y = y0 + (y1 - y0 - h) / 2 - top;
    QUIET(gdraw_raw(GF_XL, x - kd.x0, y, digit, gink_accent()));
    log_rec('C', x, y + top, x + wD, y + top + h, GF_XL, digit);
    /* the suffix's capitals start at the digit's top */
    QUIET(gdraw_raw(GF_BODY, x + wD + gap - ks.x0, y + top - stop, sSuffix[hud->position <= 3 ? hud->position - 1 : 3],
                    gink_accent()));
    log_rec('C', x + wD + gap, y + top, x + wD + gap + wS, y + top + sh, GF_BODY, "suffix");
}

/* "LAP 2/3" over the lap segments (or FINAL LAP / FINISH), as one block centred in the box. */
static void glap(const RaceHud *hud, int x0, int y0, int x1, int y1, int segments) {
    char value[16];
    const char *label = "Lap";
    char lapCase[16], finalCase[16];
    int ltop, lh, vtop, vh, lw, vw, blockH, by, x, segH = 6, segGap = 8, rowW;
    Ink kl, kv;
    int final = hud->laps > 1 && hud->lap >= hud->laps && !hud->finished;
    if (hud->laps <= 0 && !hud->finished) {
        return;
    }
    if (hud->finished) {
        snprintf(value, sizeof(value), "FINISH");
        label = "";
    } else if (final) {
        /* "FINAL LAP 3/3": the label grows, the value keeps its size, so the block still fits its box */
        snprintf(value, sizeof(value), "%d/%d", hud->lap, hud->laps);
        label = "Final";
        final = 0;
    } else {
        snprintf(value, sizeof(value), "%d/%d", hud->lap, hud->laps);
    }
    /* in the look's label case: "LAP 2/3" in Broadcast and Carbon, "Lap 2/3" in the others */
    glabel("Lap", lapCase, sizeof(lapCase));
    glabel("Final", finalCase, sizeof(finalCase));
    label = label[0] == '\0' ? label : strcmp(label, "Final") == 0 ? finalCase : lapCase;
    gband(GF_BODY, 0, &ltop, &lh);
    gband(final || hud->finished ? GF_HEAD : GF_LG, 1, &vtop, &vh);
    MEASURE(gdraw_raw(GF_BODY, 0, 0, label, WHITE), kl);
    if (final || hud->finished) {
        MEASURE(gdraw_raw(GF_HEAD, 0, 0, value, WHITE), kv);
    } else {
        MEASURE(gdraw_tab(GF_LG, 0, 0, value, WHITE), kv);
    }
    lw = label[0] ? kl.x1 - kl.x0 + 4 : 0;
    vw = kv.x1 - kv.x0;
    if (lw + vw > x1 - x0 - 2 * GPAD && strcmp(label, finalCase) == 0) {
        /* "Final" doesn't fit beside the value in a narrow cell: "Lap", and the last segment shows it's the final one */
        label = lapCase;
        MEASURE(gdraw_raw(GF_BODY, 0, 0, label, WHITE), kl);
        lw = kl.x1 - kl.x0 + 4;
    }
    rowW = lw + vw;
    blockH = vh + (segments ? segGap + segH : 0);
    by = y0 + (y1 - y0 - blockH) / 2;
    x = x0 + (x1 - x0 - rowW) / 2;
    if (label[0]) {
        /* LAP sits on the digits' baseline */
        QUIET(gdraw_raw(GF_BODY, x - kl.x0, by + vh - lh - ltop, label, gink_label()));
        log_rec('C', x, by + vh - lh, x + lw - 4, by + vh, GF_BODY, label);
    }
    if (final || hud->finished) {
        QUIET(gdraw_raw(GF_HEAD, x + lw - kv.x0, by - vtop, value, gink_accent()));
    } else {
        QUIET(gdraw_tab(GF_LG, x + lw - kv.x0, by - vtop, value, gink_accent()));
    }
    log_rec('C', x + lw, by, x + lw + vw, by + vh, GF_LG, value);
    if (segments && hud->laps > 0) {
        int n = hud->laps > 5 ? 5 : hud->laps, k, sw = (rowW - (n - 1) * 3) / n, sx = x, sy = by + vh + segGap;
        Colour lit = gbroadcast() ? (Colour){ 255, 150, 20, 255 } : gcarbon() ? (Colour){ 0, 190, 200, 255 } : GOLD;
        Colour unlit = { 255, 255, 255, 50 };
        int q = sQuiet;
        sw = sw < 6 ? 6 : sw;
        sx = x + (rowW - (n * sw + (n - 1) * 3)) / 2;
        sQuiet = 1;
        for (k = 0; k < n; k++) {
            int a = sx + k * (sw + 3), filled = k < hud->lap - 1 || hud->finished ? sw : (k == hud->lap - 1 ? sw / 2 : 0);
            if (sStyle == 0 || gcarbon()) {
                fill(a, sy, a + sw, sy + segH, unlit);
                if (filled) {
                    fill(a, sy, a + filled, sy + segH, lit);
                }
            } else {
                smooth_box(a, sy, a + sw, sy + segH, 2, unlit, unlit, (Colour){ 0, 0, 0, 0 }, 1);
                if (filled) {
                    smooth_box(a, sy, a + filled, sy + segH, 2, lit, lit, (Colour){ 0, 0, 0, 0 }, 1);
                }
            }
        }
        sQuiet = q;
        log_rec('P', sx, sy, sx + n * sw + (n - 1) * 3, sy + segH, 0, "segments");
    }
}

/* The item held: its picture, and the count beside it ("x3") rather than over its corner. */
static void gtable_row(int slot, const char *label, const char *time, Colour timeInk, const char *delta, Colour deltaInk,
                       int lit);

/* Times in tables and fact lists: the heading weight, except where the heading font is a bigger size than the text
 * (Dock, Jungle) and Classic (its timer sprites): there the text font, so a screen keeps one text size */
static int gtime_font(void) {
    return sStyle == 0 || sStyle == 3 ? GF_BODY : GF_HEAD;
}

/*
 * A hint centred in a cell, wrapped at words so every line keeps 8 px clear of the cell's sides (more than the
 * padding: a hint that nearly fills its cell reads as crammed). Lines one pitch apart from y. measure: only count lines.
 */
static int sWrapWords; /* gwrap: one word a line (the carry hints, so every look breaks them alike) */
static int gwrap(int kind, int x0, int x1, int y, const char *text, Colour c, int pitch, int measure) {
    int width = x1 - x0 - 2 * (gpad_x(SK_PANEL) + 2), lines = 0, top, h;
    const char *p = port_text(text);
    gband(kind, 0, &top, &h);
    while (*p != '\0') {
        char line[64];
        int n = 0, best = 0;
        while (p[n] != '\0' && n < 63) {
            int e = n;
            while (p[e] != '\0' && p[e] != ' ') {
                e++;
            }
            snprintf(line, sizeof(line), "%.*s", e, p);
            if (best > 0 && (sWrapWords || gtext_width(kind, line, 0) > width)) {
                break;
            }
            best = e;
            n = e;
            while (p[n] == ' ') {
                n++;
            }
        }
        snprintf(line, sizeof(line), "%.*s", best, p);
        if (!measure) {
            gtext_ex(kind, x0, y + lines * pitch, x1, y + lines * pitch + h, line, c, GA_CENTRE, 0, NULL, NULL);
        }
        lines++;
        p += n;
    }
    return lines;
}

static int sNoCaption; /* set: gbananas draws into a given area, without its caption */

/* A HUD label in the look's case: Broadcast and Carbon set every label in capitals (TOTAL, BEST LAP, VS BEST RUN), on
   every screen; the other looks as written. buf holds the capitals. */
/*
 * Labels, tabs, card titles, headings and buttons follow the look's label case: Broadcast and Carbon in capitals, Dock
 * and Jungle in title case, the others in sentence case. Prose, hints and values keep their own case. A word in
 * capitals of 4 letters or more (a heading's "DRAW KINDS", "FINAL") is taken as a plain word; shorter ones (HUD, GPU)
 * and words with figures stay as written.
 */
static const char *glabel(const char *s, char *buf, int size) {
    static const char *const sSmall[] = { "a", "an", "and", "of", "on", "the", "to", "in", "vs" };
    const char *t;
    int i = 0, word = 0, first = 1;
    if (s == NULL) {
        return s;
    }
    t = port_text(s);
    if (sStyle == 2) {
        upper_case(t, buf, size);
        return buf;
    }
    while (t[i] != '\0' && i < size - 1) {
        int a = i, b = i, caps = 1, letters = 0, k;
        while (t[b] != '\0' && t[b] != ' ') {
            if (t[b] >= 'a' && t[b] <= 'z') {
                caps = 0;
            }
            if ((t[b] >= 'A' && t[b] <= 'Z') || (t[b] >= 'a' && t[b] <= 'z')) {
                letters++;
            } else if (t[b] != '\'' && t[b] != '-') {
                caps = 0; /* "T.T.", "3/3", "4x": as written */
            }
            b++;
        }
        for (k = a; k < b && k < size - 1; k++) {
            char c = t[k];
            int plain = caps && letters >= 4; /* a heading's capitals: a plain word */
            if (plain && c >= 'A' && c <= 'Z' && (k > a || (!first && sStyle != 3))) {
                /* (sentence case: a heading's later words all lower, "DRAW KINDS" -> "Draw kinds") */
                c = (char) (c - 'A' + 'a');
            }
            if (sStyle == 3 && k > a && t[k - 1] == '-' && c >= 'a' && c <= 'z') {
                c = (char) (c - 'a' + 'A'); /* Title Case takes each part of a compound: "Anti-Aliasing" */
            }
            if (k == a && c >= 'a' && c <= 'z') {
                int up = first || sStyle == 3;
                unsigned j;
                if (!first && sStyle == 3) {
                    for (j = 0; j < sizeof(sSmall) / sizeof(sSmall[0]); j++) {
                        if ((int) strlen(sSmall[j]) == b - a && strncmp(t + a, sSmall[j], (size_t) (b - a)) == 0) {
                            up = 0;
                        }
                    }
                }
                if (up) {
                    c = (char) (c - 'a' + 'A');
                }
            }
            buf[k] = c;
        }
        i = b;
        while (t[i] == ' ' && i < size - 1) {
            buf[i] = ' ';
            i++;
        }
        first = 0;
        word++;
    }
    buf[i < size ? i : size - 1] = '\0';
    (void) word;
    return buf;
}

/* A heading or card title in the look's label case. */
static void gtitle(int kind, int x0, int y0, int x1, int y1, const char *s, Colour c, int align) {
    char buf[64];
    gtext(kind, x0, y0, x1, y1, glabel(s, buf, sizeof(buf)), c, align);
}

/* A square cell's caption on its top padding line; returns the top of what is left under it (caption + 4 px). */
static int gcaption(int x0, int y0, int x1, const char *label) {
    int bt, bh;
    char upper[40];
    gband(GF_BODY, 0, &bt, &bh);
    label = glabel(label, upper, sizeof(upper));
    gtext(GF_BODY, x0, y0 + GPAD, x1, y0 + GPAD + bh, label, gink_label(), GA_CENTRE);
    return y0 + GPAD + bh + 4;
}

/* The item picture's size beside a one-digit count ("x1"): the cell's padding, 6 px, and the count around it. */
static int gitem_size(int cellW) {
    int size = cellW - 2 * GPAD - (gtext_width(GF_LG, "x1", 1) + 6);
    return size > 36 ? 36 : size;
}

/*
 * The item's picture at 1:1: the inside of the ROM's 40x40 icon, 30x30, the game's own pixels (a resize of the
 * framed 40x40 smeared the art and its frame). The icon's frame is 4 px of blue and a 1 px bevel round the art (a light
 * line on its top and right, a dark one on its left, a lilac one on its bottom), and the art sits one row high in it:
 * the cut is at x 5, y 4, inside the bevel on every side.
 */
#define GITEM 30
#define GITEM_X 5
#define GITEM_Y 4
static const Picture *gitem_pic(int index) {
    static Picture sInner[15];
    static unsigned char sPx[15][GITEM * GITEM * 4];
    const Picture *p = &sItems[index];
    if (p->rgba == NULL || p->width < GITEM_X + GITEM || p->height < GITEM_Y + GITEM) {
        return p;
    }
    if (sInner[index].rgba == NULL) {
        int y;
        for (y = 0; y < GITEM; y++) {
            memcpy(sPx[index] + y * GITEM * 4, p->rgba + ((y + GITEM_Y) * p->width + GITEM_X) * 4, GITEM * 4);
        }
        sInner[index].width = GITEM;
        sInner[index].height = GITEM;
        sInner[index].rgba = sPx[index];
    }
    return &sInner[index];
}

/*
 * A picture and its count ("x3") as one group centred in the cell by their ink: 6 px apart, or 4 when 6 would cost the
 * cell its padding; the count's ink centred on the picture's height (not sat on its bottom line).
 */
static void gcount_group(const Picture *pic, int w, int h, const char *count, int x0, int y0, int x1, int y1) {
    Ink k, kc;
    int inkW, countW = 0, gap = 0, sx, cy, bt, bh;
    MEASURE(picture_fit(pic, 0, 0, w, h), k);
    inkW = k.x1 > k.x0 ? k.x1 - k.x0 : w;
    gband(GF_LG, 1, &bt, &bh);
    if (count[0] != '\0') {
        MEASURE(gnum(GF_LG, 0, 0, 1000, bh, count, WHITE, GA_LEFT), kc);
        countW = kc.x1 - kc.x0;
        /* 6 px, closing to 3 (the multiplication sign's own side bearing keeps it clear) to keep the padding */
        for (gap = 6; gap > 3 && inkW + gap + countW > x1 - x0 - 2 * GPAD; gap--) {
        }
    }
    sx = x0 + (x1 - x0 - (inkW + gap + countW) + 1) / 2;
    gpic_fit(pic, sx, y0, sx + inkW, y1, w, h, 0);
    if (countW) {
        cy = y0 + (y1 - y0) / 2;
        gnum(GF_LG, sx + inkW + gap - kc.x0, cy - (kc.y0 + kc.y1) / 2, x1, cy - (kc.y0 + kc.y1) / 2 + bh, count, gink_accent(),
             GA_LEFT);
    }
}

/* The ghosts icon less its first row and column: the texture carries a stray opaque black line on its top and left
   edges (invisible on Classic's dark panels, a black stripe on the others). */
static const Picture *gghost_pic(void) {
    static Picture sClean;
    static unsigned char sPx[64 * 64 * 4];
    if (sGhostIcon.rgba == NULL || sGhostIcon.width < 2 || sGhostIcon.width > 65 || sGhostIcon.height > 65) {
        return &sGhostIcon;
    }
    if (sClean.rgba == NULL) {
        int y, w = sGhostIcon.width - 1, h = sGhostIcon.height - 1;
        for (y = 0; y < h; y++) {
            memcpy(sPx + y * w * 4, sGhostIcon.rgba + ((y + 1) * sGhostIcon.width + 1) * 4, w * 4);
        }
        sClean.width = w;
        sClean.height = h;
        sClean.rgba = sPx;
    }
    return &sClean;
}

static void gitem(const RaceHud *hud, int x0, int y0, int x1, int y1) {
    int index = hud->item * 3 + hud->itemLevel, size;
    char count[8] = "";
    y0 = gcaption(x0, y0, x1, "Item");
    y1 -= GPAD;
    if (hud->item < 0 || index < 0 || index >= 15 || sItems[index].rgba == NULL) {
        gtext(GF_BODY, x0, y0, x1, y1, "No item", gink_dim(), GA_CENTRE);
        return;
    }
    if (hud->itemCount >= 1 && hud->itemCount <= 10) { /* "x1" too: the cell reads the same as the bananas' */
        snprintf(count, sizeof(count), "x%d", hud->itemCount);
    }
    size = GITEM; /* always 1:1 */
    gcount_group(gitem_pic(index), size, size, count, x0, y0, x1, y1);
}

/* The bananas: the game's banana and "x07" in the large digits, as one group centred in the box. */
static void gbananas(const RaceHud *hud, int x0, int y0, int x1, int y1) {
    char count[8];
    int h = 36, w = sBanana.width * h / (sBanana.height > 0 ? sBanana.height : 1), countW, gx;
    snprintf(count, sizeof(count), "x%d", hud->bananas % 100); /* one count format with the item's ("x3", "x7") */
    if (!sNoCaption) {
        y0 = gcaption(x0, y0, x1, "Bananas");
        y1 -= GPAD;
    }
    if (hud->mode == RACE_HUD_MODE_RACE && hud->silverCoins >= 0) {
        /* A silver coin race: the bananas and the coins as two rows, icons in one column, numbers in another */
        char coins[8];
        int ih = 18, rowGap = 4, top2, h2, rowsH = 2 * ih + rowGap, ry = y0 + (y1 - y0 - rowsH) / 2, iw = 18, nx, nw;
        snprintf(coins, sizeof(coins), "%d/8", hud->silverCoins);
        nw = gtext_width(GF_LG, count, 1);
        if (gtext_width(GF_LG, coins, 1) > nw) {
            nw = gtext_width(GF_LG, coins, 1);
        }
        {
            /* 6 px between icons and numbers, 4 when Classic's wide timer digits would otherwise crowd the cell's sides */
            int gap = iw + 6 + nw <= x1 - x0 - 2 * GPAD ? 6 : 4;
            gx = x0 + (x1 - x0 - (iw + gap + nw)) / 2;
            nx = gx + iw + gap;
        }
        gband(GF_LG, 1, &top2, &h2);
        {
            /* the two numbers share their ink's left edge ("x7" over "5/8"), not their cells' */
            Ink a, b;
            MEASURE(gnum(GF_LG, 0, 0, 1000, h2, count, WHITE, GA_LEFT), a);
            MEASURE(gnum(GF_LG, 0, 0, 1000, h2, coins, WHITE, GA_LEFT), b);
            gpic_fit(&sBanana, gx, ry, gx + iw, ry + ih, iw, ih, 0);
            gnum(GF_LG, nx - a.x0, ry, x1, ry + ih, count, gink_accent(), GA_LEFT);
            gpic_fit(&sCoin, gx, ry + ih + rowGap, gx + iw, ry + rowsH, iw, ih, 0);
            gnum(GF_LG, nx - b.x0, ry + ih + rowGap, x1, ry + rowsH, coins, hud->silverCoins >= 8 ? BEHIND : WHITE, GA_LEFT);
        }
        return;
    }
    countW = gtext_width(GF_LG, count, 1);
    {
        /*
         * the banana's ink as tall as the item picture beside a count (the cells read as one set), shrunk only if the
         * group would lose the cell's padding; laid out by its ink, 6 px before the count
         */
        int target = gitem_size(x1 - x0), inkW, inkH;
        Ink k;
        MEASURE(picture_fit(&sBanana, 0, 0, sBanana.width * target / sBanana.height, target), k);
        inkH = k.y1 - k.y0 > 0 ? k.y1 - k.y0 : 1;
        h = target * target / inkH;
        w = sBanana.width * h / sBanana.height;
        MEASURE(picture_fit(&sBanana, 0, 0, w, h), k);
        inkW = k.x1 - k.x0;
        if (inkW + 6 + countW > x1 - x0 - 2 * GPAD) {
            h = h * (x1 - x0 - 2 * GPAD - 6 - countW) / (inkW > 0 ? inkW : 1);
            w = sBanana.width * h / sBanana.height;
            MEASURE(picture_fit(&sBanana, 0, 0, w, h), k);
            inkW = k.x1 - k.x0;
        }
        (void) inkH;
        gcount_group(&sBanana, w, h, count, x0, y0, x1, y1);
    }
}

/* Rows of "label ... value" in a box: labels at the left padding, values right-aligned, one even pitch. */
static void gfacts(int x0, int y0, int x1, int y1, const char *const *labels, const char *const *values, int n,
                   const Colour *valueInk) {
    int k, top, h, pitch, blockH, by, padX = gpad_x(SK_PANEL);
    gband(GF_BODY, 0, &top, &h);
    pitch = (y1 - y0 - 2 * GPAD) / (n > 0 ? n : 1);
    if (pitch > 20) {
        pitch = 20;
    }
    blockH = (n - 1) * pitch + h;
    by = y0 + (y1 - y0 - blockH) / 2;
    for (k = 0; k < n; k++) {
        int ry = by + k * pitch;
        char up[40];
        gtext(GF_BODY, x0 + padX, ry, x1 - padX, ry + h, glabel(labels[k], up, sizeof(up)), gink_label(), GA_LEFT);
        gnum(gtime_font(), x0 + padX, ry, x1 - padX - (gbroadcast() ? 0 : 0), ry + h, values[k],
             valueInk != NULL ? valueInk[k] : WHITE, GA_RIGHT);
    }
}

/* "Current" is the lap being driven (the last timed lap while the race runs), not the last lap finished. */
static int gcurrent_lap(const RaceHud *hud, int lastDone) {
    int timed, done;
    timed_laps(hud, &timed, &done);
    return timed > done && timed > 0 ? hud->lapTimes[timed - 1] : lastDone;
}

/* ---- Race (Glass, Broadcast, Carbon): the standings tower on the left half, the hero, times, item, bananas right ---- */
static void grace_tower(const RaceHud *hud) {
    static const Colour podium[3] = { { 255, 200, 40, 255 }, { 200, 210, 230, 255 }, { 220, 140, 70, 255 } };
    int i, lapNumber, lapTime, best, x0 = gc0(0), x1 = gc1(1), rx0 = gc0(2), rx1 = gc1(3);
    for (i = 0; i < hud->racerCount && i < RACE_HUD_RACERS; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        int y0 = gy0(i), y1 = gy1(i), nameX, rank1;
        char number[4], gap[16];
        Colour ink = r->player ? gink_lit() : WHITE;
        snprintf(number, sizeof(number), "%d", i + 1);
        gbox(x0, y0, x1, y1, r->player ? SK_LIT : SK_ROW);
        /* rank cell (22 px square, podium colours in the strip looks), then the portrait cell */
        /* rank cell 18 px (a digit is at most 10): the names keep 8 px clear of the gaps ("Bumper -6.11") */
        rank1 = x0 + (sStyle == 0 ? 16 : 18) + gslant(); /* Classic: its 9 px timer digits need less */
        if (sStyle == 2) {
            Colour c = i < 3 ? podium[i] : gcarbon() ? (Colour){ 44, 50, 62, 255 } : (Colour){ 60, 80, 160, 255 };
            /* Broadcast: every rank cell starts after the lit row's 4 px stripe and a 2 px gap, so the stripe stands clear */
            int rx = gbroadcast() ? x0 + 6 : x0, q = sQuiet;
            rank1 = rx + 22;
            sQuiet = 1;
            fill(rx + 1, y0 + 1, rx + 22, y1 - 1, c);
            sQuiet = q;
            gnum(GF_HEAD, rx + 1, y0, rx + 22, y1, number,
                 i < 3 ? (Colour){ 20, 20, 40, 255 } : WHITE, GA_CENTRE);
        } else {
            /* Classic: the game's timer digits (its fun font's 4 and 8 closed up into blobs at this size) */
            gnum(sStyle == 0 ? GF_LG : GF_HEAD, x0 + 2, y0, x0 + (sStyle == 0 ? 16 : 18), y1, number, r->player ? ink : WHITE, GA_CENTRE);
        }
        gportrait(r, rank1 + 2, y0, rank1 + 24, y1);
        nameX = rank1 + 24 + 6;
        {
            /* the name ends 8 px clear of the widest gap in the column (the gaps are right-aligned, so their left ends vary) */
            int j, widest = 0, nameX1;
            for (j = 0; j < hud->racerCount && j < RACE_HUD_RACERS; j++) {
                if (!hud->racers[j].player) {
                    int w;
                    format_gap(hud->racers[j].gap, gap, sizeof(gap));
                    w = gtext_width(GF_BODY, gap, 1);
                    widest = w > widest ? w : widest;
                }
            }
            nameX1 = x1 - gpad_x(SK_ROW) - widest - 8;
            if (sStyle == 2) {
                /* the racer's own row names them "YOU" where the others' codes go, as every look's says "You" */
                gtext(GF_BODY, nameX, y0, nameX1, y1, r->player ? "YOU" : racer_code(r), r->player && gbroadcast() ? ink : WHITE,
                      GA_LEFT);
            } else {
                sUiNoShadow = r->player;
                gtext(GF_BODY, nameX, y0, nameX1, y1, r->player ? "You" : racer_name(r), ink, GA_LEFT);
                sUiNoShadow = 0;
            }
        }
        if (!r->player) {
            format_gap(r->gap, gap, sizeof(gap));
            gnum(GF_BODY, nameX, y0, x1 - gpad_x(SK_ROW), y1, gap, r->gap > 0 ? AHEAD : BEHIND, GA_RIGHT);
        }
    }
    /* Hero: the place in the left half, the lap and its segments in the right half; no divider. */
    gbox(rx0, gy0(0), rx1, gy1(1), SK_CARD);
    /* the place takes 60 px, the lap the rest: "FINAL 3/3" needs more than half the card */
    gplace(hud, rx0, gy0(0), rx0 + 60, gy1(1));
    glap(hud, rx0 + 60, gy0(0), rx1 - gslant(), gy1(1), 1);
    /* Times: Total, Current, Best */
    lap_facts(hud, &lapNumber, &lapTime, &best);
    {
        static const char *const names[3] = { "Total", "Current", "Best lap" }; /* "Best lap": the term every screen uses */
        char v[3][16];
        const char *values[3];
        gformat_time(hud->raceTime, v[0], 16);
        if (lapNumber > 0) {
            gformat_time(gcurrent_lap(hud, lapTime), v[1], 16);
        } else {
            snprintf(v[1], 16, "--'--\"--");
        }
        if (best >= 0) {
            gformat_time(best, v[2], 16);
        } else {
            snprintf(v[2], 16, "--'--\"--");
        }
        values[0] = v[0];
        values[1] = v[1];
        values[2] = v[2];
        gbox(rx0, gy0(2), rx1, gy1(4), SK_PANEL);
        gfacts(rx0, gy0(2), rx1, gy1(4), names, values, 3, NULL);
    }
    /* Item and bananas: two equal squares on the column grid */
    gbox(gc0(2), gy0(5), gc1(2), gy1(7), SK_PANEL);
    gitem(hud, gc0(2), gy0(5), gc1(2), gy1(7));
    gbox(gc0(3), gy0(5), gc1(3), gy1(7), SK_PANEL);
    gbananas(hud, gc0(3), gy0(5), gc1(3), gy1(7));
}

/* ---- Race (Dock, Jungle): the header across the top, the map left, the racers near you and item, bananas right ---- */
static void grace_dock(const RaceHud *hud) {
    int i, me = 0, first, lapNumber, lapTime, best;
    /* every group in a box, as on the other looks: place and lap, then the race times */
    gbox(gc0(0), gy0(0), gc1(1), gy1(1), SK_PANEL);
    gbox(gc0(2), gy0(0), gc1(3), gy1(1), SK_PANEL);
    gplace(hud, gc0(0), gy0(0), gc0(0) + 60, gy1(1));
    glap(hud, gc0(0) + 60, gy0(0), gc1(1), gy1(1), 1);
    lap_facts(hud, &lapNumber, &lapTime, &best);
    {
        static const char *const names[2] = { "Total", "Current" };
        char v[2][16];
        const char *values[2];
        gformat_time(hud->raceTime, v[0], 16);
        if (lapNumber > 0) {
            gformat_time(gcurrent_lap(hud, lapTime), v[1], 16);
        } else {
            snprintf(v[1], 16, "--'--\"--");
        }
        values[0] = v[0];
        values[1] = v[1];
        gfacts(gc0(2), gy0(0), gc1(3), gy1(1), names, values, 2, NULL);
    }
    for (i = 0; i < hud->racerCount && i < RACE_HUD_RACERS; i++) {
        if (hud->racers[i].player) {
            me = i;
        }
    }
    first = me - 1 < 0 ? 0 : me - 1;
    if (first + 3 > hud->racerCount) {
        first = hud->racerCount - 3 < 0 ? 0 : hud->racerCount - 3;
    }
    for (i = first; i < first + 3 && i < hud->racerCount; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        int slot = 2 + (i - first), y0 = gy0(slot), y1 = gy1(slot), x0 = gc0(2), x1 = gc1(3);
        char number[4], gap[16];
        snprintf(number, sizeof(number), "%d", i + 1);
        gbox(x0, y0, x1, y1, r->player ? SK_LIT : SK_ROW);
        gnum(sStyle == 0 ? GF_LG : GF_HEAD, x0 + 2, y0, x0 + 22, y1, number, r->player ? GOLD : WHITE, GA_CENTRE);
        gportrait(r, x0 + 24, y0, x0 + 46, y1);
        gtext(GF_BODY, x0 + 52, y0, x1 - 40, y1, r->player ? "You" : racer_name(r), WHITE, GA_LEFT);
        if (!r->player) {
            format_gap(r->gap, gap, sizeof(gap));
            gnum(GF_BODY, x0 + 52, y0, x1 - GPAD, y1, gap, r->gap > 0 ? AHEAD : BEHIND, GA_RIGHT);
        }
    }
    gbox(gc0(2), gy0(5), gc1(2), gy1(7), SK_PANEL);
    gitem(hud, gc0(2), gy0(5), gc1(2), gy1(7));
    gbox(gc0(3), gy0(5), gc1(3), gy1(7), SK_PANEL);
    gbananas(hud, gc0(3), gy0(5), gc1(3), gy1(7));
    {
        /* under the map: the best lap */
        char t[16];
        if (best >= 0) {
            gformat_time(best, t, sizeof(t));
        }
        gtable_row(7, "Best lap", best >= 0 ? t : "--'--\"--", best >= 0 ? WHITE : gink_dim(), NULL, WHITE, 0);
    }
}

/* ---- Settings ---- */

#define GVALUE_W 114 /* every value chip's width: the widest value (a backup's date) 8 px clear of its arrows */
/* A row's value control at the right end of the row: a stepper chip, a switch, or an arrow for an action. */
/* A value's units in Broadcast and Carbon's capitals ("30 FPS", as their monitor sets them); as written elsewhere. */
static const char *gunit_case(const char *in, char *out, int size) {
    static const char *const sUnits[] = { "fps", "ms", "ips" };
    int n = 0, i = 0;
    if (sStyle != 2) {
        return in;
    }
    while (in[i] != '\0' && n < size - 1) {
        int w = 0, j, unit = 0;
        while (in[i + w] != '\0' && in[i + w] != ' ') {
            w++;
        }
        for (j = 0; j < 3; j++) {
            unit |= (int) strlen(sUnits[j]) == w && strncmp(in + i, sUnits[j], (size_t) w) == 0;
        }
        for (j = 0; j < w && n < size - 1; j++) {
            char c = in[i + j];
            out[n++] = unit && c >= 'a' && c <= 'z' ? (char) (c - 'a' + 'A') : c;
        }
        i += w;
        while (in[i] == ' ' && n < size - 1) {
            out[n++] = in[i++];
        }
    }
    out[n] = '\0';
    return out;
}

static int gvalue(const BsMenuRow *row, int x0, int y0, int x1, int y1, int lit) {
    int inset = (y1 - y0 - 16) / 2, cx1 = x1 - inset - gslant(), cx0, cy0 = y0 + inset, cy1 = y1 - inset;
    int arrowW = sArrowLeft.rgba != NULL ? sArrowLeft.width : 8;
    if (row->value[0] == '\0') {
        /* an action: a chevron at the right padding */
        Ink k;
        int ax;
        MEASURE(gicon_raw("Next", 0, 0, WHITE, WHITE), k);
        ax = x1 - gpad_x(SK_ROW) - k.x1;
        QUIET(gicon_raw("Next", ax, y0 + (y1 - y0 - (k.y1 - k.y0)) / 2 - k.y0, lit && sStyle == 1 ? gink_lit() : WHITE,
                        WHITE));
        log_rec('P', ax + k.x0, y0 + (y1 - y0 - (k.y1 - k.y0)) / 2, ax + k.x1, y0 + (y1 - y0 - (k.y1 - k.y0)) / 2 + k.y1 - k.y0,
                0, "chevron");
        return ax + k.x0;
    }
    if ((strcmp(row->value, "On") == 0 || strcmp(row->value, "Off") == 0) && !port_text_french()) {
        int on = strcmp(row->value, "On") == 0;
        if (sStyle == 1 || sStyle == 3) {
            /* a switch: track 32 x 16 */
            Colour track = on ? (Colour){ 245, 170, 30, 255 } : (Colour){ 20, 30, 80, 200 };
            int tx0 = cx1 - 32;
            QUIET(smooth_box(tx0, cy0, cx1, cy1, 8, track, track, (Colour){ 255, 255, 255, on ? 140 : 80 }, 1));
            QUIET(round_disc(on ? cx1 - 8 : tx0 + 8, (cy0 + cy1) / 2, 6, on ? WHITE : (Colour){ 150, 160, 195, 255 }));
            log_rec('B', tx0, cy0, cx1, cy1, SK_CHIP, "switch");
            gink_box(0, 0, -1, -1);
            return tx0;
        }
        cx0 = cx1 - GVALUE_W; /* as wide as the steppers above and below, so the value column lines up */
        if (sStyle == 2) {
            Colour f = on ? (gcarbon() ? (Colour){ 0, 112, 122, 255 } : (Colour){ 168, 98, 6, 255 }) : (Colour){ 54, 60, 92, 255 };
            int q = sQuiet;
            sQuiet = 1;
            if (gcarbon()) {
                gfill_edge(cx0, cy0, cx1, cy1, f, (Colour){ 0, 0, 0, 0 });
            } else {
                slab(cx0, cy0, cx1, cy1, 4, f, f);
            }
            sQuiet = q;
            log_rec('B', cx0, cy0, cx1, cy1, SK_CHIP, "badge");
            gink_box(cx0, cy0, cx1, cy1);
        } else {
            gbox(cx0, cy0, cx1, cy1, SK_CHIP);
        }
        sUiNoShadow = sStyle == 0; /* (Classic: a chip's value outlined alone, as its tab names: see the stepper's) */
        gtext(GF_BODY, cx0, cy0, cx1, cy1, on ? "ON" : "OFF", on ? WHITE : (Colour){ 196, 202, 226, 255 }, GA_CENTRE);
        sUiNoShadow = 0;
        return cx0;
    }
    {
        /* a stepper: one width on every page (GVALUE_W), arrows 4 px in;
           only a value longer still widens its own chip */
        char buf[64];
        const char *value = gunit_case(row->value, buf, sizeof(buf));
        int need = gtext_width(GF_BODY, value, 0) + 2 * arrowW + 2 * 4 + 2 * 8, w = need > GVALUE_W ? need : GVALUE_W;
        cx0 = cx1 - w;
        gbox(cx0, cy0, cx1, cy1, SK_CHIP);
        if (sArrowLeft.rgba != NULL) {
            /*
             * the game's arrow without its faint drop shadow (dark pixels a shade off the chip: the eye doesn't see
             * them, the gaps would count them), so its ink is the outline and fill alone, an even number of rows that
             * sits exactly in the middle of the chip; the right arrow is the left one mirrored, so the pair matches
             * pixel for pixel (the game's two sprites sit a row apart)
             */
            static Picture clean, mirrored;
            if (clean.rgba == NULL) {
                /*
                 * the arrow made mirror symmetric top to bottom: the sprite's faint dark shadow dropped (pixels a
                 * shade off the chip), then its upper half, tip row included, drawn again upside down under it. Its
                 * ink is then an even number of rows with a 2 px tip, the same shape and shading above and below,
                 * and sits exactly in the middle of the chip's even inside
                 */
                int w = sArrowLeft.width, h = sArrowLeft.height, mx, my, r0 = -1, tip = -1, tipX = 1 << 20, n;
                for (my = 0; my < h; my++) {
                    for (mx = 0; mx < w; mx++) {
                        const unsigned char *q = sArrowLeft.rgba + (my * w + mx) * 4;
                        if (q[3] > 12 && q[0] + q[1] + q[2] >= 64) {
                            r0 = r0 < 0 ? my : r0;
                            if (mx < tipX) {
                                tipX = mx, tip = my;
                            }
                        }
                    }
                }
                n = tip >= r0 && r0 >= 0 ? tip - r0 + 1 : 0;
                if (n > 0 && (clean.rgba = calloc((size_t) w * 2 * n, 4)) != NULL &&
                    (mirrored.rgba = calloc((size_t) w * 2 * n, 4)) != NULL) {
                    clean.width = mirrored.width = w;
                    clean.height = mirrored.height = 2 * n;
                    for (my = 0; my < 2 * n; my++) {
                        int src = r0 + (my < n ? my : 2 * n - 1 - my);
                        for (mx = 0; mx < w; mx++) {
                            const unsigned char *q = sArrowLeft.rgba + (src * w + mx) * 4;
                            unsigned char *d = clean.rgba + (my * w + mx) * 4;
                            if (q[0] + q[1] + q[2] >= 64) {
                                memcpy(d, q, 4);
                            }
                        }
                        for (mx = 0; mx < w; mx++) {
                            memcpy(mirrored.rgba + (my * w + mx) * 4, clean.rgba + (my * w + w - 1 - mx) * 4, 4);
                        }
                    }
                }
            }
            if (mirrored.rgba != NULL) {
                gpic_fit(&clean, cx0 + 4, cy0, cx0 + 4 + arrowW, cy1, clean.width, clean.height, 0);
                gmirror_rows(&clean, sPicX, sPicY);
                gpic_fit(&mirrored, cx1 - 4 - arrowW, cy0, cx1 - 4, cy1, clean.width, clean.height, 0);
                gmirror_rows(&mirrored, sPicX, sPicY);
            }
        }
        /* on a gold focused row the chip is a dark gold: its value in white, as yellow on it was too faint */
        /* Classic: outlined alone, no drop shadow: outline, shadow and a descender ("30 fps") filled the chip's
           inside to its frame; outlined alone it keeps a clear row above and below */
        sUiNoShadow = sStyle == 0;
        gtext(GF_BODY, cx0 + 4 + arrowW, cy0, cx1 - 4 - arrowW, cy1, value,
              lit && gink_lit().r + gink_lit().g + gink_lit().b < 3 * 110 ? WHITE : YELLOW, GA_CENTRE);
        sUiNoShadow = 0;
    }
    return cx0;
}

/* The description under the rows: the picked row's name and its tip (or the page's note), filling the slots left. */
static void gdescription(const BsMenu *m, int x0, int y0, int x1, int y1, const char *tip, const char *name) {
    int top, h, lines = 0, pitch = 14, padX = gpad_x(SK_PANEL), by, tipW;
    gbox(x0, y0, x1, y1, SK_PANEL);
    gband(GF_BODY, 0, &top, &h);
    if (m->pages[0] != '\0') {
        gnum(GF_BODY, x0 + padX, y0, x1 - padX, y0 + GSLOT, m->pages, gink_dim(), GA_RIGHT);
    }
    if (y1 - y0 >= 40 && name != NULL && name[0] != '\0') {
        /* two lines: the name (heading) over the tip, the pair centred */
        int ht, hh;
        gband(GF_HEAD, 0, &ht, &hh);
        tipW = x1 - x0 - 2 * padX;
        (void) tipW;
        lines = 2;
        by = y0 + (y1 - y0 - (hh + 8 + h)) / 2;
        gtitle(GF_HEAD, x0 + padX, by, x1 - padX, by + hh, name, gink_accent(), GA_CENTRE);
        gtext(GF_BODY, x0 + padX, by + hh + 8, x1 - padX, by + hh + 8 + h, tip, HINT, GA_CENTRE);
    } else if (y1 - y0 >= 40 && tip != NULL && strchr(port_text(tip), ' ') != NULL &&
               gtext_width(GF_BODY, port_text(tip), 0) > x1 - x0 - 2 * padX) {
        /* a tip too wide for one line, on two balanced lines (split at the space nearest its middle), 8 px apart */
        const char *t = port_text(tip);
        int n = (int) strlen(t), best = -1, i;
        char a[96], b[96];
        for (i = 0; i < n; i++) {
            if (t[i] == ' ' && (best < 0 || abs(i - n / 2) < abs(best - n / 2))) {
                best = i;
            }
        }
        snprintf(a, sizeof(a), "%.*s", best, t);
        snprintf(b, sizeof(b), "%s", t + best + 1);
        by = y0 + (y1 - y0 - (2 * h + 8)) / 2;
        gtext_ex(GF_BODY, x0 + padX, by, x1 - padX, by + h, a, HINT, GA_CENTRE, 0, NULL, NULL);
        gtext_ex(GF_BODY, x0 + padX, by + h + 8, x1 - padX, by + 2 * h + 8, b, HINT, GA_CENTRE, 0, NULL, NULL);
    } else {
        gtext(GF_BODY, x0 + padX, y0, x1 - padX, y1, tip, HINT, GA_CENTRE);
    }
    (void) lines;
    (void) pitch;
}

/* Settings pages show 4 rows (menu_3ds ROWS_PER_PAGE 4 when this lands): 2 tab rows + 4 touch rows + the tip */
#define GMENU_ROWS 4
/* Each row's value chip as last drawn (x0 .. x1), for a tap's step back on the chip's left half */
static volatile short sGRowChipX0[GMENU_ROWS], sGRowChipX1[GMENU_ROWS];
static void gmenu_rows(const BsMenu *m, int x0, int x1, int firstSlot, const char **tip, const char **name) {
    int i;
    for (i = 0; i < m->rowCount && i < GMENU_ROWS; i++) {
        const BsMenuRow *row = &m->rows[i];
        int y0 = gty0(firstSlot + i), y1 = gty1(firstSlot + i), lit = i == m->picked, valueX;
        gbox(x0, y0, x1, y1, lit ? SK_LIT : SK_ROW);
        if (lit) {
            if (row->hint[0] != '\0') {
                *tip = row->hint;
            }
            *name = row->label;
        }
        valueX = gvalue(row, x0, y0, x1, y1, lit);
        sGRowChipX0[i] = (short) valueX;
        sGRowChipX1[i] = (short) (x1 - (y1 - y0 - 16) / 2 - gslant());
        sUiFlat = lit; /* Classic: the focused row's label as heavy as the others' (no drop shadow on the light fill) */
        gtitle(GF_BODY, x0 + gpad_x(SK_ROW), y0, valueX - 6, y1, row->label, lit ? gink_lit() : WHITE, GA_LEFT);
        sUiFlat = 0;
    }
}

static void gmenu(const State *s) {
    const BsMenu *m = &s->menu;
    const char *tip = m->note, *name = NULL;
    int i, rows = m->rowCount < GMENU_ROWS ? m->rowCount : GMENU_ROWS;
    /* Every look: the pages as a 4 x 2 strip of tabs, the rows full width under them. A debug build's 10 pages go 5 to
       a row (as the touch strip counts them), 54 px tabs 8 px apart centred in the frame, in the small type. */
    int perRow = m->tabCount > 8 ? 5 : 4, tabFont = m->tabCount > 8 ? GF_SM : GF_BODY;
    sTabSmall = perRow == 5;
    for (i = 0; i < m->tabCount && i < 2 * perRow; i++) {
        int lit = i == m->tab, x0, x1, y0 = gty0(i / perRow), y1 = gty1(i / perRow);
        char upper[40], shortName[40];
        if (perRow == 4) {
            x0 = gc0(i % 4);
            x1 = gc1(i % 4);
        } else {
            int w = (GX1 - GX0 - GGAP * 4) / 5, left = GX0 + (GX1 - GX0 - (5 * w + GGAP * 4)) / 2;
            x0 = left + (i % 5) * (w + GGAP);
            x1 = x0 + w;
        }
        if (sStyle == 3 && !lit) {
            log_rec('B', x0, y0, x1, y1, SK_TAB, "tab"); /* Dock's other tabs are text without a box */
            gink_box(x0, y0, x1, y1);
        } else {
            gbox(x0, y0, x1, y1, lit ? SK_TAB_LIT : SK_TAB);
        }
        {
            /* 54 px tabs take the short names (never squeezed letters): "Pad", "HUD", "Start", "Tests" in every look, and a name still
               too wide keeps its last word */
            static const char *const sShort[][2] = { { "Controls", "Pad" }, { "Race HUD", "HUD" }, { "Start-up", "Start" },
                                                     { "Test run", "Tests" } };
            const char *nm = m->tabs[i], *sp;
            unsigned j;
            {
                /* the short names on every page and tab count, so a tab keeps one name */
                for (j = 0; j < sizeof(sShort) / sizeof(sShort[0]); j++) {
                    if (strcmp(nm, sShort[j][0]) == 0) {
                        nm = sShort[j][1];
                    }
                }
            }
            nm = port_text(nm);
            sp = strrchr(nm, ' ');
            snprintf(shortName, sizeof(shortName), "%s", nm);
            upper_case(nm, upper, sizeof(upper));
            if (sp != NULL && gtext_width(tabFont, sStyle == 2 ? upper : shortName, 0) > x1 - x0 - 8 - 2 * gslant()) {
                snprintf(shortName, sizeof(shortName), "%s", sp + 1);
            }
        }
        glabel(shortName, upper, sizeof(upper)); /* the tabs in the look's label case */
        sUiNoShadow = 1; /* Classic's small tab names: outline only, as a drop shadow blurred them */
        gtext_ex(tabFont, x0 + 4 + gslant(), y0, x1 - 4 - gslant(), y1, upper,
                 lit ? gink_accent() : (Colour){ 200, 212, 240, 255 }, GA_CENTRE, 0,
                 NULL, NULL);
        sUiNoShadow = 0;
    }
    sTabSmall = 0;
    gmenu_rows(m, GX0, GX1, 2, &tip, &name);
    if (name == NULL && m->tabCount > 0) {
        name = port_text(m->tabs[m->tab]); /* no row picked: the page's name heads its note */
    }
    /* the note under the rows is the tip alone: a heading repeated the picked row's label or the lit tab's name, a
       line away from them ("Tap again to restore" twice) */
    name = NULL;
    gdescription(m, GX0, gty0(2 + rows), GX1, GY1, tip, name);
}

/* ---- The Adventure save screen ---- */
static void gprogress(const RaceHud *hud, const char *title, const char *right, int logo) {
    int i;
    char line[48];
    /* Header (slots 0-1, full width): the logo in column 0 and the three totals in columns 1-3, or the totals in thirds */
    gbox(GX0, gy0(0), GX1, gy1(1), SK_PANEL);
    if (logo && sSmallLogo.rgba != NULL) {
        gpic_fit(&sSmallLogo, gc0(0) + GPAD, gy0(0) + GPAD, gc1(0), gy1(1) - GPAD, 68, 36, 0);
    }
    for (i = 0; i < 3; i++) {
        int a, b, value = i == 0 ? hud->balloons : (i == 1 ? hud->ttAmulet : hud->wizpigAmulet);
        int total = i == 0 ? 47 : 4, ht, hh, by, blockH;
        const Picture *icon = i == 0 ? &sBalloon : (i == 1 ? &sTTAmulet[hud->ttAmulet < 0 || hud->ttAmulet > 4 ? 0 : hud->ttAmulet]
                                                           : &sWizpigAmulet[hud->wizpigAmulet < 0 || hud->wizpigAmulet > 4 ? 0 : hud->wizpigAmulet]);
        char num[16];
        if (logo) {
            a = gc0(1 + i);
            b = gc1(1 + i) - (i == 2 ? GPAD : 0);
        } else {
            gsplit(GX0 + GPAD, GX1 - GPAD, 3, i, &a, &b);
        }
        gband(GF_HEAD, 1, &ht, &hh);
        blockH = 20 + 5 + hh;
        by = gy0(0) + (gy1(1) - gy0(0) - blockH) / 2;
        gpic_fit(icon, a, by, b, by + 20, 20, 20, 0);
        snprintf(num, sizeof(num), "%d/%d", value, total);
        gnum(GF_HEAD, a, by + 25, b, by + 25 + hh, num, gink_accent(), GA_CENTRE);
    }
    /* The title on slot 2: the adventure on the left, the save file's name on the right */
    gtitle(GF_HEAD, GX0 + GPAD, gy0(2), GX1 - GPAD, gy1(2), title, gink_onbg(), GA_LEFT);
    if (right != NULL) {
        gtext_ex(GF_BODY, GX0 + GPAD, gy0(2), GX1 - GPAD, gy1(2), right, sVariant == 4 ? (Colour){ 11, 32, 80, 255 } : WHITE,
                 GA_RIGHT, 0, NULL, NULL);
    }
    for (i = 0; i < RACE_HUD_WORLDS; i++) {
        int y0 = gy0(3 + i), y1 = gy1(3 + i), balloons = hud->worldBalloons[i], bx0 = 126, bx1 = 226, k;
        Colour ink = balloons > 0 ? WHITE : gink_dim();
        gbox(GX0, y0, GX1, y1, SK_ROW);
        gtitle(GF_BODY, GX0 + gpad_x(SK_ROW), y0, bx0 - 6, y1, sWorldShortNames[i], ink, GA_LEFT); /* a row label: the look's label case */
        {
            /* the world's 8 balloons as 8 equal segments, 4 px tall, centred on the row */
            int sw = (bx1 - bx0 - 7 * 2) / 8, sy = y0 + (y1 - y0 - 4) / 2, q = sQuiet;
            Colour lit = gbroadcast() ? (Colour){ 255, 150, 20, 255 } : gcarbon() ? (Colour){ 0, 190, 200, 255 } : GOLD;
            sQuiet = 1;
            for (k = 0; k < 8; k++) {
                int a = bx0 + k * (sw + 2);
                fill(a, sy, a + sw, sy + 4, k < balloons ? lit : (Colour){ 255, 255, 255, 40 });
            }
            sQuiet = q;
            log_rec('P', bx0, sy, bx0 + 8 * sw + 14, sy + 4, 0, "segments");
        }
        {
            int tx = 238;
            const Picture *trophy = hud->worldTrophy[i] > 0 ? &sTrophy[1] : &sTrophy[0];
            const Picture *key = (hud->worldKeys & (1 << i)) ? &sKey[1] : &sKey[0];
            tx = gpic_left(trophy, tx, y0, y1, hud->worldTrophy[i] <= 0);
            gpic_left(key, tx + 4, y0, y1, !(hud->worldKeys & (1 << i)));
        }
        snprintf(line, sizeof(line), "%d/8", balloons);
        gnum(GF_BODY, 260, y0, GX1 - gpad_x(SK_ROW), y1, line, hud->worldBosses & (1 << i) ? BEHIND : ink, GA_RIGHT);
    }
}

static void gsave(const RaceHud *hud) {
    char line[48];
    snprintf(line, sizeof(line), port_text("Save file %s"), hud->name);
    gprogress(hud, hud->adventureTwo ? "ADVENTURE TWO" : "ADVENTURE", line, 1);
}

/* ---- Hooks ---- */

/* Grid layouts: pictures resized smoothly (on by default), or by the nearest pixel as before. */
void bs_set_smooth_art(int on) {
    sSmoothArt = on;
}

void bs_set_grid(int on) {
    sGrid = on;
    make_background();
    sRaceBaseMade = 0;
    sChanges = (1 << BS_AREA_COUNT) - 1;
}

int bs_grid_on(void) {
    return sGrid;
}

int bs_menu_rows(void) {
    return sGrid ? GMENU_ROWS : BS_MENU_ROWS;
}

/* The bar button under a tap at x (y in the bar, 200 and down), or -1 for the gaps and edges. */
int bs_grid_button_hit(int x) {
    int i, n = sGBtnCount;
    for (i = 0; i < n; i++) {
        if (sGBtnX0[i] >= 0 && x >= sGBtnX0[i] && x < sGBtnX1[i]) {
            return i;
        }
    }
    return -1;
}

/* Settings on the grid: two rows of tabs (4 or 5 a row), then GMENU_ROWS touch rows on the 28 px pitch. */
static int grid_menu_hit(int x, int y, int tabCount, int *index, int *stepBack) {
    int perRow = tabCount > 8 ? 5 : 4, k;
    for (k = 0; k < 2; k++) {
        if (y >= gty0(k) && y < gty0(k) + GTPITCH) {
            int col;
            for (col = 0; col < perRow; col++) {
                int x0, x1, tab = k * perRow + col;
                if (perRow == 4) {
                    x0 = gc0(col);
                    x1 = gc1(col);
                } else {
                    int w = (GX1 - GX0 - GGAP * 4) / 5, left = GX0 + (GX1 - GX0 - (5 * w + GGAP * 4)) / 2;
                    x0 = left + col * (w + GGAP);
                    x1 = x0 + w;
                }
                if (x >= x0 - GGAP / 2 && x < x1 + GGAP / 2 && tab < tabCount) {
                    *index = tab;
                    return BS_MENU_HIT_TAB;
                }
            }
            return BS_MENU_HIT_NONE;
        }
    }
    for (k = 0; k < GMENU_ROWS; k++) {
        if (y >= gty0(2 + k) && y < gty0(2 + k) + GTPITCH && x >= GX0 && x < GX1) {
            int c0 = sGRowChipX0[k], c1 = sGRowChipX1[k];
            *index = k;
            *stepBack = c1 > c0 && x >= c0 && x < (c0 + c1) / 2;
            return BS_MENU_HIT_ROW;
        }
    }
    return BS_MENU_HIT_NONE;
}

/* ======== The other race modes, the hub, the tracks menu and the home screen, on the same grid ======== */

/* Which race screens show the map, and the box it is fitted into (the panel less its padding). */
static int gshows_map(const RaceHud *hud) {
    switch (hud->mode) {
        case RACE_HUD_MODE_RACE:
            return sStyle == 3;
        case RACE_HUD_MODE_MENU_SAVE:
            return 0;
        case RACE_HUD_MODE_HUB: /* the hub's map, when the hub has one */
            return hud->mapSprite >= 0 || hud->mapBitmap != NULL;
        default:
            return 1;
    }
}

static void gmap_panel(const RaceHud *hud, int *p) {
    switch (hud->mode) {
        case RACE_HUD_MODE_RACE: /* Dock: slot 7 under it holds the best lap */
            p[0] = gc0(0); p[1] = gy0(2); p[2] = gc1(1); p[3] = gy1(6);
            break;
        case RACE_HUD_MODE_HUB:
            p[0] = gc0(2); p[1] = gy0(0); p[2] = gc1(3); p[3] = gy1(5);
            break;
        case RACE_HUD_MODE_MENU_TRACK: /* under the track's name */
            p[0] = gc0(0); p[1] = gy0(1); p[2] = gc1(1); p[3] = gy1(7);
            break;
        default:
            p[0] = gc0(2); p[1] = gy0(3); p[2] = gc1(3); p[3] = gy1(7);
            break;
    }
}

static void gmap_box(const RaceHud *hud, int *box) {
    int p[4];
    gmap_panel(hud, p);
    box[0] = p[0] + GPAD;
    box[1] = p[1] + GPAD;
    box[2] = p[2] - GPAD;
    box[3] = p[3] - GPAD;
}

/* The panels drawn once with the map (under it). */
static void gbase_panels(const RaceHud *hud) {
    int p[4];
    if (!gshows_map(hud)) {
        return;
    }
    gmap_panel(hud, p);
    gbox(p[0], p[1], p[2], p[3], SK_PANEL);
}

/* A label over a value, as a block centred in the box (or several such pairs with an even pitch). */
static void gpairs(int x0, int y0, int x1, int y1, const char *const *labels, const char *const *values,
                   const Colour *valueInk, int n, int align) {
    int lt, lh, vt, vh, k, pairH, gapIn = 3, gapOut = 9, blockH, by, padX = gpad_x(SK_PANEL);
    gband(GF_BODY, 0, &lt, &lh);
    gband(gtime_font(), 1, &vt, &vh);
    pairH = lh + gapIn + vh;
    blockH = n * pairH + (n - 1) * gapOut;
    by = y0 + (y1 - y0 - blockH) / 2;
    for (k = 0; k < n; k++) {
        int py = by + k * (pairH + gapOut);
        char up[40];
        gtext(GF_BODY, x0 + padX, py, x1 - padX, py + lh, glabel(labels[k], up, sizeof(up)), gink_label(), align);
        gnum(gtime_font(), x0 + padX, py + lh + gapIn, x1 - padX, py + pairH, values[k],
             valueInk != NULL ? valueInk[k] : WHITE, align);
    }
}

static const char *sHeroGap; /* set: shown after the race time on the hero */
static Colour sHeroGapInk;

/*
 * A hero card's small table: the title centred, then one row per fact with the label left and the value right, every
 * value in one font on one band, the rows spread evenly down the card (the time trial's table, in short).
 */
static void ghero_table(int x0, int y0, int x1, int y1, const char *title, const char *const *labels,
                        const char *const *values, const Colour *inks, int n) {
    int ht, hh, bt, bh, vf = gtime_font(), vt, vh, rh, top = y0 + GPAD, bot = y1 - GPAD, k, step, used, by;
    int lx = x0 + gpad_x(SK_CARD) + 2, rx = x1 - gpad_x(SK_CARD) - 2;
    gband(GF_HEAD, 0, &ht, &hh);
    gband(GF_BODY, 0, &bt, &bh);
    gband(vf, 1, &vt, &vh);
    rh = vh > bh ? vh : bh;
    used = hh + n * rh;
    step = (bot - top - used) / (n + 1);
    if (step > 10) {
        step = 10;
    }
    by = top + (bot - top - (used + n * step)) / 2;
    if (gtext_width(GF_HEAD, title, 0) > rx - lx) {
        /* a title wider than the card's padding allows (Classic's "Taj's challenge" came within 2 px of its sides):
           its letters 1 px closer, centred */
        const char *t = port_text(title);
        Ink k;
        int x;
        MEASURE(gdraw_tight(GF_HEAD, 0, 0, t, gink_accent()), k);
        x = x0 + (x1 - x0 - (k.x1 - k.x0)) / 2 - k.x0;
        log_rec('C', x + k.x0, by, x + k.x1, by + hh, GF_HEAD, t);
        QUIET(gdraw_tight(GF_HEAD, x, by - ht, t, gink_accent()));
    } else {
        gtitle(GF_HEAD, x0, by, x1, by + hh, title, gink_accent(), GA_CENTRE);
    }
    by += hh + step;
    for (k = 0; k < n; k++) {
        char up[40];
        gtext(GF_BODY, lx, by, rx, by + rh, glabel(labels[k], up, sizeof(up)), gink_label(), GA_LEFT);
        gnum(vf, lx, by, rx, by + rh, values[k], inks[k], GA_RIGHT);
        by += rh + step;
    }
}

/* The hero card of a mode: the place and lap (a race), or its title over a small table of the lap, time and gap. */
static void ghero(const RaceHud *hud, int x0, int y0, int x1, int y1, const char *title, const char *sub, int place) {
    const char *labels[3], *values[3];
    Colour inks[3];
    char lap[16], t[16];
    int n = 0;
    (void) sub;
    gbox(x0, y0, x1, y1, SK_CARD);
    if (place) {
        /* the place left, the lap right (as in a race) */
        gplace(hud, x0, y0, x0 + 60, y1);
        glap(hud, x0 + 60, y0, x1 - gslant(), y1, 1);
        return;
    }
    /* time trial (it has a gap to show): no lap row, the splits table's lit row is the lap; two rows fit 3 slots */
    if ((hud->laps > 0 || hud->finished) && sHeroGap == NULL) {
        int final = !hud->finished && hud->laps > 1 && hud->lap >= hud->laps;
        snprintf(lap, sizeof(lap), "%d/%d", hud->lap > hud->laps ? hud->laps : hud->lap, hud->laps);
        labels[n] = final ? "Final lap" : "Lap";
        values[n] = lap;
        inks[n++] = final ? gink_accent() : WHITE;
    }
    gformat_time(hud->raceTime, t, sizeof(t));
    labels[n] = "Time";
    values[n] = t;
    inks[n++] = WHITE;
    if (sHeroGap != NULL) {
        labels[n] = "Vs best run";
        values[n] = sHeroGap;
        inks[n++] = sHeroGapInk;
    }
    ghero_table(x0, y0, x1, y1, title, labels, values, inks, n);
}

/* The lap times (last two) and the best lap, as label/value pairs. */
static void glap_list(const RaceHud *hud, int x0, int y0, int x1, int y1) {
    int i, best = -1, timed, done, n = 0;
    char labels[3][16], values[3][16];
    const char *lp[3], *vp[3];
    timed_laps(hud, &timed, &done);
    for (i = timed > 2 ? timed - 2 : 0; i < timed && n < 2; i++, n++) {
        snprintf(labels[n], 16, port_text("Lap %d"), i + 1);
        gformat_time(hud->lapTimes[i], values[n], 16);
    }
    for (i = 0; i < done; i++) {
        if (best < 0 || hud->lapTimes[i] < best) {
            best = hud->lapTimes[i];
        }
    }
    snprintf(labels[n], 16, "%s", port_text("Best lap"));
    if (best >= 0) {
        gformat_time(best, values[n], 16);
    } else {
        snprintf(values[n], 16, "--'--\"--");
    }
    n++;
    for (i = 0; i < n; i++) {
        lp[i] = labels[i];
        vp[i] = values[i];
    }
    gbox(x0, y0, x1, y1, SK_PANEL);
    gpairs(x0, y0, x1, y1, lp, vp, NULL, n, GA_LEFT);
}

/* One row of a table on the left half: the label, a time right-aligned on one column, a delta at the right. */
static void gtable_row(int slot, const char *label, const char *time, Colour timeInk, const char *delta, Colour deltaInk,
                       int lit) {
    int x0 = gc0(0), x1 = gc1(1), y0 = gy0(slot), y1 = gy1(slot), timeR = x1 - gpad_x(SK_ROW);
    char up[40];
    label = glabel(label, up, sizeof(up));
    /* the lap being driven is a marker, not a selection (the racer's own row holds the screen's one highlight): a
       plain row, its label in the accent */
    gbox(x0, y0, x1, y1, SK_ROW);
    {
        /* a label that would come within 8 px of the time (Classic's wide timer digits: "Record lap") is set 1 px
           tighter a letter */
        int lx = x0 + gpad_x(SK_ROW), tw = time != NULL && time[0] != '\0' ? gtext_width(gtime_font(), time, 1) : 0;
        Colour lc = lit ? gink_accent() : gink_label();
        if (tw > 0 && gtext_width(GF_BODY, label, 0) > timeR - tw - 8 - lx) {
            const char *t = port_text(label);
            int bt, bh;
            Ink k;
            gband(GF_BODY, 0, &bt, &bh);
            MEASURE(gdraw_tight(GF_BODY, 0, 0, t, lc), k);
            QUIET(gdraw_tight(GF_BODY, lx - k.x0, y0 + (y1 - y0 - bh) / 2 - bt, t, lc));
            log_rec('C', lx, y0 + (y1 - y0 - bh) / 2, lx + k.x1 - k.x0, y0 + (y1 - y0 - bh) / 2 + bh, GF_BODY, t);
        } else {
            gtext(GF_BODY, lx, y0, timeR, y1, label, lc, GA_LEFT);
        }
    }
    if (time != NULL && time[0] != '\0') {
        gnum(gtime_font(), x0 + 40, y0, timeR, y1, time, timeInk, GA_RIGHT);
    }
    (void) delta;
    (void) deltaInk;
}

/* Time trial: the splits table on the left half (one row a slot), the hero and the map on the right. */
static void gtime_trial(const RaceHud *hud) {
    int i, row = 0, best = -1, laps = hud->laps > 0 ? hud->laps : 3;
    char t[16], d[16];
    static const char *const sNone = "--'--\"--";
    laps = laps > 4 ? 4 : laps;
    /* The laps: run ones with their time against the best run's lap, the current one lit, later ones empty. */
    for (i = 0; i < laps; i++) {
        int finished = i < hud->lapsTimed - 1 || (hud->finished && i < hud->lapsTimed), current = !hud->finished && i == hud->lapsTimed - 1;
        char label[16];
        Colour dInk = WHITE;
        snprintf(label, sizeof(label), port_text("Lap %d"), i + 1);
        d[0] = '\0';
        if (i < hud->lapsTimed && i < RACE_HUD_LAPS) {
            gformat_time(hud->lapTimes[i], t, sizeof(t));
            if (finished && i < hud->bestLapCount && hud->bestLaps[i] > 0) {
                int g = hud->lapTimes[i] - hud->bestLaps[i];
                format_gap(-g, d, sizeof(d));
                d[0] = g <= 0 ? '-' : '+';
                dInk = g <= 0 ? BEHIND : AHEAD;
            }
            if (finished && (best < 0 || hud->lapTimes[i] < best)) {
                best = hud->lapTimes[i];
            }
        } else {
            snprintf(t, sizeof(t), "%s", sNone);
        }
        /* a run lap's time is green when it beat the best run's lap, red when it lost */
        gtable_row(row++, label, t, i >= hud->lapsTimed ? gink_dim() : d[0] != '\0' ? dInk : WHITE, d, dInk, current);
    }
    if (best >= 0) {
        gformat_time(best, t, sizeof(t));
    }
    gtable_row(row++, "Best lap", best >= 0 ? t : sNone, best >= 0 ? WHITE : gink_dim(), NULL, WHITE, 0);
    /* Against the best run so far */
    d[0] = '\0';
    if (hud->bestGap != RACE_HUD_NO_GAP) {
        int v = (hud->bestGap < 0 ? -hud->bestGap : hud->bestGap) * 100 / 60;
        snprintf(d, sizeof(d), "%c%d.%02d", hud->bestGap <= 0 ? '-' : '+', v / 100, v % 100);
    }
    if (hud->bestTotal > 0) {
        gformat_time(hud->bestTotal, t, sizeof(t));
    }
    gtable_row(row++, "Best run", hud->bestTotal > 0 ? t : sNone, hud->bestTotal > 0 ? WHITE : gink_dim(), NULL, WHITE, 0);
    /* The course's records */
    if (hud->record >= 0) {
        gformat_time(hud->record, t, sizeof(t));
    }
    gtable_row(row++, "Record", hud->record >= 0 ? t : sNone, hud->record >= 0 ? WHITE : gink_dim(), NULL, WHITE, 0);
    if (hud->recordLap >= 0) {
        gformat_time(hud->recordLap, t, sizeof(t));
    }
    gtable_row(row++, "Record lap", hud->recordLap >= 0 ? t : sNone, hud->recordLap >= 0 ? WHITE : gink_dim(), NULL, WHITE, 0);
    if (row < 8) {
        /* T.T.'s time: shown while his ghost is out */
        if (hud->staffTime > 0) {
            gformat_time(hud->staffTime, t, sizeof(t));
        }
        gtable_row(row++, "T.T.", hud->staffTime > 0 ? t : sNone, hud->staffTime > 0 ? WHITE : gink_dim(), NULL, WHITE, 0);
    }
    /* the gap to the best run goes beside the race time on the hero */
    sHeroGap = d[0] != '\0' ? d : NULL;
    sHeroGapInk = hud->bestGap <= 0 ? BEHIND : AHEAD;
    ghero(hud, gc0(2), gy0(0), gc1(3), gy1(2), "Time trial", NULL, 0); /* 3 slots: the map gets 5 */
    sHeroGap = NULL;
    ghost_marker(&hud->ghost);
    ghost_marker(&hud->staffGhost);
}

/* A big portrait (40 px, the art doubled) with its ink centred in the box. */
static void gportrait_big(const RaceHudRacer *r, int x0, int y0, int x1, int y1, int grey) {
    int px = x0 + (x1 - x0 - 40) / 2, py = y0 + (y1 - y0 - 40) / 2;
    const Picture *p = NULL;
    if (r->special == RACE_HUD_NOT_SPECIAL && r->character >= 0 && r->character < 10) {
        p = &sPortraits[r->character];
    } else if (r->special > RACE_HUD_NOT_SPECIAL && r->special < RACE_HUD_SPECIALS && sSpecialPortraits[r->special].rgba != NULL) {
        p = &sSpecialPortraits[r->special];
    }
    if (p != NULL && p->rgba != NULL) {
        QUIET(picture_scaled(p, px, py, 2, 0));
    } else if (r->special > RACE_HUD_NOT_SPECIAL && r->special < RACE_HUD_SPECIALS) {
        char initial[2] = { (char) (sSpecialNames[r->special][0] - 'a' + 'A'), 0 };
        QUIET(fill(px, py, px + 40, py + 40, (Colour){ 0, 20, 90, 255 }));
        /* the look's large digits may have no letters: then its heading font */
        QUIET(gtext_ex(gtext_width(GF_XL, initial, 0) > 0 ? GF_XL : GF_HEAD, px, py, px + 40, py + 40, initial, WHITE, GA_CENTRE, 0,
                       NULL, NULL));
    }
    if (grey) {
        QUIET(fill(px, py, px + 40, py + 40, (Colour){ 40, 40, 70, 150 }));
    }
    log_rec('P', px, py, px + 40, py + 40, 0, "portrait");
}

/* Whether a racer has a picture: the character's, or a boss's portrait the player added (else the name stands alone). */
static int ghas_portrait(const RaceHudRacer *r) {
    if (r->special == RACE_HUD_NOT_SPECIAL) {
        return r->character >= 0 && r->character < 10 && sPortraits[r->character].rgba != NULL;
    }
    return r->special > RACE_HUD_NOT_SPECIAL && r->special < RACE_HUD_SPECIALS && sSpecialPortraits[r->special].rgba != NULL;
}

/*
 * Boss races and Taj's challenges. Left half: the two of you as cards of two slots (the leader on top), then the laps
 * as the time trial's table. Right half: the place and lap as in a race, the map under them.
 */
static void gversus(const RaceHud *hud) {
    static const char *const sShort[3] = { "Car race", "Hover race", "Plane race" };
    int i, timed, done, best = -1, laps = hud->laps > 0 ? hud->laps : 3, row = 4;
    char t[16];
    for (i = 0; i < hud->racerCount && i < 2; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        int y0 = gy0(2 * i), y1 = gy1(2 * i + 1), x0 = gc0(0), x1 = gc1(1), bt, bh, ht, hh, tx, n = 0, k, blockH, by, rightX;
        char number[4], name[24], gap[16], round[24];
        const char *lines[2];
        Colour ink = r->player ? gink_lit() : WHITE;
        gbox(x0, y0, x1, y1, r->player ? SK_LIT : SK_PANEL);
        gband(GF_BODY, 0, &bt, &bh);
        gband(GF_HEAD, 0, &ht, &hh);
        snprintf(number, sizeof(number), "%d", i + 1);
        /* the place in a cell, the portrait (when the game has one: the bosses have none), then the name and its lines */
        tx = x0 + GPAD + 12 + 6;
        if (ghas_portrait(r)) {
            gportrait(r, tx, y0, tx + 22, y1);
            tx += 22 + 6;
        }
        if (r->player) {
            /* Broadcast and Carbon write it in capitals, as their race list does */
            snprintf(name, sizeof(name), "%s", sStyle == 2 ? "YOU" : port_text("You"));
        } else if (r->special > RACE_HUD_NOT_SPECIAL && r->special < RACE_HUD_SPECIALS) {
            snprintf(name, sizeof(name), "%s", sSpecialNames[r->special]);
            name[0] = (char) (name[0] - 'a' + 'A');
        } else {
            snprintf(name, sizeof(name), "%s", racer_name(r));
        }
        if (!r->player && hud->mode == RACE_HUD_MODE_TAJ) {
            /* one line under the name: the 2-slot card holds two lines with room (the vehicle is on the top screen) */
            (void) sShort;
            lines[n++] = "Win a balloon";
        } else if (!r->player) {
            snprintf(round, sizeof(round), port_text("Round %d"), hud->round > 0 ? hud->round : 1);
            lines[n++] = round;
        }
        /* the right of the card: your item, or the rival's gap in the large digits */
        rightX = x1 - GPAD;
        if (r->player) {
            int index = hud->item * 3 + hud->itemLevel;
            if (hud->item >= 0 && index >= 0 && index < 15 && sItems[index].rgba != NULL) {
                gpic_fit(gitem_pic(index), x1 - GPAD - GITEM, y0, x1 - GPAD, y1, GITEM, GITEM, 0);
                rightX = x1 - GPAD - 36 - 8;
            }
        } else {
            format_gap(r->gap, gap, sizeof(gap));
            rightX = x1 - GPAD - gtext_width(GF_BODY, gap, 1) - 8;
        }
        blockH = hh + n * (bh + (n > 1 ? 3 : 8));
        by = y0 + (y1 - y0 - blockH) / 2;
        /* the place on the name's line */
        sUiNoShadow = r->player; /* on the lit card the outline alone: a shadow jutting past "You" read as "You." */
        gnum(GF_HEAD, x0 + GPAD, by, x0 + GPAD + 12, by + hh, number, ink, GA_CENTRE);
        gtext(GF_HEAD, tx, by, rightX, by + hh, name, ink, GA_LEFT);
        sUiNoShadow = 0;
        if (!r->player) {
            /* the rival's gap on the name's line, at the right, as the standings write gaps */
            gnum(GF_BODY, tx, by, x1 - GPAD, by + hh, gap, r->gap > 0 ? AHEAD : BEHIND, GA_RIGHT);
        }
        for (k = 0; k < n; k++) {
            int ly = by + hh + (n > 1 ? 3 : 8) + k * (bh + (n > 1 ? 3 : 8));
            gtext(GF_BODY, tx, ly, rightX, ly + bh, lines[k], k == 0 ? WHITE : gink_label(), GA_LEFT);
        }
    }
    /* the laps (run, current lit, still to run), then the best lap */
    timed_laps(hud, &timed, &done);
    laps = laps > 3 ? 3 : laps;
    for (i = 0; i < laps; i++) {
        char label[16];
        snprintf(label, sizeof(label), port_text("Lap %d"), i + 1);
        if (i < timed && i < RACE_HUD_LAPS) {
            gformat_time(hud->lapTimes[i], t, sizeof(t));
        } else {
            snprintf(t, sizeof(t), "--'--\"--");
        }
        gtable_row(row++, label, t, i < timed ? WHITE : gink_dim(), NULL, WHITE, i == timed - 1 && i >= done);
    }
    for (i = 0; i < done; i++) {
        if (best < 0 || hud->lapTimes[i] < best) {
            best = hud->lapTimes[i];
        }
    }
    if (best >= 0) {
        gformat_time(best, t, sizeof(t));
    }
    gtable_row(row, "Best lap", best >= 0 ? t : "--'--\"--", best >= 0 ? WHITE : gink_dim(), NULL, WHITE, 0);
    /* the hero as in the time trial (the place is the cards' order), the map under it */
    ghero(hud, gc0(2), gy0(0), gc1(3), gy1(2), hud->mode == RACE_HUD_MODE_TAJ ? "Taj's challenge" : "Boss race", NULL, 0);
}

/* The challenges (bananas, eggs, battle): the four racers in column 0, what you carry in column 1. */
static void gchallenge(const RaceHud *hud) {
    int i, k, place = 1;
    const RaceHudRacer *me = NULL;
    for (i = 0; i < hud->racerCount; i++) {
        if (hud->racers[i].player) {
            me = &hud->racers[i];
            place = i + 1;
        }
    }
    gbox(gc0(0), gy0(0), gc1(0), gy1(7), SK_PANEL);
    for (i = 0; i < hud->racerCount && i < 4; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        int rh = (gy1(7) - gy0(0) - 2 * GPAD) / 4, y0 = gy0(0) + GPAD + i * rh, y1 = y0 + rh, x0 = gc0(0) + 4, x1 = gc1(0) - 4;
        int bt, bh, by, line2;
        char score[12];
        /* the racer's own row: on a light (gold) fill the count and its segments take the row's dark ink, lit
           segments solid and the rest a faint shade of it, as gold on gold barely differed; Classic's lit row is blue,
           so its count stays yellow like the column's (white with its shadow read as a "Z") */
        int darkRow = r->player && gink_lit().r + gink_lit().g + gink_lit().b < 3 * 110;
        Colour scoreInk = r->player && sStyle != 0 ? gink_lit() : GOLD;
        Colour segOn = darkRow ? gink_lit() : GOLD;
        Colour segOff = darkRow ? (Colour){ 0, 0, 0, 56 } : (Colour){ 255, 255, 255, 40 };
        gband(GF_BODY, 0, &bt, &bh);
        if (r->player) {
            gbox(x0, y0 + 2, x1, y1 - 2, SK_LIT);
        }
        if (gbroadcast()) {
            /* clear of the lit slab's stripe (the same inset on every row, so they line up; 7, so "7/10" with its
               slash spaced box to box keeps 2 px inside the lit slab) */
            x0 += 7;
        }
        by = y0 + (rh - (18 + 6 + 6)) / 2;
        gportrait(r, x0 + 4, by + 1, x0 + 20, by + 17);
        if (r->out) {
            QUIET(fill(x0 + 4, by + 1, x0 + 20, by + 17, (Colour){ 40, 40, 70, 150 }));
        }
        line2 = by + 18 + 6;
        switch (hud->mode) {
            case RACE_HUD_MODE_BANANAS:
            case RACE_HUD_MODE_EGGS:
                snprintf(score, sizeof(score), "%d/%d", r->score, hud->goal > 0 ? hud->goal : (hud->mode == RACE_HUD_MODE_EGGS ? 3 : 10));
                gnum(gtext_width(GF_HEAD, score, 1) <= x1 - 6 - (x0 + 26) ? GF_HEAD : gtext_width(GF_BODY, score, 1) <= x1 - 6 - (x0 + 26) || sStyle != 0 ? GF_BODY : GF_SM, x0 + 26, by, x1 - 6, by + 18, score,
                     scoreInk, GA_LEFT);
                {
                    int n = hud->mode == RACE_HUD_MODE_EGGS ? 3 : 10, pw = (x1 - x0 - 8 - (n - 1) * 2) / n, q = sQuiet;
                    pw = pw > 10 ? 10 : pw;
                    sQuiet = 1;
                    for (k = 0; k < n; k++) {
                        int a = x0 + 4 + k * (pw + 2);
                        fill(a, line2, a + pw, line2 + 6, k < r->score ? segOn : segOff);
                    }
                    sQuiet = q;
                    log_rec('P', x0 + 4, line2, x0 + 4 + n * pw + (n - 1) * 2, line2 + 6, 0, "pips");
                }
                break;
            default: /* battle: the bananas left as health */
                snprintf(score, sizeof(score), "%d", r->score);
                gtext(GF_BODY, x0 + 26, by, x1 - 4, by + 18, r->out ? "Out" : "", AHEAD, GA_LEFT);
                if (!r->out) {
                    gnum(GF_HEAD, x0 + 26, by, x1 - 4, by + 18, score, scoreInk, GA_LEFT);
                }
                {
                    /* the bar counts the health a battle starts with (3), not 10: 2 of 3 reads two thirds full */
                    int n = r->score > 3 ? r->score : 3, pw = (x1 - x0 - 8 - (n - 1) * 2) / n, q = sQuiet;
                    sQuiet = 1;
                    for (k = 0; k < n; k++) {
                        int a = x0 + 4 + k * (pw + 2);
                        fill(a, line2, a + pw, line2 + 6, k < r->score ? segOn : segOff);
                    }
                    sQuiet = q;
                    log_rec('P', x0 + 4, line2, x0 + 4 + n * pw + (n - 1) * 2, line2 + 6, 0, "pips");
                }
                break;
        }
    }
    gbox(gc0(1), gy0(0), gc1(1), gy1(2), SK_PANEL);
    gitem(hud, gc0(1), gy0(0), gc1(1), gy1(2));
    gbox(gc0(1), gy0(3), gc1(1), gy1(7), SK_PANEL);
    {
        /* what you carry: the caption on the top line, the picture and the hint centred under it */
        int x0 = gc0(1), x1 = gc1(1), bt, bh, by, blockH, top;
        gband(GF_BODY, 0, &bt, &bh);
        top = gcaption(x0, gy0(3), x1, hud->mode == RACE_HUD_MODE_BATTLE ? "Health" : "Carrying");
        if (hud->mode == RACE_HUD_MODE_EGGS) {
            int carrying = me != NULL && me->carrying;
            /* the same words as the bananas' hint ("Take home"), wrapped the same way */
            const char *hint = carrying ? "Take home" : "Grab one";
            int lines;
            sWrapWords = 1; /* two lines in every look, as the bananas' hint */
            lines = gwrap(GF_BODY, x0, x1, 0, hint, YELLOW, 6 + bh, 1);
            sWrapWords = 0;
            blockH = 40 + 10 + bh + (lines - 1) * (6 + bh);
            by = top + (gy1(7) - GPAD - top - blockH) / 2;
            if (carrying) {
                gpic_fit(sEggWhole.rgba != NULL ? &sEggWhole : &sEggIcon, x0, by, x1, by + 40, 40, 40, 0); /* 1:1, the ROM's own pixels */
            } else {
                gtext(GF_BODY, x0, by, x1, by + 40, "No egg", gink_dim(), GA_CENTRE);
            }
            sWrapWords = 1;
            gwrap(GF_BODY, x0, x1, by + 50, hint, carrying ? YELLOW : gink_dim(), 6 + bh, 0);
            sWrapWords = 0;
        } else {
            int battle = hud->mode == RACE_HUD_MODE_BATTLE;
            const char *hint = battle ? "Last one left wins" : "Take home"; /* "them" left out: the count above says what */
            int lines;
            sWrapWords = !battle; /* "Take home" on two lines in every look, as the eggs' hint */
            lines = gwrap(GF_BODY, x0, x1, 0, hint, gink_dim(), 6 + bh, 1);
            sWrapWords = 0;
            blockH = 40 + 10 + bh + (lines - 1) * (6 + bh);
            by = top + (gy1(7) - GPAD - top - blockH) / 2;
            sNoCaption = 1;
            gbananas(hud, x0, by, x1, by + 40);
            sNoCaption = 0;
            sWrapWords = !battle;
            /* the carry hint in yellow, as the eggs' one; the battle rule stays dim */
            gwrap(GF_BODY, x0, x1, by + 50, hint, battle ? gink_dim() : YELLOW, 6 + bh, 0);
            sWrapWords = 0;
        }
    }
    {
        /* the hero: the mode's name across the top, then two halves (the place or racers left, the rule or time) */
        int ht, hh, bt, bh, lt, lh, by, blockH, rowH, hx0 = gc0(2), hx1 = gc1(3), mid = (gc0(2) + gc1(3)) / 2, gap = 8;
        RaceHud h = *hud;
        h.position = place;
        gbox(hx0, gy0(0), hx1, gy1(2), SK_CARD);
        gband(GF_HEAD, 0, &ht, &hh);
        gband(GF_BODY, 0, &bt, &bh);
        gband(GF_LG, 1, &lt, &lh);
        rowH = 26 > 2 * bh + 6 ? 26 : 2 * bh + 6;
        gap = 10;
        if (hud->mode == RACE_HUD_MODE_BATTLE) {
            int vt2, vh2, r2;
            gband(gtime_font(), 1, &vt2, &vh2);
            r2 = vh2 > bh ? vh2 : bh;
            rowH = 2 * r2 + 10;
            gap = 8;
        }
        blockH = hh + gap + rowH;
        by = gy0(0) + (gy1(2) - gy0(0) - blockH) / 2;
        if (hud->mode != RACE_HUD_MODE_BATTLE)
            gtitle(GF_HEAD, hx0, by, hx1, by + hh, hud->mode == RACE_HUD_MODE_EGGS ? "Egg collector"
                                              : hud->mode == RACE_HUD_MODE_BATTLE ? "Battle" : "Bananas",
              gink_accent(), GA_CENTRE);
        by += hh + gap;
        if (hud->mode == RACE_HUD_MODE_BATTLE) {
            char left[16], t[16];
            const char *labels[2] = { "Racers left", "Time" }, *values[2] = { left, t };
            Colour inks[2];
            inks[0] = gink_accent();
            inks[1] = WHITE;
            snprintf(left, sizeof(left), "%d/%d", hud->racersLeft, hud->racerCount);
            gformat_time(hud->raceTime, t, sizeof(t));
            ghero_table(hx0, gy0(0), hx1, gy1(2), "Battle", labels, values, inks, 2);
        } else {
            char first[24], second[24];
            int ty = by + (rowH - (2 * bh + 6)) / 2;
            gplace(&h, hx0, by, mid, by + rowH);
            snprintf(first, sizeof(first), "%s", port_text("First to"));
            snprintf(second, sizeof(second), port_text("%d wins"), hud->goal > 0 ? hud->goal : 10);
            gtext(GF_BODY, mid, ty, hx1, ty + bh, first, HINT, GA_CENTRE);
            gtext(GF_BODY, mid, ty + bh + 6, hx1, ty + 2 * bh + 6, second, HINT, GA_CENTRE);
        }
    }
}

/* The hub: the balloons and worlds on the left half, the map and the amulets on the right. */
static int ghub_cols(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = plat_getenv("DKR_HUB_COLS");
        on = e == NULL || atoi(e) != 0;
    }
    return on;
}
static void ghub(const RaceHud *hud) {
    int i, hubTrophyX = -1, hubNameX1 = 96, hubKeyGap = 3;
    char count[16], line[16];
    static const char *const sTitle[RACE_HUD_WORLDS + 1] = { "Timber's Island", "Dino Domain", "Sherbet Island",
                                                             "Snowflake Mountain", "Dragon Forest", "Future Fun Land" };
    if (!gshows_map(hud)) {
        /* no map: the Adventure screen's layout, titled with the world you are in */
        gprogress(hud, hud->world >= 0 && hud->world <= RACE_HUD_WORLDS ? sTitle[hud->world] : "", NULL, 0);
        return;
    }
    /* balloons total (slots 0-1) */
    gbox(gc0(0), gy0(0), gc1(1), gy1(1), SK_PANEL);
    {
        int lt, lh, w;
        gband(GF_LG, 1, &lt, &lh);
        snprintf(count, sizeof(count), "%d", hud->balloons);
        w = 24 + 8 + gtext_width(GF_LG, count, 1) + 8 + gtext_width(GF_BODY, "Balloons", 0);
        {
            int x = gc0(0) + (gc1(1) - gc0(0) - w) / 2, xr;
            gpic_fit(&sBalloon, x, gy0(0), x + 24, gy1(1), 24, 24, 0);
            gtext_ex(GF_LG, x + 32, gy0(0), gc1(1), gy1(1), count, gink_accent(), GA_LEFT, 1, NULL, &xr);
            gtitle(GF_BODY, xr + 8, gy0(0), gc1(1), gy1(1), "Balloons", gink_label(), GA_LEFT);
        }
    }
    if (hud->world >= 0 && hud->world <= RACE_HUD_WORLDS) {
        gtitle(GF_HEAD, GX0 + GPAD, gy0(2), gc1(1) - GPAD, gy1(2), sTitle[hud->world], gink_onbg(), GA_LEFT);
    }
    {
        /*
         * the worlds' columns, laid from the right: the count on the row's padding, then its key and trophy, the same
         * gap (cols) from the count to the key and from the trophy to the widest name, so no name runs into its trophy
         * (Sherbet Island in Barlow did). DKR_HUB_COLS=0 in env.txt: the trophy at x 100 as before.
         */
        int right = gc1(1) - gpad_x(SK_ROW), nameX = gc0(0) + gpad_x(SK_ROW), nameW = 0, cntW = 0, trW = 0, keyW = 0, j, g;
        Ink k;
        for (j = 0; j < RACE_HUD_WORLDS; j++) { /* (each measured as drawn: its ink's right edge) */
            MEASURE(gtitle(GF_BODY, nameX, gy0(3), 1000, gy1(3), sWorldShortNames[j], WHITE, GA_LEFT), k);
            nameW = k.x1 > nameW ? k.x1 : nameW;
        }
        MEASURE(gnum(GF_BODY, 120, gy0(3), right, gy1(3), "8/8", WHITE, GA_RIGHT), k);
        cntW = k.x0; /* (the count's left ink edge) */
        for (j = 0; j < 2; j++) {
            MEASURE(picture(&sTrophy[j], 0, 0), k);
            trW = k.x1 - k.x0 > trW ? k.x1 - k.x0 : trW;
            MEASURE(picture(&sKey[j], 0, 0), k);
            keyW = k.x1 - k.x0 > keyW ? k.x1 - k.x0 : keyW;
        }
        {
            int room = cntW - nameW - trW - 3 - keyW; /* the clear columns for the two gaps */
            g = room / 2;
            g = g > 7 ? 7 : g;
            /* (an odd column left over: the trophy a pixel further from its key, so the name's and count's gaps match) */
            hubKeyGap = 3 + (g < 7 && (room & 1) ? 1 : 0);
            hubTrophyX = g >= 2 && ghub_cols() ? cntW - g - keyW - hubKeyGap - trW : -1;
        }
        hubNameX1 = hubTrophyX >= 0 ? hubTrophyX - g : 96;
        if (g < 2 && ghub_cols()) {
            log_rec('E', nameX, gy0(3), right, gy1(3), g, "hub-names-too-wide");
        }
    }
    for (i = 0; i < RACE_HUD_WORLDS; i++) {
        int y0 = gy0(3 + i), y1 = gy1(3 + i), balloons = hud->worldBalloons[i], tx;
        Colour ink = balloons > 0 ? WHITE : gink_dim();
        gbox(gc0(0), y0, gc1(1), y1, SK_ROW);
        gtitle(GF_BODY, gc0(0) + gpad_x(SK_ROW), y0, hubNameX1, y1, sWorldShortNames[i], ink, GA_LEFT); /* a row label: the look's label case */
        tx = gpic_left(hud->worldTrophy[i] > 0 ? &sTrophy[1] : &sTrophy[0], hubTrophyX >= 0 ? hubTrophyX : 100, y0, y1, hud->worldTrophy[i] <= 0);
        gpic_left((hud->worldKeys & (1 << i)) ? &sKey[1] : &sKey[0], tx + (hubTrophyX >= 0 ? hubKeyGap : 3), y0, y1, !(hud->worldKeys & (1 << i)));
        snprintf(line, sizeof(line), "%d/8", balloons);
        gnum(GF_BODY, 120, y0, gc1(1) - gpad_x(SK_ROW), y1, line, hud->worldBosses & (1 << i) ? BEHIND : ink, GA_RIGHT);
    }
    /* the doors and Taj on the map */
    for (i = 0; i < hud->doorCount && i < RACE_HUD_DOORS; i++) {
        if (hud->doors[i].onMap && sMap.rgba != NULL) {
            int dx, dy;
            map_point(&hud->doors[i], &dx, &dy);
            fill(dx - 3, dy - 3, dx + 4, dy + 4, DARK);
            fill(dx - 2, dy - 2, dx + 3, dy + 3, GOLD);
        }
    }
    draw_extra_point(&hud->taj, (Colour){ 60, 90, 220, 255 });
    /* the amulets: one box each, under the map */
    for (i = 0; i < 2; i++) {
        int x0 = gc0(2 + i), x1 = gc1(2 + i), v = i == 0 ? hud->ttAmulet : hud->wizpigAmulet, w;
        const Picture *p = i == 0 ? &sTTAmulet[v < 0 || v > 4 ? 0 : v] : &sWizpigAmulet[v < 0 || v > 4 ? 0 : v];
        gbox(x0, gy0(6), x1, gy1(7), SK_PANEL);
        snprintf(count, sizeof(count), "%d/4", v);
        w = 24 + 6 + gtext_width(GF_HEAD, count, 1);
        {
            int x = x0 + (x1 - x0 - w) / 2;
            gpic_fit(p, x, gy0(6), x + 24, gy1(7), 24, 24, 0);
            gnum(GF_HEAD, x + 30, gy0(6), x1, gy1(7), count, gink_accent(), GA_LEFT);
        }
    }
}

/* The tracks menu: the map with the track's name, the records beside it. */
static void gtrack_menu(const RaceHud *hud) {
    static const char *const sVehicles[3] = { "Car", "Hover", "Plane" };
    char t1[16], t2[16], t3[16], title[32];
    /* the track's saved bests: the same terms as the time trial table's (Record, Record lap; Best lap is this run's) */
    const char *labels[3] = { "Record", "Record lap", "Vehicle" }, *values[3];
    int i;
    for (i = 0; hud->name[i] != '\0' && i < 31; i++) {
        title[i] = hud->name[i];
    }
    title[i] = '\0';
    {
        char cased[32];
        snprintf(title, sizeof(title), "%s", glabel(title, cased, sizeof(cased)));
    }
    /* the track's name in its own card over the map */
    gbox(gc0(0), gy0(0), gc1(1), gy1(0), SK_CARD);
    if (gtext_width(GF_HEAD, title, 0) > gc1(1) - gc0(0) - 2 * GPAD) {
        /* a name wider than the card's padding (Classic's "Crescent Island"): its letters 1 px closer, centred */
        Ink k;
        int x, ht, hh;
        gband(GF_HEAD, 0, &ht, &hh);
        MEASURE(gdraw_tight(GF_HEAD, 0, 0, title, gink_accent()), k);
        x = gc0(0) + (gc1(1) - gc0(0) - (k.x1 - k.x0)) / 2 - k.x0;
        log_rec('C', x + k.x0, gy0(0) + (gy1(0) - gy0(0) - hh) / 2, x + k.x1, gy0(0) + (gy1(0) - gy0(0) - hh) / 2 + hh, GF_HEAD, title);
        QUIET(gdraw_tight(GF_HEAD, x, gy0(0) + (gy1(0) - gy0(0) - hh) / 2 - ht, title, gink_accent()));
    } else {
        gtext_ex(GF_HEAD, gc0(0) + GPAD, gy0(0), gc1(1) - GPAD, gy1(0), title, gink_accent(), GA_CENTRE, 0, NULL, NULL);
    }
    if (hud->record >= 0) {
        gformat_time(hud->record, t1, sizeof(t1));
    } else {
        snprintf(t1, sizeof(t1), "--'--\"--");
    }
    if (hud->recordLap >= 0) {
        gformat_time(hud->recordLap, t2, sizeof(t2));
    } else {
        snprintf(t2, sizeof(t2), "--'--\"--");
    }
    if (hud->bestTotal > 0) {
        gformat_time(hud->bestTotal, t3, sizeof(t3));
    } else {
        snprintf(t3, sizeof(t3), "--'--\"--");
    }
    values[0] = t1;
    values[1] = t2;
    values[2] = hud->vehicle >= 0 && hud->vehicle < 3 ? sVehicles[hud->vehicle] : "--";
    /*
     * four stat tiles of two slots on the right half: the label over the value (left and right), the two lines as one
     * block centred top to bottom, 8 px apart so the tile is not an empty middle between them
     */
    for (i = 0; i < 4; i++) {
        int y0 = gy0(2 * i), y1 = gy1(2 * i + 1), x0 = gc0(2), x1 = gc1(3), bt, bh, vt, vh, by, vk = gtime_font();
        gbox(x0, y0, x1, y1, SK_PANEL);
        gband(GF_BODY, 0, &bt, &bh);
        gband(vk, 1, &vt, &vh);
        if (i == 3) {
            gpic_fit(gghost_pic(), x0 + GPAD, y0 + GPAD, x0 + GPAD + 36, y1 - GPAD, 35, 35, 0);
            x0 += GPAD + 36;
        }
        if (sStyle == 0 && i < 3) {
            int cell;
            sSprBig = 0;
            gsprite_metrics(&cell, &vt, &vh); /* Classic numbers are the timer sprites */
        }
        {
            /* 8 px between label and value: the block then fills the tile (the large font's "1" floated in its cell) */
            int lg = 8;
            by = y0 + (y1 - y0 - (bh + lg + vh)) / 2;
            gtitle(GF_BODY, x0 + GPAD, by, x1 - GPAD, by + bh, i < 3 ? labels[i] : "Your best run", gink_label(), GA_LEFT);
            if (i == 2) {
                gtext(vk == GF_LG ? GF_HEAD : vk, x0 + GPAD, by + bh + lg, x1 - GPAD, by + bh + lg + vh, values[i], WHITE, GA_RIGHT);
            } else {
                gnum(vk, x0 + GPAD, by + bh + lg, x1 - GPAD, by + bh + lg + vh, i < 3 ? values[i] : t3, WHITE, GA_RIGHT);
            }
        }
    }
}

/* The map's ink, for the layout log (the map is drawn once, with the panels under it). */
static void gmap_log(const RaceHud *hud) {
    static const unsigned char *sFor;
    static int sForW, sForH, bx0, by0, bx1, by1;
    int x, y;
    if (!gshows_map(hud) || sMap.rgba == NULL) {
        return;
    }
    if (sFor != sMap.rgba || sForW != sMap.width || sForH != sMap.height) {
        /* a new map can reuse the old one's memory: key on the size too */
        sFor = sMap.rgba;
        sForW = sMap.width;
        sForH = sMap.height;
        bx0 = by0 = 100000;
        bx1 = by1 = -1;
        for (y = 0; y < sMap.height; y++) {
            for (x = 0; x < sMap.width; x++) {
                if (sMap.rgba[(y * sMap.width + x) * 4 + 3] != 0) {
                    bx0 = x < bx0 ? x : bx0;
                    by0 = y < by0 ? y : by0;
                    bx1 = x + 1 > bx1 ? x + 1 : bx1;
                    by1 = y + 1 > by1 ? y + 1 : by1;
                }
            }
        }
    }
    if (bx1 > bx0) {
        log_rec('P', sMapX + bx0, sMapY + by0, sMapX + bx1, sMapY + by1, 0, "map");
    }
}

/* Every race screen on the grid. */
static void gdraw_race(const RaceHud *hud) {
    switch (hud->mode) {
        case RACE_HUD_MODE_TIME_TRIAL:
            gtime_trial(hud);
            break;
        case RACE_HUD_MODE_BOSS:
        case RACE_HUD_MODE_TAJ:
            gversus(hud);
            break;
        case RACE_HUD_MODE_BANANAS:
        case RACE_HUD_MODE_BATTLE:
        case RACE_HUD_MODE_EGGS:
            gchallenge(hud);
            break;
        case RACE_HUD_MODE_HUB:
            ghub(hud);
            break;
        case RACE_HUD_MODE_MENU_SAVE:
            gsave(hud);
            return;
        case RACE_HUD_MODE_MENU_TRACK:
            gtrack_menu(hud);
            return;
        default:
            if (sStyle == 3) {
                grace_dock(hud);
            } else {
                grace_tower(hud);
            }
            break;
    }
    if (gshows_map(hud) && hud->mode != RACE_HUD_MODE_HUB) {
        draw_map_markers(hud);
    }
    gmap_log(hud);
}

/* The home screen: the logo over the controls card. */
/* ---- Debug pages: the profiler's readout and the live monitor (frame graph over a page) ---- */

/* Is this word a number (right-aligned in its column by the readout's fixed-width layout)? */
static int gdebug_number(const char *w, int n) {
    int i = (w[0] == '+' || w[0] == '-' || w[0] == '(' || w[0] == 'x') && n > 1 ? 1 : 0;
    return i < n && ((w[i] >= '0' && w[i] <= '9') || (w[i] == '.' && i + 1 < n && w[i + 1] >= '0' && w[i + 1] <= '9'));
}

/*
 * One readout line. The producer lays it out for a 40 column fixed-width font; here every word keeps its column in a
 * 40 column grid across x0..x1, in the look's text font: words start on their column, numbers end on theirs (so
 * decimal points and per cent signs line up down a table), and a word never comes closer than 4 px to the one before.
 */
static unsigned long long sDebugNumEnds; /* the page's number columns: bit c set when a number ends on column c */
#define GDEBUG_GAP 10 /* the least gap between table columns, cell to cell (ink to ink: 8 or more) */
static int sDebugColX[64]; /* where each number column ends on screen (0: the even spread); see gdebug_text */

/* Does a word ending on column end head a number column (within a column either way)? Returns that column or -1. */
static int gdebug_column(int end) {
    int d;
    for (d = 0; d <= 1; d++) {
        if (end + d < 64 && (sDebugNumEnds >> (end + d) & 1)) {
            return end + d;
        }
        if (end - d >= 0 && (sDebugNumEnds >> (end - d) & 1)) {
            return end - d;
        }
    }
    return -1;
}

/* The words of a readout line: their text, columns, and whether each is a number. */
typedef struct {
    const char *w;
    int n, start, end, num;
} GWord;
static int gdebug_words(const char *line, GWord *out, int max) {
    int col = 0, count = 0;
    const char *p = line;
    while (*p != '\0' && count < max) {
        while (*p == ' ') {
            p++;
            col++;
        }
        if (*p == '\0') {
            break;
        }
        out[count].w = p;
        out[count].start = col;
        while (*p != '\0' && *p != ' ') {
            p++;
            col++;
        }
        out[count].n = (int) (p - out[count].w);
        out[count].end = col;
        out[count].num = gdebug_number(out[count].w, out[count].n);
        count++;
    }
    return count;
}

/* The readout's metric abbreviations (tris, rdp, bld...): units, not words, so lower case in every look but Broadcast
   and Carbon, whose labels are all capitals; a word that is not one is a label and takes the look's label case. */
static int gdebug_abbrev(const char *w, int n) {
    static const char *const sAbbrev[] = { "tris", "drw", "rdp", "bld", "upd", "vis", "fps", "ms" };
    unsigned j;
    for (j = 0; j < sizeof(sAbbrev) / sizeof(sAbbrev[0]); j++) {
        if ((int) strlen(sAbbrev[j]) == n && strncmp(w, sAbbrev[j], (size_t) n) == 0) {
            return 1;
        }
    }
    return 0;
}
static void gdebug_case(char *word, int size) {
    char tmp[64];
    if (gdebug_abbrev(word, (int) strlen(word))) {
        if (sStyle == 2) {
            upper_case(word, tmp, sizeof(tmp));
            snprintf(word, (size_t) size, "%s", tmp);
        }
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s", word);
    glabel(tmp, word, size);
}

static void gdebug_line(int kind, int x0, int x1, int y0, int y1, const char *line, Colour ink, int mode) {
    int header = mode == 2;
    GWord words[24];
    int count = gdebug_words(line, words, 24), i = 0, lastR = x0, space = gtext_width(kind, "a a", 0) - 2 * gtext_width(kind, "a", 0);
    /* a column is the readout's 40ths of the line, but no wider than a digit and a pixel: narrow fonts (Classic) keep
       their numbers close to their labels instead of spread across the panel (in 16ths of a pixel) */
    int cw = (x1 - x0) * 16 / 40, cwMax = (gdigit_cell(kind) + 1) * 16, last = 63;
    cw = cw > cwMax ? cwMax : cw;
    while (last > 0 && !(sDebugNumEnds >> last & 1)) {
        last--;
    }
    if (mode != 0 && last > 0) {
        /* a table spreads its columns so the last one ends on the panel's right padding, as its title's hint does */
        cw = (x1 - x0) * 16 / last;
    }
    space = space < 3 ? 3 : space;
    if (mode == 0) {
        /* prose: the line set as one string, so the font's own space, side bearings and kerning space it, as any text
           (word by word with a measured gap left the space beside a digit uneven); runs of spaces read as one */
        char flat[96];
        int n = 0, k;
        for (k = 0; line[k] != '\0' && n < (int) sizeof(flat) - 1; k++) {
            if (line[k] == ' ' && (n == 0 || flat[n - 1] == ' ')) {
                continue;
            }
            flat[n++] = line[k];
        }
        while (n > 0 && flat[n - 1] == ' ') {
            n--;
        }
        flat[n] = '\0';
        /* a sentence starts with a capital, unless it starts with a unit ("tris 1058") or continues a list */
        {
            int w0 = 0;
            while (flat[w0] != '\0' && flat[w0] != ' ' && flat[w0] != ':') {
                w0++;
            }
            /* (a graph key's colour starting a line carries on the key from the line above: "orange textures") */
            static const char *const sKey[] = { "green", "cyan", "yellow", "orange", "red", "grey" };
            int key = 0, j;
            for (j = 0; j < 6; j++) {
                key |= (int) strlen(sKey[j]) == w0 && strncmp(flat, sKey[j], (size_t) w0) == 0;
            }
            if (flat[0] >= 'a' && flat[0] <= 'z' && !gdebug_abbrev(flat, w0) && !key) {
                flat[0] = (char) (flat[0] - 'a' + 'A');
            }
        }
        if (sStyle == 2) {
            /* Broadcast and Carbon: the units in capitals, in values and prose too ("30 FPS", "33.3 MS") */
            int a = 0, b;
            while (flat[a] != '\0') {
                while (flat[a] == ' ' || flat[a] == '(' || flat[a] == ',') {
                    a++;
                }
                for (b = a; flat[b] != '\0' && flat[b] != ' ' && flat[b] != ',' && flat[b] != ')' && flat[b] != ':' && flat[b] != ';'; b++) {
                }
                if (b > a && gdebug_abbrev(flat + a, b - a)) {
                    int k;
                    for (k = a; k < b; k++) {
                        flat[k] = (char) (flat[k] - 'a' + 'A');
                    }
                }
                a = b;
                while (flat[a] != '\0' && flat[a] != ' ') {
                    a++;
                }
            }
        }
        (void) i;
        (void) lastR;
        gtext_ex(kind, x0, y0, x1, y1, flat, ink, GA_LEFT, 0, NULL, NULL);
        return;
    }
    while (i < count) {
        /* a number on a column other lines share ends on it, and so does a header word over one; the rest flows */
        int j = i, x, wpx, colEnd = -1, tabNum;
        char word[48];
        if (mode == 1 && words[i].num && i > 0 && words[i].end < 64 && (sDebugNumEnds >> words[i].end & 1)) {
            colEnd = words[i].end;
        } else if (header && !words[i].num) {
            colEnd = gdebug_column(words[i].end);
        }
        if (colEnd < 0 && !words[i].num) {
            /* words one space apart read as one phrase ("held back by renderer"), up to a number or a column head */
            while (j + 1 < count && words[j + 1].start == words[j].end + 1 && !words[j + 1].num &&
                   (!header || gdebug_column(words[j + 1].end) < 0)) {
                j++;
            }
        }
        snprintf(word, sizeof(word), "%.*s", (int) (words[j].w + words[j].n - words[i].w), words[i].w);
        /* a table's numbers in tabular digits: every column the same width of digits, so the gaps between them are
           even ("0.62 0.02 1.44" had 7 and 9 px) */
        if (!words[i].num && (mode == 2 || (mode == 1 && i == 0))) {
            gdebug_case(word, sizeof(word)); /* a column head or a row's name: a label */
        }
        tabNum = mode == 1 && words[i].num && j == i;
        wpx = gtext_width(kind, word, tabNum);
        if (colEnd >= 0) {
            x = (sDebugColX[colEnd] > 0 ? sDebugColX[colEnd] : x0 + colEnd * cw / 16) - wpx;
        } else if (i == 0) {
            x = x0; /* every line starts at the panel's padding: no indents */
        } else {
            /* after the word before: one space, or a wider gap where the readout put two or more */
            /* prose: one space; in a table, a wider gap where the readout put two or more */
            x = lastR + (mode != 0 && words[i].start - words[i - 1].end >= 2 ? 3 * space : space);
        }
        if (i > 0 && x < lastR + space) {
            x = lastR + space;
        }
        if (x + wpx > x1) {
            x = x1 - wpx; /* the last word on a long line keeps the padding */
        }
        gtext_ex(kind, x, y0, x + wpx + 8, y1, word, ink, GA_LEFT, tabNum, NULL, NULL);
        lastR = x + wpx;
        i = j + 1;
    }
}

static int gdebug_count_numbers(const char *line) {
    GWord words[24];
    int c = gdebug_words(line, words, 24), k, n = 0;
    for (k = 0; k < c; k++) {
        n += words[k].num;
    }
    return n;
}

/* The readout's lines in a panel: the title line in the accent colour, the rest in the text colour, on one even pitch. */
static void gdebug_text(const State *s, int x0, int y0, int x1, int y1) {
    int i, top, h, pitch, by, n = s->overlayLines, padX = gpad_x(SK_PANEL), modes[16];
    while (n > 0 && s->overlay[n - 1][0] == '\0') {
        n--;
    }
    if (n <= 0) {
        gbox(x0, y0, x1, y1, SK_PANEL);
        return;
    }
    gband(GF_BODY, 0, &top, &h);
    /*
     * A table: a header (a line of words only) over rows with three or more numbers each. Only its rows snap their
     * numbers to columns (where numbers end on two or more rows); every other line is prose, single spaced.
     */
    memset(modes, 0, sizeof(modes));
    for (i = 1; i + 1 < n && i < 16; i++) {
        GWord words[24];
        int c = gdebug_words(s->overlay[i], words, 24), k, r, wordsOnly = 1;
        for (k = 0; k < c; k++) {
            wordsOnly &= !words[k].num;
        }
        if (!wordsOnly || gdebug_count_numbers(s->overlay[i + 1]) < 3) {
            continue;
        }
        modes[i] = 2;
        for (r = i + 1; r < n && r < 16 && gdebug_count_numbers(s->overlay[r]) >= 3; r++) {
            modes[r] = 1;
        }
    }
    {
        int ends[64];
        memset(ends, 0, sizeof(ends));
        for (i = 1; i < n && i < 16; i++) {
            GWord words[24];
            int c = gdebug_words(s->overlay[i], words, 24), k;
            for (k = 1; k < c && modes[i] == 1; k++) {
                if (words[k].num && words[k].end < 64) {
                    ends[words[k].end]++;
                }
            }
        }
        sDebugNumEnds = 0;
        for (i = 0; i < 64; i++) {
            if (ends[i] >= 2) {
                sDebugNumEnds |= 1ULL << i;
            }
        }
    }
    {
        /*
         * The columns spread evenly across the line, except where the widest number (or heading) of a column would
         * come within 8 px of the column before: walking from the right, such a column ends 8 px before the next
         * one's widest entry ("0.62 0.02 1.44" in a wide font had 5 px gaps).
         */
        int colW[64], lx0 = x0 + padX, lx1 = x1 - padX, last = 63, c, next = -1;
        memset(colW, 0, sizeof(colW));
        memset(sDebugColX, 0, sizeof(sDebugColX));
        for (i = 1; i < n && i < 16; i++) {
            GWord words[24];
            int cnt = gdebug_words(s->overlay[i], words, 24), k;
            for (k = 0; k < cnt; k++) {
                char word[48];
                int col = -1, w;
                if (modes[i] == 1 && k > 0 && words[k].num && words[k].end < 64 && (sDebugNumEnds >> words[k].end & 1)) {
                    col = words[k].end;
                } else if (modes[i] == 2 && !words[k].num) {
                    col = gdebug_column(words[k].end);
                }
                if (col < 0) {
                    continue;
                }
                snprintf(word, sizeof(word), "%.*s", words[k].n, words[k].w);
                w = gtext_width(GF_BODY, word, modes[i] == 1);
                colW[col] = w > colW[col] ? w : colW[col];
            }
        }
        while (last > 0 && !(sDebugNumEnds >> last & 1)) {
            last--;
        }
        for (c = last; c > 0; c--) {
            if (sDebugNumEnds >> c & 1) {
                int lin = lx0 + (lx1 - lx0) * c / last;
                sDebugColX[c] = next < 0 ? lin : (lin < sDebugColX[next] - colW[next] - GDEBUG_GAP ? lin : sDebugColX[next] - colW[next] - GDEBUG_GAP);
                next = c;
            }
        }
    }
    /*
     * the lines spread over the whole panel, which always reaches the bar: a short page gets more leading, never an
     * empty band inside its panel or between the panel and the bar
     */
    pitch = (y1 - y0 - 2 * GPAD) / n;
    gbox(x0, y0, x1, y1, SK_PANEL);
    /* from the top padding on every page, so the title sits in the same place whatever the page's length */
    by = y0 + GPAD + (pitch - h) / 2;
    for (i = 0; i < n; i++) {
        /* each line is given its capitals' band, so gtext_ex centres the capitals exactly on it */
        const char *gapAt = i == 0 ? strstr(s->overlay[i], "  ") : NULL;
        if (gapAt != NULL && modes[0] == 0) {
            /* the title: the page's name on the left, its hint ("tap: page") apart from it on the right edge */
            char left[64];
            const char *right = gapAt;
            while (*right == ' ') {
                right++;
            }
            snprintf(left, sizeof(left), "%.*s", (int) (gapAt - s->overlay[i]), s->overlay[i]);
            {
                char cased[64];
                gdebug_line(GF_BODY, x0 + padX, x1 - padX, by, by + h, glabel(left, cased, sizeof(cased)), gink_accent(), 0);
            }
            gtext_ex(GF_BODY, x0 + padX, by, x1 - padX, by + h, right, gink_accent(), GA_RIGHT, 0, NULL, NULL);
            continue;
        }
        gdebug_line(GF_BODY, x0 + padX, x1 - padX, by + i * pitch, by + i * pitch + h, s->overlay[i],
                    i == 0 ? gink_accent() : WHITE, i < 16 ? modes[i] : 0);
    }
}

/*
 * DKR_PROF_TERM (default on; the owner 2026-11-15: the profiler had too many pages in the look's text settings): the
 * readout in a small fixed-width terminal font (term_font.h: 5x7 glyphs in 6x9 cells) instead of the look's fonts,
 * with no case changes or column snapping: the readout's own spaces line its columns up, as on a terminal. 47 columns
 * and 14 lines fit the panel under the graph, so the readout needs 3 pages instead of 5 (port_profile.c).
 */
#define TERM_COLS 47
#define TERM_LINES 14
#define TERM_PAD 9 /* frame to ink, left and right; the 14 lines leave 7 above their first capitals and 7 under the last */
static int gterm_on(void) {
    return plat_switch("DKR_PROF_TERM", 1) != 0;
}

/* Text in the terminal font from (x, y), the top of its cells, a colour per character (or one for all); stops at x1. */
static void gterm_text(int x, int y, int x1, const char *t, const Colour *each, Colour ink) {
    int k, i, j;
    for (k = 0; t[k] != '\0' && x + TERM_GLYPH_W - 1 <= x1; k++, x += TERM_FONT_W) {
        unsigned char c = (unsigned char) t[k];
        Colour col = each != NULL ? each[k] : ink;
        if (c < 32 || c > 126) {
            c = '?';
        }
        for (j = 0; j < TERM_GLYPH_H; j++) {
            unsigned char bits = sTermFont[c - 32][j];
            for (i = 0; i < TERM_GLYPH_W; i++) {
                if (bits & (0x10 >> i)) {
                    plot(x + i, y + j, col.r, col.g, col.b, col.a);
                }
            }
        }
    }
}

/* The ink width of text in the terminal font: whole cells but the last one's gap. */
static int gterm_width(const char *t) {
    int n = (int) strlen(t);
    return n > 0 ? n * TERM_FONT_W - 1 : 0;
}

/* The readout's lines in the terminal font: the title in the accent colour (its hint on the right edge), the graph's
   key in the graph's colours, a column head line (the line under the title, words alone) in the label colour, the rest
   in white. */
static void gterm_lines(const State *s, int x0, int y0, int x1, int y1) {
    /* the graph's key, its words in the order of sGraphColours */
    static const char *const sKey[BS_GRAPH_KINDS] = { "logic", "build", "render", "textures", "GPU", "retrace" };
    int n = s->overlayLines, i, lx0, lx1, top;
    while (n > 0 && s->overlay[n - 1][0] == '\0') {
        n--;
    }
    gbox(x0, y0, x1, y1, SK_PANEL);
    log_rec('P', x0, y0, x1, y1, 0, "terminal");
    lx0 = x0 + 1 + TERM_PAD; /* the frame is 1 px */
    lx1 = x1 - 2 - TERM_PAD; /* the last pixel ink may use */
    /* 14 lines centred on their capitals (7 rows each) and descenders: as many rows above the first as under the last */
    top = y0 + 1 + (y1 - y0 - 2 - ((TERM_LINES - 1) * TERM_FONT_H + 7)) / 2;
    for (i = 0; i < n && i < TERM_LINES; i++) {
        const char *line = s->overlay[i];
        int y = top + i * TERM_FONT_H, k, digits = 0;
        Colour each[64];
        if (i == 0) {
            const char *gap = strstr(line, "  ");
            char left[64];
            snprintf(left, sizeof(left), "%.*s", gap != NULL ? (int) (gap - line) : (int) strlen(line), line);
            gterm_text(lx0, y, lx1, left, NULL, gink_accent());
            if (gap != NULL) {
                while (*gap == ' ') {
                    gap++;
                }
                gterm_text(lx1 + 1 - gterm_width(gap), y, lx1, gap, NULL, gink_accent());
            }
            continue;
        }
        for (k = 0; line[k] != '\0' && k < 64; k++) {
            digits |= line[k] >= '0' && line[k] <= '9';
            each[k] = WHITE;
        }
        if (strncmp(line, "graph:", 6) == 0) {
            /* the key: each cause in its bar colour */
            int w = 0;
            while (line[w] != '\0' && w < 64) {
                int e = w, c;
                while (line[e] != '\0' && line[e] != ' ' && e < 64) {
                    e++;
                }
                for (c = 0; c < BS_GRAPH_KINDS; c++) {
                    if ((int) strlen(sKey[c]) == e - w && strncmp(line + w, sKey[c], (size_t) (e - w)) == 0) {
                        int q;
                        for (q = w; q < e; q++) {
                            each[q] = sGraphColours[c];
                        }
                    }
                }
                w = e;
                while (line[w] == ' ' && w < 64) {
                    w++;
                }
            }
            gterm_text(lx0, y, lx1, line, each, WHITE);
            continue;
        }
        gterm_text(lx0, y, lx1, line, NULL, i == 1 && !digits ? gink_label() : WHITE); /* a table's column heads */
    }
}

/* The frame graph in a panel: the newest frame on the right, a bar of 2 px per frame, 50 ms full height, 60 and 30 fps lines. */
static void gdebug_graph(const State *s, int x0, int y0, int x1, int y1) {
    /* 54 ms full height, so a late frame (50 ms) shows above the 30 fps line and a hitch's white cap stays inside
       the graph, while the 60 and 30 fps labels keep clear of each other */
    int gx0, gx1 = x1 - GPAD, gy0_ = y0 + 4, gy1_ = y1 - 4, frames, first, i, labelW, top, h, lx;
    int y60, y30;
    float full = 54.0f;
    gbox(x0, y0, x1, y1, SK_PANEL);
    labelW = gterm_on() ? gterm_width("60 fps") : gtext_width(GF_BODY, sStyle == 2 ? "60 FPS" : "60 fps", 0); /* the lines are frame rates: labelled so, as 60 sits under 30 (a unit: capitals in Broadcast/Carbon) */
    lx = gterm_on() ? x0 + 1 + TERM_PAD : x0 + gpad_x(SK_PANEL); /* terminal: on the readout's left edge (gterm_lines) */
    gx0 = lx + labelW + 8;
    frames = (gx1 - gx0) / 2;
    frames = frames > s->graphCount ? s->graphCount : frames;
    first = s->graphCount - frames;
    y60 = gy1_ - (int) (16.7f * (gy1_ - gy0_) / full + 0.5f);
    y30 = gy1_ - (int) (33.3f * (gy1_ - gy0_) / full + 0.5f);
    {
        int q = sQuiet;
        sQuiet = 1;
        fill(gx0, gy0_, gx1, gy1_, (Colour){ 0, 0, 20, 160 });
        for (i = 0; i < frames; i++) {
            int k = first + i, kind = s->graphKind[k] & 15, bx = gx1 - (frames - i) * 2;
            int bh = (int) (s->graphHeight[k] * 0.5f * (gy1_ - gy0_) / full + 0.5f);
            Colour c = sGraphColours[kind < BS_GRAPH_KINDS ? kind : 0];
            bh = bh > gy1_ - gy0_ ? gy1_ - gy0_ : bh < 1 ? 1 : bh;
            fill(bx, gy1_ - bh, bx + 2, gy1_, c);
            if (s->graphKind[k] & BS_GRAPH_HITCH) {
                fill(bx, gy1_ - bh, bx + 2, gy1_ - bh + 2, WHITE);
            }
        }
        fill(lx + labelW + 3, y60, gx1, y60 + 1, (Colour){ 255, 255, 255, 110 });
        fill(lx + labelW + 3, y30, gx1, y30 + 1, (Colour){ 255, 255, 255, 80 });
        sQuiet = q;
    }
    log_rec('P', gx0, gy0_, gx1, gy1_, 0, "graph");
    if (gterm_on()) {
        /* the terminal font's labels, their digits (cell rows 0-6) centred on their lines, 4 px or more apart */
        int c60 = y60, c30 = y30, need = 7 + 4 - (y60 - y30);
        if (need > 0) {
            c60 += (need + 1) / 2;
            c30 -= need / 2;
        }
        gterm_text(lx, c60 - 3, lx + labelW, "60 fps", NULL, gink_dim());
        gterm_text(lx, c30 - 3, lx + labelW, "30 fps", NULL, gink_dim());
        return;
    }
    gband(GF_BODY, 0, &top, &h);
    {
        /* each label centred on its line, unless that leaves less than 3 px between them: then both move apart
           evenly, the 60 down and the 30 up */
        Ink kl;
        int c60 = y60, c30 = y30, need, desc;
        MEASURE(gdraw_raw(GF_BODY, 0, 0, sStyle == 2 ? "30 FPS" : "30 fps", WHITE), kl);
        desc = kl.y1 - (top + h) > 0 ? kl.y1 - (top + h) : 0; /* the 30's descender (the p), above the 60's capitals */
        need = h + desc + 3 - (c60 - c30);
        if (need > 0) {
            c60 += (need + 1) / 2;
            c30 -= need / 2;
        }
        sUiCrispAll = 1;
        gtext_ex(GF_BODY, lx, c60 - h, lx + labelW, c60 + h + 1, sStyle == 2 ? "60 FPS" : "60 fps", gink_dim(), GA_RIGHT, 0,
                 NULL, NULL);
        gtext_ex(GF_BODY, lx, c30 - h, lx + labelW, c30 + h + 1, sStyle == 2 ? "30 FPS" : "30 fps", gink_dim(), GA_RIGHT, 0,
                 NULL, NULL);
        sUiCrispAll = 0;
    }
}

static void gdebug_inner(const State *s);
/*
 * The live monitor: the graph on slots 0-1, its page on slots 2-7. The profiler (no graph): the logo on slots 0-1, the
 * newest message on slot 2 (a one-line panel, so it is never half empty), the readout on slots 3-7.
 */
static void gdebug(const State *s) {
    sReadText = 1;
    gdebug_inner(s);
    sReadText = 0;
}

static void gdebug_inner(const State *s) {
    if (s->graphCount > 0) {
        gdebug_graph(s, GX0, gy0(0), GX1, gy1(1));
        if (gterm_on()) {
            gterm_lines(s, GX0, gy0(2), GX1, gy1(7));
            return;
        }
        gdebug_text(s, GX0, gy0(2), GX1, gy1(7));
        return;
    }
    if (gterm_on()) {
        gterm_lines(s, GX0, gy0(0), GX1, gy1(7)); /* no graph: the readout has the whole screen above the buttons */
        return;
    }
    if (sLogo.rgba != NULL) {
        gpic_fit(sLogoClean.rgba != NULL ? &sLogoClean : &sLogo, GX0, gy0(0), GX1, gy1(1), 160, 44, 0);
    }
    {
        const char *msg = newest_message(s);
        gbox(GX0, gy0(2), GX1, gy1(2), SK_PANEL);
        gtext_ex(GF_BODY, GX0 + gpad_x(SK_PANEL), gy0(2), GX1 - gpad_x(SK_PANEL), gy1(2), msg, gink_label(), GA_LEFT, 0,
                 NULL, NULL);
    }
    gdebug_text(s, GX0, gy0(3), GX1, gy1(7));
}

/* A key cap's side padding, both sides together: 10 in Dock and Jungle, whose wide text font needs the room for the
   12 px between the two columns, 12 elsewhere */
static int gchip_pad(void) {
    return sStyle == 3 ? 10 : 12;
}

/* A key cap's width: its name and padding, at least 20 px; every one-letter key as wide as the widest (X, B, L, A
   are one size of cap, as on the console) */
static int gkey_cap_w(char (*keys)[24], int count, int i) {
    int w = gtext_width(GF_BODY, keys[i], 0) + gchip_pad(), j;
    if (keys[i][1] == '\0') {
        for (j = 0; j < count; j++) {
            if (keys[j][0] != '\0' && keys[j][1] == '\0') {
                int o = gtext_width(GF_BODY, keys[j], 0) + gchip_pad();
                w = o > w ? o : w;
            }
        }
    }
    w = w < 20 ? 20 : w;
    return w;
}

#define GKEY_GAP 8 /* key cap to its action: 8 px, so the label reads as the cap's, not crowded by it */
static void ghome(const State *s) {
    static const struct {
        unsigned short actions;
        const char *key, *what;
    } sRows[] = {
        { 0, "Circle Pad", "Steer" },          { CTRL_N64_A, NULL, "Accelerate" },  { CTRL_N64_B, NULL, "Brake" },
        { CTRL_N64_R, NULL, "Hop / drift" },   { CTRL_N64_Z, NULL, "Use item" },    { CTRL_N64_LOOK_BACK, NULL, "Look back" },
        { CTRL_N64_CU, NULL, "Camera view" },  { CTRL_N64_CD, NULL, "Track arrows" }, { CTRL_N64_CR, NULL, "HUD toggle" },
        { 0, "START", "Pause" },
    };
    char keys[10][24];
    const char *what[10];
    char whatCased[10][24]; /* the actions are labels: the look's label case ("Use Item", "USE ITEM") */
    int i, count = 0, capW[2] = { 0, 0 }, bt, bh, pitch, by, rows, rightX;
    char build[48];
    const char *msg = PLAT_DEBUG_TOOLS ? newest_message(s) : "";
    char shortMsg[160];
    int c0 = 3, c1 = 7;
    if (msg != NULL && msg[0] != '\0') {
        /* a debug build keeps the newest message in its own panel on slot 7; the logo and the card move up a slot */
        sReadText = 1; /* measured in the font the status line is drawn in */
        /* a line with a branch (a slash) always drops its commit hash, so every look shows the same data */
        if (strchr(msg, '/') != NULL || gtext_width(GF_BODY, msg, 0) > GX1 - GX0 - 2 * gpad_x(SK_PANEL)) {
            const char *slash = strchr(msg, '/');
            if (slash != NULL) {
                const char *start = slash, *end = slash;
                while (start > msg && start[-1] != ' ') {
                    start--;
                }
                while (*end != '\0' && *end != ' ') {
                    end++;
                }
                /*
                 * the branch is shown in full in every look: the line drops the commit hash (always), then if still too long
                 * the year of the date), and the branch is cut with ".." only if it still does not fit
                 */
                {
                    char tmp[160];
                    int k, w = GX1 - GX0 - 2 * gpad_x(SK_PANEL), keep;
                    snprintf(shortMsg, sizeof(shortMsg), "%s", msg);
                    for (k = 0; shortMsg[k] != '\0'; k++) {
                        int n = 0;
                        while (n < 12 && ((shortMsg[k + n] >= '0' && shortMsg[k + n] <= '9') ||
                                          (shortMsg[k + n] >= 'a' && shortMsg[k + n] <= 'f'))) {
                            n++;
                        }
                        if (n == 7 && (k == 0 || shortMsg[k - 1] == ' ') && shortMsg[k + n] == ' ') {
                            snprintf(tmp, sizeof(tmp), "%.*s%s", k, shortMsg, shortMsg + k + n + 1);
                            snprintf(shortMsg, sizeof(shortMsg), "%s", tmp);
                            break;
                        }
                    }
                    if (gtext_width(GF_BODY, shortMsg, 0) > w) {
                        for (k = 0; shortMsg[k] != '\0'; k++) {
                            if (shortMsg[k] == ' ' && shortMsg[k + 1] == '2' && shortMsg[k + 2] == '0' && shortMsg[k + 5] == '-') {
                                snprintf(tmp, sizeof(tmp), "%.*s%s", k + 1, shortMsg, shortMsg + k + 6);
                                snprintf(shortMsg, sizeof(shortMsg), "%s", tmp);
                                break;
                            }
                        }
                    }
                    if (gtext_width(GF_BODY, shortMsg, 0) > w) {
                        snprintf(tmp, sizeof(tmp), "%s", shortMsg);
                        slash = strchr(tmp, '/');
                        start = end = slash;
                        while (start > tmp && start[-1] != ' ') {
                            start--;
                        }
                        while (*end != '\0' && *end != ' ') {
                            end++;
                        }
                        for (keep = (int) (end - start); keep > 4; keep--) {
                            snprintf(shortMsg, sizeof(shortMsg), "%.*s%.*s..%s", (int) (start - tmp), tmp, keep, start, end);
                            if (gtext_width(GF_BODY, shortMsg, 0) <= w) {
                                break;
                            }
                        }
                    }
                }
                msg = shortMsg;
            }
        }
        sReadText = 0;
        c0 = 2; /* the card needs its five slots, so the logo shrinks to 44 px here (a player build shows it 1:1) */
        c1 = 6;
        if (gtext_width(GF_BODY, msg, 0) > GX1 - GX0 - 2 * gpad_x(SK_PANEL) || strchr(msg, '?') != NULL) {
            /*
             * a question (too long for one line): it takes the card's place. The question in the heading font over the
             * rest in a panel of two slots; the logo centred above (or two balanced lines when there is no sentence break)
             */
            char first[160];
            int cut = -1, k, bt, bh, ht, hh, by, bestDiff = 100000, w = GX1 - GX0 - 2 * gpad_x(SK_PANEL), head = 0;
            for (k = 0; msg[k] != '\0' && k < 158; k++) {
                if ((msg[k] == '?' || msg[k] == '.') && msg[k + 1] == ' ') {
                    snprintf(first, sizeof(first), "%.*s", k + 1, msg);
                    if (gtext_width(GF_HEAD, first, 0) <= w && gtext_width(GF_BODY, msg + k + 2, 0) <= w) {
                        cut = k + 1;
                        head = 1;
                        break;
                    }
                }
            }
            for (k = 0; !head && msg[k] != '\0' && k < 159; k++) {
                if (msg[k] == ' ') {
                    int w1, w2;
                    snprintf(first, sizeof(first), "%.*s", k, msg);
                    w1 = gtext_width(GF_BODY, first, 0);
                    w2 = gtext_width(GF_BODY, msg + k + 1, 0);
                    if (w1 <= w && w2 <= w && abs(w1 - w2) < bestDiff) {
                        bestDiff = abs(w1 - w2);
                        cut = k;
                    }
                }
            }
            snprintf(first, sizeof(first), "%.*s", cut < 0 ? 0 : cut, msg);
            if (head) {
                char cased[160];
                snprintf(first, sizeof(first), "%s", glabel(first, cased, sizeof(cased))); /* a heading: the look's case */
            }
            /*
             * the dialog on slots 6-7, ending 4 px over the bar like every screen's content, so the question sits on the
             * verbs that answer it (no dead band between them); the logo 1:1 centred in the space above. The two lines
             * keep 8 px between them.
             */
            gbox(GX0, gy0(6), GX1, gy1(7), SK_PANEL);
            gband(GF_BODY, 0, &bt, &bh);
            gband(head ? GF_HEAD : GF_BODY, 0, &ht, &hh);
            by = gy0(6) + (gy1(7) - gy0(6) - (hh + 8 + bh)) / 2;
            gtext_ex(head ? GF_HEAD : GF_BODY, GX0, by, GX1, by + hh, first, head ? gink_accent() : WHITE, GA_CENTRE, 0, NULL, NULL);
            gtext_ex(GF_BODY, GX0, by + hh + 8, GX1, by + hh + 8 + bh, msg + cut + 1, WHITE, GA_CENTRE, 0, NULL, NULL);
            if (sLogo.rgba != NULL) {
                gpic_fit(sLogoClean.rgba != NULL ? &sLogoClean : &sLogo, GX0, GY0, GX1, gy0(6), 240, 96, 0); /* the vector at 240x96: 24 px above and below it */
            }
            return;
        } else {
            gbox(GX0, gy0(7), GX1, gy1(7), SK_PANEL);
            sReadText = 1; /* a status line is read, not glanced at (Classic: in a text font of the other looks' size) */
            gtext_ex(GF_BODY, GX0 + gpad_x(SK_PANEL), gy0(7), GX1 - gpad_x(SK_PANEL), gy1(7), msg, gink_label(), GA_LEFT, 0, NULL,
                     NULL);
            sReadText = 0;
        }
    }
    if (sLogo.rgba != NULL) {
        gpic_fit(sLogoClean.rgba != NULL ? &sLogoClean : &sLogo, GX0, gy0(0), GX1, gy1(c0 - 1), 160, c0 == 3 ? 64 : 44, 0);
    }
    for (i = 0; i < (int) (sizeof(sRows) / sizeof(sRows[0])) && count < 10; i++) {
        if (sRows[i].actions != 0) {
            controls_keys_for(sRows[i].actions, keys[count], sizeof(keys[0]));
            if (keys[count][0] == '\0') {
                continue;
            }
        } else {
            snprintf(keys[count], sizeof(keys[0]), "%s", sRows[i].key);
        }
        what[count] = glabel(sRows[i].what, whatCased[count], sizeof(whatCased[0]));
        count++;
    }
    gbox(GX0, gy0(c0), GX1, gy1(c1), SK_PANEL);
    gband(GF_BODY, 0, &bt, &bh);
    rows = (count + 1) / 2;
    /* the heading line, then the rows, all on one even pitch */
    pitch = (gy1(c1) - gy0(c0) - 2 * GPAD) / (rows + 1);
    /* the block centred, so the pitch's remainder is shared above and below (it all fell under the last row) */
    by = gy0(c0) + (gy1(c1) - gy0(c0) - pitch * (rows + 1)) / 2;
    gtitle(GF_HEAD, GX0 + gpad_x(SK_PANEL), by, GX1 - GPAD, by + pitch, port_text("Buttons"), gink_accent(), GA_LEFT); /* not "Controls": the CONTROLS button sits under it */
#ifndef __3DS__
    if (sPlayerPreview) {
        /* a player build's line: its release tag without git's distance and hash ("v0.3.0-alpha", not
           "v0.3.0-alpha-4-g5f2a9a2") */
        const char *v = plat_build_version(), *g = strstr(v, "-g");
        int n = g != NULL ? (int) (g - v) : (int) strlen(v);
        while (g != NULL && n > 0 && v[n - 1] >= '0' && v[n - 1] <= '9') {
            n--;
        }
        if (g != NULL && n > 0 && v[n - 1] == '-') {
            n--;
        }
        snprintf(build, sizeof(build), port_text("Version %.20s"), "");
        snprintf(build + strlen(build), sizeof(build) - strlen(build), "%.*s", n, v);
    } else
#endif
    if (PLAT_DEBUG_TOOLS) {
        snprintf(build, sizeof(build), port_text("Build %.24s"), plat_build_version());
    } else {
        snprintf(build, sizeof(build), port_text("Version %.20s"), plat_version());
    }
    gtext_ex(GF_BODY, GX0 + GPAD, by, GX1 - gpad_x(SK_PANEL), by + pitch, build, gink_dim(), GA_RIGHT, 0, NULL, NULL);
    for (i = 0; i < count; i++) {
        int w = gkey_cap_w(keys, count, i);
        capW[i % 2] = w > capW[i % 2] ? w : capW[i % 2];
    }
    {
        /* the right column starts on column 2, or later when a left action would come within 12 px of its caps */
        int leftEnd = 0;
        for (i = 0; i < count; i += 2) {
            int e = GX0 + gpad_x(SK_PANEL) + capW[0] + GKEY_GAP + gtext_width(GF_BODY, what[i], 0);
            leftEnd = e > leftEnd ? e : leftEnd;
        }
        rightX = leftEnd + 12 > gc0(2) + GPAD ? leftEnd + 12 : gc0(2) + GPAD;
        /* ... but never closer to the card's right edge than the left column is to its left edge */
        {
            int rightW = 0, limit;
            for (i = 1; i < count; i += 2) {
                int e = capW[1] + GKEY_GAP + gtext_width(GF_BODY, what[i], 0);
                rightW = e > rightW ? e : rightW;
            }
            limit = GX1 - gpad_x(SK_PANEL) - rightW;
            /* spread: the right column's longest action ends on the card's right padding, as the left column starts
               on its left padding (no dead band at the right of the card) */
            rightX = limit > leftEnd + 12 ? limit : leftEnd + 12;
        }
    }
    for (i = 0; i < count; i++) {
        int col = i % 2, x0 = col ? rightX : GX0 + gpad_x(SK_PANEL), ry0 = by + pitch * (1 + i / 2), ry1 = ry0 + pitch;
        int ch = pitch - GGAPY, cy0 = ry0 + (pitch - ch) / 2, w = gkey_cap_w(keys, count, i);
        Colour cap = { 235, 240, 255, 255 }, ink = { 20, 26, 70, 255 };
        /* key caps right-aligned in their column's cap width, so the actions start on one line */
        {
            int cx1 = x0 + capW[col], cx0 = cx1 - w, q = sQuiet;
            sQuiet = 1;
            if (gcarbon() || sStyle == 0) {
                gfill_edge(cx0, cy0, cx1, cy0 + ch, cap, (Colour){ 0, 0, 0, 0 });
            } else if (gbroadcast()) {
                slab(cx0, cy0, cx1, cy0 + ch, 3, cap, cap);
            } else {
                smooth_box(cx0, cy0, cx1, cy0 + ch, 5, cap, (Colour){ 210, 220, 245, 255 }, (Colour){ 0, 0, 0, 0 }, 1);
            }
            sQuiet = q;
            log_rec('B', cx0, cy0, cx1, cy0 + ch, SK_CHIP, "chip");
            gink_box(cx0, cy0, cx1, cy0 + ch);
            gtext_ex(GF_BODY, cx0, cy0, cx1, cy0 + ch, keys[i], ink, GA_CENTRE, 0, NULL, NULL);
            gtext(GF_BODY, cx1 + GKEY_GAP, ry0, (col ? GX1 : gc1(1)) - GPAD, ry1, what[i], WHITE, GA_LEFT);
        }
    }
}

/* Busy: one line in a panel where the buttons go. */
static void gpanel_text_inner(const char *t, int x1);
static void gpanel_text(const char *t, int withButton) {
    sReadText = 1;
    /* beside a button the panel takes the bar's first two thirds, up to the gap before the third */
    gpanel_text_inner(t, withButton ? GX0 + 2 * ((GX1 - GX0 - GGAP * 2) / 3) + GGAP : GX1);
    sReadText = 0;
}
static void gpanel_text_inner(const char *t, int right) {
    int len = (int) strlen(t), pct = -1, y1 = gbtn_y1();
    gbox(GX0, GBTN_Y0, right, y1, SK_PANEL);
    if (len >= 2 && t[len - 1] == '%') {
        int k = len - 2, v = 0, m = 1;
        while (k >= 0 && t[k] >= '0' && t[k] <= '9') {
            v += (t[k--] - '0') * m;
            m *= 10;
        }
        pct = m > 1 && v <= 100 ? v : -1;
    }
    if (pct < 0) {
        /* (a line wider than the panel a size smaller, never over its edge) */
        int kind = gtext_width(GF_BODY, t, 0) > right - GX0 - 2 * GPAD ? GF_SM : GF_BODY;
        gtext(kind, GX0 + GPAD, GBTN_Y0, right - GPAD, y1, t, YELLOW, GA_CENTRE);
        return;
    }
    /* a download: what is downloading on the first line; under it its percentage and the progress bar, so the panel
       shows how far it has got at a glance (the whole line with its figure did not fit the panel) */
    {
        char line[96], num[8];
        const char *sp = strrchr(t, ' ');
        int n = sp != NULL ? (int) (sp - t) : len, lt, lh, top, nw, q = sQuiet, rowY0, rowY1;
        int bx0 = GX0 + gpad_x(SK_PANEL), bx1 = right - gpad_x(SK_PANEL), by0, by1;
        Ink kn;
        snprintf(line, sizeof(line), "%.*s", n < (int) sizeof(line) - 1 ? n : (int) sizeof(line) - 1, t);
        snprintf(num, sizeof(num), "%d%%", pct);
        gband(GF_BODY, 0, &lt, &lh);
        {
            int dt, dh, gap, lk = gtext_width(GF_BODY, line, 0) > bx1 - bx0 ? GF_SM : GF_BODY, g;
            gband(GF_BODY, 1, &dt, &dh);
            /*
             * the line and the figures' row as one block, centred in the panel by its ink (outline, shadow and
             * descenders too): the same gap above and below to the pixel; the gap between the two rows 5 px (the
             * line's descenders clear of the figures), or 4 or 6 when 5 would leave an odd number of rows to share
             */
            top = 0;
            for (g = 0; g < 3; g++) {
                Ink a, b;
                gap = g == 0 ? 5 : g == 1 ? 4 : 6;
                MEASURE(gtext(lk, bx0, 0, bx1, lh, line, YELLOW, GA_LEFT), a);
                MEASURE(gnum(GF_BODY, bx0, lh + gap, bx1, lh + gap + dh, num, YELLOW, GA_LEFT), b);
                {
                    int t0 = a.y0 < b.y0 ? a.y0 : b.y0, t1 = a.y1 > b.y1 ? a.y1 : b.y1;
                    top = GBTN_Y0 + (y1 - GBTN_Y0 - (t1 - t0)) / 2 - t0;
                    if (!((y1 - GBTN_Y0 - (t1 - t0)) & 1)) {
                        break;
                    }
                }
            }
            gtext(lk, bx0, top, bx1, top + lh, line, YELLOW, GA_LEFT);
            rowY0 = top + lh + gap;
            rowY1 = rowY0 + dh;
        }
        /* the figure leads the bar, at the left (at the right it stood under the line's dots): "45%" then the bar */
        MEASURE(gnum(GF_BODY, bx0, rowY0, bx1, rowY1, num, YELLOW, GA_LEFT), kn);
        nw = kn.x1 - kn.x0;
        gnum(GF_BODY, bx0, rowY0, bx1, rowY1, num, YELLOW, GA_LEFT);
        by0 = (rowY0 + rowY1) / 2 - 2;
        by1 = by0 + 4;
        sQuiet = 1;
        fill(bx0 + nw + 6, by0, bx1, by1, (Colour){ 0, 0, 0, 110 });
        fill(bx0 + nw + 6, by0, bx0 + nw + 6 + (bx1 - bx0 - nw - 6) * pct / 100, by1, YELLOW);
        sQuiet = q;
        log_rec('P', bx0 + nw + 6, by0, bx1, by1, 0, "progress");
    }
}

#ifndef __3DS__
/* Host only: the digit and punctuation metrics of each grid font (DKR_BOTTOM_METRICS), for the number check. */
/*
 * The digit check sheet: for each text font, the digits 0-9 drawn on their own (left half) and as tabular numbers
 * (right half), each in a 15 px cell, rows 30 px apart from y 10. tools/glyphcheck.py compares the two inks digit by
 * digit, so a change to the tabular drawing that alters a glyph (a "1" turned into "]") is caught.
 */
static void gglyph_sheet(void) {
    static const int sKinds[4] = { GF_BODY, GF_HEAD, GF_SM, GF_LG };
    int r, d, q = sQuiet;
    sQuiet = 1;
    if (getenv("DKR_GLYPH_ALPHA") != NULL) {
        /* host check: every letter and digit in each type size, on a panel as the screens show them */
        static const char *const sLines[] = { "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "abcdefghijklm", "nopqrstuvwxyz",
                                              "0123456789 +-.:/% !?'\"(),;" };
        int kinds[3] = { GF_BODY, GF_HEAD, GF_SM }, y = 4, i, k;
        gbox(0, 0, BS_WIDTH, BS_HEIGHT, SK_PANEL);
        for (k = 0; k < 3; k++) {
            for (i = 0; i < 5; i++) {
                gtext(kinds[k], 6, y, 314, y + 14, sLines[i], WHITE, GA_LEFT);
                y += 15;
            }
            y += 4;
        }
        sQuiet = q;
        return;
    }
    fill(0, 0, BS_WIDTH, 198, (Colour){ 0, 0, 0, 255 });
    for (r = 0; r < 4; r++) {
        int kind = sKinds[r], y = 10 + r * 30;
        for (d = 0; d < 10; d++) {
            char one[2] = { (char) ('0' + d), 0 };
            Ink k;
            MEASURE(gdraw_raw(kind, 0, 0, one, WHITE), k);
            gdraw_raw(kind, 4 + d * 15 - k.x0, y, one, WHITE);
            MEASURE(gdraw_tab(kind, 0, 0, one, WHITE), k);
            gdraw_tab(kind, 164 + d * 15 - k.x0, y, one, WHITE);
        }
        if (gsprite_nums(kind)) {
            fill(0, y, 2, y + 2, (Colour){ 255, 0, 0, 255 }); /* tabular digits are the timer sprites here, by design */
        }
    }

    sQuiet = q;
}

void bs_grid_metrics(void) {
    int kind, d;
    for (kind = 0; kind <= GF_XL; kind++) {
        printf("font %d cell %d point %d sign %d inks:", kind, gdigit_cell(kind), gpunct_cell(kind, '.'), gpunct_cell(kind, '+'));
        for (d = 0; d < 14; d++) {
            char one[2] = { "0123456789.:+-"[d], 0 };
            Ink k;
            MEASURE(gdraw_raw(kind, 0, 0, one, WHITE), k);
            printf(" %c=%d(%d..%d)", one[0], k.x1 - k.x0, k.x0, k.x1);
        }
        printf("\n");
    }
}
#endif

/* A panel in the look's own style, for code outside the grid (the messages before the ROM is loaded). */
static void grid_panel(int x0, int y0, int x1, int y1) {
    gbox(x0, y0, x1, y1, SK_PANEL);
}
