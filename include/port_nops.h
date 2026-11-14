/*
 * Native renderer ops (PICA_ALL test build, PORT_NATIVE_OPS): the game's draw-state calls write the port renderer's
 * own ops instead of N64 RDP/RSP commands.
 *
 * The game still records a frame for the renderer, which runs on the other core while the game builds the next one,
 * so the calls below are recorded as ops in that frame. Each op is one 64-bit word, so every call site writes the
 * same number of words or fewer: w0 = 0xC0 (the N64's no-op, so an old-format reader skips it) | NOP_MARK | id << 19
 * | 19 bits, w1 = 32 bits. The renderer (gfx_rdp.c native_op) applies each op to its draw state directly.
 *
 * Syncs (pipe, load, tile, full) write nothing: the renderer has no pipeline to wait for.
 * Combiner and other-mode pairs, which need 56 bits, are kept once each in a table (port_nop_intern) and the op
 * names the entry.
 *
 * gPortNativeOps is the switch, read by the game at each level load (with native draws, port_native_draw): with it
 * off every call writes the N64 command as before, and the renderer reads both kinds, so a frame may hold either.
 * The game's constant lists (gs* macros) are N64 data; see port_nop_static in gfx_rdp.c.
 */
#ifndef PORT_NOPS_H
#define PORT_NOPS_H
#if defined(PORT_NATIVE_OPS) && defined(_LANGUAGE_C)

extern int gPortNativeOps;
unsigned int port_nop_intern(int table, unsigned int a, unsigned int b);

/* The walk census (DKR_WALK_CENSUS, host builds only: PORT_CENSUS_SITES): the commands the renderer still walks are
 * named by the source line that wrote them, see port/host/census_sites.c. Nothing on the 3DS. */
#ifdef PORT_CENSUS_SITES
extern int gPortCensusSites;
void port_census_site(const void *cmd, const char *file, int line);
#define PORT_CENSUS_SITE(cmd)                                                                                          \
    if (gPortCensusSites)                                                                                              \
    port_census_site((cmd), __FILE__, __LINE__)
#else
#define PORT_CENSUS_SITE(cmd) ((void) 0)
#endif

#define NOP_MARK 0x00800000u
#define NOP_TAG 0xC0000000u
enum {
    NOP_GEOM_SET,
    NOP_GEOM_CLR,
    NOP_OMH,     /* 19 bits: shift << 8 | length; w1: the bits */
    NOP_OML,
    NOP_OMALL,   /* w1: table entry (H, L) */
    NOP_COMBINE, /* w1: table entry (the combiner's two words) */
    NOP_PRIM,    /* 19 bits: min level << 8 | lod fraction; w1: RGBA */
    NOP_COLOR,   /* 19 bits: 0 environment, 1 blend, 2 fog, 3 fill; w1: the colour */
    NOP_SCISSOR, /* quarter pixels: 19 bits: mode << 17 | ulx << 5 | uly >> 7; w1: (uly & 127) << 25 | lrx << 12 | lry */
    NOP_FILLRECT, /* pixels: 19 bits: lry; w1: ulx << 20 | uly << 10 | lrx */
    NOP_IMAGE,   /* 19 bits: kind << 17 (0 colour, 1 depth, 2 texture) | fmt << 14 | siz << 12 | width - 1; w1: address */
    NOP_TILE,    /* 19 bits: cmt << 17 | tile << 14 | line << 5 | siz << 3 | fmt;
                    w1: shifts << 27 | masks << 23 | cms << 21 | shiftt << 17 | maskt << 13 | palette << 9 | tmem */
    NOP_TILESIZE, /* 19 bits: tile << 16 | s0 << 4 | t0 >> 8; w1: (t0 & 255) << 24 | s1 << 12 | t1 */
    NOP_LOADTILE, /* as NOP_TILESIZE */
    NOP_LOADBLOCK, /* as NOP_TILESIZE, with s0, t0, the last texel and dxt */
    NOP_EXT,     /* 19 bits: sub-op << 15 | 15 bits (below) */
    NOP_COUNT
};
/* The NOP_EXT ops (sub-op in bits 15-18 of the 19). */
enum {
    NOPX_LOADTLUT, /* bits: tile; w1: count */
    NOPX_CALL,     /* bits: branch (no return) << 14 | counted << 13 | count; w1: the list */
    NOPX_END,
    NOPX_VTX,      /* bits: (count - 1) << 3 | append; w1: the vertices */
    NOPX_TRI,      /* bits: textured << 12 | count; w1: the triangles */
    NOPX_VIEWPORT, /* w1: the Vp */
    NOPX_FOG,      /* w1: multiplier << 16 | offset */
    NOPX_BILLBOARD, /* w1: on */
    NOPX_MVP,      /* bits: matrix index */
    NOPX_MTX,      /* bits: matrix index; w1: the Mtx */
    NOPX_TEXRECT,  /* three words (the op, then two NOPX_DATA), packed as gSPTextureRectangle below */
    NOPX_DATA,
    NOPX_COUNT
};
#define NOPX_W0(sub, bits) NOP_W0(NOP_EXT, ((unsigned int) (sub) << 15) | ((unsigned int) (bits) & 0x7FFF))
#define NOP_W0(id, bits) (NOP_TAG | NOP_MARK | ((unsigned int) (id) << 19) | ((unsigned int) (bits) & 0x7FFFF))
#define NOP_PUT(pkt, id, bits, word)                                                                                   \
    {                                                                                                                  \
        Gfx *_n = (Gfx *) (pkt);                                                                                       \
        PORT_CENSUS_SITE(_n);                                                                                          \
        _n->words.w0 = NOP_W0(id, bits);                                                                               \
        _n->words.w1 = (unsigned int) (word);                                                                          \
    }

