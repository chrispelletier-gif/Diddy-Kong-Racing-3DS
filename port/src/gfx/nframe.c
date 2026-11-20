/*
 * The native frame (roadmap R4, DKR_NATIVE_FRAME): the frame read as a flat stream of records.
 *
 * With the native switches on (native ops, DKR_NATIVE_RECORDS, DKR_NATIVE_LEFTOVERS, DKR_NATIVE_CAMERA and
 * DKR_NATIVE_TEXHANDLE), what the game writes into a frame is already the renderer's own records, one or more 64 bit
 * words each, the same size as the N64 commands they replaced (port_nops.h, port_drawtag.h):
 *   DRAW      PORT_NDRAW_TAG | material, then its data words (meshes, dynamic batches, sprites, billboards)
 *   RECT      PORT_NRECT_TAG | material, then the rectangle's three words
 *   MTXF      PORT_MTXF_TAG | index, then the address of a float matrix (and the ANCHOR, light and mesh/track tags)
 *   KIND      PORT_DRAW_TAG | kind
 *   MATERIAL  PORT_NMAT_TAG | material after a material's settings (its first use only), and PORT_NTEX_TAG: a
 *             texture by handle, so later DRAWs name the material and no tile or texture load op is run again
 *   STATE     a native op (NOP_MARK): modes, colours, scissor, viewport, fog
 *   FILL      NOP_FILLRECT: a fill rectangle, drawn by fill_rect_at
 *   LIST      NOPX_CALL: a constant list, converted to native ops in place the first time it runs (nop_static), so it
 *             is the same records again; run from here without the walk's recursion
 * run_native_frame reads that stream with one loop and hands each record to the handler the walk uses for it
 * (gfx_internal.h): no N64 opcode table, no per command census/trace/op count/timing tests, and lists are a small
 * stack of positions instead of nested walks. N64 commands still in a frame (a list nop_static couldn't convert, the
 * frames written before native ops are on) go to the walk itself, a command at a time, so nothing draws differently;
 * the census (DKR_WALK_CENSUS) shows them as the classes they always were.
 *
 * The walk (run_command_list) stays for every switch off, the reference renderer, snapshots, traced frames and the
 * N64 comparison tool.
 */
#include <ultra64.h>
#include "types.h"
#include "f3ddkr.h"
#include "structs.h"
#include "port.h"
#include "plat/plat.h"
#include "port_drawtag.h"
#include "gfx_internal.h"

#define UNLIKELY(x) __builtin_expect((x) != 0, 0)
#define NO_LIMIT 0x7FFFFFFF

typedef struct NFrameReturn {
    const Gfx *dl;
    s32 remaining;
} NFrameReturn;

/* PLAT_HOT_GAME: the game core runs it every frame on an Old 3DS (plat.h, P57 D); out of line, so that it lies in
 * that stretch rather than inlined into port_gfx_run_frame. */
PLAT_HOT_GAME PLAT_NOINLINE void run_native_frame(const Gfx *dl) {
    NFrameReturn stack[GFX_LIST_DEPTH];
    s32 depth = 0;
    s32 remaining = NO_LIMIT;
    s32 census = gfx_frame_census_on();

    for (;;) {
        u32 w0, w1;
        if (remaining <= 0) {
            goto list_done;
        }
        w0 = dl->words.w0;
        w1 = dl->words.w1;

        if (UNLIKELY((w0 >> 24) != 0xC0)) {
            // An N64 command, as the game wrote it.
            u32 op = w0 >> 24;
            PortCmd pc;
            s32 kind = port_cmd(w0, w1, &pc);
            if (kind == PCMD_CALL) {
                // G_DL / G_DMADL: a LIST, as for NOPX_CALL below (the walk's call_display_list).
                s32 max = (op == (u32) G_DMADL) ? (s32) pc.count : -1;
                s32 stop = FALSE;
                const Gfx *list;
                if (UNLIKELY(census)) {
                    gfx_frame_census(dl, depth);
                }
                dl++;
                remaining--;
                list = gfx_frame_list(w1, max, dl - 1, depth, &stop);
                if (stop) {
                    goto list_done;
                }
                if (pc.flag) {
                    remaining = 0; // A branch: this list ends with the called one
                }
                if (list != NULL) {
                    stack[depth].dl = dl;
                    stack[depth].remaining = remaining;
                    depth++;
                    dl = list;
                    remaining = (max < 0) ? NO_LIMIT : max;
                }
                continue;
            }
            if (kind == PCMD_END) {
                goto list_done;
            }
            // Anything else (its census is the walk's): a texture rectangle with its two G_RDPHALF words, or one command.
            {
                s32 n = (op == (u32) (u8) G_TEXRECT || op == (u32) (u8) G_TEXRECTFLIP) ? 3 : 1;
                gfx_frame_walk(dl, n, depth);
                dl += n;
                remaining -= n;
            }
            continue;
        }

        if (UNLIKELY(census)) {
            gfx_frame_census(dl, depth);
        }
        dl++;
        remaining--;

        if (!(w0 & NOP_MARK)) {
            // A tag: DRAW, RECT, MTXF, KIND, MATERIAL and the rest (gfx_rdp.c run_tag).
            s32 used = gfx_frame_tag(dl, w1, remaining, depth);
            dl += used;
            remaining -= used;
            continue;
        }
        if (((w0 >> 19) & 15) != NOP_EXT) {
            gfx_frame_op(w0, w1); // STATE and FILL
            continue;
        }
        switch ((w0 >> 15) & 15) {
            case NOPX_TEXRECT:
                if (remaining >= 2) {
                    gfx_frame_texrect(dl - 1);
                    dl += 2;
                    remaining -= 2;
                }
                break;
            case NOPX_CALL: {
                u32 bits = w0 & 0x7FFF;
                s32 max = ((bits >> 13) & 1) ? (s32) (bits & 0xFF) : -1;
                s32 stop = FALSE;
                const Gfx *list = gfx_frame_list(w1, max, dl - 1, depth, &stop);
                if (stop) {
                    goto list_done;
                }
                if ((bits >> 14) & 1) {
                    remaining = 0; // A branch: this list ends with the called one
                }
                if (list != NULL) {
                    stack[depth].dl = dl;
                    stack[depth].remaining = remaining;
                    depth++;
                    dl = list;
                    remaining = (max < 0) ? NO_LIMIT : max;
                }
                break;
            }
            case NOPX_END:
                goto list_done;
            default:
                gfx_frame_ext(w0, w1, dl - 1, depth);
                break;
        }
        continue;

    list_done:
        if (depth == 0) {
            return;
        }
        depth--;
        dl = stack[depth].dl;
        remaining = stack[depth].remaining;
    }
}
