/*
 * The race HUD on the 3DS bottom screen (roadmap G9): once a frame (src/thread3_main.c), during a one player race,
 * this copies what the HUD shows (position, lap, times, item, bananas) and where every racer is on the track's map
 * into a RaceHud (plat/race_hud.h) for the bottom screen to draw, and works out the time gaps between the racers.
 *
 * A gap is measured at the track's checkpoints: the race clock is noted as each racer first reaches each one, and the
 * gap to another racer is the difference at the newest checkpoint both have reached, as race broadcasts do.
 *
 * Where the HUD goes is a setting, racehud= in settings.txt (DKR_RACE_HUD in env.txt for a test): 1 the bottom screen
 * (the 3DS's default; the top screen keeps only the countdown, lap announcements, wrong way, direction arrows and the
 * finish), 2 both screens, 0 the top screen only, as on the N64 (the default on PC, which has no bottom screen).
 * src/game_ui.c asks port_race_hud_hides() which of its pieces to leave out.
 */
#include <ultra64.h>
#include "types.h"
#include "structs.h"
#include "game.h"
#include "game_ui.h"
#include "menu.h"
#include "objects.h"
#include "thread3_main.h"
#include "tracks.h"
#include "math_util.h"
#include "port.h"
#include "plat/plat.h"
#include "plat/race_hud.h"
#include "plat/settings.h"
#include "asset_enums.h"
#include "textures_sprites.h"
#include <stdlib.h>
extern CheckpointNode *gTrackCheckpoints;
extern s32 gNumberOfMainCheckpoints, gNumberOfTotalCheckpoints;

/* The bottom screen's side (plat_3ds.c; on PC bottom_preview.c draws it for DKR_BOTTOM_RACE). */
void plat_set_race_hud(const RaceHud *hud);
/* Whether there is a bottom screen to show the race HUD on. */
int plat_race_hud_screen(void);
int plat_race_hud_covered(void);
void plat_set_bottom_style(int style);

extern s32 gMinimapDotOffsetX;
extern s16 gTTGhostTimeToBeat; // T.T.'s time on this track (objects.c), while his ghost is out.

/* How many checkpoints a racer's gaps can be timed at: (laps + 3) * checkpoints is the most a race counts. */
#define MAX_STAMPS 768

static volatile s32 sSetting = -1; // RACE_HUD_*, -1 until loaded.
static s32 sActive;                // The bottom screen is showing this race's HUD.
static s32 sMenuShown;             // It is showing a menu's companion (menu_companion).
static s32 sMapSprite = -1;        // The current track's map, as minimap_init loaded it.
static void *sHubMapModel;         // The hub whose own map is drawn (below).
static void *sMapModel;            // The level model it belongs to: no map until the new level's is loaded.
static s32 sClock;                 // 60ths of a second since the start.
static s32 sTracking, sTrackedMode; // In a race (of this RACE_HUD_MODE_), keeping the clock and checkpoints.
static s16 sReached[RACE_HUD_RACERS];
static s32 sStamps[RACE_HUD_RACERS][MAX_STAMPS];

enum { RACE_HUD_TOP, RACE_HUD_BOTTOM, RACE_HUD_BOTH };

s32 port_race_hud_setting(void) {
    if (sSetting < 0) {
        const char *env = plat_getenv("DKR_RACE_HUD");
        // Without a bottom screen (PC, unless DKR_BOTTOM_RACE wants its pictures) the N64's HUD.
        s32 fallback = plat_race_hud_screen() ? RACE_HUD_BOTTOM : RACE_HUD_TOP;
        s32 value = env != NULL ? atoi(env) : settings_get("racehud", fallback);
        sSetting = (value < RACE_HUD_TOP || value > RACE_HUD_BOTH) ? fallback : value;
    }
    return sSetting;
}

void port_set_race_hud_setting(s32 value) {
    sSetting = value;
    settings_set("racehud", value);
}

static s32 hub_map_ready(LevelModel *model);

/*
 * Called by minimap_init (src/game_ui.c) with the track's map sprite, before it is loaded over the index. A hub's own
 * map is drawn here, during the load, rather than on its first frame (a stall there on the Old 3DS).
 */
void port_race_hud_map(void *model, s32 spriteIndex) {
    sMapModel = model;
    sMapSprite = spriteIndex;
    sHubMapModel = NULL; // A new level: a hub's own map is drawn again.
    if (model != NULL && level_type() == RACETYPE_HUBWORLD && port_race_hud_setting() != RACE_HUD_TOP) {
        hub_map_ready(model);
    }
}

/* Whether game_ui.c leaves this HUD element (HUD_*, or -1 for the track map itself) off the top screen. */
s32 port_race_hud_hides(s32 element) {
    // While the bottom screen's menu covers the race HUD, the top screen keeps its own.
    if (!sActive || port_race_hud_setting() != RACE_HUD_BOTTOM || plat_race_hud_covered()) {
        return FALSE;
    }
    switch (element) {
        case -1:
        case HUD_RACE_POSITION:
        case HUD_RACE_POSITION_END:
        case HUD_WEAPON_DISPLAY:
        case HUD_WEAPON_QUANTITY:
        case HUD_LAP_COUNT_LABEL:
        case HUD_LAP_COUNT_CURRENT:
        case HUD_LAP_COUNT_SEPERATOR:
        case HUD_LAP_COUNT_TOTAL:
        case HUD_LAP_COUNT_FLAG:
        case HUD_BANANA_COUNT_ICON_SPIN:
        case HUD_BANANA_COUNT_NUMBER_1:
        case HUD_BANANA_COUNT_NUMBER_2:
        case HUD_BANANA_COUNT_X:
        case HUD_BANANA_COUNT_ICON_STATIC:
        case HUD_BANANA_COUNT_SPARKLE:
        case HUD_RACE_TIME_LABEL:
        case HUD_RACE_TIME_NUMBER:
        case HUD_MINIMAP_MARKER:
        case HUD_SPEEDOMETRE_ARROW:
        case HUD_SPEEDOMETRE_0:
        case HUD_SPEEDOMETRE_30:
        case HUD_SPEEDOMETRE_60:
        case HUD_SPEEDOMETRE_90:
        case HUD_SPEEDOMETRE_120:
        case HUD_SPEEDOMETRE_150:
        case HUD_SPEEDOMETRE_BG:
        case HUD_SILVER_COIN_TALLY:
        case HUD_TREASURE_METRE:
        case HUD_CHALLENGE_PORTRAIT:
        case HUD_EGG_CHALLENGE_ICON:
        case HUD_BATTLE_BANANA_ICON:
        case HUD_BATTLE_BANANA_X:
        case HUD_BATTLE_BANANA_COUNT_1:
        case HUD_BATTLE_BANANA_COUNT_2:
        case HUD_BALLOON_COUNT_ICON:
        case HUD_BALLOON_COUNT_X:
        case HUD_BALLOON_COUNT_NUMBER_1:
        case HUD_BALLOON_COUNT_NUMBER_2:
            return TRUE;
    }
    return FALSE;
}

/*
 * What the bottom screen has a HUD for (RACE_HUD_MODE_*), or -1: anything one player drives in, but not a cutscene
 * (and on the N64's screen only, the two to four player games).
 */