/* The N64 versions, kept under other names for the switch's off side. */
#define N64_NOPARAM(pkt, cmd)                                                                                          \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        _g->words.w0 = _SHIFTL(cmd, 24, 8);                                                                            \
        _g->words.w1 = 0;                                                                                              \
    }

#undef gDPPipeSync
#undef gDPLoadSync
#undef gDPTileSync
#undef gDPFullSync
#define gDPPipeSync(pkt)                                                                                               \
    if (!gPortNativeOps) N64_NOPARAM(pkt, G_RDPPIPESYNC)
#define gDPLoadSync(pkt)                                                                                               \
    if (!gPortNativeOps) N64_NOPARAM(pkt, G_RDPLOADSYNC)
#define gDPTileSync(pkt)                                                                                               \
    if (!gPortNativeOps) N64_NOPARAM(pkt, G_RDPTILESYNC)
#define gDPFullSync(pkt)                                                                                               \
    if (!gPortNativeOps) N64_NOPARAM(pkt, G_RDPFULLSYNC)

#undef gSPSetGeometryMode
#undef gSPClearGeometryMode
#define gSPSetGeometryMode(pkt, word)                                                                                  \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        _g->words.w0 = gPortNativeOps ? NOP_W0(NOP_GEOM_SET, 0) : _SHIFTL(G_SETGEOMETRYMODE, 24, 8);                   \
        _g->words.w1 = (unsigned int) (word);                                                                          \
    }
#define gSPClearGeometryMode(pkt, word)                                                                                \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        _g->words.w0 = gPortNativeOps ? NOP_W0(NOP_GEOM_CLR, 0) : _SHIFTL(G_CLEARGEOMETRYMODE, 24, 8);                 \
        _g->words.w1 = (unsigned int) (word);                                                                          \
    }

#undef gSPSetOtherMode
#define gSPSetOtherMode(pkt, cmd, sft, len, data)                                                                      \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        _g->words.w0 = gPortNativeOps ? NOP_W0(((cmd) == G_SETOTHERMODE_H) ? NOP_OMH : NOP_OML,                        \
                                               _SHIFTL(sft, 8, 8) | _SHIFTL(len, 0, 8))                                \
                                      : (_SHIFTL(cmd, 24, 8) | _SHIFTL(sft, 8, 8) | _SHIFTL(len, 0, 8));               \
        _g->words.w1 = (unsigned int) (data);                                                                          \
    }

