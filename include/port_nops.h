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
    NOP_LOADTLUT, /* 19 bits: tile; w1: count */
    NOP_COUNT
};
#define NOP_W0(id, bits) (NOP_TAG | NOP_MARK | ((unsigned int) (id) << 19) | ((unsigned int) (bits) & 0x7FFFF))
#define NOP_PUT(pkt, id, bits, word)                                                                                   \
    {                                                                                                                  \
        Gfx *_n = (Gfx *) (pkt);                                                                                       \
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
        if (gPortNativeOps) {                                                                                          \
            _g->words.w0 = NOP_W0(NOP_LOADTLUT, (tile) & 7);                                                           \
            _g->words.w1 = (unsigned int) (count);                                                                     \
        } else {                                                                                                       \
            _g->words.w0 = _SHIFTL(G_LOADTLUT, 24, 8);                                                                 \
            _g->words.w1 = _SHIFTL((tile), 24, 3) | _SHIFTL((count), 14, 10);                                          \
        }                                                                                                              \
    }

#endif
#endif
