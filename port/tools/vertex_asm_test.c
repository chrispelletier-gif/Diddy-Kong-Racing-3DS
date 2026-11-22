/*
 * P60 B: checks gfx_vertex_asm (src/gfx/gfx_vertex_asm.h, DKR_VERTEX_ASM) against native_record's C on random
 * vertices, word for word, in qemu-arm's ARM11 (the Old 3DS's CPU). Freestanding: Linux system calls only, no libc.
 *
 *   arm-none-eabi-gcc -O3 -ffast-math -march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -marm \
 *       -fno-math-errno -funsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-stack-protector \
 *       -nostdlib -static -Isrc/gfx -o vertex_asm_test tools/vertex_asm_test.c -lgcc
 *   qemu-arm -cpu arm11mpcore ./vertex_asm_test        (from port/; the flags are the hot files' on the 3DS)
 *
 * The reference is a copy of native_record's three passes (transform_vertices_vfp, the colour and
 * fog loop with fog_factor, the edges loop with float_sign), run with FPSCR 0 and with RunFast's FZ + DN. Cases: any
 * fog setting, w == 0 (fog's 0.0001), denormal w (flushed or not), 1 to 64 vertices, large and small values. Prints
 * "vertex asm: N vertices checked, M words differ" and exits 1 if any word differs. VERTEX_ASM_BREAK=1 (K6) drops the
 * fog's alpha from the reference on purpose, so the test must report differences.
 *
 * Only with DKR_VFP_VECTORS' transform: native_record's C transform (transform_vertices), compiled with -ffast-math like
 * the rest of the hot files, adds the products in another order and differs in the last bit, so DKR_VERTEX_ASM is used
 * only with DKR_VFP_VECTORS on (the Old 3DS default).
 */
#include <stdint.h>

typedef int16_t s16;
typedef int32_t s32;
typedef uint8_t u8;
typedef uint32_t u32;

typedef struct Vertex {
    s16 x, y, z;
    u8 r, g, b, a;
} Vertex;

typedef struct RspVertex {
    float x, y, z, w;
    u32 rgba;
    u32 fogFlat;
} RspVertex;

#include "gfx_vertex_asm.h"

_Static_assert(sizeof(Vertex) == VERTEX_ASM_SRC_SIZE, "Vertex size");
_Static_assert(sizeof(RspVertex) == VERTEX_ASM_DST_SIZE, "RspVertex size");

/* ---- freestanding bits ---- */
void *memcpy(void *d, const void *s, unsigned n) {
    u8 *dd = d;
    const u8 *ss = s;
    while (n--) {
        *dd++ = *ss++;
    }
    return d;
}
unsigned strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}
void *memset(void *d, int c, unsigned n) {
    u8 *dd = d;
    while (n--) {
        *dd++ = (u8) c;
    }
    return d;
}
static void sys_write(const char *s, int n) {
    register int r0 __asm__("r0") = 1;
    register const char *r1 __asm__("r1") = s;
    register int r2 __asm__("r2") = n;
    register int r7 __asm__("r7") = 4;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
}
static void sys_exit(int code) {
    register int r0 __asm__("r0") = code;
    register int r7 __asm__("r7") = 1;
    __asm__ volatile("svc 0" : : "r"(r0), "r"(r7));
    for (;;) {
    }
}
static void puts_(const char *s) {
    int n = 0;
    while (s[n]) {
        n++;
    }
    sys_write(s, n);
}
static void putu(u32 v) {
    char b[12];
    int n = 11;
    b[n] = 0;
    do {
        b[--n] = (char) ('0' + v % 10);
        v /= 10;
    } while (v);
    puts_(b + n);
}
static void puthex(u32 v) {
    char b[9];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    }
    b[8] = 0;
    puts_(b);
}
static u32 sRng = 0x12345678u;
static u32 rnd(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}
static float bits_f(u32 b) {
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}
static u32 f_bits(float f) {
    u32 b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}