#undef gDPSetOtherMode
#define gDPSetOtherMode(pkt, mode0, mode1)                                                                             \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        if (gPortNativeOps) {                                                                                          \
            _g->words.w0 = NOP_W0(NOP_OMALL, 0);                                                                       \
            _g->words.w1 = port_nop_intern(0, _SHIFTL(mode0, 0, 24), (unsigned int) (mode1));                          \
        } else {                                                                                                       \
            _g->words.w0 = _SHIFTL(G_RDPSETOTHERMODE, 24, 8) | _SHIFTL(mode0, 0, 24);                                  \
            _g->words.w1 = (unsigned int) (mode1);                                                                     \
        }                                                                                                              \
    }

#undef gDPSetCombineLERP
#define gDPSetCombineLERP(pkt, a0, b0, c0, d0, Aa0, Ab0, Ac0, Ad0, a1, b1, c1, d1, Aa1, Ab1, Ac1, Ad1)                  \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        unsigned int _c0 =                                                                                             \
            _SHIFTL(GCCc0w0(G_CCMUX_##a0, G_CCMUX_##c0, G_ACMUX_##Aa0, G_ACMUX_##Ac0) | GCCc1w0(G_CCMUX_##a1, G_CCMUX_##c1), \
                    0, 24);                                                                                            \
        unsigned int _c1 = (unsigned int) (GCCc0w1(G_CCMUX_##b0, G_CCMUX_##d0, G_ACMUX_##Ab0, G_ACMUX_##Ad0) |          \
                                           GCCc1w1(G_CCMUX_##b1, G_ACMUX_##Aa1, G_ACMUX_##Ac1, G_CCMUX_##d1,            \
                                                   G_ACMUX_##Ab1, G_ACMUX_##Ad1));                                     \
        if (gPortNativeOps) {                                                                                          \
            _g->words.w0 = NOP_W0(NOP_COMBINE, 0);                                                                     \
            _g->words.w1 = port_nop_intern(1, _c0, _c1);                                                               \
        } else {                                                                                                       \
            _g->words.w0 = _SHIFTL(G_SETCOMBINE, 24, 8) | _c0;                                                         \
            _g->words.w1 = _c1;                                                                                        \
        }                                                                                                              \
    }

#undef gDPSetColor
#define gDPSetColor(pkt, c, d)                                                                                         \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        _g->words.w0 = gPortNativeOps ? NOP_W0(NOP_COLOR, ((c) == G_SETENVCOLOR)     ? 0                               \
                                                          : ((c) == G_SETBLENDCOLOR) ? 1                               \
                                                          : ((c) == G_SETFOGCOLOR)   ? 2                               \
                                                                                     : 3)                              \
                                      : _SHIFTL(c, 24, 8);                                                             \
        _g->words.w1 = (unsigned int) (d);                                                                             \
    }

#undef gDPSetPrimColor
#define gDPSetPrimColor(pkt, m, l, r, g, b, a)                                                                         \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        _g->words.w0 = gPortNativeOps ? NOP_W0(NOP_PRIM, _SHIFTL(m, 8, 8) | _SHIFTL(l, 0, 8))                          \
                                      : (_SHIFTL(G_SETPRIMCOLOR, 24, 8) | _SHIFTL(m, 8, 8) | _SHIFTL(l, 0, 8));        \
        _g->words.w1 = (_SHIFTL(r, 24, 8) | _SHIFTL(g, 16, 8) | _SHIFTL(b, 8, 8) | _SHIFTL(a, 0, 8));                  \
    }
#undef gDPSetPrimColorRGBA
#define gDPSetPrimColorRGBA(pkt, rgba)                                                                                 \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        _g->words.w0 = gPortNativeOps ? NOP_W0(NOP_PRIM, 0) : _SHIFTL(G_SETPRIMCOLOR, 24, 8);                          \
        _g->words.w1 = (rgba);                                                                                         \
    }