static s32 hud_mode(Object *player) {
    if (player == NULL || get_game_mode() != GAMEMODE_INGAME || cutscene_id() == 10 ||
        get_number_of_active_players() != 1) {
        return -1;
    }
    if (is_taj_challenge()) {
        return RACE_HUD_MODE_TAJ;
    }
    switch (level_type()) {
        case RACETYPE_DEFAULT:
        case RACETYPE_HORSESHOE_GULCH:
            return is_in_time_trial() ? RACE_HUD_MODE_TIME_TRIAL : RACE_HUD_MODE_RACE;
        case RACETYPE_BOSS:
            return RACE_HUD_MODE_BOSS;
        case RACETYPE_CHALLENGE_BANANAS:
            return RACE_HUD_MODE_BANANAS;
        case RACETYPE_CHALLENGE_BATTLE:
            return RACE_HUD_MODE_BATTLE;
        case RACETYPE_CHALLENGE_EGGS:
            return RACE_HUD_MODE_EGGS;
        case RACETYPE_HUBWORLD:
            return RACE_HUD_MODE_HUB;
    }
    return -1;
}

/*
 * The hubs' own maps. The ROM has no map sprite for a hub (its model's map fields are left over from a track), so the
 * map is drawn from the hub's geometry: its ground, seen from above, lighter where it is higher. Positions on it are
 * pixels (times 4) of this picture, north up like the track maps: -z (where a kart at heading 0 drives) is up. In
 * Adventure 2 the world is mirrored left to right, and so is the picture.
 */
enum { HUB_MAP_SIZE = 160 };
static unsigned char *sHubMaps[2];
static s32 sHubMapUse;
static s32 sHubMapW, sHubMapH, sHubMapMirrored, sHubMapId;
static f32 sHubMapScale;
static s32 sHubPicture; // This frame's positions are on the hub's picture (set before any map_position).

/* The picture's x for a world x. */
static f32 hub_map_x(LevelModel *model, f32 x) {
    return (sHubMapMirrored ? model->upperXBounds - x : x - model->lowerXBounds) * sHubMapScale;
}

