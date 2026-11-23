#ifndef PORT_HIDDEN_SKIP_H
#define PORT_HIDDEN_SKIP_H
/*
 * P67 (proposals/3ds-hidden-batch-skip-plan.md): track batches that no camera in the current camera cell can see are
 * left out (DKR_HIDDEN_SKIP, default off), from a table baked on the PC (port/src/game/hidden_tables.inc).
 *
 * Game side: port/src/game/hidden_skip.c. Host side (PORT_HIDDEN_BAKE, host build only): the pose log
 * (port/host/hidden_poses.c, DKR_HIDDEN_POSES=1) and the bake (port/host/hidden_bake.c, DKR_HIDDEN_BAKE=<pose file>).
 * This header holds only plain C types so that both sides can include it.
 *
 * A level's table block (little-endian, offsets in bytes from the block's start):
 *   u32 magic 'DKRH'; u16 version; u16 level (courseId)
 *   u32 geomHash (port_hidden_geom_hash)
 *   u16 numSegments; u16 totalBatches; u8 subN; u8 sectors; u16 rowBytes (= ceil(totalBatches / 8))
 *   u16 fovMax100; u16 aspectMax1000; u16 numCells; u16 numRows
 *   u16 firstBatch[numSegments]
 *   u16 cellIndex[numSegments * subN * subN * sectors]   0xFFFF: no cell (draw everything)
 *   HiddenCell cells[numCells]                            22 bytes each
 *   u8 rows[numRows][rowBytes]                            bit (firstBatch[seg] + i): 1 = may show a pixel, draw it
 */
#define HIDDEN_MAGIC 0x484B5244u /* "DKRH" */
#define HIDDEN_VERSION 1
#define HIDDEN_HEADER_BYTES 28
#define HIDDEN_CELL_BYTES 22
#define HIDDEN_NO_CELL 0xFFFF

/* A cell's gates (all must hold for its row to apply) and row. Stored packed, little-endian, in this order. */
typedef struct {
    short box[6]; /* x1, y1, z1, x2, y2, z2: the camera position (y as the drawn camera's, shake included) */
    short pitchMin, pitchMax, rollMin, rollMax;
    unsigned short row;
} HiddenCell;

/* The drawn camera's pose (what func_80067D3C builds the view from) and the cell key it falls in. */
typedef struct {
    int level, seg;
    float x, y, z;
    unsigned short yaw; /* trans.rotation.y_rotation as a u16 (the sectors are its top bits) */
    short pitch, roll;  /* x_rotation + pitch, z_rotation */
    int fov100, aspect1000;
    short segBox[6]; /* segmentsBoundingBoxes[seg] (x1, y1, z1, x2, y2, z2) */
} HiddenPose;

/* The X/Z sub-cell (0 .. n*n-1) of a position in a segment's box split n x n (clamped to the box). */
static inline int hidden_sub_cell(const short segBox[6], float x, float z, int n) {
    int sx, sz;
    float w = (float) (segBox[3] - segBox[0] + 1), d = (float) (segBox[5] - segBox[2] + 1);
    sx = (int) ((x - segBox[0]) * n / w);
    sz = (int) ((z - segBox[2]) * n / d);
    sx = sx < 0 ? 0 : sx >= n ? n - 1 : sx;
    sz = sz < 0 ? 0 : sz >= n ? n - 1 : sz;
    return sz * n + sx;
}

static inline int hidden_sector(unsigned short yaw, int sectors) {
    int shift = sectors == 16 ? 12 : sectors == 8 ? 13 : sectors == 4 ? 14 : 16;
    return shift >= 16 ? 0 : yaw >> shift;
}

/* Why a pose gets no row (the game's log counts them). */
enum {
    HIDDEN_NR_SEGMENT, /* camera segment -1 or out of range */
    HIDDEN_NR_CUTSCENE,
    HIDDEN_NR_FOV,     /* field of view or aspect wider than baked */
    HIDDEN_NR_NO_CELL, /* no cell at the key (no table: the same) */
    HIDDEN_NR_BOX,     /* position outside the cell's box */
    HIDDEN_NR_ANGLE,   /* pitch or roll outside the cell's ranges */
    HIDDEN_NR_PORTRAIT,
    HIDDEN_NR_COUNT
};

