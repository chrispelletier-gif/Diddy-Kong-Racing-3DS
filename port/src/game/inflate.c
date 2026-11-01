/*
 * Raw DEFLATE decoder replacing the game's hand written MIPS inflate routines (src/hasm/gzip_asm.s).
 *
 * The game decompresses in place: the compressed data sits at the end of the output buffer. The output never
 * catches up with the input that is still to be read, so reading a few bytes ahead is safe, but writing past the
 * current match or literal is not (those bytes may still be unread input). Copies therefore never overshoot.
 *
 * Speed (level loads spend most of their time here): a 32-bit bit buffer topped up a byte at a time, 10-bit
 * lookup tables whose entries already carry each length or distance code's base and extra bit count, and match
 * copies done 4 bytes at a time when the distance allows.
 */
#include <ultra64.h>
#include "types.h"
#include "port.h"

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288
#define FAST_BITS 10 // Literal/length table; codes longer than this take the slow path.
#define FAST_SIZE (1 << FAST_BITS)
#define FAST_MASK (FAST_SIZE - 1)
#define DIST_BITS 8 // Distance table: the codes are short, and a smaller table is quicker to fill per block.
#define DIST_MASK ((1 << DIST_BITS) - 1)
#define CODELEN_BITS 7 // Code length codes are 7 bits at most, so their table holds them all.

/*
 * A fast table entry, for the code whose bits (lowest first) index it:
 *   bits 0-3   code length (0: the code is longer than FAST_BITS, use the slow path)
 *   bits 4-7   extra bits that follow (lengths and distances)
 *   bit  8     literal (lit/len table)
 *   bit  9     end of block (lit/len table)
 *   bit  10    invalid symbol
 *   bits 16-31 literal byte, or the length or distance base
 */
#define E_LIT 0x100
#define E_END 0x200
#define E_BAD 0x400

typedef struct Huffman {
    s16 count[MAXBITS + 1];
    s16 symbol[FIXLCODES];
    s32 bits; // Of the fast table's index.
    u32 fast[FAST_SIZE];
} Huffman;

typedef struct InflateState {
    const u8 *in;
    u8 *out;
    u8 *start; // Of the output, and its end: a damaged stream stops instead of writing outside it.
    u8 *end;
    u32 bitBuf;
    s32 bitCount;
} InflateState;