#undef gDPSetEnvColorRGBA
#define gDPSetEnvColorRGBA(pkt, rgba)                                                                                  \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        _g->words.w0 = gPortNativeOps ? NOP_W0(NOP_COLOR, 0) : _SHIFTL(G_SETENVCOLOR, 24, 8);                          \
        _g->words.w1 = (rgba);                                                                                         \
    }

#define NOP_SCISSOR_PUT(pkt, mode, x0, y0, x1, y1)                                                                     \
    {                                                                                                                  \
        unsigned int _x0 = (unsigned int) (x0) & 0xFFF, _y0 = (unsigned int) (y0) & 0xFFF;                             \
        unsigned int _x1 = (unsigned int) (x1) & 0xFFF, _y1 = (unsigned int) (y1) & 0xFFF;                             \
        NOP_PUT(pkt, NOP_SCISSOR, ((unsigned int) (mode) & 3) << 17 | _x0 << 5 | _y0 >> 7,                             \
                (_y0 & 127) << 25 | _x1 << 12 | _y1);                                                                  \
    }
#undef gDPSetScissor
#define gDPSetScissor(pkt, mode, ulx, uly, lrx, lry)                                                                   \
    {                                                                                                                  \
        if (gPortNativeOps) {                                                                                          \
            NOP_SCISSOR_PUT(pkt, mode, (int) ((float) (ulx) * 4.0F), (int) ((float) (uly) * 4.0F),                     \
                            (int) ((float) (lrx) * 4.0F), (int) ((float) (lry) * 4.0F))                                \
        } else {                                                                                                       \
            Gfx *_g = (Gfx *) pkt;                                                                                     \
            _g->words.w0 = _SHIFTL(G_SETSCISSOR, 24, 8) | _SHIFTL((int) ((float) (ulx) * 4.0F), 12, 12) |              \
                           _SHIFTL((int) ((float) (uly) * 4.0F), 0, 12);                                               \
            _g->words.w1 = _SHIFTL(mode, 24, 2) | _SHIFTL((int) ((float) (lrx) * 4.0F), 12, 12) |                      \
                           _SHIFTL((int) ((float) (lry) * 4.0F), 0, 12);                                               \
        }                                                                                                              \
    }
#undef gDPSetScissorFrac
#define gDPSetScissorFrac(pkt, mode, ulx, uly, lrx, lry)                                                               \
    {                                                                                                                  \
        if (gPortNativeOps) {                                                                                          \
            NOP_SCISSOR_PUT(pkt, mode, (int) (ulx), (int) (uly), (int) (lrx), (int) (lry))                             \
        } else {                                                                                                       \
            Gfx *_g = (Gfx *) pkt;                                                                                     \
            _g->words.w0 = _SHIFTL(G_SETSCISSOR, 24, 8) | _SHIFTL((int) ((ulx)), 12, 12) | _SHIFTL((int) ((uly)), 0, 12); \
            _g->words.w1 = _SHIFTL(mode, 24, 2) | _SHIFTL((int) ((lrx)), 12, 12) | _SHIFTL((int) ((lry)), 0, 12);      \
        }                                                                                                              \
    }

#undef gDPFillRectangle
#define gDPFillRectangle(pkt, ulx, uly, lrx, lry)                                                                      \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        PORT_CENSUS_SITE(_g); \
        if (gPortNativeOps) {                                                                                          \
            _g->words.w0 = NOP_W0(NOP_FILLRECT, _SHIFTL((lry), 0, 10));                                                \
            _g->words.w1 = _SHIFTL((ulx), 20, 10) | _SHIFTL((uly), 10, 10) | _SHIFTL((lrx), 0, 10);                    \
        } else {                                                                                                       \
            _g->words.w0 = (_SHIFTL(G_FILLRECT, 24, 8) | _SHIFTL((lrx), 14, 10) | _SHIFTL((lry), 2, 10));              \
            _g->words.w1 = (_SHIFTL((ulx), 14, 10) | _SHIFTL((uly), 2, 10));                                           \
        }                                                                                                              \
    }

