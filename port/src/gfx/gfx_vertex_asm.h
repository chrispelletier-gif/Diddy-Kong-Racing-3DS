/*
 * P60 B (DKR_VERTEX_ASM): the native draws' vertex pass as one hand-scheduled VFP loop (ARM11, 3DS only).
 *
 * native_record's C makes each load's vertices in three passes over memory: the transform (transform_vertices_vfp,
 * P27), then a loop for the colour and fog, then a loop for the screen edges (vertex_outcode's sums, keeping the screen
 * x and y). gfx_vertex_asm does all three in one pass, with the same operations in the same order, so its results are
 * bit-identical (port/tools/vertex_asm_test.c runs both on random vertices in qemu-arm, in both FPSCR modes; the game's
 * DKR_VERTEX_ASM_CHECK compares them on the console).
 *
 * The loop is software pipelined: while the VFP works through vertex i's transform (four short vector instructions,
 * FPSCR LEN = 4, as P27), the same iteration does vertex i-1's screen x and y, its five edge differences, its fog
 * divide (started early, so its ~19 cycles run alongside) and its fog's scale and clamp, and the ARM side packs the
 * edges' sign bits (moved over with vmov: no VFP compares, whose flag transfers stall). One last pass does the last
 * vertex's half with the transform skipped (every A instruction is conditional on the vertices left).
 *
 * Registers (FPSCR LEN = 4 throughout the loop; an instruction writing s0-s7 is scalar):
 *   s8-s23   the matrix's rows (vectors)          s24-s27  x, y, z, w of the vertex just transformed (vector)
 *   s28-s31  scaleX, transX, scaleY, transY      s7       1e-3 (the near plane's edge)
 *   s0-s2    the vertex's x, y, z (then the edge differences), s3 / s4 screen x / y, s5 the fog, s6 w
 *   r0 src, r1 vertices left to transform, r2 the previous vertex's RspVertex, r3 args, r4 / r5 / r6 screen x / y /
 *   edges out, r7 the edges ANDed, r8 the fog's zero test mask (0: no fog), r11 the previous vertex's colour, lr the
 *   next one's.
 *
 * Fog, as fog_factor: w = (w != 0) ? w : 0.0001; fog = (z / w) * fogMul + fogOffset, clamped to 0..255 and truncated.
 * w != 0 is tested on the ARM side from w's bits: all but the sign without flush to zero; the exponent with it (FPSCR
 * FZ, which makes the VFP compare a denormal equal to zero). The clamp is vcvt (truncates, saturates) then usat.
 *
 * The C side passes the layout's sizes as these numbers; gfx_rdp.c checks them against the structs.
 */
#ifndef GFX_VERTEX_ASM_H
#define GFX_VERTEX_ASM_H

#define VERTEX_ASM_SRC_SIZE 10 /* sizeof(Vertex): s16 x, y, z; u8 r, g, b, a */
#define VERTEX_ASM_DST_SIZE 24 /* sizeof(RspVertex): float x, y, z, w; u32 rgba at 16 */
#define VERTEX_ASM_DST_RGBA 16

/* gfx_vertex_asm's other inputs (offsets used by the assembly: keep the order). */
typedef struct VertexAsmArgs {
    float (*m)[4];    /*  0: the load's matrix */
    float *sx, *sy;   /*  4,  8: screen x and y out (sNativeSx / sNativeSy at the load's first vertex) */
    unsigned char *out; /* 12: the edges out (sNativeOut) */
    float scaleX, transX, scaleY, transY; /* 16-28: the Viewport */
    float fogMul, fogOffset; /* 32, 36 */
    float tiny;       /* 40: 0.0001f, fog_factor's w for w == 0 */
    float edge;       /* 44: 1e-3f, the near plane's edge in vertex_outcode */
    unsigned int fog; /* 48: nonzero: the fog factor goes into the colour's alpha */
} VertexAsmArgs;

#if defined(__arm__) && defined(__VFP_FP__) && !defined(__SOFTFP__)
#define HAVE_VERTEX_ASM 1

#ifndef VERTEX_ASM_SECTION
#define VERTEX_ASM_SECTION
#endif

/*
 * src: count (>= 1) Vertex; dst: count RspVertex (x, y, z, w and rgba written; the next word left as it is).
 * Returns the AND of the vertices' edges (0x1F ANDed with each).
 */