static const u16 sLengthBase[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 sLengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 sDistBase[30] = { 1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                   193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 sDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* Each byte with its bits in reverse order: deflate sends Huffman codes highest bit first. */
static u8 sReverse8[256];

enum { TABLE_CODELEN, TABLE_LITLEN, TABLE_DIST };

/* Built once before main (build_fixed_tables), not on first use: several threads unpack at the same time. */
static Huffman sFixedLen;
static Huffman sFixedDist;

/* Tops the bit buffer up to at least 25 bits: enough for any code plus its extra bits, or two 10-bit codes. */
static inline void refill(InflateState *s) {
    while (s->bitCount <= 24) {
        s->bitBuf |= (u32) (*s->in++) << s->bitCount;
        s->bitCount += 8;
    }
}

static inline u32 get_bits(InflateState *s, s32 n) {
    u32 val;
    if (n == 0) {
        return 0;
    }
    refill(s);
    val = s->bitBuf & ((1u << n) - 1);
    s->bitBuf >>= n;
    s->bitCount -= n;
    return val;
}

static u32 table_entry(s32 kind, s32 sym, s32 len) {
    if (kind == TABLE_LITLEN) {
        if (sym < 256) {
            return ((u32) sym << 16) | E_LIT | len;
        }
        if (sym == 256) {
            return E_END | len;
        }
        sym -= 257;
        if (sym >= 29) {
            return E_BAD | len;
        }
        return ((u32) sLengthBase[sym] << 16) | (sLengthExtra[sym] << 4) | len;
    }
    if (kind == TABLE_DIST) {
        if (sym >= 30) {
            return E_BAD | len;
        }
        return ((u32) sDistBase[sym] << 16) | (sDistExtra[sym] << 4) | len;
    }
    return ((u32) sym << 16) | len;
}

/* Builds a canonical Huffman decoding table. Returns 0 on success. */
static s32 build_huffman(Huffman *h, const u8 *lengths, s32 n, s32 kind) {
    s16 offs[MAXBITS + 1];
    s32 i, len, left;
    s32 code, sym, reversed, fill, step;

    for (len = 0; len <= MAXBITS; len++) {
        h->count[len] = 0;
    }
    for (sym = 0; sym < n; sym++) {
        h->count[lengths[sym]]++;
    }
    h->bits = kind == TABLE_LITLEN ? FAST_BITS : kind == TABLE_DIST ? DIST_BITS : CODELEN_BITS;
    bzero(h->fast, sizeof(h->fast[0]) << h->bits);
    if (h->count[0] == n) {
        return 0;
    }
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return -1;
        }
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (sym = 0; sym < n; sym++) {
        if (lengths[sym] != 0) {
            h->symbol[offs[lengths[sym]]++] = sym;
        }
    }

    // Fast table: walk the canonical codes in order.
    code = 0;
    i = 0;
    for (len = 1; len <= h->bits; len++) {
        s32 c;
        for (c = 0; c < h->count[len]; c++, i++, code++) {
            s32 b;
            u32 entry = table_entry(kind, h->symbol[i], len);
            (void) b;
            reversed = (sReverse8[code & 0xFF] << 8 | sReverse8[(code >> 8) & 0xFF]) >> (16 - len);
            step = 1 << len;
            for (fill = reversed; fill < (1 << h->bits); fill += step) {
                h->fast[fill] = entry;
            }
        }
        code <<= 1;
    }
    return 0;
}

/* A code longer than FAST_BITS (or one the table has no entry for): walk the canonical counts, bit by bit. */
static s32 decode_slow(InflateState *s, const Huffman *h, s32 kind, u32 *entry) {
    s32 code = 0, first = 0, index = 0, len, count;
    refill(s);
    for (len = 1; len <= MAXBITS; len++) {
        code |= (s->bitBuf >> (len - 1)) & 1;
        count = h->count[len];
        if (code - count < first) {
            s->bitBuf >>= len;
            s->bitCount -= len;
            *entry = table_entry(kind, h->symbol[index + (code - first)], 0);
            return 0;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* Returns the table entry for the next code, with its code bits already taken (the length field is then 0). */
static inline s32 decode(InflateState *s, const Huffman *h, s32 kind, u32 *entry) {
    u32 e;
    s32 len;
    refill(s);
    e = h->fast[s->bitBuf & ((1u << h->bits) - 1)];
    len = e & 15;
    if (len != 0) {
        s->bitBuf >>= len;
        s->bitCount -= len;
        *entry = e & ~15u;
        return 0;
    }
    return decode_slow(s, h, kind, entry);
}

/* Loads 4 bytes, lowest first, from any alignment. */
static inline u32 load_le32(const u8 *p) {
    u32 v;
    __builtin_memcpy(&v, p, 4); // One unaligned load on ARMv6K and x86.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

/*
 * The main loop keeps the bit buffer in locals: the output is written through a byte pointer, which could alias
 * the state struct, so the compiler would otherwise reload it after every byte.
 */
#define LOCAL_REFILL()                                                                                                 \
    do {                                                                                                               \
        if (bitCount <= 24) {                                                                                          \
            u32 n_ = (u32) (31 - bitCount) >> 3;                                                                       \
            bitBuf |= load_le32(in) << bitCount;                                                                       \
            in += n_;                                                                                                  \
            bitCount += n_ << 3;                                                                                       \
        }                                                                                                              \
    } while (0)
#define LOCAL_DROP(n)                                                                                                  \
    do {                                                                                                               \
        bitBuf >>= (n);                                                                                                \
        bitCount -= (n);                                                                                               \
    } while (0)
#define SAVE_STATE()                                                                                                   \
    do {                                                                                                               \
        s->in = in;                                                                                                    \
        s->bitBuf = bitBuf;                                                                                            \
        s->bitCount = bitCount;                                                                                        \
        s->out = out;                                                                                                  \
    } while (0)

static s32 inflate_codes(InflateState *s, const Huffman *lencode, const Huffman *distcode) {
    const u8 *in = s->in;
    u32 bitBuf = s->bitBuf;
    s32 bitCount = s->bitCount;
    u8 *out = s->out;
    u8 *const start = s->start;
    u8 *const end = s->end;
    u32 e, len, dist, extra;
    u8 *from;

    for (;;) {
        // A literal or length whose code is in the table; a longer code goes the slow way.
        LOCAL_REFILL();
        e = lencode->fast[bitBuf & FAST_MASK];
        if ((e & 15) != 0) {
            LOCAL_DROP(e & 15);
        } else {
            SAVE_STATE();
            if (decode_slow(s, lencode, TABLE_LITLEN, &e) != 0) {
                return -1;
            }
            in = s->in;
            bitBuf = s->bitBuf;
            bitCount = s->bitCount;
        }
        if (e & E_LIT) {
            if (out >= end) {
                return -1;
            }
            *out++ = (u8) (e >> 16);
            // A second literal often fits in the bits already buffered.
            if (bitCount >= FAST_BITS) {
                u32 e2 = lencode->fast[bitBuf & FAST_MASK];
                if ((e2 & (E_LIT | 15)) > E_LIT) {
                    if (out >= end) {
                        return -1;
                    }
                    LOCAL_DROP(e2 & 15);
                    *out++ = (u8) (e2 >> 16);
                }
            }
            continue;
        }
        if (e & (E_END | E_BAD)) {
            SAVE_STATE();
            return (e & E_BAD) ? -1 : 0;
        }
        // A match: its length (base + extra bits), then its distance.
        len = e >> 16;
        extra = (e >> 4) & 15;
        len += bitBuf & ((1u << extra) - 1);
        LOCAL_DROP(extra);
        LOCAL_REFILL();
        e = distcode->fast[bitBuf & DIST_MASK];
        if ((e & 15) != 0) {
            LOCAL_DROP(e & 15);
        } else {
            SAVE_STATE();
            if (decode_slow(s, distcode, TABLE_DIST, &e) != 0) {
                return -1;
            }
            in = s->in;
            bitBuf = s->bitBuf;
            bitCount = s->bitCount;
            LOCAL_REFILL();
        }
        if (e & E_BAD) {
            return -1;
        }
        dist = e >> 16;
        extra = (e >> 4) & 15;
        dist += bitBuf & ((1u << extra) - 1);
        LOCAL_DROP(extra);
        if (dist > (u32) (out - start) || len > (u32) (end - out)) {
            return -1;
        }
        from = out - dist;
        if (dist >= 4) {
            // Whole words, never past the match: the bytes after it may be input still to be read.
            while (len >= 4) {
                u32 w;
                __builtin_memcpy(&w, from, 4);
                __builtin_memcpy(out, &w, 4);
                out += 4;
                from += 4;
                len -= 4;
            }
        } else if (dist == 1) {
            u8 b = *from;
            while (len >= 4) {
                out[0] = b;
                out[1] = b;
                out[2] = b;
                out[3] = b;
                out += 4;
                len -= 4;
            }
            from = out - 1;
        }
        while (len--) {
            *out++ = *from++;
        }
    }
}

static s32 inflate_stored(InflateState *s) {
    u32 len;
    // Discard the remaining bits of the current byte, and give back any whole bytes that were read ahead.
    s->in -= s->bitCount >> 3;
    s->bitBuf = 0;
    s->bitCount = 0;
    len = s->in[0] | (s->in[1] << 8);
    if ((len ^ 0xFFFF) != (u32) (s->in[2] | (s->in[3] << 8)) || len > (u32) (s->end - s->out)) {
        return -1;
    }
    s->in += 4;
    while (len--) {
        *s->out++ = *s->in++;
    }
    return 0;
}

__attribute__((constructor)) static void build_fixed_tables(void) {
    u8 lengths[FIXLCODES];
    s32 sym, b;
    for (sym = 0; sym < 256; sym++) {
        for (b = 0; b < 8; b++) {
            sReverse8[sym] |= ((sym >> b) & 1) << (7 - b);
        }
    }
    for (sym = 0; sym < 144; sym++) {
        lengths[sym] = 8;
    }
    for (; sym < 256; sym++) {
        lengths[sym] = 9;
    }
    for (; sym < 280; sym++) {
        lengths[sym] = 7;
    }
    for (; sym < FIXLCODES; sym++) {
        lengths[sym] = 8;
    }
    build_huffman(&sFixedLen, lengths, FIXLCODES, TABLE_LITLEN);
    for (sym = 0; sym < MAXDCODES; sym++) {
        lengths[sym] = 5;
    }
    build_huffman(&sFixedDist, lengths, MAXDCODES, TABLE_DIST);
}

static s32 inflate_fixed(InflateState *s) {
    return inflate_codes(s, &sFixedLen, &sFixedDist);
}

static s32 inflate_dynamic(InflateState *s) {
    static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    // On the stack, not static: the 3DS bottom screen's thread unpacks textures and track maps while the game's
    // threads load levels, and shared tables mixed up both streams (crash report #192: heap overrun).
    Huffman lencode, distcode;
    u8 lengths[MAXLCODES + MAXDCODES];
    s32 nlen, ndist, ncode, index, sym, len;
    u32 e;

    nlen = get_bits(s, 5) + 257;
    ndist = get_bits(s, 5) + 1;
    ncode = get_bits(s, 4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) {
        return -1;
    }
    for (index = 0; index < ncode; index++) {
        lengths[order[index]] = get_bits(s, 3);
    }
    for (; index < 19; index++) {
        lengths[order[index]] = 0;
    }
    if (build_huffman(&lencode, lengths, 19, TABLE_CODELEN) != 0) {
        return -1;
    }
    index = 0;
    while (index < nlen + ndist) {
        if (decode(s, &lencode, TABLE_CODELEN, &e) != 0) {
            return -1;
        }
        sym = e >> 16;
        if (sym < 16) {
            lengths[index++] = sym;
        } else {
            len = 0;
            if (sym == 16) {
                if (index == 0) {
                    return -1;
                }
                len = lengths[index - 1];
                sym = 3 + get_bits(s, 2);
            } else if (sym == 17) {
                sym = 3 + get_bits(s, 3);
            } else {
                sym = 11 + get_bits(s, 7);
            }
            if (index + sym > nlen + ndist) {
                return -1;
            }
            while (sym--) {
                lengths[index++] = len;
            }
        }
    }
    if (build_huffman(&lencode, lengths, nlen, TABLE_LITLEN) != 0 ||
        build_huffman(&distcode, lengths + nlen, ndist, TABLE_DIST) != 0) {
        return -1;
    }
    return inflate_codes(s, &lencode, &distcode);
}

/*
 * Decompresses a raw deflate stream. Every caller passes the stream that follows the game's 5 byte header, whose
 * first 4 bytes are the unpacked size (little endian): nothing is written past that. Returns the number of bytes
 * written, or -1 on error.
 */
s32 port_inflate(const u8 *in, u8 *out) {
    InflateState s;
    s32 last, type, err;
    u32 size = in[-5] | (in[-4] << 8) | (in[-3] << 16) | ((u32) in[-2] << 24);

    s.in = in;
    s.out = out;
    s.start = out;
    s.end = out + size;
    s.bitBuf = 0;
    s.bitCount = 0;
    do {
        last = get_bits(&s, 1);
        type = get_bits(&s, 2);
        switch (type) {
            case 0:
                err = inflate_stored(&s);
                break;
            case 1:
                err = inflate_fixed(&s);
                break;
            case 2:
                err = inflate_dynamic(&s);
                break;
            default:
                err = -1;
                break;
        }
        if (err != 0) {
            port_log("inflate: corrupt data");
            return -1;
        }
    } while (!last);
    return (s32) (s.out - out);
}