#undef gSetImage
#define gSetImage(pkt, cmd, fmt, siz, width, i)                                                                        \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        PORT_CENSUS_SITE(_g); \
        _g->words.w0 = gPortNativeOps ? NOP_W0(NOP_IMAGE, (((cmd) == G_SETCIMG) ? 0 : ((cmd) == G_SETZIMG) ? 1 : 2) << 17 | \
                                                              _SHIFTL(fmt, 14, 3) | _SHIFTL(siz, 12, 2) |              \
                                                              _SHIFTL((width) - 1, 0, 12))                             \
                                      : (_SHIFTL(cmd, 24, 8) | _SHIFTL(fmt, 21, 3) | _SHIFTL(siz, 19, 2) |             \
                                         _SHIFTL((width) - 1, 0, 12));                                                 \
        _g->words.w1 = (unsigned int) (i);                                                                             \
    }

#undef gDPSetTile
#define gDPSetTile(pkt, fmt, siz, line, tmem, tile, palette, cmt, maskt, shiftt, cms, masks, shifts)                     \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        PORT_CENSUS_SITE(_g); \
        if (gPortNativeOps) {                                                                                          \
            _g->words.w0 = NOP_W0(NOP_TILE, _SHIFTL(cmt, 17, 2) | _SHIFTL(tile, 14, 3) | _SHIFTL(line, 5, 9) |         \
                                                _SHIFTL(siz, 3, 2) | _SHIFTL(fmt, 0, 3));                              \
            _g->words.w1 = _SHIFTL(shifts, 27, 4) | _SHIFTL(masks, 23, 4) | _SHIFTL(cms, 21, 2) |                      \
                           _SHIFTL(shiftt, 17, 4) | _SHIFTL(maskt, 13, 4) | _SHIFTL(palette, 9, 4) |                   \
                           _SHIFTL(tmem, 0, 9);                                                                        \
        } else {                                                                                                       \
            _g->words.w0 = _SHIFTL(G_SETTILE, 24, 8) | _SHIFTL(fmt, 21, 3) | _SHIFTL(siz, 19, 2) |                     \
                           _SHIFTL(line, 9, 9) | _SHIFTL(tmem, 0, 9);                                                  \
            _g->words.w1 = _SHIFTL(tile, 24, 3) | _SHIFTL(palette, 20, 4) | _SHIFTL(cmt, 18, 2) |                      \
                           _SHIFTL(maskt, 14, 4) | _SHIFTL(shiftt, 10, 4) | _SHIFTL(cms, 8, 2) |                       \
                           _SHIFTL(masks, 4, 4) | _SHIFTL(shifts, 0, 4);                                               \
        }                                                                                                              \
    }

#define NOP_RECT4(id, tile, a, b, c, d)                                                                                \
    NOP_W0(id, _SHIFTL(tile, 16, 3) | _SHIFTL(a, 4, 12) | (((unsigned int) (b) >> 8) & 15))
#define NOP_RECT4_W1(b, c, d) (_SHIFTL(b, 24, 8) | _SHIFTL(c, 12, 12) | _SHIFTL(d, 0, 12))

#undef gDPLoadTileGeneric
#define gDPLoadTileGeneric(pkt, c, tile, uls, ult, lrs, lrt)                                                           \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        PORT_CENSUS_SITE(_g); \
        if (gPortNativeOps) {                                                                                          \
            _g->words.w0 = NOP_RECT4(((c) == G_SETTILESIZE) ? NOP_TILESIZE : NOP_LOADTILE, tile, uls, ult, lrs, lrt);  \
            _g->words.w1 = NOP_RECT4_W1(ult, lrs, lrt);                                                                \
        } else {                                                                                                       \
            _g->words.w0 = _SHIFTL(c, 24, 8) | _SHIFTL(uls, 12, 12) | _SHIFTL(ult, 0, 12);                             \
            _g->words.w1 = _SHIFTL(tile, 24, 3) | _SHIFTL(lrs, 12, 12) | _SHIFTL(lrt, 0, 12);                          \
        }                                                                                                              \
    }

