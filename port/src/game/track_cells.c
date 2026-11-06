/*
 * Track face cell maps (DKR_NATIVE_CELLS, default on; 0 = the original loops).
 *
 * The game splits each track segment's bounding box into an 8x8 grid and gives every face a 16-bit mask of the
 * columns (low byte) and rows (high byte) its box touches (unk10, made once at level load by func_8002C954). Its face
 * searches (collision candidates, ground height, water height, shadows) then walk every face of every batch and test
 * that mask. This keeps, for each batch, a 64-bit map of the grid cells (column, row) that any of its faces touches,
 * so a search can skip a batch none of whose faces could pass its mask test. Faces in the batches that are left are
 * tested exactly as before, in the same order, so the results are the same bit for bit.
 */
#include "types.h"
#include "structs.h"
#include "plat/plat.h"
#include "port_track_cells.h"

static LevelModel *sModel;
static u64 *sMaps;      // every batch's map, segment after segment
static s32 *sFirst;     // each segment's first map
static s32 sSegments;
static s32 sEpoch = -1;
static s32 sOn;

static u64 cells_of_mask(s32 mask) {
    u64 cells = 0;
    u32 cols = mask & 0xFF, rows = (mask >> 8) & 0xFF;
    s32 c;
    for (c = 0; c < 8; c++) {
        if (cols & (1u << c)) {
            cells |= (u64) rows << (c * 8);
        }
    }
    return cells;
}

void port_cells_build(LevelModel *model) {
    s32 total = 0, k, b, f, at;
    plat_free(sMaps);
    plat_free(sFirst);
    sMaps = NULL;
    sFirst = NULL;
    sModel = NULL;
    if (model == NULL || model->numberOfSegments <= 0) {
        return;
    }
    for (k = 0; k < model->numberOfSegments; k++) {
        total += model->segments[k].numberOfBatches;
    }
    sMaps = plat_malloc(sizeof(u64) * (total + 1));
    sFirst = plat_malloc(sizeof(s32) * model->numberOfSegments);
    if (sMaps == NULL || sFirst == NULL) {
        plat_free(sMaps);
        plat_free(sFirst);
        sMaps = NULL;
        sFirst = NULL;
        return;
    }
    at = 0;
    for (k = 0; k < model->numberOfSegments; k++) {
        LevelModelSegment *seg = &model->segments[k];
        sFirst[k] = at;
        for (b = 0; b < seg->numberOfBatches; b++) {
            u64 cells = 0;
            for (f = seg->batches[b].facesOffset; f < seg->batches[b + 1].facesOffset; f++) {
                cells |= cells_of_mask(seg->unk10[f]);
            }
            sMaps[at++] = cells;
        }
    }
    sSegments = model->numberOfSegments;
    sModel = model;
}

const u64 *port_cells_segment_maps(LevelModel *model, LevelModelSegment *seg) {
    s32 k;
    if (sEpoch != gPlatSwitchEpoch) {
        sEpoch = gPlatSwitchEpoch;
        sOn = plat_switch("DKR_NATIVE_CELLS", 1);
    }
    if (!sOn || model != sModel || sMaps == NULL) {
        return NULL;
    }
    k = seg - model->segments;
    if (k < 0 || k >= sSegments) {
        return NULL;
    }
    return &sMaps[sFirst[k]];
}

u64 port_cells_any(s32 mask) {
    return cells_of_mask(mask);
}

u64 port_cells_all(s32 mask) {
    // A face passes (unk10 & mask) == mask only if it has every bit of the mask, so the cell of the mask's lowest
    // column and lowest row. Without a column or a row, nothing can be skipped (all ones).
    u32 cols = mask & 0xFF, rows = (mask >> 8) & 0xFF;
    if (cols == 0 || rows == 0) {
        return ~(u64) 0;
    }
    return (u64) (rows & -rows) << (__builtin_ctz(cols) * 8);
}
