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
    unsigned char advance;
    signed char x, y;
    unsigned char width, height;
    unsigned short offset;
} UiGlyph;
typedef struct UiFont {
    int space, base; /* base: pixels from the text's top to its baseline. */
    const UiGlyph *glyphs; /* '!' (0x21) to '~' (0x7E). */
    const unsigned char *pixels; /* 8-bit coverage, row by row from offset. */
} UiFont;
#include "ui_font_data.h"

static Font sFonts[3];
static Picture sLogo, sSmallLogo; /* The title logo, and at half size for the menus' companions. */
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

/* Every other column: the silver coin is drawn twice as wide as the game shows it. */
static void narrow(Picture *p) {
    int w = p->width / 2, x, y;
    for (y = 0; p->rgba != NULL && y < p->height; y++) {
        for (x = 0; x < w; x++) {
            memcpy(p->rgba + (y * w + x) * 4, p->rgba + (y * p->width + x * 2) * 4, 4);
        }
    }
    p->width = w;
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
    sSmallLogo = sLogo;
    if ((sSmallLogo.rgba = malloc(160 * 64 * 4)) != NULL) {
        memcpy(sSmallLogo.rgba, sLogo.rgba, 160 * 64 * 4);
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

static void plot(int x, int y, int r, int g, int b, int a) {
    unsigned short *p, d;
    int dr, dg, db;
    if (x < 0 || x >= BS_WIDTH || y < sClipTop || y >= sClipEnd || a <= 0) {
        return;
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
}

static int is_lit_edge(Colour edge) {
    return edge.r > 200 && edge.g > 150 && edge.b < 100 && edge.a > 200;
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
                smooth_box(x0, y0, x1, y1, (y1 - y0) > 40 ? 7 : (y1 - y0) / 2, (Colour){ 24, 44, 118, 165 },
                           (Colour){ 16, 34, 100, 170 }, (Colour){ 190, 215, 255, 80 }, 1);
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
                smooth_box(x0, y0, x1, y1, (y1 - y0) > 40 ? 9 : (y1 - y0) / 2, (Colour){ 0, 20, 80, 70 },
                           (Colour){ 0, 20, 80, 70 }, (Colour){ 0, 0, 0, 0 }, 1);
            }
            break;
    }
}

/* Part of a picture, tinted (255 each: as it is); a black tint with alpha draws its shadow. */
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
            if (t[3] != 0) {
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
            return NULL; /* Classic keeps the game's fonts. */
    }
}

/* The look's own font for the small text and headings (NULL: the game's). */
static const UiFont *ui_font(int font) {
    if ((font != FONT_SMALL && font != FONT_FUN) || !font_ready(font)) {
        return NULL;
    }
    return ui_font_kind(font == FONT_FUN ? UI_HEAD : UI_BODY);
}

static int ui_advance(const UiFont *u, unsigned char c) {
    return c <= 0x20 || c >= 0x7F ? u->space : u->glyphs[c - 0x21].advance;
}

