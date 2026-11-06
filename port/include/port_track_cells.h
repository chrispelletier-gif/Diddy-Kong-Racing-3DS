#ifndef PORT_TRACK_CELLS_H
#define PORT_TRACK_CELLS_H
/* Track face cell maps for the game's face searches (port/src/game/track_cells.c, DKR_NATIVE_CELLS). */
#include "types.h"
#include "structs.h"

/* Builds the maps of a level model whose face masks (unk10) are made. */
void port_cells_build(LevelModel *model);
/* The segment's maps, one per batch, or NULL (switch off, or not the current model): then search as before. */
const u64 *port_cells_segment_maps(LevelModel *model, LevelModelSegment *seg);
/* The cells a batch must touch for a face to pass ((unk10 & mask) low and high bytes both non-zero). */
u64 port_cells_any(s32 mask);
/* The same for (unk10 & mask) == mask: all ones when the mask has no column or no row (nothing can be skipped). */
u64 port_cells_all(s32 mask);
/* Whether a search may skip batch b: maps from port_cells_segment_maps, need from port_cells_any/_all. */
#define PORT_CELLS_SKIP(maps, b, need) ((maps) != NULL && (need) != ~(u64) 0 && !((maps)[b] & (need)))
#endif
