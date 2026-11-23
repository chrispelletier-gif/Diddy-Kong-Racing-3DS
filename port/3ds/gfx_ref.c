/*
 * The software renderer (host/gfx_soft.c) built into the 3DS port under other names, as a reference for comparing
 * the GPU renderer against on identical frames (DKR_DIFF_EVERY, see gfx_rdp.c).
 */
#define gfxb_init refb_init
#define gfxb_start_frame refb_start_frame
#define gfxb_end_frame refb_end_frame
#define gfxb_clear_color refb_clear_color
#define gfxb_clear_depth refb_clear_depth
#define gfxb_draw_triangles refb_draw_triangles
#define gfxb_draw_indexed refb_draw_indexed
#define gfxb_texture_upload refb_texture_upload
#define gfxb_texture_free refb_texture_free
#define gfxb_texture_share refb_texture_share
#define gfxb_gpu_transform refb_gpu_transform
#define gfxb_cpu_cull_ok refb_cpu_cull_ok
#define gfxb_cpu_cull_margin refb_cpu_cull_margin
#define gfxb_model_check refb_model_check
#define gfxb_draw_model refb_draw_model
#define gfxb_mesh_create refb_mesh_create
#define gfxb_mesh_free refb_mesh_free
#define gfxb_draw_meshes refb_draw_meshes
#define gfxb_soft_save_png refb_save_png
#define gfxb_gpu_kinds_frame refb_gpu_kinds_frame
#define gfxb_gpu_kind refb_gpu_kind
#define gfxb_gpu_kind_call refb_gpu_kind_call
#define gfxb_wave_table refb_wave_table
#define gfxb_wave_table_call refb_wave_table_call
#define gfxb_wave_tile refb_wave_tile
#define gfxb_wave_tile_call refb_wave_tile_call
#define gfxb_wave_shader_ok refb_wave_shader_ok
#define gfxb_walk_frame_start refb_walk_frame_start
#define gfxb_walk_draw_indexed refb_walk_draw_indexed
#define gfxb_walk_coverage refb_walk_coverage
#define gfxb_model_split_totals refb_model_split_totals
#define gGfxbModelSplitOn gRefbModelSplitOn
#define gfxb_sprite_points refb_sprite_points
#define gfxb_draw_sprites refb_draw_sprites
#define host_frames_rendered ref_frames_rendered
#include <stdlib.h>
// None of the host renderer's own debug settings apply here.
#define getenv(name) ((char *) 0)
#include "../host/gfx_soft.c"