/* A float of random sign with a random exponent in [lo, hi] (powers of two) and random mantissa. */
static float rnd_float(int lo, int hi) {
    u32 e = (u32) (127 + lo + (int) (rnd() % (u32) (hi - lo + 1)));
    return bits_f((rnd() & 0x80000000u) | (e << 23) | (rnd() & 0x7FFFFFu));
}
static u32 get_fpscr(void) {
    u32 v;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(v));
    return v;
}
static void set_fpscr(u32 v) {
    __asm__ volatile("vmsr fpscr, %0" : : "r"(v));
}

/* ---- the reference: native_record's C (gfx_rdp.c), copied ---- */
static __attribute__((noinline)) void transform_vertices_vfp(const Vertex *src, s32 count, RspVertex *dst,
                                                             float (*m)[4]) {
    u32 savedFpscr, x, y, z;
    __asm__ volatile("vldmia %[m], {s8-s23}\n\t"
                     "vmrs %[saved], fpscr\n\t"
                     "bic %[x], %[saved], #0x00370000\n\t"
                     "orr %[x], %[x], #0x00030000\n\t"
                     "vmsr fpscr, %[x]\n"
                     "1:\n\t"
                     "ldrsh %[x], [%[src]]\n\t"
                     "ldrsh %[y], [%[src], #2]\n\t"
                     "ldrsh %[z], [%[src], #4]\n\t"
                     "add %[src], %[src], %[srcSize]\n\t"
                     "vmov s0, %[x]\n\t"
                     "vmov s1, %[y]\n\t"
                     "vmov s2, %[z]\n\t"
                     "vcvt.f32.s32 s0, s0\n\t"
                     "vcvt.f32.s32 s1, s1\n\t"
                     "vcvt.f32.s32 s2, s2\n\t"
                     "vmul.f32 s24, s8, s0\n\t"
                     "vmla.f32 s24, s12, s1\n\t"
                     "vmla.f32 s24, s16, s2\n\t"
                     "vadd.f32 s24, s24, s20\n\t"
                     "vstmia %[dst], {s24-s27}\n\t"
                     "add %[dst], %[dst], %[dstSize]\n\t"
                     "subs %[count], %[count], #1\n\t"
                     "bne 1b\n\t"
                     "vmsr fpscr, %[saved]"
                     : [src] "+r"(src), [dst] "+r"(dst), [count] "+r"(count), [saved] "=&r"(savedFpscr), [x] "=&r"(x),
                       [y] "=&r"(y), [z] "=&r"(z)
                     : [m] "r"(m), [srcSize] "I"(sizeof(Vertex)), [dstSize] "I"(sizeof(RspVertex))
                     : "d0", "d1", "d4", "d5", "d6", "d7", "d8", "d9", "d10", "d11", "d12", "d13", "cc", "memory");
}
static inline u8 fog_factor(const RspVertex *v, float fogMul, float fogOffset) {
    float w = (v->w != 0.0f) ? v->w : 0.0001f;
    float fog = (v->z / w) * fogMul + fogOffset;
    return (fog < 0.0f) ? 0 : (fog > 255.0f) ? 255 : (u8) fog;
}
static inline u32 float_sign(float f) {
    u32 bits;
    __builtin_memcpy(&bits, &f, 4);
    return bits >> 31;
}
static int sBreak; // K6: the reference leaves the fog out, on purpose
static __attribute__((noinline)) u32 reference(const Vertex *src, s32 c, RspVertex *verts, const VertexAsmArgs *a) {
    s32 k, i;
    u32 all = 0x1F;
    transform_vertices_vfp(src, c, verts, a->m);
    for (k = 0; k < c; k++) {
        u32 rgba;
        __builtin_memcpy(&rgba, &src[k].r, 4);
        if (a->fog && !sBreak) {
            rgba = (rgba & 0x00FFFFFFu) | ((u32) fog_factor(&verts[k], a->fogMul, a->fogOffset) << 24);
        }
        verts[k].rgba = rgba;
    }
    for (i = 0; i < c; i++) {
        const RspVertex *v = &verts[i];
        float w = v->w, x = v->x * a->scaleX + w * a->transX, y = v->y * a->scaleY + w * a->transY;
        u32 oc = float_sign(w - x) | (float_sign(x + w) << 1) | (float_sign(w - y) << 2) | (float_sign(y + w) << 3) |
                 ((float_sign(1e-3f - w) ^ 1u) << 4);
        a->sx[i] = x;
        a->sy[i] = y;
        a->out[i] = (u8) oc;
        all &= oc;
    }
    return all;
}

