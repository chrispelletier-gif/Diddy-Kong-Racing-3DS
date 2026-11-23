/*
 * DKR_HIDDEN_POSES=1 (host only, P67 proposals/3ds-hidden-batch-skip-plan.md section 3.2): the pose log the hidden
 * batch bake samples from. Every drawn race frame's camera pose (hidden_skip.c sends it: the same pose func_80067D3C
 * builds the view from, outside cutscenes, before the finish) is added to its finest key, (level, camera segment, 4x4
 * X/Z sub-cell of the segment's box, 16 yaw sectors): frames, the position box, the pitch and roll ranges and the
 * widest field of view and aspect. Coarser cells are unions of these, so one log serves every cell size.
 *
 * Written when a level ends, every 600 logged frames and at exit, appended to hidden_poses.txt in DKR_DATA_DIR (or DKR_HIDDEN_POSES_FILE), one
 * line a key: level seg sub sector frames x1 y1 z1 x2 y2 z2 pmin pmax rmin rmax fov100 aspect1000
 * port/tools/hidden_bake.py merges the logs of several runs (same key: frames added, ranges joined).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "plat/plat.h"
#include "port_hidden_skip.h"

#define HP_MAX_KEYS 8192

typedef struct {
    int used, seg, sub, sector, frames;
    float box[6];
    short pmin, pmax, rmin, rmax;
    int fov100, aspect1000;
} HpKey;

static int sOn = -1, sLevel = -1, sCount, sAtExit, sSinceFlush, sQuiet;
static HpKey *sKeys;

static int on(void) {
    if (sOn < 0) {
        const char *e = getenv("DKR_HIDDEN_POSES");
        sOn = e != NULL && atoi(e) != 0;
        if (sOn) {
            sKeys = calloc(HP_MAX_KEYS, sizeof(HpKey));
            sOn = sKeys != NULL;
        }
    }
    return sOn;
}

static void flush(void) {
    char path[512];
    const char *file = getenv("DKR_HIDDEN_POSES_FILE"), *dir = getenv("DKR_DATA_DIR");
    FILE *f;
    int i, n = 0;
    if (sCount == 0) {
        return;
    }
    if (file != NULL) {
        snprintf(path, sizeof(path), "%s", file);
    } else {
        snprintf(path, sizeof(path), "%s/hidden_poses.txt", dir != NULL ? dir : ".");
    }
    f = fopen(path, "a");
    if (f == NULL) {
        plat_log("hidden poses: can't write %s", path);
        return;
    }
    for (i = 0; i < HP_MAX_KEYS; i++) {
        HpKey *k = &sKeys[i];
        if (!k->used) {
            continue;
        }
        fprintf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", sLevel, k->seg, k->sub, k->sector, k->frames,
                (int) floorf(k->box[0]), (int) floorf(k->box[1]), (int) floorf(k->box[2]), (int) ceilf(k->box[3]),
                (int) ceilf(k->box[4]), (int) ceilf(k->box[5]), k->pmin, k->pmax, k->rmin, k->rmax, k->fov100,
                k->aspect1000);
        n++;
    }
    fclose(f);
    if (!sQuiet++ || sCount >= HP_MAX_KEYS * 3 / 4) {
        plat_log("hidden poses: level %d, %d keys written to %s%s", sLevel, n, path,
                 sCount >= HP_MAX_KEYS * 3 / 4 ? " (key table full: some frames left out)" : "");
    }
    memset(sKeys, 0, sizeof(HpKey) * HP_MAX_KEYS);
    sCount = 0;
}

static void at_exit(void) {
    flush();
}

void hidden_poses_frame(const HiddenPose *p) {
    unsigned int h;
    int sub, sector;
    HpKey *k;
    if (!on()) {
        return;
    }
    if (!sAtExit) {
        atexit(at_exit);
        sAtExit = 1;
    }
    if (p->level != sLevel) {
        flush();
        sLevel = p->level;
    }
    sub = hidden_sub_cell(p->segBox, p->x, p->z, 4);
    sector = hidden_sector(p->yaw, 16);
    h = ((unsigned int) p->seg * 2654435761u) ^ ((unsigned int) (sub * 16 + sector) * 40503u);
    h &= HP_MAX_KEYS - 1;
    while (sKeys[h].used && !(sKeys[h].seg == p->seg && sKeys[h].sub == sub && sKeys[h].sector == sector)) {
        h = (h + 1) & (HP_MAX_KEYS - 1);
    }
    k = &sKeys[h];
    if (!k->used) {
        if (sCount >= HP_MAX_KEYS * 3 / 4) {
            return;
        }
        k->used = 1;
        k->seg = p->seg;
        k->sub = sub;
        k->sector = sector;
        k->box[0] = k->box[3] = p->x;
        k->box[1] = k->box[4] = p->y;
        k->box[2] = k->box[5] = p->z;
        k->pmin = k->pmax = p->pitch;
        k->rmin = k->rmax = p->roll;
        sCount++;
    }
    k->frames++;
    if (++sSinceFlush >= 600) {
        // Every 600 frames too, so a run that is stopped keeps its poses (the merge adds a key's lines up)
        sSinceFlush = 0;
        flush();
    }
    k->box[0] = p->x < k->box[0] ? p->x : k->box[0];
    k->box[1] = p->y < k->box[1] ? p->y : k->box[1];
    k->box[2] = p->z < k->box[2] ? p->z : k->box[2];
    k->box[3] = p->x > k->box[3] ? p->x : k->box[3];
    k->box[4] = p->y > k->box[4] ? p->y : k->box[4];
    k->box[5] = p->z > k->box[5] ? p->z : k->box[5];
    k->pmin = p->pitch < k->pmin ? p->pitch : k->pmin;
    k->pmax = p->pitch > k->pmax ? p->pitch : k->pmax;
    k->rmin = p->roll < k->rmin ? p->roll : k->rmin;
    k->rmax = p->roll > k->rmax ? p->roll : k->rmax;
    k->fov100 = p->fov100 > k->fov100 ? p->fov100 : k->fov100;
    k->aspect1000 = p->aspect1000 > k->aspect1000 ? p->aspect1000 : k->aspect1000;
}