static int text_width_as_is(int font, const char *text) {
    int width = 0;
    char prev = 0;
    const UiFont *u = ui_font(font);
    if (u != NULL) {
        for (; *text != '\0'; text++) {
            width += ui_advance(u, (unsigned char) *text);
        }
        return width;
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

/* Draws text in one of the looks' fonts from x with its top at y; returns where it ended. */
static int ui_draw(const UiFont *u, int x, int y, const char *text, Colour colour, int shadow, int right) {
    for (; *text != '\0'; text++) {
        unsigned char c = (unsigned char) *text;
        int step = ui_advance(u, c), i, j;
        if (x + step > right) {
            break;
        }
        if (c > 0x20 && c < 0x7F) {
            const UiGlyph *g = &u->glyphs[c - 0x21];
            for (j = 0; j < g->height; j++) {
                for (i = 0; i < g->width; i++) {
                    int cover = u->pixels[g->offset + j * g->width + i];
                    if (cover != 0 && shadow) {
                        plot(x + g->x + i + 1, y + g->y + j + 1, 0, 0, 0, cover * 170 / 255);
                    }
                }
            }
            for (j = 0; j < g->height; j++) {
                for (i = 0; i < g->width; i++) {
                    int cover = u->pixels[g->offset + j * g->width + i];
                    if (cover != 0) {
                        plot(x + g->x + i, y + g->y + j, colour.r, colour.g, colour.b, cover * colour.a / 255);
                    }
                }
            }
        }
        x += step;
    }
    return x;
}

/* Draws text from x (until right, if text would go further) with its top at y, as it is. Returns where it ended. */
static int text_clipped_as_is(int font, int x, int y, const char *text, Colour colour, int shadow, int right) {
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

static int text_clipped(int font, int x, int y, const char *text, Colour colour, int shadow, int right) {
    return text_clipped_as_is(font, x, y, port_text(text), colour, shadow, right);
}

static int text(int font, int x, int y, const char *s, Colour colour, int shadow) {
    return text_clipped(font, x, y, s, colour, shadow, BS_WIDTH);
}

static void text_centred_as_is(int font, int x0, int x1, int y, const char *s, Colour colour, int shadow) {
    int width = text_width_as_is(font, s);
    text_clipped_as_is(font, x0 + (x1 - x0 - width) / 2, y, s, colour, shadow, x1);
}

static void text_centred(int font, int x0, int x1, int y, const char *s, Colour colour, int shadow) {
    text_centred_as_is(font, x0, x1, y, port_text(s), colour, shadow);
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
        if (y >= BS_STATUS_Y) {
            r = 4;
            g = 8;
            b = 34;
        }
        for (x = 0; x < BS_WIDTH; x++) {
            int light = 0;
            if (y < BS_STATUS_Y) {
                if (sStyle == 0) {
                    light = ((x / 20) + (y / 20)) % 2 == 0 ? 7 : 0;
                } else if (sStyle == 1) {
                    light = ((x / 20) + (y / 20)) % 2 == 0 ? 3 : 0;
                    light += (60 - (x - 60) * (x - 60) / 80 - (y + 20) * (y + 20) / 90) > 0 ? 8 : 0;
                } else if (sStyle == 2) {
                    light = ((x + y) / 6) % 2 == 0 ? 3 : 0; // Faint diagonal stripes.
                    if (y < 3) {
                        r = 235 - x * 20 / BS_WIDTH;
                        g = 40 + x * 100 / BS_WIDTH;
                        b = 30;
                    }
                } else {
                    // Dock: calm, with a soft darker swell near the bottom like the title's sea.
                    light = y > 170 + (int) (4 * sinf(x / 24.0f)) ? -10 : 0;
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
    char overlay[OVERLAY_LINES][48];
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
static void draw_messages(const State *s, int y0, int y1) {
    enum { MAX_LINES = 24 };
    const char *starts[MAX_LINES];
    int lengths[MAX_LINES], newest[MAX_LINES];
    int font = font_ready(FONT_SMALL) ? FONT_SMALL : FONT_MONO, lineHeight = font == FONT_SMALL ? 12 : 9;
    int fit = (y1 - y0 - 6) / lineHeight, count = 0, m, i;
    int width = BS_WIDTH - 24;

    box(4, y0, BS_WIDTH - 4, y1, BOX_FILL, BOX_EDGE);
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
    for (i = 0; i < count; i++) {
        char line[160];
        int y = y0 + 3 + (count - 1 - i) * lineHeight;
        snprintf(line, sizeof(line), "%.*s", lengths[i], starts[i]);
        text(font, 12, y, line, newest[i] ? WHITE : DIM, 1);
    }
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
            slab(8, y0 + 2, 16 + text_width(FONT_FUN, "CONTROLS") + 13, y0 + 22, 5, red, orange);
        }
        text(FONT_FUN, sStyle == 2 ? 16 : 12, y0 + (sStyle == 2 ? 5 : 4), "CONTROLS", WHITE, 1);
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
            text_centred(FONT_SMALL, x, x + w, y + 1, keys[i], ink, 0);
            text(FONT_SMALL, x + w + 6, y + 1, what[i], WHITE, 1);
            continue;
        }
        text(FONT_SMALL, x, y, keys[i], YELLOW, 1);
        text(FONT_SMALL, x + keyWidth[i % 2], y, what[i], WHITE, 1);
    }
}

static void draw_logo(void) {
    if (sLogo.rgba != NULL) {
        picture(&sLogo, (BS_WIDTH - 160) / 2, sStyle == 2 ? 7 : 3); /* Broadcast's top stripe needs room */
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

static void draw_home(const State *s) {
    if (s->overlayLines > 0 && s->graphCount > 0) {
        // The live monitor: the frame graph over the profiler's page.
        draw_graph(s);
        draw_overlay(s, GRAPH_Y1 + 2, BS_BUTTONS_Y);
        return;
    }
    draw_logo();
    if (s->overlayLines > 0) {
        draw_messages(s, 70, 98);
        draw_overlay(s, 100, BS_BUTTONS_Y);
    } else if (sLogo.rgba == NULL) {
        draw_messages(s, 70, BS_BUTTONS_Y - 1); // No ROM yet: what is happening, or why it can't start.
    } else {
        // The controls card fills the screen under the logo; a debug build keeps the newest message on its last line.
        draw_controls(72, BS_BUTTONS_Y - 4);
        if (PLAT_DEBUG_TOOLS && font_ready(FONT_SMALL)) {
            text_clipped(FONT_SMALL, 12, BS_BUTTONS_Y - 18, newest_message(s), DIM, 1, BS_WIDTH - 12);
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
    box(x0, y, x1, y + 20, (Colour){ 0, 0, 20, 110 }, (Colour){ 140, 180, 255, 110 });
    arrow(1, x0 + 4, arrowY);
    arrow(0, x1 - 4 - arrowWidth, arrowY);
    text_centred(FONT_SMALL, x0 + arrowWidth + 6, x1 - arrowWidth - 6, y + 6, value, YELLOW, 1);
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

static void draw_menu(const State *s) {
    const BsMenu *m = &s->menu;
    int i, tabHeight = BS_MENU_TAB_HEIGHT(m->tabCount);
    const char *tip = m->note;

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
        } else if (!draw_on_off(row->value, MENU_PANEL_X1 - 48, y + 3)) {
            stepper(BS_MENU_VALUE_X, MENU_PANEL_X1 - 8, y + 3, row->value);
        }
        text_clipped(FONT_SMALL, MENU_PANEL_X0 + 14, y + 8, row->label, WHITE, 1, labelEnd);
    }
    fill(MENU_PANEL_X0 + 8, MENU_TIP_Y - 1, MENU_PANEL_X1 - 6, MENU_TIP_Y, (Colour){ 140, 180, 255, 80 });
    text_clipped(FONT_SMALL, MENU_PANEL_X0 + 12, MENU_TIP_Y + 11, tip, HINT, 1, MENU_PANEL_X1 - 6);
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

int bs_menu_hit(int x, int y, int tabCount, int *index, int *stepBack) {
    *index = 0;
    *stepBack = 0;
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
    Colour red = { 235, 40, 40, 255 }, orange = { 255, 150, 20, 255 };
    for (i = 0; i < m->tabCount && i < BS_MENU_TABS; i++) {
        int row = i / perRow, col = i % perRow, x0 = 6 + col * w, y0 = STRIP_TAB_Y0 + row * STRIP_TAB_PITCH;
        int lit = i == m->tab;
        char upper[40];
        Colour ink = lit ? (sStyle == 3 ? (Colour){ 60, 30, 0, 255 } : WHITE) : (Colour){ 190, 205, 240, 255 };
        if (sStyle == 2) {
            if (lit) {
                slab(x0, y0, x0 + w - 2, y0 + STRIP_TAB_H, 5, red, orange);
            } else {
                slab(x0, y0, x0 + w - 2, y0 + STRIP_TAB_H, 5, (Colour){ 30, 44, 110, 235 }, (Colour){ 22, 32, 92, 235 });
            }
        } else if (lit) {
            smooth_box(x0, y0, x0 + w - 2, y0 + STRIP_TAB_H, 10, (Colour){ 255, 226, 90, 255 }, (Colour){ 235, 150, 10, 255 },
                       (Colour){ 255, 255, 255, 150 }, 1);
        }
        upper_case(port_text(m->tabs[i]), upper, sizeof(upper));
        text_centred_as_is(FONT_SMALL, x0, x0 + w - 2, y0 + 6, sStyle == 2 ? upper : port_text(m->tabs[i]), ink, !lit);
    }
    for (i = 0; i < m->rowCount && i < BS_MENU_ROWS; i++) {
        const BsMenuRow *row = &m->rows[i];
        int y = STRIP_ROW_Y0 + i * STRIP_ROW_H, labelEnd = BS_WIDTH - 10, hot = i == m->picked;
        Colour ink = WHITE;
        if (hot) {
            if (sStyle == 2) {
                slab(6, y, 314, y + STRIP_ROW_H - 2, 5, (Colour){ 255, 240, 220, 245 }, (Colour){ 255, 220, 170, 245 });
                ink = (Colour){ 20, 20, 50, 255 };
            } else {
                smooth_box(6, y, 314, y + STRIP_ROW_H - 2, 10, (Colour){ 255, 255, 255, 40 }, (Colour){ 255, 255, 255, 40 },
                           PICKED_EDGE, 1);
            }
            if (row->hint[0] != '\0') {
                tip = row->hint;
            }
        } else if (sStyle == 2) {
            slab(6, y, 314, y + STRIP_ROW_H - 2, 5, (Colour){ 26, 38, 96, 235 }, (Colour){ 26, 38, 96, 235 });
        } else {
            fill(18, y + STRIP_ROW_H - 2, 302, y + STRIP_ROW_H - 1, (Colour){ 255, 255, 255, 30 });
        }
        if (row->value[0] == '\0') {
            arrow(0, 296, y + 4);
            labelEnd = 290;
        } else if (!draw_on_off(row->value, BS_WIDTH - 52, y)) {
            stepper(BS_MENU_VALUE_X, BS_WIDTH - 10, y, row->value);
        }
        text_clipped(FONT_SMALL, 18, y + 5, row->label, ink, sStyle != 2 || !hot, labelEnd);
    }
    fill(10, STRIP_TIP_Y - 1, BS_WIDTH - 10, STRIP_TIP_Y, (Colour){ 255, 255, 255, 40 });
    text_clipped(FONT_SMALL, 14, STRIP_TIP_Y + 9, tip, HINT, 1, m->pages[0] != '\0' ? BS_WIDTH - 50 : BS_WIDTH - 10);
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
static void draw_buttons_styled(const char (*buttons)[40], int count) {
    int i, cell = count > 1 ? 40 / count : 40;
    int y0 = BS_BUTTONS_Y + 2, y1 = BS_STATUS_Y - 3;

    if (sStyle == 3) {
        fill(0, BS_BUTTONS_Y, BS_WIDTH, BS_STATUS_Y, (Colour){ 8, 16, 60, 235 });
        fill(0, BS_BUTTONS_Y, BS_WIDTH, BS_BUTTONS_Y + 1, (Colour){ 255, 255, 255, 60 });
    } else if (sStyle == 2) {
        fill(0, BS_BUTTONS_Y, BS_WIDTH, BS_STATUS_Y, (Colour){ 6, 9, 30, 255 });
    }
    for (i = 0; i < count; i++) {
        const char *label = buttons[i];
        int grey = label[0] == '-', on = label[0] == '+', x0 = (i * cell + 1) * 8, x1 = (i + 1) * cell * 8;
        const char *t = label + (grey || on);
        char upper[40];
        Colour ink = grey ? (Colour){ 130, 135, 160, 255 } : WHITE;
        if (count == 1 || x1 > BS_WIDTH - 8) {
            x1 = BS_WIDTH - 8;
        }
        if (grey && label[1] == '\0') {
            continue;
        }
        if (sStyle == 1) {
            Colour top = on ? (Colour){ 110, 230, 110, 255 } : (Colour){ 90, 160, 255, 255 };
            Colour bottom = on ? (Colour){ 30, 150, 40, 255 } : (Colour){ 30, 80, 220, 255 };
            if (grey) {
                top = (Colour){ 130, 135, 155, 255 };
                bottom = (Colour){ 80, 85, 105, 255 };
            }
            smooth_box(x0, y0, x1, y1, 11, top, bottom, (Colour){ 255, 255, 255, grey ? 80 : 150 }, 1);
        } else if (sStyle == 2) {
            Colour top = i == 0 && !grey ? (Colour){ 235, 40, 40, 255 } : (Colour){ 52, 74, 168, 255 };
            Colour bottom = i == 0 && !grey ? (Colour){ 255, 150, 20, 255 } : (Colour){ 36, 54, 134, 255 };
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
                int w2 = text_width(FONT_SMALL, t), iconW = 18;
                int tx = x0 + (x1 - x0 - w2 - iconW) / 2;
                if (!draw_icon(t, tx + 7, cy, dockInk, (Colour){ 8, 16, 60, 255 })) {
                    iconW = 0;
                    tx = x0 + (x1 - x0 - w2) / 2;
                }
                text_clipped(FONT_SMALL, tx + iconW, cy - 5, t, dockInk, 0, x1);
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
    int i, cell = count > 1 ? 40 / count : 40;

    if (sStyle != 0) {
        draw_buttons_styled(buttons, count);
        return;
    }
    for (i = 0; i < count; i++) {
        const char *label = buttons[i];
        int grey = label[0] == '-', on = label[0] == '+', x0 = (i * cell + 1) * 8, x1 = (i + 1) * cell * 8, y, row;
        Colour top = { 70, 130, 255, 255 }, bottom = { 20, 50, 170, 255 }, edge = { 255, 214, 40, 255 };
        if (count == 1 || x1 > BS_WIDTH - 8) {
            x1 = BS_WIDTH - 8;
        }
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
#define RACE_ROWS_Y 200
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
static Picture sGhostIcon, sTimesIcon, sEggIcon, sEgg, sCoin, sBalloon, sCross, sSmallBanana, sTinyBanana;
static Picture sTrophy[2], sKey[2], sTTAmulet[5], sWizpigAmulet[5]; /* [not collected, collected]; by pieces. */

/* The minimap's colour of each character (game_ui.c, gHudMinimapColours). */
static const Colour sCharacterColours[10] = {
    { 255, 160, 0, 255 }, { 255, 255, 0, 255 }, { 0, 128, 255, 255 },   { 255, 255, 255, 255 }, { 0, 175, 0, 255 },
    { 0, 255, 128, 255 }, { 255, 0, 0, 255 },   { 255, 128, 190, 255 }, { 128, 128, 128, 255 }, { 0, 0, 255, 255 },
};

static const Colour GOLD = { 255, 214, 40, 255 };
static const Colour AHEAD = { 255, 150, 140, 255 };
static const Colour BEHIND = { 140, 255, 160, 255 };
static const Colour LABEL_BLUE = { 170, 195, 240, 255 };

/* The track's map, drawn smooth at the size its panel allows, with its offsets, for the sprite in sMapSprite. */
static Picture sMap;
static int sMapSprite = -2, sMapMirrored, sMapX, sMapY, sMapTexX, sMapTexY, sMapTexWidth;
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
    if (load_texture(ASSET_TEX2D_HUD_SILVERCOINCOLLECTED, &sCoin)) {
        crop(&sCoin);
        narrow(&sCoin);
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

static int piece(const Picture *p, int x, int y, int gap) {
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

/* The track's map from the ROM: its sprite gives the texture and where the texture sits in the sprite. */
static void load_map(int sprite, int mirrored, const unsigned char *colour, const int *box) {
    const unsigned char *table, *data;
    unsigned long tableSize, dataSize, start;
    Picture tex = { 0, 0, NULL };
    int texture, outW, outH, x, y, pad = 3;
    unsigned char *alpha = NULL;

    free(sMap.rgba);
    sMap.rgba = NULL;
    sMapSprite = sprite;
    sMapMirrored = mirrored;
    memcpy(sMapColour, colour, 3);
    memcpy(sMapBox, box, sizeof(sMapBox));
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
            // A sharp edge: the soft alpha is stretched so the edge is about one pixel wide.
            a = (int) ((sum[3] - 70) * 255 / 80);
            a = a < 0 ? 0 : a > 255 ? 255 : a;
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
    // A dark rim, two pixels wide, so the track stands off the panel.
    for (y = 0; y < outH; y++) {
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
            rim = rim * 200 / 255;
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
    sMapX = box[0] + (box[2] - box[0] - outW) / 2;
    sMapY = box[1] + (box[3] - box[1] - outH) / 2;
    free(alpha);
    free(tex.rgba);
}

/*
 * Where a racer's map position (race_hud.h: sprite pixels times 4) is on the screen. The position is off by about a
 * pixel from where the game's marker sprites show (their anchor): moved back by this much, racers sit on the track
 * all the way round (measured on PC on two tracks).
 */
static void map_point(const RaceHudPoint *r, int *x, int *y) {
    float u = r->mapX / 4.0f - sMapTexX - 1.5f, v = r->mapY / 4.0f - sMapTexY - 1.0f;
    if (sMapMirrored) {
        u = sMapTexWidth - u;
    }
    *x = sMapX + 3 + (int) (u * sMapScale);
    *y = sMapY + 3 + (int) (v * sMapScale);
}

static void dot(int cx, int cy, Colour c) {
    int x, y;
    for (y = -3; y <= 3; y++) {
        for (x = -3; x <= 3; x++) {
            int d = x * x + y * y;
            if (d <= 6) {
                plot(cx + x, cy + y, c.r, c.g, c.b, 255);
            } else if (d <= 11) {
                plot(cx + x, cy + y, 0, 4, 30, 220);
            }
        }
    }
    plot(cx - 1, cy - 1, 255, 255, 255, 170);
}

/* The player's arrow: the HUD's gold, pointing along heading (0 up, counter-clockwise). */
static void arrow_marker(int cx, int cy, unsigned heading) {
    static const float sShape[3][2] = { { 0, -7 }, { 5, 5 }, { -5, 5 } };
    float angle = heading * (6.2831853f / 65536.0f), c = cosf(angle), s = sinf(angle), p[3][2];
    int i, x, y, pass;
    for (i = 0; i < 3; i++) {
        // Counter-clockwise on a screen whose y grows downwards.
        p[i][0] = sShape[i][0] * c + sShape[i][1] * s;
        p[i][1] = -sShape[i][0] * s + sShape[i][1] * c;
    }
    for (pass = 0; pass < 2; pass++) {
        // An outline (the triangle one pixel bigger, dark), then the gold.
        float grow = pass == 0 ? 1.25f : 1.0f;
        for (y = -9; y <= 9; y++) {
            for (x = -9; x <= 9; x++) {
                int inside = 1;
                for (i = 0; i < 3 && inside; i++) {
                    const float *a = p[i], *b = p[(i + 1) % 3];
                    float cross = (b[0] - a[0]) * (y / grow - a[1]) - (b[1] - a[1]) * (x / grow - a[0]);
                    inside = cross >= 0;
                }
                if (inside) {
                    if (pass == 0) {
                        plot(cx + x, cy + y, 60, 20, 0, 230);
                    } else {
                        plot(cx + x, cy + y, y < 0 ? 255 : 240, y < 0 ? 236 : 190, y < 0 ? 90 : 20, 255);
                    }
                }
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

/* The parts that only change with the track or the mode: the background, the panels and the map. */
static int sRaceBaseMode = -1;
static void make_race_base(const RaceHud *hud) {
    unsigned short *image = sImage;
    int top = sClipTop, end = sClipEnd, mapBox[4], sprite = hud->mapSprite;
    unsigned char colour[3];
    memcpy(colour, hud->mapColour, 3);
    if (hud->mode == RACE_HUD_MODE_MENU_TRACK || (sprite < 0 && hud->level > 0)) {
        // (A race's own level is only set by the host preview, which has no track model to take the map from.)
        sprite = level_map(hud->level, colour);
    }
    map_box(hud->mode, mapBox);
    if (own_race_layout(hud)) {
        // Dock's map: the big one on the left. Glass and Broadcast have none.
        mapBox[0] = 6;
        mapBox[1] = 52;
        mapBox[2] = 150;
        mapBox[3] = 198;
    }
    if (sprite != sMapSprite || hud->mapMirrored != sMapMirrored || memcmp(colour, sMapColour, 3) ||
        memcmp(mapBox, sMapBox, sizeof(mapBox))) {
        load_map(sprite, hud->mapMirrored, colour, mapBox);
    } else if (sRaceBaseMade && sRaceBaseMode == hud->mode) {
        return;
    }
    sImage = sRaceBase;
    sClipTop = 0;
    sClipEnd = RACE_ROWS_Y;
    memcpy(sRaceBase, sBackground, sizeof(sRaceBase));
    if (hud->mode == RACE_HUD_MODE_MENU_TRACK) {
        box(MENU_MAP_X0, MENU_PANEL_Y0, MENU_MAP_X1, MENU_PANEL_Y1, BOX_FILL, BOX_EDGE);
        box(MENU_RECORDS_X0, MENU_PANEL_Y0, MENU_RECORDS_X1, MENU_PANEL_Y1, BOX_FILL, BOX_EDGE);
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
    } else if (own_race_layout(hud)) {
        // The look draws its own panels each frame.
    } else {
        box(TOWER_X0, UPPER_Y0, TOWER_X1, LOWER_Y1, BOX_FILL, BOX_EDGE);
        box(MIDDLE_X0, UPPER_Y0, MIDDLE_X1, UPPER_Y1, BOX_FILL, BOX_EDGE);
        box(RIGHT_X0, UPPER_Y0, RIGHT_X1, UPPER_Y1, BOX_FILL, BOX_EDGE);
        box(MIDDLE_X0, LOWER_Y0, MIDDLE_X1, LOWER_Y1, BOX_FILL, BOX_EDGE);
        box(RIGHT_X0, LOWER_Y0, RIGHT_X1, LOWER_Y1, BOX_FILL, BOX_EDGE);
    }
    if (sMap.rgba != NULL && !(own_race_layout(hud) && sStyle != 3)) {
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
            text_clipped(FONT_SMALL, 52, y + 6, gap, r->gap > 0 ? AHEAD : BEHIND, 1, TOWER_X1 - 2);
        }
    }
}

static void draw_item(const RaceHud *hud) {
    int index = hud->item * 3 + hud->itemLevel;
    // The slot, then the item at the N64's own size (40 pixels, as sharp as the art is) in it.
    box(117, 24, 165, 72, (Colour){ 0, 0, 20, 60 }, (Colour){ 140, 180, 255, 70 });
    if (hud->item < 0 || index < 0 || index >= 15 || sItems[index].rgba == NULL) {
        return;
    }
    picture_scaled(&sItems[index], 121, 28, 1, 1);
    if (hud->itemCount > 1 && hud->itemCount <= 10) {
        piece(&sItemCounts[hud->itemCount - 1], 150, 60, 0);
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

/* The clock's size and where it starts: the big digits when they fit, else the small ones. */
static int time_small(const RaceHud *hud, int y) {
    return DRY(time_row(hud, 0, y, 0)) > RIGHT_X1 - RIGHT_X0 - 8;
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
            dot(x, y, colour);
        }
    }
    for (i = 0; i < hud->racerCount; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        if (r->player && r->at.onMap) {
            map_point(&r->at, &x, &y);
            arrow_marker(x, y, r->at.heading);
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
    text(FONT_SMALL, MIDDLE_X0 + 44, 12, sStyle == 0 ? "Best run" : "Best", HINT, 1);
    if (hud->bestGap == RACE_HUD_NO_GAP) {
        text(FONT_SMALL, MIDDLE_X0 + 44, 24, hud->bestTotal > 0 ? "" : "none yet", DIM, 1);
    } else {
        text(FONT_SMALL, MIDDLE_X0 + 44, 12 + line_pitch(), hud->bestGap <= 0 ? (sStyle == 0 ? "you lead" : "ahead") : "behind",
             hud->bestGap <= 0 ? BEHIND : AHEAD, 1);
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
            text(FONT_SMALL, 26, y + 52, "You", YELLOW, 1);
        } else {
            char gap[16];
            text(FONT_SMALL, 26, y + 52, subtitle, DIM, 1);
            format_gap(r->gap, gap, sizeof(gap));
            text(FONT_SMALL, 26, y + 52 + line_pitch(), gap, r->gap > 0 ? AHEAD : BEHIND, 1);
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
                title = sTiny[v]; /* the look's heading font is wider than the game's */
            }
            text_centred(FONT_FUN, MIDDLE_X0, MIDDLE_X1, 22, title, WHITE, 1);
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
    text_centred(FONT_SMALL, MIDDLE_X0, MIDDLE_X1, LOWER_Y0 + 38, "Silver coins", HINT, 1);
    for (i = 0; i < 8; i++) {
        int cx = MIDDLE_X0 + 8 + (i % 4) * 20, cy = LOWER_Y0 + 56 + (i / 4) * 16;
        if (i < hud->silverCoins) {
            picture_scaled(&sCoin, cx, cy, 1, 1);
        } else {
            picture_dim(&sCoin, cx, cy);
        }
    }
    snprintf(count, sizeof(count), "%d", hud->silverCoins);
    x = text(FONT_FUN, MIDDLE_X0 + 8, LOWER_Y0 + 82, count, hud->silverCoins >= 8 ? BEHIND : YELLOW, 1);
    text(FONT_SMALL, x + 3, LOWER_Y0 + 85, "/ 8", DIM, 1);
}

/* The four racers of a challenge, with full-size portraits and their score. */
static void draw_four_rows(const RaceHud *hud) {
    int i, k;
    for (i = 0; i < hud->racerCount && i < 4; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        int y = 8 + i * 47, x = 54;
        char score[12];
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
                x = text(FONT_FUN, x, y, score, YELLOW, 1);
                snprintf(score, sizeof(score), "/ %d", hud->goal);
                text(FONT_SMALL, x + 3, y + 3, score, DIM, 1);
                for (k = 0; k < 3; k++) {
                    if (k < r->score) {
                        picture_scaled(&sEggIcon, 54 + k * 12, y + 20, 1, 1);
                    } else {
                        picture_dim(&sEggIcon, 54 + k * 12, y + 20);
                    }
                }
                if (r->carrying) {
                    text(FONT_SMALL, 54, y + 33, sStyle == 0 ? "+1 held" : "+1", YELLOW, 1);
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
        if (me != NULL && me->carrying) {
            picture_scaled(&sEgg, MIDDLE_X0 + 8, LOWER_Y0 + 20, 1, 1);
            text(FONT_SMALL, MIDDLE_X0 + 46, LOWER_Y0 + 26, "Get it", YELLOW, 1);
            text(FONT_SMALL, MIDDLE_X0 + 46, LOWER_Y0 + 26 + line_pitch(), "to your", YELLOW, 1);
            text(FONT_SMALL, MIDDLE_X0 + 46, LOWER_Y0 + 26 + 2 * line_pitch(), "nest", YELLOW, 1);
        } else {
            text(FONT_SMALL, MIDDLE_X0 + 8, LOWER_Y0 + 26, "No egg", DIM, 1);
        }
        text_centred(FONT_SMALL, MIDDLE_X0, MIDDLE_X1, LOWER_Y0 + 70, "Drop it if hit", DIM, 1);
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
                title = "EGGS"; /* the look's heading font is wider than the game's */
            }
            text_centred(FONT_FUN, RIGHT_X0, RIGHT_X1, 9, title, WHITE, 1);
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
        {
            char name[24];
            char *space;
            snprintf(name, sizeof(name), "%s", sWorldShortNames[i]);
            space = strrchr(name, ' ');
            if (space != NULL && text_width(FONT_SMALL, name) > TOWER_X1 - TOWER_X0 - 8) {
                *space = '\0'; /* the look's font is wider: "Snowflake Mtn" becomes "Snowflake" */
            }
            text(FONT_SMALL, TOWER_X0 + 5, y, name, balloons > 0 ? WHITE : DIM, 1);
        }
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
            text(FONT_SMALL, 252, y + 2, "cleared", BEHIND, 1);
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
        slab(94, 8, 94 + 8 + text_width(FONT_FUN, hud->adventureTwo ? "ADVENTURE TWO" : "ADVENTURE") + 13, 28, 5, red, orange);
        text(FONT_FUN, 102, 11, hud->adventureTwo ? "ADVENTURE TWO" : "ADVENTURE", WHITE, 1);
    } else {
        text(FONT_FUN, 96, 9, hud->adventureTwo ? "ADVENTURE TWO" : "ADVENTURE", sStyle == 1 || sStyle == 3 ? GOLD : WHITE, 1);
    }
    snprintf(line, sizeof(line), port_text("Save file %s"), hud->name);
    text(FONT_SMALL, sStyle == 2 ? 100 : 98, sStyle == 2 ? 32 : 28, line, WHITE, 1);
    if (sStyle == 3) {
        // Rings: the balloons, then the two amulets.
        progress_ring(56, 104, 40, 6, hud->balloons * 256 / 47, GOLD);
        picture_scaled(&sBalloon, 42, 76, 1, 1);
        fraction_centred(16, 96, 111, hud->balloons, 47);
        progress_ring(30, 168, 15, 4, hud->ttAmulet * 256 / 4, (Colour){ 180, 230, 255, 255 });
        if (hud->ttAmulet >= 0 && hud->ttAmulet <= 4) {
            picture(&sTTAmulet[hud->ttAmulet], 19, 157);
        }
        fraction_centred(6, 54, 187, hud->ttAmulet, 4);
        progress_ring(82, 168, 15, 4, hud->wizpigAmulet * 256 / 4, (Colour){ 180, 230, 255, 255 });
        if (hud->wizpigAmulet >= 0 && hud->wizpigAmulet <= 4) {
            picture(&sWizpigAmulet[hud->wizpigAmulet], 71, 157);
        }
        fraction_centred(58, 106, 187, hud->wizpigAmulet, 4);
        for (i = 0; i < RACE_HUD_WORLDS; i++) {
            int y = 56 + i * 28, balloons = hud->worldBalloons[i];
            Colour ink = balloons > 0 ? WHITE : DIM;
            text(FONT_SMALL, 118, y, sWorldShortNames[i], ink, 1);
            snprintf(line, sizeof(line), "%d/8", balloons);
            text(FONT_SMALL, 300 - text_width(FONT_SMALL, line), y, line, ink, 1);
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
            picture_fit(icon, gx, 54, 28, 28);
            fraction_at(gx + 36, 54, value, i == 0 ? 47 : 4);
            text(FONT_SMALL, gx + 36, 71, name, LABEL_BLUE, 1);
        }
    }
    for (i = 0; i < RACE_HUD_WORLDS; i++) {
        int y = sStyle == 2 ? 92 + i * 21 : 96 + i * 20, balloons = hud->worldBalloons[i], k;
        Colour ink = balloons > 0 ? WHITE : DIM;
        if (sStyle == 2) {
            slab(8, y, 312, y + 19, 4, slabFill, slabFill);
            for (k = 0; k < 8; k++) {
                slab(132 + k * 13, y + 6, 132 + k * 13 + 10, y + 13, 3, k < balloons ? red : (Colour){ 255, 255, 255, 30 },
                     k < balloons ? orange : (Colour){ 255, 255, 255, 30 });
            }
            text(FONT_SMALL, 18, y + 4, sWorldShortNames[i], ink, 1);
        } else {
            text(FONT_SMALL, 18, y + 2, sWorldShortNames[i], ink, 1);
            smooth_box(112, y + 5, 224, y + 10, 2, (Colour){ 0, 0, 0, 90 }, (Colour){ 0, 0, 0, 90 }, (Colour){ 0, 0, 0, 0 }, 1);
            if (balloons > 0) {
                smooth_box(112, y + 5, 112 + 112 * balloons / 8, y + 10, 2, (Colour){ 255, 236, 110, 255 },
                           (Colour){ 240, 160, 20, 255 }, (Colour){ 0, 0, 0, 0 }, 1);
            }
            snprintf(line, sizeof(line), "%d/8", balloons);
            text(FONT_SMALL, 230, y + 2, line, ink, 1);
        }
        trophy_and_key(hud, i, sStyle == 2 ? 236 : 262, y + (sStyle == 2 ? 1 : -1), 16);
        if (sStyle == 2) {
            if (hud->worldBosses & (1 << i)) {
                text(FONT_SMALL, 274, y + 4, "DONE", BEHIND, 1);
            } else {
                snprintf(line, sizeof(line), "%d/8", balloons);
                text(FONT_SMALL, 276, y + 4, line, ink, 1);
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
        text(FONT_SMALL, x0, MENU_PANEL_Y0 + 72, sVehicles[hud->vehicle], DIM, 1);
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
        return;
    }
    picture_scaled(&sItems[index], x, y, 1, 1);
    if (hud->itemCount > 1 && hud->itemCount <= 10) {
        piece(&sItemCounts[hud->itemCount - 1], countX, countY, 0);
    }
}

/* A picture shrunk or grown to w by h (nearest pixel) at x, y. */
static void picture_fit(const Picture *p, int x, int y, int w, int h) {
    int i, j;
    if (p->rgba == NULL || w <= 0 || h <= 0) {
        return;
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
    if (rightAligned) {
        x -= text_width(FONT_SMALL, gap);
    }
    text(FONT_SMALL, x, y, gap, r->gap > 0 ? AHEAD : BEHIND, 1);
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
                race_time(dx, y - 2, values[k], 1);
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
        }
    }
    text_centred(FONT_SMALL, 120, 196, 178, "Item", LABEL_BLUE, 1);
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
            text(FONT_SMALL, 56, y + 5, racer_code(r), (Colour){ 20, 20, 50, 255 }, 0);
            text(FONT_SMALL, 120 - text_width(FONT_SMALL, "YOU"), y + 5, "YOU", (Colour){ 200, 40, 30, 255 }, 0);
        } else {
            text(FONT_SMALL, 56, y + 5, racer_code(r), WHITE, 1);
            gap_text(122, y + 5, 1, r);
        }
    }
    // The place block, the lap slab and the time strip.
    slab(144, 8, 232, 64, 10, red, orange);
    place_centred(hud, 150, 8, 224, 64, 3);
    slab(226, 8, 316, 64, 6, slabLight, slabLight);
    if (hud->laps > 1 && hud->lap >= hud->laps && !hud->finished) {
        text_centred(FONT_FUN, 226, 316, 14, "FINAL LAP", WHITE, 1);
    } else {
        lap_pieces(hud, 240, 12);
    }
    lap_segments(hud, 240, 34, 20, 3, 6, red, (Colour){ 255, 255, 255, 40 }, 1);
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
            int y = 70 + row * 14;
            text(FONT_SMALL, 146, y, names[k], LABEL_BLUE, 1);
            if (shown[k]) {
                race_time(dx, y, values[k], 1);
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
        text(FONT_FUN, 92, 8, "FINAL LAP", GOLD, 1);
    } else {
        lap_pieces(hud, 92, 6);
    }
    lap_segments(hud, 94, 25, 8, 4, 8, GOLD, (Colour){ 255, 255, 255, 50 }, 0);
    lap_facts(hud, &lapNumber, &lapTime, &best);
    {
        // Total on top, Current below: the same label font and digit size, digits in one column.
        int totalW = text_width(FONT_SMALL, "Total"), currentW = text_width(FONT_SMALL, "Current");
        int dx = 176 + (totalW > currentW ? totalW : currentW) + 6;
        text(FONT_SMALL, 176, 8, "Total", LABEL_BLUE, 1);
        race_time(dx, 8, hud->raceTime, 1);
        if (lapNumber > 0) {
            text(FONT_SMALL, 176, 26, "Current", LABEL_BLUE, 1);
            race_time(dx, 26, lapTime, 1);
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


static void draw_race(const State *s) {
    const RaceHud *hud = &s->race;
    make_race_base(hud);
    memcpy(sImage, sRaceBase, sizeof(sRaceBase));
    switch (hud->mode) {
        case RACE_HUD_MODE_TIME_TRIAL:
            draw_time_trial(hud);
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
            draw_menu_save(hud);
            return;
        case RACE_HUD_MODE_MENU_TRACK:
            draw_menu_track(hud);
            return;
        default:
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
    // The kart, with fading copies behind it for motion.
    if (sSetupFace.rgba != NULL) {
        int size = (int) (13 * unit), kx = cx - size / 2 + (int) (sc.kartOffset * unit), ky = cy + (int) (2 * unit);
        for (i = sc.ghosts; i > 0; i--) {
            picture_stretched(&sSetupFace, kx - i * size / 3, ky, size, size, r, 70 / i);
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
        case SETUP_PAGE_STYLE:
            if (i == SETUP_STYLE_CLASSIC) {
                plain.hud = 1;
                draw_screen_style(r, 0, plain);
            } else {
                plain.ghosts = i == SETUP_STYLE_SPEEDY ? 2 : 0;
                draw_screen_style(r, 2, plain);
            }
            break;
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
    static const int sRows[BS_AREA_COUNT + 1] = { 0, BS_BUTTONS_Y, BS_STATUS_Y, BS_HEIGHT };
    *first = sRows[area];
    *end = sRows[area + 1];
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