static void hub_map_build(LevelModel *model, s32 mirrored) {
    s32 width = model->upperXBounds - model->lowerXBounds, depth = model->upperZBounds - model->lowerZBounds;
    s32 seg, b, t, x, y, n;
    s32 lowY = 0x7FFF, highY = -0x7FFF;
    unsigned char *rgba;
    s16 *height;
    sHubMapModel = NULL;
    if (width <= 0 || depth <= 0) {
        return;
    }
    height = malloc(HUB_MAP_SIZE * HUB_MAP_SIZE * sizeof(s16));
    if (sHubMaps[sHubMapUse ^ 1] == NULL) {
        sHubMaps[sHubMapUse ^ 1] = malloc(HUB_MAP_SIZE * HUB_MAP_SIZE * 4);
    }
    rgba = sHubMaps[sHubMapUse ^ 1];
    if (rgba == NULL || height == NULL) {
        free(height);
        return;
    }
    // Drawn into the buffer that isn't on show, then switched to.
    sHubMapUse ^= 1;
    sHubMapId++;
    sHubMapMirrored = mirrored;
    sHubMapScale = (f32) (HUB_MAP_SIZE - 1) / (width > depth ? width : depth);
    sHubMapW = (s32) (width * sHubMapScale) + 1;
    sHubMapH = (s32) (depth * sHubMapScale) + 1;
    for (n = 0; n < sHubMapW * sHubMapH * 4; n++) {
        rgba[n] = 0;
    }
    for (n = 0; n < HUB_MAP_SIZE * HUB_MAP_SIZE; n++) {
        height[n] = -0x7FFF;
    }
    for (seg = 0; seg < model->numberOfSegments; seg++) {
        LevelModelSegment *segment = &model->segments[seg];
        for (b = 0; b < segment->numberOfBatches; b++) {
            TriangleBatchInfo *batch = &segment->batches[b];
            s32 end = segment->batches[b + 1].facesOffset;
            // Water and what the level renderer never draws (hidden collision) are not ground anyone sees.
            if (batch->flags & (RENDER_WATER | RENDER_HIDDEN)) {
                continue;
            }
            for (t = batch->facesOffset; t < end; t++) {
                Triangle *tri = &segment->triangles[t];
                Vertex *v[3];
                f32 px[3], py[3], ax, az, bx, bz, cx, cz, nx, ny, nz, len, area, inverse, up;
                s32 k, x0, x1, y0, y1;
                for (k = 0; k < 3; k++) {
                    s32 index = batch->verticesOffset + tri->verticesArray[k + 1];
                    if (index < 0 || index >= segment->numberOfVertices) {
                        break;
                    }
                    v[k] = &segment->vertices[index];
                }
                if (k < 3) {
                    continue;
                }
                ax = v[1]->x - v[0]->x;
                az = v[1]->z - v[0]->z;
                bx = v[2]->x - v[0]->x;
                bz = v[2]->z - v[0]->z;
                cx = v[1]->y - v[0]->y;
                cz = v[2]->y - v[0]->y;
                // The normal's up part against its length: ground, not walls. Only faces seen from above count (a
                // ceiling seen from below would cover the floor under it), unless they are drawn from both sides.
                nx = cx * bz - az * cz;
                nz = ax * cz - cx * bx;
                ny = az * bx - ax * bz;
                len = sqrtf(nx * nx + ny * ny + nz * nz);
                up = (tri->flags & 0x40) ? (ny < 0 ? -ny : ny) : ny; // (Seen from above, the hubs' ground has ny > 0.)
                if (len <= 0.0f || up / len < 0.35f) {
                    continue;
                }
                for (k = 0; k < 3; k++) {
                    px[k] = hub_map_x(model, v[k]->x);
                    py[k] = (v[k]->z - model->lowerZBounds) * sHubMapScale;
                }
                area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
                if (area == 0.0f) {
                    continue;
                }
                inverse = 1.0f / area;
                // (Whole pixels, a pixel wider each way: the points are never negative once the model's bounds hold.)
                x0 = (s32) MIN(MIN(px[0], px[1]), px[2]) - 1;
                x1 = (s32) MAX(MAX(px[0], px[1]), px[2]) + 1;
                y0 = (s32) MIN(MIN(py[0], py[1]), py[2]) - 1;
                y1 = (s32) MAX(MAX(py[0], py[1]), py[2]) + 1;
                if (x0 < 0) { x0 = 0; }
                if (y0 < 0) { y0 = 0; }
                if (x1 >= sHubMapW) { x1 = sHubMapW - 1; }
                if (y1 >= sHubMapH) { y1 = sHubMapH - 1; }
                for (y = y0; y <= y1; y++) {
                    for (x = x0; x <= x1; x++) {
                        f32 fx = x + 0.5f, fy = y + 0.5f;
                        f32 w1 = ((fx - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (fy - py[0])) * inverse;
                        f32 w2 = ((px[1] - px[0]) * (fy - py[0]) - (fx - px[0]) * (py[1] - py[0])) * inverse;
                        f32 w0 = 1.0f - w1 - w2;
                        s32 h;
                        if (w0 < -0.06f || w1 < -0.06f || w2 < -0.06f) {
                            continue;
                        }
                        h = (s32) (w0 * v[0]->y + w1 * v[1]->y + w2 * v[2]->y);
                        if (h > height[y * HUB_MAP_SIZE + x]) {
                            height[y * HUB_MAP_SIZE + x] = (s16) h;
                        }
                    }
                }
            }
        }
    }
    for (y = 0; y < sHubMapH; y++) {
        for (x = 0; x < sHubMapW; x++) {
            s32 h = height[y * HUB_MAP_SIZE + x];
            if (h != -0x7FFF) {
                lowY = h < lowY ? h : lowY;
                highY = h > highY ? h : highY;
            }
        }
    }
    for (y = 0; y < sHubMapH; y++) {
        for (x = 0; x < sHubMapW; x++) {
            s32 h = height[y * HUB_MAP_SIZE + x];
            unsigned char *o = rgba + (y * sHubMapW + x) * 4;
            if (h == -0x7FFF) {
                continue;
            }
            // Light for the highest ground, a darker blue-grey for the lowest.
            n = highY > lowY ? 150 + (h - lowY) * 105 / (highY - lowY) : 255;
            o[0] = o[1] = o[2] = (unsigned char) n;
            o[3] = 255;
        }
    }
    free(height);
    sHubMapModel = model;
}

/* The hub's picture, made if this hub (or its mirroring) has none yet. FALSE if it couldn't be made. */
static s32 hub_map_ready(LevelModel *model) {
    s32 mirrored = (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) != 0;
    if (model != sHubMapModel || mirrored != sHubMapMirrored) {
        hub_map_build(model, mirrored);
    }
    return model == sHubMapModel;
}

/* The racer's place on the map, as minimap_marker_pos works it out, in the map sprite's pixels (y down). */
static void map_position(RaceHudPoint *out, Object *obj, LevelModel *model) {
    f32 aspect, angleCos, angleSin, scaledX, scaledY, rotX, rotY, x, y;
    s32 width = model->upperXBounds - model->lowerXBounds, depth = model->upperZBounds - model->lowerZBounds;
    s32 mirrored = (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) != 0;
    s32 heading;

    if (width == 0 || depth == 0) {
        out->onMap = FALSE;
        return;
    }
    if (sHubPicture) {
        // The hub's own picture: -z up, no rotation, mirrored as the world is.
        out->onMap = TRUE;
        out->mapX = (s16) (hub_map_x(model, obj->trans.x_position) * 4.0f);
        out->mapY = (s16) ((obj->trans.z_position - model->lowerZBounds) * sHubMapScale * 4.0f);
        heading = obj->trans.rotation.y_rotation & 0xFFFF;
        out->heading = (u16) (sHubMapMirrored ? 0xFFFF - heading : heading);
        return;
    }
    aspect = width / (f32) depth;
    angleCos = coss_f((model->minimapRotation * 0xFFFF) / 360);
    angleSin = sins_f((model->minimapRotation * 0xFFFF) / 360);
    scaledX = (60.0f * aspect * model->minimapXScale * (obj->trans.x_position - model->lowerXBounds)) / width;
    scaledY = (model->minimapYScale * -60.0f * (obj->trans.z_position - model->lowerZBounds)) / depth;
    rotX = scaledX * angleCos + scaledY * angleSin;
    rotY = scaledX * angleSin - scaledY * angleCos;
    if (mirrored) {
        // The map is flipped about its anchor: this is where the marker lands on the unflipped sprite.
        x = rotX - model->minimapOffsetXAdv2 + gMinimapDotOffsetX;
        y = rotY - model->minimapOffsetYAdv2;
    } else {
        x = rotX + model->minimapOffsetXAdv1;
        y = rotY - model->minimapOffsetYAdv1;
    }
    out->onMap = TRUE;
    out->mapX = (s16) (x * 4.0f);
    out->mapY = (s16) (y * 4.0f);
    heading = (obj->trans.rotation.y_rotation - ((model->minimapRotation * 0xFFFF) / 360)) & 0xFFFF;
    out->heading = (u16) (mirrored ? 0xFFFF - heading : heading);
}

/* The clock at each checkpoint a racer reaches for the first time. */
static void stamp(s32 index, s32 checkpoint) {
    if (checkpoint >= MAX_STAMPS) {
        checkpoint = MAX_STAMPS - 1;
    }
    while (sReached[index] < checkpoint) {
        sStamps[index][++sReached[index]] = sClock;
    }
}

static s32 gap(s32 index, s32 playerIndex, s32 checkpoints) {
    s32 both = sReached[index] < sReached[playerIndex] ? sReached[index] : sReached[playerIndex];
    s32 laps;
    if (both <= 0 || index == playerIndex) {
        return RACE_HUD_NO_GAP;
    }
    laps = sReached[index] - sReached[playerIndex];
    laps = checkpoints > 0 ? (laps < 0 ? -laps : laps) / checkpoints : 0;
    if (laps > 0) {
        return sReached[index] > sReached[playerIndex] ? RACE_HUD_LAPPED + laps : -(RACE_HUD_LAPPED + laps);
    }
    return sStamps[playerIndex][both] - sStamps[index][both];
}

/* The boss or Taj a racer is, by its vehicle (gBossVehicles in objects.c; Taj rides the carpet in his challenges). */
static s8 special_racer(Object_Racer *r) {
    switch (r->vehicleID) {
        case VEHICLE_TRICKY:
            return RACE_HUD_TRICKY;
        case VEHICLE_BLUEY:
            return RACE_HUD_BLUEY;
        case VEHICLE_SMOKEY:
        case VEHICLE_PTERODACTYL:
            return RACE_HUD_SMOKEY;
        case VEHICLE_BUBBLER:
            return RACE_HUD_BUBBLER;
        case VEHICLE_WIZPIG:
        case VEHICLE_ROCKET:
            return RACE_HUD_WIZPIG;
        case VEHICLE_CARPET:
            return is_taj_challenge() ? RACE_HUD_TAJ : RACE_HUD_NOT_SPECIAL;
    }
    return RACE_HUD_NOT_SPECIAL;
}

/*
 * Time trial: the best run's splits, kept per track and vehicle in the data directory (ttbest_LEVEL_VEHICLE.bin) so
 * the bottom screen can show how a run compares with it as it goes, checkpoint by checkpoint, as the ghost does on
 * the track. A run that beats it replaces it at the finish.
 */
#define BEST_MAGIC 0x54544231 // "TTB1"
typedef struct BestRun {
    s32 magic;
    s32 total;
    s32 lapCount;
    s32 laps[RACE_HUD_LAPS];
    s32 stampCount;
    u16 stamps[MAX_STAMPS];
} BestRun;

static BestRun sBest;
static s32 sBestLevel = -1, sBestVehicle = -1, sBestSaved;

/*
 * The tracks menu's companion shows each track's best total as the cursor moves. Opening a file on the SD card for
 * every move held the game thread for 150 ms and more (bug report #217: the data directory keeps every bug report
 * snapshot, and looking up a name that isn't there reads all of it), so the totals are read once, the first time a
 * menu shows a companion: one pass over the directory, opening only the ttbest files it has.
 */
#define BEST_LEVELS 128
#define BEST_VEHICLES 3
static s32 sBestTotals[BEST_LEVELS][BEST_VEHICLES]; // 0: none.
static s32 sBestTotalsRead;

/*
 * P58 S1 (DKR_TTBEST_INDEX, default 1; 2026-11-23, proposals/3ds-sd-access-vs-kohjoh-pak.md): the bests off the game
 * thread's race and menu frames. On the 3DS the cost of a small SD file is its open (~70-100 ms on the owner's card,
 * a missing name 51-166 ms), and the old way paid it on the game thread three times: the menu's first companion
 * frame scanned the folder and opened every ttbest file (15-313 ms with none), a time trial's first HUD frame opened
 * its track's file (on the frame of the race-start hitch), and a new best was written at the finish line with the
 * temp file, remove and rename (227-569 ms measured on the writer).
 *   - ttbest_index.bin holds every track's best total (BestIndex). It is read once at boot, on the main thread before
 *     the game starts (port_race_hud_boot), so the menus never scan. When it is missing (the first start with this
 *     build) it is made once from the ttbest files (the old scan) and saved.
 *   - A time trial's best run is read during the level load, behind the black screen (port_race_hud_loaded), and not
 *     at all when the index says the track has none.
 *   - A new best and the index are saved by the writer thread (plat_save_file_later: temp file and rename there).
 * 0 = the old way. The ttbest files stay as they were in both ways: nothing is deleted.
 *
 * A test run's time trials (DKR_AUTORUN_TIME_TRIAL) keep their bests apart (ttbench_*, best_store), so a test run
 * never replaces a player's best; there every finish saves, so each race measures the write.
 *
 * The self-check (DKR_TTBEST_CHECK, default 1 in a debug build, 0 in a player's): at boot, after the index is read,
 * it is compared with the files themselves (the old scan): "time trial bests check: N files, M differ" in log.txt
 * and the checks cells. So each start of a test build checks what the last session's saves left on the card, with no
 * button or race needed (the switch bench can't race time trials: the AI doesn't drive the player's kart in one).
 * 2 is the check broken on purpose (rule K6): a new best's file is saved but the index is not told, so the next check must report it (the
 * host test, DKR_TTBEST_TEST, does exactly that in CI).
 */
#define BEST_CHECK_DEFAULT PLAT_DEBUG_TOOLS
#define BEST_INDEX_MAGIC 0x54544931 // "TTI1"
typedef struct BestIndex {
    s32 magic;
    s32 levels, vehicles; // BEST_LEVELS, BEST_VEHICLES
    s32 totals[BEST_LEVELS][BEST_VEHICLES];
} BestIndex;

static const char *sBestPrefix = "ttbest"; // The store: "ttbest" the player's, "ttbench" a test run's
static s32 sBestOpens;                     // Files opened (or looked for) by the bests, for the host test
static s32 sBestScans;                     // Folder scans, for the host test
static s32 sBestStoreFixed;                // The host test's store ("tttest") stays whatever runs

static s32 best_index_on(void) {
    return plat_switch("DKR_TTBEST_INDEX", 1) != 0;
}

static void best_path(char *out, s32 size, s32 level, s32 vehicle) {
    plat_snprintf(out, size, "%s/%s_%d_%d.bin", plat_data_dir(), sBestPrefix, (int) level, (int) vehicle);
}

static void best_index_path(char *out, s32 size) {
    plat_snprintf(out, size, "%s/%s_index.bin", plat_data_dir(), sBestPrefix);
}

static void *best_open(const char *path, unsigned int *size) {
    sBestOpens++;
    return plat_load_file(path, size);
}

/* A test run's time trials keep their own bests (switching store forgets what was read from the other). */
static void best_store(void) {
    const char *wanted = port_autorun_active() ? "ttbench" : "ttbest";
    if (!sBestStoreFixed && wanted != sBestPrefix) {
        sBestPrefix = wanted;
        sBestTotalsRead = FALSE;
        sBestLevel = sBestVehicle = -1;
        sBest.magic = 0;
    }
}

static void best_load(s32 level, s32 vehicle) {
    char path[256];
    unsigned int size = 0;
    void *data;
    if (level == sBestLevel && vehicle == sBestVehicle) {
        return;
    }
    sBestLevel = level;
    sBestVehicle = vehicle;
    sBest.magic = 0;
    if (best_index_on() && sBestTotalsRead && level >= 0 && level < BEST_LEVELS && vehicle >= 0 &&
        vehicle < BEST_VEHICLES && sBestTotals[level][vehicle] <= 0) {
        return; // P58: the index says there is none, so no SD lookup for a name that isn't there.
    }
    best_path(path, sizeof(path), level, vehicle);
    data = best_open(path, &size);
    if (data != NULL && size == sizeof(BestRun) && ((BestRun *) data)->magic == BEST_MAGIC) {
        sBest = *(BestRun *) data;
    }
    free(data);
}

/* Reads a number at *text and moves past it; -1 if there is none. */
static s32 best_number(const char **text) {
    s32 value = -1;
    while (**text >= '0' && **text <= '9' && value < 1000) {
        value = (value < 0 ? 0 : value * 10) + (**text - '0');
        (*text)++;
    }
    return value;
}

/* The old way: one pass over the directory, opening each of the store's files. Returns how many were read. */
static s32 best_totals_scan(s32 totals[BEST_LEVELS][BEST_VEHICLES]) {
    static char sNames[BEST_LEVELS * BEST_VEHICLES][24];
    char path[256], prefix[16];
    s32 count, found = 0, i, prefixLength;
    bzero(totals, sizeof(s32) * BEST_LEVELS * BEST_VEHICLES);
    prefixLength = plat_snprintf(prefix, sizeof(prefix), "%s_", sBestPrefix);
    sBestScans++;
    count = plat_list_dir_prefix(plat_data_dir(), prefix, sNames[0], sizeof(sNames[0]), ARRAY_COUNT(sNames));
    for (i = 0; i < count; i++) {
        // ttbest_LEVEL_VEHICLE.bin (ttbest_index.bin has no number, so it is passed over)
        const char *text = sNames[i] + prefixLength;
        s32 level = best_number(&text), vehicle = -1;
        unsigned int size = 0;
        void *data;
        if (*text == '_') {
            text++;
            vehicle = best_number(&text);
        }
        if (level < 0 || level >= BEST_LEVELS || vehicle < 0 || vehicle >= BEST_VEHICLES ||
            text[0] != '.' || text[1] != 'b' || text[2] != 'i' || text[3] != 'n' || text[4] != '\0') {
            continue;
        }
        best_path(path, sizeof(path), level, vehicle);
        data = best_open(path, &size);
        if (data != NULL && size == sizeof(BestRun) && ((BestRun *) data)->magic == BEST_MAGIC) {
            totals[level][vehicle] = ((BestRun *) data)->total;
            found++;
        }
        free(data);
    }
    return found;
}

/* Queues the index (sBestTotals) for the writer thread. */
static void best_index_save(void) {
    char path[256];
    BestIndex *index = malloc(sizeof(BestIndex));
    if (index == NULL) {
        return;
    }
    index->magic = BEST_INDEX_MAGIC;
    index->levels = BEST_LEVELS;
    index->vehicles = BEST_VEHICLES;
    bcopy(sBestTotals, index->totals, sizeof(index->totals));
    best_index_path(path, sizeof(path));
    plat_save_file_later(path, index, sizeof(BestIndex));
}

static void best_totals_read(void) {
    unsigned long long start = plat_time_ns();
    s32 found = 0, i, j;
    sBestTotalsRead = TRUE;
    if (best_index_on()) {
        char path[256];
        unsigned int size = 0;
        BestIndex *index;
        best_index_path(path, sizeof(path));
        index = best_open(path, &size);
        if (index != NULL && size == sizeof(BestIndex) && index->magic == BEST_INDEX_MAGIC &&
            index->levels == BEST_LEVELS && index->vehicles == BEST_VEHICLES) {
            bcopy(index->totals, sBestTotals, sizeof(sBestTotals));
            free(index);
            for (i = 0; i < BEST_LEVELS; i++) {
                for (j = 0; j < BEST_VEHICLES; j++) {
                    found += sBestTotals[i][j] > 0;
                }
            }
            plat_log("time trial bests: %s index, %d bests, read in %d ms", sBestPrefix, (int) found,
                     (int) ((plat_time_ns() - start) / 1000000ULL));
            return;
        }
        free(index);
        found = best_totals_scan(sBestTotals);
        best_index_save();
        plat_log("time trial bests: %s index made from %d files in %d ms", sBestPrefix, (int) found,
                 (int) ((plat_time_ns() - start) / 1000000ULL));
        return;
    }
    found = best_totals_scan(sBestTotals);
    plat_log("time trial bests: %d read in %d ms", (int) found, (int) ((plat_time_ns() - start) / 1000000ULL));
}

/* DKR_TTBEST_CHECK: the index as held against the files. Returns how many tracks differ. */
static s32 best_check(void) {
    static s32 sFiles[BEST_LEVELS][BEST_VEHICLES];
    s32 found, differ = 0, i, j;
    if (!sBestTotalsRead) {
        best_totals_read();
    }
    plat_saves_drain(); // The queued bests on the card first
    found = best_totals_scan(sFiles);
    for (i = 0; i < BEST_LEVELS; i++) {
        for (j = 0; j < BEST_VEHICLES; j++) {
            if ((sFiles[i][j] > 0 ? sFiles[i][j] : 0) != (sBestTotals[i][j] > 0 ? sBestTotals[i][j] : 0)) {
                if (differ < 4) {
                    plat_log("time trial bests check: %s_%d_%d: file %d, index %d", sBestPrefix, (int) i, (int) j,
                             (int) sFiles[i][j], (int) sBestTotals[i][j]);
                }
                differ++;
            }
        }
    }
    plat_log("time trial bests check: %d files, %d differ", (int) found, (int) differ);
    plat_check_report("time trial bests", differ != 0);
    return differ;
}

/* At the finish: this run becomes the best if it is (the stamps are the player's, sStamps[playerIndex]). */
static void best_finish(s32 playerIndex, const RaceHud *hud) {
    char path[256];
    s32 i, index = best_index_on(), bench;
    best_store();
    bench = sBestPrefix[2] == 'b' && sBestPrefix[3] == 'e'; // "ttbench": every finish saves, to measure the write
    if (sBestSaved || hud->raceTime <= 0 ||
        (!bench && sBest.magic == BEST_MAGIC && sBest.total <= hud->raceTime)) {
        sBestSaved = TRUE;
        return;
    }
    sBestSaved = TRUE;
    sBest.magic = BEST_MAGIC;
    sBest.total = hud->raceTime;
    sBest.lapCount = hud->lapsTimed;
    for (i = 0; i < RACE_HUD_LAPS; i++) {
        sBest.laps[i] = i < hud->lapsTimed ? hud->lapTimes[i] : 0;
    }
    sBest.stampCount = sReached[playerIndex] + 1;
    for (i = 0; i < MAX_STAMPS; i++) {
        s32 t = i < sBest.stampCount ? sStamps[playerIndex][i] : 0;
        sBest.stamps[i] = (u16) (t > 0xFFFF ? 0xFFFF : t);
    }
    best_path(path, sizeof(path), sBestLevel, sBestVehicle);
    if (index) {
        // P58: the writer thread saves the run, then the index (in that order: a stop in between leaves the index
        // one best behind, which the next best or DKR_TTBEST_CHECK puts right).
        BestRun *copy = malloc(sizeof(BestRun));
        if (copy != NULL) {
            *copy = sBest;
            plat_save_file_later(path, copy, sizeof(BestRun));
        }
        if (!sBestTotalsRead) {
            best_totals_read();
        }
        if (plat_switch("DKR_TTBEST_CHECK", BEST_CHECK_DEFAULT) == 2) {
            plat_log("time trial bests: check broken on purpose (rule K6): the index misses %s_%d_%d", sBestPrefix,
                     (int) sBestLevel, (int) sBestVehicle);
            return;
        }
    } else {
        plat_save_file(path, &sBest, sizeof(sBest));
    }
    if (sBestLevel >= 0 && sBestLevel < BEST_LEVELS && sBestVehicle >= 0 && sBestVehicle < BEST_VEHICLES) {
        sBestTotals[sBestLevel][sBestVehicle] = sBest.total;
    }
    if (index) {
        best_index_save();
    }
}

/* P58: at boot, on the main thread before the game threads start (src/port_main.c): the player's bests index. */
void port_race_hud_boot(void) {
    if (best_index_on()) {
        best_totals_read(); // The player's store (a test run's is picked later, best_store: not at boot)
        if (plat_switch("DKR_TTBEST_CHECK", BEST_CHECK_DEFAULT) != 0) {
            best_check();
        }
    }
}

/* P58: at the end of a level load (port_loadtime.c, the game's loads only), behind the black screen: a time trial's
 * best run, so its first HUD frame opens nothing. FALSE when not a time trial (nothing done). */
s32 port_race_hud_loaded(void) {
    Object *player;
    Object_Racer *me;
    if (!is_in_time_trial()) {
        return FALSE;
    }
    best_store();
    player = get_racer_object_by_port(PLAYER_ONE);
    if (!best_index_on() || player == NULL) {
        return TRUE;
    }
    me = player->racer;
    if (!sBestTotalsRead) {
        best_totals_read();
    }
    best_load(level_id(), me->vehicleID);
    return TRUE;
}

/* The bosses' rematches (the second race in each world). */
static s32 boss_round(void) {
    switch (level_id()) {
        case ASSET_LEVEL_TRICKYTOPS2:
        case ASSET_LEVEL_BLUEY2:
        case ASSET_LEVEL_BUBBLER2:
        case ASSET_LEVEL_SMOKEY2:
        case ASSET_LEVEL_WIZPIG2:
            return 2;
    }
    return 1;
}

/* A challenge's score: bananas home, or eggs in the nest; in battle, bananas left. */
static s32 challenge_score(s32 mode, Object_Racer *r) {
    return mode == RACE_HUD_MODE_BATTLE ? r->bananas : r->lap;
}

/* The hub's progress: balloons, each world's balloons, trophy, key and boss, the amulets, the doors and Taj. */
static void hub_progress(RaceHud *hud, Settings *settings, LevelModel *model) {
    Object **objects;
    s32 i, start, count;
    hud->world = settings->worldId;
    hud->balloons = settings->balloonsPtr[0];
    for (i = 0; i < RACE_HUD_WORLDS; i++) {
        hud->worldBalloons[i] = settings->balloonsPtr[i + 1];
        hud->worldTrophy[i] = (settings->trophies >> (i * 2)) & 3;
    }
    hud->worldKeys = (settings->keys >> 1) & 0x1F;
    hud->worldBosses = (settings->bosses >> 1) & 0x1F;
    hud->ttAmulet = settings->ttAmulet;
    hud->wizpigAmulet = settings->wizpigAmulet;
    if (model == NULL) {
        return;
    }
    objects = objGetObjList(&start, &count);
    for (i = start; i < count; i++) {
        Object *obj = objects[i];
        if (obj == NULL || (obj->trans.flags & OBJ_FLAGS_PARTICLE)) {
            continue;
        }
        if (obj->behaviorId == BHV_DOOR && hud->doorCount < RACE_HUD_DOORS) {
            map_position(&hud->doors[hud->doorCount++], obj, model);
        } else if (obj->behaviorId == BHV_PARK_WARDEN && !hud->taj.onMap) {
            map_position(&hud->taj, obj, model);
        }
    }
}

/*
 * A course record, or -1 when it is still the game's own placeholder (clear_lap_records fills each from a table: a
 * course time of 3:00:00 and a lap of 1:00:00, so a plain "under three minutes" check showed the empty lap as a record).
 */
static s32 record_time(s32 vehicle, s32 level, s32 lap) {
    Settings *settings = get_settings();
    u16 *defaults = (u16 *) get_misc_asset(ASSET_MISC_23);
    u16 time = lap ? settings->flapTimesPtr[vehicle][level] : settings->courseTimesPtr[vehicle][level];
    if (time == 0 || time >= 10800) {
        return -1;
    }
    if (defaults != NULL && time == defaults[level * 12 + vehicle * 4 + (lap ? 2 : 0)]) {
        return -1;
    }
    return time;
}

/* Time trial: the course records, T.T., the ghosts, and how this run compares with the best one. */
static void time_trial_progress(RaceHud *hud, Object_Racer *me, LevelModel *model) {
    Settings *settings = get_settings();
    s32 vehicle = me->vehicleID, i, at;
    Object *ghost;
    hud->record = hud->recordLap = hud->staffTime = -1;
    hud->bestGap = RACE_HUD_NO_GAP;
    if (vehicle >= 0 && vehicle < 3) {
        hud->record = record_time(vehicle, settings->courseId, FALSE);
        hud->recordLap = record_time(vehicle, settings->courseId, TRUE);
    }
    if (timetrial_ghost_staff() != NULL) {
        hud->staffTime = gTTGhostTimeToBeat;
    }
    best_store();
    best_load(level_id(), vehicle);
    if (sBest.magic == BEST_MAGIC) {
        hud->bestTotal = sBest.total;
        hud->bestLapCount = sBest.lapCount;
        for (i = 0; i < RACE_HUD_LAPS; i++) {
            hud->bestLaps[i] = sBest.laps[i];
        }
        at = me->racerIndex >= 0 && me->racerIndex < RACE_HUD_RACERS ? sReached[me->racerIndex] : 0;
        if (at > 0 && at < sBest.stampCount && at < MAX_STAMPS) {
            hud->bestGap = sStamps[me->racerIndex][at] - sBest.stamps[at];
        }
    }
    if (model != NULL) {
        ghost = timetrial_player_ghost();
        if (ghost != NULL && !(ghost->trans.flags & OBJ_FLAGS_INVISIBLE)) {
            map_position(&hud->ghost, ghost, model);
        }
        ghost = timetrial_ghost_staff();
        if (ghost != NULL && !(ghost->trans.flags & OBJ_FLAGS_INVISIBLE)) {
            map_position(&hud->staffGhost, ghost, model);
        }
    }
}

/* Not racing: what the menu has highlighted, for the bottom screen (RACE_HUD_MODE_MENU_*). FALSE: nothing. */
static s32 menu_companion(RaceHud *hud) {
    static const RaceHud sEmpty;
    Settings *file = NULL;
    char *name = NULL;
    s32 level = -1, vehicle = 0;
    if (get_game_mode() != GAMEMODE_MENU) {
        return FALSE;
    }
    best_store();
    if (!sBestTotalsRead) {
        best_totals_read();
    }
    switch (port_menu_focus(&level, &vehicle, &file, &name)) {
        case PORT_MENU_FOCUS_FILE:
            *hud = sEmpty;
            hud->mode = RACE_HUD_MODE_MENU_SAVE;
            hud->mapSprite = -1;
            hub_progress(hud, file, NULL);
            hud->world = -1;
            hud->adventureTwo = (file->cutsceneFlags & CUTSCENE_ADVENTURE_TWO) != 0;
            plat_snprintf(hud->name, sizeof(hud->name), "%.3s", name);
            return TRUE;
        case PORT_MENU_FOCUS_TRACK: {
            char *trackName = level_name(level);
            *hud = sEmpty;
            hud->mode = RACE_HUD_MODE_MENU_TRACK;
            hud->mapSprite = -1;
            hud->level = level;
            hud->vehicle = vehicle;
            hud->record = hud->recordLap = hud->bestTotal = -1;
            if (vehicle >= 0 && vehicle < BEST_VEHICLES && level >= 0) {
                hud->record = record_time(vehicle, level, FALSE);
                hud->recordLap = record_time(vehicle, level, TRUE);
                if (level < BEST_LEVELS && sBestTotals[level][vehicle] > 0) {
                    hud->bestTotal = sBestTotals[level][vehicle];
                }
            }
            plat_snprintf(hud->name, sizeof(hud->name), "%s", trackName != NULL ? trackName : "");
            return TRUE;
        }
    }
    return FALSE;
}

/* Once a game frame (src/thread3_main.c), with the number of retraces it took. */
void port_race_hud_frame(s32 updateRate) {
    static const RaceHud sEmpty;
    static RaceHud hud;
    Object *player = get_racer_object_by_port(PLAYER_ONE);
    Object **racers;
    Object_Racer *me;
    LevelModel *model;
    LevelHeader *header;
    s32 count, i, j, checkpoints, mode = hud_mode(player);
    s32 order[RACE_HUD_RACERS];

    if (mode < 0 && port_race_hud_setting() != RACE_HUD_TOP && menu_companion(&hud)) {
        sActive = FALSE;
        sMenuShown = TRUE;
        plat_set_race_hud(&hud);
        return;
    }
    if (mode >= 0) {
        racers = get_racer_objects(&count);
        if (count > RACE_HUD_RACERS) {
            count = RACE_HUD_RACERS;
        }
        // The clock and the checkpoints are kept whichever screen the HUD is on, so that switching it to the bottom
        // in the middle of a race doesn't start the gaps (or a time trial's splits) over from there.
        if (!sTracking || get_race_countdown() > 0 || mode != sTrackedMode) {
            // A new race (or the start of this one): the clock and the gaps start over.
            sClock = 0;
            sBestSaved = FALSE;
            for (i = 0; i < RACE_HUD_RACERS; i++) {
                sReached[i] = 0;
            }
        } else if (!is_game_paused()) {
            sClock += updateRate;
        }
        sTracking = TRUE;
        sTrackedMode = mode;
        for (i = 0; i < count; i++) {
            Object_Racer *r = racers[i]->racer;
            if (r->racerIndex >= 0 && r->racerIndex < RACE_HUD_RACERS) {
                stamp(r->racerIndex, r->courseCheckpoint);
            }
        }
    } else {
        sTracking = FALSE;
    }
    if (port_race_hud_setting() == RACE_HUD_TOP || mode < 0) {
        if (sActive || sMenuShown) {
            sActive = FALSE;
            sMenuShown = FALSE;
            plat_set_race_hud(NULL);
        }
        return;
    }
    sMenuShown = FALSE;
    me = player->racer;
    header = level_header();
    model = get_current_level_model();
    checkpoints = get_checkpoint_count();
    sActive = TRUE;

    hud = sEmpty;
    hud.mode = mode;
    hud.mapSprite = model != NULL && model == sMapModel ? sMapSprite : -1;
    hud.mapMirrored = (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) != 0;
    // A hub (driving round it, or Taj's challenges on it) has its own picture, and the positions are on that.
    sHubPicture = model != NULL && level_type() == RACETYPE_HUBWORLD && hub_map_ready(model);
    if (sHubPicture) {
        hud.mapBitmap = sHubMaps[sHubMapUse];
        hud.mapBitmapW = sHubMapW;
        hud.mapBitmapH = sHubMapH;
        hud.mapBitmapId = sHubMapId;
        hud.mapMirrored = FALSE; // (Mirrored in the picture already.)
    }
    hud.silverCoins = -1;
    if (model != NULL) {
        hud.mapColour[0] = (model->minimapColor >> 16) & 0xFF;
        hud.mapColour[1] = (model->minimapColor >> 8) & 0xFF;
        hud.mapColour[2] = model->minimapColor & 0xFF;
    }
    // The racers in race order (racePosition from 1; a finished racer keeps its finishing place); in a challenge, by
    // score (the knocked out last).
    for (i = 0; i < count; i++) {
        order[i] = i;
    }
    for (i = 1; i < count; i++) {
        for (j = i; j > 0; j--) {
            Object_Racer *a = racers[order[j - 1]]->racer, *b = racers[order[j]]->racer;
            s32 swap;
            if (mode == RACE_HUD_MODE_BANANAS || mode == RACE_HUD_MODE_BATTLE || mode == RACE_HUD_MODE_EGGS) {
                s32 outA = mode == RACE_HUD_MODE_BATTLE && a->raceFinished && a->bananas <= 0;
                s32 outB = mode == RACE_HUD_MODE_BATTLE && b->raceFinished && b->bananas <= 0;
                swap = outA != outB ? outA : challenge_score(mode, b) > challenge_score(mode, a);
            } else {
                swap = b->racePosition < a->racePosition;
            }
            if (!swap) {
                break;
            }
            swap = order[j];
            order[j] = order[j - 1];
            order[j - 1] = swap;
        }
    }
    hud.racerCount = 0;
    for (j = 0; j < count; j++) {
        Object_Racer *r = racers[order[j]]->racer;
        RaceHudRacer *out = &hud.racers[hud.racerCount++];
        out->character = r->characterId;
        out->special = special_racer(r);
        if (out->special == RACE_HUD_NOT_SPECIAL && r->vehicleID >= VEHICLE_BOSSES) {
            // A boss vehicle with no portrait of its own: its characterId is a stand-in, not its face.
            out->character = -1;
        }
        out->player = racers[order[j]] == player;
        out->gap = (r->racerIndex >= 0 && r->racerIndex < RACE_HUD_RACERS && me->racerIndex >= 0 &&
                    me->racerIndex < RACE_HUD_RACERS)
                       ? gap(r->racerIndex, me->racerIndex, checkpoints)
                       : RACE_HUD_NO_GAP;
        out->score = (s8) challenge_score(mode, r);
        if (mode == RACE_HUD_MODE_BANANAS) {
            out->carrying = r->bananas > 0;
        } else if (mode == RACE_HUD_MODE_EGGS) {
            out->carrying = r->held_obj != NULL;
        }
        out->out = mode == RACE_HUD_MODE_BATTLE && r->raceFinished && r->bananas <= 0;
        if (mode == RACE_HUD_MODE_BATTLE && !out->out) {
            hud.racersLeft++;
        }
        if (model != NULL) {
            map_position(&out->at, racers[order[j]], model);
        }
    }
#ifndef __3DS__
    /* Host only (DKR_DUMP_PATHS=file): each level's checkpoints once, as map points, for the bottom screen's
       centre-line table (tools/map_paths) */
    if (model != NULL && plat_getenv("DKR_DUMP_PATHS") != NULL && gNumberOfTotalCheckpoints > 0) {
        static s32 sDumped[128];
        s32 lv = level_id();
        if (lv >= 0 && lv < 128 && !sDumped[lv]) {
            static char sBuf[16384];
            s32 c, n;
            sDumped[lv] = 1;
            n = plat_snprintf(sBuf, sizeof(sBuf), "level %d sprite %d main %d total %d mode %d\n", (int) lv, (int) sMapSprite,
                              (int) gNumberOfMainCheckpoints, (int) gNumberOfTotalCheckpoints, (int) mode);
            for (c = 0; c < gNumberOfTotalCheckpoints && n < (s32) sizeof(sBuf) - 100; c++) {
                RaceHudPoint pt;
                map_position(&pt, gTrackCheckpoints[c].obj, model);
                n += plat_snprintf(sBuf + n, sizeof(sBuf) - n, "cp %d id %d alt %d x %d y %d scale %d\n", (int) c,
                                   (int) gTrackCheckpoints[c].checkpointID, (int) gTrackCheckpoints[c].altRouteID,
                                   (int) pt.mapX, (int) pt.mapY, (int) (gTrackCheckpoints[c].scale * 100));
            }
            plat_append_file(plat_getenv("DKR_DUMP_PATHS"), sBuf, (unsigned int) n);
        }
    }
#endif
    hud.position = me->racePosition;
    // The race types whose top screen HUD counts laps (game_ui.c): the races, Horseshoe Gulch's and the bosses'.
    hud.laps = (level_type() == RACETYPE_DEFAULT || level_type() == RACETYPE_HORSESHOE_GULCH ||
                level_type() == RACETYPE_BOSS) &&
                       mode != RACE_HUD_MODE_HUB
                   ? header->laps
                   : 0;
    hud.lap = me->countLap + 1;
    if (hud.laps > 0 && hud.lap > hud.laps) {
        hud.lap = hud.laps;
    }
    for (i = 0; i <= me->countLap && i < RACE_HUD_LAPS && (hud.laps == 0 || i < hud.laps); i++) {
        hud.lapTimes[i] = me->lap_times[i];
        hud.raceTime += me->lap_times[i];
        hud.lapsTimed = i + 1;
    }
    hud.item = me->balloon_quantity > 0 ? me->balloon_type : -1;
    hud.itemLevel = me->balloon_level;
    hud.itemCount = me->balloon_quantity;
    hud.bananas = me->bananas;
    hud.countdown = get_race_countdown() > 0;
    hud.finished = me->raceFinished;
    hud.vehicle = me->vehicleID;
    switch (mode) {
        case RACE_HUD_MODE_RACE:
            if (check_if_silver_coin_race()) {
                hud.silverCoins = me->silverCoinCount;
            }
            break;
        case RACE_HUD_MODE_BOSS:
            hud.round = boss_round();
            break;
        case RACE_HUD_MODE_BANANAS:
            hud.goal = 10;
            break;
        case RACE_HUD_MODE_EGGS:
            hud.goal = 3;
            break;
        case RACE_HUD_MODE_TIME_TRIAL:
            time_trial_progress(&hud, me, model);
            if (hud.finished && me->racerIndex >= 0 && me->racerIndex < RACE_HUD_RACERS) {
                best_finish(me->racerIndex, &hud);
            }
            break;
        case RACE_HUD_MODE_HUB:
            hub_progress(&hud, get_settings(), model);
            break;
    }
    plat_set_race_hud(&hud);
}

/* The Race HUD page of the Settings menu (and SCREEN OPTIONS in the game's menus): where the HUD goes, and how the bottom screen looks. */
int race_hud_option_count(void) {
    return 2;
}

const char *race_hud_option_label(int index) {
    return index == 1 ? "Bottom screen look" : "Race HUD on";
}

/* 0-3 the base looks, 4-7 their colour variants (bottom_screen.c sVariant; plat_set_bottom_style maps them) */
#define LOOK_COUNT 8
static const char *const sLookNames[LOOK_COUNT] = { "Classic", "Glass", "Broadcast", "Dock", "Carbon", "Jungle", "Sunset", "Daylight" };

s32 port_bottom_style(void) {
    const char *env = plat_getenv("DKR_BOTTOM_STYLE");
    s32 value = env != NULL ? atoi(env) : settings_get("bottomstyle", 0);
    return (value < 0 || value >= LOOK_COUNT) ? 0 : value;
}

void race_hud_option_value(int index, char *out, int size) {
    static const char *const sNames[3] = { "Top", "Bottom", "Both" };
    if (index == 1) {
        plat_snprintf(out, size, "%s", sLookNames[port_bottom_style()]);
        return;
    }
    plat_snprintf(out, size, "%s", sNames[port_race_hud_setting()]);
}

void race_hud_option_change(int index, int step) {
    // Bottom, Both, Top: the order the row steps through.
    static const s32 sOrder[3] = { RACE_HUD_BOTTOM, RACE_HUD_BOTH, RACE_HUD_TOP };
    s32 at = 0;
    if (index == 1) {
        s32 look = (port_bottom_style() + (step < 0 ? LOOK_COUNT - 1 : 1)) % LOOK_COUNT;
        settings_set("bottomstyle", look);
        plat_set_bottom_style(look);
        return;
    }
    while (at < 2 && sOrder[at] != port_race_hud_setting()) {
        at++;
    }
    port_set_race_hud_setting(sOrder[(at + (step < 0 ? 2 : 1)) % 3]);
}

const char *race_hud_option_hint(int index) {
    if (index == 1) {
        static const char *const sHints[LOOK_COUNT] = { "The game's own boxes", "Soft glass cards, round buttons",
                                               "Race TV slabs in red and yellow", "Few boxes, icon bar below",
                                               "Broadcast in black and teal", "Dock in jungle green",
                                               "Glass in sunset red", "Glass in light blue" };
        return sHints[port_bottom_style()];
    }
    switch (port_race_hud_setting()) {
        case RACE_HUD_TOP:
            return "Lap, item, map on the top screen";
        case RACE_HUD_BOTH:
            return "On both screens";
        default:
            return "Standings, item and map below";
    }
}

#ifndef __3DS__
/*
 * Host only, CI (host_main.c, DKR_TTBEST_TEST=1, with DKR_DATA_DIR an empty folder): P58's time trial bests index
 * against the files, in a store of its own (tttest_*): the index made once from the files, then read without a scan;
 * no SD lookup for a track without a best; a new best saved with the index and the self-check clean; then the
 * self-check broken on purpose (rule K6: DKR_TTBEST_CHECK=2, the index not told of a new best), which must report it;
 * and the old way (DKR_TTBEST_INDEX=0) still opening the file. Returns the number of failures, described in report.
 */
static void ttbest_test_expect(char *report, int size, int *at, int *failed, int ok, const char *what) {
    if (*at < size - 1) {
        *at += plat_snprintf(report + *at, size - *at, "%s: %s\n", ok ? "ok" : "FAILED", what);
        if (*at > size - 1) {
            *at = size - 1;
        }
    }
    *failed += !ok;
}

static void ttbest_test_write(s32 level, s32 vehicle, s32 total) {
    static BestRun sRun;
    char path[256];
    bzero(&sRun, sizeof(sRun));
    sRun.magic = BEST_MAGIC;
    sRun.total = total;
    best_path(path, sizeof(path), level, vehicle);
    plat_save_file(path, &sRun, total > 0 ? sizeof(sRun) : 0);
}

int port_race_hud_ttbest_test(char *report, int size) {
    static RaceHud sHud;
    char path[256];
    int at = 0, failed = 0;
    s32 opens, differ;
    report[0] = '\0';
    sBestStoreFixed = TRUE;
    sBestPrefix = "tttest";
    plat_override_switch("DKR_TTBEST_INDEX", 1);
    best_index_path(path, sizeof(path));
    plat_save_file(path, "x", 1); // Not an index: made again from the files
    ttbest_test_write(9, 1, 0);
    ttbest_test_write(11, 0, 0);
    ttbest_test_write(5, 0, 1000);
    ttbest_test_write(7, 2, 2000);

    sBestTotalsRead = FALSE;
    sBestScans = 0;
    best_totals_read();
    ttbest_test_expect(report, size, &at, &failed,
                       sBestTotals[5][0] == 1000 && sBestTotals[7][2] == 2000 && sBestTotals[9][1] == 0 &&
                           sBestScans == 1,
                       "no index: made from the files (one scan)");
    bzero(sBestTotals, sizeof(sBestTotals));
    sBestTotalsRead = FALSE;
    sBestScans = 0;
    best_totals_read();
    ttbest_test_expect(report, size, &at, &failed,
                       sBestTotals[5][0] == 1000 && sBestTotals[7][2] == 2000 && sBestScans == 0,
                       "the index read back, no scan");

    sBestLevel = -1;
    opens = sBestOpens;
    best_load(9, 1);
    ttbest_test_expect(report, size, &at, &failed, sBestOpens == opens && sBest.magic != BEST_MAGIC,
                       "a track without a best: no file opened");
    opens = sBestOpens;
    best_load(5, 0);
    ttbest_test_expect(report, size, &at, &failed,
                       sBestOpens == opens + 1 && sBest.magic == BEST_MAGIC && sBest.total == 1000,
                       "a track with a best: its file read");

    best_load(9, 1);
    bzero(&sHud, sizeof(sHud));
    sHud.raceTime = 1500;
    sHud.lapsTimed = 1;
    sHud.lapTimes[0] = 1500;
    sReached[0] = 0;
    sBestSaved = FALSE;
    best_finish(0, &sHud);
    differ = best_check();
    ttbest_test_expect(report, size, &at, &failed, differ == 0, "a new best saved with the index: check 0 differ");
    bzero(sBestTotals, sizeof(sBestTotals));
    sBestTotalsRead = FALSE;
    sBestScans = 0;
    best_totals_read();
    ttbest_test_expect(report, size, &at, &failed, sBestTotals[9][1] == 1500 && sBestScans == 0,
                       "the saved index holds the new best");

    plat_override_switch("DKR_TTBEST_CHECK", 2);
    best_load(5, 0);
    sHud.raceTime = 900;
    sBestSaved = FALSE;
    best_finish(0, &sHud);
    plat_override_switch("DKR_TTBEST_CHECK", -1);
    differ = best_check();
    ttbest_test_expect(report, size, &at, &failed, differ == 1,
                       "rule K6: the check broken on purpose (index not told of a best) reports 1 differ");

    plat_override_switch("DKR_TTBEST_INDEX", 0);
    sBestLevel = -1;
    opens = sBestOpens;
    best_load(11, 0);
    ttbest_test_expect(report, size, &at, &failed, sBestOpens == opens + 1, "the old way: the file looked for");
    plat_override_switch("DKR_TTBEST_INDEX", -1);

    sBestStoreFixed = FALSE;
    sBestPrefix = "ttbest";
    sBestTotalsRead = FALSE;
    sBestLevel = sBestVehicle = -1;
    sBest.magic = 0;
    return failed;
}
#endif