/* A level's table, read from its block (hidden_table_open). */
typedef struct {
    const unsigned char *block;
    int numSegments, totalBatches, subN, sectors, rowBytes, numCells, numRows, fovMax100, aspectMax1000;
    const unsigned char *cellIndex, *cells, *rows;
} HiddenTable;

static inline unsigned int hidden_rd16(const unsigned char *p) {
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8);
}

static inline short hidden_rds16(const unsigned char *p) {
    return (short) hidden_rd16(p);
}

/*
 * Reads a block and checks it against the loaded level: its course, the geometry hash, the segment and batch counts
 * and each segment's first batch (firstBatch: numSegments + 1 entries). 0: usable; 1: baked from another model (or
 * not a table); 2: damaged (sizes don't add up); 3: the batch numbering differs.
 */
static inline int hidden_table_open(HiddenTable *t, const unsigned char *b, int size, int level, unsigned int hash,
                                    int numSegments, const unsigned short *firstBatch) {
    int s, total = firstBatch[numSegments];
    if (b == 0 || size < 28 || (hidden_rd16(b) | (hidden_rd16(b + 2) << 16)) != HIDDEN_MAGIC ||
        hidden_rd16(b + 4) != HIDDEN_VERSION || (int) hidden_rd16(b + 6) != level ||
        (hidden_rd16(b + 8) | (hidden_rd16(b + 10) << 16)) != hash || (int) hidden_rd16(b + 12) != numSegments ||
        (int) hidden_rd16(b + 14) != total) {
        return 1;
    }
    t->numSegments = numSegments;
    t->totalBatches = total;
    t->subN = b[16];
    t->sectors = b[17];
    t->rowBytes = hidden_rd16(b + 18);
    t->fovMax100 = hidden_rd16(b + 20);
    t->aspectMax1000 = hidden_rd16(b + 22);
    t->numCells = hidden_rd16(b + 24);
    t->numRows = hidden_rd16(b + 26);
    t->cellIndex = b + 28 + numSegments * 2;
    t->cells = t->cellIndex + numSegments * t->subN * t->subN * t->sectors * 2;
    t->rows = t->cells + t->numCells * HIDDEN_CELL_BYTES;
    if (t->rowBytes != (total + 7) / 8 || t->subN < 1 || t->subN > 4 || t->sectors < 1 || t->sectors > 16 ||
        t->rows + t->numRows * t->rowBytes > b + size) {
        return 2;
    }
    for (s = 0; s < numSegments; s++) {
        if (hidden_rd16(b + 28 + s * 2) != firstBatch[s]) {
            return 3;
        }
    }
    t->block = b;
    return 0;
}

/* The row a table gives this pose: one bit a batch (firstBatch[seg] + i), 1 = draw it. NULL (and *reason) when the
 * pose is outside every gate of its cell, or has no cell. */
