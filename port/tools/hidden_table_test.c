/*
 * P67 hidden batch table lookup (port/include/port_hidden_skip.h, used by port/src/game/hidden_skip.c): a small table
 * built in memory (2 segments, 9 batches, 2x2 sub-cells, 4 sectors, 3 cells) and the game's lookup against it: the key
 * (segment, sub-cell, sector), every gate (box on each side, pitch, roll, field of view, aspect, segment -1 and out of
 * range, an empty slot), the row bits, and the checks that refuse a table (another model's hash, another level, a
 * damaged block, another batch numbering).
 *
 *   gcc -Iport/include -o hidden_table_test port/tools/hidden_table_test.c && ./hidden_table_test
 * Rule K6: built with -DHIDDEN_TABLE_TEST_BREAK the lookup leaves the pitch gate out; the test must then fail.
 */
#include <stdio.h>
#include <string.h>
#include "port_hidden_skip.h"

static unsigned char sBlock[1024];
static int sSize, sFailed, sRun;

static void put16(unsigned char **p, int v) {
    (*p)[0] = (unsigned char) (v & 0xFF);
    (*p)[1] = (unsigned char) ((v >> 8) & 0xFF);
    *p += 2;
}

#define HASH 0x12345678u
#define LEVEL 8
static const unsigned short sFirst[3] = { 0, 5, 9 };
static const short sSeg0Box[6] = { 0, 0, 0, 999, 199, 999 }, sSeg1Box[6] = { 1000, 0, 0, 1999, 199, 999 };

/* Cells: 0 = seg 0, sub 0 (x < 500, z < 500), sector 1; 1 = seg 0, sub 3 (x >= 500, z >= 500), sector 1;
 * 2 = seg 1, sub 0, sector 3. Rows: 0 leaves out batches 2 and 3; 1 leaves out batch 7; cell 1 shares row 0. */
static void build(void) {
    unsigned char *p = sBlock;
    int s, i, slots = 2 * 2 * 2 * 4;
    unsigned short index[32];
    struct {
        short box[6], pmin, pmax, rmin, rmax;
        int row;
    } cells[3] = {
        { { 100, 10, 100, 400, 60, 400 }, -1000, 1000, -200, 200, 0 },
        { { 600, 10, 600, 900, 60, 900 }, -1000, 1000, -200, 200, 0 },
        { { 1100, 10, 100, 1400, 60, 400 }, 0, 3000, 0, 0, 1 },
    };
    for (i = 0; i < slots; i++) {
        index[i] = HIDDEN_NO_CELL;
    }
    index[(0 * 4 + 0) * 4 + 1] = 0;
    index[(0 * 4 + 3) * 4 + 1] = 1;
    index[(1 * 4 + 0) * 4 + 3] = 2;
    put16(&p, HIDDEN_MAGIC & 0xFFFF);
    put16(&p, HIDDEN_MAGIC >> 16);
    put16(&p, HIDDEN_VERSION);
    put16(&p, LEVEL);
    put16(&p, HASH & 0xFFFF);
    put16(&p, HASH >> 16);
    put16(&p, 2);
    put16(&p, 9);
    *p++ = 2;
    *p++ = 4;
    put16(&p, 2);
    put16(&p, 6000);
    put16(&p, 1667);
    put16(&p, 3);
    put16(&p, 2);
    for (s = 0; s < 2; s++) {
        put16(&p, sFirst[s]);
    }
    for (i = 0; i < slots; i++) {
        put16(&p, index[i]);
    }
    for (i = 0; i < 3; i++) {
        for (s = 0; s < 6; s++) {
            put16(&p, cells[i].box[s]);
        }
        put16(&p, cells[i].pmin);
        put16(&p, cells[i].pmax);
        put16(&p, cells[i].rmin);
        put16(&p, cells[i].rmax);
        put16(&p, cells[i].row);
    }
    // Row 0: all but batches 2, 3; row 1: all but 7
    *p++ = 0xF3;
    *p++ = 0x01;
    *p++ = 0x7F;
    *p++ = 0x01;
    sSize = (int) (p - sBlock);
}