#undef gDPLoadBlock
#define gDPLoadBlock(pkt, tile, uls, ult, lrs, dxt)                                                                    \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        PORT_CENSUS_SITE(_g); \
        if (gPortNativeOps) {                                                                                          \
            _g->words.w0 = NOP_RECT4(NOP_LOADBLOCK, tile, uls, ult, MIN(lrs, G_TX_LDBLK_MAX_TXL), dxt);                \
            _g->words.w1 = NOP_RECT4_W1(ult, MIN(lrs, G_TX_LDBLK_MAX_TXL), dxt);                                       \
        } else {                                                                                                       \
            _g->words.w0 = (_SHIFTL(G_LOADBLOCK, 24, 8) | _SHIFTL(uls, 12, 12) | _SHIFTL(ult, 0, 12));                 \
            _g->words.w1 = (_SHIFTL(tile, 24, 3) | _SHIFTL((MIN(lrs, G_TX_LDBLK_MAX_TXL)), 12, 12) |                   \
                            _SHIFTL(dxt, 0, 12));                                                                      \
        }                                                                                                              \
    }

#undef gDPLoadTLUTCmd
#define gDPLoadTLUTCmd(pkt, tile, count)                                                                               \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) pkt;                                                                                         \
        PORT_CENSUS_SITE(_g); \
        if (gPortNativeOps) {                                                                                          \
            _g->words.w0 = NOPX_W0(NOPX_LOADTLUT, (tile) & 7);                                                           \
            _g->words.w1 = (unsigned int) (count);                                                                     \
        } else {                                                                                                       \
            _g->words.w0 = _SHIFTL(G_LOADTLUT, 24, 8);                                                                 \
            _g->words.w1 = _SHIFTL((tile), 24, 3) | _SHIFTL((count), 14, 10);                                          \
        }                                                                                                              \
    }


/* Lists, vertices, triangles, matrices and the viewport. Segments and the perspective normaliser write nothing: the
 * renderer takes plain addresses and makes its own projection. */
#undef gDma1p
#define gDma1p(pkt, c, s, l, p)                                                                                        \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        PORT_CENSUS_SITE(_g); \
        if (gPortNativeOps && (c) == G_DL) {                                                                           \
            _g->words.w0 = NOPX_W0(NOPX_CALL, ((p) == G_DL_NOPUSH) << 14);                                             \
        } else if (gPortNativeOps && (c) == G_VTX) {                                                                   \
            _g->words.w0 = NOPX_W0(NOPX_VTX, (((p) >> 3) & 31) << 3 | ((p) & 1));                                      \
        } else if (gPortNativeOps && (c) == G_MOVEMEM && (p) == G_MV_VIEWPORT) {                                       \
            _g->words.w0 = NOPX_W0(NOPX_VIEWPORT, 0);                                                                  \
        } else if (gPortNativeOps && (c) == G_MTX) {                                                                   \
            _g->words.w0 = NOPX_W0(NOPX_MTX, ((p) >> 6) & 3);                                                          \
        } else {                                                                                                       \
            _g->words.w0 = (_SHIFTL((c), 24, 8) | _SHIFTL((p), 16, 8) | _SHIFTL((l), 0, 16));                          \
        }                                                                                                              \
        _g->words.w1 = (unsigned int) (s);                                                                             \
    }

#undef gImmp1
#define gImmp1(pkt, c, p0)                                                                                             \
    if (!gPortNativeOps || (c) != G_PERSPNORMALIZE) {                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        _g->words.w0 = _SHIFTL((c), 24, 8);                                                                            \
        _g->words.w1 = (unsigned int) (p0);                                                                            \
    }