static inline const unsigned char *hidden_table_row(const HiddenTable *t, const HiddenPose *p, int *reason) {
    int sub, sector, key;
    unsigned int cell;
    const unsigned char *c;
    if (p->seg < 0 || p->seg >= t->numSegments) {
        *reason = HIDDEN_NR_SEGMENT;
        return 0;
    }
    if (p->fov100 > t->fovMax100 || p->aspect1000 > t->aspectMax1000) {
        *reason = HIDDEN_NR_FOV;
        return 0;
    }
    sub = hidden_sub_cell(p->segBox, p->x, p->z, t->subN);
    sector = hidden_sector(p->yaw, t->sectors);
    key = (p->seg * t->subN * t->subN + sub) * t->sectors + sector;
    cell = hidden_rd16(t->cellIndex + key * 2);
    if (cell == HIDDEN_NO_CELL || (int) cell >= t->numCells) {
        *reason = HIDDEN_NR_NO_CELL;
        return 0;
    }
    c = t->cells + cell * HIDDEN_CELL_BYTES;
    if (p->x < hidden_rds16(c + 0) || p->y < hidden_rds16(c + 2) || p->z < hidden_rds16(c + 4) ||
        p->x > hidden_rds16(c + 6) || p->y > hidden_rds16(c + 8) || p->z > hidden_rds16(c + 10)) {
        *reason = HIDDEN_NR_BOX;
        return 0;
    }
#ifdef HIDDEN_TABLE_TEST_BREAK
    // port/tools/hidden_table_test.c broken on purpose (rule K6): the pitch gate left out
    if (p->roll < hidden_rds16(c + 16) || p->roll > hidden_rds16(c + 18) || (int) hidden_rd16(c + 20) >= t->numRows) {
#else
    if (p->pitch < hidden_rds16(c + 12) || p->pitch > hidden_rds16(c + 14) || p->roll < hidden_rds16(c + 16) ||
        p->roll > hidden_rds16(c + 18) || (int) hidden_rd16(c + 20) >= t->numRows) {
#endif
        *reason = HIDDEN_NR_ANGLE;
        return 0;
    }
    return t->rows + hidden_rd16(c + 20) * t->rowBytes;
}

static inline int hidden_row_bit(const unsigned char *row, int bit) {
    return (row[bit >> 3] >> (bit & 7)) & 1;
}

/* port/host/hidden_poses.c (DKR_HIDDEN_POSES=1): one drawn race frame's camera pose. */
void hidden_poses_frame(const HiddenPose *pose);

/* What the bake reads of a level (filled by the game side, hidden_skip.c). */
typedef struct {
    const void *verts; /* Vertex (x, y, z s16, then 4 colour bytes) */
    const void *tris;  /* Triangle (flags, vi0, vi1, vi2, then 3 uv words) */
    int numVerts, numTris;
    int kind; /* HIDDEN_BATCH_* */
} HiddenBatch;
#define HIDDEN_BATCH_OCCLUDER 1 /* opaque pass, no cut-outs, decals, blending, vertex alpha or water: hides others */
#define HIDDEN_BATCH_OPAQUE 2   /* drawn in the opaque pass (else the see-through pass) */
#define HIDDEN_BATCH_ALWAYS 4   /* never decided by the table (RENDER_HIDDEN, water): its bit is always 1 */

typedef struct {
    int level, numSegments, totalBatches;
    unsigned int geomHash;
    const short *segBoxes;        /* numSegments * 6 */
    const int *firstBatch;        /* numSegments + 1 */
    const HiddenBatch *batches;   /* totalBatches */
    const unsigned char *allowed; /* numSegments * numSegments: [camera seg][seg] Rare's row says the seg may be drawn */
    float near, far;              /* the game's camera planes */
} HiddenLevel;

/* The game's view-projection for a camera at the origin with this rotation (the game's own matrix code), at this
 * vertical field of view (degrees) and aspect, written as 16 floats (row vector convention, as gViewProjMatrixF). */
void port_hidden_view_proj(unsigned short yaw, short pitch, short roll, float fovy, float aspect, float near,
                           float far, float out[16]);

/* port/host/hidden_bake.c (DKR_HIDDEN_BAKE=<pose file>): bakes this level if the pose file has it; 1 when it did. */
int hidden_bake_level(const HiddenLevel *level);
/* The host's table override (DKR_HIDDEN_TABLE_DIR: <dir>/hidden_<level>.bin). */
const unsigned char *hidden_host_table(int level, int *size);
/* The cell size evaluation (DKR_HIDDEN_EVAL=<dir>,<dir>,...: each dir's hidden_<level>.bin; the first one skips,
 * every one is counted and checked): how many, and table k for a level (its name: the dir's last part). */
int hidden_host_eval_count(void);
const unsigned char *hidden_host_eval_table(int k, int level, int *size, char *name, int nameSize);
void port_hidden_exit_log(void);

#endif
