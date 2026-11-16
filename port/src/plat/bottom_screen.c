/*
 * The 3DS bottom screen, drawn in the game's own style (see bottom_screen.h for its areas). Everything that makes it
 * look like Diddy Kong Racing comes from the player's ROM, the same way the game loads it: the menu title font
 * (BigFont), the small font of its menus (SmallFont), the colourful font (FunFont), the title logo, the blue menu
 * arrows and the ON / OFF labels of the options menu. Until the ROM is loaded (and if a picture cannot be read),
 * text is drawn with the 8x8 font the platform passes in, so the screen works before then.
 *
 * The state (messages, menu page, buttons...) is changed by the bs_set_* calls; bs_take_changes copies it for
 * drawing, so the drawing can happen on another thread while the state changes again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

#include "bottom_screen.h"
#include "setup_screen.h"
#include "plat.h"
#include "controls.h"
#include "../../../include/asset_enums.h"

/* The ROM and the asset decompressor (os_pi.c, inflate.c). */
const unsigned char *port_rom_data(void);
unsigned long port_rom_size(void);
unsigned long port_assets_lut_start(void);
unsigned long port_assets_lut_end(void);
long port_inflate(const unsigned char *in, unsigned char *out);

/*
 * The screen is written in English; everything drawn with text() and the others below goes through port_text
 * (plat.h), so it shows in French when the game is set to FRANCAIS.
 */

/******************************** Pictures and fonts from the ROM ********************************/

typedef struct Picture {
    int width, height;
    unsigned char *rgba; /* 4 bytes a pixel, row by row. NULL: not loaded. */
} Picture;

typedef struct Glyph {
    unsigned char texture; /* 0xFF: none. */
    unsigned char advance, x, y, s, t, width, height;
} Glyph;

typedef struct Font {
    int loaded;
    int fixedAdvance, spaceWidth;
    Glyph glyphs[96];
    Picture textures[32];
} Font;

enum { FONT_SMALL, FONT_BIG, FONT_FUN, FONT_MONO };

/* The looks' own fonts for small text and headings (Barlow Condensed for Broadcast, Outfit for Dock), rasterised by tools/ui_font. */
typedef struct UiGlyph {
    unsigned short advance; /* in 1/64 px: a line keeps its true width (the pen is rounded per glyph, never summed) */
    signed char x, y;
    unsigned char width, height;
    unsigned int offset;
} UiGlyph;
typedef struct UiKern {
    unsigned char a, b; /* the pair, sorted by a then b */
    short k;            /* the adjustment in 1/64 px */
} UiKern;
typedef struct UiFont {
    int space, base; /* space: in 1/64 px; base: pixels from the text's top to its baseline. */
    const UiGlyph *glyphs; /* '!' (0x21) to '~' (0x7E), then 0x7F the multiplication sign, 0x80 the minus sign. */
    const unsigned char *pixels; /* 8-bit coverage, row by row from offset. */
    const UiKern *kerns;
    int kernCount;
    int outlined; /* Classic: crisp glyphs drawn with a dark outline and a drop shadow, the face shaded top to bottom */
    int phases;   /* 4: glyphs also drawn 1/4, 1/2 and 3/4 px right, each set of 96 after the last; 1: whole pixels.
                     Then two more sets: the capitals a column wider, and the key cap letters' narrow forms (width 0: none) */
} UiFont;
#define UI_TIMES 0x7F /* "x" in a count, as the font's multiplication sign */
#define UI_MINUS 0x80 /* "-" before a number, as the font's minus sign */
#define UI_PROP0 0x81 /* 0x81-0x8A: the digits' proportional forms, for running text */
#define UI_NG 106 /* glyphs per pen phase: 0x21-0x8A */
#include "ui_font_data.h"
#include "logo_vector_data.h"
#include "term_font.h"

static Font sFonts[3];
static Picture sLogo, sSmallLogo; /* The title logo, and at half size for the menus' companions. */
static Picture sLogoClean; /* The grid's logo: the ROM's, sharpened, with a crisp edge (logo_clean) */
/* The grid's logo, redrawn as a vector and rendered at each size it is drawn (logo_vector_data.h), each shown 1:1 */
static Picture sLogoVector[4] = { { 240, 96, kLogoVector240x96 }, { 160, 64, kLogoVector160x64 }, { 110, 44, kLogoVector110x44 }, { 68, 27, kLogoVector68x27 } };
static Picture sArrowLeft, sArrowRight;
static Picture sOnOff[2][2]; /* [on, off][not chosen, chosen] */
static const unsigned char *sMono;

static unsigned long read_be32(const unsigned char *p) {
    return ((unsigned long) p[0] << 24) | ((unsigned long) p[1] << 16) | ((unsigned long) p[2] << 8) | p[3];
}

static unsigned read_be16(const unsigned char *p) {
    return (p[0] << 8) | p[1];
}

/* An asset section of the ROM, or NULL. */
static const unsigned char *section(int index, unsigned long *size) {
    const unsigned char *rom = port_rom_data();
    unsigned long start, end, base;
    if (rom == NULL) {
        return NULL;
    }
    base = port_assets_lut_end();
    start = read_be32(rom + port_assets_lut_start() + 4 + index * 4);
    end = read_be32(rom + port_assets_lut_start() + 8 + index * 4);
    if (end < start || base + end > port_rom_size()) {
        return NULL;
    }
    *size = end - start;
    return rom + base + start;
}

/* Decodes a 2D texture of the game (textures_sprites.c, load_texture) into RGBA. */
/*
 * Frame frame of a 2D texture of the game (textures_sprites.c, load_texture) into RGBA; the texture's place in its
 * sprite goes to *posX, *posY if they are not NULL.
 */
static int load_texture_frame(int id, int frame, Picture *out, int *posX, int *posY) {
    const unsigned char *table, *data, *header, *pixels;
    unsigned char *unpacked = NULL, *swapped = NULL;
    unsigned long tableSize, dataSize, start, end, need, stride;
    int width, height, format, bytes, i;

    table = section(ASSET_TEXTURES_2D_TABLE, &tableSize);
    data = section(ASSET_TEXTURES_2D, &dataSize);
    if (table == NULL || data == NULL || id < 0 || (unsigned long) (id + 2) * 4 > tableSize) {
        return 0;
    }
    start = read_be32(table + id * 4);
    end = read_be32(table + id * 4 + 4);
    if (end > dataSize || end < start + 0x20) {
        return 0;
    }
    data += start;
    width = data[0];
    height = data[1];
    format = data[2] & 0xF;
    // RGBA32, RGBA16, I8, IA16, IA8.
    bytes = format == 0 ? 4 : (format == 1 || format == 4) ? 2 : (format == 2 || format == 5) ? 1 : 0;
    // An animated texture's frames follow each other, each with its own header.
    stride = 0x20 + (unsigned long) width * height * bytes;
    need = stride * (frame + 1);
    if (bytes == 0 || width == 0 || height == 0 || frame < 0) {
        return 0;
    }
    if (data[0x1D]) {
        // Compressed: a 5 byte header (the size, little endian, then the level) and the deflate stream, which
        // unpacks to the header and the pixels.
        unsigned long size = data[0x20] | (data[0x21] << 8) | (data[0x22] << 16) | ((unsigned long) data[0x23] << 24);
        if (size < need || size > 256 * 1024) {
            return 0;
        }
        unpacked = malloc(size + 64);
        if (unpacked == NULL || port_inflate(data + 0x25, unpacked) < (long) need) {
            free(unpacked);
            return 0;
        }
        header = unpacked + stride * frame;
    } else {
        if (end - start < need) {
            return 0;
        }
        header = data + stride * frame;
    }
    pixels = header + 0x20;
    if (read_be16(header + 6) & 0x400) {
        // Interlaced (for the N64's texture memory): on every other row, each pair of 8 byte (4 for 16 bits and less)
        // blocks is swapped.
        int block = bytes == 4 ? 8 : 4, rowBytes = width * bytes, y, x, j;
        swapped = malloc(stride);
        if (swapped == NULL) {
            free(unpacked);
            return 0;
        }
        memcpy(swapped, pixels, (unsigned long) width * height * bytes);
        for (y = 1; y < height; y += 2) {
            unsigned char *row = swapped + y * rowBytes;
            for (x = 0; x + block * 2 <= rowBytes; x += block * 2) {
                for (j = 0; j < block; j++) {
                    unsigned char t = row[x + j];
                    row[x + j] = row[x + block + j];
                    row[x + block + j] = t;
                }
            }
        }
        pixels = swapped;
    }
    if (posX != NULL) {
        *posX = (signed char) header[3];
        *posY = (signed char) header[4];
    }
    out->rgba = malloc(width * height * 4);
    if (out->rgba == NULL) {
        free(unpacked);
        free(swapped);
        return 0;
    }
    out->width = width;
    out->height = height;
    for (i = 0; i < width * height; i++) {
        unsigned char *o = out->rgba + i * 4;
        unsigned v;
        switch (format) {
            case 0:
                memcpy(o, pixels + i * 4, 4);
                break;
            case 1:
                v = read_be16(pixels + i * 2);
                o[0] = ((v >> 11) & 31) * 255 / 31;
                o[1] = ((v >> 6) & 31) * 255 / 31;
                o[2] = ((v >> 1) & 31) * 255 / 31;
                o[3] = (v & 1) ? 255 : 0;
                break;
            case 2:
                o[0] = o[1] = o[2] = o[3] = pixels[i];
                break;
            case 4:
                o[0] = o[1] = o[2] = pixels[i * 2];
                o[3] = pixels[i * 2 + 1];
                break;
            default:
                // IA8: 4 bits of intensity, 4 of alpha.
                o[0] = o[1] = o[2] = (pixels[i] >> 4) * 17;
                o[3] = (pixels[i] & 15) * 17;
                break;
        }
    }
    free(unpacked);
    free(swapped);
    return 1;
}

static int load_texture(int id, Picture *out) {
    return load_texture_frame(id, 0, out, NULL, NULL);
}

/* A font of the game (font.c, load_fonts and load_font). */
static int load_font(int index, Font *font) {
    unsigned long size;
    const unsigned char *fonts = section(ASSET_FONTS, &size), *f;
    int i;

    if (fonts == NULL || size < 4 || index >= (int) read_be32(fonts) || 4 + (index + 1) * 0x400UL > size) {
        return 0;
    }
    f = fonts + 4 + index * 0x400;
    font->fixedAdvance = read_be16(f + 0x20);
    font->spaceWidth = read_be16(f + 0x24);
    for (i = 0; i < 32; i++) {
        int id = (short) read_be16(f + 0x40 + i * 2);
        if (id == -1) {
            break;
        }
        if (!load_texture(id, &font->textures[i])) {
            return 0;
        }
    }
    for (i = 0; i < 96; i++) {
        const unsigned char *c = f + 0x100 + i * 8;
        Glyph *g = &font->glyphs[i];
        g->texture = c[0];
        g->advance = c[1];
        g->x = c[2];
        g->y = c[3];
        g->s = c[4];
        g->t = c[5];
        g->width = c[6];
        g->height = c[7];
        if (g->texture != 0xFF && (g->texture >= 32 || font->textures[g->texture].rgba == NULL ||
                                   g->s + g->width > font->textures[g->texture].width ||
                                   g->t + g->height > font->textures[g->texture].height)) {
            g->texture = 0xFF;
        }
    }
    font->loaded = 1;
    return 1;
}

/* Halves a picture, averaging each 2x2 block (the menu arrows are drawn at half size). */
static void halve(Picture *p) {
    int w = p->width / 2, h = p->height / 2, x, y, c;
    unsigned char *small;
    if (p->rgba == NULL || (small = malloc(w * h * 4)) == NULL) {
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const unsigned char *a = p->rgba + ((y * 2) * p->width + x * 2) * 4, *b = a + p->width * 4;
            int alpha = a[3] + a[7] + b[3] + b[7];
            for (c = 0; c < 3; c++) {
                // Weighted by alpha, so transparent texels do not darken the edges.
                int sum = a[c] * a[3] + a[4 + c] * a[7] + b[c] * b[3] + b[4 + c] * b[7];
                small[(y * w + x) * 4 + c] = alpha ? sum / alpha : 0;
            }
            small[(y * w + x) * 4 + 3] = alpha / 4;
        }
    }
    free(p->rgba);
    p->rgba = small;
    p->width = w;
    p->height = h;
}

/* Cuts a picture down to the texels it shows. */
static void crop(Picture *p) {
    int x0 = p->width, y0 = p->height, x1 = -1, y1 = -1, x, y;
    unsigned char *cut;
    for (y = 0; p->rgba != NULL && y < p->height; y++) {
        for (x = 0; x < p->width; x++) {
            if (p->rgba[(y * p->width + x) * 4 + 3] != 0) {
                x0 = x < x0 ? x : x0;
                x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0;
                y1 = y > y1 ? y : y1;
            }
        }
    }
    if (x1 < 0 || (cut = malloc((x1 - x0 + 1) * (y1 - y0 + 1) * 4)) == NULL) {
        return;
    }
    for (y = y0; y <= y1; y++) {
        memcpy(cut + (y - y0) * (x1 - x0 + 1) * 4, p->rgba + (y * p->width + x0) * 4, (x1 - x0 + 1) * 4);
    }
    free(p->rgba);
    p->rgba = cut;
    p->width = x1 - x0 + 1;
    p->height = y1 - y0 + 1;
}

/* Scales a picture down to height (keeping its shape), each texel the alpha-weighted average of those it covers. */
static void shrink_to_height(Picture *p, int height) {
    int w, x, y, c;
    unsigned char *small;
    if (p->rgba == NULL || height <= 0 || p->height <= height) {
        return;
    }
    w = (p->width * height + p->height / 2) / p->height;
    if (w < 1 || (small = malloc(w * height * 4)) == NULL) {
        return;
    }
    for (y = 0; y < height; y++) {
        int sy0 = y * p->height / height, sy1 = (y + 1) * p->height / height;
        for (x = 0; x < w; x++) {
            int sx0 = x * p->width / w, sx1 = (x + 1) * p->width / w, sum[3] = { 0, 0, 0 }, alpha = 0, n = 0, i, j;
            for (j = sy0; j < sy1; j++) {
                for (i = sx0; i < sx1; i++) {
                    const unsigned char *t = p->rgba + (j * p->width + i) * 4;
                    for (c = 0; c < 3; c++) {
                        sum[c] += t[c] * t[3];
                    }
                    alpha += t[3];
                    n++;
                }
            }
            for (c = 0; c < 3; c++) {
                small[(y * w + x) * 4 + c] = alpha ? sum[c] / alpha : 0;
            }
            small[(y * w + x) * 4 + 3] = n ? alpha / n : 0;
        }
    }
    free(p->rgba);
    p->rgba = small;
    p->width = w;
    p->height = height;
}

/*
 * The ROM logo is soft (an N64 texture made to be filtered). The grid shows it cleaned, made from the ROM's own pixels
 * at start (nothing of it is stored): its colours sharpened against their opaque neighbours (unsharp mask, 0.8), and
 * its edge redrawn crisp: the alpha sampled 8 x 8 per pixel (bilinear), each sample cut at half, then averaged.
 */
static void logo_clean(void) {
    const int w = 160, h = 64, n = 8;
    int x, y, c;
    const unsigned char *src = sLogo.rgba;
    unsigned char *out;
    if (src == NULL || (out = malloc((size_t) w * h * 4)) == NULL) {
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const unsigned char *p = src + (y * w + x) * 4;
            int sum[3] = { 0, 0, 0 }, wsum = 0, i, j, cover = 0;
            for (j = -1; j <= 1; j++) {
                for (i = -1; i <= 1; i++) {
                    int xx = x + i < 0 ? 0 : x + i >= w ? w - 1 : x + i, yy = y + j < 0 ? 0 : y + j >= h ? h - 1 : y + j;
                    const unsigned char *q = src + (yy * w + xx) * 4;
                    int k = (i ? 1 : 2) * (j ? 1 : 2);
                    if (q[3] >= 128) {
                        for (c = 0; c < 3; c++) {
                            sum[c] += q[c] * k;
                        }
                        wsum += k;
                    }
                }
            }
            for (c = 0; c < 3; c++) {
                int v = p[c];
                if (wsum > 0) {
                    v = p[c] + (p[c] - sum[c] / wsum) * 4 / 5;
                }
                out[(y * w + x) * 4 + c] = (unsigned char) (v < 0 ? 0 : v > 255 ? 255 : v);
            }
            for (j = 0; j < n; j++) {
                for (i = 0; i < n; i++) {
                    float fx = x + (i + 0.5f) / n - 0.5f, fy = y + (j + 0.5f) / n - 0.5f, ax, ay, v;
                    int x0 = (int) floorf(fx), y0 = (int) floorf(fy), x1, y1;
                    ax = fx - x0;
                    ay = fy - y0;
                    x1 = x0 + 1 >= w ? w - 1 : x0 + 1;
                    y1 = y0 + 1 >= h ? h - 1 : y0 + 1;
                    x0 = x0 < 0 ? 0 : x0;
                    y0 = y0 < 0 ? 0 : y0;
                    v = (src[(y0 * w + x0) * 4 + 3] * (1 - ax) + src[(y0 * w + x1) * 4 + 3] * ax) * (1 - ay) +
                        (src[(y1 * w + x0) * 4 + 3] * (1 - ax) + src[(y1 * w + x1) * 4 + 3] * ax) * ay;
                    cover += v >= 128.0f;
                }
            }
            out[(y * w + x) * 4 + 3] = (unsigned char) (cover * 255 / (n * n));
        }
    }
    sLogoClean.width = w;
    sLogoClean.height = h;
    sLogoClean.rgba = out;
}

/* The title logo is ten strips of 16x64. */
static void load_logo(void) {
    Picture strip;
    int i, y;
    sLogo.rgba = calloc(160 * 64, 4);
    if (sLogo.rgba == NULL) {
        return;
    }
    sLogo.width = 160;
    sLogo.height = 64;
    for (i = 0; i < 10; i++) {
        if (!load_texture(ASSET_TEX2D_MENU_TITLE_0 + i, &strip) || strip.width != 16 || strip.height != 64) {
            free(sLogo.rgba);
            sLogo.rgba = NULL;
            return;
        }
        for (y = 0; y < 64; y++) {
            memcpy(sLogo.rgba + (y * 160 + i * 16) * 4, strip.rgba + y * 16 * 4, 16 * 4);
        }
        free(strip.rgba);
    }
    logo_clean();
    sSmallLogo = sLogo;
    if ((sSmallLogo.rgba = malloc(160 * 64 * 4)) != NULL) {
        memcpy(sSmallLogo.rgba, sLogoClean.rgba != NULL ? sLogoClean.rgba : sLogo.rgba, 160 * 64 * 4); /* from the clean one */
        halve(&sSmallLogo);
    }
}

static unsigned sChanges;
static void load_race_art(void);
static void load_setup_art(void);

void bs_load_assets(void) {
    static const int sFontAssets[3] = { ASSET_FONTS_SMALLFONT, ASSET_FONTS_BIGFONT, ASSET_FONTS_FUNFONT };
    int i;
    for (i = 0; i < 3; i++) {
        if (!load_font(sFontAssets[i], &sFonts[i])) {
            plat_log("bottom screen: font %d not loaded", sFontAssets[i]);
        }
    }
    load_logo();
    if (load_texture(ASSET_TEX2D_MENU_ARROWBLUELEFT, &sArrowLeft)) {
        halve(&sArrowLeft);
    }
    if (load_texture(ASSET_TEX2D_MENU_ARROWBLUERIGHT, &sArrowRight)) {
        halve(&sArrowRight);
    }
    load_texture(ASSET_TEX2D_MENU_LABELONOPTIONNONSELECTED, &sOnOff[0][0]);
    load_texture(ASSET_TEX2D_MENU_LABELONOPTIONSELECTED, &sOnOff[0][1]);
    load_texture(ASSET_TEX2D_MENU_LABELOFFOPTIONNONSELECTED, &sOnOff[1][0]);
    load_texture(ASSET_TEX2D_MENU_LABELOFFOPTIONSELECTED, &sOnOff[1][1]);
    load_race_art();
    load_setup_art();
    sChanges = (1 << BS_AREA_COUNT) - 1;
}

/******************************** Drawing ********************************/

typedef struct Colour {
    unsigned char r, g, b, a;
} Colour;

static const Colour WHITE = { 255, 255, 255, 255 };
static const Colour YELLOW = { 255, 226, 60, 255 };
static const Colour HINT = { 150, 200, 255, 255 };
static const Colour DIM = { 185, 195, 225, 255 };
static const Colour BOX_FILL = { 0, 8, 40, 120 };
static const Colour BOX_EDGE = { 140, 180, 255, 150 };

static unsigned short *sImage;
static int sClipTop, sClipEnd; /* The rows of the area being drawn. */
static unsigned short sBackground[BS_WIDTH * BS_HEIGHT];
static int sBackgroundMade;

static unsigned short rgb565(int r, int g, int b) {
    return (unsigned short) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Scratch: a layout log (boxes, text and number ink boxes) for measuring alignment on the PC build. */
typedef struct { char kind; int x0, y0, x1, y1, r; char s[40]; } LogRec;
static LogRec sLogRecs[4000];
static int sLogN, sLogOn, sTrack, sInkX0, sInkY0, sInkX1, sInkY1;
static int sSolidX0 = 99999, sSolidX1 = -99999; /* a dry run's solid ink (alpha half and up): its columns */
static int sDryInk, sQuiet; /* grid: measuring ink without drawing; a helper's parts are not logged */
void bs_log_enable(int on) { sLogOn = on; }
static int sLogResetBase;
void bs_log_reset(void) { sLogN = 0; sLogResetBase = 1; }
static void log_rec(char kind, int x0, int y0, int x1, int y1, int r, const char *str) {
    if (!sLogOn || sClipEnd < 0 || sLogN >= 4000 || sDryInk || sQuiet) {
        return;
    }
    sLogRecs[sLogN].kind = kind;
    sLogRecs[sLogN].x0 = x0; sLogRecs[sLogN].y0 = y0; sLogRecs[sLogN].x1 = x1; sLogRecs[sLogN].y1 = y1; sLogRecs[sLogN].r = r;
    snprintf(sLogRecs[sLogN].s, sizeof(sLogRecs[sLogN].s), "%s", str != NULL ? str : "");
    sLogN++;
}
void bs_log_write(const char *path) {
    FILE *f = fopen(path, "w");
    int i;
    if (f == NULL) {
        return;
    }
    for (i = 0; i < sLogN; i++) {
        fprintf(f, "%c %d %d %d %d %d %s\n", sLogRecs[i].kind, sLogRecs[i].x0, sLogRecs[i].y0, sLogRecs[i].x1, sLogRecs[i].y1, sLogRecs[i].r, sLogRecs[i].s[0] ? sLogRecs[i].s : "-");
    }
    fclose(f);
}
static void ink_begin(void) { sInkX0 = sInkY0 = 9999; sInkX1 = sInkY1 = -1; sTrack = 1; }
static int ink_end(void) { sTrack = 0; return sInkX1 >= 0; }

static void plot(int x, int y, int r, int g, int b, int a) {
    unsigned short *p, d;
    int dr, dg, db;
    if (sDryInk) {
        if (r + g + b > 20 && a > 12 && y >= sClipTop && y < sClipEnd) { /* rows: MEASURE opens them all */
            if (a >= 128) {
                if (x < sSolidX0) sSolidX0 = x;
                if (x > sSolidX1) sSolidX1 = x;
            }
            if (x < sInkX0) sInkX0 = x;
            if (x > sInkX1) sInkX1 = x;
            if (y < sInkY0) sInkY0 = y;
            if (y > sInkY1) sInkY1 = y;
        }
        return;
    }
    if (x < 0 || x >= BS_WIDTH || y < sClipTop || y >= sClipEnd || a <= 0) {
        return;
    }
    if (sTrack && r + g + b > 20 && a > 12) {
        if (x < sInkX0) sInkX0 = x;
        if (x > sInkX1) sInkX1 = x;
        if (y < sInkY0) sInkY0 = y;
        if (y > sInkY1) sInkY1 = y;
    }
    p = &sImage[y * BS_WIDTH + x];
    if (a >= 255) {
        *p = rgb565(r, g, b);
        return;
    }
    d = *p;
    dr = (d >> 11) << 3;
    dg = ((d >> 5) & 63) << 2;
    db = (d & 31) << 3;
    *p = rgb565(dr + (r - dr) * a / 255, dg + (g - dg) * a / 255, db + (b - db) * a / 255);
}

static void fill(int x0, int y0, int x1, int y1, Colour c) {
    int x, y;
    for (y = y0 < sClipTop ? sClipTop : y0; y < y1 && y < sClipEnd; y++) {
        for (x = x0; x < x1; x++) {
            plot(x, y, c.r, c.g, c.b, c.a);
        }
    }
}

/* A box with rounded corners and a one pixel edge, like the game's dialogue boxes. */
static void box_classic(int x0, int y0, int x1, int y1, Colour inside, Colour edge) {
    int y;
    log_rec('B', x0, y0, x1, y1, 0, "classic");
    fill(x0 + 1, y0 + 1, x1 - 1, y1 - 1, inside);
    fill(x0 + 2, y0, x1 - 2, y0 + 1, edge);
    fill(x0 + 2, y1 - 1, x1 - 2, y1, edge);
    for (y = y0 + 2; y < y1 - 2; y++) {
        plot(x0, y, edge.r, edge.g, edge.b, edge.a);
        plot(x1 - 1, y, edge.r, edge.g, edge.b, edge.a);
    }
    plot(x0 + 1, y0 + 1, edge.r, edge.g, edge.b, edge.a);
    plot(x1 - 2, y0 + 1, edge.r, edge.g, edge.b, edge.a);
    plot(x0 + 1, y1 - 2, edge.r, edge.g, edge.b, edge.a);
    plot(x1 - 2, y1 - 2, edge.r, edge.g, edge.b, edge.a);
}

/******************************** Looks ********************************/

/*
 * The bottom screen's look (a setting, port_race_hud.c): the game's own boxes (Classic), or one of three newer
 * styles from the mockups, which draw every box, button and tab with smooth shapes instead. They keep the Classic
 * layout, so taps land in the same places.
 */
static int sStyle;
static int sNoItemLabel = 1;
static int sTipCard = 1;
#define STRIP_CHIP_H 17
static int wrapped_centred(int x0, int x1, int y, const char *s, Colour colour);
static int sFixTexts = 1;
static int sDockOutline = 1;
static int line_pitch(void);
static int sGrid; /* O1h redo: the grid layouts (bottom_grid.inc) */
static void grid_panel(int x0, int y0, int x1, int y1); /* bottom_grid.inc */
static int sSmoothArt = 1; /* grid: pictures resized smoothly (DKR_BOTTOM_SMOOTH=0 for the old nearest-pixel resize) */
enum { SK_PANEL, SK_ROW, SK_LIT, SK_CHIP, SK_CARD, SK_TAB, SK_TAB_LIT, SK_CUR };
static void gbox(int x0, int y0, int x1, int y1, int kind);
static int sVariant; /* scratch mockups: 0 none, 1 Carbon (on Broadcast), 2 Jungle (Dock), 3 Sunset (Glass), 4 Daylight (Glass) */
/* Lettering nudges (pixels down) found on a real screen, by style: tabs, then the buttons' fun-font text. */
static const int sTabNudge[4] = { 2, 2, 1, 1 };
static const int sButtonNudge[4] = { 0, -1, -1, 0 };
static int sRaceBaseMade;

static Colour mix(Colour a, Colour b, int t, int span) {
    Colour c;
    if (span < 1) {
        return a;
    }
    c.r = (unsigned char) (a.r + (b.r - a.r) * t / span);
    c.g = (unsigned char) (a.g + (b.g - a.g) * t / span);
    c.b = (unsigned char) (a.b + (b.b - a.b) * t / span);
    c.a = (unsigned char) (a.a + (b.a - a.a) * t / span);
    return c;
}

static void plot_c(int x, int y, Colour c, int cover) {
    int a = c.a * cover / 255;
    if (a > 0) {
        plot(x, y, c.r, c.g, c.b, a);
    }
}

/* How much of the pixel (x, y) a rounded rectangle covers, 0 to 255 (one pixel anti-aliased edge). */
static int round_cover(int x, int y, int x0, int y0, int x1, int y1, int r) {
    float px = x + 0.5f, py = y + 0.5f, dx, dy, d;
    if (x < x0 || x >= x1 || y < y0 || y >= y1) {
        return 0;
    }
    dx = (float) (x0 + r) - px;
    if (px > x1 - r) {
        dx = px - (float) (x1 - r);
    }
    dy = (float) (y0 + r) - py;
    if (py > y1 - r) {
        dy = py - (float) (y1 - r);
    }
    if (dx <= 0 || dy <= 0) {
        return 255;
    }
    d = sqrtf(dx * dx + dy * dy) - (float) r + 0.5f;
    return d <= 0 ? 255 : d >= 1 ? 0 : (int) ((1 - d) * 255);
}

/* A smooth rounded rectangle filled from top to bottom, with an optional edge of the given width (1 or 2). */
static void smooth_box(int x0, int y0, int x1, int y1, int r, Colour top, Colour bottom, Colour edge, int edgeWidth) {
    int x, y;
    log_rec('B', x0, y0, x1, y1, r, "smooth");
    if (sGrid) {
        r = 0; /* the grid's design standard: rectangular boxes in every look (Glass, Dock and the looks built on them) */
    }
    if (r > (x1 - x0) / 2) {
        r = (x1 - x0) / 2;
    }
    if (r > (y1 - y0) / 2) {
        r = (y1 - y0) / 2;
    }
    for (y = y0 < sClipTop ? sClipTop : y0; y < y1 && y < sClipEnd; y++) {
        Colour c = mix(top, bottom, y - y0, y1 - y0 - 1);
        for (x = x0; x < x1; x++) {
            int outer = round_cover(x, y, x0, y0, x1, y1, r);
            if (outer == 0) {
                continue;
            }
            if (c.a > 0) {
                plot_c(x, y, c, outer);
            }
            if (edge.a > 0) {
                int inner = round_cover(x, y, x0 + edgeWidth, y0 + edgeWidth, x1 - edgeWidth, y1 - edgeWidth,
                                        r > edgeWidth ? r - edgeWidth : 0);
                plot_c(x, y, edge, outer * (255 - inner) / 255);
            }
        }
    }
}

static void round_disc(int cx, int cy, int r, Colour c) {
    smooth_box(cx - r, cy - r, cx + r, cy + r, r, c, c, (Colour){ 0, 0, 0, 0 }, 1);
}

static void bar(int x0, int y0, int x1, int y1, Colour c) {
    smooth_box(x0, y0, x1, y1, 1, c, c, (Colour){ 0, 0, 0, 0 }, 1);
}

/* A slanted slab (leaning right, like a broadcast graphic), filled top to bottom, edges anti-aliased. */
static void slab(int x0, int y0, int x1, int y1, int slant, Colour top, Colour bottom) {
    int x, y, h = y1 - y0;
    log_rec('B', x0, y0, x1, y1, slant, "slab");
    int carbon = sVariant == 1;
    if (sGrid) {
        slant = 0; /* the grid's standard: every box is a rectangle */
    }
    if (carbon) {
        int lit = (top.r > 200 && top.g < 100) || (top.r > 250 && top.g > 230);
        slant = 0;
        if (lit) {
            top = (Colour){ 0, 112, 122, 255 };
            bottom = (Colour){ 0, 94, 114, 255 };
        } else if (top.b > top.r + 40) {
            top = (Colour){ 28, 31, 38, 245 };
            bottom = (Colour){ 28, 31, 38, 245 };
        }
    }
    for (y = y0 < sClipTop ? sClipTop : y0; y < y1 && y < sClipEnd; y++) {
        float left = x0 + (float) slant * (y1 - 1 - y) / (h > 1 ? h - 1 : 1);
        float right = x1 - (float) slant * (y - y0) / (h > 1 ? h - 1 : 1);
        Colour c = mix(top, bottom, y - y0, h - 1);
        int xl = (int) left, xr = (int) right;
        for (x = xl; x <= xr; x++) {
            float cover = 1.0f;
            if (x == xl) {
                cover = 1.0f - (left - (float) xl);
            }
            if (x == xr) {
                cover *= right - (float) xr;
            }
            plot_c(x, y, c, (int) (cover * 255));
        }
    }
    if (carbon) {
        Colour e = top.g > 120 && top.b > 120 && top.r < 50 ? (Colour){ 120, 240, 240, 255 } : (Colour){ 74, 82, 98, 255 };
        fill(x0, y0, x1 + 1, y0 + 1, e);
        fill(x0, y1 - 1, x1 + 1, y1, e);
        fill(x0, y0, x0 + 1, y1, e);
        fill(x1, y0, x1 + 1, y1, e);
    }
}

static int is_lit_edge(Colour edge) {
    return edge.r > 200 && edge.g > 150 && edge.b < 100 && edge.a > 200;
}

/* Panel colours for Glass (dock 0) and Dock (dock 1) in the current palette (scratch mockups). */
static void palette_panel(int dock, Colour *top, Colour *bottom, Colour *edge) {
    switch (sVariant) {
        case 2:
            *top = (Colour){ 0, 60, 60, dock ? 90 : 175 };
            *bottom = (Colour){ 0, 48, 50, dock ? 90 : 180 };
            *edge = dock ? (Colour){ 0, 0, 0, 0 } : (Colour){ 160, 255, 230, 80 };
            break;
        case 3:
            *top = (Colour){ 92, 30, 104, dock ? 120 : 190 };
            *bottom = (Colour){ 70, 22, 90, dock ? 120 : 195 };
            *edge = dock ? (Colour){ 0, 0, 0, 0 } : (Colour){ 255, 190, 210, 110 };
            break;
        case 4:
            *top = (Colour){ 20, 46, 110, 235 };
            *bottom = (Colour){ 14, 34, 92, 240 };
            *edge = (Colour){ 255, 255, 255, 170 };
            break;
        default:
            if (dock) {
                *top = *bottom = (Colour){ 0, 20, 80, 70 };
                *edge = (Colour){ 0, 0, 0, 0 };
            } else {
                *top = (Colour){ 24, 44, 118, 165 };
                *bottom = (Colour){ 16, 34, 100, 170 };
                *edge = (Colour){ 190, 215, 255, 80 };
            }
    }
}

/* A panel or highlight in the current style. Classic draws the game's own rounded box. */
static void box(int x0, int y0, int x1, int y1, Colour inside, Colour edge) {
    int lit = is_lit_edge(edge), frame = edge.a > 0 && inside.a > 0;
    if (sStyle == 0 || (inside.a == 0 && !lit)) {
        // Classic, or the outline pieces the buttons are made of.
        if (sStyle == 0 || edge.a != 0) {
            box_classic(x0, y0, x1, y1, inside, edge);
        }
        return;
    }
    switch (sStyle) {
        case 1: // Glass: frosted, smooth, softly edged.
            if (lit) {
                smooth_box(x0, y0, x1, y1, (y1 - y0) > 40 ? 8 : (y1 - y0) / 2, (Colour){ 255, 230, 100, 230 },
                           (Colour){ 235, 150, 20, 230 }, (Colour){ 255, 250, 200, 200 }, 1);
            } else if (frame) {
                Colour pt, pb, pe;
                palette_panel(0, &pt, &pb, &pe);
                smooth_box(x0, y0, x1, y1, (y1 - y0) > 40 ? 7 : (y1 - y0) / 2, pt, pb, pe, 1);
            }
            break;
        case 2: // Broadcast: flat slanted slabs, the lit one in the logo's red-to-yellow.
            if (lit) {
                slab(x0, y0, x1, y1, 5, (Colour){ 235, 40, 40, 255 }, (Colour){ 255, 150, 20, 255 });
            } else {
                slab(x0, y0, x1, y1, 5, (Colour){ 30, 44, 110, 235 }, (Colour){ 22, 32, 92, 235 });
            }
            break;
        default: // Dock: few boxes; panels are a whisper, the lit one a gold ring.
            if (lit) {
                smooth_box(x0, y0, x1, y1, (y1 - y0) / 2 < 14 ? (y1 - y0) / 2 : 14, (Colour){ 255, 255, 255, 40 },
                           (Colour){ 255, 255, 255, 40 }, (Colour){ 255, 214, 40, 255 }, 1);
            } else if (frame) {
                Colour pt, pb, pe;
                palette_panel(1, &pt, &pb, &pe);
                smooth_box(x0, y0, x1, y1, (y1 - y0) > 40 ? 9 : (y1 - y0) / 2, pt, pb, pe, 1);
            }
            break;
    }
}

/* Part of a picture, tinted (255 each: as it is); a black tint with alpha draws its shadow. */
/* Grid, Classic: the fun font drawn in one ink. 1: its dark outline kept, the coloured face in the ink; 2: the face
 * only, no outline (a disabled label). 0: the font's own colours. */
static int sFlatInk;

static void picture_part(const Picture *p, int sx, int sy, int w, int h, int x, int y, Colour tint) {
    int i, j;
    if (p->rgba == NULL) {
        return;
    }
    for (j = 0; j < h; j++) {
        if (y + j < sClipTop || y + j >= sClipEnd) {
            continue;
        }
        for (i = 0; i < w; i++) {
            const unsigned char *t = p->rgba + ((sy + j) * p->width + sx + i) * 4;
            if (t[3] != 0 && sFlatInk) {
                int lum = (t[0] * 3 + t[1] * 6 + t[2]) / 10;
                if (lum < 70 && t[0] < 120) {
                    if (sFlatInk == 1) {
                        plot(x + i, y + j, t[0], t[1], t[2], t[3] * tint.a / 255); /* the outline */
                    }
                } else {
                    plot(x + i, y + j, tint.r, tint.g, tint.b, t[3] * tint.a / 255);
                }
            } else if (t[3] != 0) {
                plot(x + i, y + j, t[0] * tint.r / 255, t[1] * tint.g / 255, t[2] * tint.b / 255, t[3] * tint.a / 255);
            }
        }
    }
}

static void picture(const Picture *p, int x, int y) {
    picture_part(p, 0, 0, p->width, p->height, x, y, WHITE);
}

static int mono_char(int x, int y, unsigned char c, Colour colour) {
    int i, j;
    if (sMono != NULL) {
        for (j = 0; j < 8; j++) {
            unsigned char bits = sMono[c * 8 + j];
            for (i = 0; i < 8; i++) {
                if (bits & (0x80 >> i)) {
                    plot(x + i, y + j, colour.r, colour.g, colour.b, colour.a);
                }
            }
        }
    }
    return 8;
}

static int font_ready(int font) {
    return font != FONT_MONO && sFonts[font].loaded;
}

/* How far a character moves the pen (font.c, get_text_width). */
static int advance(const Font *f, unsigned char c) {
    if (c <= 0x20 || c >= 0x80) {
        return f->spaceWidth;
    }
    if (f->glyphs[c - 0x20].texture == 0xFF) {
        return 0;
    }
    return f->fixedAdvance ? f->fixedAdvance : f->glyphs[c - 0x20].advance;
}

static int sNoGap; /* 1: draw the fun font without its extra letter gap (a label that has no room for it). */

/* The game's big letters fill their cells edge to edge, so they touched; every label gets a pixel between letters. */
static int letter_gap(int font) {
    return font == FONT_FUN && !sNoGap ? 1 : 0;
}

/* Letters of the fun font whose art overhangs the one before: they get a pixel more room (side tabs only). */
static int letter_kern(int font, char prev, char c) {
    return letter_gap(font) && ((prev == 'S' && c == 'A') || (prev == 'V' && c == 'E')) ? 1 : 0;
}

/* The sizes of the looks' fonts: the small text, headings, and the HUD's big, large and small numerals and labels. */
enum { UI_BODY, UI_HEAD, UI_XL, UI_LG, UI_SM };

static const UiFont *ui_font_kind(int kind) {
    switch (sStyle) {
        case 1:
            return kind == UI_HEAD ? &kGlassHead : kind == UI_XL ? &kGlassXL : kind == UI_LG ? &kGlassLG
                   : kind == UI_SM ? &kGlassSM : &kGlass;
        case 2:
            return kind == UI_HEAD ? &kBarlowHead : kind == UI_XL ? &kBarlowXL : kind == UI_LG ? &kBarlowLG
                   : kind == UI_SM ? &kBarlowSM : &kBarlow;
        case 3:
            return kind == UI_HEAD ? &kOutfitHead : kind == UI_XL ? &kOutfitXL : kind == UI_LG ? &kOutfitLG
                   : kind == UI_SM ? &kOutfitSM : &kOutfit;
        default:
            /* Classic on the grid: Lilita One, drawn in the game's style (outlined, shadowed); off the grid the game's fonts */
            if (!sGrid) {
                return NULL;
            }
            return kind == UI_HEAD ? &kClassicHead : kind == UI_XL ? &kClassicXL : kind == UI_LG ? &kClassicLG
                   : kind == UI_SM ? &kClassicSM : &kClassic;
    }
}

/* The look's own font for the small text and headings (NULL: the game's). */
static const UiFont *ui_font(int font) {
    /* the grid's looks bring their own fonts, so they typeset from the first frame; the old layouts wait for the ROM's
       (the 8x8 console font until then) */
    if ((font != FONT_SMALL && font != FONT_FUN) || (!font_ready(font) && !sGrid)) {
        return NULL;
    }
    return ui_font_kind(font == FONT_FUN ? UI_HEAD : UI_BODY);
}

static int ui_has(unsigned char c) {
    return c > 0x20 && c < 0x21 + UI_NG;
}

/* a capital drawn in a variant (a lone letter in its key cap, centred to the pixel): 1 a column wider (the key
   cap letters' wide form), 2 the key cap letters' narrow form */
static int sUiWide;
/* The glyph drawn phase quarters of a pixel right of its whole-pixel place. */
static const UiGlyph *ui_glyph(const UiFont *u, unsigned char c, int phase, const unsigned char **pixels) {
    const UiGlyph *g = &u->glyphs[phase * UI_NG + c - 0x21];
    if (sUiWide && u->glyphs[(u->phases + sUiWide - 1) * UI_NG + c - 0x21].width > 0) {
        g = &u->glyphs[(u->phases + sUiWide - 1) * UI_NG + c - 0x21]; /* the variants follow the phases' sets */
    }
    *pixels = u->pixels + g->offset;
    return g;
}

/* The pair's kerning in 1/64 px (the font's own table, as a word processor applies it). */
static int ui_kern(const UiFont *u, unsigned char a, unsigned char b) {
    int lo = 0, hi = u->kernCount - 1;
    unsigned key = (unsigned) a << 8 | b;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        unsigned k = (unsigned) u->kerns[mid].a << 8 | u->kerns[mid].b;
        if (k == key) {
            return u->kerns[mid].k;
        }
        if (k < key) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return 0;
}

/* One character's advance in 1/64 px. */
/* A drop shadow fills the column after each glyph, so a shadowed line's word space would read a pixel narrower than
   the font's (Outfit's 2.6 px space left "ROM loaded" 2-3 px apart): the space gets that pixel back. */
static int sUiShadowSpace;
static int ui_adv64(const UiFont *u, unsigned char c) {
    return ui_has(c) ? u->glyphs[c - 0x21].advance : u->space + sUiShadowSpace;
}

/*
 * Where each character of a line starts and the line's width: the pen moves in 1/64 px by the glyphs' advances and
 * the pairs' kerning, and each glyph lands on the quarter pixel nearest the pen (xs: its whole pixel from the line's
 * origin, ph: its quarter, the glyph image drawn that far right), so letter gaps stay even as a word processor's do.
 * Fonts in whole pixels (phases 1) land on the nearest pixel. xs and ph may be NULL; they hold one entry a character.
 */
static int sUiTrack; /* extra space between letters, 1/64 px (Classic's button labels: no shared outline) */
static int sPen0; /* where a line's pen starts, in 1/64 px: a centred label set a fraction of a pixel over */
/*
 * Classic (whole-pixel letters with an outline): a label whose room is odd is centred by drawing one letter a column
 * wider (its middle column twice: a bar or a bowl a pixel longer), never by a wider gap, so every gap between letters
 * stays one outline pixel. The letter: the one nearest the label's middle whose middle column is a bar or a gap, not a
 * lone stem (E, O, S, N: not I, T or 1). sUiWiden set: the line is drawn so.
 */
static int sUiWiden;
static int sUiWidenCol = -1; /* ui_glyph_draw: this glyph column drawn twice (-1: none) */
static int ui_widen_pick(const char *s) {
    static const char *const sOk = "BCDEFGHJKLMNOPQRSUWZabcdeghmnopqsuwz023456789";
    int n = (int) strlen(s), best = -1, bd = 1 << 20, i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        int ok = strchr(sOk, c) != NULL || (c >= UI_PROP0 + 2 && c < UI_PROP0 + 10) || c == UI_PROP0;
        int d = 2 * i - (n - 1);
        d = d < 0 ? -d : d;
        if (c != 0 && ok && d < bd) {
            bd = d, best = i;
        }
    }
    return best;
}
#define UI_LINE_MAX_ 256 /* (UI_LINE_MAX, below) */
/* A figure: tabular (0-9) or proportional (UI_PROP0...). */
static int ui_is_fig(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= UI_PROP0 && c < UI_PROP0 + 10);
}

/*
 * The clear columns between two glyphs drawn at x (whole pixels, their quarter ph), where their inks come nearest on
 * the same row (as the eye measures a slash's gap: "2/3" reads with the 2's foot and the 3's shoulder equally far off).
 */
static int sUiNoShadow; /* outlined text without its drop shadow (small tab names, the racer's own lit row) */
static int ui_ink_gap(const UiFont *u, unsigned char a, int xa, int pa, unsigned char b, int xb, int pb) {
    /* the glyphs' faces, the ink in the text's colour (an outline or shadow, darker than what is behind it, reads as
       the letter's edge, not as the gap) */
    static const signed char off[1][2] = { { 0, 0 } };
    int noff = 1;
    int ra[64], lb[64], y0 = -24, best = 1 << 20, r, n, o;
    const unsigned char *px;
    const UiGlyph *g;
    for (r = 0; r < 64; r++) {
        ra[r] = -(1 << 20), lb[r] = 1 << 20;
    }
    for (n = 0; n < 2; n++) {
        int x = n ? xb : xa, i, j;
        g = ui_glyph(u, n ? b : a, n ? pb : pa, &px);
        for (j = 0; j < g->height; j++) {
            for (i = 0; i < g->width; i++) {
                int cov = px[j * g->width + i];
                if (cov < (u->outlined ? 112 : 184)) { /* (solid ink: a half-covered edge pixel reads as the edge, not the letter; an outlined figure is drawn solid from 112) */
                    continue;
                }
                for (o = 0; o < noff; o++) {
                    int row = g->y + j + off[o][1] - y0, col = x + g->x + i + off[o][0];
                    if (row < 0 || row >= 64) {
                        continue;
                    }
                    if (n == 0 && col > ra[row]) {
                        ra[row] = col;
                    }
                    if (n == 1 && col < lb[row]) {
                        lb[row] = col;
                    }
                }
            }
        }
    }
    {
        /* the gap between the two inks' boxes: the first's rightmost solid column to the second's leftmost, whatever
           rows they are on (as owner rule 25 measures a slash's gaps, ink to ink on both sides) */
        int right = -(1 << 20), left = 1 << 20;
        for (r = 0; r < 64; r++) {
            right = ra[r] > right ? ra[r] : right;
            left = lb[r] < left ? lb[r] : left;
        }
        if (right > -(1 << 19) && left < (1 << 19)) {
            best = left - right - 1;
        }
    }
    return best;
}

static int ui_layout_ph(const UiFont *u, const char *s, int *xs, int *ph) {
    int pen = u->phases > 1 ? sPen0 : 0, i, widen = sUiWiden ? ui_widen_pick(s) : -1, n, extra = 0;
    int lx[UI_LINE_MAX_], lp[UI_LINE_MAX_];
    unsigned char prev = 0;
    for (i = 0; s[i] != '\0' && i < UI_LINE_MAX_; i++) {
        unsigned char c = (unsigned char) s[i];
        if (prev != 0 && prev != ' ' && c != ' ') {
            pen += sUiTrack; /* between letters, not round a word space */
        }
        if (prev != 0 && (ui_has(prev) || prev == ' ') && (ui_has(c) || c == ' ')) {
            pen += ui_kern(u, prev, c); /* a space is kerned only against the figures beside it */
        }
        {
            int q = u->phases > 1 ? (pen + 8) >> 4 : ((pen + 32) >> 6) << 2;
            lx[i] = q >> 2;
            lp[i] = q & 3;
        }
        pen += ui_adv64(u, c) + (i == widen ? 64 : 0);
        prev = c;
    }
    n = i;
    /*
     * a fraction ("2/3" laps, "9/10" bananas): the slash exactly as far from the figure before it as from the one
     * after it (box to box: the clear columns between their inks), the nearer side opened by whole pixels, never closed
     */
    for (i = 1; i + 1 < n; i++) {
        unsigned char a = (unsigned char) s[i - 1], b = (unsigned char) s[i + 1];
        if (s[i] == '/' && ui_is_fig(a) && ui_is_fig(b)) {
            int k, gl, gr, t, dl, dr;
            gl = ui_ink_gap(u, a, lx[i - 1], lp[i - 1], '/', lx[i], lp[i]);
            gr = ui_ink_gap(u, '/', lx[i], lp[i], b, lx[i + 1], lp[i + 1]);
            if (gl > 64 || gr > 64) {
                continue;
            }
            t = gl > gr ? gl : gr;
            t = t < 2 ? 2 : t; /* 2 clear columns at least, so every fraction in a look is spaced alike ("2/3", "3/3", "7/10") */
            if (gl == t && gr == t) {
                continue;
            }
            dl = t - gl, dr = t - gr;
            for (k = i; k < n; k++) {
                lx[k] += dl + (k > i ? dr : 0);
            }
            extra += dl + dr;
        }
    }
    for (i = 0; i < n; i++) {
        if (xs != NULL) {
            xs[i] = lx[i];
        }
        if (ph != NULL) {
            ph[i] = lp[i];
        }
    }
    return ((pen + 32) >> 6) + extra;
}

static int ui_layout(const UiFont *u, const char *s, int *xs) {
    return ui_layout_ph(u, s, xs, NULL);
}

/* One character alone, in whole pixels (a line is measured with ui_layout, which kerns it). */
static int ui_advance(const UiFont *u, unsigned char c) {
    return (ui_adv64(u, c) + 32) >> 6;
}

/* Twice the middle of a capital letter, from the text's top: the capitals' band is what the eye centres on. */
/* How far to move the small digits so that, drawn on the same line as a label at the same y, they share its baseline. */
static int digit_dy(void);
static int ui_cap_middle2(const UiFont *u) {
    return u->glyphs['H' - 0x21].y + u->base;
}

static int digit_dy(void) {
    const UiFont *u = ui_font(FONT_SMALL);
    return u != NULL ? u->base - 12 : 0; /* the digits' last ink row is 11 below their y, a capital's is base - 1 below its y */
}

static int text_width_as_is(int font, const char *text) {
    int width = 0;
    char prev = 0;
    const UiFont *u = ui_font(font);
    if (u != NULL) {
        return ui_layout(u, text, NULL);
    }
    if (!font_ready(font)) {
        return (int) strlen(text) * 8;
    }
    for (; *text != '\0'; text++) {
        width += advance(&sFonts[font], (unsigned char) *text) + letter_gap(font) + letter_kern(font, prev, *text);
        prev = *text;
    }
    return width;
}

/* Draws text in one of the looks' fonts from x with its top at y, typeset by ui_layout; returns where it ended. */
#define UI_LINE_MAX 256
/* Outlined text on a lit (light) row: the face alone, as outline and drop shadow on the light fill read as extra weight. */
static int sUiFlat;
static int sUiCrisp; /* the glyph being drawn is a figure, a minus or a times sign */
static int sUiCrispAll; /* set: every glyph of the text is drawn solid (small Classic labels, as the graph's) */
static int sUiFaceOnly; /* ui_draw: an outlined font's face drawn alone (dark ink, flat): solid pixels, no faint edge */
static void ui_glyph_draw(const UiFont *u, const UiGlyph *g, const unsigned char *px, int x0_, int y, Colour colour, int pass) {
    int i, j, hh = u->glyphs['H' - 0x21].height, capTop = u->base - hh;
    for (j = 0; j < g->height; j++) {
        int rep, reps;
        for (i = 0; i < g->width; i++) for (rep = 0, reps = i == sUiWidenCol ? 2 : 1; rep < reps; rep++) {
            /* (a widened letter: columns after its middle one a pixel right, the middle one drawn twice) */
            int x = x0_ + (sUiWidenCol >= 0 && i > sUiWidenCol ? 1 : 0) + rep, cover, dx, dy;
            cover = px[j * g->width + i];
            if (cover == 0 || (pass < 2 && cover < (u->outlined && (sUiCrisp || sUiFaceOnly) ? 112 : 80))) {
                /* (a solid-drawn glyph's outline follows the pixels it draws: 112 and up) */
                continue; /* the outline and shadow follow the face's solid part, not its faint edge */
            }
            if (pass == 0) {
                /* the drop shadow: the outline's shape 1 px right and 2 px down (2, not 1: the ink, outline and
                   shadow, is then the face's height plus 4, an even number of rows like the face's, so a label
                   centres exactly in the grid's even boxes) */
                for (dy = 1; dy <= 3; dy++) {
                    for (dx = 0; dx <= 2; dx++) {
                        plot(x + g->x + i + dx, y + g->y + j + dy, 8, 10, 24, 255);
                    }
                }
            } else if (pass == 1) {
                /* the outline: 1 px of dark round the face */
                for (dy = -1; dy <= 1; dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        plot(x + g->x + i + dx, y + g->y + j + dy, 16, 18, 34, 255);
                    }
                }
            } else if (pass == 2) {
                plot(x + g->x + i + 1, y + g->y + j + 1, 0, 0, 0, cover * 170 / 255);
            } else {
                Colour c = colour;
                if (u->outlined) {
                    /* the face shaded down the capitals' height as the game's lettering is: full ink on top, 80% at the foot */
                    int t = g->y + j - capTop, f;
                    t = t < 0 ? 0 : t > hh ? hh : t;
                    f = 256 - t * 52 / (hh > 0 ? hh : 1);
                    c.r = (unsigned char) (c.r * f >> 8);
                    c.g = (unsigned char) (c.g * f >> 8);
                    c.b = (unsigned char) (c.b * f >> 8);
                }
                if (u->outlined && (sUiCrisp || sUiFaceOnly)) {
                    /* outlined figures: solid pixels like the timer faces they echo, as a small counter (the 4's)
                     * otherwise fills with mid-tones of ink over the outline */
                    if (cover < 112) {
                        continue;
                    }
                    cover = 255;
                }
                plot(x + g->x + i, y + g->y + j, c.r, c.g, c.b, cover * c.a / 255);
            }
        }
    }
}

static int ui_draw(const UiFont *u, int x, int y, const char *text, Colour colour, int shadow, int right) {
    int xs[UI_LINE_MAX], ph[UI_LINE_MAX], n = 0, i, pass, end, widen;
    char line[UI_LINE_MAX];
    snprintf(line, sizeof(line), "%s", text);
    widen = sUiWiden ? ui_widen_pick(line) : -1;
    sUiShadowSpace = shadow && !u->outlined ? 64 : 0;
    end = ui_layout_ph(u, line, xs, ph);
    sUiShadowSpace = 0;
    while (line[n] != '\0') {
        n++;
    }
    /* what fits before right */
    for (i = 0; i < n; i++) {
        int next = i + 1 < n ? xs[i + 1] : end;
        if (x + next > right) {
            n = i;
            end = xs[i];
            break;
        }
    }
    /*
     * outlined fonts: every glyph's shadow, then every outline, then the faces, so no outline cuts a neighbour; dark
     * ink (a key's name on a light key cap) is drawn as its face alone, as a dark outline would swallow it
     */
    /* (a face alone, with no outline round it, is drawn solid: its faint edge pixels left the letter's ink a pixel
       off its middle in a key cap, "X") */
    sUiFaceOnly = u->outlined && (colour.r + colour.g + colour.b < 3 * 110 || sUiFlat);
    for (pass = u->outlined ? (colour.r + colour.g + colour.b < 3 * 110 || sUiFlat ? 3 : sUiNoShadow ? 1 : 0) : (shadow ? 2 : 3); pass <= 3; pass++) {
        if (u->outlined && pass == 2) {
            continue;
        }
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char) line[i];
            if (ui_has(c)) {
                const unsigned char *px;
                const UiGlyph *g = ui_glyph(u, c, ph[i], &px);
                sUiCrisp = sUiCrispAll || (c >= '0' && c <= '9') || (c >= UI_PROP0 && c < UI_PROP0 + 10) || c == UI_TIMES || c == UI_MINUS ||
                           c == '/' || c == '-'; /* (Classic: a number's slash and hyphen as solid as its digits) */
                sUiWidenCol = i == widen ? g->width / 2 : -1;
                ui_glyph_draw(u, g, px, x + xs[i], y, colour, pass);
                sUiWidenCol = -1;
                sUiCrisp = 0;
            }
        }
    }
    sUiFaceOnly = 0;
    return x + end;
}

/* Draws text from x (until right, if text would go further) with its top at y, as it is. Returns where it ended. */
static int text_clipped_as_is_impl(int font, int x, int y, const char *text, Colour colour, int shadow, int right) {
    const Font *f = &sFonts[font];
    char prev = 0;
    const UiFont *u = ui_font(font);
    if (u != NULL) {
        return ui_draw(u, x, y, text, colour, shadow, right);
    }
    if (!font_ready(font)) {
        // Before the ROM's fonts are loaded: the 8x8 font, a little lower so it sits like the small font.
        for (; *text != '\0' && x + 8 <= right; text++) {
            if (shadow) {
                mono_char(x + 1, y + 2, (unsigned char) *text, (Colour){ 0, 0, 0, 160 });
            }
            x += mono_char(x, y + 1, (unsigned char) *text, colour);
        }
        return x;
    }
    for (; *text != '\0'; text++) {
        unsigned char c = (unsigned char) *text;
        int step = advance(f, c) + letter_gap(font);
        int lift = letter_gap(font) && (c == 'A' || c == 'V') ? 1 : 0; /* these two sat a pixel low against their neighbours */
        x += letter_kern(font, prev, (char) c);
        prev = (char) c;
        if (x + step > right) {
            break;
        }
        if (c > 0x20 && c < 0x80 && f->glyphs[c - 0x20].texture != 0xFF) {
            const Glyph *g = &f->glyphs[c - 0x20];
            const Picture *t = &f->textures[g->texture];
            if (shadow) {
                picture_part(t, g->s, g->t, g->width, g->height, x + g->x + 1, y + g->y + 1 - lift, (Colour){ 0, 0, 0, 170 });
            }
            picture_part(t, g->s, g->t, g->width, g->height, x + g->x, y + g->y - lift, colour);
        }
        x += step;
    }
    return x;
}

/* The same in the game's language (port_text). */
static int text_width(int font, const char *text) {
    return text_width_as_is(font, port_text(text));
}

static int text_clipped_as_is(int font, int x, int y, const char *text, Colour colour, int shadow, int right) {
    int r;
    if (!sLogOn || sClipEnd < 0 || sDryInk || sQuiet) {
        return text_clipped_as_is_impl(font, x, y, text, colour, shadow, right);
    }
    ink_begin();
    r = text_clipped_as_is_impl(font, x, y, text, colour, shadow, right);
    if (ink_end()) {
        log_rec('T', sInkX0, sInkY0, sInkX1, sInkY1, font, text);
    }
    return r;
}
static int text_clipped(int font, int x, int y, const char *text, Colour colour, int shadow, int right) {
    return text_clipped_as_is(font, x, y, port_text(text), colour, shadow, right);
}

static int text(int font, int x, int y, const char *s, Colour colour, int shadow) {
    return text_clipped(font, x, y, s, colour, shadow, BS_WIDTH);
}

/* Where a string starts so that its ink (not its advance box) is centred in room pixels. */
static int ink_start(int font, const char *s, int room) {
    int width = text_width_as_is(font, s), inkL = 0, inkR = width;
    const char *c;
    const UiFont *u = ui_font(font);
    if (u != NULL) {
        int xs[UI_LINE_MAX], ph[UI_LINE_MAX], first = -1, last = -1, i;
        char line[UI_LINE_MAX];
        snprintf(line, sizeof(line), "%s", s);
        ui_layout_ph(u, line, xs, ph);
        for (i = 0; line[i] != '\0'; i++) {
            if (ui_has((unsigned char) line[i])) {
                first = first < 0 ? i : first;
                last = i;
            }
        }
        (void) c;
        if (first >= 0) {
            const unsigned char *px;
            const UiGlyph *g0 = ui_glyph(u, (unsigned char) line[first], ph[first], &px);
            const UiGlyph *g1 = ui_glyph(u, (unsigned char) line[last], ph[last], &px);
            inkL = xs[first] + g0->x;
            inkR = xs[last] + g1->x + g1->width;
        }
    }
    return (room - (inkR - inkL)) / 2 - inkL;
}

static void text_centred_as_is(int font, int x0, int x1, int y, const char *s, Colour colour, int shadow) {
    text_clipped_as_is(font, x0 + ink_start(font, s, x1 - x0), y, s, colour, shadow, x1);
}

static void text_centred(int font, int x0, int x1, int y, const char *s, Colour colour, int shadow) {
    text_centred_as_is(font, x0, x1, y, port_text(s), colour, shadow);
}

static int ui_width(const UiFont *u, const char *s) {
    return ui_layout(u, s, NULL);
}

/*
 * Text that has to fit between x0 and x1 (a menu value, label or tip, a line under a card): as it is when it fits;
 * in a look's smaller font when that fits; otherwise cut with "..." so that it never just stops at the edge. Centred
 * between x0 and x1, or from x0. Returns where it ended.
 */
static int text_fit_as_is(int font, int x0, int x1, int y, const char *s, Colour colour, int shadow, int centred) {
    char cut[160];
    const UiFont *u = ui_font(font), *small = NULL;
    int room = x1 - x0, width = text_width_as_is(font, s), n;
    if (width <= room) {
        return text_clipped_as_is(font, centred ? x0 + ink_start(font, s, room) + 1 : x0, y, s, colour, shadow, x1 + 2);
    }
    if (u != NULL && font == FONT_SMALL) {
        small = ui_font_kind(UI_SM);
        width = ui_width(small, s);
        if (width <= room) {
            // Its capitals on the same middle line as the bigger font's would have been.
            return ui_draw(small, centred ? x0 + (room - width) / 2 : x0,
                           y + (ui_cap_middle2(u) - ui_cap_middle2(small)) / 2, s, colour, shadow, x1);
        }
    }
    // Neither fits: as much as fits, then "...".
    n = (int) strlen(s);
    if (n > (int) sizeof(cut) - 4) {
        n = (int) sizeof(cut) - 4;
    }
    for (; n > 0; n--) {
        while (n > 0 && s[n - 1] == ' ') {
            n--;
        }
        snprintf(cut, sizeof(cut), "%.*s...", n, s);
        width = small != NULL ? ui_width(small, cut) : text_width_as_is(font, cut);
        if (width <= room) {
            break;
        }
    }
    if (n == 0) {
        snprintf(cut, sizeof(cut), "...");
        width = small != NULL ? ui_width(small, cut) : text_width_as_is(font, cut);
    }
    if (small != NULL) {
        return ui_draw(small, centred ? x0 + (room - width) / 2 : x0,
                       y + (ui_cap_middle2(u) - ui_cap_middle2(small)) / 2, cut, colour, shadow, x1 + 1);
    }
    return text_clipped_as_is(font, centred ? x0 + (room - width) / 2 : x0, y, cut, colour, shadow, x1 + 1);
}

/* The same in the game's language (port_text). */
static int text_fit(int font, int x0, int x1, int y, const char *s, Colour colour, int shadow) {
    return text_fit_as_is(font, x0, x1, y, port_text(s), colour, shadow, 0);
}

static int text_fit_centred(int font, int x0, int x1, int y, const char *s, Colour colour, int shadow) {
    return text_fit_as_is(font, x0, x1, y, port_text(s), colour, shadow, 1);
}

/* One letter centred in a box by the glyph's own size (a name box's initial), shadowed. */
static void letter_in_box(int font, int x0, int y0, int x1, int y1, char c, Colour colour) {
    const Font *f = &sFonts[font];
    const Glyph *g;
    int x, y;
    if (!font_ready(font) || c <= 0x20 || c >= 0x80 || f->glyphs[c - 0x20].texture == 0xFF) {
        return;
    }
    g = &f->glyphs[c - 0x20];
    x = (x0 + x1 - g->width) / 2;
    y = (y0 + y1 - g->height) / 2;
    picture_part(&f->textures[g->texture], g->s, g->t, g->width, g->height, x + 1, y + 1, (Colour){ 0, 0, 0, 170 });
    picture_part(&f->textures[g->texture], g->s, g->t, g->width, g->height, x, y, colour);
}

/* Capitals of the text in the game's language, for the fun font's headings. */
static void upper_text(char *out, int size, const char *s) {
    int i;
    s = port_text(s);
    for (i = 0; s[i] != '\0' && i < size - 1; i++) {
        out[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
    }
    out[i] = '\0';
}

/* Dense text (the profiler and the watchdog) in 8 pixel columns: the 8x8 font, or the small font centred. */
static void mono_text(int x, int y, const char *s, Colour colour) {
    int style = sStyle;
    sStyle = 0; /* debug text always uses the game's small font, whatever look is picked */
    for (; *s != '\0' && x + 8 <= BS_WIDTH; s++, x += 8) {
        if (sMono != NULL || !font_ready(FONT_SMALL)) {
            mono_char(x, y, (unsigned char) *s, colour);
        } else {
            char one[2] = { *s, '\0' };
            text_centred_as_is(FONT_SMALL, x, x + 8, y - 2, one, colour, 0);
        }
    }
    sStyle = style;
}

/*
 * The background: deep blue, lighter at the top, with a faint chequered flag pattern; the status lines sit on a
 * darker strip.
 */
static void make_background(void) {
    int x, y;
    for (y = 0; y < BS_HEIGHT; y++) {
        int r = 26 - y * 18 / BS_HEIGHT, g = 58 - y * 40 / BS_HEIGHT, b = 160 - y * 92 / BS_HEIGHT;
        if (sStyle == 1 || sStyle == 3) {
            r = 28 - y * 18 / BS_HEIGHT;
            g = 76 - y * 48 / BS_HEIGHT;
            b = 200 - y * 100 / BS_HEIGHT;
        } else if (sStyle == 2) {
            r = 20 - y * 12 / BS_HEIGHT;
            g = 30 - y * 18 / BS_HEIGHT;
            b = 84 - y * 44 / BS_HEIGHT;
        }
        if (sVariant == 1) {
            r = 16 - y * 6 / BS_HEIGHT;
            g = 18 - y * 6 / BS_HEIGHT;
            b = 24 - y * 8 / BS_HEIGHT;
        } else if (sVariant == 2) {
            r = 14 - y * 10 / BS_HEIGHT;
            g = 100 - y * 50 / BS_HEIGHT;
            b = 84 - y * 36 / BS_HEIGHT;
        } else if (sVariant == 3) {
            r = 96 + y * 110 / BS_HEIGHT;
            g = 34 + y * 40 / BS_HEIGHT;
            b = 130 - y * 60 / BS_HEIGHT;
        } else if (sVariant == 4) {
            r = 150 - y * 60 / BS_HEIGHT;
            g = 200 - y * 50 / BS_HEIGHT;
            b = 245 - y * 30 / BS_HEIGHT;
        }
        if (y >= BS_STATUS_Y) {
            r = 4;
            g = 8;
            b = 34;
        }
        for (x = 0; x < BS_WIDTH; x++) {
            int light = 0;
            if (y < BS_STATUS_Y) {
                if (sVariant == 1) {
                    light = (x % 16 == 0 || y % 16 == 0) ? 4 : 0; /* a fine grid */
                    if (y < 3 && !sGrid) {
                        r = 0; g = 190; b = 190;
                    }
                } else if (sVariant != 0) {
                    light = 0;
                } else if (sStyle == 0) {
                    light = ((x / 20) + (y / 20)) % 2 == 0 ? 7 : 0;
                } else if (sStyle == 1) {
                    light = ((x / 20) + (y / 20)) % 2 == 0 ? 3 : 0;
                    if (!sGrid) {
                        light += (60 - (x - 60) * (x - 60) / 80 - (y + 20) * (y + 20) / 90) > 0 ? 8 : 0;
                    }
                } else if (sStyle == 2) {
                    light = ((x + y) / 6) % 2 == 0 ? 3 : 0; // Faint diagonal stripes.
                    if (y < 3 && !sGrid) {
                        r = 235 - x * 20 / BS_WIDTH;
                        g = 40 + x * 100 / BS_WIDTH;
                        b = 30;
                    }
                } else {
                    // Dock: calm, with a soft darker swell near the bottom like the title's sea.
                    light = !sGrid && y > 170 + (int) (4 * sinf(x / 24.0f)) ? -10 : 0; /* not on the grid: it hid panel bottoms */
                }
            }
            sBackground[y * BS_WIDTH + x] = rgb565(r + light < 0 ? 0 : r + light, g + light < 0 ? 0 : g + light,
                                                   b + light < 0 ? 0 : b + light);
        }
    }
    // No line above the status strip: it read as a stray line under the buttons and threw their alignment off.
    sBackgroundMade = 1;
}

/* The look to draw in: 0 Classic, 1 Glass, 2 Broadcast, 3 Dock. Redraws everything. */
void bs_set_variant(int v) {
    sVariant = v;
    make_background();
    sRaceBaseMade = 0;
    sChanges = (1 << BS_AREA_COUNT) - 1;
}

void bs_set_style(int style) {
    if (style < 0 || style > 3 || style == sStyle) {
        return;
    }
    sStyle = style;
    make_background();
    sRaceBaseMade = 0;
    sChanges = (1 << BS_AREA_COUNT) - 1;
}

/******************************** State ********************************/

#define MESSAGES 24
#define OVERLAY_LINES 16

typedef struct State {
    char messages[MESSAGES][160];
    int messageCount; /* The newest is messages[(messageCount - 1) % MESSAGES]. */
    char overlay[OVERLAY_LINES][64];
    int overlayLines;
    int graphCount; /* The profiler's frame graph (bs_set_frame_graph); 0: none. */
    unsigned char graphHeight[BS_GRAPH_FRAMES], graphKind[BS_GRAPH_FRAMES];
    int menuShown;
    BsMenu menu;
    int buttonCount; /* 0: the panel text instead. */
    char buttons[4][40];
    char panelText[64];
    char status[2][48];
    int raceShown;
    RaceHud race;
    int setupShown;
    SetupScreen setup;
} State;

static State sState, sDrawn;

void bs_init(const unsigned char *monoFont) {
    sMono = monoFont;
    if (!sBackgroundMade) {
        make_background();
    }
    sChanges = (1 << BS_AREA_COUNT) - 1;
}

void bs_add_message(const char *text) {
    snprintf(sState.messages[sState.messageCount % MESSAGES], sizeof(sState.messages[0]), "%s", text);
    sState.messageCount++;
    sChanges |= 1 << BS_AREA_TOP;
}

void bs_clear_messages(void) {
    sState.messageCount = 0;
    sChanges |= 1 << BS_AREA_TOP;
}

void bs_set_overlay(const char *text) {
    int line = 0;
    while (line < OVERLAY_LINES && *text != '\0') {
        const char *end = strchr(text, '\n');
        int length = end != NULL ? (int) (end - text) : (int) strlen(text);
        snprintf(sState.overlay[line++], sizeof(sState.overlay[0]), "%.*s", length, text);
        text += length + (end != NULL);
    }
    sState.overlayLines = line;
    sChanges |= 1 << BS_AREA_TOP;
}

void bs_set_frame_graph(const unsigned char *halfMs, const unsigned char *kinds, int count) {
    count = count < BS_GRAPH_FRAMES ? count : BS_GRAPH_FRAMES;
    memcpy(sState.graphHeight, halfMs, count);
    memcpy(sState.graphKind, kinds, count);
    sState.graphCount = count;
    sChanges |= 1 << BS_AREA_TOP;
}

void bs_show_menu(const BsMenu *menu) {
    sState.menuShown = menu != NULL;
    if (menu != NULL) {
        sState.menu = *menu;
    }
    sChanges |= 1 << BS_AREA_TOP;
}

int bs_race_hud_covered(void) {
    return sState.menuShown || sState.overlayLines > 0 || sState.setupShown;
}

void bs_set_setup(const SetupScreen *setup) {
    sState.setupShown = setup != NULL;
    if (setup != NULL) {
        sState.setup = *setup;
    }
    sChanges |= (1 << BS_AREA_TOP) | (1 << BS_AREA_BUTTONS);
}

void bs_set_buttons(const char *const *labels, int count) {
    int i;
    sState.buttonCount = count > 4 ? 4 : count;
    for (i = 0; i < sState.buttonCount; i++) {
        snprintf(sState.buttons[i], sizeof(sState.buttons[0]), "%s", labels[i]);
    }
    sChanges |= 1 << BS_AREA_BUTTONS;
}

void bs_set_panel_text(const char *text) {
    sState.buttonCount = 0;
    snprintf(sState.panelText, sizeof(sState.panelText), "%s", text);
    sChanges |= 1 << BS_AREA_BUTTONS;
}

void bs_set_status_line(int line, const char *text) {
    if (line >= 0 && line < 2 && strcmp(sState.status[line], text) != 0) {
        snprintf(sState.status[line], sizeof(sState.status[0]), "%s", text);
        sChanges |= 1 << BS_AREA_STATUS;
    }
}

void bs_set_race_hud(const RaceHud *hud) {
    sState.raceShown = hud != NULL;
    if (hud != NULL) {
        sState.race = *hud;
    }
    sChanges |= 1 << BS_AREA_TOP;
}

unsigned bs_take_changes(void) {
    static int sFrench = -1;
    unsigned changes;
    if (port_text_french() != sFrench) {
        // The game's language changed: everything is redrawn in it.
        sFrench = port_text_french();
        sChanges = (1 << BS_AREA_COUNT) - 1;
    }
    changes = sChanges;
    sChanges = 0;
    if (changes != 0) {
        sDrawn = sState;
    }
    return changes;
}

/******************************** The areas ********************************/

static const char *newest_message(const State *s) {
    return s->messageCount > 0 ? s->messages[(s->messageCount - 1) % MESSAGES] : "";
}

/*
 * The messages box: the newest messages, wrapped to the box's width, as many as fit, the newest at the bottom.
 */
static int sMeasureOnly; /* draw_messages only counts its lines (the grid centres the before-ROM group with it) */
static int draw_messages(const State *s, int y0, int y1) {
    enum { MAX_LINES = 24 };
    const char *starts[MAX_LINES];
    int lengths[MAX_LINES], newest[MAX_LINES];
    /* the grid gives console lines 2 px of leading (the font's 8 rows touch at 8) */
    /* the grid's looks typeset in their own fonts from the first frame; the old layouts use the console font until
       the ROM's fonts load */
    int font = font_ready(FONT_SMALL) || sGrid ? FONT_SMALL : FONT_MONO, lineHeight = font == FONT_SMALL ? (sGrid ? 14 : 12) : 9; /* grid: the title panel's 14 px pitch */
    int fit = (y1 - y0 - 6) / lineHeight, count = 0, m, i;
    int width = BS_WIDTH - 24;

    if (!sGrid && !sMeasureOnly) {
        box(4, y0, BS_WIDTH - 4, y1, BOX_FILL, BOX_EDGE);
    }
    // From the newest message back, until the box is full; each message's lines are found first to last.
    for (m = s->messageCount - 1; m >= 0 && m >= s->messageCount - MESSAGES && count < fit; m--) {
        const char *text = s->messages[m % MESSAGES];
        const char *lineStarts[8];
        int lineLengths[8], lines = 0;
        while (*text == ' ') {
            text++;
        }
        do {
            int length = (int) strlen(text), cut = length;
            char part[160];
            snprintf(part, sizeof(part), "%s", text);
            while (cut > 0 && text_width(font, part) > width) {
                // Back to the space before the last word that does not fit (or cut the word).
                int space = cut - 1;
                while (space > 0 && text[space] != ' ') {
                    space--;
                }
                cut = space > 0 ? space : cut - 1;
                part[cut] = '\0';
            }
            if (cut == 0) {
                cut = length;
            }
            if (lines < 8) {
                lineStarts[lines] = text;
                lineLengths[lines] = cut;
                lines++;
            }
            text += cut;
            while (*text == ' ') {
                text++;
            }
        } while (*text != '\0');
        for (i = lines - 1; i >= 0 && count < fit; i--) {
            starts[count] = lineStarts[i];
            lengths[count] = lineLengths[i];
            newest[count] = m == s->messageCount - 1;
            count++;
        }
    }
    if (sMeasureOnly) {
        return count;
    }
    if (sGrid) {
        /* the grid: the look's own panel in the 8 px safe frame, as tall as its lines */
        /* the title panel's padding: 10 px over the first line's top, 10 under the last line's 8 rows */
        grid_panel(8, y0, BS_WIDTH - 8, y0 + 10 + ((count > 0 ? count : 1) - 1) * lineHeight + 8 + 10);
    }
    for (i = 0; i < count; i++) {
        char line[160];
        int y = y0 + (sGrid ? 9 : 3) + (count - 1 - i) * lineHeight; /* grid: the ink 9 px from the top, as from the bottom */
        snprintf(line, sizeof(line), "%.*s", lengths[i], starts[i]);
        text(font, sGrid ? 14 : 12, y, line, newest[i] ? WHITE : DIM, 1);
    }
    return count;
}

/*
 * The controls card: what each button does, with the player's own mapping (controls.c), two to a line. A debug build
 * names itself in the corner and lists the bug report (plat.h PLAT_DEBUG_TOOLS).
 */
static void draw_controls(int y0, int y1) {
    enum { MAX_ROWS = 10 }; // (SELECT+touch for a bug report is a debug shortcut: not listed.)
    static const struct {
        unsigned short actions; // 0: a fixed key.
        const char *key, *what;
    } sRows[] = {
        { 0, "Circle Pad", "Steer" },
        { CTRL_N64_A, NULL, "Accelerate" },
        { CTRL_N64_B, NULL, "Brake" },
        { CTRL_N64_R, NULL, "Hop / drift" },
        { CTRL_N64_Z, NULL, "Use item" },
        { CTRL_N64_LOOK_BACK, NULL, "Look back" },
        { CTRL_N64_CU, NULL, "Camera view" },
        { CTRL_N64_CD, NULL, "Track arrows" },
        { CTRL_N64_CR, NULL, "HUD toggle" },
        { 0, "START", "Pause" },
    };
    char keys[MAX_ROWS][24];
    const char *what[MAX_ROWS];
    int i, count = 0, lineHeight = font_ready(FONT_SMALL) ? 12 : 10, keyWidth[2] = { 0, 0 }, top = y0 + 4;
    if (sStyle <= 1) {
        box(4, y0, BS_WIDTH - 4, y1, BOX_FILL, BOX_EDGE);
    }
    for (i = 0; i < (int) (sizeof(sRows) / sizeof(sRows[0])); i++) {
        if (sRows[i].actions != 0) {
            controls_keys_for(sRows[i].actions, keys[count], sizeof(keys[0]));
            if (keys[count][0] == '\0') {
                continue; // Mapped to no button.
            }
        } else {
            snprintf(keys[count], sizeof(keys[0]), "%s", sRows[i].key);
        }
        what[count++] = sRows[i].what;
    }
    if (font_ready(FONT_FUN)) {
        if (sStyle == 2) {
            // Broadcast: the heading on a red tag, like a race broadcast's lower third.
            Colour red = { 235, 40, 40, 255 }, orange = { 255, 150, 20, 255 };
            slab(8, y0 + 2, 16 + text_width(FONT_FUN, "CONTROLS") + 7, y0 + 22, 5, red, orange);
        }
        text(FONT_FUN, sStyle == 2 ? 16 : 12, y0 + (sStyle == 2 ? 7 : 4), "CONTROLS", WHITE, 1);
        top = y0 + 21;
    }
    if (sStyle != 0 && font_ready(FONT_SMALL)) {
        int rows = (count + 1) / 2;
        lineHeight = rows > 0 ? (y1 - top - 24) / rows : 20;
        lineHeight = lineHeight > 22 ? 22 : (lineHeight < 12 ? 12 : lineHeight);
        top += 4;
    }
    if (font_ready(FONT_SMALL)) {
        char build[48];
        // Players see the version; debug builds the git describe, which names the commit too.
        if (PLAT_DEBUG_TOOLS) {
            snprintf(build, sizeof(build), port_text("Build %.24s"), plat_build_version());
        } else {
            snprintf(build, sizeof(build), port_text("Version %.20s"), plat_version());
        }
        text(FONT_SMALL, BS_WIDTH - 12 - text_width(FONT_SMALL, build), y0 + 6, build, DIM, 1);
    }
    for (i = 0; i < count; i++) {
        int width = text_width(FONT_SMALL, keys[i]) + (sStyle != 0 ? 14 : 8);
        if (sStyle != 0 && width < 20) {
            width = 20;
        }
        if (width > keyWidth[i % 2]) {
            keyWidth[i % 2] = width;
        }
    }
    for (i = 0; i < count; i++) {
        int x = (i % 2) ? 164 : 12, y = top + (i / 2) * lineHeight;
        if (!font_ready(FONT_SMALL) && (i % 2)) {
            continue; // The 8x8 font only fits one column; the second waits for the ROM.
        }
        if (sStyle != 0) {
            // The keys as white caps (a pill; a slanted one in Broadcast), the action beside them.
            int w = text_width(FONT_SMALL, keys[i]) + 14, cy = y + 6, half = lineHeight >= 18 ? 8 : 6;
            Colour cap = { 235, 240, 255, 255 }, ink = { 20, 26, 70, 255 };
            if (w < 20) {
                w = 20;
            }
            if (sStyle == 2) {
                // The row's slab and the key's cap, both kept inside the line so that neighbouring lines never touch.
                {
                    int right = x + w + 6 + text_width(FONT_SMALL, what[i]) + 10;
                    right = right < x + 142 ? x + 142 : right; // Long ones (SELECT+touch) get a longer slab.
                    slab(x - 3, cy - half - 1, right, cy + half + 1, 4, (Colour){ 26, 38, 96, 235 }, (Colour){ 26, 38, 96, 235 });
                }
                slab(x, cy - half, x + w, cy + half, 4, cap, cap);
            } else {
                smooth_box(x, cy - half, x + w, cy + half, half, cap, (Colour){ 205, 215, 245, 255 }, (Colour){ 0, 0, 0, 0 }, 1);
            }
            text_centred(FONT_SMALL, x + 1, x + w + (sStyle == 3 ? 1 : 2), y + (sStyle == 2 ? 2 : 1), keys[i], ink, 0);
            text(FONT_SMALL, x + w + 6, y + (sStyle == 2 ? 2 : 1), what[i], WHITE, 1);
            continue;
        }
        text(FONT_SMALL, x, y, keys[i], YELLOW, 1);
        text(FONT_SMALL, x + keyWidth[i % 2], y, what[i], WHITE, 1);
    }
}

static int sTitleY = 8; /* the grid's before-ROM title panel top (the title and the messages are centred as one group) */
static void draw_logo(void) {
    if (sLogo.rgba != NULL) {
        picture(&sLogo, (BS_WIDTH - 160) / 2, sStyle == 2 ? 7 : 3); /* Broadcast's top stripe needs room */
    } else if (sGrid) {
        /* before the ROM, on the grid: the title in the look's own panel (no line of it on the bare background, which
           is light in Daylight), on slots 0-1, its two lines centred in it */
        grid_panel(8, sTitleY, BS_WIDTH - 8, sTitleY + 44);
        text_centred(FONT_FUN, 0, BS_WIDTH, sTitleY + 8, "Diddy Kong Racing", YELLOW, 1);
        text_centred(FONT_SMALL, 0, BS_WIDTH, sTitleY + 24, "Nintendo 3DS port", DIM, 1);
    } else {
        text_centred(FONT_MONO, 0, BS_WIDTH, 20, "Diddy Kong Racing", YELLOW, 1);
        text_centred(FONT_MONO, 0, BS_WIDTH, 36, "Nintendo 3DS port", DIM, 1);
    }
}

static void draw_overlay(const State *s, int y0, int y1) {
    int i;
    box(2, y0, BS_WIDTH - 2, y1, (Colour){ 0, 0, 20, 170 }, BOX_EDGE);
    for (i = 0; i < s->overlayLines && y0 + 2 + i * 8 + 8 <= y1; i++) {
        mono_text(4, y0 + 2 + i * 8, s->overlay[i], i == 0 ? YELLOW : WHITE);
    }
}

/*
 * The profiler's frame graph: a bar per frame (2 pixels wide, the newest on the right), its height the frame's length
 * (the top is 50 ms), coloured by what took its time (bottom_screen.h BS_GRAPH_*); lines at 60 and 30 fps.
 */
#define GRAPH_Y1 66
#define GRAPH_PX_PER_MS (GRAPH_Y1 / 50.0f)
static const Colour sGraphColours[BS_GRAPH_KINDS] = {
    { 80, 230, 110, 255 },  /* logic */
    { 80, 210, 255, 255 },  /* scene */
    { 255, 220, 60, 255 },  /* renderer */
    { 255, 140, 40, 255 },  /* textures */
    { 255, 70, 70, 255 },   /* GPU */
    { 120, 130, 170, 255 }, /* retrace */
};

static void draw_graph(const State *s) {
    int i, x0 = BS_WIDTH - 2 - s->graphCount * 2;
    fill(2, 0, BS_WIDTH - 2, GRAPH_Y1 + 1, (Colour){ 0, 0, 20, 200 });
    for (i = 0; i < s->graphCount; i++) {
        int kind = s->graphKind[i] & 15, h = (int) (s->graphHeight[i] * 0.5f * GRAPH_PX_PER_MS + 0.5f);
        Colour c = sGraphColours[kind < BS_GRAPH_KINDS ? kind : 0];
        h = h > GRAPH_Y1 ? GRAPH_Y1 : h < 1 ? 1 : h;
        fill(x0 + i * 2, GRAPH_Y1 - h, x0 + i * 2 + 2, GRAPH_Y1, c);
        if (s->graphKind[i] & BS_GRAPH_HITCH) {
            fill(x0 + i * 2, GRAPH_Y1 - h, x0 + i * 2 + 2, GRAPH_Y1 - h + 2, WHITE);
        }
    }
    fill(2, GRAPH_Y1 - (int) (16.7f * GRAPH_PX_PER_MS), BS_WIDTH - 2, GRAPH_Y1 - (int) (16.7f * GRAPH_PX_PER_MS) + 1,
         (Colour){ 255, 255, 255, 90 });
    fill(2, GRAPH_Y1 - (int) (33.3f * GRAPH_PX_PER_MS), BS_WIDTH - 2, GRAPH_Y1 - (int) (33.3f * GRAPH_PX_PER_MS) + 1,
         (Colour){ 255, 255, 255, 60 });
    mono_text(4, 1, "60", (Colour){ 255, 255, 255, 140 });
    fill(2, GRAPH_Y1, BS_WIDTH - 2, GRAPH_Y1 + 1, BOX_EDGE);
}

static void ghome(const State *s);
static void gdebug(const State *s);
static void gpanel_text(const char *t, int withButton);
static int sBtnLastThird; /* the one button drawn in the bar's last third (beside a progress panel) */
#ifndef __3DS__
static int sGlyphSheet; /* host: the digit check sheet (bs_grid_glyph_sheet) instead of the home screen */
static void gglyph_sheet(void);
void bs_grid_glyph_sheet(int on) {
    sGlyphSheet = on;
}
static int sPlayerPreview; /* host: the home as a player build shows it (bs_preview_player_build) */
void bs_preview_player_build(int on) {
    sPlayerPreview = on;
}
#endif
static void draw_home(const State *s) {
#ifndef __3DS__
    if (sGrid && sGlyphSheet) {
        gglyph_sheet();
        return;
    }
#endif
    if (sGrid && s->overlayLines == 0 && sLogo.rgba != NULL) {
        ghome(s);
        return;
    }
    if (sGrid && s->overlayLines > 0 && sLogo.rgba != NULL) {
        gdebug(s); /* the profiler and the live monitor, on the grid */
        return;
    }
    if (s->overlayLines > 0 && s->graphCount > 0) {
        // The live monitor: the frame graph over the profiler's page.
        draw_graph(s);
        draw_overlay(s, GRAPH_Y1 + 2, BS_BUTTONS_Y);
        return;
    }
    if (sGrid && sLogo.rgba == NULL && s->overlayLines == 0) {
        /* before the ROM: the title (44) and the messages panel (4 + 10 a line + 4) centred as one group in 8..196,
           4 px apart, so no band is left empty under them */
        int count, h;
        sMeasureOnly = 1;
        sTitleY = 8;
        count = draw_messages(s, 56, 196);
        sMeasureOnly = 0;
        h = 44 + 4 + 10 + ((count > 0 ? count : 1) - 1) * 14 + 8 + 10;
        sTitleY = 8 + (188 - h) / 2;
        draw_logo();
        draw_messages(s, sTitleY + 48, 196);
        sTitleY = 8;
        return;
    }
    draw_logo();
    if (s->overlayLines > 0) {
        draw_messages(s, 70, 98);
        draw_overlay(s, 100, BS_BUTTONS_Y);
    } else if (sLogo.rgba == NULL) {
        draw_messages(s, sGrid ? 56 : 70, sGrid ? 196 : BS_BUTTONS_Y - 1); // No ROM yet (grid: under the title lines): what is happening, or why it can't start.
    } else {
        // The controls card fills the screen under the logo; a debug build keeps the newest message on its last line.
        draw_controls(72, BS_BUTTONS_Y - 4);
        if (PLAT_DEBUG_TOOLS && font_ready(FONT_SMALL)) {
            const char *msg = newest_message(s);
            char shortMsg[160];
            if (sFixTexts && text_width(FONT_SMALL, msg) > BS_WIDTH - 24) {
                /* Too long for the line: leave out the branch name (the word with a slash), keeping the rest. */
                const char *slash = strchr(msg, '/');
                if (slash != NULL) {
                    const char *start = slash, *end = slash;
                    while (start > msg && start[-1] != ' ') {
                        start--;
                    }
                    while (*end != '\0' && *end != ' ') {
                        end++;
                    }
                    while (*end == ' ') {
                        end++;
                    }
                    snprintf(shortMsg, sizeof(shortMsg), "%.*s%s", (int) (start - msg), msg, end);
                    msg = shortMsg;
                }
            }
            if (0) {
            } else {
                text_fit(FONT_SMALL, 12, BS_WIDTH - 12, BS_BUTTONS_Y - 18, msg, DIM, 1);
            }
        }
    }
}

/*
 * A value of "On" or "Off" is shown as a switch with the game's labels: ON lit, or OFF dim (both side by side, the
 * chosen one lit, read the wrong way round: OFF looked brighter when off).
 */
static int draw_on_off(const char *value, int x, int y) {
    int on = strcmp(value, "On") == 0;
    if (port_text_french()) {
        return 0; // The labels say ON and OFF: the value is written out (OUI, NON) instead.
    }
    // Of the game's two versions of each label, the one with white letters is the brighter one; OFF gets the other,
    // greyed.
    const Picture *label = on ? &sOnOff[0][0] : &sOnOff[1][1];
    if ((!on && strcmp(value, "Off") != 0) || label->rgba == NULL) {
        return 0;
    }
    if (sStyle == 1 || sStyle == 3) {
        // A switch: gold with the knob right when on, dim with the knob left when off.
        Colour track = on ? (Colour){ 245, 170, 30, 255 } : (Colour){ 20, 30, 80, 200 };
        smooth_box(x, y + 2, x + 38, y + 18, 8, track, track, (Colour){ 255, 255, 255, on ? 120 : 70 }, 1);
        round_disc(on ? x + 30 : x + 8, y + 10, 6, on ? WHITE : (Colour){ 150, 160, 195, 255 });
        return 1;
    }
    if (sStyle == 2) {
        // Broadcast: the game's orange label gives white letters only 2.2:1, so the badge is drawn darker.
        Colour fill1 = on ? (Colour){ 168, 98, 6, 255 } : (Colour){ 54, 60, 92, 255 };
        const char *word = on ? "ON" : "OFF";
        smooth_box(x, y + 1, x + label->width, y + label->height, 4, fill1, fill1, (Colour){ 255, 255, 255, 60 }, 1);
        text_centred_as_is(FONT_SMALL, x, x + label->width, y + 5, word, on ? WHITE : (Colour){ 196, 202, 226, 255 }, 0);
        return 1;
    }
    picture_part(label, 0, 0, label->width, label->height, x, y, on ? WHITE : (Colour){ 150, 150, 160, 255 });
    return 1;
}

static void arrow(int left, int x, int y) {
    const Picture *p = left ? &sArrowLeft : &sArrowRight;
    if (p->rgba != NULL) {
        picture(p, x, y);
    } else {
        text(FONT_MONO, x, y + 1, left ? "<" : ">", YELLOW, 1);
    }
}

/* The fun font's capitals (the game's headings, red edged in yellow), centred between x0 and x1 or from x0. */
static void heading(int x0, int x1, int y, const char *s, int centred) {
    char upper[40];
    upper_text(upper, sizeof(upper), s);
    if (centred) {
        text_centred_as_is(FONT_FUN, x0, x1, y, upper, WHITE, 1);
    } else {
        text_clipped_as_is(FONT_FUN, x0, y, upper, WHITE, 1, x1);
    }
}

static const Colour PICKED_FILL = { 255, 200, 40, 70 };
static const Colour PICKED_EDGE = { 255, 214, 40, 255 };

/* A value between the game's arrows in a chip from x0 to x1: its left half steps back, its right half forward. */
static void stepper(int x0, int x1, int y, const char *value) {
    int arrowWidth = sArrowLeft.rgba != NULL ? sArrowLeft.width : 8;
    int arrowY = y + 10 - (sArrowLeft.rgba != NULL ? sArrowLeft.height : 8) / 2;
    int inset = (sStyle == 1 || sStyle == 3) ? 8 : 6, widened = 0;
    {
        int need = text_width(FONT_SMALL, value) + 2 * arrowWidth + 2 * inset;
        if (need > x1 - x0 && x1 - need >= x0 - 28) {
            x0 = x1 - need; /* a long value widens its chip leftwards instead of being cut */
            widened = sStyle != 1;
        }
    }
    if (sVariant == 1) {
        fill(x0, y + 2, x1, y + STRIP_CHIP_H, (Colour){ 12, 14, 18, 255 }); /* inside the row, no border of its own */
    } else {
        box(x0, y, x1, y + 20, (Colour){ 0, 0, 20, 110 }, (Colour){ 140, 180, 255, 110 });
    }
    arrow(1, x0 + 4, arrowY);
    arrow(0, x1 - 4 - arrowWidth, arrowY);
    // The value's capitals centred on the chip's middle line, whatever the look's font height (the game's font: y + 6).
    const UiFont *u = ui_font(FONT_SMALL);
    int textY = u != NULL ? y + 11 - (ui_cap_middle2(u) + 1) / 2 : y + 6;
    text_fit_centred(FONT_SMALL, x0 + arrowWidth + inset - widened, x1 - arrowWidth - inset - widened, textY, value, YELLOW, 1);
}

/*
 * The menu's look, from the mockups (design B, "tabs"): the pages as tabs down the left in the game's heading font,
 * the chosen one lit in the HUD's gold; on the right a panel with the page's title, its rows
 * (each a label and a value: between arrows, the game's ON or OFF, or an arrow for a row that does something), the
 * row touched last lit, and under the rows that row's tip.
 */
#define MENU_PANEL_X0 (BS_MENU_TAB_X1)
#define MENU_PANEL_X1 (BS_WIDTH - 4)
#define MENU_TIP_Y (BS_MENU_FIRST_Y + BS_MENU_ROWS * BS_MENU_ROW_HEIGHT)

static void draw_menu_strip(const State *s);
static void gmenu(const State *s);
static void gbuttons(const char (*buttons)[40], int count);

/* Scratch: a page with fewer rows than the menu holds shows its tip in a card filling the space under them. */
static int tip_card(const BsMenu *m, int x0, int x1, int top, int bottom, const char *tip) {
    int lines, pitch = line_pitch(), y;
    if (!sTipCard || m->rowCount >= BS_MENU_ROWS) {
        return 0;
    }
    box(x0, top, x1, bottom, (Colour){ 0, 0, 20, 70 }, (Colour){ 140, 180, 255, 90 });
    {
        int clipEnd = sClipEnd, mid = (x0 + x1) / 2, half = (x1 - x0 - 12) / 2, k, best;
        sClipEnd = -1;
        lines = wrapped_centred(mid - half, mid + half, 0, tip, HINT) / pitch;
        best = half;
        if (lines > 1) { /* balance the lines: narrow the text while it still takes the same number of lines */
            for (k = half - 3; k > 40; k -= 3) {
                if (wrapped_centred(mid - k, mid + k, 0, tip, HINT) / pitch > lines) {
                    break;
                }
                best = k;
            }
        }
        sClipEnd = clipEnd;
        x0 = mid - best - 6;
        x1 = mid + best + 6;
    }
    if (lines < 1) {
        lines = 1;
    }
    y = top + (bottom - top - lines * pitch) / 2 + 1;
    wrapped_centred(x0 + 6, x1 - 6, y, tip, HINT);
    return 1;
}

static void draw_menu(const State *s) {
    const BsMenu *m = &s->menu;
    int i, tabHeight = BS_MENU_TAB_HEIGHT(m->tabCount);
    const char *tip = m->note;

    if (sGrid) {
        gmenu(s);
        return;
    }
    if (sStyle >= 2) {
        draw_menu_strip(s);
        return;
    }

    box(MENU_PANEL_X0, BS_MENU_TAB_Y0, MENU_PANEL_X1, BS_MENU_TAB_Y1, BOX_FILL, BOX_EDGE);
    for (i = 0; i < m->tabCount && i < BS_MENU_TABS; i++) {
        int y = BS_MENU_TAB_Y0 + i * tabHeight;
        if (i == m->tab) {
            box(4, y, MENU_PANEL_X0 - 4, y + tabHeight - 2, (Colour){ 255, 200, 40, 90 }, PICKED_EDGE);
        } else {
            box(4, y, MENU_PANEL_X0 - 4, y + tabHeight - 2, (Colour){ 0, 8, 40, 70 }, (Colour){ 140, 180, 255, 80 });
        }
        char upper[40];
        upper_text(upper, sizeof(upper), m->tabs[i]);
        /* A long name (French ones) that would touch the tab's edges drops to the small font. */
        sNoGap = 1;
        if (font_ready(FONT_FUN) && text_width_as_is(FONT_FUN, upper) <= MENU_PANEL_X0 - 12) {
            sNoGap = 0;
            sNoGap = text_width_as_is(FONT_FUN, upper) > MENU_PANEL_X0 - 12;
            heading(4, MENU_PANEL_X0 - 2, y + tabHeight / 2 - 8 + sTabNudge[sStyle], m->tabs[i], 1);
            sNoGap = 0;
        } else if (font_ready(FONT_FUN)) {
            sNoGap = 0;
            text_centred_as_is(FONT_SMALL, 4, MENU_PANEL_X0 - 4, y + tabHeight / 2 - 6 + (sStyle != 0 ? 1 : 0), upper,
                               i == m->tab ? YELLOW : WHITE, 1);
        } else {
            sNoGap = 0;
            text_clipped(FONT_SMALL, 10, y + tabHeight / 2 - 6, m->tabs[i], i == m->tab ? YELLOW : WHITE, 1,
                         MENU_PANEL_X0 - 4);
        }
    }
    heading(MENU_PANEL_X0 + 12, MENU_PANEL_X1 - 40, 11, m->title, 0);
    if (m->pages[0] != '\0') {
        text(FONT_SMALL, MENU_PANEL_X1 - 8 - text_width(FONT_SMALL, m->pages), 13, m->pages, HINT, 1);
    }
    for (i = 0; i < m->rowCount && i < BS_MENU_ROWS; i++) {
        const BsMenuRow *row = &m->rows[i];
        int y = BS_MENU_FIRST_Y + i * BS_MENU_ROW_HEIGHT, labelEnd = BS_MENU_VALUE_X - 4;
        if (i == m->picked) {
            box(MENU_PANEL_X0 + 6, y, MENU_PANEL_X1 - 4, y + BS_MENU_ROW_HEIGHT - 2, PICKED_FILL, PICKED_EDGE);
            if (row->hint[0] != '\0') {
                tip = row->hint;
            }
        } else {
            fill(MENU_PANEL_X0 + 12, y + BS_MENU_ROW_HEIGHT - 2, MENU_PANEL_X1 - 10, y + BS_MENU_ROW_HEIGHT - 1,
                 (Colour){ 140, 180, 255, 50 });
        }
        if (row->value[0] == '\0') {
            arrow(0, MENU_PANEL_X1 - 20, y + 7);
            labelEnd = MENU_PANEL_X1 - 24;
        } else if (draw_on_off(row->value, MENU_PANEL_X1 - 48, y + 3)) {
            labelEnd = MENU_PANEL_X1 - 52;
        } else {
            stepper(BS_MENU_VALUE_X, MENU_PANEL_X1 - 8, y + 3, row->value);
        }
        text_fit(FONT_SMALL, MENU_PANEL_X0 + 14, labelEnd, y + 8, row->label, WHITE, 1);
    }
    if (!tip_card(m, MENU_PANEL_X0 + 6, MENU_PANEL_X1 - 4, BS_MENU_FIRST_Y + m->rowCount * BS_MENU_ROW_HEIGHT + 2,
                  MENU_TIP_Y + 28, tip)) {
        fill(MENU_PANEL_X0 + 8, MENU_TIP_Y - 1, MENU_PANEL_X1 - 6, MENU_TIP_Y, (Colour){ 140, 180, 255, 80 });
        text_fit(FONT_SMALL, MENU_PANEL_X0 + 12, MENU_PANEL_X1 - 8, MENU_TIP_Y + 11, tip, HINT, 1);
    }
}

/*
 * Broadcast and Dock lay the Settings menu out as the mockups do: the pages as a strip of tabs across the top (two
 * rows when there are more than five), the rows full width below, the tip under them. Taps use bs_menu_hit.
 */
#define STRIP_TAB_Y0 4
#define STRIP_TAB_H 20
#define STRIP_TAB_PITCH 22
#define STRIP_ROW_Y0 50
#define STRIP_ROW_H 21
#define STRIP_TIP_Y (STRIP_ROW_Y0 + BS_MENU_ROWS * STRIP_ROW_H + 3)

static int strip_perrow(int tabCount) {
    return tabCount > 5 ? (tabCount + 1) / 2 : (tabCount > 0 ? tabCount : 1);
}

static int grid_menu_hit(int x, int y, int tabCount, int *index, int *stepBack);

int bs_menu_hit(int x, int y, int tabCount, int *index, int *stepBack) {
    *index = 0;
    *stepBack = 0;
    if (sGrid) {
        return grid_menu_hit(x, y, tabCount, index, stepBack);
    }
    if (sStyle >= 2) {
        int perRow = strip_perrow(tabCount), w = (BS_WIDTH - 12) / perRow;
        if (y >= STRIP_TAB_Y0 && y < STRIP_ROW_Y0 - 2) {
            int row = (y - STRIP_TAB_Y0) / STRIP_TAB_PITCH, col = (x - 6) / w, tab = row * perRow + col;
            if (x >= 6 && col < perRow && tab < tabCount) {
                *index = tab;
                return BS_MENU_HIT_TAB;
            }
            return BS_MENU_HIT_NONE;
        }
        if (y >= STRIP_ROW_Y0 && y < STRIP_ROW_Y0 + BS_MENU_ROWS * STRIP_ROW_H) {
            *index = (y - STRIP_ROW_Y0) / STRIP_ROW_H;
            *stepBack = x >= BS_MENU_VALUE_X && x < BS_MENU_VALUE_SPLIT;
            return BS_MENU_HIT_ROW;
        }
        return BS_MENU_HIT_NONE;
    }
    if (x < BS_MENU_TAB_X1) {
        int slot = (y - BS_MENU_TAB_Y0) / BS_MENU_TAB_HEIGHT(tabCount);
        if (y >= BS_MENU_TAB_Y0 && slot < tabCount) {
            *index = slot;
            return BS_MENU_HIT_TAB;
        }
        return BS_MENU_HIT_NONE;
    }
    if (y >= BS_MENU_FIRST_Y) {
        int slot = (y - BS_MENU_FIRST_Y) / BS_MENU_ROW_HEIGHT;
        if (slot < BS_MENU_ROWS) {
            *index = slot;
            *stepBack = x >= BS_MENU_VALUE_X && x < BS_MENU_VALUE_SPLIT;
            return BS_MENU_HIT_ROW;
        }
    }
    return BS_MENU_HIT_NONE;
}

static void upper_case(const char *in, char *out, int size);

static void draw_menu_strip(const State *s) {
    const BsMenu *m = &s->menu;
    int i, perRow = strip_perrow(m->tabCount), w = (BS_WIDTH - 12) / perRow;
    const char *tip = m->note;
    int L = sVariant == 1 ? 4 : 6, R = sVariant == 1 ? 316 : 314; /* the rows' edges; Carbon lines up with the race panels (4 to 316) */
    Colour red = { 235, 40, 40, 255 }, orange = { 255, 150, 20, 255 };
    for (i = 0; i < m->tabCount && i < BS_MENU_TABS; i++) {
        int row = i / perRow, col = i % perRow, x0 = 6 + col * w, y0 = STRIP_TAB_Y0 + row * STRIP_TAB_PITCH;
        int carbonW = 0; /* Carbon: tabs 3 px apart, the grid spanning exactly the rows' width (6 to 314) */
        if (sVariant == 1) {
            int slot = R - L + 3; /* equal tabs, equal 3 px gaps, whole pixels */
            x0 = L + col * slot / perRow;
            carbonW = L + (col + 1) * slot / perRow - 3 - x0;
        }
        int lit = i == m->tab;
        char upper[40];
        Colour ink = lit ? (sStyle == 3 ? (Colour){ 60, 30, 0, 255 } : WHITE) : (Colour){ 190, 205, 240, 255 };
        if (sStyle == 2) {
            int x1 = sVariant == 1 ? x0 + carbonW : x0 + w - 2;
            if (lit) {
                slab(x0, y0, x1, y0 + STRIP_TAB_H, 5, red, orange);
            } else {
                slab(x0, y0, x1, y0 + STRIP_TAB_H, 5, (Colour){ 30, 44, 110, 235 }, (Colour){ 22, 32, 92, 235 });
            }
        } else if (lit) {
            smooth_box(x0, y0, x0 + w - 2, y0 + STRIP_TAB_H, 10, (Colour){ 255, 226, 90, 255 }, (Colour){ 235, 150, 10, 255 },
                       (Colour){ 255, 255, 255, 150 }, 1);
        }
        {
            const char *label = port_text(m->tabs[i]);
            if (sFixTexts && perRow > 4 && strcmp(m->tabs[i], "Race HUD") == 0) {
                label = "HUD"; /* the full name does not fit five to a row without losing its space */
            }
            upper_case(label, upper, sizeof(upper));
            text_fit_as_is(FONT_SMALL, x0 + 3, x0 + (sVariant == 1 ? carbonW + 2 : w) - 5, y0 + 6, sStyle == 2 && sVariant != 1 ? upper : label, ink, !lit, 1);
        }
    }
    for (i = 0; i < m->rowCount && i < BS_MENU_ROWS; i++) {
        const BsMenuRow *row = &m->rows[i];
        int y = STRIP_ROW_Y0 + i * STRIP_ROW_H, labelEnd = BS_WIDTH - 10, hot = i == m->picked;
        Colour ink = WHITE;
        if (hot) {
            if (sStyle == 2) {
                slab(L, y, R, y + STRIP_ROW_H - 2, 5, (Colour){ 255, 240, 220, 245 }, (Colour){ 255, 220, 170, 245 });
                ink = sVariant == 1 ? WHITE : (Colour){ 20, 20, 50, 255 };
            } else {
                smooth_box(L, y, R, y + STRIP_ROW_H - 2, 10, (Colour){ 255, 255, 255, 40 }, (Colour){ 255, 255, 255, 40 },
                           PICKED_EDGE, 1);
            }
            if (row->hint[0] != '\0') {
                tip = row->hint;
            }
        } else if (sStyle == 2) {
            slab(L, y, R, y + STRIP_ROW_H - 2, 5, (Colour){ 26, 38, 96, 235 }, (Colour){ 26, 38, 96, 235 });
        } else {
            fill(18, y + STRIP_ROW_H - 2, 302, y + STRIP_ROW_H - 1, (Colour){ 255, 255, 255, 30 });
        }
        if (row->value[0] == '\0') {
            arrow(0, 296, y + 4);
            labelEnd = 290;
        } else if (draw_on_off(row->value, sVariant == 1 ? R - 40 : BS_WIDTH - (sFixTexts ? 46 : 52), y)) {
            labelEnd = BS_WIDTH - 58;
        } else {
            stepper(BS_MENU_VALUE_X, sVariant == 1 ? R - 4 : BS_WIDTH - 10, y, row->value);
            labelEnd = BS_MENU_VALUE_X - 4;
        }
        text_fit(FONT_SMALL, 18, labelEnd, y + 5, row->label, ink, sStyle != 2 || !hot);
    }
    if (!tip_card(m, L, R, STRIP_ROW_Y0 + m->rowCount * STRIP_ROW_H + 2, BS_BUTTONS_Y - 4, tip)) {
        fill(10, STRIP_TIP_Y - 1, BS_WIDTH - 10, STRIP_TIP_Y, (Colour){ 255, 255, 255, 40 });
        text_fit(FONT_SMALL, 14, m->pages[0] != '\0' ? BS_WIDTH - 50 : BS_WIDTH - 12, STRIP_TIP_Y + 9, tip, HINT, 1);
    }
    if (m->pages[0] != '\0') {
        text(FONT_SMALL, BS_WIDTH - 10 - text_width(FONT_SMALL, m->pages), STRIP_TIP_Y + 9, m->pages, HINT, 1);
    }
}

/* Small icons for the new styles' buttons, drawn smooth in a 14 pixel square around (cx, cy). */
static void line_px(int x0, int y0, int x1, int y1, Colour c) {
    int i, n = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    for (i = 0; i <= n; i++) {
        int x = x0 + (x1 - x0) * i / (n ? n : 1), y = y0 + (y1 - y0) * i / (n ? n : 1);
        fill(x, y, x + 2, y + 2, c);
    }
}

/* Returns 1 if the label has an icon. */
static int draw_icon(const char *label, int cx, int cy, Colour c, Colour hole) {
    int i;
    if (strcmp(label, "Pause") == 0) {
        bar(cx - 5, cy - 6, cx - 1, cy + 6, c);
        bar(cx + 2, cy - 6, cx + 6, cy + 6, c);
    } else if (strcmp(label, "Settings") == 0) {
        static const signed char sTeeth[8][2] = { { 0, -6 }, { 4, -4 }, { 6, 0 }, { 4, 4 }, { 0, 6 }, { -4, 4 }, { -6, 0 }, { -4, -4 } };
        for (i = 0; i < 8; i++) {
            round_disc(cx + sTeeth[i][0], cy + sTeeth[i][1], 2, c);
        }
        round_disc(cx, cy, 5, c);
        round_disc(cx, cy, 2, hole);
    } else if (strcmp(label, "Controls") == 0) {
        smooth_box(cx - 9, cy - 5, cx + 9, cy + 5, 5, c, c, (Colour){ 0, 0, 0, 0 }, 1);
        bar(cx - 6, cy - 1, cx - 2, cy + 1, hole);
        bar(cx - 5, cy - 2, cx - 3, cy + 2, hole);
        round_disc(cx + 4, cy - 1, 1, hole);
        round_disc(cx + 6, cy + 1, 1, hole);
    } else if (strcmp(label, "Updates") == 0) {
        bar(cx - 1, cy - 7, cx + 2, cy + 1, c);
        line_px(cx - 5, cy - 1, cx, cy + 5, c);
        line_px(cx + 5, cy - 1, cx, cy + 5, c);
        bar(cx - 7, cy + 6, cx + 8, cy + 8, c);
    } else if (strcmp(label, "Close") == 0) {
        line_px(cx - 5, cy - 5, cx + 5, cy + 5, c);
        line_px(cx + 5, cy - 5, cx - 5, cy + 5, c);
    } else if (strcmp(label, "Prev") == 0) {
        line_px(cx + 3, cy - 6, cx - 3, cy, c);
        line_px(cx - 3, cy, cx + 3, cy + 6, c);
    } else if (strcmp(label, "Next") == 0) {
        line_px(cx - 3, cy - 6, cx + 3, cy, c);
        line_px(cx + 3, cy, cx - 3, cy + 6, c);
    } else {
        return 0;
    }
    return 1;
}

static void upper_case(const char *in, char *out, int size) {
    int j;
    for (j = 0; in[j] != '\0' && j < size - 1; j++) {
        out[j] = (in[j] >= 'a' && in[j] <= 'z') ? in[j] - 32 : in[j];
    }
    out[j] = '\0';
}

/* The buttons in one of the newer styles: pills (Glass), slanted slabs (Broadcast), a dock of icons (Dock). */
/* Equal buttons across 4 to 316 (the panels' width) with equal gaps: 6 px for two or three, 4 for four. */
static void button_span(int i, int count, int *x0, int *x1) {
    int gap = count >= 4 ? 4 : 6, w = (312 - gap * (count - 1)) / count;
    *x0 = 4 + i * (w + gap);
    *x1 = *x0 + w;
}

static void draw_buttons_styled(const char (*buttons)[40], int count) {
    int i;
    int y0 = BS_BUTTONS_Y + 2, y1 = BS_STATUS_Y - 3;

    if (sStyle == 3) {
        fill(0, BS_BUTTONS_Y, BS_WIDTH, BS_STATUS_Y,
             sVariant == 2 ? (Colour){ 2, 36, 36, 235 } : sVariant == 3 ? (Colour){ 52, 14, 62, 235 } : (Colour){ 8, 16, 60, 235 });
        fill(0, BS_BUTTONS_Y, BS_WIDTH, BS_BUTTONS_Y + 1, (Colour){ 255, 255, 255, 60 });
    } else if (sStyle == 2) {
        fill(0, BS_BUTTONS_Y, BS_WIDTH, BS_STATUS_Y, (Colour){ 6, 9, 30, 255 });
    }
    for (i = 0; i < count; i++) {
        const char *label = buttons[i];
        int grey = label[0] == '-', on = label[0] == '+', x0, x1;
        button_span(i, count, &x0, &x1);
        const char *t = label + (grey || on);
        char upper[40];
        Colour ink = grey ? (Colour){ 130, 135, 160, 255 } : WHITE;
        if (grey && label[1] == '\0') {
            continue;
        }
        if (sStyle == 3 && sDockOutline) {
            smooth_box(x0, y0, x1, y1, 11, (Colour){ 255, 255, 255, grey ? 6 : 20 }, (Colour){ 255, 255, 255, grey ? 6 : 20 },
                       (Colour){ 255, 255, 255, grey ? 40 : 110 }, 1);
        }
        if (sStyle == 1) {
            Colour top = on ? (Colour){ 110, 230, 110, 255 } : (Colour){ 52, 104, 216, 255 };
            Colour bottom = on ? (Colour){ 30, 150, 40, 255 } : (Colour){ 30, 68, 196, 255 };
            if (sVariant == 3 && !on) {
                top = (Colour){ 196, 78, 70, 255 };
                bottom = (Colour){ 160, 48, 64, 255 };
            } else if (sVariant == 2 && !on) {
                top = (Colour){ 14, 122, 100, 255 };
                bottom = (Colour){ 8, 94, 86, 255 };
            }
            if (grey) {
                top = (Colour){ 130, 135, 155, 255 };
                bottom = (Colour){ 80, 85, 105, 255 };
            }
            smooth_box(x0, y0, x1, y1, 11, top, bottom, (Colour){ 255, 255, 255, grey ? 80 : 150 }, 1);
        } else if (sStyle == 2) {
            Colour top = (Colour){ 52, 74, 168, 255 };
            Colour bottom = (Colour){ 36, 54, 134, 255 };
            if (on) {
                top = (Colour){ 60, 200, 90, 255 };
                bottom = (Colour){ 40, 160, 70, 255 };
            } else if (grey) {
                top = (Colour){ 70, 74, 100, 255 };
                bottom = (Colour){ 54, 58, 84, 255 };
            }
            slab(x0, y0 + 1, x1, y1, 8, top, bottom);
        } else if (on) {
            smooth_box(x0 + 8, y0 + 1, x1 - 8, y0 + 3, 1, (Colour){ 90, 220, 90, 255 }, (Colour){ 90, 220, 90, 255 },
                       (Colour){ 0, 0, 0, 0 }, 1);
        }
        {
            int cy = (y0 + y1) / 2;
            int w;
            // The language first, then capitals: port_text only knows the words as the menus write them.
            upper_case(port_text(t), upper, sizeof(upper));
            w = text_width(FONT_FUN, upper);
            if (sStyle == 3) {
                Colour dockInk = grey ? ink : (on ? (Colour){ 120, 235, 120, 255 } : WHITE);
                char title[40];
                int w2, iconW = 18;
                // Title case, as Settings, Controls and Updates are written: "master" reads "Master".
                snprintf(title, sizeof(title), "%s", port_text(t));
                if (title[0] >= 'a' && title[0] <= 'z') {
                    title[0] -= 'a' - 'A';
                }
                w2 = text_width_as_is(FONT_SMALL, title);
                int tx = x0 + (x1 - x0 - w2 - iconW) / 2;
                if (!draw_icon(t, tx + 7, cy, dockInk, (Colour){ 8, 16, 60, 255 })) {
                    iconW = 0;
                    tx = x0 + (x1 - x0 - w2) / 2;
                }
                text_clipped_as_is(FONT_SMALL, tx + iconW, cy - 5, title, dockInk, 0, x1);
            } else {
                int iconW = 0;
                Colour hole = sStyle == 1 ? (Colour){ 40, 100, 235, 255 } : (Colour){ 245, 90, 30, 255 };
                if (strcmp(t, "Pause") == 0 || strcmp(t, "Settings") == 0 || strcmp(t, "Controls") == 0 ||
                    strcmp(t, "Updates") == 0 || strcmp(t, "Close") == 0 || strcmp(t, "Prev") == 0 ||
                    strcmp(t, "Next") == 0) {
                    iconW = 20;
                }
                if (sStyle == 2 && !(i == 0 && !grey)) {
                    hole = (Colour){ 40, 60, 140, 255 };
                }
                /* A label (with its icon) that would run into the button's rounded ends drops the letter gap. */
                if (w + iconW > x1 - x0 - 14) {
                    sNoGap = 1;
                    w = text_width(FONT_FUN, upper);
                }
                if (sStyle == 2 && iconW && w + iconW > x1 - x0 - 26) {
                    // Broadcast's slabs lean 8 pixels at each end: a label that still would touch them loses its icon.
                    iconW = 0;
                }
                if (iconW) {
                    int tx = x0 + (x1 - x0 - w - iconW) / 2;
                    draw_icon(t, tx + 7, cy, ink, hole);
                    text_clipped(FONT_FUN, tx + iconW, y0 + 5 + sButtonNudge[sStyle], upper, ink, !grey, x1);
                } else {
                    text_centred(FONT_FUN, x0, x1, y0 + 5 + sButtonNudge[sStyle], upper, ink, !grey);
                }
                sNoGap = 0;
            }
        }
    }
}

/* Buttons in the style of the game's blue arrows: blue, edged in yellow; grey if they can't be tapped, green if on. */
static void draw_button_row(const char (*buttons)[40], int count) {
    int i;

    if (sGrid && !font_ready(FONT_FUN)) {
        /* before the ROM: the looks' own fonts are built in (Classic's too), so their bar is the same as after it */
        gbuttons(buttons, count);
        return;
    }
    if (sGrid && !font_ready(FONT_FUN)) {
        /* before the ROM, Classic: the game's fonts aren't loaded yet, so the bar's labels are in the console font */
        int n = 0, slot = 0, w;
        for (i = 0; i < count; i++) {
            n += buttons[i][0] != '\0' && strcmp(buttons[i], "-") != 0;
        }
        w = n > 0 ? (BS_WIDTH - 16 - 8 * (n - 1)) / n : 0;
        for (i = 0; i < count; i++) {
            const char *t = buttons[i] + (buttons[i][0] == '-' || buttons[i][0] == '+' || buttons[i][0] == '*');
            char up[40];
            int x0, len, k;
            if (buttons[i][0] == '\0' || strcmp(buttons[i], "-") == 0) {
                continue;
            }
            x0 = 8 + slot++ * (w + 8);
            box_classic(x0, 200, x0 + w, 232, (Colour){ 30, 68, 196, 255 }, (Colour){ 255, 214, 40, 255 });
            for (len = 0; t[len] != '\0' && len < 39; len++) {
                up[len] = (char) (t[len] >= 'a' && t[len] <= 'z' ? t[len] - 32 : t[len]);
            }
            up[len] = '\0';
            k = len * 8;
            mono_text(x0 + (w - k) / 2, 212, up, WHITE);
        }
        return;
    }
    if (sGrid) {
        gbuttons(buttons, count);
        return;
    }
    if (sStyle != 0) {
        draw_buttons_styled(buttons, count);
        return;
    }
    for (i = 0; i < count; i++) {
        const char *label = buttons[i];
        int grey = label[0] == '-', on = label[0] == '+', x0, x1, y, row;
        Colour top = { 70, 130, 255, 255 }, bottom = { 20, 50, 170, 255 }, edge = { 255, 214, 40, 255 };
        button_span(i, count, &x0, &x1);
        x0 += 1; /* the outline is drawn one pixel outside */
        x1 -= 1;
        if (grey && label[1] == '\0') {
            continue; // A gap.
        }
        if (grey) {
            top = (Colour){ 150, 150, 160, 255 };
            bottom = (Colour){ 90, 90, 104, 255 };
            edge = (Colour){ 190, 190, 200, 255 };
        } else if (on) {
            top = (Colour){ 90, 220, 90, 255 };
            bottom = (Colour){ 20, 130, 30, 255 };
        }
        // A dark outline, the yellow edge, then the fill getting darker downwards.
        box(x0 - 1, BS_BUTTONS_Y + 1, x1 + 1, BS_STATUS_Y - 2, (Colour){ 0, 0, 0, 0 }, (Colour){ 0, 0, 30, 220 });
        box(x0, BS_BUTTONS_Y + 2, x1, BS_STATUS_Y - 3, (Colour){ 0, 0, 0, 0 }, edge);
        for (y = BS_BUTTONS_Y + 3, row = 0; y < BS_STATUS_Y - 4; y++, row++) {
            int span = BS_STATUS_Y - 4 - (BS_BUTTONS_Y + 3);
            Colour c = { top.r + (bottom.r - top.r) * row / span, top.g + (bottom.g - top.g) * row / span,
                         top.b + (bottom.b - top.b) * row / span, 255 };
            fill(x0 + 1, y, x1 - 1, y + 1, c);
        }
        // A highlight along the top.
        fill(x0 + 2, BS_BUTTONS_Y + 3, x1 - 2, BS_BUTTONS_Y + 4, (Colour){ 255, 255, 255, 90 });
        if (font_ready(FONT_FUN)) {
            char upper[40];
            upper_text(upper, sizeof(upper), label + (grey || on));
            text_centred_as_is(FONT_FUN, x0, x1, BS_BUTTONS_Y + 6 + sButtonNudge[0], upper, grey ? (Colour){ 120, 120, 130, 255 } : WHITE,
                               !grey);
        } else {
            text_centred(FONT_SMALL, x0, x1, BS_BUTTONS_Y + 6, label + (grey || on), grey ? DIM : WHITE, !grey);
        }
    }
}

static void draw_buttons(const State *s) {
    if (s->setupShown) {
        // The setup's own: Back, the middle one and Next, a gap where one is left out.
        char labels[3][40];
        int i;
        for (i = 0; i < 3; i++) {
            snprintf(labels[i], sizeof(labels[i]), "%s", s->setup.buttons[i][0] != '\0' ? s->setup.buttons[i] : "-");
        }
        draw_button_row(labels, 3);
    } else if (s->buttonCount == 0 && sGrid) {
        gpanel_text(s->panelText, 0);
    } else if (s->buttonCount == 1 && s->panelText[0] != '\0' && sGrid) {
        /* a job that can be stopped: its progress over two thirds of the bar, its one button (Cancel) in the last */
        sBtnLastThird = 1;
        draw_button_row(s->buttons, 1);
        sBtnLastThird = 0;
        gpanel_text(s->panelText, 1);
    } else if (s->buttonCount == 0) {
        box(4, BS_BUTTONS_Y + 2, BS_WIDTH - 4, BS_STATUS_Y - 3, BOX_FILL, BOX_EDGE);
        text_centred(FONT_SMALL, 8, BS_WIDTH - 8, BS_BUTTONS_Y + 6, s->panelText, YELLOW, 1);
    } else {
        draw_button_row(s->buttons, s->buttonCount);
    }
}

static void draw_status(const State *s) {
    static const Colour sStatusColour = { 120, 140, 200, 255 };
    if (!PLAT_DEBUG_TOOLS) {
        return; // The watchdog's counters are for debugging: a player's build leaves the strip empty.
    }
    mono_text(4, BS_STATUS_Y, s->status[0], sStatusColour);
    mono_text(4, BS_STATUS_Y + 8, s->status[1], sStatusColour);
}

/******************************** The race HUD ********************************/

/*
 * During a race (race_hud.h) the top area shows the race in the layout chosen from the mockups ("standings tower"):
 *   x 4-92     the racers in race order, each as the game's portrait with the time gap to the player
 *   x 96-186   the item held, then the bananas and the last and best lap times
 *   x 190-316  the position, lap and race time, then the track's map with every racer on it
 * Everything is the game's own art at its own size, so nothing is blurred. The panels and the map,
 * which only change with the track, are drawn once into sRaceBase; each update draws the rest over a copy of it.
 */
#define RACE_ROWS_Y BS_BUTTONS_Y
#define TOWER_X0 4
#define TOWER_X1 92
#define MIDDLE_X0 96
#define MIDDLE_X1 186
#define RIGHT_X0 190
#define RIGHT_X1 316
#define UPPER_Y0 4
#define UPPER_Y1 92
#define LOWER_Y0 96
#define LOWER_Y1 198
#define TOWER_ROW_Y 6
#define TOWER_ROW_HEIGHT 24

/* The race art from the ROM, loaded with the fonts. */
static Picture sPortraits[10]; /* By the game's Character, halved to 20x20. */
static Picture sItems[15];     /* balloon_type * 3 + balloon_level. */
static Picture sItemCounts[10];
static Picture sPlaces[8], sSuffixes[4];
static Picture sDigits[10], sSmallDigits[10], sColon, sSmallColon, sSlash;
static Picture sLapLabel, sTimeLabel, sFinalLabel, sLapSmallLabel, sFinishLabel, sBanana, sTimes;
static Picture sEggWhole; /* the egg object's two textures as one 32x40 picture */
static Picture sGhostIcon, sTimesIcon, sEggIcon, sEgg, sCoin, sBalloon, sCross, sSmallBanana, sTinyBanana;
static Picture sTrophy[2], sKey[2], sTTAmulet[5], sWizpigAmulet[5]; /* [not collected, collected]; by pieces. */

/* The minimap's colour of each character (game_ui.c, gHudMinimapColours). */
static const Colour sCharacterColours[10] = {
    { 255, 160, 0, 255 }, { 255, 255, 0, 255 }, { 0, 128, 255, 255 },   { 255, 255, 255, 255 }, { 0, 175, 0, 255 },
    { 0, 255, 128, 255 }, { 255, 0, 0, 255 },   { 255, 128, 190, 255 }, { 128, 128, 128, 255 }, { 0, 0, 255, 255 },
};

static const Colour GOLD = { 255, 214, 40, 255 };
#define AHEAD (sVariant == 3 ? (Colour){ 255, 170, 176, 255 } : (Colour){ 255, 150, 140, 255 })
static const Colour BEHIND = { 140, 255, 160, 255 };
static const Colour LABEL_BLUE = { 170, 195, 240, 255 };

/* The track's map, drawn smooth at the size its panel allows, with its offsets, for the sprite in sMapSprite. */
static Picture sMap;
static const unsigned char *sMapBitmap; /* The game's own map picture, if the level has no sprite. */
static int sMapBitmapId;                /* Its RaceHud.mapBitmapId. */
static int sMapSprite = -2, sMapMirrored, sMapX, sMapY, sMapTexX, sMapTexY, sMapTexWidth;
static int sMapLane = 99; /* the grid: the track's typical width on screen, which the markers must fit inside */
static int sArrowSmall; /* the map's bends are too tight somewhere for the big arrow: the small one, for the whole race */
static unsigned char sMapColour[3];
static int sMapBox[4]; /* The panel the map is fitted to: x0, y0, x1, y1. */
static float sMapScale;
static unsigned short sRaceBase[BS_WIDTH * RACE_ROWS_Y];
static int sRaceBaseMade;

/*
 * The banana counter's sprite is one picture in two pieces (the stem and upper half, then the lower curve), each
 * placed by its offset in the sprite.
 */
static void load_banana(void) {
    Picture part[2];
    int x[2], y[2], i, x0, y0, x1, y1, px, py;
    for (i = 0; i < 2; i++) {
        if (!load_texture_frame(ASSET_TEX2D_HUD_BANANAICON_0 + i, 0, &part[i], &x[i], &y[i])) {
            if (i == 1) {
                free(part[0].rgba);
            }
            return;
        }
    }
    x0 = x[0] < x[1] ? x[0] : x[1];
    y0 = y[0] < y[1] ? y[0] : y[1];
    x1 = x[0] + part[0].width > x[1] + part[1].width ? x[0] + part[0].width : x[1] + part[1].width;
    y1 = y[0] + part[0].height > y[1] + part[1].height ? y[0] + part[0].height : y[1] + part[1].height;
    sBanana.width = x1 - x0;
    sBanana.height = y1 - y0;
    sBanana.rgba = calloc((unsigned long) sBanana.width * sBanana.height, 4);
    for (i = 0; i < 2; i++) {
        for (py = 0; sBanana.rgba != NULL && py < part[i].height; py++) {
            for (px = 0; px < part[i].width; px++) {
                const unsigned char *t = part[i].rgba + (py * part[i].width + px) * 4;
                if (t[3] != 0) {
                    memcpy(sBanana.rgba + ((y[i] - y0 + py) * sBanana.width + x[i] - x0 + px) * 4, t, 4);
                }
            }
        }
        free(part[i].rgba);
    }
}

/*
 * The ROM's colons are two short bars a single dark row apart (the small one's are 2x1 pixels), which read as "=" on
 * this screen. These are two dots shaded like the digits: each row takes the colours of the same row of the digit 1
 * (its lit edge, middle and shaded edge), with the digits' dark edge and the two dark rows under them. top is the
 * first lit row of each dot, so the pair sits around the middle of the digits.
 */
static void load_colon(const Picture *digit, Picture *out, int dot, int top0, int top1) {
    int x, y, d, size = dot + 2;
    if (digit->rgba == NULL) {
        return;
    }
    out->width = size;
    out->height = digit->height;
    out->rgba = calloc((unsigned long) out->width * out->height, 4);
    for (d = 0; out->rgba != NULL && d < 2; d++) {
        int top = d ? top1 : top0;
        for (y = top - 1; y <= top + dot + 1 && y < out->height; y++) {
            int first = -1, last = -1, i;
            const unsigned char *row = digit->rgba + y * digit->width * 4;
            // The lit run of the digit's row.
            for (i = 0; i < digit->width; i++) {
                if (row[i * 4 + 3] != 0 && row[i * 4] + row[i * 4 + 1] + row[i * 4 + 2] > 120) {
                    if (first < 0) {
                        first = i;
                    }
                    last = i;
                }
            }
            for (x = 0; x < size; x++) {
                unsigned char *o = out->rgba + (y * out->width + x) * 4;
                int edgeX = x == 0 || x == size - 1, edgeY = y == top - 1 || y >= top + dot;
                if (edgeX && edgeY) {
                    // Rounded corners, like the digits'.
                    continue;
                }
                if (edgeX || edgeY || first < 0) {
                    o[0] = o[1] = o[2] = 0;
                } else {
                    int at = x == 1 ? first : x == dot ? last : (first + last) / 2;
                    memcpy(o, row + at * 4, 3);
                }
                o[3] = 255;
            }
        }
    }
}

/*
 * The bosses' and Taj's portraits. The ROM has none, and drawn ones cannot be shipped, so they are the player's own:
 * 40x40 PNGs like the game's portraits (frame included), in sdmc:/3ds/dkr/portraits (port/tools/portraits makes
 * them). A file that is missing, is not a PNG or is any other size is not used, and that racer gets a name box.
 */
#define PORTRAIT_SIZE 40
#define PORTRAIT_FILE_MAX (256 * 1024)
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS PORTRAIT_SIZE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "third_party/stb_image.h"
#pragma GCC diagnostic pop

static const char *const sSpecialNames[RACE_HUD_SPECIALS] = { "tricky", "bluey", "bubbler", "smokey", "wizpig", "taj" };
static Picture sSpecialPortraits[RACE_HUD_SPECIALS]; /* Halved to 20x20; rgba NULL: the name box. */

static void load_special_portrait(int index, Picture *out) {
    char path[256];
    unsigned char *file, *rgba;
    long size;
    int w, h, channels;
    FILE *f;
    snprintf(path, sizeof(path), "%s/portraits/%s.png", plat_data_dir(), sSpecialNames[index]);
    if ((f = fopen(path, "rb")) == NULL) {
        return;
    }
    file = NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && size <= PORTRAIT_FILE_MAX &&
        fseek(f, 0, SEEK_SET) == 0 && (file = malloc(size)) != NULL && fread(file, 1, size, f) == (size_t) size) {
        // The size is checked from the header before anything is decoded.
        if (!stbi_info_from_memory(file, (int) size, &w, &h, &channels) || w != PORTRAIT_SIZE || h != PORTRAIT_SIZE) {
            plat_log("portrait %s: not a %dx%d PNG, using the name box\n", path, PORTRAIT_SIZE, PORTRAIT_SIZE);
        } else if ((rgba = stbi_load_from_memory(file, (int) size, &w, &h, &channels, 4)) == NULL ||
                   w != PORTRAIT_SIZE || h != PORTRAIT_SIZE) {
            plat_log("portrait %s: could not be read, using the name box\n", path);
            stbi_image_free(rgba);
        } else {
            out->width = w;
            out->height = h;
            out->rgba = malloc((unsigned long) w * h * 4);
            if (out->rgba != NULL) {
                memcpy(out->rgba, rgba, (unsigned long) w * h * 4);
                halve(out);
            }
            stbi_image_free(rgba);
        }
    }
    free(file);
    fclose(f);
}

static void load_race_art(void) {
    // The portraits are in a different order from the characters (Character in enums.h).
    static const int sPortraitOfCharacter[10] = { 0, 1, 2, 4, 3, 5, 6, 7, 8, 9 };
    int i;
    for (i = 0; i < 10; i++) {
        if (load_texture(ASSET_TEX2D_HUD_KRUNCHICON + sPortraitOfCharacter[i], &sPortraits[i])) {
            halve(&sPortraits[i]);
        }
        load_texture(ASSET_TEX2D_HUD_TIMERNUMBER_0 + i, &sDigits[i]);
        load_texture(ASSET_TEX2D_HUD_TIMERNUMBERSMALL_0 + i, &sSmallDigits[i]);
        load_texture(ASSET_TEX2D_HUD_WEAPONICONROCKETNUMBERS_0 + i, &sItemCounts[i]);
    }
    for (i = 0; i < 15; i++) {
        load_texture(ASSET_TEX2D_HUD_WEAPONICONBOOST_0 + i, &sItems[i]);
    }
    for (i = 0; i < 8; i++) {
        load_texture(ASSET_TEX2D_HUD_PLACENUMBER_0 + i, &sPlaces[i]);
    }
    for (i = 0; i < 4; i++) {
        load_texture_frame(ASSET_TEX2D_HUD_PLACENUMBERSUFFIXES, i, &sSuffixes[i], NULL, NULL);
    }
    load_colon(&sDigits[1], &sColon, 3, 2, 9);
    load_colon(&sSmallDigits[1], &sSmallColon, 2, 2, 7);
    load_texture(ASSET_TEX2D_HUD_FORWARDSLASH, &sSlash);
    load_texture(ASSET_TEX2D_HUD_LAP, &sLapLabel);
    load_texture(ASSET_TEX2D_HUD_TIME, &sTimeLabel);
    load_texture(ASSET_TEX2D_HUD_FINALSMALL, &sFinalLabel);
    load_texture(ASSET_TEX2D_HUD_LAPSMALL, &sLapSmallLabel);
    load_texture(ASSET_TEX2D_HUD_FINISHSMALL, &sFinishLabel);
    load_banana();
    load_texture(ASSET_TEX2D_HUD_XSMALL, &sTimes);
    load_texture(ASSET_TEX2D_MENU_ICONGHOSTS, &sGhostIcon);
    load_texture(ASSET_TEX2D_MENU_ICONTIMES, &sTimesIcon);
    halve(&sTimesIcon);
    load_texture(ASSET_TEX2D_HUD_EGGICON, &sEggIcon);
    load_texture(ASSET_TEX2D_OBJECTS_EGG_0, &sEgg);
    {
        /*
         * The whole egg: the game's egg object is two textures, the top (32x31) and its bottom cap (28x9) two pixels
         * in, so together they make a 32x40 egg, more than twice the HUD icon's detail.
         */
        Picture cap = { 0, 0, NULL };
        if (sEgg.rgba != NULL && load_texture(ASSET_TEX2D_OBJECTS_EGG_1, &cap) && cap.rgba != NULL) {
            int w = sEgg.width, h = sEgg.height + cap.height, y, x;
            sEggWhole.rgba = calloc((size_t) w * h, 4);
            if (sEggWhole.rgba != NULL) {
                sEggWhole.width = w;
                sEggWhole.height = h;
                memcpy(sEggWhole.rgba, sEgg.rgba, (size_t) sEgg.width * sEgg.height * 4);
                for (y = 0; y < cap.height; y++) {
                    for (x = 0; x < cap.width && (w - cap.width) / 2 + x < w; x++) {
                        memcpy(sEggWhole.rgba + ((sEgg.height + y) * w + (w - cap.width) / 2 + x) * 4,
                               cap.rgba + (y * cap.width + x) * 4, 4);
                    }
                }
            }
            free(cap.rgba);
        }
    }
    // The coin as the track shows it, face on (the HUD's own coin is an edge-on bar that read as a grey smear).
    if (load_texture(ASSET_TEX2D_OBJECTS_SILVERCOIN_0, &sCoin)) {
        crop(&sCoin);
        shrink_to_height(&sCoin, 16);
    }
    load_texture(ASSET_TEX2D_HUD_GOLDENBALLOONICON, &sBalloon);
    load_texture(ASSET_TEX2D_HUD_RETICLEX, &sCross);
    halve(&sCross);
    for (i = 0; i < 2; i++) {
        load_texture(ASSET_TEX2D_MENU_ICONTROPHYNOTCOLLECTED + i, &sTrophy[i]);
        halve(&sTrophy[i]);
        load_texture(ASSET_TEX2D_MENU_ICONKEYNOTCOLLECTED + i, &sKey[i]);
        halve(&sKey[i]);
    }
    for (i = 0; i < 5; i++) {
        load_texture(ASSET_TEX2D_MENU_ICONTTAMULET0_0 + i * 2, &sTTAmulet[i]);
        halve(&sTTAmulet[i]);
        load_texture(ASSET_TEX2D_MENU_ICONWIZPIGAMULET0_0 + i * 2, &sWizpigAmulet[i]);
        halve(&sWizpigAmulet[i]);
    }
    sSmallBanana = sBanana;
    if (sBanana.rgba != NULL &&
        (sSmallBanana.rgba = malloc((unsigned long) sBanana.width * sBanana.height * 4)) != NULL) {
        memcpy(sSmallBanana.rgba, sBanana.rgba, (unsigned long) sBanana.width * sBanana.height * 4);
        halve(&sSmallBanana);
    }
    sTinyBanana = sSmallBanana;
    if (sSmallBanana.rgba != NULL &&
        (sTinyBanana.rgba = malloc((unsigned long) sSmallBanana.width * sSmallBanana.height * 4)) != NULL) {
        memcpy(sTinyBanana.rgba, sSmallBanana.rgba, (unsigned long) sSmallBanana.width * sSmallBanana.height * 4);
        halve(&sTinyBanana);
    }
    for (i = 0; i < RACE_HUD_SPECIALS; i++) {
        load_special_portrait(i, &sSpecialPortraits[i]);
    }
}

/* A picture at a whole-number size (nearest pixel, so it stays sharp), with a one pixel dark outline. */
static void picture_scaled(const Picture *p, int x, int y, int scale, int outline) {
    int i, j, k;
    static const int sOffsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    if (p->rgba == NULL) {
        return;
    }
    for (k = outline ? 0 : 4; k <= 4; k++) {
        for (j = 0; j < p->height * scale; j++) {
            for (i = 0; i < p->width * scale; i++) {
                const unsigned char *t = p->rgba + ((j / scale) * p->width + i / scale) * 4;
                if (t[3] == 0) {
                    continue;
                }
                if (k < 4) {
                    plot(x + i + sOffsets[k][0], y + j + sOffsets[k][1], 0, 4, 30, t[3] * 150 / 255);
                } else {
                    plot(x + i, y + j, t[0], t[1], t[2], t[3]);
                }
            }
        }
    }
}

/* The leftmost and rightmost columns a picture uses, so pieces can be packed by what they show. */
static void used_columns(const Picture *p, int *first, int *end) {
    int x, y;
    *first = p->width;
    *end = 0;
    for (y = 0; p->rgba != NULL && y < p->height; y++) {
        for (x = 0; x < p->width; x++) {
            if (p->rgba[(y * p->width + x) * 4 + 3] > 40) {
                if (x < *first) {
                    *first = x;
                }
                if (x + 1 > *end) {
                    *end = x + 1;
                }
            }
        }
    }
    if (*end <= *first) {
        *first = 0;
        *end = p->width;
    }
}

/* Draws a picture by its used columns from x, and returns where the next piece goes (gap pixels on). */
/* In the Glass, Broadcast and Dock looks the HUD's sprite numerals and labels ("3RD", "LAP", "TIME", the digits) are the
 * game's own lettering; they are set in the look's font instead, standing on the same baseline. Returns where the next
 * piece goes, or -1 when p is not one of those sprites (or the look is Classic). */
static int alt_piece(const Picture *p, int x, int y, int gap) {
    char text[8] = "";
    int kind = UI_LG, tabular = 0, i, top, bottom;
    Colour colour = sStyle == 2 ? WHITE : GOLD;
    const UiFont *u;
    if (sStyle == 0 || p == NULL || p->rgba == NULL) {
        return -1;
    }
    if (p >= sDigits && p < sDigits + 10) {
        text[0] = (char) ('0' + (p - sDigits));
        tabular = 1;
    } else if (p >= sSmallDigits && p < sSmallDigits + 10) {
        text[0] = (char) ('0' + (p - sSmallDigits));
        kind = UI_HEAD;
        tabular = 1;
    } else if (p >= sItemCounts && p < sItemCounts + 10) {
        // The count on a small round badge in the item's corner.
        smooth_box(x, y + 2, x + 18, y + 20, 9, (Colour){ 0, 0, 20, 190 }, (Colour){ 0, 0, 20, 190 },
                   (Colour){ 255, 255, 255, 120 }, 1);
        text[0] = (char) ('0' + (p - sItemCounts) + 1);
        u = ui_font_kind(UI_HEAD);
        ui_draw(u, x + (18 - ui_advance(u, (unsigned char) text[0])) / 2, y + 2 + 9 - u->base + 5, text, WHITE, 0, BS_WIDTH);
        return x + 18 + gap;
    } else if (p >= sPlaces && p < sPlaces + 8) {
        text[0] = (char) ('1' + (p - sPlaces));
        kind = UI_XL;
    } else if (p >= sSuffixes && p < sSuffixes + 4) {
        static const char *const suffixes[4] = { "ST", "ND", "RD", "TH" };
        snprintf(text, sizeof(text), "%s", suffixes[p - sSuffixes]);
        kind = UI_SM;
    } else if (p == &sColon || p == &sSmallColon) {
        snprintf(text, sizeof(text), ":");
        kind = p == &sColon ? UI_LG : UI_HEAD;
    } else if (p == &sSlash) {
        snprintf(text, sizeof(text), "/");
    } else if (p == &sTimes) {
        snprintf(text, sizeof(text), "x");
        kind = UI_HEAD;
    } else if (p == &sTimeLabel || p == &sLapLabel) {
        snprintf(text, sizeof(text), p == &sTimeLabel ? "TIME" : "LAP");
        kind = UI_SM;
        colour = LABEL_BLUE;
    } else if (p == &sFinalLabel) {
        snprintf(text, sizeof(text), "FINAL");
    } else if (p == &sLapSmallLabel) {
        snprintf(text, sizeof(text), "LAP");
    } else if (p == &sFinishLabel) {
        snprintf(text, sizeof(text), "FINISH");
    } else {
        return -1;
    }
    u = ui_font_kind(kind);
    // The sprite's ink rows say where its baseline is.
    top = p->height;
    bottom = 0;
    for (i = 0; i < p->width * p->height; i++) {
        if (p->rgba[i * 4 + 3] > 40) {
            int row = i / p->width;
            top = row < top ? row : top;
            bottom = row + 1 > bottom ? row + 1 : bottom;
        }
    }
    if (bottom == 0) {
        bottom = p->height;
    }
    if (tabular) {
        int cell = 0, c;
        for (c = 0; c < 10; c++) {
            int a = ui_advance(u, (unsigned char) ('0' + c));
            cell = a > cell ? a : cell;
        }
        ui_draw(u, x + (cell - ui_advance(u, (unsigned char) text[0])) / 2, y + bottom - u->base, text, colour, 1, BS_WIDTH);
        return x + cell + gap;
    }
    x = ui_draw(u, x, y + bottom - u->base, text, colour, 1, BS_WIDTH);
    (void) top;
    return x + gap;
}

static int piece_impl(const Picture *p, int x, int y, int gap);
static int piece(const Picture *p, int x, int y, int gap) {
    int r;
    if (!sLogOn || sClipEnd < 0 || sDryInk || sQuiet) {
        return piece_impl(p, x, y, gap);
    }
    ink_begin();
    r = piece_impl(p, x, y, gap);
    if (ink_end()) {
        log_rec('N', sInkX0, sInkY0, sInkX1, sInkY1, 0, "");
    }
    return r;
}
static int piece_impl(const Picture *p, int x, int y, int gap) {
    int first, end, alt = alt_piece(p, x, y, gap);
    if (alt >= 0) {
        return alt;
    }
    used_columns(p, &first, &end);
    picture_scaled(p, x - first, y, 1, 1);
    return x + end - first + gap;
}

/* A time as the game shows it, MM:SS:HH (hud_timer_render), from 60ths of a second. */
static int race_time(int x, int y, int frames, int small) {
    int hundredths, seconds, minutes, digits[6], i;
    const Picture *set = small ? sSmallDigits : sDigits;
    const Picture *colon = small ? &sSmallColon : &sColon;
    if (frames < 0) {
        frames = 0;
    }
    if (frames > 60 * 60 * 100 - 1) {
        frames = 60 * 60 * 100 - 1;
    }
    hundredths = (frames % 60) * 100 / 60;
    seconds = (frames / 60) % 60;
    minutes = frames / 3600;
    digits[0] = minutes / 10;
    digits[1] = minutes % 10;
    digits[2] = seconds / 10;
    digits[3] = seconds % 10;
    digits[4] = hundredths / 10;
    digits[5] = hundredths % 10;
    for (i = 0; i < 6; i++) {
        x = piece(&set[digits[i]], x, y, 1);
        if (i == 1 || i == 3) {
            x = piece(colon, x, y, 1);
        }
    }
    return x;
}

/* A gap as "+1.42" (ahead of the player) or "-0.61", "+1 lap", or "" before the first checkpoint. */
static void format_gap(int gap, char *out, int size) {
    int value;
    if (gap == RACE_HUD_NO_GAP) {
        out[0] = '\0';
        return;
    }
    if (gap >= RACE_HUD_LAPPED || gap <= -RACE_HUD_LAPPED) {
        int laps = gap > 0 ? gap - RACE_HUD_LAPPED : -gap - RACE_HUD_LAPPED;
        snprintf(out, size, port_text("%c%d lap%s"), gap > 0 ? '+' : '-', laps, laps == 1 ? "" : "s");
        return;
    }
    value = (gap < 0 ? -gap : gap) * 100 / 60;
    if (value > 9999) {
        value = 9999;
    }
    snprintf(out, size, "%c%d.%02d", gap >= 0 ? '+' : '-', value / 100, value % 100);
}

/* A time gap drawn with its decimal points in one column: the whole part ends where the point is, the hundredths start there. */
static void draw_gap(int x, int y, int rightAligned, const char *gap, Colour c) {
    const char *dot = strchr(gap, '.');
    char whole[16];
    int xd, n;
    if (dot == NULL) {
        text(FONT_SMALL, rightAligned ? x - text_width(FONT_SMALL, gap) : x, y, gap, c, 1);
        return;
    }
    n = (int) (dot - gap);
    snprintf(whole, sizeof(whole), "%.*s", n, gap);
    xd = rightAligned ? x - text_width(FONT_SMALL, ".00") : x + text_width(FONT_SMALL, "-0");
    text(FONT_SMALL, xd - text_width(FONT_SMALL, whole), y, whole, c, 1);
    text(FONT_SMALL, xd, y, dot, c, 1);
}


/*
 * The grid's maps, redrawn: the ROM's map picture gives the track's shape, and only its centre line is kept (the
 * shape thinned to one pixel, short spurs pruned). The track is drawn again round that line as a light lane of one
 * width (wider open ground keeps its own size), a solid dark rim two pixels wide, and the start line as a small
 * checker of 2 x 2 squares across the lane. Every pixel is whole: no stubs, notches, double rims or half-tones.
 * The markers sit on the centre line (map_snap) and the player's arrow points along it.
 */
static int sMapRebuild;           /* set by make_race_base: the next map loaded is redrawn so */
static int sMapOpen;              /* set by make_race_base: an arena, whose open ground keeps its size */
static int sMapRebuilt;           /* the map in sMap was redrawn (sMapSkel, sMapR and sMapChk are valid) */
static unsigned char *sMapSkel;   /* the centre line, a pixel per sMap pixel */
static int sMapR = 4;             /* the lane's half width: 2 * sMapR + 1 px */
static int sMapChk[4] = { 0, 0, -1, -1 }; /* the checker's box in sMap pixels (x0, y0, x1, y1 inclusive) */

#ifndef __3DS__ /* the traced centre line: host dumps for tools/map_paths only */
static int map_skel_at(int x, int y, int w, int h) {
    return x >= 0 && y >= 0 && x < w && y < h && sMapSkel[y * w + x];
}

/* The centre line's direction at (x, y): the main axis of the line's pixels within 5 px, as an angle (radians,
   0 = right, y down). */
static float map_tangent(int x, int y) {
    int w = sMap.width, h = sMap.height, i, j, n = 0;
    float sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, mx, my;
    for (j = -5; j <= 5; j++) {
        for (i = -5; i <= 5; i++) {
            if (i * i + j * j <= 25 && map_skel_at(x + i, y + j, w, h)) {
                sx += i;
                sy += j;
                sxx += (float) i * i;
                syy += (float) j * j;
                sxy += (float) i * j;
                n++;
            }
        }
    }
    if (n < 2) {
        return 0.0f;
    }
    mx = sx / n;
    my = sy / n;
    sxx = sxx / n - mx * mx;
    syy = syy / n - my * my;
    sxy = sxy / n - mx * my;
    return 0.5f * atan2f(2.0f * sxy, sxx - syy);
}

/* The centre line's pixel nearest (x, y) within 8 px, in sMap pixels; 0 if none. */
static int map_snap(int x, int y, int *sx, int *sy) {
    int w = sMap.width, h = sMap.height, i, j, best = 1 << 30;
    for (j = -8; j <= 8; j++) {
        for (i = -8; i <= 8; i++) {
            if (i * i + j * j < best && map_skel_at(x + i, y + j, w, h)) {
                best = i * i + j * j;
                *sx = x + i;
                *sy = y + j;
            }
        }
    }
    return best != 1 << 30;
}

static void map_rebuild(int w, int h, const unsigned char *alpha) {
    static const unsigned char sRimC[3] = { 0, 10, 60 }, sDark[3] = { 16, 18, 28 };
    unsigned char *m = calloc((size_t) w * h, 1), *lane = calloc((size_t) w * h, 1), *lab = NULL;
    unsigned short *dt = calloc((size_t) w * h, sizeof(unsigned short));
    int *stack = malloc(sizeof(int) * (size_t) w * h);
    int x, y, i, j, k, n, light[3] = { 0, 0, 0 }, nl = 0, ncx = 0, ncy = 0, nc = 0, changed, iter;
    if (m == NULL || lane == NULL || dt == NULL || stack == NULL) {
        free(m), free(lane), free(dt), free(stack);
        return;
    }
    free(sMapSkel);
    sMapSkel = calloc((size_t) w * h, 1);
    if (sMapSkel == NULL) {
        free(m), free(lane), free(dt), free(stack);
        return;
    }
    for (i = 0; i < w * h; i++) {
        const unsigned char *o = sMap.rgba + i * 4;
        m[i] = alpha[i] != 0;
        if (m[i] && o[0] == sDark[0] && o[1] == sDark[1] && o[2] == sDark[2]) {
            ncx += i % w, ncy += i / w, nc++;
        } else if (m[i]) {
            light[0] += o[0], light[1] += o[1], light[2] += o[2], nl++;
        }
    }
    for (k = 0; k < 3 && nl; k++) {
        light[k] /= nl;
    }
    // Holes in the shape smaller than 64 px (the picture's specks, a pond) are track.
    lab = calloc((size_t) w * h, 1);
    for (i = 0; lab != NULL && i < w * h; i++) {
        int top = 0, area = 0, edge = 0, s;
        if (m[i] || lab[i]) {
            continue;
        }
        stack[top++] = i;
        lab[i] = 1;
        while (top) {
            s = stack[--top];
            area++;
            x = s % w, y = s / w;
            edge |= x == 0 || y == 0 || x == w - 1 || y == h - 1;
            if (x > 0 && !m[s - 1] && !lab[s - 1]) lab[s - 1] = 1, stack[top++] = s - 1;
            if (x < w - 1 && !m[s + 1] && !lab[s + 1]) lab[s + 1] = 1, stack[top++] = s + 1;
            if (y > 0 && !m[s - w] && !lab[s - w]) lab[s - w] = 1, stack[top++] = s - w;
            if (y < h - 1 && !m[s + w] && !lab[s + w]) lab[s + w] = 1, stack[top++] = s + w;
        }
        if (!edge && area < 64) {
            // a second pass over the same hole marks it track
            stack[top++] = i;
            m[i] = 1;
            while (top) {
                s = stack[--top];
                x = s % w, y = s / w;
                if (x > 0 && !m[s - 1]) m[s - 1] = 1, stack[top++] = s - 1;
                if (x < w - 1 && !m[s + 1]) m[s + 1] = 1, stack[top++] = s + 1;
                if (y > 0 && !m[s - w]) m[s - w] = 1, stack[top++] = s - w;
                if (y < h - 1 && !m[s + w]) m[s + w] = 1, stack[top++] = s + w;
            }
        }
    }
    free(lab);
    // Distance to the shape's edge (chamfer 3-4, in thirds of a pixel).
    for (i = 0; i < w * h; i++) {
        dt[i] = m[i] ? 60000 : 0;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int v = dt[y * w + x];
            if (!v) continue;
            v = x > 0 ? (dt[y * w + x - 1] + 3 < v ? dt[y * w + x - 1] + 3 : v) : 3;
            if (y > 0) {
                v = dt[(y - 1) * w + x] + 3 < v ? dt[(y - 1) * w + x] + 3 : v;
                if (x > 0) v = dt[(y - 1) * w + x - 1] + 4 < v ? dt[(y - 1) * w + x - 1] + 4 : v;
                if (x < w - 1) v = dt[(y - 1) * w + x + 1] + 4 < v ? dt[(y - 1) * w + x + 1] + 4 : v;
            } else {
                v = 3;
            }
            dt[y * w + x] = (unsigned short) v;
        }
    }
    for (y = h - 1; y >= 0; y--) {
        for (x = w - 1; x >= 0; x--) {
            int v = dt[y * w + x];
            if (!v) continue;
            if (x == w - 1 || y == h - 1) v = 3;
            if (x < w - 1) v = dt[y * w + x + 1] + 3 < v ? dt[y * w + x + 1] + 3 : v;
            if (y < h - 1) {
                v = dt[(y + 1) * w + x] + 3 < v ? dt[(y + 1) * w + x] + 3 : v;
                if (x < w - 1) v = dt[(y + 1) * w + x + 1] + 4 < v ? dt[(y + 1) * w + x + 1] + 4 : v;
                if (x > 0) v = dt[(y + 1) * w + x - 1] + 4 < v ? dt[(y + 1) * w + x - 1] + 4 : v;
            }
            dt[y * w + x] = (unsigned short) v;
        }
    }
    // The centre line: the shape thinned to one pixel (Zhang-Suen).
    memcpy(sMapSkel, m, (size_t) w * h);
    for (iter = 0, changed = 1; changed && iter < 200; iter++) {
        int sub;
        changed = 0;
        for (sub = 0; sub < 2; sub++) {
            n = 0;
            for (y = 1; y < h - 1; y++) {
                for (x = 1; x < w - 1; x++) {
                    unsigned char *s = sMapSkel + y * w + x;
                    int p[8], b = 0, a = 0;
                    if (!*s) continue;
                    p[0] = s[-w], p[1] = s[-w + 1], p[2] = s[1], p[3] = s[w + 1];
                    p[4] = s[w], p[5] = s[w - 1], p[6] = s[-1], p[7] = s[-w - 1];
                    for (k = 0; k < 8; k++) {
                        b += p[k] != 0;
                        a += !p[k] && p[(k + 1) & 7];
                    }
                    if (b < 2 || b > 6 || a != 1) continue;
                    if (sub == 0 ? (p[0] && p[2] && p[4]) || (p[2] && p[4] && p[6])
                                 : (p[0] && p[2] && p[6]) || (p[0] && p[4] && p[6])) {
                        continue;
                    }
                    stack[n++] = y * w + x;
                }
            }
            for (k = 0; k < n; k++) {
                sMapSkel[stack[k]] = 0;
            }
            changed |= n > 0;
        }
    }
    // The lane's half width: the centre line's typical distance to the edge, 4 px at least (a marker and a light
    // pixel each side of it).
    {
        int hist[64] = { 0 }, tot = 0, acc = 0;
        for (i = 0; i < w * h; i++) {
            if (sMapSkel[i]) {
                int d = dt[i] / 3;
                hist[d < 63 ? d : 63]++;
                tot++;
            }
        }
        sMapR = 4;
        for (i = 0; i < 64 && tot; i++) {
            acc += hist[i];
            if (acc * 2 >= tot) {
                sMapR = i < 4 ? 4 : i > 8 ? 8 : i;
                break;
            }
        }
    }
    // Spurs: a branch from a loose end to a junction shorter than the lane is wide comes off (three rounds).
    for (iter = 0; iter < 3; iter++) {
        for (y = 1; y < h - 1; y++) {
            for (x = 1; x < w - 1; x++) {
                int cx = x, cy = y, len = 0, junction = 0, nb, nx = 0, ny = 0, top = 0;
                if (!sMapSkel[y * w + x]) continue;
                for (nb = 0, j = -1; j <= 1; j++) for (i = -1; i <= 1; i++) nb += (i || j) && sMapSkel[(y + j) * w + x + i];
                if (nb != 1) continue;
                while (len < 4 * sMapR + 4) {
                    stack[top++] = cy * w + cx;
                    sMapSkel[cy * w + cx] = 2;
                    len++;
                    for (nb = 0, j = -1; j <= 1; j++) {
                        for (i = -1; i <= 1; i++) {
                            int v = (i || j) && cx + i > 0 && cy + j > 0 && cx + i < w - 1 && cy + j < h - 1 ? sMapSkel[(cy + j) * w + cx + i] : 0;
                            if (v == 1) nb++, nx = cx + i, ny = cy + j;
                        }
                    }
                    if (nb != 1) {
                        junction = nb >= 2;
                        break;
                    }
                    cx = nx, cy = ny;
                }
                for (k = 0; k < top; k++) {
                    sMapSkel[stack[k]] = junction && len < 2 * sMapR + 3 ? 0 : 1;
                }
            }
        }
    }
    // The lane: a disc of the lane's half width round each centre line pixel (open ground keeps its own size).
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int r;
            if (!sMapSkel[y * w + x]) continue;
            r = sMapOpen && dt[y * w + x] / 3 > sMapR * 8 / 5 ? dt[y * w + x] / 3 : sMapR;
            for (j = -r; j <= r; j++) {
                for (i = -r; i <= r; i++) {
                    if (i * i + j * j <= r * r + r / 2 && x + i >= 0 && y + j >= 0 && x + i < w && y + j < h) {
                        lane[(y + j) * w + x + i] = 1;
                    }
                }
            }
        }
    }
    // Draw: the lane light, a rim two pixels round it, the panel elsewhere.
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned char *o = sMap.rgba + (y * w + x) * 4;
            int rim = 0;
            if (lane[y * w + x]) {
                o[0] = (unsigned char) light[0], o[1] = (unsigned char) light[1], o[2] = (unsigned char) light[2], o[3] = 255;
                continue;
            }
            for (j = -2; j <= 2 && !rim; j++) {
                for (i = -2; i <= 2 && !rim; i++) {
                    rim = i * i + j * j <= 5 && x + i >= 0 && y + j >= 0 && x + i < w && y + j < h && lane[(y + j) * w + x + i];
                }
            }
            o[0] = sRimC[0], o[1] = sRimC[1], o[2] = sRimC[2], o[3] = rim ? 255 : 0;
        }
    }
    // The start line: a checker of 2 x 2 squares (4 px along the lane, 6 or 4 across) in a one-pixel dark frame, so
    // its light squares read against the light lane; on the centre line nearest the picture's own checks, turned to
    // the lane in quarter turns, with a light pixel to the rim all round.
    sMapChk[2] = sMapChk[3] = -1;
    if (nc > 0) {
        int sx, sy;
        if (map_snap(ncx / nc, ncy / nc, &sx, &sy)) {
            float t = map_tangent(sx, sy);
            int along_x = fabsf(cosf(t)) >= fabsf(sinf(t)), across;
            for (across = 6; across >= 4; across -= 2) {
                int bw = (along_x ? 4 : across) + 2, bh = (along_x ? across : 4) + 2, x0 = sx - bw / 2, y0 = sy - bh / 2, ok = 1;
                for (j = -1; j <= bh && ok; j++) {
                    for (i = -1; i <= bw && ok; i++) {
                        ok = x0 + i >= 0 && y0 + j >= 0 && x0 + i < w && y0 + j < h && lane[(y0 + j) * w + x0 + i];
                    }
                }
                if (!ok && across > 4) {
                    continue;
                }
                for (j = 0; j < bh; j++) {
                    for (i = 0; i < bw; i++) {
                        unsigned char *o = sMap.rgba + ((y0 + j) * w + x0 + i) * 4;
                        int frame = i == 0 || j == 0 || i == bw - 1 || j == bh - 1;
                        if (frame || (((i - 1) >> 1) + ((j - 1) >> 1)) & 1) {
                            o[0] = sDark[0], o[1] = sDark[1], o[2] = sDark[2];
                        } else {
                            o[0] = 255, o[1] = 255, o[2] = 255;
                        }
                    }
                }
                sMapChk[0] = x0, sMapChk[1] = y0, sMapChk[2] = x0 + bw - 1, sMapChk[3] = y0 + bh - 1;
                break;
            }
        }
    }
    // A panel gap one or two pixels across between two rims becomes rim (one solid rim, no sliver).
    for (iter = 0; iter < 8; iter++) {
        int filled = 0;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                unsigned char *o = sMap.rgba + (y * w + x) * 4;
                int hz = 0, vt = 0;
#define MA(xx, yy) ((xx) >= 0 && (yy) >= 0 && (xx) < w && (yy) < h && sMap.rgba[((yy) * w + (xx)) * 4 + 3])
                if (o[3]) continue;
                hz = (MA(x - 1, y) && (MA(x + 1, y) || MA(x + 2, y))) || (MA(x + 1, y) && MA(x - 2, y));
                vt = (MA(x, y - 1) && (MA(x, y + 1) || MA(x, y + 2))) || (MA(x, y + 1) && MA(x, y - 2));
#undef MA
                if (hz || vt) {
                    o[0] = sRimC[0], o[1] = sRimC[1], o[2] = sRimC[2], o[3] = 255;
                    filled++;
                }
            }
        }
        if (!filled) break;
    }
    sMapLane = 2 * sMapR + 1;
    sMapRebuilt = 1;
    free(m), free(lane), free(dt), free(stack);
}
#endif /* __3DS__ */

/*
 * The grid's track maps, drawn from data: each track's centre line (bottom_map_paths.h, made once by
 * tools/map_paths from the ROM's map picture, oriented by the checkpoints) drawn as a thick line: a light lane
 * 2 * MAP_LANE_R + 1 px wide over a dark rim two pixels wider each side. Strands that cross or run side by side
 * merge into one lane and one rim; a sliver or pocket of the panel 3 px or narrower between them is filled as rim.
 * The start line is the one checker (2 x 2 squares in a 1 px frame), centred in the lane where the picture's checks
 * are. The map picture itself only gives where the checks are; the tracks menu uses the same data, no race loaded.
 */
#include "bottom_map_paths.h"
#ifdef __3DS__
#define MAP_DUMPING 0
#else
#define MAP_DUMPING (getenv("DKR_DUMP_SKEL") != NULL) /* host: tracing the ROM pictures again (tools/map_paths) */
#endif
#define MAP_LANE_R 5 /* a 9 x 9 arrow and a light pixel each side: 11 px */
#define MAP_PATH_MAX 4096
static float sPathPt[MAP_PATH_MAX][4]; /* x, y (sMap pixels), the direction of travel (unit) */
static int sPathN;
#define MAP_LINES_MAX 16
#define MAP_APART 14.0f /* px along the track: two parts further apart than this, side by side, are two strands */
static float sPathArc[MAP_PATH_MAX];  /* how far along its line (px) */
static unsigned char sPathLine[MAP_PATH_MAX];
static float sLineLen[MAP_LINES_MAX];
static int sLineClosed[MAP_LINES_MAX];
static int sLinkN, sLink[64][2]; /* a line's end sample and the other line's sample it joins */

/* Distance along one line between two of its samples (a loop: the shorter way round). */
static float path_along(int a, int b) {
    float d = fabsf(sPathArc[a] - sPathArc[b]);
    int l = sPathLine[a];
    if (l < MAP_LINES_MAX && sLineClosed[l] && sLineLen[l] - d < d) {
        d = sLineLen[l] - d;
    }
    return d;
}

/* Distance along the track between two samples: on one line, or through a join of the two lines. */
static float path_apart(int a, int b) {
    float best = 1e9f;
    int i;
    if (sPathLine[a] == sPathLine[b]) {
        return path_along(a, b);
    }
    for (i = 0; i < sLinkN; i++) {
        int e = sLink[i][0], o = sLink[i][1];
        if (sPathLine[e] == sPathLine[a] && sPathLine[o] == sPathLine[b]) {
            float d = path_along(a, e) + path_along(o, b);
            best = d < best ? d : best;
        } else if (sPathLine[e] == sPathLine[b] && sPathLine[o] == sPathLine[a]) {
            float d = path_along(b, e) + path_along(o, a);
            best = d < best ? d : best;
        }
    }
    return best;
}

/* The joins: each open line's two ends, and the nearest sample (within 3 px) of every other line. */
static void path_links(void) {
    int i, j, e;
    sLinkN = 0;
    for (i = 0; i < sPathN; i++) {
        int l = sPathLine[i], end = (i == 0 || sPathLine[i - 1] != l) || (i == sPathN - 1 || sPathLine[i + 1] != l);
        if (!end || (l < MAP_LINES_MAX && sLineClosed[l])) {
            continue;
        }
        for (e = 0; e < MAP_LINES_MAX; e++) {
            int bj = -1;
            float bd = 9.0f;
            if (e == l) {
                continue;
            }
            for (j = 0; j < sPathN; j++) {
                float dx, dy, d;
                if (sPathLine[j] != e) {
                    continue;
                }
                dx = sPathPt[j][0] - sPathPt[i][0], dy = sPathPt[j][1] - sPathPt[i][1], d = dx * dx + dy * dy;
                if (d < bd) {
                    bd = d, bj = j;
                }
            }
            if (bj >= 0 && sLinkN < 64) {
                sLink[sLinkN][0] = i, sLink[sLinkN][1] = bj, sLinkN++;
            }
        }
    }
}

static void path_to_map_px(const short *p, float *x, float *y) {
    float u = p[0] / 4.0f - sMapTexX - 1.5f, v = p[1] / 4.0f - sMapTexY - 1.0f;
    if (sMapMirrored) {
        u = sMapTexWidth - u;
    }
    *x = 3 + u * sMapScale;
    *y = 3 + v * sMapScale;
}

static const MapPathTrack *path_track(int sprite) {
    unsigned i;
    for (i = 0; i < sizeof(kMapPathTracks) / sizeof(kMapPathTracks[0]); i++) {
        if (kMapPathTracks[i].sprite == sprite) {
            return &kMapPathTracks[i];
        }
    }
    return NULL;
}

/* The centre line's point nearest (x, y) in sMap pixels within 10 px: where it is and the way the track runs. */
static int path_snap(float x, float y, float *sx, float *sy, float *tx, float *ty) {
    int i, bi = -1;
    float best = 100.0f;
    for (i = 0; i < sPathN; i++) {
        float dx = sPathPt[i][0] - x, dy = sPathPt[i][1] - y, d = dx * dx + dy * dy;
        if (d < best) {
            best = d;
            bi = i;
        }
    }
    if (bi < 0) {
        return 0;
    }
    *sx = sPathPt[bi][0], *sy = sPathPt[bi][1], *tx = sPathPt[bi][2], *ty = sPathPt[bi][3];
    return 1;
}

/* Grow (dilate 1) or shrink (erode 0) a mask by a disc of i*i + j*j <= r2, into out. */
static void map_morph(const unsigned char *in, unsigned char *out, int w, int h, int r2, int dilate) {
    int x, y, i, j, r = (int) sqrtf((float) r2);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int v = !dilate;
            for (j = -r; j <= r && v == !dilate; j++) {
                for (i = -r; i <= r; i++) {
                    int on;
                    if (i * i + j * j > r2) {
                        continue;
                    }
                    on = x + i >= 0 && y + j >= 0 && x + i < w && y + j < h && in[(y + j) * w + x + i];
                    if (dilate ? on : !on) {
                        v = dilate;
                        break;
                    }
                }
            }
            out[y * w + x] = (unsigned char) v;
        }
    }
}

static int map_from_paths(int w, int h, int sprite) {
    static const unsigned char sRimC[3] = { 0, 10, 60 }, sDark[3] = { 16, 18, 28 };
    const MapPathTrack *t = path_track(sprite);
    unsigned char *lane, *rim, *hole;
    int *near;
    float *dist;
    int i, j, k, x, y, ncx = 0, ncy = 0, nc = 0, light[3] = { 0, 0, 0 }, nl = 0;
    if (t == NULL || sMap.rgba == NULL) {
        return 0;
    }
    lane = calloc((size_t) w * h, 1);
    rim = calloc((size_t) w * h, 1);
    if (lane == NULL || rim == NULL) {
        free(lane), free(rim);
        return 0;
    }
    // The picture's checks (where the start is) and its light colour, before it is drawn over.
    for (i = 0; i < w * h; i++) {
        const unsigned char *o = sMap.rgba + i * 4;
        if (o[3] == 255 && o[0] == sDark[0] && o[1] == sDark[1] && o[2] == sDark[2]) {
            ncx += i % w, ncy += i / w, nc++;
        } else if (o[3] == 255 && o[0] + o[1] + o[2] >= 3 * 160) {
            light[0] += o[0], light[1] += o[1], light[2] += o[2], nl++;
        }
    }
    for (k = 0; k < 3; k++) {
        light[k] = nl ? light[k] / nl : 236;
    }
    // The picture's own holes (the infields, and the narrow slots between strands that run side by side): its
    // ground is the light pixels and the checks; what is not ground and does not reach the picture's edge is a hole.
    // The lane drawn from the centre line keeps them, so the map has the picture's strands and holes.
    hole = calloc((size_t) w * h, 1);
    if (hole != NULL) {
        int *stack = malloc(sizeof(int) * (size_t) w * h);
        for (i = 0; i < w * h; i++) {
            const unsigned char *o = sMap.rgba + i * 4;
            int ground = o[3] >= 128 && (o[0] + o[1] + o[2] >= 3 * 160 || (o[0] == sDark[0] && o[1] == sDark[1] && o[2] == sDark[2]));
            hole[i] = ground ? 0 : 1;
        }
        for (i = 0; stack != NULL && i < w * h; i++) {
            int top = 0, area = 0, edge = 0, n0 = 0, sidx;
            if (hole[i] != 1) {
                continue;
            }
            stack[top++] = i, hole[i] = 2;
            while (top) {
                sidx = stack[--top];
                stack[w * h - 1 - n0++] = sidx;
                area++;
                x = sidx % w, y = sidx / w;
                edge |= x == 0 || y == 0 || x == w - 1 || y == h - 1;
                if (x > 0 && hole[sidx - 1] == 1) hole[sidx - 1] = 2, stack[top++] = sidx - 1;
                if (x < w - 1 && hole[sidx + 1] == 1) hole[sidx + 1] = 2, stack[top++] = sidx + 1;
                if (y > 0 && hole[sidx - w] == 1) hole[sidx - w] = 2, stack[top++] = sidx - w;
                if (y < h - 1 && hole[sidx + w] == 1) hole[sidx + w] = 2, stack[top++] = sidx + w;
            }
            for (j = 0; j < n0; j++) {
                hole[stack[w * h - 1 - j]] = edge || area < 6 ? 3 : 4; /* 4: a hole; 3: the outside, or a speck */
            }
        }
        free(stack);
    }
    // The centre line, sampled every half pixel (with its line and how far along it each sample is).
    float arc = 0.0f;
    sPathN = 0;
    for (k = 0; k < t->count; k++) {
        const MapPathLine *l = &kMapPathLines[t->first + k];
        int segs = l->closed ? l->count : l->count - 1;
        for (i = 0; i < segs; i++) {
            float x0, y0, x1, y1, len, s;
            path_to_map_px(kMapPathPoints[l->first + i], &x0, &y0);
            path_to_map_px(kMapPathPoints[l->first + (i + 1) % l->count], &x1, &y1);
            len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
            for (s = 0; s < len && sPathN < MAP_PATH_MAX; s += 0.5f) {
                float *p = sPathPt[sPathN];
                sPathLine[sPathN] = (unsigned char) k;
                sPathArc[sPathN++] = arc + s;
                p[0] = x0 + (x1 - x0) * s / len, p[1] = y0 + (y1 - y0) * s / len;
                p[2] = (x1 - x0) / len, p[3] = (y1 - y0) / len;
            }
            arc += len;
        }
        if (k < MAP_LINES_MAX) {
            sLineLen[k] = arc, sLineClosed[k] = l->closed;
        }
        arc = 0.0f;
    }
    // The lane as a mask: discs round the line, tested at pixel centres (round joins and caps); each pixel notes the
    // line's sample nearest it.
    near = malloc(sizeof(int) * (size_t) w * h);
    dist = malloc(sizeof(float) * (size_t) w * h);
    if (near == NULL || dist == NULL) {
        free(near), free(dist), free(lane), free(rim), free(hole);
        return 0;
    }
    for (i = 0; i < w * h; i++) {
        near[i] = -1, dist[i] = 1e9f;
    }
    for (i = 0; i < sPathN; i++) {
        int R = MAP_LANE_R, cx = (int) sPathPt[i][0], cy = (int) sPathPt[i][1];
        for (y = cy - R - 1; y <= cy + R + 1; y++) {
            for (x = cx - R - 1; x <= cx + R + 1; x++) {
                float dx = x + 0.5f - sPathPt[i][0], dy = y + 0.5f - sPathPt[i][1], d = dx * dx + dy * dy;
                if (x >= 0 && y >= 0 && x < w && y < h && d <= (R + 0.5f) * (R + 0.5f)) {
                    lane[y * w + x] = 1;
                    if (d < dist[y * w + x]) {
                        dist[y * w + x] = d, near[y * w + x] = i;
                    }
                }
            }
        }
    }
    // Where two lines join (a fork, a shortcut), the joining end's nearest sample on the other line.
    path_links();
    // Two strands of track that run closer than the lane's width keep apart: where neighbouring pixels belong to
    // parts of the track far apart along it, a line of the rim (2 px: one pixel each side) between them, not one
    // merged lane. Near a fork or a bend the parts are close along the track and stay one lane.
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int pi = y * w + x, q;
            if (!lane[pi] || near[pi] < 0) {
                continue;
            }
            for (q = 0; q < 2; q++) {
                int qi = q == 0 ? pi + 1 : pi + w;
                if ((q == 0 && x + 1 >= w) || (q == 1 && y + 1 >= h) || !lane[qi] || near[qi] < 0) {
                    continue;
                }
                if (path_apart(near[pi], near[qi]) > MAP_APART) {
                    rim[pi] = rim[qi] = 2;
                }
            }
        }
    }
    // The divider's loose ends (where two strands part) lose 3 px, so no stub pokes into the lane.
    for (k = 0; k < 3; k++) {
        for (i = 0; i < w * h; i++) {
            int nb = 0, dx, dy;
            if (rim[i] != 2) {
                continue;
            }
            x = i % w, y = i / w;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if ((dx || dy) && (xx < 0 || yy < 0 || xx >= w || yy >= h || !lane[yy * w + xx] || rim[yy * w + xx] >= 2)) {
                        nb++;
                    }
                }
            }
            if (nb <= 2) {
                rim[i] = 3; /* (an end: let go after this pass) */
            }
        }
        for (i = 0; i < w * h; i++) {
            rim[i] = rim[i] == 3 ? 0 : rim[i];
        }
    }
    for (i = 0; i < w * h; i++) {
        if (rim[i] == 2 || (hole != NULL && hole[i] == 4)) {
            lane[i] = 0;
        }
        rim[i] = 0;
    }
    free(near), free(dist), free(hole);
    // The rim: the lane grown by 2 px, less the lane, so exactly 2 px round it and never further into a hole.
    map_morph(lane, rim, w, h, 5, 1);
    {
        // A gap of the panel under 2 px between two rims closes (rim), as does a pocket of it under 12 px; a speck of
        // rim left alone in the lane (under 4 px) is lane.
        unsigned char *c = calloc((size_t) w * h, 1), *seen = calloc((size_t) w * h, 1);
        int *stack = malloc(sizeof(int) * (size_t) w * h);
        if (c != NULL && seen != NULL && stack != NULL) {
            map_morph(rim, c, w, h, 2, 1);
            map_morph(c, seen, w, h, 2, 0);
            for (i = 0; i < w * h; i++) {
                rim[i] |= seen[i] && !lane[i];
                seen[i] = 0;
            }
            for (k = 0; k < 2; k++) {
                for (i = 0; i < w * h; i++) {
                    int top = 0, area = 0, edge = 0, n0 = 0, sidx, want = k == 0 ? 0 : 1;
                    /* k 0: pockets of the panel (neither lane nor rim); k 1: specks of rim */
                    if (seen[i] || lane[i] || (rim[i] != 0) != want) {
                        continue;
                    }
                    stack[top++] = i, seen[i] = 1;
                    while (top) {
                        int nb[4], m;
                        sidx = stack[--top];
                        stack[w * h - 1 - n0++] = sidx;
                        area++;
                        x = sidx % w, y = sidx / w;
                        edge |= x == 0 || y == 0 || x == w - 1 || y == h - 1;
                        nb[0] = x > 0 ? sidx - 1 : -1, nb[1] = x < w - 1 ? sidx + 1 : -1;
                        nb[2] = y > 0 ? sidx - w : -1, nb[3] = y < h - 1 ? sidx + w : -1;
                        for (m = 0; m < 4; m++) {
                            if (nb[m] < 0 || seen[nb[m]] || lane[nb[m]]) {
                                continue;
                            }
                            if ((rim[nb[m]] != 0) != want) {
                                edge |= want; /* a rim speck that touches the panel is part of the outline, not a speck */
                                continue;
                            }
                            seen[nb[m]] = 1, stack[top++] = nb[m];
                        }
                    }
                    if (!edge && area < (k == 0 ? 12 : 4)) {
                        for (j = 0; j < n0; j++) {
                            int t2 = stack[w * h - 1 - j];
                            if (k == 0) {
                                rim[t2] = 1;
                            } else {
                                rim[t2] = 0, lane[t2] = 1;
                            }
                        }
                    }
                }
                memset(seen, 0, (size_t) w * h);
            }
        }
        free(c), free(seen), free(stack);
    }
    for (i = 0; i < w * h; i++) {
        unsigned char *o = sMap.rgba + i * 4;
        if (lane[i]) {
            o[0] = (unsigned char) light[0], o[1] = (unsigned char) light[1], o[2] = (unsigned char) light[2], o[3] = 255;
        } else {
            o[0] = sRimC[0], o[1] = sRimC[1], o[2] = sRimC[2], o[3] = rim[i] ? 255 : 0;
        }
    }
    // The start line: the checker, centred in the lane at the centre line's point nearest the picture's checks,
    // turned to the lane in quarter turns: 4 px along it, 6 across, in a 1 px frame.
    sMapChk[2] = sMapChk[3] = -1;
    if (nc > 0) {
        float sx, sy, tx, ty;
        if (path_snap((float) ncx / nc + 0.5f, (float) ncy / nc + 0.5f, &sx, &sy, &tx, &ty)) {
            int along_x = fabsf(tx) >= fabsf(ty), bw = along_x ? 6 : 8, bh = along_x ? 8 : 6, a0 = 0, a1 = 0, x0, y0;
            // centred across the lane as drawn (equal light each side), along the lane's axis nearest its normal
            int nx = along_x ? 0 : 1, ny = along_x ? 1 : 0, cx = (int) floorf(sx), cy = (int) floorf(sy);
            while (a0 < 20 && cx - nx * (a0 + 1) >= 0 && cy - ny * (a0 + 1) >= 0 && lane[(cy - ny * (a0 + 1)) * w + cx - nx * (a0 + 1)]) a0++;
            while (a1 < 20 && cx + nx * (a1 + 1) < w && cy + ny * (a1 + 1) < h && lane[(cy + ny * (a1 + 1)) * w + cx + nx * (a1 + 1)]) a1++;
            // the lane spans cx - a0 .. cx + a1 across: the box's centre on its middle (half pixels to the lower side)
            if (along_x) {
                y0 = cy - a0 + (a0 + a1 + 1 - bh) / 2, x0 = (int) floorf(sx - bw / 2.0f + 0.5f);
            } else {
                x0 = cx - a0 + (a0 + a1 + 1 - bw) / 2, y0 = (int) floorf(sy - bh / 2.0f + 0.5f);
            }
            {
                /* clear of the rim and of a slot between strands by a light pixel: moved (up to 4 px) if it is not */
                int d, di, dj, ok = 0;
                for (d = 0; d <= 16 && !ok; d++) {
                    for (dj = -4; dj <= 4 && !ok; dj++) {
                        for (di = -4; di <= 4 && !ok; di++) {
                            int ii, jj, all = 1;
                            if (di * di + dj * dj != d) {
                                continue;
                            }
                            for (jj = -1; jj <= bh && all; jj++) {
                                for (ii = -1; ii <= bw && all; ii++) {
                                    int px = x0 + di + ii, py = y0 + dj + jj;
                                    all = px >= 0 && py >= 0 && px < w && py < h && lane[py * w + px];
                                }
                            }
                            if (all) {
                                x0 += di, y0 += dj, ok = 1;
                            }
                        }
                    }
                }
            }
            for (j = 0; j < bh; j++) {
                for (i = 0; i < bw; i++) {
                    unsigned char *o;
                    int frame = i == 0 || j == 0 || i == bw - 1 || j == bh - 1;
                    if (x0 + i < 0 || y0 + j < 0 || x0 + i >= w || y0 + j >= h) {
                        continue;
                    }
                    o = sMap.rgba + ((y0 + j) * w + x0 + i) * 4;
                    if (frame || ((((i - 1) >> 1) + ((j - 1) >> 1)) & 1)) {
                        o[0] = sDark[0], o[1] = sDark[1], o[2] = sDark[2];
                    } else {
                        o[0] = o[1] = o[2] = 255;
                    }
                }
            }
            sMapChk[0] = x0, sMapChk[1] = y0, sMapChk[2] = x0 + bw - 1, sMapChk[3] = y0 + bh - 1;
        }
    }
    free(lane), free(rim);
    sMapR = MAP_LANE_R;
    sMapLane = 2 * MAP_LANE_R + 1;
    return 1;
}

/*
 * The grid's arenas keep the ROM's outline, made as the tracks are: the light ground as a mask, made symmetric when
 * the picture nearly is (the battle diamond), closed with a small disc (no pointed wedge at a hole's end, no
 * one-pixel bump), and ringed by a rim exactly 2 px wide.
 */
static void map_tidy_outline(int w, int h, int sprite) {
    static const unsigned char sRimC[3] = { 0, 10, 60 };
    unsigned char *lane = calloc((size_t) w * h, 1), *tmp = calloc((size_t) w * h, 1);
    long light[3] = { 0, 0, 0 }, nl = 0;
    int i, x, y, minX = w, maxX = -1, same = 0, all = 0;
    if (lane == NULL || tmp == NULL) {
        free(lane), free(tmp);
        return;
    }
    for (i = 0; i < w * h; i++) {
        const unsigned char *o = sMap.rgba + i * 4;
        if (o[3] == 255 && o[0] + o[1] + o[2] >= 3 * 160) {
            lane[i] = 1;
            light[0] += o[0], light[1] += o[1], light[2] += o[2], nl++;
            minX = i % w < minX ? i % w : minX;
            maxX = i % w > maxX ? i % w : maxX;
        }
    }
    if (nl == 0) {
        free(lane), free(tmp);
        return;
    }
    // left-right symmetric (about the ground's middle) when all but 3% of it already is: the mirror's common part
    for (y = 0; y < h; y++) {
        for (x = minX; x <= maxX; x++) {
            int m = minX + maxX - x;
            if (lane[y * w + x] || lane[y * w + m]) {
                all++;
                same += lane[y * w + x] && lane[y * w + m];
            }
        }
    }
    if (same * 100 >= all * 97) {
        for (y = 0; y < h; y++) {
            for (x = minX; x <= maxX; x++) {
                tmp[y * w + x] = lane[y * w + x] && lane[y * w + minX + maxX - x];
            }
        }
        memcpy(lane, tmp, (size_t) w * h);
    }
    map_morph(lane, tmp, w, h, 5, 1);
    map_morph(tmp, lane, w, h, 5, 0);
    map_morph(lane, tmp, w, h, 5, 1);
    for (i = 0; i < w * h; i++) {
        unsigned char *o = sMap.rgba + i * 4;
        if (lane[i]) {
            o[0] = (unsigned char) (light[0] / nl), o[1] = (unsigned char) (light[1] / nl), o[2] = (unsigned char) (light[2] / nl);
            o[3] = 255;
        } else {
            o[0] = sRimC[0], o[1] = sRimC[1], o[2] = sRimC[2], o[3] = tmp[i] ? 255 : 0;
        }
    }
    free(lane), free(tmp);
}

/* The track's map from the ROM: its sprite gives the texture and where the texture sits in the sprite. */
static void load_map(int sprite, int mirrored, const unsigned char *colour, const int *box,
                     const unsigned char *bitmap, int bitmapW, int bitmapH, int bitmapId) {
    const unsigned char *table, *data;
    unsigned long tableSize, dataSize, start;
    Picture tex = { 0, 0, NULL };
    int texture, outW, outH, x, y, pad = 3;
    unsigned char *alpha = NULL;

    free(sMap.rgba);
    sMap.rgba = NULL;
    sMapSprite = sprite;
    sMapMirrored = mirrored;
    sMapBitmap = bitmap;
    sMapBitmapId = bitmapId;
    memcpy(sMapColour, colour, 3);
    memcpy(sMapBox, box, sizeof(sMapBox));
    if (bitmap != NULL) {
        // The game drew this map itself: its pixels are the texture, with no offset.
        tex.width = bitmapW;
        tex.height = bitmapH;
        tex.rgba = malloc((size_t) bitmapW * bitmapH * 4);
        if (tex.rgba == NULL) {
            return;
        }
        memcpy(tex.rgba, bitmap, (size_t) bitmapW * bitmapH * 4);
        sMapTexX = sMapTexY = 0;
        goto have_texture;
    }
    table = section(ASSET_SPRITES_TABLE, &tableSize);
    data = section(ASSET_SPRITES, &dataSize);
    if (sprite < 0 || table == NULL || data == NULL || (unsigned long) (sprite + 2) * 4 > tableSize) {
        return;
    }
    start = read_be32(table + sprite * 4);
    if (start + 13 > dataSize) {
        return;
    }
    // SpriteAsset: the first texture, the frames, the anchor, then each frame's first texture.
    texture = (short) read_be16(data + start) + data[start + 12];
    if (!load_texture_frame(texture, 0, &tex, &sMapTexX, &sMapTexY)) {
        return;
    }
have_texture:
    sMapTexWidth = tex.width;
    // As large as the panel allows, scaled smoothly: the map is a mask, so its edge is redrawn sharp below.
    sMapScale = (float) (box[2] - box[0] - 10) / tex.width;
    if ((float) (box[3] - box[1] - 10) / tex.height < sMapScale) {
        sMapScale = (float) (box[3] - box[1] - 10) / tex.height;
    }
    outW = (int) (tex.width * sMapScale) + pad * 2;
    outH = (int) (tex.height * sMapScale) + pad * 2;
    sMap.rgba = calloc(outW * outH, 4);
    alpha = calloc(outW * outH, 1);
    if (sMap.rgba == NULL || alpha == NULL) {
        free(sMap.rgba);
        sMap.rgba = NULL;
        free(alpha);
        free(tex.rgba);
        return;
    }
    sMap.width = outW;
    sMap.height = outH;
    for (y = 0; y < outH; y++) {
        for (x = 0; x < outW; x++) {
            // Bilinear, from the centre of the output pixel.
            float u = (x - pad + 0.5f) / sMapScale - 0.5f, v = (y - pad + 0.5f) / sMapScale - 0.5f;
            int u0 = (int) (u + 1) - 1, v0 = (int) (v + 1) - 1, c, i;
            float fu = u - u0, fv = v - v0, sum[4] = { 0, 0, 0, 0 };
            unsigned char *o = sMap.rgba + (y * outW + x) * 4;
            int a;
            for (i = 0; i < 4; i++) {
                int su = u0 + (i & 1), sv = v0 + (i >> 1);
                float w = ((i & 1) ? fu : 1 - fu) * ((i >> 1) ? fv : 1 - fv);
                const unsigned char *t;
                if (mirrored) {
                    su = tex.width - 1 - su;
                }
                if (su < 0 || sv < 0 || su >= tex.width || sv >= tex.height) {
                    continue;
                }
                t = tex.rgba + (sv * tex.width + su) * 4;
                for (c = 0; c < 3; c++) {
                    sum[c] += t[c] * t[3] * w;
                }
                sum[3] += t[3] * w;
            }
            // A sharp edge: the soft alpha is stretched so the edge is about one pixel wide (the grid: none, every
            // pixel in or out, and its colour the nearest texel's, so the start line's checks stay square).
            a = (int) ((sum[3] - 70) * 255 / 80);
            a = a < 0 ? 0 : a > 255 ? 255 : a;
            if (sGrid) {
                int nu = (int) (u + 0.5f), nv = (int) (v + 0.5f);
                a = sum[3] >= 128 ? 255 : 0;
                if (mirrored) {
                    nu = tex.width - 1 - nu;
                }
                if (a && nu >= 0 && nv >= 0 && nu < tex.width && nv < tex.height && tex.rgba[(nv * tex.width + nu) * 4 + 3] >= 128) {
                    const unsigned char *t = tex.rgba + (nv * tex.width + nu) * 4;
                    for (c = 0; c < 3; c++) {
                        sum[c] = (float) t[c] * sum[3];
                    }
                }
            }
            alpha[y * outW + x] = (unsigned char) a;
            for (c = 0; c < 3; c++) {
                // The light track with a touch of the level's own map colour.
                int lum = sum[3] > 0 ? (int) (sum[c] / sum[3]) : 0;
                int tint = (255 * 7 + colour[c]) / 8;
                o[c] = (unsigned char) (lum * tint / 255);
            }
            o[3] = (unsigned char) a;
        }
    }
    if (sGrid) {
        /*
         * The grid's map, pixel-clean: lone track pixels (a jagged bend's spur) dropped; every track pixel either the
         * track's light colour or the start line's dark check (no grey half-tones); the track's own edge pixel always
         * light, so the checks sit inside the rim, not on it.
         */
        int pass, i, light[3] = { 0, 0, 0 }, nl = 0, hist[64] = { 0 }, runs = 0, acc = 0;
        for (pass = 0; pass < 2; pass++) {
            for (y = 0; y < outH; y++) {
                for (x = 0; x < outW; x++) {
                    int nb = 0;
                    if (!alpha[y * outW + x]) {
                        continue;
                    }
                    nb += x > 0 && alpha[y * outW + x - 1];
                    nb += x + 1 < outW && alpha[y * outW + x + 1];
                    nb += y > 0 && alpha[(y - 1) * outW + x];
                    nb += y + 1 < outH && alpha[(y + 1) * outW + x];
                    if (nb < 2) {
                        alpha[y * outW + x] = 0;
                        sMap.rgba[(y * outW + x) * 4 + 3] = 0;
                    }
                }
            }
        }
        for (i = 0; i < outW * outH; i++) {
            const unsigned char *o = sMap.rgba + i * 4;
            if (alpha[i] && o[0] + o[1] + o[2] >= 3 * 160) {
                light[0] += o[0];
                light[1] += o[1];
                light[2] += o[2];
                nl++;
            }
        }
        for (i = 0; i < 3 && nl; i++) {
            light[i] /= nl;
        }
        for (y = 0; y < outH; y++) {
            for (x = 0; x < outW; x++) {
                unsigned char *o = sMap.rgba + (y * outW + x) * 4;
                int edge, c;
                if (!alpha[y * outW + x]) {
                    continue;
                }
                edge = x == 0 || y == 0 || x + 1 == outW || y + 1 == outH || !alpha[y * outW + x - 1] ||
                       !alpha[y * outW + x + 1] || !alpha[(y - 1) * outW + x] || !alpha[(y + 1) * outW + x];
                if (!edge && o[0] + o[1] + o[2] < 3 * 128) {
                    o[0] = 16, o[1] = 18, o[2] = 28; /* a dark check */
                } else {
                    for (c = 0; c < 3; c++) {
                        o[c] = (unsigned char) light[c];
                    }
                }
            }
        }
        /* the lane's width: the shorter of a track pixel's horizontal and vertical runs, typically (30th percentile) */
        for (y = 0; y < outH; y++) {
            for (x = 0; x < outW; x++) {
                int a0, a1, b0, b1, w;
                if (!alpha[y * outW + x] || (x + y) % 3) {
                    continue;
                }
                for (a0 = x; a0 > 0 && alpha[y * outW + a0 - 1]; a0--) {
                }
                for (a1 = x; a1 + 1 < outW && alpha[y * outW + a1 + 1]; a1++) {
                }
                for (b0 = y; b0 > 0 && alpha[(b0 - 1) * outW + x]; b0--) {
                }
                for (b1 = y; b1 + 1 < outH && alpha[(b1 + 1) * outW + x]; b1++) {
                }
                w = a1 - a0 + 1 < b1 - b0 + 1 ? a1 - a0 + 1 : b1 - b0 + 1;
                hist[w < 63 ? w : 63]++;
                runs++;
            }
        }
        sMapLane = 99;
        for (i = 0; i < 64 && runs; i++) {
            acc += hist[i];
            if (acc * 10 >= runs * 3) {
                sMapLane = i;
                break;
            }
        }
    }
    sMapRebuilt = 0;
    sPathN = 0;
    sArrowSmall = 0;
    if (sMapRebuild && sGrid && bitmap == NULL && !MAP_DUMPING && map_from_paths(outW, outH, sprite)) {
        sMapRebuilt = 1;
    }
#ifndef __3DS__
    else if (sMapRebuild && sGrid && getenv("DKR_DUMP_SKEL") != NULL) {
        /* host: the map traced to its centre line for tools/map_paths (the picture is left redrawn: a dump run only) */
        map_rebuild(outW, outH, alpha);
        if (sMapRebuilt) {
            /* host: the traced centre line, for tools/map_paths (maps with no checkpoint path) */
            FILE *f = fopen(getenv("DKR_DUMP_SKEL"), "a");
            if (f != NULL) {
                int i;
                fprintf(f, "map sprite %d bitmap %d scale %.5f tex %d %d w %d h %d\n", sprite, bitmap != NULL, sMapScale,
                        sMapTexX, sMapTexY, outW, outH);
                for (i = 0; i < outW * outH; i++) {
                    if (sMapSkel[i]) {
                        fprintf(f, "s %d %d\n", i % outW, i / outW);
                    }
                }
                fclose(f);
            }
        }
    }
#endif
    // A dark rim, two pixels wide, so the track stands off the panel.
    for (y = 0; y < outH && !sMapRebuilt; y++) {
        for (x = 0; x < outW; x++) {
            unsigned char *o = sMap.rgba + (y * outW + x) * 4;
            int i, j, rim = 0;
            for (j = -2; j <= 2; j++) {
                for (i = -2; i <= 2; i++) {
                    int xx = x + i, yy = y + j;
                    if (xx >= 0 && yy >= 0 && xx < outW && yy < outH && alpha[yy * outW + xx] > rim) {
                        rim = alpha[yy * outW + xx];
                    }
                }
            }
            rim = sGrid ? (rim ? 255 : 0) : rim * 200 / 255; /* the grid: a solid rim, no soft edge */
            if (o[3] < 255 && rim > 0) {
                // The track over its rim.
                int a = o[3], out = a + rim * (255 - a) / 255, c;
                static const unsigned char sRim[3] = { 0, 10, 60 };
                for (c = 0; c < 3; c++) {
                    o[c] = out > 0 ? (unsigned char) ((o[c] * a + sRim[c] * rim * (255 - a) / 255) / out) : 0;
                }
                o[3] = (unsigned char) out;
            }
        }
    }
    {
        // (The grid: a gap of the panel narrower than 4 px between two strands' rims is filled, one solid rim.)
        if (sGrid && !sMapRebuilt) {
            static const unsigned char sRimC[3] = { 0, 10, 60 };
            int pass;
            for (pass = 0; pass < 2; pass++) {
                for (y = 0; y < outH; y++) {
                    for (x = 0; x < outW; x++) {
                        unsigned char *o = sMap.rgba + (y * outW + x) * 4;
                        int l = 0, r = 0, u = 0, d = 0, k;
                        if (o[3] != 0) {
                            continue;
                        }
                        for (k = 1; k <= 3; k++) {
                            l |= x - k >= 0 && sMap.rgba[(y * outW + x - k) * 4 + 3] != 0;
                            r |= x + k < outW && sMap.rgba[(y * outW + x + k) * 4 + 3] != 0;
                            u |= y - k >= 0 && sMap.rgba[((y - k) * outW + x) * 4 + 3] != 0;
                            d |= y + k < outH && sMap.rgba[((y + k) * outW + x) * 4 + 3] != 0;
                        }
                        if ((l && r) || (u && d)) {
                            o[0] = sRimC[0], o[1] = sRimC[1], o[2] = sRimC[2], o[3] = 254; /* (254: filled this pass) */
                        }
                    }
                }
                for (y = 0; y < outW * outH; y++) {
                    if (sMap.rgba[y * 4 + 3] == 254) {
                        sMap.rgba[y * 4 + 3] = 255;
                    }
                }
            }
            /* then any speck of the panel the passes left, one or two pixels across, until none is left */
            for (pass = 0; pass < 8; pass++) {
                int filled = 0;
                for (y = 0; y < outH; y++) {
                    for (x = 0; x < outW; x++) {
                        unsigned char *o = sMap.rgba + (y * outW + x) * 4;
                        int h = 0, v = 0, k;
                        if (o[3] != 0) {
                            continue;
                        }
                        for (k = 1; k <= 2 && !h; k++) {
                            h = x - 1 >= 0 && sMap.rgba[(y * outW + x - 1) * 4 + 3] != 0 && x + k < outW &&
                                sMap.rgba[(y * outW + x + k) * 4 + 3] != 0;
                            h |= x + 1 < outW && sMap.rgba[(y * outW + x + 1) * 4 + 3] != 0 && x - k >= 0 &&
                                 sMap.rgba[(y * outW + x - k) * 4 + 3] != 0;
                        }
                        for (k = 1; k <= 2 && !v; k++) {
                            v = y - 1 >= 0 && sMap.rgba[((y - 1) * outW + x) * 4 + 3] != 0 && y + k < outH &&
                                sMap.rgba[((y + k) * outW + x) * 4 + 3] != 0;
                            v |= y + 1 < outH && sMap.rgba[((y + 1) * outW + x) * 4 + 3] != 0 && y - k >= 0 &&
                                 sMap.rgba[((y - k) * outW + x) * 4 + 3] != 0;
                        }
                        if (h || v) {
                            o[0] = sRimC[0], o[1] = sRimC[1], o[2] = sRimC[2], o[3] = 255;
                            filled++;
                        }
                    }
                }
                if (!filled) {
                    break;
                }
            }
        }
        if (sMapRebuild && sGrid && !sMapRebuilt) {
            map_tidy_outline(outW, outH, bitmap != NULL ? 1000 + bitmapId : sprite);
        }
        // Centre what is drawn, not the sprite's rectangle: its empty margins differ from track to track.
        int minX = outW, maxX = -1, minY = outH, maxY = -1;
        for (y = 0; y < outH; y++) {
            for (x = 0; x < outW; x++) {
                if (sMap.rgba[(y * outW + x) * 4 + 3] != 0) {
                    minX = x < minX ? x : minX;
                    maxX = x > maxX ? x : maxX;
                    minY = y < minY ? y : minY;
                    maxY = y > maxY ? y : maxY;
                }
            }
        }
        if (maxX < minX) {
            minX = 0;
            maxX = outW - 1;
            minY = 0;
            maxY = outH - 1;
        }
        sMapX = (box[0] + box[2]) / 2 - (minX + maxX + 1) / 2;
        sMapY = (box[1] + box[3]) / 2 - (minY + maxY + 1) / 2;
    }
    free(alpha);
    free(tex.rgba);
}

/*
 * Where a racer's map position (race_hud.h: sprite pixels times 4) is on the screen. The position is off by about a
 * pixel from where the game's marker sprites show (their anchor): moved back by this much, racers sit on the track
 * all the way round (measured on PC on two tracks).
 */
static void map_point(const RaceHudPoint *r, int *x, int *y) {
    // (A sprite's marker anchor sits about a pixel off; the game's own map picture has none.)
    float u = r->mapX / 4.0f - sMapTexX - (sMapBitmap ? 0.0f : 1.5f);
    float v = r->mapY / 4.0f - sMapTexY - (sMapBitmap ? 0.0f : 1.0f);
    if (sMapMirrored) {
        u = sMapTexWidth - u;
    }
    *x = sMapX + 3 + (int) (u * sMapScale);
    *y = sMapY + 3 + (int) (v * sMapScale);
}

/* The marker sizes the grid's map allows: a marker sits inside the lane, clear of the rim (sMapLane, load_map). */
static int marker_small(void) {
    return sGrid && sMapLane < 9;
}

static void dot(int cx, int cy, Colour c) {
    int x, y, fill = marker_small() ? 2 : 6, ring = marker_small() ? 5 : 11;
    for (y = -3; y <= 3; y++) {
        for (x = -3; x <= 3; x++) {
            int d = x * x + y * y;
            if (d <= fill) {
                plot(cx + x, cy + y, c.r, c.g, c.b, 255);
            } else if (d <= ring) {
                plot(cx + x, cy + y, 0, 4, 30, sGrid ? 255 : 220);
            }
        }
    }
    if (!marker_small()) {
        plot(cx - 1, cy - 1, 255, 255, 255, 170);
    }
}

/* How much the player's arrow shrinks to fit the grid's lane, across it with a light pixel each side (1 elsewhere). */
static float arrow_scale(void) {
    float k = 1.0f;
    if (sGrid && sMapLane < 16) {
        k = (sMapLane - 4) / 10.0f;
        if (k < 0.5f) {
            k = 0.5f;
        }
    }
    return k;
}

/*
 * The player's arrow as pixels round its centre (mask[10][10]): 1 the gold, 2 its outline. The triangle is sampled at
 * pixel centres and ringed with a solid pixel on every side it has (its eight neighbours), so no edge stipples.
 */
static void arrow_mask(unsigned heading, unsigned char mask[21][21]) {
    static const float sShape[3][2] = { { 0, -7 }, { 5, 5 }, { -5, 5 } };
    float angle = heading * (6.2831853f / 65536.0f), c = cosf(angle), s = sinf(angle), p[3][2], k = arrow_scale();
    int i, x, y, dx, dy;
    for (i = 0; i < 3; i++) {
        // Counter-clockwise on a screen whose y grows downwards.
        p[i][0] = k * (sShape[i][0] * c + sShape[i][1] * s);
        p[i][1] = k * (-sShape[i][0] * s + sShape[i][1] * c);
    }
    memset(mask, 0, 21 * 21);
    for (y = -9; y <= 9; y++) {
        for (x = -9; x <= 9; x++) {
            int inside = 1;
            for (i = 0; i < 3 && inside; i++) {
                const float *a = p[i], *b = p[(i + 1) % 3];
                inside = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]) >= 0;
            }
            mask[y + 10][x + 10] = (unsigned char) inside;
        }
    }
    for (y = 0; y < 21; y++) {
        for (x = 0; x < 21; x++) {
            for (dy = -1; dy <= 1 && mask[y][x] == 0; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    if (y + dy >= 0 && y + dy < 21 && x + dx >= 0 && x + dx < 21 && mask[y + dy][x + dx] == 1) {
                        mask[y][x] = 2;
                        break;
                    }
                }
            }
        }
    }
}

/* The player's arrow: the HUD's gold, pointing along heading (0 up, counter-clockwise); on the grid's maps it shrinks
 * to fit the lane. */
static void arrow_marker(int cx, int cy, unsigned heading) {
    unsigned char mask[21][21];
    int x, y;
    arrow_mask(heading, mask);
    for (y = -10; y <= 10; y++) {
        for (x = -10; x <= 10; x++) {
            if (mask[y + 10][x + 10] == 1) {
                plot(cx + x, cy + y, y < 0 ? 255 : 240, y < 0 ? 236 : 190, y < 0 ? 90 : 20, 255);
            } else if (mask[y + 10][x + 10] == 2) {
                plot(cx + x, cy + y, 60, 20, 0, 255);
            }
        }
    }
}

/* The hub's layout: the progress down the left, the map across the rest, the amulets under it. */
#define HUB_MAP_Y0 4
#define HUB_MAP_Y1 160
#define HUB_AMULET_Y0 164

/* The menus' companions: the tracks menu's map panel and records panel, the file select's progress panel. */
#define MENU_PANEL_Y0 4
#define MENU_PANEL_Y1 196
#define MENU_MAP_X0 4
#define MENU_MAP_X1 196
#define MENU_RECORDS_X0 200
#define MENU_RECORDS_X1 316
#define SAVE_PANEL_Y0 42

/*
 * A level's map sprite and colour, as its model in the ROM gives them (tracks.c, generate_track: the level header's
 * geometry is the model; LevelModel's minimapSpriteIndex and minimapColor), or -1. Remembered for each level.
 */
static int level_map(int level, unsigned char *colour) {
    enum { LEVELS = 128 };
    static signed char sKnown[LEVELS]; /* 0 not read, 1 read, -1 none. */
    static int sSprites[LEVELS];
    static unsigned char sColours[LEVELS][3];
    const unsigned char *headers, *headerData, *models, *modelData;
    unsigned long headersSize, headerDataSize, modelsSize, modelDataSize, start, end, size;
    unsigned char *model;
    int geometry;
    if (level < 0 || level >= LEVELS) {
        return -1;
    }
    if (sKnown[level] == 0) {
        sKnown[level] = -1;
        headers = section(ASSET_LEVEL_HEADERS_TABLE, &headersSize);
        headerData = section(ASSET_LEVEL_HEADERS, &headerDataSize);
        models = section(ASSET_LEVEL_MODELS_TABLE, &modelsSize);
        modelData = section(ASSET_LEVEL_MODELS, &modelDataSize);
        if (headers == NULL || headerData == NULL || models == NULL || modelData == NULL ||
            (unsigned long) (level + 1) * 4 > headersSize) {
            return -1;
        }
        start = read_be32(headers + level * 4);
        if (start + 0x36 > headerDataSize) {
            return -1;
        }
        geometry = (short) read_be16(headerData + start + 0x34);
        if (geometry < 0 || (unsigned long) (geometry + 2) * 4 > modelsSize) {
            return -1;
        }
        start = read_be32(models + geometry * 4);
        end = read_be32(models + geometry * 4 + 4);
        if (end > modelDataSize || end < start + 5) {
            return -1;
        }
        // A 5 byte header (the size, little endian, then the level) and the deflate stream.
        size = modelData[start] | (modelData[start + 1] << 8) | (modelData[start + 2] << 16) |
               ((unsigned long) modelData[start + 3] << 24);
        if (size < 0x40 || size > 2 * 1024 * 1024 || (model = malloc(size + 64)) == NULL) {
            return -1;
        }
        if (port_inflate(modelData + start + 5, model) >= 0x40) {
            sSprites[level] = (int) read_be32(model + 0x20);
            memcpy(sColours[level], model + 0x39, 3);
            sKnown[level] = 1;
        }
        free(model);
    }
    if (sKnown[level] != 1) {
        return -1;
    }
    memcpy(colour, sColours[level], 3);
    return sSprites[level];
}

/* The panel each mode's map goes in: x0, y0, x1, y1. */
static void map_box(int mode, int *box) {
    if (mode == RACE_HUD_MODE_MENU_TRACK) {
        box[0] = MENU_MAP_X0 + 4;
        box[1] = MENU_PANEL_Y0 + 24;
        box[2] = MENU_MAP_X1 - 4;
        box[3] = MENU_PANEL_Y1 - 4;
    } else if (mode == RACE_HUD_MODE_HUB) {
        box[0] = MIDDLE_X0;
        box[1] = HUB_MAP_Y0 + 18;
        box[2] = RIGHT_X1;
        box[3] = HUB_MAP_Y1 - 14;
    } else {
        box[0] = RIGHT_X0;
        box[1] = LOWER_Y0;
        box[2] = RIGHT_X1;
        box[3] = LOWER_Y1;
    }
}

/* Whether this race is drawn with a look's own layout rather than the Classic panels (defined with them below). */
static int own_race_layout(const RaceHud *hud);
static int dock_tt(const RaceHud *hud);
static int gshows_map(const RaceHud *hud);
static void gmap_box(const RaceHud *hud, int *box);
static void gbase_panels(const RaceHud *hud);

/* The parts that only change with the track or the mode: the background, the panels and the map. */
static int sRaceBaseMode = -1;
static void make_race_base(const RaceHud *hud) {
    if (sLogResetBase) {
        sRaceBaseMade = 0;
        sLogResetBase = 0;
    }
    unsigned short *image = sImage;
    int top = sClipTop, end = sClipEnd, mapBox[4], sprite = hud->mapSprite;
    unsigned char colour[3];
    memcpy(colour, hud->mapColour, 3);
    if (hud->mode == RACE_HUD_MODE_MENU_TRACK || (sprite < 0 && hud->level > 0)) {
        // (A race's own level is only set by the host preview, which has no track model to take the map from.)
        sprite = level_map(hud->level, colour);
    }
    if (hud->mapBitmap != NULL) {
        sprite = -1;
    }
    map_box(hud->mode, mapBox);
    if (sGrid) {
        gmap_box(hud, mapBox);
    } else if (own_race_layout(hud) || dock_tt(hud)) {
        // Dock's map: the big one on the left. Glass and Broadcast have none.
        mapBox[0] = 6;
        mapBox[1] = dock_tt(hud) ? 48 : 52;
        mapBox[2] = 150;
        mapBox[3] = dock_tt(hud) ? 134 : 198;
        if (sGrid && !dock_tt(hud)) {
            mapBox[0] = 6 + 6;
            mapBox[1] = 4 + 2 * 26 + 6;
            mapBox[2] = 158 - 6;
            mapBox[3] = 208 - 6;
        }
    }
    {
        /* the grid redraws every map but the hub's own picture from its centre line (map_rebuild) */
        int want = sGrid && hud->mode != RACE_HUD_MODE_HUB && hud->mode != RACE_HUD_MODE_MENU_SAVE;
        int open = hud->mode == RACE_HUD_MODE_BATTLE || hud->mode == RACE_HUD_MODE_BANANAS || hud->mode == RACE_HUD_MODE_EGGS;
        if (want != sMapRebuild || open != sMapOpen) {
            sMapRebuild = want;
            sMapOpen = open;
            sMapSprite = -2; /* (loaded again) */
        }
    }
    if (sprite != sMapSprite || hud->mapBitmap != sMapBitmap ||
        (hud->mapBitmap != NULL && hud->mapBitmapId != sMapBitmapId) || hud->mapMirrored != sMapMirrored ||
        memcmp(colour, sMapColour, 3) || memcmp(mapBox, sMapBox, sizeof(mapBox))) {
        load_map(sprite, hud->mapMirrored, colour, mapBox, hud->mapBitmap, hud->mapBitmapW, hud->mapBitmapH,
                 hud->mapBitmapId);
    } else if (sRaceBaseMade && sRaceBaseMode == hud->mode) {
        return;
    }
    sImage = sRaceBase;
    sClipTop = 0;
    sClipEnd = RACE_ROWS_Y;
    memcpy(sRaceBase, sBackground, sizeof(sRaceBase));
    if (sGrid) {
        gbase_panels(hud);
    } else if (hud->mode == RACE_HUD_MODE_MENU_TRACK) {
        box(MENU_MAP_X0, MENU_PANEL_Y0, MENU_MAP_X1, MENU_PANEL_Y1, BOX_FILL, BOX_EDGE);
        box(MENU_RECORDS_X0, MENU_PANEL_Y0, MENU_RECORDS_X1, MENU_PANEL_Y1, BOX_FILL, BOX_EDGE);
    } else if (hud->mode == RACE_HUD_MODE_MENU_SAVE && sGrid) {
        // The grid's save screen draws its own panels.
    } else if (hud->mode == RACE_HUD_MODE_MENU_SAVE) {
        if (sStyle <= 1) {
            box(6, SAVE_PANEL_Y0, BS_WIDTH - 6, MENU_PANEL_Y1, BOX_FILL, BOX_EDGE);
        }
        if (sSmallLogo.rgba != NULL) {
            picture(&sSmallLogo, 6, sStyle == 2 ? 8 : 4);
        }
    } else if (hud->mode == RACE_HUD_MODE_HUB) {
        box(TOWER_X0, UPPER_Y0, TOWER_X1, LOWER_Y1, BOX_FILL, BOX_EDGE);
        box(MIDDLE_X0, HUB_MAP_Y0, RIGHT_X1, HUB_MAP_Y1, BOX_FILL, BOX_EDGE);
        box(MIDDLE_X0, HUB_AMULET_Y0, RIGHT_X1, LOWER_Y1, BOX_FILL, BOX_EDGE);
    } else if (sGrid && own_race_layout(hud) && sStyle == 3) {
        gbox(6, 4 + 2 * 26, 158, 208, SK_PANEL); /* the map's panel (drawn once, under the map) */
    } else if (own_race_layout(hud) || dock_tt(hud)) {
        // The look draws its own panels each frame.
    } else {
        box(TOWER_X0, UPPER_Y0, TOWER_X1, LOWER_Y1, BOX_FILL, BOX_EDGE);
        box(MIDDLE_X0, UPPER_Y0, MIDDLE_X1, UPPER_Y1, BOX_FILL, BOX_EDGE);
        box(RIGHT_X0, UPPER_Y0, RIGHT_X1, UPPER_Y1, BOX_FILL, BOX_EDGE);
        box(MIDDLE_X0, LOWER_Y0, MIDDLE_X1, LOWER_Y1, BOX_FILL, BOX_EDGE);
        box(RIGHT_X0, LOWER_Y0, RIGHT_X1, LOWER_Y1, BOX_FILL, BOX_EDGE);
    }
    if (sMap.rgba != NULL && (sGrid ? gshows_map(hud) : !(own_race_layout(hud) && sStyle != 3 && !dock_tt(hud)))) {
        picture(&sMap, sMapX, sMapY);
    }
    sRaceBaseMade = 1;
    sRaceBaseMode = hud->mode;
    sImage = image;
    sClipTop = top;
    sClipEnd = end;
}

/* The colours of the bosses and Taj, for their frames and map dots (RaceHudRacer.special). */
static const Colour sSpecialColours[RACE_HUD_SPECIALS] = {
    { 0, 175, 0, 255 },   { 0, 128, 255, 255 },   { 255, 64, 32, 255 },
    { 255, 128, 0, 255 }, { 255, 128, 190, 255 }, { 160, 64, 255, 255 },
};

/* A racer's colour; 0 if it has neither a portrait nor a colour (an unknown character). */
static int racer_colour(const RaceHudRacer *r, Colour *out) {
    if (r->special > RACE_HUD_NOT_SPECIAL && r->special < RACE_HUD_SPECIALS) {
        *out = sSpecialColours[r->special];
        return 1;
    }
    if (r->special == RACE_HUD_NOT_SPECIAL && r->character >= 0 && r->character < 10) {
        *out = sCharacterColours[r->character];
        return 1;
    }
    return 0;
}

static void draw_tower(const RaceHud *hud) {
    int i;
    for (i = 0; i < hud->racerCount && i < RACE_HUD_RACERS; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        int y = TOWER_ROW_Y + i * TOWER_ROW_HEIGHT, character = r->character;
        char number[4], gap[16];
        Colour colour;
        if (r->player) {
            // The player's row: lit in the HUD's gold.
            box(TOWER_X0 + 2, y - 1, TOWER_X1 - 2, y + TOWER_ROW_HEIGHT - 1, (Colour){ 255, 200, 40, 70 }, GOLD);
        }
        snprintf(number, sizeof(number), "%d", i + 1);
        text_centred(FONT_FUN, TOWER_X0 + 3, TOWER_X0 + 19, y + 5, number, WHITE, 1);
        if (racer_colour(r, &colour)) {
            // The portrait in a frame of the racer's map colour, so the map's dots can be matched to faces.
            fill(25, y + 1, 47, y + 23, colour);
            fill(24, y, 48, y + 1, (Colour){ 0, 4, 30, 200 });
            fill(24, y + 23, 48, y + 24, (Colour){ 0, 4, 30, 200 });
            if (r->special == RACE_HUD_NOT_SPECIAL) {
                picture(&sPortraits[character], 26, y + 2);
            } else if (sSpecialPortraits[r->special].rgba != NULL) {
                picture(&sSpecialPortraits[r->special], 26, y + 2);
            } else {
                // The name box: the boss's initial on its colour.
                char initial[2] = { (char) (sSpecialNames[r->special][0] - 'a' + 'A'), 0 };
                fill(26, y + 2, 46, y + 22, (Colour){ 0, 20, 90, 255 });
                letter_in_box(FONT_FUN, 26, y + 2, 46, y + 22, initial[0], WHITE);
            }
        }
        if (r->player) {
            text(FONT_SMALL, 53, y + 6, "You", YELLOW, 1);
        } else {
            format_gap(r->gap, gap, sizeof(gap));
            draw_gap(52, y + 6, 0, gap, r->gap > 0 ? AHEAD : BEHIND);
        }
    }
}

static void draw_item(const RaceHud *hud) {
    int index = hud->item * 3 + hud->itemLevel;
    // The slot, then the item at the N64's own size (40 pixels, as sharp as the art is) in it.
    box(117, 24, 165, 72, (Colour){ 0, 0, 20, 60 }, (Colour){ 140, 180, 255, 70 });
    if (hud->item < 0 || index < 0 || index >= 15 || sItems[index].rgba == NULL) {
        if (sNoItemLabel) {
            text_centred(FONT_SMALL, 117, 165, 38, "No", DIM, 1);
            text_centred(FONT_SMALL, 117, 165, 38 + line_pitch(), "item", DIM, 1);
        }
        return;
    }
    picture_scaled(&sItems[index], 121, 28, 1, 1);
    if (hud->itemCount > 1 && hud->itemCount <= 10) {
        piece(&sItemCounts[hud->itemCount - 1], 143, 50, 0);
    }
}

/* Measures what drawing would take: runs the drawing with every row clipped away and gives the result. */
#define DRY(expr) \
    ({ \
        int dryEnd_ = sClipEnd, dryValue_; \
        sClipEnd = -1; \
        dryValue_ = (expr); \
        sClipEnd = dryEnd_; \
        dryValue_; \
    })

/* The distance between two lines of small text: the look fonts are a little taller than the game's. */
static int line_pitch(void) {
    return sStyle == 0 ? 11 : 13;
}

/* Small text centred between x0 and x1; a line too wide for the panel breaks at spaces. Returns the next line's y. */
static int wrapped_centred(int x0, int x1, int y, const char *s, Colour colour) {
    char line[48];
    int start = 0, len, lastSpace, i;
    s = port_text(s);
    len = (int) strlen(s);
    while (start < len) {
        int end = len;
        lastSpace = -1;
        for (i = start; i <= len; i++) {
            if (s[i] == ' ' || s[i] == '\0') {
                char save = s[i];
                int n = i - start, w;
                if (n >= (int) sizeof(line)) {
                    break;
                }
                memcpy(line, s + start, n);
                line[n] = '\0';
                w = text_width_as_is(FONT_SMALL, line);
                (void) save;
                if (w > x1 - x0 - 6 && lastSpace >= 0) {
                    break;
                }
                lastSpace = i;
            }
        }
        end = lastSpace >= 0 ? lastSpace : len;
        memcpy(line, s + start, end - start);
        line[end - start] = '\0';
        text_centred_as_is(FONT_SMALL, x0, x1, y, line, colour, 1);
        y += line_pitch();
        start = end + 1;
    }
    return y;
}

/* The x that centres something width wide in the top right panel. */
static int right_centred(int width) {
    return RIGHT_X0 + (RIGHT_X1 - RIGHT_X0 - width) / 2;
}

/* "Lap 2/3", "FINAL LAP" or "FINISH" from x; returns where it ends (x when there is none). */
static int lap_row(const RaceHud *hud, int x, int y) {
    if (hud->finished) {
        return piece(&sFinishLabel, x, y, 0);
    } else if (hud->laps > 0 && hud->lap >= hud->laps && hud->laps > 1) {
        x = piece(&sFinalLabel, x, y, 7);
        return piece(&sLapSmallLabel, x, y, 0);
    } else if (hud->laps > 0) {
        x = piece(&sLapLabel, x, y + 3, 4);
        x = piece(&sDigits[hud->lap % 10], x, y, 1);
        x = piece(&sSlash, x, y + 1, 1);
        return piece(&sDigits[hud->laps % 10], x, y, 0);
    }
    return x;
}

static int place_row(int place, int x, int y) {
    x = piece(&sPlaces[place - 1], x, y, 1);
    return piece(&sSuffixes[place <= 3 ? place - 1 : 3], x, y + 2, 0);
}

static int time_row(const RaceHud *hud, int x, int y, int small) {
    const Picture *digit = small ? &sSmallDigits[0] : &sDigits[0];
    int labelY = y + digit->height - sTimeLabel.height; /* bottoms on one line */
    x = piece(&sTimeLabel, x, labelY, 4);
    return race_time(x, y, hud->raceTime, small);
}

/* The widest the clock row can be (TIME, then MM:SS:HH with every digit the set's widest): the Classic look trims each
 * digit to its used columns, so the clock's own width changes as the digits tick. */
static int time_row_widest(int y, int small) {
    const Picture *set = small ? sSmallDigits : sDigits;
    const Picture *colon = small ? &sSmallColon : &sColon;
    const Picture *digit = &set[0];
    int widest = 0, d, i, x;
    for (d = 0; d < 10; d++) {
        int w = DRY(piece(&set[d], 0, y, 1));
        if (w > widest) {
            widest = w;
            digit = &set[d];
        }
    }
    x = DRY(piece(&sTimeLabel, 0, y, 4));
    for (i = 0; i < 6; i++) {
        x = DRY(piece(digit, x, y, 1));
        if (i == 1 || i == 3) {
            x = DRY(piece(colon, x, y, 1));
        }
    }
    return x;
}

/* The clock's size and where it starts: the big digits when they fit, else the small ones. Decided on the widest the
 * clock can get, not on the time shown, so the size doesn't flip back and forth as the digits change. */
static int time_small(const RaceHud *hud, int y) {
    (void) hud;
    return time_row_widest(y, 0) > RIGHT_X1 - RIGHT_X0 - 8;
}

/* The x where the lap line and the clock both start, so their left edges line up (centred as a pair). */
static int lap_time_left(const RaceHud *hud, int lapWidth, int y) {
    int small = time_small(hud, y);
    int tw = DRY(time_row(hud, 0, small ? y + 2 : y, small));
    int w = lapWidth > tw ? lapWidth : tw;
    return right_centred(w);
}

static void time_at(const RaceHud *hud, int x, int y) {
    int small = time_small(hud, y);
    time_row(hud, x, small ? y + 2 : y, small);
}

static void time_centred(const RaceHud *hud, int y) {
    int small = time_small(hud, y);
    int yy = small ? y + 2 : y;
    time_row(hud, right_centred(DRY(time_row(hud, 0, yy, small))), yy, small);
}

static void draw_standing(const RaceHud *hud) {
    int lapW = hud->laps > 0 || hud->finished ? DRY(lap_row(hud, 0, 44)) : 0, left;
    if (hud->position >= 1 && hud->position <= 8) {
        place_row(hud->position, right_centred(DRY(place_row(hud->position, 0, 9))), 9);
    }
    left = lap_time_left(hud, lapW, 68);
    if (lapW > 0) {
        lap_row(hud, left, 44);
    }
    time_at(hud, left, 68);
}

/* The laps the HUD has times for, and of those the finished ones (after the finish line, all of them). */
static void timed_laps(const RaceHud *hud, int *timed, int *done) {
    *timed = hud->lapsTimed > RACE_HUD_LAPS ? RACE_HUD_LAPS : hud->lapsTimed;
    if (hud->finished && hud->laps > 0 && *timed > hud->laps) {
        // The game starts timing another lap as the player crosses the line.
        *timed = hud->laps;
    }
    *done = hud->finished ? *timed : *timed - 1;
}

static void draw_bananas_and_laps(const RaceHud *hud) {
    int x, best = -1, i, timed, done;
    char label[16];
    picture_scaled(&sBanana, MIDDLE_X0 + 6, LOWER_Y0 + 3, 1, 1);
    x = piece(&sTimes, MIDDLE_X0 + 42, LOWER_Y0 + 17, 2);
    x = piece(&sDigits[(hud->bananas / 10) % 10], x, LOWER_Y0 + 14, 1);
    piece(&sDigits[hud->bananas % 10], x, LOWER_Y0 + 14, 0);
    // The last finished lap (or the lap so far, on the first) and the best finished lap.
    timed_laps(hud, &timed, &done);
    for (i = 0; i < done; i++) {
        if (best < 0 || hud->lapTimes[i] < best) {
            best = hud->lapTimes[i];
        }
    }
    if (done > 0) {
        snprintf(label, sizeof(label), port_text("Lap %d"), done);
        text(FONT_SMALL, MIDDLE_X0 + 44, LOWER_Y0 + 42, label, HINT, 1);
        race_time(MIDDLE_X0 + 8, LOWER_Y0 + 58, hud->lapTimes[done - 1], 1);
    } else if (timed > 0) {
        snprintf(label, sizeof(label), port_text("Lap %d"), timed);
        text(FONT_SMALL, MIDDLE_X0 + 44, LOWER_Y0 + 42, label, HINT, 1);
        race_time(MIDDLE_X0 + 8, LOWER_Y0 + 58, hud->lapTimes[timed - 1], 1);
    }
    text(FONT_SMALL, MIDDLE_X0 + 8, LOWER_Y0 + 76, "Best lap", HINT, 1);
    if (best >= 0) {
        race_time(MIDDLE_X0 + 8, LOWER_Y0 + 88, best, 1);
    } else {
        text(FONT_SMALL, MIDDLE_X0 + 8, LOWER_Y0 + 88, "--", DIM, 1);
    }
}

/*
 * The redrawn maps' player marker: a solid triangle pointing the way the racer faces, as the game's own map shows it
 * (up, and up-right for the diagonals, hand-drawn on a 7 x 7 grid), turned in quarter turns to the eight directions,
 * ringed by one solid dark pixel: 9 x 9. dir: 0 up, then clockwise in eighths.
 */
static void arrow_pixels(int dir, int small, unsigned char mask[9][9]) {
    /* up and up-right: a triangle with a sharp tip, as wide at its base as it is long, mirror symmetric about its
       axis. The small one (5 x 5, 7 x 7 ringed) is for the bends where the big one would touch the rim. */
    static const char *const sHead[2] = { "...#......#.....###....###...#####..#####.#######",
                                          "......#...###.######..#####...###.....##......#.." };
    static const char *const sSmall[2] = { ".......#...###.#####.....", "....#..##.####..##....#.." }; /* up: rows 1, 3, 5 wide */
    int x, y, i, j, n = small ? 5 : 7, o = small ? 2 : 1;
    if (small && (dir & 1)) {
        /* 5 px can't draw a clean triangle on the diagonal (it read as a "4"): the small one turns to the nearer
           quarter (clockwise of the diagonal), always a clean triangle */
        dir = (dir + 1) & 7;
    }
    memset(mask, 0, 81);
    for (y = 0; y < n; y++) {
        for (x = 0; x < n; x++) {
            int sx = x, sy = y, q;
            // the head's pixel that lands on (x, y) after (dir / 2) quarter turns clockwise
            for (q = 0; q < (dir & 7) / 2; q++) {
                int t = sx;
                sx = sy;
                sy = n - 1 - t;
            }
            if ((small ? sSmall : sHead)[dir & 1][sy * n + sx] == '#') {
                mask[y + o][x + o] = 1;
            }
        }
    }
    for (y = 0; y < 9; y++) {
        for (x = 0; x < 9; x++) {
            for (j = -1; j <= 1 && mask[y][x] == 0; j++) {
                for (i = -1; i <= 1; i++) {
                    if (y + j >= 0 && y + j < 9 && x + i >= 0 && x + i < 9 && mask[y + j][x + i] == 1) {
                        mask[y][x] = 2;
                        break;
                    }
                }
            }
        }
    }
}


static void arrow_pixel(int cx, int cy, int dir, int small) {
    unsigned char mask[9][9];
    int x, y;
    arrow_pixels(dir, small, mask);
    for (y = 0; y < 9; y++) {
        for (x = 0; x < 9; x++) {
            if (mask[y][x] == 1) {
                plot(cx + x - 4, cy + y - 4, 255, 214, 40, 255);
            } else if (mask[y][x] == 2) {
                plot(cx + x - 4, cy + y - 4, 60, 20, 0, 255);
            }
        }
    }
}

/* Whether a marker (mask, 9 x 9 round (cx, cy)) keeps one light pixel between it and the grid map's rim. */
static int map_clear_at(int cx, int cy, unsigned char mask[9][9]) {
    int x, y, i, j;
    for (y = 0; y < 9; y++) {
        for (x = 0; x < 9; x++) {
            for (j = -1; j <= 1 && mask[y][x] != 0; j++) {
                for (i = -1; i <= 1; i++) {
                    int px = cx + x - 4 + i - sMapX, py = cy + y - 4 + j - sMapY;
                    const unsigned char *t;
                    if (px < 0 || py < 0 || px >= sMap.width || py >= sMap.height) {
                        return 0;
                    }
                    t = sMap.rgba + (py * sMap.width + px) * 4;
                    if (t[3] < 128 || (t[0] < 40 && t[1] < 40 && t[2] < 110)) {
                        return 0; /* the rim (or off the map) */
                    }
                }
            }
        }
    }
    return 1;
}

/* Nudge a marker, by as little as it takes (up to 5 px: past the end of a slot between strands), so it is clear of the rim; else leave it. */
static int map_clear(int *cx, int *cy, unsigned char mask[9][9]) {
    int d, i, j;
    for (d = 0; d <= 25; d++) {
        for (j = -5; j <= 5; j++) {
            for (i = -5; i <= 5; i++) {
                if (i * i + j * j == d && map_clear_at(*cx + i, *cy + j, mask)) {
                    *cx += i;
                    *cy += j;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void dot_pixels(unsigned char mask[9][9]) {
    int x, y, ring = marker_small() ? 5 : 11;
    for (y = 0; y < 9; y++) {
        for (x = 0; x < 9; x++) {
            mask[y][x] = (x - 4) * (x - 4) + (y - 4) * (y - 4) <= ring;
        }
    }
}

/* A marker's place on a redrawn map: on its centre line. The arrow's direction: along the line, the way the racer
   faces (0 up, then clockwise in eighths). */
static void map_on_line(int *x, int *y, unsigned heading, int *dir) {
    float sx, sy, a = heading * (6.2831853f / 65536.0f), hx = -sinf(a), hy = -cosf(a), tx = hx, ty = hy, ang;
    if (path_snap(*x - sMapX + 0.5f, *y - sMapY + 0.5f, &sx, &sy, &tx, &ty)) {
        if (tx * hx + ty * hy < 0) {
            tx = -tx, ty = -ty; /* (the racer's own heading says which way along the line it is going) */
        }
        *x = sMapX + (int) floorf(sx);
        *y = sMapY + (int) floorf(sy);
    }
    ang = atan2f(tx, -ty) * (8.0f / 6.2831853f);
    *dir = ((int) floorf(ang + 0.5f) % 8 + 8) % 8;
}

static void draw_map_markers(const RaceHud *hud) {
    int i, x, y;
    if (sMap.rgba == NULL) {
        return;
    }
    // The others first, last place first, so the leaders are on top; then the player.
    for (i = hud->racerCount - 1; i >= 0; i--) {
        const RaceHudRacer *r = &hud->racers[i];
        Colour colour;
        if (!r->player && r->at.onMap && racer_colour(r, &colour)) {
            map_point(&r->at, &x, &y);
            if (sMapRebuilt) {
                int d;
                map_on_line(&x, &y, r->at.heading, &d);
            }
            if (sGrid) {
                unsigned char m[9][9];
                dot_pixels(m);
                map_clear(&x, &y, m);
            }
            dot(x, y, colour);
        }
    }
    for (i = 0; i < hud->racerCount; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        if (r->player && r->at.onMap) {
            map_point(&r->at, &x, &y);
            if (sMapRebuilt) {
                int d;
                unsigned char m[9][9];
                map_on_line(&x, &y, r->at.heading, &d);
                arrow_pixels(d, 0, m);
                if (!sArrowSmall && map_clear(&x, &y, m)) {
                    arrow_pixel(x, y, d, 0);
                } else {
                    sArrowSmall = 1;
                    arrow_pixels(d, 1, m);
                    map_clear(&x, &y, m);
                    arrow_pixel(x, y, d, 1);
                }
            } else if (sGrid) {
                /* the grid's other maps (the arenas): the same arrow, turned to the racer's heading */
                float ha = r->at.heading * (6.2831853f / 65536.0f), ang = atan2f(-sinf(ha), cosf(ha)) * (8.0f / 6.2831853f);
                int d = ((int) floorf(ang + 0.5f) % 8 + 8) % 8;
                unsigned char m[9][9];
                arrow_pixels(d, 0, m);
                if (!sArrowSmall && map_clear(&x, &y, m)) {
                    arrow_pixel(x, y, d, 0);
                } else {
                    sArrowSmall = 1;
                    arrow_pixels(d, 1, m);
                    map_clear(&x, &y, m);
                    arrow_pixel(x, y, d, 1);
                }
            } else {
                arrow_marker(x, y, r->at.heading);
            }
        }
    }
}

/******** The other modes (race_hud.h RACE_HUD_MODE_*), laid out as the mockups in 3ds/bottom-ui/mode-mockups. ********/

static const Colour SLOT_EDGE = { 140, 180, 255, 70 };
static const Colour UNLIT = { 40, 50, 100, 255 };
static const Colour DARK = { 0, 4, 30, 220 };

/* A picture faded towards the panel's blue: what is not collected yet. */
static void picture_dim(const Picture *p, int x, int y) {
    int i, j;
    for (j = 0; p->rgba != NULL && j < p->height; j++) {
        for (i = 0; i < p->width; i++) {
            const unsigned char *t = p->rgba + (j * p->width + i) * 4;
            if (t[3] != 0) {
                plot(x + i, y + j, (t[0] + 40 * 2) / 3, (t[1] + 50 * 2) / 3, (t[2] + 90 * 2) / 3, t[3] * 3 / 5);
            }
        }
    }
}

/* A small lit or unlit square, as the mockups' meters are made of. */
static void pip(int x, int y, int w, int h, int lit) {
    fill(x - 1, y - 1, x + w + 1, y + h + 1, DARK);
    fill(x, y, x + w, y + h, lit ? GOLD : UNLIT);
}

/* A portrait in its frame at 20 pixels (the tower's) or 40 (the challenges'), greyed out when knocked out. */
static void framed_portrait(const RaceHudRacer *r, int x, int y, int full, int grey) {
    int size = full ? 40 : 20;
    Colour colour;
    if (!racer_colour(r, &colour)) {
        return;
    }
    fill(x - 2, y - 2, x + size + 2, y + size + 2, (Colour){ 0, 4, 30, 200 });
    fill(x - 1, y - 1, x + size + 1, y + size + 1, grey ? (Colour){ 120, 130, 160, 255 } : colour);
    if (r->special == RACE_HUD_NOT_SPECIAL) {
        const Picture *p = &sPortraits[r->character];
        if (full) {
            picture_scaled(p, x, y, 2, 0);
        } else {
            picture(p, x, y);
        }
    } else if (sSpecialPortraits[r->special].rgba != NULL) {
        picture_scaled(&sSpecialPortraits[r->special], x, y, full ? 2 : 1, 0);
    } else {
        char initial[2] = { (char) (sSpecialNames[r->special][0] - 'a' + 'A'), 0 };
        fill(x, y, x + size, y + size, (Colour){ 0, 20, 90, 255 });
        letter_in_box(full ? FONT_BIG : FONT_FUN, x, y, x + size, y + size, initial[0], WHITE);
    }
    if (grey) {
        fill(x, y, x + size, y + size, (Colour){ 40, 40, 70, 150 });
    }
}

/* A seconds gap in the big digits: "-0.08", the sign as a bar (the game has no plus or minus digit). */
static int seconds_gap(int x, int y, int gap) {
    int value = (gap < 0 ? -gap : gap) * 100 / 60, digitsShown = 0, div;
    const Picture *one = &sDigits[1];
    int mid = y + one->height / 2 - 1;
    if (value > 99999) {
        value = 99999;
    }
    fill(x, mid - 1, x + 8, mid + 3, DARK);
    fill(x + 1, mid, x + 7, mid + 2, GOLD);
    if (gap > 0) {
        fill(x + 3, mid - 3, x + 5, mid + 5, DARK);
        fill(x + 3, mid - 2, x + 5, mid + 4, GOLD);
        fill(x + 1, mid, x + 7, mid + 2, GOLD);
    }
    x += 10;
    for (div = 10000; div >= 1; div /= 10) {
        int d = (value / div) % 10;
        if (div > 100 && d == 0 && !digitsShown) {
            continue;
        }
        digitsShown = 1;
        x = piece(&sDigits[d], x, y, 1);
        if (div == 100) {
            // The decimal point: the lower dot of the colon.
            fill(x, y + one->height - 5, x + 4, y + one->height - 1, DARK);
            fill(x + 1, y + one->height - 4, x + 3, y + one->height - 2, GOLD);
            x += 5;
        }
    }
    return x;
}

/* Lap and time, top right, as in a race but without the place (time trial, battle). */
static void draw_lap_and_time(const RaceHud *hud, const char *title, int laps) {
    int lapW = laps && hud->laps > 0 ? DRY(lap_row(hud, 0, 40)) : 0, left = lap_time_left(hud, lapW, 68);
    text_centred(FONT_FUN, RIGHT_X0, RIGHT_X1, 9, title, WHITE, 1);
    if (lapW > 0) {
        lap_row(hud, left, 40);
    }
    time_at(hud, left, 68);
}

/* Lap times down a panel (the lower middle one): the finished laps, then the best. */
static void draw_lap_list(const RaceHud *hud, int x0, int y0) {
    int i, y = y0 + 6, best = -1, timed, done;
    char label[16];
    timed_laps(hud, &timed, &done);
    // The last two laps: the one being driven and the one before.
    for (i = timed > 2 ? timed - 2 : 0; i < timed; i++) {
        snprintf(label, sizeof(label), port_text("Lap %d"), i + 1);
        text(FONT_SMALL, x0 + 8, y, label, HINT, 1);
        race_time(x0 + 8, y + line_pitch(), hud->lapTimes[i], 1);
        y += 27 + (sStyle != 0) * 3;
    }
    for (i = 0; i < done; i++) {
        if (best < 0 || hud->lapTimes[i] < best) {
            best = hud->lapTimes[i];
        }
    }
    text(FONT_SMALL, x0 + 8, y, "Best lap", HINT, 1);
    if (best >= 0) {
        race_time(x0 + 8, y + line_pitch(), best, 1);
    } else {
        text(FONT_SMALL, x0 + 8, y + line_pitch(), "--", DIM, 1);
    }
}

static void draw_extra_point(const RaceHudPoint *p, Colour c) {
    int x, y;
    if (p->onMap && sMap.rgba != NULL) {
        map_point(p, &x, &y);
        dot(x, y, c);
    }
}

/* A see-through arrow for a ghost. */
static void ghost_marker(const RaceHudPoint *p) {
    int x, y, i, j;
    if (!p->onMap || sMap.rgba == NULL) {
        return;
    }
    map_point(p, &x, &y);
    for (j = -4; j <= 4; j++) {
        for (i = -4; i <= 4; i++) {
            int d = i * i + j * j;
            if (d <= 12) {
                plot(x + i, y + j, 220, 235, 255, d <= 6 ? 170 : 110);
            }
        }
    }
}

static void draw_time_trial(const RaceHud *hud) {
    int y = 32, i, best = -1;
    char label[24], delta[16];
    // Left: this run's laps, each against the best run's same lap.
    text(FONT_FUN, TOWER_X0 + 8, 9, "SPLITS", WHITE, 1);
    for (i = 0; i < hud->lapsTimed && i < RACE_HUD_LAPS && i < 4; i++) {
        int finished = i < hud->lapsTimed - 1 || hud->finished;
        snprintf(label, sizeof(label), port_text("Lap %d"), i + 1);
        text(FONT_SMALL, TOWER_X0 + 8, y, label, finished ? HINT : YELLOW, 1);
        if (finished && i < hud->bestLapCount && hud->bestLaps[i] > 0) {
            int d = hud->lapTimes[i] - hud->bestLaps[i];
            format_gap(-d, delta, sizeof(delta));
            // format_gap's "+" is ahead; a lap time under the best is the good one.
            delta[0] = d <= 0 ? '-' : '+';
            text(FONT_SMALL, TOWER_X1 - 6 - text_width(FONT_SMALL, delta), y, delta, d <= 0 ? BEHIND : AHEAD, 1);
        }
        race_time(TOWER_X0 + 8, y + line_pitch(), hud->lapTimes[i], 1);
        y += 30 + (sStyle != 0) * 3;
        if (finished && (best < 0 || hud->lapTimes[i] < best)) {
            best = hud->lapTimes[i];
        }
    }
    fill(TOWER_X0 + 6, y - 2, TOWER_X1 - 6, y - 1, SLOT_EDGE);
    text(FONT_SMALL, TOWER_X0 + 8, y + 2, "Best lap", HINT, 1);
    if (best >= 0) {
        race_time(TOWER_X0 + 8, y + 2 + line_pitch(), best, 1);
    } else {
        text(FONT_SMALL, TOWER_X0 + 8, y + 2 + line_pitch(), "--", DIM, 1);
    }
    if (hud->bestTotal > 0 && y + 54 < LOWER_Y1) {
        text(FONT_SMALL, TOWER_X0 + 8, y + 32, "Best run", HINT, 1);
        race_time(TOWER_X0 + 8, y + 32 + line_pitch(), hud->bestTotal, 1);
    }
    // Middle top: against the best run, the thing to glance at.
    picture_scaled(&sGhostIcon, MIDDLE_X0 + 3, UPPER_Y0 + 5, 1, 1);
    // The same words in every look: "ahead" alone read as the best run being ahead of you.
    text_fit(FONT_SMALL, MIDDLE_X0 + 44, MIDDLE_X1 - 3, 12, "vs best", HINT, 1);
    if (hud->bestGap == RACE_HUD_NO_GAP) {
        text_fit(FONT_SMALL, MIDDLE_X0 + 44, MIDDLE_X1 - 3, 24, hud->bestTotal > 0 ? "" : "none yet", DIM, 1);
    } else {
        text_fit(FONT_SMALL, MIDDLE_X0 + 44, MIDDLE_X1 - 3, 12 + line_pitch(),
                 hud->bestGap <= 0 ? "ahead" : "behind", hud->bestGap <= 0 ? BEHIND : AHEAD, 1);
        seconds_gap(MIDDLE_X0 + 8, 60, hud->bestGap);
    }
    // Middle bottom: the course record and T.T.'s time.
    picture_scaled(&sTimesIcon, MIDDLE_X0 + 6, LOWER_Y0 + 6, 1, 1);
    text(FONT_SMALL, MIDDLE_X0 + 32, LOWER_Y0 + 5, "Record", HINT, 1);
    if (hud->record >= 0) {
        race_time(MIDDLE_X0 + 8, LOWER_Y0 + 28, hud->record, 1);
    } else {
        text(FONT_SMALL, MIDDLE_X0 + 8, LOWER_Y0 + 28, "--", DIM, 1);
    }
    if (hud->staffTime > 0) {
        RaceHudRacer tt = { 8 /* CHARACTER_TT */, RACE_HUD_NOT_SPECIAL, 0 };
        framed_portrait(&tt, MIDDLE_X0 + 8, LOWER_Y0 + 50, 0, 0);
        text(FONT_SMALL, MIDDLE_X0 + 34, LOWER_Y0 + 50, "T.T.", HINT, 1);
        text(FONT_SMALL, MIDDLE_X0 + 34, LOWER_Y0 + 50 + line_pitch(), "to beat", DIM, 1);
        race_time(MIDDLE_X0 + 8, LOWER_Y0 + 78, hud->staffTime, 1);
    } else if (hud->recordLap >= 0) {
        text(FONT_SMALL, MIDDLE_X0 + 8, LOWER_Y0 + 50, "Record lap", HINT, 1);
        race_time(MIDDLE_X0 + 8, LOWER_Y0 + 62, hud->recordLap, 1);
    }
    // Right: lap and time; the map with the ghosts.
    draw_lap_and_time(hud, "TIME TRIAL", 1);
    ghost_marker(&hud->ghost);
    ghost_marker(&hud->staffGhost);
}

/* A boss race or Taj's challenge: the two of you, big, the rival on top while ahead. */
static void draw_head_to_head(const RaceHud *hud, const char *subtitle) {
    int i;
    for (i = 0; i < hud->racerCount && i < 2; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        int y = i == 0 ? 10 : 112;
        char number[4];
        if (r->player) {
            box(TOWER_X0 + 2, y - 4, TOWER_X1 - 2, y + 78, (Colour){ 255, 200, 40, 70 }, GOLD);
        }
        snprintf(number, sizeof(number), "%d", i + 1);
        text(FONT_FUN, TOWER_X0 + 6, y + 2, number, WHITE, 1);
        framed_portrait(r, 27, y + 3, 1, 0);
        if (r->player) {
            text(FONT_SMALL, 26, y + 52, "You", sStyle == 1 ? (Colour){ 70, 36, 0, 255 } : YELLOW, 0);
        } else {
            char gap[16];
            text(FONT_SMALL, 26, y + 52, subtitle, DIM, 1);
            format_gap(r->gap, gap, sizeof(gap));
            draw_gap(26, y + 52 + line_pitch(), 0, gap, r->gap > 0 ? AHEAD : BEHIND);
        }
    }
    text(FONT_FUN, 36, 92, "VS", WHITE, 1);
}

static void draw_versus(const RaceHud *hud) {
    char subtitle[24];
    if (hud->mode == RACE_HUD_MODE_TAJ) {
        static const char *const sNames[3] = { "CAR RACE", "HOVER RACE", "PLANE RACE" };
        static const char *const sShort[3] = { "Car race", "Hover race", "Plane race" };
        int v = hud->vehicle >= 0 && hud->vehicle < 3 ? hud->vehicle : 0;
        draw_head_to_head(hud, sShort[v]);
        if (text_width(FONT_FUN, sNames[v]) <= MIDDLE_X1 - MIDDLE_X0 - 6) {
            text_centred(FONT_FUN, MIDDLE_X0, MIDDLE_X1, 22, sNames[v], WHITE, 1);
        } else {
            static const char *const sTiny[3] = { "CAR", "HOVER", "PLANE" };
            const char *title = sNames[v];
            sNoGap = 1;
            if (text_width(FONT_FUN, title) > MIDDLE_X1 - MIDDLE_X0 - 6) {
                title = sTiny[v];
            }
            if (sStyle == 3 || text_width(FONT_FUN, title) > MIDDLE_X1 - MIDDLE_X0 - 6) {
                title = sNames[v];
                sNoGap = 0;
                text_fit_centred(FONT_SMALL, MIDDLE_X0 + 2, MIDDLE_X1 - 2, 23, title, WHITE, 1);
            } else {
                text_centred(FONT_FUN, MIDDLE_X0, MIDDLE_X1, 22, title, WHITE, 1);
            }
            sNoGap = 0;
        }
        text_centred(FONT_SMALL, MIDDLE_X0, MIDDLE_X1, 52, "Taj's challenge", YELLOW, 1);
        wrapped_centred(MIDDLE_X0, MIDDLE_X1, 52 + line_pitch() + 1, "Win: a new balloon", DIM);
    } else {
        snprintf(subtitle, sizeof(subtitle), port_text("Round %d"), hud->round > 0 ? hud->round : 1);
        draw_head_to_head(hud, subtitle);
        draw_item(hud);
    }
    draw_lap_list(hud, MIDDLE_X0, LOWER_Y0);
    draw_standing(hud);
}

/* A silver coin race: the tower and the race as usual, the coins under the bananas instead of the lap times. */
static void draw_silver_coins(const RaceHud *hud) {
    int x, i;
    char count[8];
    draw_tower(hud);
    draw_item(hud);
    draw_standing(hud);
    picture_scaled(&sSmallBanana, MIDDLE_X0 + 6, LOWER_Y0 + 5, 1, 1);
    x = piece(&sTimes, MIDDLE_X0 + 26, LOWER_Y0 + 14, 2);
    x = piece(&sDigits[(hud->bananas / 10) % 10], x, LOWER_Y0 + 11, 1);
    piece(&sDigits[hud->bananas % 10], x, LOWER_Y0 + 11, 0);
    // The count beside the heading, in one font; under it the eight coins, those collected lit.
    snprintf(count, sizeof(count), "%d/8", hud->silverCoins);
    x = MIDDLE_X1 - 6 - text_width_as_is(FONT_SMALL, count);
    // "Coins": "Silver coins" left no room for the count in the wider looks' fonts (the pictures say silver).
    text_fit(FONT_SMALL, MIDDLE_X0 + 8, x - 4, LOWER_Y0 + 38, "Coins", HINT, 1);
    text_clipped_as_is(FONT_SMALL, x, LOWER_Y0 + 38, count, hud->silverCoins >= 8 ? BEHIND : YELLOW, 1, BS_WIDTH);
    for (i = 0; i < 8; i++) {
        int cx = MIDDLE_X0 + (MIDDLE_X1 - MIDDLE_X0 - 4 * 21 + 4) / 2 + (i % 4) * 21, cy = LOWER_Y0 + 56 + (i / 4) * 21;
        if (i < hud->silverCoins) {
            picture(&sCoin, cx, cy);
        } else {
            picture_dim(&sCoin, cx, cy);
        }
    }
}

/* The four racers of a challenge, with full-size portraits and their score. */
static void draw_four_rows(const RaceHud *hud) {
    int i, k;
    for (i = 0; i < hud->racerCount && i < 4; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        int y = 8 + i * 47, x = 54;
        char score[12], goal[12];
        if (r->player) {
            box(TOWER_X0 + 2, y - 3, TOWER_X1 - 2, y + 44, (Colour){ 255, 200, 40, 70 }, GOLD);
        }
        framed_portrait(r, 10, y, 1, r->out);
        if (r->out) {
            picture_scaled(&sCross, 22, y + 12, 1, 1);
        }
        switch (hud->mode) {
            case RACE_HUD_MODE_BANANAS:
                snprintf(score, sizeof(score), "%d", r->score);
                x = text(FONT_FUN, x, y, score, YELLOW, 1);
                text(FONT_SMALL, x + 3, y + 3, "/10", DIM, 1);
                for (k = 0; k < 10; k++) {
                    pip(54 + (k % 5) * 7, y + 20 + (k / 5) * 10, 4, 6, k < r->score);
                }
                break;
            case RACE_HUD_MODE_BATTLE:
                for (k = 0; k < 10; k++) {
                    int bx = 54 + (k % 5) * 7, by = y + 4 + (k / 5) * 15;
                    if (k < r->score) {
                        picture_scaled(&sTinyBanana, bx, by, 1, 0);
                    } else {
                        picture_dim(&sTinyBanana, bx, by);
                    }
                }
                if (r->out) {
                    text(FONT_SMALL, 54, y + 34, "Out", AHEAD, 1);
                }
                break;
            default:
                snprintf(score, sizeof(score), "%d", r->score);
                snprintf(goal, sizeof(goal), sStyle == 0 ? " / %d" : "/%d", hud->goal);
                if (sStyle == 0) {
                    /* The game's big font has no slash: the whole score in the small font, one font, one baseline. */
                    x = text(FONT_SMALL, x, y + 3, score, YELLOW, 1);
                    text(FONT_SMALL, x, y + 3, goal, DIM, 1);
                } else {
                    /* The goal in the score's own font, dimmer, on its baseline. */
                    x = text(FONT_FUN, x, y, score, YELLOW, 1);
                    text(FONT_FUN, x + 2, y, goal, DIM, 1);
                }
                for (k = 0; k < 3; k++) {
                    if (k < r->score) {
                        picture_scaled(&sEggIcon, 54 + k * 12, y + 20, 1, 1);
                    } else {
                        picture_dim(&sEggIcon, 54 + k * 12, y + 20);
                    }
                }
                break;
        }
    }
}

static void draw_challenge(const RaceHud *hud) {
    int x;
    char line[24];
    const RaceHudRacer *me = NULL;
    int i;
    for (i = 0; i < hud->racerCount; i++) {
        if (hud->racers[i].player) {
            me = &hud->racers[i];
        }
    }
    draw_four_rows(hud);
    draw_item(hud);
    if (hud->mode == RACE_HUD_MODE_EGGS) {
        text_centred(FONT_SMALL, MIDDLE_X0, MIDDLE_X1, LOWER_Y0 + 5, "Carrying", HINT, 1);
        /* The egg centred under the label, its instruction in two wrapped lines beneath it, the hint at the foot. */
        if (me != NULL && me->carrying) {
            picture_scaled(&sEggIcon, (MIDDLE_X0 + MIDDLE_X1 - sEggIcon.width * 2) / 2, LOWER_Y0 + 20, 2, 1);
            wrapped_centred(MIDDLE_X0, MIDDLE_X1,
                            wrapped_centred(MIDDLE_X0, MIDDLE_X1, LOWER_Y0 + 24 + sEggIcon.height * 2, "Get it to", YELLOW),
                            "your nest", YELLOW);
        } else {
            text_centred(FONT_SMALL, MIDDLE_X0, MIDDLE_X1, LOWER_Y0 + 40, "No egg", DIM, 1);
        }
        text_centred(FONT_SMALL, MIDDLE_X0, MIDDLE_X1, LOWER_Y1 - line_pitch() - 3, "Drop it if hit", DIM, 1);
    } else {
        text_centred(FONT_SMALL, MIDDLE_X0, MIDDLE_X1, LOWER_Y0 + 5,
                     hud->mode == RACE_HUD_MODE_BATTLE ? "Your health" : "Carrying", HINT, 1);
        picture_scaled(&sSmallBanana, MIDDLE_X0 + 8, LOWER_Y0 + 20, 1, 1);
        x = piece(&sTimes, MIDDLE_X0 + 28, LOWER_Y0 + 30, 2);
        x = piece(&sDigits[(hud->bananas / 10) % 10], x, LOWER_Y0 + 26, 1);
        piece(&sDigits[hud->bananas % 10], x, LOWER_Y0 + 26, 0);
        if (hud->mode == RACE_HUD_MODE_BATTLE) {
            if (sStyle == 0) {
                wrapped_centred(MIDDLE_X0, MIDDLE_X1,
                                wrapped_centred(MIDDLE_X0, MIDDLE_X1, LOWER_Y0 + 54, "Hits take bananas", DIM) + 2,
                                "Last one left wins", DIM);
            } else {
                wrapped_centred(MIDDLE_X0, MIDDLE_X1, LOWER_Y0 + 56, "Last one left wins", DIM); /* room for two lines */
            }
        } else {
            wrapped_centred(MIDDLE_X0, MIDDLE_X1, wrapped_centred(MIDDLE_X0, MIDDLE_X1, LOWER_Y0 + 54, "Bring them home", DIM) + 2,
                            "to your base", DIM);
        }
    }
    if (hud->mode == RACE_HUD_MODE_BATTLE) {
        int w;
        text_centred(FONT_FUN, RIGHT_X0, RIGHT_X1, 9, "BATTLE", WHITE, 1);
        text_centred(FONT_SMALL, RIGHT_X0, RIGHT_X1, 32, "Racers left", HINT, 1);
        w = DRY(({ int e_ = piece(&sDigits[hud->racersLeft % 10], 0, 46, 1); e_ = piece(&sSlash, e_, 47, 1);
                   piece(&sDigits[hud->racerCount % 10], e_, 46, 0); }));
        x = piece(&sDigits[hud->racersLeft % 10], right_centred(w), 46, 1);
        x = piece(&sSlash, x, 47, 1);
        piece(&sDigits[hud->racerCount % 10], x, 46, 0);
        time_centred(hud, 70);
    } else {
        int place = 1;
        for (i = 0; i < hud->racerCount; i++) {
            if (hud->racers[i].player) {
                place = i + 1;
            }
        }
        {
            const char *title = hud->mode == RACE_HUD_MODE_EGGS ? "EGG COLLECTOR" : "BANANAS";
            if (text_width(FONT_FUN, title) > RIGHT_X1 - RIGHT_X0 - 8) {
                text_fit_centred(FONT_SMALL, RIGHT_X0 + 2, RIGHT_X1 - 2, 11, title, WHITE, 1);
            } else {
                text_centred(FONT_FUN, RIGHT_X0, RIGHT_X1, 9, title, WHITE, 1);
            }
        }
        snprintf(line, sizeof(line), port_text("First to %d %s"), hud->goal,
                 port_text(hud->mode == RACE_HUD_MODE_EGGS ? "eggs wins" : "wins"));
        text_centred_as_is(FONT_SMALL, RIGHT_X0, RIGHT_X1, 32, line, HINT, 1);
        if (place >= 1 && place <= 8) {
            place_row(place, right_centred(DRY(place_row(place, 0, 48))), 48);
        }
    }
}

/* The world names, as the hub's title and the progress list. */
static const char *const sWorldNames[RACE_HUD_WORLDS + 1] = {
    "TIMBER'S ISLAND", "DINO DOMAIN", "SHERBET ISLAND", "SNOWFLAKE MOUNTAIN", "DRAGON FOREST", "FUTURE FUN LAND"
};
static const char *const sWorldShortNames[RACE_HUD_WORLDS] = { "Dino Domain", "Sherbet Island", "Snowflake Mtn",
                                                               "Dragon Forest", "Future Fun" };

static void draw_hub(const RaceHud *hud) {
    int i, k, x, y = 46;
    char count[8];
    // Left: balloons, then each world's balloons, trophy and key.
    picture_scaled(&sBalloon, TOWER_X0 + 6, 8, 1, 1);
    snprintf(count, sizeof(count), "%d", hud->balloons);
    x = TOWER_X0 + 40;
    for (i = 0; count[i] != '\0'; i++) {
        x = piece(&sDigits[count[i] - '0'], x, 18, 1);
    }
    for (i = 0; i < RACE_HUD_WORLDS; i++) {
        int balloons = hud->worldBalloons[i];
        // The whole name in every look: a wider font gets its small size rather than losing a word.
        text_fit(FONT_SMALL, TOWER_X0 + 5, TOWER_X1 - 3, y, sWorldShortNames[i], balloons > 0 ? WHITE : DIM, 1);
        // Eight balloons a world (four races, four silver coin races): two rows of four.
        for (k = 0; k < 8; k++) {
            pip(TOWER_X0 + 6 + (k % 4) * 8, y + 13 + (k / 4) * 7, 4, 4, k < balloons);
        }
        if (hud->worldTrophy[i] > 0) {
            picture(&sTrophy[1], TOWER_X0 + 44, y + 11);
        } else {
            picture_dim(&sTrophy[0], TOWER_X0 + 44, y + 11);
        }
        if (hud->worldKeys & (1 << i)) {
            picture(&sKey[1], TOWER_X0 + 62, y + 11);
        } else {
            picture_dim(&sKey[0], TOWER_X0 + 62, y + 11);
        }
        y += 30;
    }
    // The map, with the doors, Taj and you.
    if (hud->world >= 0 && hud->world <= RACE_HUD_WORLDS) {
        text(FONT_FUN, MIDDLE_X0 + 6, 9, sWorldNames[hud->world], WHITE, 1);
    }
    for (i = 0; i < hud->doorCount && i < RACE_HUD_DOORS; i++) {
        if (hud->doors[i].onMap && sMap.rgba != NULL) {
            int dx, dy;
            map_point(&hud->doors[i], &dx, &dy);
            fill(dx - 3, dy - 3, dx + 4, dy + 4, DARK);
            fill(dx - 2, dy - 2, dx + 3, dy + 3, GOLD);
        }
    }
    draw_extra_point(&hud->taj, (Colour){ 60, 90, 220, 255 });
    if (hud->doorCount > 0) {
        fill(MIDDLE_X0 + 6, HUB_MAP_Y1 - 12, MIDDLE_X0 + 10, HUB_MAP_Y1 - 8, GOLD);
        text(FONT_SMALL, MIDDLE_X0 + 13, HUB_MAP_Y1 - 15, "Doors", DIM, 1);
    }
    if (hud->taj.onMap) {
        dot(MIDDLE_X0 + 62, HUB_MAP_Y1 - 10, (Colour){ 60, 90, 220, 255 });
        text(FONT_SMALL, MIDDLE_X0 + 69, HUB_MAP_Y1 - 15, "Taj", DIM, 1);
    }
    // The amulets: the menu's own art, by pieces found.
    text(FONT_SMALL, MIDDLE_X0 + 6, HUB_AMULET_Y0 + 12, "Amulets", HINT, 1);
    if (hud->ttAmulet >= 0 && hud->ttAmulet <= 4) {
        picture(&sTTAmulet[hud->ttAmulet], MIDDLE_X0 + 64, HUB_AMULET_Y0 + 3);
    }
    snprintf(count, sizeof(count), "%d/4", hud->ttAmulet);
    text(FONT_SMALL, MIDDLE_X0 + 92, HUB_AMULET_Y0 + 12, count, YELLOW, 1);
    if (hud->wizpigAmulet >= 0 && hud->wizpigAmulet <= 4) {
        picture(&sWizpigAmulet[hud->wizpigAmulet], MIDDLE_X0 + 132, HUB_AMULET_Y0 + 3);
    }
    snprintf(count, sizeof(count), "%d/4", hud->wizpigAmulet);
    text(FONT_SMALL, MIDDLE_X0 + 160, HUB_AMULET_Y0 + 12, count, YELLOW, 1);
}

/* The file select menu: the highlighted adventure's progress at a glance while picking a game. */
/* A picture scaled so its visible pixels (not its frame) fit size by size, centred on cx, cy. */
static void picture_fit_visible(const Picture *p, int cx, int cy, int size) {
    int x0 = p->width, y0 = p->height, x1 = -1, y1 = -1, i, j, w, h, dw, dh;
    if (p->rgba == NULL) {
        return;
    }
    for (j = 0; j < p->height; j++) {
        for (i = 0; i < p->width; i++) {
            if (p->rgba[(j * p->width + i) * 4 + 3] > 40) {
                x0 = i < x0 ? i : x0;
                x1 = i > x1 ? i : x1;
                y0 = j < y0 ? j : y0;
                y1 = j > y1 ? j : y1;
            }
        }
    }
    if (x1 < x0 || y1 < y0) {
        return;
    }
    w = x1 - x0 + 1;
    h = y1 - y0 + 1;
    if (w >= h) {
        dw = size;
        dh = (h * size + w / 2) / w;
    } else {
        dh = size;
        dw = (w * size + h / 2) / h;
    }
    for (j = 0; j < dh; j++) {
        for (i = 0; i < dw; i++) {
            const unsigned char *t = p->rgba + (((y0 + j * h / dh) * p->width) + x0 + i * w / dw) * 4;
            if (t[3] != 0) {
                plot(cx - dw / 2 + i, cy - dh / 2 + j, t[0], t[1], t[2], t[3]);
            }
        }
    }
}

static void picture_fit(const Picture *p, int x, int y, int w, int h);
static void menu_save_styled(const RaceHud *hud);
static void draw_menu_save(const RaceHud *hud) {
    int i, k, x, y = 98;
    char line[48];
    if (sStyle != 0) {
        menu_save_styled(hud);
        return;
    }
    text(FONT_FUN, 96, 9, hud->adventureTwo ? "ADVENTURE TWO" : "ADVENTURE", WHITE, 1);
    snprintf(line, sizeof(line), port_text("Save file %s"), hud->name);
    text(FONT_SMALL, 96, 26, line, DIM, 1);
    picture_scaled(&sBalloon, 14, SAVE_PANEL_Y0 + 7, 1, 1);
    text(FONT_SMALL, 48, SAVE_PANEL_Y0 + 6, "Balloons", HINT, 1);
    snprintf(line, sizeof(line), "%d", hud->balloons);
    x = 48;
    for (i = 0; line[i] != '\0'; i++) {
        x = piece(&sDigits[line[i] - '0'], x, SAVE_PANEL_Y0 + 18, 1);
    }
    text(FONT_SMALL, x + 4, SAVE_PANEL_Y0 + 24, "/ 47", DIM, 1);
    // The amulets: the menu's own art, by pieces found.
    if (hud->ttAmulet >= 0 && hud->ttAmulet <= 4) {
        picture(&sTTAmulet[hud->ttAmulet], 170, SAVE_PANEL_Y0 + 6);
    }
    text(FONT_SMALL, 198, SAVE_PANEL_Y0 + 6, "T.T. amulet", HINT, 1);
    snprintf(line, sizeof(line), port_text("%d of 4 pieces"), hud->ttAmulet);
    text(FONT_SMALL, 198, SAVE_PANEL_Y0 + 17, line, YELLOW, 1);
    if (hud->wizpigAmulet > 0 && hud->wizpigAmulet <= 4) {
        picture(&sWizpigAmulet[hud->wizpigAmulet], 170, SAVE_PANEL_Y0 + 30);
        text(FONT_SMALL, 198, SAVE_PANEL_Y0 + 30, "Wizpig amulet", HINT, 1);
        snprintf(line, sizeof(line), port_text("%d of 4 pieces"), hud->wizpigAmulet);
        text(FONT_SMALL, 198, SAVE_PANEL_Y0 + 41, line, YELLOW, 1);
    }
    // Each world: its eight balloons (four races, four silver coin races), trophy, key and boss.
    for (i = 0; i < RACE_HUD_WORLDS; i++) {
        int balloons = hud->worldBalloons[i];
        text(FONT_SMALL, 14, y + 2, sWorldShortNames[i], balloons > 0 ? WHITE : DIM, 1);
        for (k = 0; k < 8; k++) {
            pip(120 + k * 10, y + 4, 6, 6, k < balloons);
        }
        if (hud->worldTrophy[i] > 0) {
            picture(&sTrophy[1], 210, y);
        } else {
            picture_dim(&sTrophy[0], 210, y);
        }
        if (hud->worldKeys & (1 << i)) {
            picture(&sKey[1], 230, y);
        } else {
            picture_dim(&sKey[0], 230, y);
        }
        if (hud->worldBosses & (1 << i)) {
            text_fit(FONT_SMALL, 252, BS_WIDTH - 8, y + 2, "Cleared", BEHIND, 1);
        }
        y += 19;
    }
}

/* A ring from the top, clockwise: frac of it lit (the Dock look's progress rings). */
static void progress_ring(int cx, int cy, int r, int w, int frac256, Colour lit) {
    int x, y;
    for (y = -r - 1; y <= r + 1; y++) {
        for (x = -r - 1; x <= r + 1; x++) {
            float d = sqrtf((float) (x * x + y * y)), a;
            int cover;
            if (d > r + 0.5f || d < r - w - 0.5f) {
                continue;
            }
            cover = d > r ? (int) ((r + 0.5f - d) * 255 * 2) : (d < r - w ? (int) ((d - (r - w - 0.5f)) * 255 * 2) : 255);
            cover = cover > 255 ? 255 : (cover < 0 ? 0 : cover);
            a = atan2f((float) x, (float) -y); /* 0 at the top, growing clockwise. */
            if (a < 0) {
                a += 6.2831853f;
            }
            if ((int) (a * 256 / 6.2831853f) < frac256) {
                plot_c(cx + x, cy + y, lit, cover);
            } else {
                plot_c(cx + x, cy + y, (Colour){ 0, 0, 0, 90 }, cover);
            }
        }
    }
}

static void trophy_and_key(const RaceHud *hud, int world, int x, int y, int gap) {
    if (hud->worldTrophy[world] > 0) {
        picture(&sTrophy[1], x, y);
    } else {
        picture_dim(&sTrophy[0], x, y);
    }
    if (hud->worldKeys & (1 << world)) {
        picture(&sKey[1], x + gap, y);
    } else {
        picture_dim(&sKey[0], x + gap, y);
    }
}

/* "23 / 47" in one size and one colour, the total fainter: the progress numbers of the file select companion. */
static int fraction_at(int x, int y, int value, int total) {
    char text_[16];
    Colour gold = sStyle == 2 ? WHITE : GOLD, faint = gold;
    const UiFont *u = ui_font_kind(UI_HEAD);
    faint.a = 150;
    snprintf(text_, sizeof(text_), "%d", value);
    x = ui_draw(u, x, y, text_, gold, 1, BS_WIDTH);
    snprintf(text_, sizeof(text_), " / %d", total);
    return ui_draw(u, x, y, text_, faint, 1, BS_WIDTH);
}

static int fraction_width(int value, int total) {
    return DRY(fraction_at(0, 0, value, total));
}

static void fraction_centred(int x0, int x1, int y, int value, int total) {
    fraction_at(x0 + (x1 - x0 - fraction_width(value, total)) / 2, y, value, total);
}

static int number_pieces(int x, int y, int value) {
    char count[8];
    int i;
    snprintf(count, sizeof(count), "%d", value);
    for (i = 0; count[i] != '\0'; i++) {
        x = piece(&sDigits[count[i] - '0'], x, y, 1);
    }
    return x;
}

/* The file select companion in the Glass, Broadcast and Dock layouts. */
static void menu_save_styled(const RaceHud *hud) {
    int i;
    char line[48];
    Colour slabFill = { 26, 38, 96, 235 }, slabLight = { 36, 54, 130, 235 }, red = { 235, 40, 40, 255 },
           orange = { 255, 150, 20, 255 }, chip = { 0, 0, 0, 60 };
    if (sStyle == 2) {
        slab(94, 8, 94 + 8 + text_width(FONT_FUN, hud->adventureTwo ? "ADVENTURE TWO" : "ADVENTURE") + 7, 28, 5, red, orange);
        text(FONT_FUN, 102, 13, hud->adventureTwo ? "ADVENTURE TWO" : "ADVENTURE", WHITE, 1);
    } else {
        text(FONT_FUN, 96, 9, hud->adventureTwo ? "ADVENTURE TWO" : "ADVENTURE", sVariant == 4 ? (Colour){ 11, 32, 80, 255 } : sStyle == 1 || sStyle == 3 ? GOLD : WHITE, sVariant == 4 ? 0 : 1);
    }
    snprintf(line, sizeof(line), port_text("Save file %s"), hud->name);
    text(FONT_SMALL, sStyle == 2 ? 100 : 98, sStyle == 2 ? 32 : 28, line, sVariant == 4 ? (Colour){ 11, 32, 80, 255 } : WHITE, sVariant == 4 ? 0 : 1);
    if (sStyle == 3) {
        // Rings: the balloons, then the two amulets.
        progress_ring(56, 100, 36, 6, hud->balloons * 256 / 47, GOLD);
        picture_scaled(&sBalloon, 42, 72, 1, 1);
        fraction_centred(16, 96, 107, hud->balloons, 47);
        // Each amulet's ring with its name under the count, as the other looks name them.
        progress_ring(30, 154, 13, 4, hud->ttAmulet * 256 / 4, (Colour){ 180, 230, 255, 255 });
        if (hud->ttAmulet >= 0 && hud->ttAmulet <= 4) {
            picture_fit_visible(&sTTAmulet[hud->ttAmulet], 30, 154, 16);
        }
        fraction_centred(6, 54, 170, hud->ttAmulet, 4);
        text_fit_centred(FONT_SMALL, 6, 54, 182, "T.T.", LABEL_BLUE, 1);
        progress_ring(82, 154, 13, 4, hud->wizpigAmulet * 256 / 4, (Colour){ 180, 230, 255, 255 });
        if (hud->wizpigAmulet >= 0 && hud->wizpigAmulet <= 4) {
            picture_fit_visible(&sWizpigAmulet[hud->wizpigAmulet], 82, 154, 16);
        }
        fraction_centred(58, 106, 170, hud->wizpigAmulet, 4);
        text_fit_centred(FONT_SMALL, 58, 106, 182, "Wizpig", LABEL_BLUE, 1);
        for (i = 0; i < RACE_HUD_WORLDS; i++) {
            int y = 56 + i * 28, balloons = hud->worldBalloons[i];
            Colour ink = balloons > 0 ? WHITE : DIM;
            text(FONT_SMALL, 118, y, sWorldShortNames[i], ink, 1);
            // A world whose boss is beaten shows its count in the "Cleared" green of the other looks.
            snprintf(line, sizeof(line), "%d/8", balloons);
            text(FONT_SMALL, 300 - text_width(FONT_SMALL, line), y, line, hud->worldBosses & (1 << i) ? BEHIND : ink, 1);
            smooth_box(118, y + 15, 304, y + 19, 2, (Colour){ 0, 0, 0, 80 }, (Colour){ 0, 0, 0, 80 }, (Colour){ 0, 0, 0, 0 }, 1);
            if (balloons > 0) {
                smooth_box(118, y + 15, 118 + 186 * balloons / 8, y + 19, 2, GOLD, GOLD, (Colour){ 0, 0, 0, 0 }, 1);
            }
            trophy_and_key(hud, i, 232, y - 3, 16);
        }
        return;
    }
    // The three stat chips (Glass: rounded; Broadcast: slanted slabs).
    for (i = 0; i < 3; i++) {
        int cx = sStyle == 2 ? (i == 0 ? 8 : (i == 1 ? 128 : 222)) : 16 + i * 98;
        int cw = sStyle == 2 ? (i == 0 ? 114 : (i == 1 ? 88 : 90)) : 90;
        if (sVariant == 1) { /* Carbon: 4 to 316 like the buttons and race panels */
            cx = i == 0 ? 4 : (i == 1 ? 126 : 218);
            cw = i == 0 ? 118 : (i == 1 ? 88 : 98);
        }
        int value = i == 0 ? hud->balloons : (i == 1 ? hud->ttAmulet : hud->wizpigAmulet);
        const char *name = i == 0 ? "Balloons" : (i == 1 ? "T.T." : "Wizpig");
        if (sStyle == 2) {
            slab(cx, 48, cx + cw, 86, 5, slabLight, slabLight);
        } else {
            smooth_box(cx, 50, cx + cw, 86, 8, chip, chip, (Colour){ 255, 255, 255, 40 }, 1);
        }
        {
            // Icon and the two text lines as one group, centred in the chip; every icon in the same 28 px square.
            const Picture *icon = i == 0 ? &sBalloon : (i == 1 ? &sTTAmulet[hud->ttAmulet < 0 || hud->ttAmulet > 4 ? 0 : hud->ttAmulet]
                                                                  : &sWizpigAmulet[hud->wizpigAmulet < 0 || hud->wizpigAmulet > 4 ? 0 : hud->wizpigAmulet]);
            int numberW = fraction_width(value, i == 0 ? 47 : 4), nameW = text_width(FONT_SMALL, name);
            int textW = numberW > nameW ? numberW : nameW, groupW = 28 + 8 + textW, gx = cx + (cw - groupW) / 2;
            picture_fit_visible(icon, gx + 14, 68, 28);
            // The number and its label share one centre line, the pair centred top to bottom in the chip.
            fraction_at(gx + 36 + (textW - numberW) / 2, 55, value, i == 0 ? 47 : 4);
            text(FONT_SMALL, gx + 36 + (textW - nameW) / 2, 70, name, LABEL_BLUE, 1);
        }
    }
    for (i = 0; i < RACE_HUD_WORLDS; i++) {
        int y = sStyle == 2 ? 92 + i * 21 : 96 + i * 20, balloons = hud->worldBalloons[i], k;
        Colour ink = balloons > 0 ? WHITE : sVariant == 1 ? (Colour){ 138, 144, 166, 255 } : DIM;
        if (sStyle == 2) {
            slab(sVariant == 1 ? 4 : 8, y, sVariant == 1 ? 316 : 312, y + 19, 4, slabFill, slabFill);
            for (k = 0; k < 8; k++) {
                slab(132 + k * 13, y + 6, 132 + k * 13 + 10, y + 13, 3, k < balloons ? red : (Colour){ 255, 255, 255, 30 },
                     k < balloons ? orange : (Colour){ 255, 255, 255, 30 });
            }
            text(FONT_SMALL, 18, y + 5, sWorldShortNames[i], ink, 1);
        } else {
            text(FONT_SMALL, 18, y + 2, sWorldShortNames[i], ink, 1);
            smooth_box(112, y + 5, 224, y + 10, 2, (Colour){ 0, 0, 0, 90 }, (Colour){ 0, 0, 0, 90 }, (Colour){ 0, 0, 0, 0 }, 1);
            if (balloons > 0) {
                smooth_box(112, y + 5, 112 + 112 * balloons / 8, y + 10, 2, (Colour){ 255, 236, 110, 255 },
                           (Colour){ 240, 160, 20, 255 }, (Colour){ 0, 0, 0, 0 }, 1);
            }
            snprintf(line, sizeof(line), "%d/8", balloons);
            text(FONT_SMALL, 230, y + 2, line, hud->worldBosses & (1 << i) ? BEHIND : ink, 1);
        }
        trophy_and_key(hud, i, sStyle == 2 ? 236 : 262, y + (sStyle == 2 ? 1 : -1), 16);
        if (sStyle == 2) {
            if (hud->worldBosses & (1 << i)) {
                text_fit(FONT_SMALL, 272, 311, y + 5, "Cleared", BEHIND, 1);
            } else {
                snprintf(line, sizeof(line), "%d/8", balloons);
                text(FONT_SMALL, 276, y + 5, line, ink, 1);
            }
        }
    }
}

/* The tracks menu: the highlighted track's map and its records, which the N64 kept on another screen. */
static void draw_menu_track(const RaceHud *hud) {
    static const char *const sVehicles[3] = { "Car", "Hovercraft", "Plane" };
    char title[32];
    int i, x0 = MENU_RECORDS_X0 + 8;
    for (i = 0; hud->name[i] != '\0' && i < (int) sizeof(title) - 1; i++) {
        title[i] = (hud->name[i] >= 'a' && hud->name[i] <= 'z') ? hud->name[i] - 32 : hud->name[i];
    }
    title[i] = '\0';
    text(FONT_FUN, MENU_MAP_X0 + 8, MENU_PANEL_Y0 + 5, title, WHITE, 1);
    text(FONT_SMALL, x0, MENU_PANEL_Y0 + 6, "Best race", HINT, 1);
    if (hud->record >= 0) {
        race_time(x0, MENU_PANEL_Y0 + 18, hud->record, 1);
    } else {
        text(FONT_SMALL, x0, MENU_PANEL_Y0 + 18, "--", DIM, 1);
    }
    text(FONT_SMALL, x0, MENU_PANEL_Y0 + 38, "Best lap", HINT, 1);
    if (hud->recordLap >= 0) {
        race_time(x0, MENU_PANEL_Y0 + 50, hud->recordLap, 1);
    } else {
        text(FONT_SMALL, x0, MENU_PANEL_Y0 + 50, "--", DIM, 1);
    }
    if (hud->vehicle >= 0 && hud->vehicle < 3) {
        // The records above are this vehicle's: "Vehicle Car" on one line, clear of the lap time.
        int vx = text(FONT_SMALL, x0, MENU_PANEL_Y0 + 72, "Vehicle", HINT, 1) + 5;
        text_fit(FONT_SMALL, vx, MENU_RECORDS_X1 - 4, MENU_PANEL_Y0 + 72, sVehicles[hud->vehicle], WHITE, 1);
    }
    fill(MENU_RECORDS_X0 + 6, MENU_PANEL_Y0 + 90, MENU_RECORDS_X1 - 6, MENU_PANEL_Y0 + 91, SLOT_EDGE);
    // This 3DS's own best time trial (port_race_hud.c keeps its laps for the time trial screen).
    picture_scaled(&sGhostIcon, x0, MENU_PANEL_Y0 + 98, 1, 1);
    text(FONT_SMALL, x0, MENU_PANEL_Y0 + 140, "Best run", HINT, 1);
    if (hud->bestTotal > 0) {
        race_time(x0, MENU_PANEL_Y0 + 152, hud->bestTotal, 1);
    } else {
        text(FONT_SMALL, x0, MENU_PANEL_Y0 + 152, "none yet", DIM, 1);
    }
}

/******** The looks' own race layouts (the mockups in 3ds/bottom-ui/modern-mockups): Glass, Broadcast and Dock. ********/

static const char *const sCharacterCodes[10] = { "KRU", "BUM", "TIP", "CON", "TIM", "BAN", "DRU", "PIP", "TT", "DID" };
static const char *const sCharacterNames[10] = { "Krunch", "Bumper", "Tiptup", "Conker", "Timber",
                                                 "Banjo",  "Drumstick", "Pipsy", "T.T.", "Diddy" };
static const char *const sSpecialCodes[RACE_HUD_SPECIALS] = { "TRI", "BLU", "BUB", "SMO", "WIZ", "TAJ" };
static const char *const sSpecialTitles[RACE_HUD_SPECIALS] = { "Tricky", "Bluey", "Bubbler", "Smokey", "Wizpig", "Taj" };

static const char *racer_code(const RaceHudRacer *r) {
    if (r->special > RACE_HUD_NOT_SPECIAL && r->special < RACE_HUD_SPECIALS) {
        return sSpecialCodes[r->special];
    }
    return r->character >= 0 && r->character < 10 ? sCharacterCodes[r->character] : "";
}

static const char *racer_name(const RaceHudRacer *r) {
    if (r->special > RACE_HUD_NOT_SPECIAL && r->special < RACE_HUD_SPECIALS) {
        return sSpecialTitles[r->special];
    }
    return r->character >= 0 && r->character < 10 ? sCharacterNames[r->character] : "";
}

/* The 20 pixel portrait with no frame (the mockups' rounded ring is drawn by the caller). */
static void plain_portrait(const RaceHudRacer *r, int x, int y) {
    if (r->special == RACE_HUD_NOT_SPECIAL && r->character >= 0 && r->character < 10) {
        picture(&sPortraits[r->character], x, y);
    } else if (r->special > RACE_HUD_NOT_SPECIAL && r->special < RACE_HUD_SPECIALS &&
               sSpecialPortraits[r->special].rgba != NULL) {
        picture(&sSpecialPortraits[r->special], x, y);
    } else if (r->special > RACE_HUD_NOT_SPECIAL && r->special < RACE_HUD_SPECIALS) {
        char initial[2] = { (char) (sSpecialNames[r->special][0] - 'a' + 'A'), 0 };
        fill(x, y, x + 20, y + 20, (Colour){ 0, 20, 90, 255 });
        letter_in_box(FONT_FUN, x, y, x + 20, y + 20, initial[0], WHITE);
    }
}

static void ring_portrait(const RaceHudRacer *r, int x, int y, int lit) {
    Colour colour = { 255, 255, 255, 200 };
    if (!lit) {
        racer_colour(r, &colour);
    }
    smooth_box(x - 2, y - 2, x + 22, y + 22, 12, colour, colour, (Colour){ 0, 0, 0, 0 }, 1);
    plain_portrait(r, x, y);
}


/* The place, its suffix and "of n", the lap and the time, at the positions each look wants. Returns nothing. */
/* The place and its suffix, together centred in the box x0..x1 by y0..y1 (no "of 8": the place alone says it). */
static void place_centred(const RaceHud *hud, int x0, int y0, int x1, int y1, int suffixDy) {
    int w, h, y;
    if (hud->position < 1 || hud->position > 8) {
        return;
    }
    w = DRY(place_row(hud->position, 0, 0));
    h = sPlaces[hud->position - 1].height;
    y = y0 + (y1 - y0 - h) / 2;
    (void) suffixDy;
    place_row(hud->position, x0 + (x1 - x0 - w) / 2, y);
}

static int place_pieces(const RaceHud *hud, int x, int y, int suffixDy) {
    if (hud->position >= 1 && hud->position <= 8) {
        int suffix = hud->position <= 3 ? hud->position - 1 : 3;
        x = piece(&sPlaces[hud->position - 1], x, y, 1);
        piece(&sSuffixes[suffix], x, y + suffixDy, 0);
    }
    return x; /* Where the suffix starts: "of 8" goes under it. */
}

static void lap_pieces(const RaceHud *hud, int x, int y) {
    if (hud->finished) {
        piece(&sFinishLabel, x, y, 0);
    } else if (hud->laps > 0 && hud->lap >= hud->laps && hud->laps > 1) {
        x = piece(&sFinalLabel, x, y, 7);
        piece(&sLapSmallLabel, x, y, 0);
    } else if (hud->laps > 0) {
        x = piece(&sLapLabel, x, y + 3, 4);
        x = piece(&sDigits[hud->lap % 10], x, y, 1);
        x = piece(&sSlash, x, y + 1, 1);
        piece(&sDigits[hud->laps % 10], x, y, 0);
    }
}

/* The last finished lap's number and time (or the lap so far), and the best finished lap's time; -1 when none. */
/* A little stopwatch with the time of the lap being driven beside it: stands in for a "Lap 1" label (the lap counter says
 * which lap it is). Returns where it ends. */
static int lap_clock(int x, int y, int frames, Colour c, int xMax) {
    static const char *const rows[7] = { "..###..", ".#...#.", "#..#..#", "#..##.#", "#.....#", ".#...#.", "..###.." };
    int i, j, width = 11 + DRY(race_time(0, 0, frames, 1));
    if (x + width > xMax) {
        x = xMax - width; /* keeps its digits inside the panel */
    }
    for (j = 0; j < 7; j++) {
        for (i = 0; i < 7; i++) {
            if (rows[j][i] == '#') {
                fill(x + i + 1, y + j + 2, x + i + 2, y + j + 3, (Colour){ 0, 0, 20, 160 });
                fill(x + i, y + j + 1, x + i + 1, y + j + 2, c);
            }
        }
    }
    fill(x + 2, y, x + 5, y + 1, c);
    return race_time(x + 11, y - 1, frames, 1);
}

static void lap_facts(const RaceHud *hud, int *lapNumber, int *lapTime, int *best) {
    int i, timed, done;
    timed_laps(hud, &timed, &done);
    *best = -1;
    for (i = 0; i < done; i++) {
        if (*best < 0 || hud->lapTimes[i] < *best) {
            *best = hud->lapTimes[i];
        }
    }
    *lapNumber = done > 0 ? done : timed;
    *lapTime = *lapNumber > 0 ? hud->lapTimes[*lapNumber - 1] : -1;
}

static void item_in_slot(const RaceHud *hud, int x, int y, int countX, int countY) {
    int index = hud->item * 3 + hud->itemLevel;
    if (hud->item < 0 || index < 0 || index >= 15 || sItems[index].rgba == NULL) {
        if (sNoItemLabel) {
            text_centred(FONT_SMALL, x - 2, x + 42, y + 14, "No", DIM, 1);
            text_centred(FONT_SMALL, x - 2, x + 42, y + 14 + line_pitch(), "item", DIM, 1);
        }
        return;
    }
    picture_scaled(&sItems[index], x, y, 1, 1);
    if (hud->itemCount > 1 && hud->itemCount <= 10) {
        piece(&sItemCounts[hud->itemCount - 1], countX, countY, 0);
    }
}

/* A picture shrunk or grown to w by h (nearest pixel) at x, y. */
/*
 * A picture resized smoothly (grid layouts): every output pixel averages 4x4 samples, each a Catmull-Rom (bicubic)
 * read of the premultiplied picture, with the sample's alpha pushed towards 0 or 1 so the outline comes out as a clean
 * curve with one pixel of anti-aliasing instead of the nearest pixel's staircase. Made once per picture and size.
 */
static float cr_weight(float t) {
    t = t < 0 ? -t : t;
    return t < 1 ? 1.5f * t * t * t - 2.5f * t * t + 1 : t < 2 ? -0.5f * t * t * t + 2.5f * t * t - 4 * t + 2 : 0;
}

static void smooth_sample(const Picture *p, float sx, float sy, float out[4]) {
    int i0 = (int) floorf(sx), j0 = (int) floorf(sy), i, j, c;
    float acc[4] = { 0, 0, 0, 0 };
    for (j = j0 - 1; j <= j0 + 2; j++) {
        float wy = cr_weight(sy - j);
        int jj = j < 0 || j >= p->height ? -1 : j;
        for (i = i0 - 1; i <= i0 + 2; i++) {
            float wgt = wy * cr_weight(sx - i), a;
            const unsigned char *t;
            if (jj < 0 || i < 0 || i >= p->width) {
                continue; /* clear outside the picture, so its outline closes */
            }
            t = p->rgba + (jj * p->width + i) * 4;
            a = t[3] / 255.0f;
            for (c = 0; c < 3; c++) {
                acc[c] += wgt * a * (t[c] / 255.0f);
            }
            acc[3] += wgt * a;
        }
    }
    for (c = 0; c < 4; c++) {
        out[c] = acc[c] < 0 ? 0 : acc[c] > 1 ? 1 : acc[c];
    }
}

static int smooth_resize(const Picture *p, int w, int h, Picture *out) {
    enum { S = 4 };
    int x, y, u, v, c;
    out->rgba = calloc((size_t) w * h, 4);
    if (out->rgba == NULL) {
        return 0;
    }
    out->width = w;
    out->height = h;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float sum[4] = { 0, 0, 0, 0 };
            unsigned char *o = out->rgba + (y * w + x) * 4;
            for (v = 0; v < S; v++) {
                for (u = 0; u < S; u++) {
                    float smp[4], t, hard, k;
                    float sx = ((x * S + u) + 0.5f) / (w * S) * p->width - 0.5f;
                    float sy = ((y * S + v) + 0.5f) / (h * S) * p->height - 0.5f;
                    smooth_sample(p, sx, sy, smp);
                    /* the outline: alpha 0.3..0.7 becomes 0..1 on a smoothstep */
                    t = (smp[3] - 0.3f) / 0.4f;
                    t = t < 0 ? 0 : t > 1 ? 1 : t;
                    hard = t * t * (3 - 2 * t);
                    k = smp[3] > 0.001f ? hard / smp[3] : 0;
                    for (c = 0; c < 3; c++) {
                        sum[c] += smp[c] * k;
                    }
                    sum[3] += hard;
                }
            }
            for (c = 0; c < 3; c++) {
                float vv = sum[3] > 0.001f ? sum[c] / sum[3] : 0;
                vv = vv < 0 ? 0 : vv > 1 ? 1 : vv; /* the bicubic overshoots: keep the colour in range */
                o[c] = (unsigned char) (vv * 255 + 0.5f);
            }
            o[3] = (unsigned char) (sum[3] / (S * S) * 255 + 0.5f);
        }
    }
    return 1;
}

static void picture_fit(const Picture *p, int x, int y, int w, int h) {
    int i, j;
    if (p->rgba == NULL || w <= 0 || h <= 0) {
        return;
    }
    /* near its own size (0.85x to 1.2x) a picture keeps its pixels: smoothing there only softens it */
    if (sGrid && sSmoothArt && (w * 100 < p->width * 85 || w * 100 > p->width * 120)) {
        enum { CACHE = 24 };
        static struct {
            const unsigned char *src;
            int w, h, srcW, srcH;
            Picture pic;
        } sCache[CACHE];
        static int sNext;
        int k;
        for (k = 0; k < CACHE; k++) {
            if (sCache[k].src == p->rgba && sCache[k].w == w && sCache[k].h == h && sCache[k].srcW == p->width &&
                sCache[k].srcH == p->height && sCache[k].pic.rgba != NULL) {
                break;
            }
        }
        if (k == CACHE) {
            k = sNext;
            sNext = (sNext + 1) % CACHE;
            free(sCache[k].pic.rgba);
            sCache[k].pic.rgba = NULL;
            if (smooth_resize(p, w, h, &sCache[k].pic)) {
                sCache[k].src = p->rgba;
                sCache[k].w = w;
                sCache[k].h = h;
                sCache[k].srcW = p->width;
                sCache[k].srcH = p->height;
            }
        }
        if (sCache[k].pic.rgba != NULL) {
            picture(&sCache[k].pic, x, y);
            return;
        }
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            const unsigned char *t = p->rgba + ((j * p->height / h) * p->width + i * p->width / w) * 4;
            if (t[3] != 0) {
                plot(x + i, y + j, t[0], t[1], t[2], t[3]);
            }
        }
    }
}

static void bananas_count(const RaceHud *hud, int bananaX, int bananaY, int bananaHeight, int timesX, int timesY,
                          int digitsY) {
    int x;
    picture_fit(&sBanana, bananaX, bananaY, sBanana.width * bananaHeight / (sBanana.height > 0 ? sBanana.height : 1),
                bananaHeight);
    x = piece(&sTimes, timesX, timesY, 2);
    x = piece(&sDigits[(hud->bananas / 10) % 10], x, digitsY, 1);
    piece(&sDigits[hud->bananas % 10], x, digitsY, 0);
}

static void lap_segments(const RaceHud *hud, int x, int y, int w, int gap, int h, Colour lit, Colour unlit, int slant) {
    int k, n = hud->laps > 0 ? hud->laps : 0;
    for (k = 0; k < n && k < 5; k++) {
        int x0 = x + k * (w + gap);
        int filled = k < hud->lap - 1 || hud->finished ? w : (k == hud->lap - 1 ? w / 2 : 0);
        if (slant) {
            slab(x0, y, x0 + w, y + h, 3, unlit, unlit);
            if (filled > 0) {
                slab(x0, y, x0 + filled, y + h, 3, lit, lit);
            }
        } else {
            smooth_box(x0, y, x0 + w, y + h, h / 2, unlit, unlit, (Colour){ 0, 0, 0, 0 }, 1);
            if (filled > 0) {
                smooth_box(x0, y, x0 + filled, y + h, h / 2, lit, lit, (Colour){ 0, 0, 0, 0 }, 1);
            }
        }
    }
}

static void gap_text(int x, int y, int rightAligned, const RaceHudRacer *r) {
    char gap[16];
    format_gap(r->gap, gap, sizeof(gap));
    draw_gap(x, y, rightAligned, gap, r->gap > 0 ? AHEAD : BEHIND);
}

static void race_glass(const RaceHud *hud) {
    int i, lapNumber, lapTime, best;
    box(6, 6, 114, 198, BOX_FILL, BOX_EDGE);
    for (i = 0; i < hud->racerCount && i < RACE_HUD_RACERS; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        int y = 10 + i * 47 / 2;
        char number[4];
        snprintf(number, sizeof(number), "%d", i + 1);
        if (r->player) {
            smooth_box(9, y - 1, 111, y + 22, 11, (Colour){ 255, 222, 80, 235 }, (Colour){ 230, 150, 20, 235 },
                       (Colour){ 255, 250, 200, 200 }, 1);
        }
        text_centred(FONT_FUN, 10, 28, y + 4, number, WHITE, 1);
        ring_portrait(r, 32, y + 1, r->player ? 0 : 1);
        if (r->player) {
            text(FONT_SMALL, 62, y + 6, "You", (Colour){ 70, 30, 0, 255 }, 0);
        } else {
            gap_text(62, y + 6, 0, r);
        }
    }
    // The hero card: place, lap with its segments, the time.
    box(120, 6, 314, 98, BOX_FILL, BOX_EDGE);
    place_centred(hud, 124, 10, 196, 50, 2);
    lap_pieces(hud, 200, 14);
    lap_segments(hud, 200, 38, 32, 4, 5, (Colour){ 255, 214, 60, 255 }, (Colour){ 255, 255, 255, 50 }, 0);
    fill(130, 45, 304, 46, (Colour){ 255, 255, 255, 45 });
    lap_facts(hud, &lapNumber, &lapTime, &best);
    {
        // Total, Current and Best: one label font, one digit size, digits in one column.
        static const char *const names[3] = { "Total", "Current", "Best" };
        int values[3], shown[3], k, rowsShown = 0, dx, left, widest = 0;
        values[0] = hud->raceTime;
        values[1] = lapTime;
        values[2] = best;
        shown[0] = 1;
        shown[1] = lapNumber > 0;
        shown[2] = best >= 0;
        for (k = 0; k < 3; k++) {
            int w = text_width(FONT_SMALL, names[k]);
            widest = w > widest ? w : widest;
        }
        left = 120 + (194 - (widest + 10 + DRY(race_time(0, 0, 0, 1)))) / 2; /* the group, centred in the card */
        dx = left + widest + 10;
        for (k = 0; k < 3; k++) {
            if (shown[k]) {
                int y = 52 + rowsShown * 14;
                text(FONT_SMALL, left, y, names[k], LABEL_BLUE, 1);
                race_time(dx, y + digit_dy(), values[k], 1);
                rowsShown++;
            }
        }
    }
    // The item slot and the bananas.
    box(120, 104, 196, 198, BOX_FILL, BOX_EDGE);
    smooth_box(128, 112, 188, 172, 14, (Colour){ 0, 0, 0, 70 }, (Colour){ 0, 0, 0, 70 }, (Colour){ 255, 255, 255, 60 }, 1);
    {
        int index = hud->item * 3 + hud->itemLevel;
        if (hud->item >= 0 && index >= 0 && index < 15 && sItems[index].rgba != NULL) {
            picture_fit(&sItems[index], 136, 120, 44, 44);
            if (hud->itemCount > 1 && hud->itemCount <= 10) {
                piece(&sItemCounts[hud->itemCount - 1], 168, 150, 0);
            }
        } else if (sNoItemLabel) {
            text_centred(FONT_SMALL, 128, 188, 128, "No", DIM, 1);
            text_centred(FONT_SMALL, 128, 188, 128 + line_pitch(), "item", DIM, 1);
        }
    }
    box(202, 104, 314, 198, BOX_FILL, BOX_EDGE);
    bananas_count(hud, 224, 130, 40, 260, 146, 142);
}

static void race_broadcast(const RaceHud *hud) {
    static const Colour podium[3] = { { 255, 200, 40, 255 }, { 200, 210, 230, 255 }, { 220, 140, 70, 255 } };
    int i, lapNumber, lapTime, best;
    Colour slabFill = { 26, 38, 96, 235 }, slabLight = { 36, 54, 130, 235 }, red = { 235, 40, 40, 255 },
           orange = { 255, 150, 20, 255 };
    for (i = 0; i < hud->racerCount && i < RACE_HUD_RACERS; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        int y = 7 + i * 24;
        char number[4];
        Colour c = i < 3 ? podium[i] : (Colour){ 60, 80, 160, 255 }, racer;
        snprintf(number, sizeof(number), "%d", i + 1);
        if (r->player) {
            slab(4, y, 128, y + 21, 5, (Colour){ 255, 240, 220, 245 }, (Colour){ 255, 220, 170, 245 });
        } else {
            slab(4, y, 128, y + 21, 5, slabFill, slabFill);
        }
        slab(4, y, 27, y + 21, 5, c, c);
        text_centred(FONT_FUN, 7, 25, y + 3, number, WHITE, 1);
        plain_portrait(r, 28, y + 1);
        if (racer_colour(r, &racer)) {
            fill(50, y + 3, 52, y + 18, racer);
        }
        if (r->player) {
            text(FONT_SMALL, 56, y + 5, racer_code(r), sVariant == 1 ? WHITE : (Colour){ 20, 20, 50, 255 }, 0);
            text(FONT_SMALL, 120 - text_width(FONT_SMALL, "YOU"), y + 5, "YOU", sVariant == 1 ? GOLD : (Colour){ 200, 40, 30, 255 }, 0);
        } else {
            text(FONT_SMALL, 56, y + 5, racer_code(r), WHITE, 1);
            gap_text(122, y + 5, 1, r);
        }
    }
    // The place block, the lap slab and the time strip.
    slab(132, 8, 229, 64, 10, red, orange);
    place_centred(hud, 138, 8, 223, 64, 3);
    slab(234, 8, 316, 64, 6, slabLight, slabLight);
    if (hud->laps > 1 && hud->lap >= hud->laps && !hud->finished) {
        text_centred(FONT_FUN, 234, 316, 23, "FINAL LAP", WHITE, 1);
    } else {
        lap_pieces(hud, 240, 22);
    }
    lap_segments(hud, 239, 45, 20, 3, 6, red, (Colour){ 255, 255, 255, 40 }, 1);
    lap_facts(hud, &lapNumber, &lapTime, &best);
    // All the times in one slab: Total, Current and Best, one label font, one digit size, one digit column.
    slab(132, 66, 316, 114, 6, slabFill, slabFill);
    {
        static const char *const names[3] = { "TOTAL", "CURRENT", "BEST" };
        int values[3], shown[3], k, row = 0, widest = 0, dx;
        values[0] = hud->raceTime;
        values[1] = lapTime;
        values[2] = best;
        shown[0] = 1;
        shown[1] = lapNumber > 0;
        shown[2] = best >= 0;
        for (k = 0; k < 3; k++) {
            int w = text_width(FONT_SMALL, names[k]);
            widest = w > widest ? w : widest;
        }
        dx = 146 + widest + 12;
        for (k = 0; k < 3; k++) {
            int y = 71 + row * 14;
            text(FONT_SMALL, 146, y, names[k], LABEL_BLUE, 1);
            if (shown[k]) {
                race_time(dx, y + digit_dy(), values[k], 1);
            } else {
                text(FONT_SMALL, dx, y, "--", DIM, 1);
            }
            row++;
        }
    }
    slab(132, 119, 214, 198, 6, slabFill, slabFill);
    text(FONT_SMALL, 146, 124, "ITEM", LABEL_BLUE, 1);
    item_in_slot(hud, 151, 143, 179, 173);
    slab(218, 119, 316, 198, 6, slabFill, slabFill);
    text(FONT_SMALL, 232, 124, "BANANAS", LABEL_BLUE, 1);
    bananas_count(hud, 232, 144, 40, 268, 162, 158);
}

static void race_dock(const RaceHud *hud) {
    int i, me = 0, first, lapNumber, lapTime, best, rowY = 50;
    place_centred(hud, 4, 4, 84, 42, 2);
    if (hud->laps > 1 && hud->lap >= hud->laps && !hud->finished) {
        sNoGap = 1;
        text(FONT_FUN, 84, 8, "FINAL LAP", GOLD, 1);
        sNoGap = 0;
    } else {
        lap_pieces(hud, 86, 6);
    }
    lap_segments(hud, 86, 25, 8, 4, 8, GOLD, (Colour){ 255, 255, 255, 50 }, 0);
    lap_facts(hud, &lapNumber, &lapTime, &best);
    {
        // Total on top, Current below: the same label font and digit size, digits in one column.
        int totalW = text_width(FONT_SMALL, "Total"), currentW = text_width(FONT_SMALL, "Current");
        int dx = 176 + (totalW > currentW ? totalW : currentW) + 6;
        text(FONT_SMALL, 176, 8, "Total", LABEL_BLUE, 1);
        race_time(dx, 8 + digit_dy(), hud->raceTime, 1);
        if (lapNumber > 0) {
            text(FONT_SMALL, 176, 26, "Current", LABEL_BLUE, 1);
            race_time(dx, 26 + digit_dy(), lapTime, 1);
        }
    }
    fill(8, 44, 312, 45, (Colour){ 255, 255, 255, 40 });
    // The player and the racers either side of them, on a calm panel.
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
        int y = rowY + (i - first) * 30;
        char number[4];
        snprintf(number, sizeof(number), "%d", i + 1);
        if (r->player) {
            smooth_box(156, y - 3, 314, y + 25, 14, (Colour){ 255, 255, 255, 50 }, (Colour){ 255, 255, 255, 25 },
                       (Colour){ 255, 214, 40, 255 }, 1);
        }
        text_centred(FONT_FUN, 160, 176, y + 5, number, r->player ? GOLD : WHITE, 1);
        ring_portrait(r, 182, y + 3, r->player ? 0 : 1);
        text(FONT_SMALL, 212, y + 8, r->player ? "You" : racer_name(r), WHITE, 1);
        if (!r->player) {
            gap_text(306, y + 8, 1, r);
        }
    }
    fill(160, 142, 312, 143, (Colour){ 255, 255, 255, 40 });
    item_in_slot(hud, 164, 150, 190, 176);
    bananas_count(hud, 232, 150, 36, 262, 170, 166);
}

/* Whether this race is drawn with a look's own layout rather than the Classic panels. */
static int own_race_layout(const RaceHud *hud) {
    return sStyle != 0 && hud->mode == RACE_HUD_MODE_RACE && hud->silverCoins < 0;
}

/* Scratch idea 1: Dock's own time trial: the map big on the left, the ghost, record and T.T. down the right. */
static int dock_tt(const RaceHud *hud) {
    return sStyle == 3 && hud->mode == RACE_HUD_MODE_TIME_TRIAL;
}

static void time_trial_dock(const RaceHud *hud) {
    int lapNumber, lapTime, best;
    text(FONT_FUN, 8, 6, "TIME TRIAL", GOLD, 1);
    lap_pieces(hud, 8, 26);
    lap_facts(hud, &lapNumber, &lapTime, &best);
    {
        int totalW = text_width(FONT_SMALL, "Total"), currentW = text_width(FONT_SMALL, "Current");
        int dx = 176 + (totalW > currentW ? totalW : currentW) + 6;
        text(FONT_SMALL, 176, 8, "Total", LABEL_BLUE, 1);
        race_time(dx, 8 + digit_dy(), hud->raceTime, 1);
        if (lapNumber > 0) {
            text(FONT_SMALL, 176, 26, "Current", LABEL_BLUE, 1);
            race_time(dx, 26 + digit_dy(), lapTime, 1);
        }
    }
    fill(8, 44, 312, 45, (Colour){ 255, 255, 255, 40 });
    picture_scaled(&sGhostIcon, 178, 53, 1, 1);
    text(FONT_SMALL, 222, 56, "vs best", LABEL_BLUE, 1);
    if (hud->bestGap != RACE_HUD_NO_GAP) {
        text(FONT_SMALL, 222, 56 + line_pitch(), hud->bestGap <= 0 ? "ahead" : "behind", hud->bestGap <= 0 ? BEHIND : AHEAD, 1);
        seconds_gap(222, 82, hud->bestGap);
    }
    fill(174, 108, 312, 109, (Colour){ 255, 255, 255, 40 });
    picture_scaled(&sTimesIcon, 179, 114, 1, 1);
    text(FONT_SMALL, 206, 114, "Record", LABEL_BLUE, 1);
    if (hud->record >= 0) {
        race_time(179, 136, hud->record, 1);
    }
    if (hud->staffTime > 0) {
        RaceHudRacer tt = { 8 /* CHARACTER_TT */, RACE_HUD_NOT_SPECIAL, 0 };
        framed_portrait(&tt, 179, 156, 0, 0);
        text(FONT_SMALL, 206, 156, "T.T.", LABEL_BLUE, 1);
        text(FONT_SMALL, 206, 156 + line_pitch(), "to beat", DIM, 1);
        race_time(179, 181, hud->staffTime, 1);
    }
    {
        // The splits under the map, one line each: this run's laps against the best run's, then the best lap and run.
        int y = 139, k, best = -1;
        char label[24], delta[16];
        for (k = 0; k < hud->lapsTimed && k < RACE_HUD_LAPS && k < 4 && y < 175; k++) {
            int finished = k < hud->lapsTimed - 1 || hud->finished;
            snprintf(label, sizeof(label), port_text("Lap %d"), k + 1);
            text(FONT_SMALL, 6, y - 1, label, finished ? HINT : YELLOW, 1);
            race_time(58, y - 1 + digit_dy(), hud->lapTimes[k], 1);
            if (finished && k < hud->bestLapCount && hud->bestLaps[k] > 0) {
                int d = hud->lapTimes[k] - hud->bestLaps[k];
                format_gap(-d, delta, sizeof(delta));
                delta[0] = d <= 0 ? '-' : '+';
                text(FONT_SMALL, 172 - text_width(FONT_SMALL, delta), y - 1, delta, d <= 0 ? BEHIND : AHEAD, 1);
            }
            if (finished && (best < 0 || hud->lapTimes[k] < best)) {
                best = hud->lapTimes[k];
            }
            y += 15;
        }
        if (best >= 0) {
            text(FONT_SMALL, 6, y - 1, "Best lap", HINT, 1);
            race_time(58, y - 1 + digit_dy(), best, 1);
            y += 15;
        }
        if (hud->bestTotal > 0) {
            text(FONT_SMALL, 6, y - 1, "Best run", HINT, 1);
            race_time(58, y - 1 + digit_dy(), hud->bestTotal, 1);
        }
    }
    ghost_marker(&hud->ghost);
    ghost_marker(&hud->staffGhost);
}


#include "bottom_grid.inc"

static void draw_race(const State *s) {
    const RaceHud *hud = &s->race;
    make_race_base(hud);
    memcpy(sImage, sRaceBase, sizeof(sRaceBase));
    if (sGrid) {
        gdraw_race(hud);
        return;
    }
    switch (hud->mode) {
        case RACE_HUD_MODE_TIME_TRIAL:
            if (dock_tt(hud)) {
                time_trial_dock(hud);
            } else {
                draw_time_trial(hud);
            }
            break;
        case RACE_HUD_MODE_BOSS:
        case RACE_HUD_MODE_TAJ:
            draw_versus(hud);
            break;
        case RACE_HUD_MODE_BANANAS:
        case RACE_HUD_MODE_BATTLE:
        case RACE_HUD_MODE_EGGS:
            draw_challenge(hud);
            break;
        case RACE_HUD_MODE_HUB:
            draw_hub(hud);
            break;
        case RACE_HUD_MODE_MENU_SAVE:
            if (sGrid) {
                gsave(hud);
            } else {
                draw_menu_save(hud);
            }
            return;
        case RACE_HUD_MODE_MENU_TRACK:
            draw_menu_track(hud);
            return;
        default:
            if (own_race_layout(hud) && sGrid) {
                if (sStyle == 3) {
                    grace_dock(hud);
                    draw_map_markers(hud);
                } else {
                    grace_tower(hud);
                }
                return;
            }
            if (own_race_layout(hud)) {
                if (sStyle == 1) {
                    race_glass(hud);
                } else if (sStyle == 2) {
                    race_broadcast(hud);
                } else {
                    race_dock(hud);
                    draw_map_markers(hud);
                }
                return;
            }
            if (hud->silverCoins >= 0) {
                draw_silver_coins(hud);
            } else {
                draw_tower(hud);
                draw_item(hud);
                draw_standing(hud);
                draw_bananas_and_laps(hud);
            }
            break;
    }
    draw_map_markers(hud);
}

/******** The first-run setup (setup_screen.h, port_setup.c): its pages of tiles, each with a little picture. ********/

static Picture sSetupFace; /* Diddy's portrait at its own size: the kart in the tiles' pictures. */

static void load_setup_art(void) {
    load_texture(ASSET_TEX2D_HUD_KRUNCHICON + 9, &sSetupFace); // Diddy's (load_race_art's portrait order).
}

typedef struct Rect {
    int x0, y0, x1, y1;
} Rect;

#define SETUP_TILES_Y 24
#define SETUP_DOTS_Y 158
#define SETUP_HELP_Y 186

/* Where tile i of the page is. */
static Rect setup_tile(const SetupScreen *s, int i) {
    Rect r;
    int n = s->tileCount, width;
    if (s->page == SETUP_PAGE_SCREEN) {
        // Four, two by two: a picture on the left of each, the words on the right.
        r.x0 = 8 + (i % 2) * 154;
        r.y0 = SETUP_TILES_Y + (i / 2) * 66;
        r.x1 = r.x0 + 150;
        r.y1 = r.y0 + 62;
        return r;
    }
    width = (BS_WIDTH - 16 - (n - 1) * 6) / n;
    r.x0 = 8 + i * (width + 6);
    r.x1 = r.x0 + width;
    r.y0 = SETUP_TILES_Y;
    r.y1 = s->page == SETUP_PAGE_STYLE ? 178 : 152;
    return r;
}

/* A picture scaled to w by h at x, y (nearest pixel), only inside the clip rectangle, at alpha (255: as it is). */
static void picture_stretched(const Picture *p, int x, int y, int w, int h, Rect clip, int alpha) {
    int i, j;
    if (p->rgba == NULL || w <= 0 || h <= 0) {
        return;
    }
    for (j = 0; j < h; j++) {
        if (y + j < clip.y0 || y + j >= clip.y1) {
            continue;
        }
        for (i = 0; i < w; i++) {
            const unsigned char *t;
            if (x + i < clip.x0 || x + i >= clip.x1) {
                continue;
            }
            t = p->rgba + ((j * p->height / h) * p->width + i * p->width / w) * 4;
            if (t[3] != 0) {
                plot(x + i, y + j, t[0], t[1], t[2], t[3] * alpha / 255);
            }
        }
    }
}

static void fill_clipped(int x0, int y0, int x1, int y1, Rect clip, Colour c) {
    fill(x0 < clip.x0 ? clip.x0 : x0, y0 < clip.y0 ? clip.y0 : y0, x1 > clip.x1 ? clip.x1 : x1,
         y1 > clip.y1 ? clip.y1 : y1, c);
}

/*
 * A little race seen from behind the kart, in r: sky, grass, the road to the horizon, bananas on each side and Diddy
 * in his kart. zoom scales it (1: a 4:3 picture's height fills r), edge stretches what is out beyond the 4:3 picture's
 * sides, as a wide view does, tilt leans the horizon (pixels across r), ghosts trails the kart as motion does, and
 * hud adds the N64's race info along the top.
 */
typedef struct Scene {
    float zoom, edge;
    int tilt, ghosts, hud, kartOffset;
} Scene;

static void draw_scene(Rect r, Scene sc) {
    int w = r.x1 - r.x0, h = r.y1 - r.y0, cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2, x, y, i;
    float unit = h / 36.0f * sc.zoom; // The scene is 48 by 36 units, a 4:3 picture.
    int horizon = cy - (int) (3 * unit);
    static const int sBananas[4] = { -15, 15, -27, 27 };

    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            int line = horizon + sc.tilt * (x - cx) / (w > 0 ? w : 1), t;
            if (y < line) {
                t = (y - r.y0) * 255 / (h > 0 ? h : 1);
                plot(x, y, 90 + t / 4, 150 + t / 5, 250, 255);
            } else {
                // The road narrows to the horizon; grass either side.
                float depth = (float) (y - line) / (float) (r.y1 - line > 0 ? r.y1 - line : 1);
                float half = (1.5f + 13.0f * depth) * unit;
                int road = x > cx - half && x < cx + half;
                if (road) {
                    plot(x, y, 210, 175, 110, 255);
                } else {
                    plot(x, y, 70 + (int) (40 * depth), 160, 60, 255);
                }
            }
        }
    }
    // Bananas: the near ones inside the 4:3 picture, the far ones only in a wide one.
    for (i = 0; i < 4 && sBanana.rgba != NULL; i++) {
        float u = sBananas[i], size = 5.0f * unit, stretch = (u < -24 || u > 24) || i >= 2 ? sc.edge : 1.0f;
        int bw = (int) (size * stretch), bh = (int) size;
        int bx = cx + (int) (u * unit * (i >= 2 ? (sc.edge + 1.0f) / 2.0f : 1.0f)) - bw / 2;
        picture_stretched(&sBanana, bx, horizon + (int) (2 * unit), bw, bh, r, 255);
    }
    // The kart, with speed lines behind it for motion (faded copies of the face read as a second kart).
    if (sSetupFace.rgba != NULL) {
        int size = (int) (13 * unit), kx = cx - size / 2 + (int) (sc.kartOffset * unit), ky = cy + (int) (2 * unit);
        for (i = 0; i < sc.ghosts * 2; i++) {
            int ly = ky + size * (3 + i * 3) / (sc.ghosts * 6 + 2), len = size * (i % 2 ? 5 : 8) / 10, seg;
            int thick = unit >= 2.0f ? 2 : 1;
            // Three pieces, fainter away from the kart.
            for (seg = 0; seg < 3; seg++) {
                int x1 = kx + 1 - seg * len / 3, x0 = kx + 1 - (seg + 1) * len / 3;
                fill_clipped(x0, ly, x1, ly + thick, r, (Colour){ 255, 255, 255, 200 - seg * 65 });
            }
        }
        picture_stretched(&sSetupFace, kx, ky, size, size, r, 255);
    }
    if (sc.hud) {
        // The N64's race info: the place on the left, laps and bananas in the middle, the time on the right.
        int bar = (int) (2 * unit) + 1;
        fill_clipped(cx - (int) (21 * unit), r.y0 + bar, cx - (int) (17 * unit), r.y0 + 3 * bar, r, YELLOW);
        fill_clipped(cx - (int) (6 * unit), r.y0 + bar, cx + (int) (3 * unit), r.y0 + 2 * bar, r, WHITE);
        fill_clipped(cx + (int) (9 * unit), r.y0 + bar, cx + (int) (21 * unit), r.y0 + 2 * bar, r, YELLOW);
    }
}

/* A frame in the colour of a tile's border round a picture. */
static void picture_frame(Rect r, int lit) {
    Colour c = lit ? PICKED_EDGE : (Colour){ 140, 180, 255, 200 };
    fill(r.x0 - 1, r.y0 - 1, r.x1 + 1, r.y0, c);
    fill(r.x0 - 1, r.y1, r.x1 + 1, r.y1 + 1, c);
    fill(r.x0 - 1, r.y0, r.x0, r.y1, c);
    fill(r.x1, r.y0, r.x1 + 1, r.y1, c);
}

/* A 5:3 screen, as tall as fits in r and centred in it. */
static Rect wide_screen_in(Rect r, int maxHeight) {
    Rect s;
    int h = r.y1 - r.y0 < maxHeight ? r.y1 - r.y0 : maxHeight, w = h * 5 / 3;
    if (w > r.x1 - r.x0) {
        w = r.x1 - r.x0;
        h = w * 3 / 5;
    }
    s.x0 = (r.x0 + r.x1 - w) / 2;
    s.x1 = s.x0 + w;
    s.y0 = r.y0;
    s.y1 = s.y0 + h;
    return s;
}

/* The widescreen styles' pictures (0 4:3, 1 Wide, 2 Balanced, 3 Zoomed). */
static void draw_screen_style(Rect r, int style, Scene sc) {
    static const float sZoom[4] = { 1.0f, 1.0f, 1.1f, 1.25f };
    static const float sEdge[4] = { 1.0f, 1.6f, 1.3f, 1.0f };
    if (style == 0) {
        int inner = (r.y1 - r.y0) * 4 / 3;
        Rect middle = { (r.x0 + r.x1 - inner) / 2, r.y0, (r.x0 + r.x1 + inner) / 2, r.y1 };
        fill(r.x0, r.y0, r.x1, r.y1, (Colour){ 0, 0, 0, 255 });
        draw_scene(middle, sc);
        return;
    }
    sc.zoom = sZoom[style];
    sc.edge = sEdge[style];
    draw_scene(r, sc);
}

/* The two screens of a 3DS, with the race info where the tile puts it (0 bottom, 1 top, 2 both). */
static void draw_two_screens(Rect r, int where) {
    int h = (r.y1 - r.y0 - 2) / 2, w = h * 5 / 3, i;
    Rect top = { (r.x0 + r.x1 - w) / 2, r.y0, (r.x0 + r.x1 + w) / 2, r.y0 + h };
    Rect bottom = { (r.x0 + r.x1) / 2 - h * 2 / 3, r.y0 + h + 2, (r.x0 + r.x1) / 2 + h * 2 / 3, r.y0 + 2 * h + 2 };
    Scene sc = { 1.1f, 1.3f, 0, 0, where != 0, 0 };
    draw_scene(top, sc);
    picture_frame(top, 0);
    fill(bottom.x0, bottom.y0, bottom.x1, bottom.y1, (Colour){ 20, 40, 120, 255 });
    picture_frame(bottom, 0);
    if (where == 1) {
        return; // Nothing on the bottom screen.
    }
    // The standings tower down the left, a track map on the right.
    for (i = 0; i < 4; i++) {
        int y = bottom.y0 + 2 + i * (bottom.y1 - bottom.y0 - 4) / 4, rowHeight = (bottom.y1 - bottom.y0 - 4) / 4 - 1;
        Rect row = { bottom.x0 + 2, y, bottom.x0 + (bottom.x1 - bottom.x0) / 2, y + rowHeight };
        fill(row.x0, row.y0, row.x1, row.y1, i == 1 ? (Colour){ 255, 200, 40, 200 } : (Colour){ 60, 90, 190, 255 });
        picture_stretched(&sPortraits[(i * 3 + 2) % 10], row.x0 + 1, row.y0, rowHeight, rowHeight, row, 255);
    }
    {
        int mx = (bottom.x0 + bottom.x1) * 3 / 4 - 2, my = (bottom.y0 + bottom.y1) / 2;
        int rx = (bottom.x1 - bottom.x0) / 6, ry = (bottom.y1 - bottom.y0) / 3, a;
        for (a = 0; a < 64; a++) {
            float t = a * (6.2831853f / 64);
            plot(mx + (int) (rx * cosf(t)), my + (int) (ry * sinf(t)), 255, 255, 255, 255);
        }
    }
}

/* The two screens with the bottom one in a look (0 Classic .. 3 Dock): the race picture above, and below it the
 * standings, the map and the buttons, drawn with the look's own boxes (box() follows sStyle). */
static void draw_two_looks(Rect r, int look) {
    int h = (r.y1 - r.y0 - 2) / 2, w = h * 5 / 3, i, saved = sStyle;
    Rect top = { (r.x0 + r.x1 - w) / 2, r.y0, (r.x0 + r.x1 + w) / 2, r.y0 + h };
    Rect bottom = { (r.x0 + r.x1) / 2 - h * 2 / 3 - 2, r.y0 + h + 2, (r.x0 + r.x1) / 2 + h * 2 / 3 + 2, r.y0 + 2 * h + 2 };
    int bw = bottom.x1 - bottom.x0, bh = bottom.y1 - bottom.y0, rowH = (bh - 9) / 3;
    Scene sc = { 1.1f, 1.3f, 0, 0, 0, 0 };
    draw_scene(top, sc);
    picture_frame(top, 0);
    fill(bottom.x0, bottom.y0, bottom.x1, bottom.y1, (Colour){ 20, 40, 120, 255 });
    sStyle = look;
    for (i = 0; i < 3; i++) {
        int y = bottom.y0 + 2 + i * (rowH + 1);
        Colour lit = i == 0 ? PICKED_EDGE : BOX_EDGE;
        box(bottom.x0 + 2, y, bottom.x0 + bw / 2 - 1, y + rowH, i == 0 ? (Colour){ 255, 200, 40, 90 } : BOX_FILL, lit);
    }
    box(bottom.x0 + bw / 2 + 1, bottom.y0 + 2, bottom.x1 - 2, bottom.y0 + 2 + 2 * rowH, BOX_FILL, BOX_EDGE);
    // The buttons along the bottom: boxes, or in the Dock a few round icons.
    for (i = 0; i < 3; i++) {
        int x = bottom.x0 + 2 + i * (bw - 4) / 3;
        if (look == 3) {
            round_disc(x + (bw - 4) / 6, bottom.y1 - 4, 2, i == 1 ? (Colour){ 255, 214, 40, 255 } : (Colour){ 255, 255, 255, 220 });
        } else {
            box(x, bottom.y1 - 7, x + (bw - 4) / 3 - 2, bottom.y1 - 2, BOX_FILL, BOX_EDGE);
        }
    }
    sStyle = saved;
    picture_frame(bottom, 0);
}

/* Whether x, y is inside a polygon of count points (even-odd rule). */
static int inside(const float (*points)[2], int count, float x, float y) {
    int i, j, in = 0;
    for (i = 0, j = count - 1; i < count; j = i++) {
        if ((points[i][1] > y) != (points[j][1] > y) &&
            x < (points[j][0] - points[i][0]) * (y - points[i][1]) / (points[j][1] - points[i][1]) + points[i][0]) {
            in = !in;
        }
    }
    return in;
}

/* The flags on the language page, for the North American game: the United States for English, Canada for French. */
static void draw_flag(Rect r, int french) {
    // The maple leaf, in a square of 1 across (y down).
    static const float sLeaf[][2] = {
        { 0.50f, 0.05f }, { 0.43f, 0.20f }, { 0.36f, 0.17f }, { 0.40f, 0.38f }, { 0.30f, 0.28f }, { 0.27f, 0.34f },
        { 0.15f, 0.31f }, { 0.20f, 0.44f }, { 0.13f, 0.48f }, { 0.30f, 0.62f }, { 0.27f, 0.70f }, { 0.47f, 0.67f },
        { 0.47f, 0.88f }, { 0.53f, 0.88f }, { 0.53f, 0.67f }, { 0.73f, 0.70f }, { 0.70f, 0.62f }, { 0.87f, 0.48f },
        { 0.80f, 0.44f }, { 0.85f, 0.31f }, { 0.73f, 0.34f }, { 0.70f, 0.28f }, { 0.60f, 0.38f }, { 0.64f, 0.17f },
        { 0.57f, 0.20f },
    };
    static const Colour RED = { 210, 20, 40, 255 }, NAVY = { 20, 40, 120, 255 };
    int w = r.x1 - r.x0, h = r.y1 - r.y0, x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            Colour c;
            if (french) {
                // Red quarters at the sides, white square in the middle with the red maple leaf.
                if (x < w / 4 || x >= w - w / 4) {
                    c = RED;
                } else {
                    float side = (float) (w - 2 * (w / 4));
                    float u = (x - w / 4 + 0.5f) / side, v = (y + 0.5f) / h * 1.0f;
                    // The leaf fills the middle square's height.
                    u = 0.5f + (u - 0.5f) * side / h;
                    c = inside(sLeaf, sizeof(sLeaf) / sizeof(sLeaf[0]), u, v) ? RED : WHITE;
                }
            } else {
                // Thirteen stripes, red first, and the blue canton over the top seven with rows of white stars.
                int stripe = y * 13 / h, cantonW = w * 2 / 5, cantonH = h * 7 / 13;
                c = stripe % 2 == 0 ? RED : WHITE;
                if (x < cantonW && y < cantonH) {
                    int sx = x * 6 * 2 / cantonW, sy = y * 5 * 2 / cantonH;
                    c = NAVY;
                    if (sx % 2 == 1 && sy % 2 == 1 && w >= 40) {
                        c = WHITE;
                    }
                }
            }
            plot(r.x0 + x, r.y0 + y, c.r, c.g, c.b, 255);
        }
    }
}

/* Draws a tile's picture in r. */
static void draw_tile_art(const SetupScreen *s, int i, Rect r) {
    Scene plain = { 1.1f, 1.3f, 0, 0, 0, 0 };
    switch (s->page) {
        case SETUP_PAGE_LANGUAGE:
            draw_flag(r, i == 1);
            break;
        case SETUP_PAGE_STYLE: {
            int st = s->tiles[i].art; // The play style (an Old 3DS's tiles aren't in the New 3DS's order).
            if (st == SETUP_STYLE_CLASSIC) {
                plain.hud = 1;
                draw_screen_style(r, 0, plain);
            } else if (st == SETUP_STYLE_CLASSICPLUS) {
                draw_screen_style(r, 0, plain); // 4:3 picture, the map and standings below
            } else {
                plain.ghosts = st == SETUP_STYLE_SPEEDY ? 2 : 0;
                draw_screen_style(r, 2, plain);
            }
            break;
        }
        case SETUP_PAGE_PICTURE: {
            int d;
            int old = !plat_can_60fps(); // An Old 3DS's second tile is a steady 20 fps, not 60.
            plain.ghosts = i == 1 && !old ? 2 : 0;
            draw_screen_style(r, 2, plain);
            // The frame rate in the HUD's digits, top left.
            for (d = 0; d < 2; d++) {
                const Picture *digit = &sDigits[d == 0 ? (i == 1 ? (old ? 2 : 6) : 3) : 0];
                picture_stretched(digit, r.x0 + 3 + d * (digit->width - 1), r.y0 + 3, digit->width, digit->height, r,
                                  255);
            }
            break;
        }
        case SETUP_PAGE_SCREEN:
            draw_screen_style(r, i, plain);
            break;
        case SETUP_PAGE_CAMERA:
            // The N64's camera swings out and leans; the Modern one stays behind the kart, level.
            if (i == 1) {
                plain.tilt = (r.y1 - r.y0) / 3;
                plain.kartOffset = 6;
            }
            draw_scene(r, plain);
            break;
        case SETUP_PAGE_LOOK:
            draw_two_looks(r, i);
            return;
        default:
            draw_two_screens(r, i == 0 ? 0 : i == 1 ? 1 : 2);
            return; // Its screens have their own frames.
    }
    picture_frame(r, i == s->chosen);
}

static void draw_setup_tile(const SetupScreen *s, int i) {
    const SetupTile *t = &s->tiles[i];
    Rect r = setup_tile(s, i), art;
    int lit = i == s->chosen, y, l, textX0 = r.x0, textX1 = r.x1;
    Colour lineColour = lit ? YELLOW : DIM;

    if (lit) {
        box(r.x0, r.y0, r.x1, r.y1, (Colour){ 255, 200, 40, 90 }, PICKED_EDGE);
        box(r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1, (Colour){ 0, 0, 0, 0 }, PICKED_EDGE);
    } else {
        box(r.x0, r.y0, r.x1, r.y1, BOX_FILL, BOX_EDGE);
    }
    if (s->page == SETUP_PAGE_SCREEN) {
        // The picture on the left, the words on the right.
        art = (Rect){ r.x0 + 6, r.y0 + 13, r.x0 + 66, r.y0 + 49 };
        draw_tile_art(s, i, art);
        textX0 = r.x0 + 68;
        heading(textX0, textX1, r.y0 + 5, t->name, 1);
        for (l = 0; l < t->lineCount && l < 2; l++) {
            text_centred(FONT_SMALL, textX0, textX1 - 2, r.y0 + 24 + l * 11, t->lines[l], lineColour, 1);
        }
        if (t->tag[0] != '\0') {
            text_centred(FONT_SMALL, textX0, textX1 - 2, r.y0 + 47, t->tag, lit ? WHITE : HINT, 1);
        }
        return;
    }
    if (s->page == SETUP_PAGE_LANGUAGE) {
        art = (Rect){ (r.x0 + r.x1) / 2 - 42, r.y0 + 8, (r.x0 + r.x1) / 2 + 42, r.y0 + 64 };
    } else if (s->page == SETUP_PAGE_HUD || s->page == SETUP_PAGE_LOOK) {
        art = (Rect){ r.x0 + 4, r.y0 + 5, r.x1 - 4, r.y0 + 59 };
    } else {
        art = wide_screen_in((Rect){ r.x0 + 6, r.y0 + 6, r.x1 - 6, r.y1 }, s->page == SETUP_PAGE_STYLE ? 52 : 56);
    }
    draw_tile_art(s, i, art);
    y = art.y1 + 6;
    heading(textX0, textX1, y, t->name, 1);
    y += 18;
    if (s->page == SETUP_PAGE_STYLE && t->tag[0] != '\0') {
        text_centred(FONT_SMALL, textX0, textX1, y, t->tag, lit ? WHITE : HINT, 1);
        y += 15;
    }
    for (l = 0; l < t->lineCount; l++, y += 11) {
        text_centred(FONT_SMALL, textX0 + 2, textX1 - 2, y, t->lines[l], lineColour, 1);
    }
    if (s->page != SETUP_PAGE_STYLE && t->tag[0] != '\0') {
        text_centred(FONT_SMALL, textX0, textX1, y + 3, t->tag, lit ? WHITE : HINT, 1);
    }
}

/* A filled dot of radius r. */
static void disc(int cx, int cy, int r, Colour c) {
    int x, y;
    for (y = -r; y <= r; y++) {
        for (x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r + r) {
                plot(cx + x, cy + y, c.r, c.g, c.b, c.a);
            }
        }
    }
}

static void draw_setup(const SetupScreen *s) {
    int i;
    heading(0, BS_WIDTH, 4, s->question, 1);
    for (i = 0; i < s->tileCount && i < SETUP_TILES; i++) {
        draw_setup_tile(s, i);
    }
    // The fine tuning's steps: done in white, this one in gold, the rest grey.
    for (i = 0; i < s->stepCount && i < SETUP_STEPS; i++) {
        int width = (BS_WIDTH - 16) / s->stepCount, x0 = 8 + i * width, cx = x0 + width / 2;
        Colour c = i == s->step ? YELLOW : i < s->step ? WHITE : (Colour){ 150, 155, 175, 255 };
        disc(cx, SETUP_DOTS_Y + 4, 4, i <= s->step ? c : (Colour){ 60, 70, 110, 255 });
        text_centred(FONT_SMALL, x0, x0 + width, SETUP_DOTS_Y + 11, s->steps[i], c, 1);
    }
    text_centred(FONT_SMALL, 4, BS_WIDTH - 4, SETUP_HELP_Y, s->help, HINT, 1);
}

int bs_setup_hit(int x, int y) {
    const SetupScreen *s = &sState.setup;
    int i;
    if (!sState.setupShown) {
        return SETUP_HIT_NONE;
    }
    if (y >= BS_BUTTONS_Y && y < BS_STATUS_Y) {
        // As draw_button_row lays out three: 13 columns of 8 pixels each, the first one a gap.
        int column = x / 8;
        i = column / 13;
        if (i < 3 && column % 13 != 0 && s->buttons[i][0] != '\0') {
            return SETUP_HIT_BACK + i;
        }
        return SETUP_HIT_NONE;
    }
    for (i = 0; i < s->tileCount && i < SETUP_TILES; i++) {
        Rect r = setup_tile(s, i);
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) {
            return i;
        }
    }
    return SETUP_HIT_NONE;
}

void bs_area_rows(int area, int *first, int *end) {
    /* On the grid the bar's area starts in the 4 px gap above it (GY1 196 .. GBTN_Y0 200), so the bar draws whole */
    int rows[BS_AREA_COUNT + 1] = { 0, sGrid ? 198 : BS_BUTTONS_Y, BS_STATUS_Y, BS_HEIGHT };
    *first = rows[area];
    *end = rows[area + 1];
}

void bs_draw(unsigned short *image, int area) {
    if (!sBackgroundMade) {
        make_background();
    }
    sImage = image;
    bs_area_rows(area, &sClipTop, &sClipEnd);
    memcpy(image + sClipTop * BS_WIDTH, sBackground + sClipTop * BS_WIDTH,
           (sClipEnd - sClipTop) * BS_WIDTH * sizeof(unsigned short));
    switch (area) {
        case BS_AREA_TOP:
            if (sDrawn.setupShown) {
                draw_setup(&sDrawn.setup);
            } else if (sDrawn.menuShown) {
                draw_menu(&sDrawn);
            } else if (sDrawn.raceShown && sDrawn.overlayLines == 0) {
                draw_race(&sDrawn);
            } else {
                draw_home(&sDrawn);
            }
            break;
        case BS_AREA_BUTTONS:
            draw_buttons(&sDrawn);
            break;
        default:
            draw_status(&sDrawn);
            break;
    }
}

#ifndef __3DS__
/* Host only (DKR_BOTTOM_DUMP=dir): writes the ROM pictures the bottom screen draws, for the sharp-art prototype. */
int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);
static void dump_one(const char *dir, const char *name, const Picture *p) {
    char path[512];
    if (p->rgba == NULL) {
        return;
    }
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    stbi_write_png(path, p->width, p->height, 4, p->rgba, p->width * 4);
}
void bs_dump_art(const char *dir) {
    int i;
    char name[32];
    dump_one(dir, "logo", &sLogo);
    dump_one(dir, "egg", &sEgg);
    dump_one(dir, "egg_whole", &sEggWhole);
    dump_one(dir, "banana", &sBanana);
    dump_one(dir, "balloon", &sBalloon);
    dump_one(dir, "coin", &sCoin);
    for (i = 0; i < 10; i++) {
        snprintf(name, sizeof(name), "sdigit_%d", i);
        dump_one(dir, name, &sSmallDigits[i]);
        snprintf(name, sizeof(name), "digit_%d", i);
        dump_one(dir, name, &sDigits[i]);
    }
    dump_one(dir, "scolon", &sSmallColon);
    for (i = 0; i < 3 * 32; i++) {
        snprintf(name, sizeof(name), "font%d_%02d", i / 32, i % 32);
        dump_one(dir, name, &sFonts[i / 32].textures[i % 32]);
    }
    for (i = 0; i < 15; i++) {
        snprintf(name, sizeof(name), "item_%02d", i);
        dump_one(dir, name, &sItems[i]);
    }
}
#endif

#ifndef __3DS__
/*
 * Host preview only: puts each racer on the track's map, as a race would. The map is built for this hud, then each
 * racer is placed at its own angle round the map's middle, on the middle of the outermost stretch of track along that
 * ray (light, opaque pixels; the dark rim is not track), and written back as a map point (the inverse of map_point).
 */
void bs_preview_place_racers(RaceHud *hud, const short *degrees) {
    int i, x, y, n = 0;
    float cx = 0, cy = 0;
    make_race_base(hud);
    if (sMap.rgba == NULL) {
        return;
    }
    for (y = 0; y < sMap.height; y++) {
        for (x = 0; x < sMap.width; x++) {
            if (sMap.rgba[(y * sMap.width + x) * 4 + 3] == 255) {
                cx += x;
                cy += y;
                n++;
            }
        }
    }
    if (n == 0) {
        return;
    }
    cx /= n;
    cy /= n;
    {
        float placed[16][3];
        unsigned char foot[21][21];
        int np = 0;
        for (i = 0; i < hud->racerCount; i++) {
            int step, ok = 0;
            float px = 0, py = 0, deg = 0, ptx = 0, pty = -1;
            int r = hud->racers[i].player ? 10 : (marker_small() ? 2 : 3);
            float rr = hud->racers[i].player ? arrow_scale() * 7.0f + 1.0f : (float) r;
            /* the racer's own angle, or the nearest one round from it that is clear of the start's checks and of
             * the racers already placed */
            for (step = 0; step < 60 && !ok; step++) {
                float a, dx, dy, t;
                int inRun = 0, runStart = 0, bestA = -1, bestB = -1, j;
                deg = degrees[i] + (step & 1 ? -1 : 1) * 6 * ((step + 1) / 2);
                a = deg * 3.14159265f / 180.0f, dx = sinf(a), dy = -cosf(a);
                for (t = 0; t < 400; t += 0.5f) {
                    int qx = (int) (cx + dx * t), qy = (int) (cy + dy * t), track;
                    if (qx < 0 || qy < 0 || qx >= sMap.width || qy >= sMap.height) {
                        break;
                    }
                    {
                        const unsigned char *o = sMap.rgba + (qy * sMap.width + qx) * 4;
                        track = o[3] == 255 && o[0] + o[1] + o[2] > 3 * 128;
                    }
                    if (track && !inRun) {
                        inRun = 1;
                        runStart = (int) (t * 2);
                    } else if (!track && inRun) {
                        inRun = 0;
                        bestA = runStart;
                        bestB = (int) (t * 2);
                    }
                }
                if (bestA < 0) {
                    continue;
                }
                t = (bestA + bestB) / 4.0f;
                if (hud->racers[i].player) {
                    int d = ((int) deg % 360 + 360) % 360;
                    arrow_mask((unsigned short) ((360 - d) % 360 * 65536 / 360 + 0x4000), foot);
                }
                px = cx + dx * t + 0.5f;
                py = cy + dy * t + 0.5f;
                if (sMapRebuilt) {
                    /* a map drawn from its centre line: on the line (every marker fits the lane), facing the way the
                       track runs there */
                    float sx, sy;
                    if (!path_snap(px, py, &sx, &sy, &ptx, &pty)) {
                        continue;
                    }
                    px = floorf(sx) + 0.5f;
                    py = floorf(sy) + 0.5f;
                    r = hud->racers[i].player ? 4 : 3;
                    rr = (float) r;
                } else
                /* the nearest spot to the run's middle where the whole marker is on the track, clear of the rim */
                {
                    int best = 1 << 30, bx = 0, by = 0, ox, oy;
                    for (oy = -4; oy <= 4; oy++) {
                        for (ox = -4; ox <= 4; ox++) {
                            int qx = (int) px + ox, qy = (int) py + oy, fits = 1, fx, fy;
                            for (fy = -r; fy <= r && fits; fy++) {
                                for (fx = -r; fx <= r && fits; fx++) {
                                    const unsigned char *o;
                                    if (hud->racers[i].player ? fy < -10 || fy > 10 || fx < -10 || fx > 10 ||
                                                                    !foot[fy + 10][fx + 10]
                                                              : fx * fx + fy * fy > r * r + r) {
                                        continue;
                                    }
                                    if (qx + fx < 0 || qy + fy < 0 || qx + fx >= sMap.width || qy + fy >= sMap.height) {
                                        fits = 0;
                                        break;
                                    }
                                    o = sMap.rgba + ((qy + fy) * sMap.width + qx + fx) * 4;
                                    fits = o[3] == 255 && o[0] + o[1] + o[2] > 3 * 128;
                                }
                            }
                            if (fits && ox * ox + oy * oy < best) {
                                best = ox * ox + oy * oy;
                                bx = qx;
                                by = qy;
                            }
                        }
                    }
                    if (best == 1 << 30) {
                        continue;
                    }
                    px = bx + 0.5f;
                    py = by + 0.5f;
                }
                ok = 1;
                for (j = 0; j < np && ok; j++) {
                    float ex = placed[j][0] - px, ey = placed[j][1] - py;
                    float need = placed[j][2] + rr + (sMapRebuilt ? 3 : 2); /* (redrawn: 2 clear px between) */
                    ok = ex * ex + ey * ey >= need * need;
                }
                if (sMapRebuilt && sMapChk[2] >= 0 && ok) {
                    ok = (int) px + 3 + 2 < sMapChk[0] || (int) px - 3 - 2 > sMapChk[2] || (int) py + 3 + 2 < sMapChk[1] ||
                         (int) py - 3 - 2 > sMapChk[3];
                }
                for (y = (int) py - (int) rr - 5; y <= (int) py + (int) rr + 5 && ok; y++) {
                    for (x = (int) px - (int) rr - 5; x <= (int) px + (int) rr + 5 && ok; x++) {
                        const unsigned char *o;
                        if (x < 0 || y < 0 || x >= sMap.width || y >= sMap.height) {
                            continue;
                        }
                        o = sMap.rgba + (y * sMap.width + x) * 4;
                        ok = !(o[3] == 255 && o[0] == 16 && o[1] == 18 && o[2] == 28);
                    }
                }
            }
            if (!ok) {
                hud->racers[i].at.onMap = 0;
                continue;
            }
            if (np < 16) {
                placed[np][0] = px;
                placed[np][1] = py;
                placed[np][2] = sMapRebuilt ? rr : hud->racers[i].player ? arrow_scale() * 7.0f + 1.0f : (float) r;
                np++;
            }
            {
                float u = (px - 3) / sMapScale, v = (py - 3) / sMapScale;
                int d = ((int) deg % 360 + 360) % 360;
                hud->racers[i].at.onMap = 1;
                hud->racers[i].at.mapX = (short) ((u + sMapTexX + (sMapBitmap ? 0.0f : 1.5f)) * 4);
                hud->racers[i].at.mapY = (short) ((v + sMapTexY + (sMapBitmap ? 0.0f : 1.0f)) * 4);
                /* facing along the track: a quarter turn from the ray, counter-clockwise */
                hud->racers[i].at.heading = (unsigned short) ((360 - d) % 360 * 65536 / 360 + 0x4000);
                if (sMapRebuilt) {
                    /* the heading whose direction (-sin, -cos) is the track's way here */
                    float ha = atan2f(-ptx, -pty);
                    hud->racers[i].at.heading = (unsigned short) ((int) (ha * 65536.0f / 6.2831853f) & 0xFFFF);
                }
            }
        }
    }
    sRaceBaseMade = 0;
}
#endif