#undef gMoveWd
#define gMoveWd(pkt, index, offset, data)                                                                              \
    if (!gPortNativeOps || (index) != G_MW_SEGMENT) {                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        if (gPortNativeOps && (index) == G_MW_FOG) {                                                                   \
            _g->words.w0 = NOPX_W0(NOPX_FOG, 0);                                                                       \
        } else if (gPortNativeOps && (index) == 0x02 /* G_MW_BILLBOARD */) {                                                      \
            _g->words.w0 = NOPX_W0(NOPX_BILLBOARD, 0);                                                                 \
        } else if (gPortNativeOps && (index) == 0x0A /* G_MW_MVPMATRIX */) {                                                      \
            _g->words.w0 = NOPX_W0(NOPX_MVP, ((data) >> 6) & 3);                                                       \
        } else {                                                                                                       \
            _g->words.w0 = _SHIFTL(G_MOVEWORD, 24, 8) | _SHIFTL((offset), 8, 16) | _SHIFTL((index), 0, 8);             \
        }                                                                                                              \
        _g->words.w1 = (unsigned int) (data);                                                                          \
    }

#undef gSPEndDisplayList
#define gSPEndDisplayList(pkt)                                                                                         \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        _g->words.w0 = gPortNativeOps ? NOPX_W0(NOPX_END, 0) : _SHIFTL(G_ENDDL, 24, 8);                                \
        _g->words.w1 = 0;                                                                                              \
    }

#undef gSPPolygon
#define gSPPolygon(dl, ptr, numTris, texEnabled)                                                                       \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (dl);                                                                                        \
        PORT_CENSUS_SITE(_g); \
        _g->words.w0 = gPortNativeOps ? NOPX_W0(NOPX_TRI, ((texEnabled) & 1) << 12 | ((numTris) & 0xFFF))              \
                                      : (_SHIFTL((((numTris) - 1) << 4) | (texEnabled), 16, 8) | _SHIFTL(G_TRIN, 24, 8) | \
                                         _SHIFTL(((numTris) * 16), 0, 16));                                            \
        _g->words.w1 = (unsigned int) (ptr);                                                                           \
    }

#undef gDkrDmaDisplayList
#define gDkrDmaDisplayList(pkt, address, numberOfCommands)                                                             \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        PORT_CENSUS_SITE(_g); \
        _g->words.w0 = gPortNativeOps ? NOPX_W0(NOPX_CALL, 1 << 13 | ((numberOfCommands) & 0xFF))                      \
                                      : (_SHIFTL(G_DMADL, 24, 8) | _SHIFTL((numberOfCommands), 16, 8) |                \
                                         _SHIFTL((numberOfCommands * 8), 0, 16));                                      \
        _g->words.w1 = (unsigned int) (address);                                                                       \
    }


/* A texture rectangle, three words as before: corners (12 bits each, 10.2), tile, flip, s/t (s10.5) and steps (s5.10):
 *   op:    bits flip << 14 | tile << 11;  w1 xl << 20 | yl << 8 | dsdx >> 8
 *   data:  bits dtdy & 0x7FFF;            w1 xh << 20 | yh << 8 | (dsdx & 255)
 *   data:  bits dtdy >> 15;               w1 s << 16 | t */
#define NOP_TEXRECT(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy, flip)                                                  \
    {                                                                                                                  \
        unsigned int _ds = (unsigned int) (dsdx) & 0xFFFF, _dt = (unsigned int) (dtdy) & 0xFFFF;                       \
        NOP_PUT(pkt, NOP_EXT, (NOPX_TEXRECT << 15) | ((flip) & 1) << 14 | ((tile) & 7) << 11,                          \
                _SHIFTL(xl, 20, 12) | _SHIFTL(yl, 8, 12) | (_ds >> 8));                                                \
        NOP_PUT(pkt, NOP_EXT, (NOPX_DATA << 15) | (_dt & 0x7FFF), _SHIFTL(xh, 20, 12) | _SHIFTL(yh, 8, 12) | (_ds & 255)); \
        NOP_PUT(pkt, NOP_EXT, (NOPX_DATA << 15) | (_dt >> 15), _SHIFTL(s, 16, 16) | _SHIFTL(t, 0, 16));                 \
    }