__attribute__((naked, noinline, used)) VERTEX_ASM_SECTION static unsigned int
gfx_vertex_asm(__attribute__((unused)) const void *src, __attribute__((unused)) int count,
               __attribute__((unused)) void *dst, __attribute__((unused)) const VertexAsmArgs *args) {
    __asm__ volatile(
        ".syntax unified\n\t"
        "push {r4-r11, lr}\n\t"
        "vpush {d8-d15}\n\t"
        "ldr r12, [r3]\n\t"           // m
        "ldr r4, [r3, #4]\n\t"        // sx
        "ldr r5, [r3, #8]\n\t"        // sy
        "ldr r6, [r3, #12]\n\t"       // out
        "vldmia r12, {s8-s23}\n\t"    // the matrix's rows
        "add r12, r3, #16\n\t"
        "vldmia r12, {s28-s31}\n\t"   // scaleX, transX, scaleY, transY
        "vldr s7, [r3, #44]\n\t"      // 1e-3
        "ldr r8, [r3, #48]\n\t"       // fog
        "vmrs r11, fpscr\n\t"
        "push {r11}\n\t"              // put back at the end (sp stays 8-byte aligned: 9 + 16 + 1 words)
        "cmp r8, #0\n\t"
        "beq 0f\n\t"
        "mvn r8, #0x80000000\n\t"     // w's bits but the sign: zero
        "tst r11, #0x01000000\n\t"    // FZ: a denormal w compares equal to zero, so only its exponent counts
        "movne r8, #0x7F000000\n\t"
        "orrne r8, r8, #0x00800000\n"
        "0:\n\t"
        "bic r12, r11, #0x00370000\n\t" // LEN (bits 16-18) and STRIDE (20-21)
        "orr r12, r12, #0x00030000\n\t" // LEN = 4, STRIDE = 1
        "vmsr fpscr, r12\n\t"
        "mov r7, #0x1F\n\t"
        // The first vertex's transform (A) alone.
        "ldrsh r9, [r0]\n\t"
        "ldrsh r10, [r0, #2]\n\t"
        "ldrsh r12, [r0, #4]\n\t"
        "ldrh r11, [r0, #6]\n\t"
        "ldrh lr, [r0, #8]\n\t"
        "add r0, r0, #10\n\t"
        "vmov s0, s1, r9, r10\n\t"
        "vmov s2, r12\n\t"
        "orr r11, r11, lr, lsl #16\n\t" // Its colour: r, g, b, a as one word (memcpy of the 4 bytes)
        "vcvt.f32.s32 s0, s0\n\t"
        "vcvt.f32.s32 s1, s1\n\t"
        "vcvt.f32.s32 s2, s2\n\t"
        "vmul.f32 s24, s8, s0\n\t"    // s24-s27 = row 0 * x
        "vmla.f32 s24, s12, s1\n\t"   // += row 1 * y
        "vmla.f32 s24, s16, s2\n\t"   // += row 2 * z
        "vadd.f32 s24, s24, s20\n\t"  // += row 3
        "vstmia r2, {s24-s27}\n\t"
        "subs r1, r1, #1\n"           // Vertices left to transform
        "1:\n\t"
        // B: the previous vertex (s24-s27, at r2) from here on; A: the next one (r0), while r1 > 0.
        "vmov r9, s27\n\t"
        "vmul.f32 s3, s24, s28\n\t"   // x * scaleX
        "vmul.f32 s4, s25, s30\n\t"   // y * scaleY
        "vmov.f32 s6, s27\n\t"        // w
        "tst r9, r8\n\t"
        "vdivne.f32 s5, s26, s27\n\t" // fog: z / w, started early
        "beq 5f\n"                    // No fog, or w == 0
        "2:\n\t"
        "cmp r1, #0\n\t"
        "ldrshne r9, [r0]\n\t"
        "ldrshne r10, [r0, #2]\n\t"
        "ldrshne r12, [r0, #4]\n\t"
        "vmla.f32 s3, s6, s29\n\t"    // screen x = x * scaleX + w * transX
        "vmla.f32 s4, s6, s31\n\t"    // screen y = y * scaleY + w * transY
        "vmovne s0, s1, r9, r10\n\t"
        "vmovne s2, r12\n\t"
        "ldrhne r9, [r0, #6]\n\t"
        "ldrhne r10, [r0, #8]\n\t"
        "vcvtne.f32.s32 s0, s0\n\t"
        "vcvtne.f32.s32 s1, s1\n\t"
        "vcvtne.f32.s32 s2, s2\n\t"
        "orrne lr, r9, r10, lsl #16\n\t"
        "addne r0, r0, #10\n\t"
        "vmulne.f32 s24, s8, s0\n\t"
        "vstmia r4!, {s3}\n\t"
        "vmlane.f32 s24, s12, s1\n\t"
        "vstmia r5!, {s4}\n\t"
        "vmlane.f32 s24, s16, s2\n\t"
        "vsub.f32 s0, s6, s3\n\t"     // w - x
        "vadd.f32 s1, s3, s6\n\t"     // x + w
        "vsub.f32 s2, s6, s4\n\t"     // w - y
        "vadd.f32 s4, s4, s6\n\t"     // y + w
        "vsub.f32 s3, s7, s6\n\t"     // 1e-3 - w
        "vaddne.f32 s24, s24, s20\n\t"
        "addne r12, r2, #24\n\t"
        "vstmiane r12, {s24-s27}\n\t"
        "cmp r8, #0\n\t"              // Fog (to the end: no instruction below sets the flags)
        "vldrne s6, [r3, #32]\n\t"
        "vmulne.f32 s5, s5, s6\n\t"   // * fogMul
        "vmov r9, r10, s0, s1\n\t"
        "lsr r9, r9, #31\n\t"         // The edges: the differences' sign bits
        "lsr r10, r10, #31\n\t"
        "orr r9, r9, r10, lsl #1\n\t"
        "vldrne s0, [r3, #36]\n\t"
        "vaddne.f32 s5, s5, s0\n\t"   // + fogOffset
        "vmov r10, r12, s2, s3\n\t"
        "lsr r10, r10, #31\n\t"
        "orr r9, r9, r10, lsl #2\n\t"
        "mvn r12, r12\n\t"
        "lsr r12, r12, #31\n\t"
        "orr r9, r9, r12, lsl #4\n\t"
        "vcvtne.s32.f32 s5, s5\n\t"   // Truncated (saturating)
        "vmov r10, s4\n\t"
        "lsr r10, r10, #31\n\t"
        "orr r9, r9, r10, lsl #3\n\t"
        "strb r9, [r6], #1\n\t"
        "and r7, r7, r9\n\t"
        "vmovne r9, s5\n\t"
        "usatne r9, #8, r9\n\t"       // 0..255
        "bicne r11, r11, #0xFF000000\n\t"
        "orrne r11, r11, r9, lsl #24\n\t"
        "str r11, [r2, #16]\n\t"      // The colour
        "mov r11, lr\n\t"
        "add r2, r2, #24\n\t"
        "subs r1, r1, #1\n\t"
        "bpl 1b\n\t"
        "pop {r12}\n\t"
        "vmsr fpscr, r12\n\t"
        "mov r0, r7\n\t"
        "vpop {d8-d15}\n\t"
        "pop {r4-r11, pc}\n"
        "5:\n\t"                      // Rare: fog with w == 0 (or no fog: nothing to do)
        "cmp r8, #0\n\t"
        "beq 2b\n\t"
        "vldr s5, [r3, #40]\n\t"
        "vdiv.f32 s5, s26, s5\n\t"    // z / 0.0001
        "b 2b\n\t");
}
#endif