/* ---- the test ---- */
#define MAXV 64
static Vertex sSrc[MAXV + 1];
static RspVertex sVa[MAXV + 1], sVr[MAXV + 1];
static float sSxa[MAXV + 1], sSya[MAXV + 1], sSxr[MAXV + 1], sSyr[MAXV + 1];
static u8 sOuta[MAXV + 4], sOutr[MAXV + 4];
static float sM[4][4];
static u32 sShared, sChecked, sDiffer, sShown, sW0, sWDenorm, sFogged, sPast;

static void diff(const char *what, s32 i, u32 got, u32 want) {
    sDiffer++;
    if (sShown < 12) {
        sShown++;
        puts_(what);
        puts_(" vertex ");
        putu((u32) i);
        puts_(": asm ");
        puthex(got);
        puts_(" C ");
        puthex(want);
        puts_("\n");
    }
}

static void one_case(u32 fpscr) {
    VertexAsmArgs aa, ar;
    s32 c = 1 + (s32) (rnd() % MAXV), i, r, k, kind = (s32) (rnd() % 8);
    u32 alla, allr;
    // The matrix: rotation-like entries, a translation, a perspective column. Some cases make w exactly zero or
    // denormal for every vertex.
    for (r = 0; r < 4; r++) {
        for (k = 0; k < 4; k++) {
            sM[r][k] = r < 3 ? rnd_float(-12, 1) : rnd_float(-4, 12);
        }
    }
    if (kind == 6) {
        sM[0][3] = sM[1][3] = sM[2][3] = sM[3][3] = 0.0f;
    } else if (kind == 7) {
        sM[0][3] = sM[1][3] = sM[2][3] = 0.0f;
        sM[3][3] = bits_f((rnd() & 0x80000000u) | (1 + (rnd() & 0x7FFFFFu))); // denormal
    }
    for (i = 0; i <= c; i++) {
        s32 range = (rnd() & 1) ? 0x7FFF : 0x3FF;
        sSrc[i].x = (s16) ((s32) (rnd() & 0xFFFF) % (range + 1) * ((rnd() & 1) ? -1 : 1));
        sSrc[i].y = (s16) ((s32) (rnd() & 0xFFFF) % (range + 1) * ((rnd() & 1) ? -1 : 1));
        sSrc[i].z = (s16) ((s32) (rnd() & 0xFFFF) % (range + 1) * ((rnd() & 1) ? -1 : 1));
        sSrc[i].r = (u8) rnd(), sSrc[i].g = (u8) rnd(), sSrc[i].b = (u8) rnd(), sSrc[i].a = (u8) rnd();
    }
    if (kind == 5) {
        // A vertex at the origin with a zero translation: w == 0 through the sums.
        sSrc[0].x = sSrc[0].y = sSrc[0].z = 0;
        sM[3][3] = 0.0f;
    }
    aa.m = sM;
    aa.scaleX = rnd_float(-1, 1), aa.transX = rnd_float(-6, 0), aa.scaleY = rnd_float(-1, 1);
    aa.transY = rnd_float(-6, 0);
    if (rnd() & 1) {
        aa.transX = aa.transY = 0.0f;
    }
    aa.fogMul = rnd_float(-2, 9);
    aa.fogOffset = rnd_float(-2, 9);
    aa.tiny = 0.0001f;
    aa.edge = 1e-3f;
    aa.fog = (rnd() & 3) != 0;
    ar = aa;
    // Sentinels past the end and in the untouched word.
    for (i = 0; i <= MAXV; i++) {
        sVa[i].fogFlat = sVr[i].fogFlat = 0xA5A5A5A5u;
        sVa[i].x = sVr[i].x = sVa[i].y = sVr[i].y = bits_f(0x7FC0DEADu);
        sVa[i].z = sVr[i].z = sVa[i].w = sVr[i].w = bits_f(0x7FC0DEADu);
        sVa[i].rgba = sVr[i].rgba = 0x5A5A5A5Au;
        sSxa[i] = sSxr[i] = sSya[i] = sSyr[i] = bits_f(0x7FC0BEEFu);
        sOuta[i] = sOutr[i] = 0xEE;
    }
    aa.sx = sSxa, aa.sy = sSya, aa.out = sOuta;
    ar.sx = sSxr, ar.sy = sSyr, ar.out = sOutr;
    set_fpscr(fpscr);
    allr = reference(sSrc, c, sVr, &ar);
    set_fpscr(fpscr);
    alla = gfx_vertex_asm(sSrc, c, sVa, &aa);
    if (get_fpscr() != fpscr) {
        diff("fpscr not put back", 0, get_fpscr(), fpscr);
    }
    set_fpscr(0);
    for (i = 0; i <= c; i++) {
        const u32 *wa = (const u32 *) &sVa[i], *wr = (const u32 *) &sVr[i];
        for (k = 0; k < 6; k++) {
            if (wa[k] != wr[k]) {
                diff(k < 4 ? "xyzw" : k == 4 ? "rgba" : "word 5", i, wa[k], wr[k]);
            }
        }
        if (f_bits(sSxa[i]) != f_bits(sSxr[i])) {
            diff("screen x", i, f_bits(sSxa[i]), f_bits(sSxr[i]));
        }
        if (f_bits(sSya[i]) != f_bits(sSyr[i])) {
            diff("screen y", i, f_bits(sSya[i]), f_bits(sSyr[i]));
        }
        if (sOuta[i] != sOutr[i]) {
            diff("edges", i, sOuta[i], sOutr[i]);
        }
        if (i < c) {
            u32 wb = f_bits(sVr[i].w);
            sW0 += (wb << 1) == 0;
            sWDenorm += (wb << 1) != 0 && (wb & 0x7F800000u) == 0;
            sFogged += aa.fog;
            sChecked++;
        }
    }
    {
        int first = -1;
        sShared += vertex_asm_compare(sVa, sVr, sSxa, sSxr, sSya, sSyr, sOuta, sOutr, c, &first);
    }
    sPast++;
    if (alla != allr) {
        diff("edges ANDed", c, alla, allr);
    }
}

