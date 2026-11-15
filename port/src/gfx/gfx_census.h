/*
 * The walk census (DKR_WALK_CENSUS=1): what the renderer's display list walk (gfx_rdp.c run_command_list) executes,
 * counted per frame, per draw kind and per class, so the leftovers of the native renderer (roadmap finish plan,
 * stage R1) can be listed and cleared stage by stage (G1-G3, R2, R3, R4). Counting works on the 3DS too (a
 * "census" line in the log every 600 frames); the host build also names the game's source line behind each
 * command (port/host/census_sites.c, PORT_CENSUS_SITES) and writes census.csv.
 */
#ifndef GFX_CENSUS_H
#define GFX_CENSUS_H

/* What a walked command is. The first six are the finish plan's classes; the others keep them honest. */
typedef enum CensusClass {
    CENSUS_RECORD,      /* A flat record: NDRAW (mesh, dynamic, sprite), NRECT, MTXF, the native mesh and track tags */
    CENSUS_STATE,       /* A native state op (modes, colours, scissor, viewport, fog, tags...) */
    CENSUS_VERTEX_SLOT, /* NOPX_VTX, NOPX_TRI: vertices into the N64's slots, triangles drawn from them */
    CENSUS_TMEM,        /* Tile and texture load ops: NOP_TILE, TILESIZE, LOADBLOCK, LOADTILE, texture IMAGE, LOADTLUT */
    CENSUS_MATRIX,      /* NOPX_MTX: a fixed point matrix */
    CENSUS_LIST_CALL,   /* NOPX_CALL: a display list call (segment address) */
    CENSUS_RAW_N64,     /* An N64 command left as the game wrote it (not converted to a native op) */
    CENSUS_RECT,        /* A texture or fill rectangle that is not a record (NRECT is) */
    CENSUS_RECORD_SLOTS, /* Inside records: a record drawn through the vertex slots (dynamic, billboard, sprite, declined) */
    CENSUS_RECORD_DROP, /* Inside records: a record dropped (material not captured, texture gone) */
    CENSUS_CLASSES
} CensusClass;

/* Ops within a class (CENSUS_OP_RAW + opcode for a raw N64 command). */
typedef enum CensusOp {
    CENSUS_OP_NDRAW,
    CENSUS_OP_NDRAW_DYNAMIC,
    CENSUS_OP_SPRITE,
    CENSUS_OP_NRECT,
    CENSUS_OP_MTXF,
    CENSUS_OP_NATIVE_MESH,
    CENSUS_OP_NATIVE_TRACK,
    CENSUS_OP_NMAT,
    CENSUS_OP_KIND_TAG,
    CENSUS_OP_LIGHT_TAG,
    CENSUS_OP_STATE,
    CENSUS_OP_VIEWPORT,
    CENSUS_OP_VTX,
    CENSUS_OP_TRI,
    CENSUS_OP_TILE,
    CENSUS_OP_TILESIZE,
    CENSUS_OP_LOADBLOCK,
    CENSUS_OP_LOADTILE,
    CENSUS_OP_LOADTLUT,
    CENSUS_OP_IMAGE_TEX,
    CENSUS_OP_MTX,
    CENSUS_OP_CALL,
    CENSUS_OP_TEXRECT,
    CENSUS_OP_FILLRECT,
    CENSUS_OP_SLOTS_DYNAMIC,
    CENSUS_OP_SLOTS_BILLBOARD,
    CENSUS_OP_SLOTS_SPRITE,
    CENSUS_OP_SLOTS_DECLINED,
    CENSUS_OP_DROP_MATERIAL,
    CENSUS_OP_DROP_TEXTURE,
    CENSUS_OP_COUNT,
    CENSUS_OP_RAW = 0x100
} CensusOp;

#ifdef PORT_CENSUS_SITES
/* census_sites.c (host only). */
void port_census_frame_begin(int level, unsigned int levelLoads);
void port_census_frame_end(void);
void port_census_hit(const void *cmd, int depth, int kind, int cls, int op, const void *list);
void port_census_material_captured(void);
#define CENSUS_FRAME_BEGIN(level, loads) port_census_frame_begin(level, loads)
#define CENSUS_FRAME_END() port_census_frame_end()
#define CENSUS_HOST_HIT(cmd, depth, kind, cls, op, list) port_census_hit(cmd, depth, kind, cls, op, list)
#define CENSUS_MATERIAL() port_census_material_captured()
#else
#define CENSUS_FRAME_BEGIN(level, loads) ((void) 0)
#define CENSUS_FRAME_END() ((void) 0)
#define CENSUS_HOST_HIT(cmd, depth, kind, cls, op, list) ((void) 0)
#define CENSUS_MATERIAL() ((void) 0)
#endif

#endif