/*
 * The self-checks' comparison (DKR_VERTEX_ASM_CHECK in gfx_rdp.c, and port/tools/vertex_asm_test.c): the words that
 * differ between two runs over c vertices (x, y, z, w and rgba of each RspVertex, screen x, screen y, edges). *first
 * gets the first differing vertex (left as it is if none).
 */
static __attribute__((unused)) unsigned int vertex_asm_compare(const void *va, const void *vb, const float *sxa,
                                                               const float *sxb, const float *sya, const float *syb,
                                                               const unsigned char *oa, const unsigned char *ob, int c,
                                                               int *first) {
    unsigned int differ = 0, wa, wb;
    int i, k;
    for (i = 0; i < c; i++) {
        unsigned int before = differ;
        for (k = 0; k < 5; k++) {
            __builtin_memcpy(&wa, (const char *) va + i * VERTEX_ASM_DST_SIZE + 4 * k, 4);
            __builtin_memcpy(&wb, (const char *) vb + i * VERTEX_ASM_DST_SIZE + 4 * k, 4);
            differ += wa != wb;
        }
        __builtin_memcpy(&wa, &sxa[i], 4);
        __builtin_memcpy(&wb, &sxb[i], 4);
        differ += wa != wb;
        __builtin_memcpy(&wa, &sya[i], 4);
        __builtin_memcpy(&wb, &syb[i], 4);
        differ += wa != wb;
        differ += oa[i] != ob[i];
        if (differ != before && *first < 0) {
            *first = i;
        }
    }
    return differ;
}

#endif