static void expect(const char *what, int got, int want) {
    sRun++;
    if (got != want) {
        printf("FAILED: %s: got %d, want %d\n", what, got, want);
        sFailed++;
    }
}

static HiddenPose pose(int seg, float x, float y, float z, unsigned short yaw, short pitch, short roll) {
    HiddenPose p;
    memset(&p, 0, sizeof(p));
    p.level = LEVEL;
    p.seg = seg;
    p.x = x;
    p.y = y;
    p.z = z;
    p.yaw = yaw;
    p.pitch = pitch;
    p.roll = roll;
    p.fov100 = 6000;
    p.aspect1000 = 1667;
    memcpy(p.segBox, seg == 1 ? sSeg1Box : sSeg0Box, sizeof(p.segBox));
    return p;
}

/* The row's index (0 or 1), or -1 - reason when there is none. */
static int lookup(const HiddenTable *t, HiddenPose p) {
    int reason = -1;
    const unsigned char *row = hidden_table_row(t, &p, &reason);
    return row != NULL ? (int) ((row - t->rows) / t->rowBytes) : -1 - reason;
}

int main(void) {
    HiddenTable t;
    unsigned short other[3] = { 0, 4, 9 };
    const unsigned char *row;
    int reason;
    HiddenPose p;
    build();

    // Opening
    expect("open", hidden_table_open(&t, sBlock, sSize, LEVEL, HASH, 2, sFirst), 0);
    expect("open: another model's hash", hidden_table_open(&t, sBlock, sSize, LEVEL, HASH ^ 1, 2, sFirst), 1);
    expect("open: another level", hidden_table_open(&t, sBlock, sSize, LEVEL + 1, HASH, 2, sFirst), 1);
    expect("open: cut short", hidden_table_open(&t, sBlock, sSize - 1, LEVEL, HASH, 2, sFirst), 2);
    expect("open: another batch numbering", hidden_table_open(&t, sBlock, sSize, LEVEL, HASH, 2, other), 3);
    expect("open again", hidden_table_open(&t, sBlock, sSize, LEVEL, HASH, 2, sFirst), 0);

    // Keys: sector 1 is yaw 0x4000-0x7FFF, sector 3 is 0xC000-0xFFFF; sub 3 is x, z >= 500 in segment 0's box
    expect("cell 0", lookup(&t, pose(0, 200, 30, 200, 0x5000, 0, 0)), 0);
    expect("cell 1 (sub-cell 3)", lookup(&t, pose(0, 700, 30, 700, 0x4000, 0, 0)), 0);
    expect("cell 2 (segment 1, sector 3)", lookup(&t, pose(1, 1200, 30, 200, 0xFFFF, 100, 0)), 1);
    expect("other sector: no cell", lookup(&t, pose(0, 200, 30, 200, 0x8000, 0, 0)), -1 - HIDDEN_NR_NO_CELL);
    expect("other sub-cell: no cell", lookup(&t, pose(0, 700, 30, 200, 0x5000, 0, 0)), -1 - HIDDEN_NR_NO_CELL);
    expect("segment -1", lookup(&t, pose(-1, 200, 30, 200, 0x5000, 0, 0)), -1 - HIDDEN_NR_SEGMENT);
    expect("segment out of range", lookup(&t, pose(2, 200, 30, 200, 0x5000, 0, 0)), -1 - HIDDEN_NR_SEGMENT);

    // Gates
    expect("box: x below", lookup(&t, pose(0, 99, 30, 200, 0x5000, 0, 0)), -1 - HIDDEN_NR_BOX);
    expect("box: x above", lookup(&t, pose(0, 400.5f, 30, 200, 0x5000, 0, 0)), -1 - HIDDEN_NR_BOX);
    expect("box: y below", lookup(&t, pose(0, 200, 9, 200, 0x5000, 0, 0)), -1 - HIDDEN_NR_BOX);
    expect("box: y above", lookup(&t, pose(0, 200, 61, 200, 0x5000, 0, 0)), -1 - HIDDEN_NR_BOX);
    expect("box: z below", lookup(&t, pose(0, 200, 30, 99, 0x5000, 0, 0)), -1 - HIDDEN_NR_BOX);
    expect("box: z above", lookup(&t, pose(0, 200, 30, 401, 0x5000, 0, 0)), -1 - HIDDEN_NR_BOX);
    expect("box: on the edges", lookup(&t, pose(0, 100, 10, 400, 0x5000, 0, 0)), 0);
    expect("pitch below", lookup(&t, pose(0, 200, 30, 200, 0x5000, -1001, 0)), -1 - HIDDEN_NR_ANGLE);
    expect("pitch above", lookup(&t, pose(0, 200, 30, 200, 0x5000, 1001, 0)), -1 - HIDDEN_NR_ANGLE);
    expect("pitch below (cell 2)", lookup(&t, pose(1, 1200, 30, 200, 0xF000, -1, 0)), -1 - HIDDEN_NR_ANGLE);
    expect("roll below", lookup(&t, pose(0, 200, 30, 200, 0x5000, 0, -201)), -1 - HIDDEN_NR_ANGLE);
    expect("roll above", lookup(&t, pose(0, 200, 30, 200, 0x5000, 0, 201)), -1 - HIDDEN_NR_ANGLE);
    p = pose(0, 200, 30, 200, 0x5000, 0, 0);
    p.fov100 = 6001;
    expect("field of view wider", lookup(&t, p), -1 - HIDDEN_NR_FOV);
    p.fov100 = 5000;
    expect("field of view narrower", lookup(&t, p), 0);
    p.aspect1000 = 1668;
    expect("aspect wider", lookup(&t, p), -1 - HIDDEN_NR_FOV);

    // Row bits
    reason = -1;
    row = hidden_table_row(&t, &(HiddenPose) { 0 }, &reason);
    expect("all-zero pose (segment 0, sub 0, sector 0): no cell", row == NULL && reason == HIDDEN_NR_NO_CELL, 1);
    p = pose(0, 200, 30, 200, 0x5000, 0, 0);
    row = hidden_table_row(&t, &p, &reason);
    expect("row 0 batch 1", row != NULL && hidden_row_bit(row, 1), 1);
    expect("row 0 batch 2 (left out)", row != NULL && hidden_row_bit(row, 2), 0);
    expect("row 0 batch 3 (left out)", row != NULL && hidden_row_bit(row, 3), 0);
    expect("row 0 batch 8", row != NULL && hidden_row_bit(row, 8), 1);
    p = pose(1, 1200, 30, 200, 0xFFFF, 100, 0);
    row = hidden_table_row(&t, &p, &reason);
    expect("row 1 batch 7 (left out)", row != NULL && hidden_row_bit(row, 7), 0);
    expect("row 1 batch 6", row != NULL && hidden_row_bit(row, 6), 1);

    // Sub-cells and sectors
    expect("sub-cell clamps below", hidden_sub_cell(sSeg0Box, -50, -50, 2), 0);
    expect("sub-cell clamps above", hidden_sub_cell(sSeg0Box, 5000, 5000, 2), 3);
    expect("sub-cell 4x4", hidden_sub_cell(sSeg0Box, 260, 760, 4), 3 * 4 + 1);
    expect("sector 16", hidden_sector(0xFFFF, 16), 15);
    expect("sector 8", hidden_sector(0x2000, 8), 1);
    expect("sector 1", hidden_sector(0x8000, 1), 0);

    printf("hidden table test: %d checks, %s\n", sRun, sFailed ? "FAILED" : "ok");
    return sFailed != 0;
}