#undef gSPTextureRectangle
#define gSPTextureRectangle(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy)                                               \
    if (gPortNativeOps) {                                                                                              \
        NOP_TEXRECT(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy, 0)                                                    \
    } else {                                                                                                           \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        _g->words.w0 = (_SHIFTL(G_TEXRECT, 24, 8) | _SHIFTL(xh, 12, 12) | _SHIFTL(yh, 0, 12));                         \
        _g->words.w1 = (_SHIFTL(tile, 24, 3) | _SHIFTL(xl, 12, 12) | _SHIFTL(yl, 0, 12));                              \
        _g = (Gfx *) (pkt);                                                                                            \
        _g->words.w0 = _SHIFTL(G_RDPHALF_1, 24, 8);                                                                    \
        _g->words.w1 = (_SHIFTL(s, 16, 16) | _SHIFTL(t, 0, 16));                                                       \
        _g = (Gfx *) (pkt);                                                                                            \
        _g->words.w0 = _SHIFTL(G_RDPHALF_2, 24, 8);                                                                    \
        _g->words.w1 = (_SHIFTL(dsdx, 16, 16) | _SHIFTL(dtdy, 0, 16));                                                 \
    }

/* The port's tags (draw kinds, native draw records, matrices as floats...): a site for the walk census too. */
#undef gDPNoOpTag
#define gDPNoOpTag(pkt, tag)                                                                                           \
    {                                                                                                                  \
        Gfx *_g = (Gfx *) (pkt);                                                                                       \
        PORT_CENSUS_SITE(_g);                                                                                          \
        _g->words.w0 = _SHIFTL(G_NOOP, 24, 8);                                                                         \
        _g->words.w1 = (tag);                                                                                          \
    }

/* Reads a geometry or list command in either form (N64 or native op), for the code that looks ahead in a list. */
enum { PCMD_OTHER, PCMD_VTX, PCMD_TRI, PCMD_MVP, PCMD_CALL, PCMD_END };
typedef struct {
    unsigned int addr, count, flag; /* VTX: count, append; TRI: count, textured; MVP: index; CALL: count (0: none), branch */
} PortCmd;
static inline int port_cmd(unsigned int w0, unsigned int w1, PortCmd *c) {
    unsigned int op = w0 >> 24;
    c->addr = w1;
    if (op == 0xC0 && (w0 & NOP_MARK)) {
        unsigned int bits = w0 & 0x7FFF;
        if (((w0 >> 19) & 15) != NOP_EXT) {
            return PCMD_OTHER;
        }
        switch ((w0 >> 15) & 15) {
            case NOPX_VTX: c->count = ((bits >> 3) & 31) + 1; c->flag = bits & 1; return PCMD_VTX;
            case NOPX_TRI: c->count = bits & 0xFFF; c->flag = (bits >> 12) & 1; return PCMD_TRI;
            case NOPX_MVP: c->count = bits & 3; return PCMD_MVP;
            case NOPX_CALL: c->count = (bits >> 13) & 1 ? (bits & 0xFF) : 0; c->flag = (bits >> 14) & 1; return PCMD_CALL;
            case NOPX_END: return PCMD_END;
            default: return PCMD_OTHER;
        }
    }
    switch (op) {
        case G_VTX: c->count = ((w0 >> 19) & 0x1F) + 1; c->flag = (w0 >> 16) & 1; return PCMD_VTX;
        case 5 /* G_TRIN */: c->count = (w0 >> 4) & 0xFFF; c->flag = (w0 >> 16) & 1; return PCMD_TRI;
        case (unsigned char) G_MOVEWORD:
            if ((w0 & 0xFF) == 0x0A /* G_MW_MVPMATRIX */) {
                c->count = (w1 >> 6) & 3;
                return PCMD_MVP;
            }
            return PCMD_OTHER;
        case 7 /* G_DMADL */: c->count = (w0 >> 16) & 0xFF; c->flag = 0; return PCMD_CALL;
        case G_DL: c->count = 0; c->flag = ((w0 >> 16) & G_DL_NOPUSH) != 0; return PCMD_CALL;
        case (unsigned char) G_ENDDL: return PCMD_END;
        default: return PCMD_OTHER;
    }
}

#endif
#endif
