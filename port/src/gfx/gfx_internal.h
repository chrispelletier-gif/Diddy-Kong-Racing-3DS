/*
 * The renderer's own pieces shared between its files (not for the game or the platform code).
 *
 * gfx_rdp.c keeps the draw state and every handler; the record dispatcher (nframe.c, roadmap R4) reaches them through
 * these instead of copies, so a frame run either way draws exactly the same.
 */
#ifndef GFX_INTERNAL_H
#define GFX_INTERNAL_H
#ifdef PORT_NATIVE_OPS

/* How deep lists may call lists (the walk's DL_STACK_SIZE). */
#define GFX_LIST_DEPTH 18

/* gfx_rdp.c: one tag word (a no-op with a port tag in w1) whose data words start at data, with remaining words left
 * in its list. Returns the data words it used. */
s32 gfx_frame_tag(const Gfx *data, u32 w1, s32 remaining, s32 depth);
/* A native state op (NOP_MARK, not NOP_EXT), and the NOP_EXT ops other than calls, ends and texture rectangles. */
void gfx_frame_op(u32 w0, u32 w1);
s32 gfx_frame_ext(u32 w0, u32 w1, const Gfx *where, s32 depth);
/* A texture rectangle (NOPX_TEXRECT and its two data words, from rect). */
void gfx_frame_texrect(const Gfx *rect);
/* The walk (run_command_list) for maxCommands commands from dl: the N64 commands the dispatcher doesn't run itself. */
void gfx_frame_walk(const Gfx *dl, s32 maxCommands, s32 depth);
/* A list call made at depth: the list to run, or NULL (with *stop set when the calling list ends). */
const Gfx *gfx_frame_list(u32 addr, s32 maxCommands, const Gfx *where, s32 depth, s32 *stop);
/* The walk census (DKR_WALK_CENSUS) for one word, counted as the dispatcher sees it (calls are LIST records, fills
 * FILL records), and whether it counts this frame. */
void gfx_frame_census(const Gfx *at, s32 depth);
s32 gfx_frame_census_on(void);

/* nframe.c: runs a frame's list as records (DKR_NATIVE_FRAME). */
void run_native_frame(const Gfx *start);
/* P55 S4 (DKR_FLAT_FRAME): the same with the flat record path, on the frames gfx_rdp.c's flat_frame_ok allows. */
void run_flat_frame(const Gfx *start);
/* Its handlers: gfx_frame_tag with the record tag first and native_record_flat, and a state op without step marks. */
s32 gfx_frame_tag_flat(const Gfx *data, u32 w1, s32 remaining, s32 depth);
void gfx_frame_op_flat(u32 w0, u32 w1);

#endif
#endif