void __attribute__((used)) test_main(void) {
    static const u32 modes[2] = {0x00000000u, 0x03000000u}; // IEEE; RunFast's flush to zero + default NaN
    int m, n;
    for (m = 0; m < 2; m++) {
        u32 before = sDiffer;
        for (n = 0; n < 100000; n++) {
            one_case(modes[m]);
        }
        puts_(m ? "RunFast (FZ + DN): " : "IEEE: ");
        putu(sDiffer - before);
        puts_(" words differ\n");
    }
    puts_("vertex asm: ");
    putu(sChecked);
    puts_(" vertices checked (w == 0: ");
    putu(sW0);
    puts_(", denormal w: ");
    putu(sWDenorm);
    puts_(", with fog: ");
    putu(sFogged);
    puts_("), ");
    putu(sDiffer);
    puts_(" words differ (vertex_asm_compare: ");
    putu(sShared);
    puts_(")\n");
    sys_exit(sDiffer != 0 || sShared != 0);
}

/* Entry: the stack is set up by the kernel; argv's environment holds VERTEX_ASM_BREAK=1 for the K6 run. */
static int env_break(char **envp) {
    for (; *envp; envp++) {
        const char *s = *envp, *t = "VERTEX_ASM_BREAK=1";
        while (*t && *s == *t) {
            s++, t++;
        }
        if (!*t) {
            return 1;
        }
    }
    return 0;
}
void __attribute__((used)) c_start(u32 *sp) {
    int argc = (int) sp[0];
    char **envp = (char **) &sp[argc + 2];
    sBreak = env_break(envp);
    test_main();
}
__asm__(".global _start\n_start:\n\tmov r0, sp\n\tbic sp, sp, #7\n\tbl c_start\n");
