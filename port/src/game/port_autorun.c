/*
 * Unattended test run: races every track with every character, one after another, with the game's AI driving the
 * player's kart, and records a performance profile and the outcome of each race.
 *
 * It is started and stopped with the "Test run" button on the bottom screen of the 3DS, or always runs with:
 *   DKR_AUTORUN=1                   Turns it on (and the profiler with it).
 *   DKR_AUTORUN_TRACKS=all|id,...   Tracks to race (ASSET_LEVEL_* ids). Default: the 20 tracks of the tracks menu.
 *   DKR_AUTORUN_CHARACTERS=all|n,... Characters (Krunch 0, Bumper 1, Tiptup 2, Conker 3, Timber 4, Banjo 5,
 *                                   Drumstick 6, Pipsy 7, T.T. 8, Diddy 9). Default: all ten.
 *   DKR_AUTORUN_VEHICLES=all        Also every other vehicle a track allows (default: only its usual vehicle).
 *   DKR_AUTORUN_TIMEOUT=seconds     A race that hasn't finished by then is stopped and counted as a timeout (300).
 *   DKR_AUTORUN_LOOP=1              Start over after the last race, forever.
 *   DKR_AUTORUN_RESTART=1           Start from the first race instead of resuming.
 *   DKR_AUTORUN_SNAPSHOT=1          Also save a snapshot (GPU and reference images) after each race finishes.
 *   DKR_AUTORUN_TIME_TRIAL=1        Race in time trial (alone, with the ghosts) instead of against the field.
 *
 * The GPU benchmark is a short run instead: the heaviest races of the hardware test runs, each raced once with every
 * render setting of sBenchConfigs (anti-aliasing 4x, 2x or off, 30 or 60 fps), with the
 * profiler's numbers for each set side by side in the results. About 30 minutes. It replaced the anti-aliasing
 * benchmark of roadmap item G0 (AA off and on only). It is started with the "GPU bench" button in the Test run
 * confirmation, or always runs with:
 *   DKR_AUTORUN_BENCH=1               Turns it on (instead of the full run).
 *   DKR_AUTORUN_BENCH_TRACKS=id,...   Tracks (default: Jungle Falls, Crescent Island and Haunted Woods, the slowest
 *                                     tracks of the quick tests after P5, all GPU-bound with 4x anti-aliasing).
 *   DKR_AUTORUN_BENCH_CHARACTER=n     Character (default Bumper, whose races were the heaviest there).
 *   DKR_AUTORUN_BENCH_CONFIGS=n,...   The settings to race (indexes into sBenchConfigs; default all).
 * It keeps its own files (gpu_bench_state.txt, gpu_bench_results.csv with "config" and "pct_60fps" columns), so a
 * full run it interrupts carries on afterwards. DKR_AUTORUN_TIMEOUT, _LOOP and _RESTART apply to it too.
 *
 * The switch bench (roadmap P29) is the GPU benchmark's twin for the renderer's experimental switches (sSwitches): the
 * same heavy races, once with every switch at its usual setting and once with each switch flipped by itself, set in
 * the program at the start of each race (plat_override_switch), so env.txt needs no editing. The races keep the
 * player's render settings, which the results name. About 6 minutes a switch plus 6 for the usual settings. New
 * experiments add a line to sSwitches. It is started with the "Switch bench" button in the Test run confirmation,
 * or always runs with:
 *   DKR_AUTORUN_SWITCH=1              Turns it on (instead of the full run).
 *   DKR_AUTORUN_SWITCH_TRACKS=id,...  Tracks (default: the GPU benchmark's three).
 *   DKR_AUTORUN_SWITCH_CHARACTER=n    Character (default Bumper).
 *   DKR_AUTORUN_SWITCH_CONFIGS=n,...  The races' switch sets: 0 every switch at its usual setting, n switch n-1 of
 *                                     sSwitches flipped, and with two switches or more a last one with all of
 *                                     them flipped together (default all).
 * Its files are switch_bench_state.txt and switch_bench_results.csv (with "config" and "pct_60fps" columns, as the
 * GPU benchmark's).
 *
 * The quick test is the other short run: the slowest tracks of the full run, twice each with one character and the
 * player's own anti-aliasing setting, about 20 minutes. It is for telling whether a change made the slow spots
 * faster: its results compare each track with the quick test before it (usually the previous build). It is started
 * with the "Quick test" button in the Test run confirmation, or always runs with:
 *   DKR_AUTORUN_QUICK=1               Turns it on (instead of the full run).
 *   DKR_AUTORUN_QUICK_TRACKS=id,...   Tracks (default: Haunted Woods, Jungle Falls, Crescent Island, Whale Bay and
 *                                     EverFrost Peak, the slowest of hardware test run issue #12: three GPU-bound car
 *                                     tracks, one renderer-bound hover and one plane track).
 *   DKR_AUTORUN_QUICK_CHARACTER=n     Character (default Bumper, as the anti-aliasing benchmark).
 *   DKR_AUTORUN_QUICK_PASSES=n        Races per track (2).
 * Its files are quick_test_state.txt and quick_test_results.csv (with a "build" column).
 *
 * The boss fights test races every boss once (Tricky, Bluey, Bubbler, Smokey and Wizpig, the first and the second
 * fight of each), with one character, about half an hour. Boss levels aren't on the tracks menu, so the full run
 * never races them. It is started with the "Boss fights" button in the Test run confirmation, or always runs with:
 *   DKR_AUTORUN_BOSS=1                Turns it on (instead of the full run).
 *   DKR_AUTORUN_BOSS_TRACKS=id,...    Levels (default: the ten boss fights).
 *   DKR_AUTORUN_BOSS_CHARACTER=n      Character (default Bumper, as the other short runs).
 *   DKR_AUTORUN_BOSS_PASSES=n         Races per boss (1).
 * Its files are boss_test_state.txt and boss_test_results.csv (with a "build" column).
 *
 * The whole game run covers as much of the game as can be played without a person: a pass through the menus (title,
 * options, character select, the adventure and tracks menus), every race track with every vehicle it allows (the
 * characters taking turns), each race again in its adventure form with its silver coins, the four battle arenas, the ten
 * boss fights, and a tour of every hub of the overworld, where the kart is put in each part (segment) of the map in turn
 * and turned all the way round, as no AI course runs there. The adventure levels play with an everything-unlocked
 * progress in memory (the save files are never written, and the progress is put back when the run ends). Each part ends
 * with a progress report: the run's issue is made after the first part and each later part adds a comment with its
 * table (fps, 1% low, what limits it, hitches), so a run of several hours can be followed as it goes; no race starts
 * while a report is going out. The short variant (about an hour, for gathering profile-guided compiling data) races one
 * lap of each track with its usual vehicle, two silver coin races, one boss and one hub. It is started with the "Whole
 * game" or "Whole game (short)" row of the Test run menu, or always runs with:
 *   DKR_AUTORUN_WHOLE=1|2             Turns it on (2: the short variant).
 *   DKR_AUTORUN_WHOLE_PARTS=mrcabh    Only these parts: menus, races, coins, arenas, bosses, hubs (default all).
 *   DKR_AUTORUN_WHOLE_LEVELS=id,...   Only the stops in these levels (the menus are level 21, the front end).
 *   DKR_AUTORUN_REPORT_MINUTES=n      A progress report at least this often too (default 30; 0 only after each part).
 * Its files are wholegame_state.txt and wholegame_results.csv (with "stop", "part" and "build" columns); the issue's
 * number is kept in test_run_issue.txt. port_autorun_section_done (weak, empty) is called as each part ends.
 *
 * The order is character by character, each over every track, so stopping early still covers every track.
 * Progress is saved before each race (autorun_state.txt), so after a crash or a hang the next launch resumes with the
 * race after the one that failed, and records that race's failure. Each race adds a line to autorun_results.csv,
 * and problems also go to autorun_problems.txt (both in the data directory, sdmc:/3ds/dkr/ on the 3DS).
 * port/tools/autorun_report.py turns the results into a report. The game's own save is never written meanwhile.
 * When the run ends (or is stopped), the results are sent as a GitHub issue if bug reports are set up.
 */
#include <ultra64.h>
#include "types.h"
#include "structs.h"
#include "asset_enums.h"
#include "game.h"
#include "menu.h"
#include "objects.h"
#include "thread3_main.h"
#include "camera.h"
#include "math_util.h"
#include "tracks.h"
#include "port.h"
#include "plat/plat.h"
#include "port_drawtag.h"
#include "plat/profile.h"
#include "plat/recorder.h"
#include "gfx/gfx_split.h"
#include <string.h>

#define MAX_RACES 1024
#define MAX_TRACKS 64
#define FINISH_LINGER_SECONDS 8 // After the finish line: the post-race camera and results, then on to the next.
#define UNSTICK_SECONDS 10      // No new checkpoint passed for this long: the kart is put at the next checkpoint.
#define STUCK_SECONDS 60        // No new checkpoint passed for this long (even so) counts as stuck.
#define STATE_FILE "autorun_state.txt"
#define RESULTS_FILE "autorun_results.csv"
#define PROBLEMS_FILE "autorun_problems.txt"
#define HITCH_FILE "autorun_hitches.txt" // Every hitch of the run, with its cause (the flight recorder, plat/recorder.h)
#define DRAWMIX_FILE "autorun_drawmix.csv" // Each race's cost per kind of thing drawn and per object kind (profile.h)
#define HANG_FILE "autorun_hang.txt"
#define BENCH_STATE_FILE "gpu_bench_state.txt"
#define BENCH_RESULTS_FILE "gpu_bench_results.csv"
#define QUICK_STATE_FILE "quick_test_state.txt"
#define QUICK_RESULTS_FILE "quick_test_results.csv"
#define QUICK_PASSES 2
#define BOSS_STATE_FILE "boss_test_state.txt"
#define BOSS_RESULTS_FILE "boss_test_results.csv"
#define BOSS_PASSES 1
#define SWITCH_STATE_FILE "switch_bench_state.txt"
#define SWITCH_RESULTS_FILE "switch_bench_results.csv"
#define WHOLE_STATE_FILE "wholegame_state.txt"
#define WHOLE_RESULTS_FILE "wholegame_results.csv"
#define RUN_ISSUE_FILE "test_run_issue.txt" // The whole game run's issue number (written by the bug report sender)
#define TOUR_SECONDS 4      // A hub tour's time in each segment, turning the kart once round (short run: 2)
#define ARENA_SECONDS 150   // A battle arena is played this long unless it ends first (short run: 60)
#define MENU_STEP_TIMEOUT 20 // Seconds the menu pass waits for a menu before giving up on that step
#define MENU_PASS_SECONDS 300 // The whole menu pass, after which it goes back to the title screen however it can
#define REPORT_WAIT_SECONDS 300 // The longest the run waits for a progress report to go out before going on

/* Which run: the full test run, the GPU benchmark, the quick test, the boss fights or the switch bench. */
enum Mode { MODE_FULL, MODE_BENCH, MODE_QUICK, MODE_BOSS, MODE_SWITCH, MODE_WHOLE, MODE_COUNT };

/* What a stop of the whole game run is; each kind is one part of the run, in this order. Every other run's races are
 * STOP_RACE. */
enum StopKind { STOP_MENUS, STOP_RACE, STOP_COINS, STOP_ARENA, STOP_BOSS, STOP_HUB, STOP_KINDS };
static const char *const sStopKindNames[STOP_KINDS] = { "menus", "race", "coins", "arena", "boss", "hub" };
static const char *const sPartNames[STOP_KINDS] = { "Menus", "Races", "Silver coin races", "Battle arenas",
                                                    "Boss fights", "Hub tours" };
static const char sPartLetters[STOP_KINDS] = { 'm', 'r', 'c', 'a', 'b', 'h' };

typedef struct Race {
    s8 track;
    s8 character;
    s8 vehicle;
    s8 config; // The settings for the race (an index into sBenchConfigs, or the switch bench's set), or -1 to leave them.
    s8 kind;   // enum StopKind
    s8 laps;   // Laps to race instead of the track's own, or 0
} Race;

/* The GPU benchmark's render settings. The first is the default ones, which the others are compared with. */
typedef struct BenchConfig {
    s8 aa;    // PLAT_AA_*
    s8 fps60; // The 60 fps cap
    const char *name;
} BenchConfig;

static const BenchConfig sBenchConfigs[] = {
    { PLAT_AA_4X, 0, "AA 4x" },
    { PLAT_AA_2X, 0, "AA 2x" },
    { PLAT_AA_OFF, 0, "AA off" },
    { PLAT_AA_OFF, 1, "60 fps, AA off" },
    { PLAT_AA_2X, 1, "60 fps, AA 2x" },
};
#define BENCH_CONFIGS ((s32) ARRAY_COUNT(sBenchConfigs))

/*
 * The switch bench's experimental switches (plat_switch names, read by the renderer at the start of a frame). Each
 * is raced at its usual setting and, by itself, at its test setting; a switch can set up to three variables together,
 * and several switches can share a variable (one that the others need).
 */
#define BENCH_SWITCH_VARS 3
typedef struct BenchSwitch {
    const char *name; // For the results: what the test setting does.
    const char *env[BENCH_SWITCH_VARS];
    short usual, test;
    s8 alone; // A measurement, not a setting: left out of the race with every switch flipped together.
    s8 check; // Checks a bug fix, not performance: raced for one lap (user rule 2026-11-01). When every row is one,
              // the usual races are one lap too.
} BenchSwitch;

/* Command list splits (P19) are off by default (#153), early depth (P20) was dropped (#155), the white-line
 * diagnostics (P28) are off for good, and switch bench #176 settled the Phase 7b switches on the New 3DS: the depth
 * clear as a memory fill (P18), the cheaper per-draw settings (P23) and edge trimming dropping what lies wholly
 * outside (P22) are on by default, while textures in VRAM (P17), cache flushes as a system call (P24) and VFP vector
 * transforms (P27) gained nothing and stay off. The Old 3DS (roadmap O8) is another matter: its renderer is CPU-bound
 * on a slower core without a second level cache. There VFP vectors won (#238), and the texture hash kept for 8 frames
 * and cache flushes by system call won together (#241), so all three are on by default on an Old 3DS; redrawing the
 * bottom screen's race HUD less often gained nothing (#241). Rendering ahead by 2 frames won on both of its renderer
 * cores (#244, #245), so it is the Old 3DS default and the bench pins it there. In #251 a steady 20 fps cap matched
 * or beat 30 with far fewer slower frames, so it is the Old 3DS default and the bench races at 20 there. The New
 * 3DS's third top screen buffer (P35) was at most 1 fps ahead but took memory textures needed (#252), so it stays
 * off there. The frame pacer (gfx_c3d.c) still cost frames on two tracks of three, so it is raced off against on.
 * The Old 3DS renderer cuts (P38) each won in #255 (Jungle Falls 24.7 fps with all of them, 20.4 to 22.4 with one
 * off) and the walk and draw breakdowns cost 2.5 to 4 fps there, so those rows are gone. The 20 fps cap now holds
 * (src/video.c; #255 ran 60% of its frames at 30 under it), so 30 is raced against it. This list is the Old 3DS's:
 * its usual 20 fps would pin a New 3DS to 20 too. The GPU route (ideas from other 3DS ports, shelved on the New 3DS
 * after #237 where it gained nothing on a GPU-bound frame): GPU vertex maths (P30) by itself, then with the track's
 * geometry kept on the GPU (P31), then with models recorded the first time they are drawn too (P34); off by default
 * until this bench says otherwise on an Old 3DS.
 * Bench #258/#259 (v21/v22, Old 3DS) settled the rest: the frame pacer stays on (off had more frames slower than 20 fps;
 * still raced, as the bench needs one row),
 * the 20 fps cap stays (30 reached 30 on only 11% of frames), and the GPU route gained nothing there either (P31 equal,
 * P34 6.7 ms more renderer time, P30 tunnel walls missing as on the New 3DS, #234), so it is off and out of the bench.
 * Bench #263 (v24, System 80%): the two-core renderer (P21) ran 29 fps against 20-21, with 93% of frames at 30, and
 * showed the 20 fps cap had never held on the port (its code sat in the N64 branch of src/video.c). So 30 is the Old
 * 3DS's cap and the bench races at 30 there; P21 is still raced while its best renderer core is found.
 * The two-core renderer is now the Old 3DS default, raced off; with the GPU mostly idle (7-9 ms of a 33 ms frame) the
 * looks an Old 3DS has off for speed are raced on one at a time (anti-aliasing 2x, Balanced widescreen, colour boost),
 * and render ahead 1, as the split already keeps two frames in flight.
 * Bench #266 (v27, Jungle Falls): the two-core renderer off fell to 21 fps, so it is out of the bench; anti-aliasing 2x,
 * Balanced widescreen and render ahead 1 each kept 97-98% of frames at 30, so an Old 3DS now races with them as the
 * New 3DS does, and each is raced back to the old setting on the heavier tracks. Colour boost cost nothing (a look,
 * left to the player).
 * The two-core renderer's backend is woken once 16 KB of draws are queued rather than for every draw (each wake-up is
 * a system call on both cores); raced against waking it every time.
 * Back faces culled in the walk (P40, DKR_CPU_CULL, on by default on an Old 3DS) is raced off against on: it stops
 * a command's double sided triangles splitting its draw from the one sided ones (about one draw in eight).
 * The list ends with an empty entry, so it may be otherwise empty. */
static const BenchSwitch sSwitchesOld3ds[] = {
    { "backend woken for every draw (P21)", { "DKR_SPLIT_WAKE_KB", NULL }, 16, 0 },
    { "render ahead 2", { "BENCH_RENDER_AHEAD", NULL }, 1, 2 },
    { "anti-aliasing off", { "BENCH_AA", NULL }, PLAT_AA_2X, PLAT_AA_OFF },
    { "the N64's 4:3", { "BENCH_WIDESCREEN", NULL }, 2, 0 },
    { "back faces culled by the GPU, not the walk (P40 off)", { "DKR_CPU_CULL", NULL }, 1, 0 },
    // The cost rows (the profiler's monitor, 2026-10-31): a kind of thing left undrawn (DKR_SKIP_KINDS, a bit per
    // port_drawtag.h kind) against the usual race shows what it costs the GPU, which can't time draws one by one.
    { "cost: track not drawn", { "DKR_SKIP_KINDS", NULL }, 0, (1 << 1) | (1 << 5), 1 },
    { "cost: racers and objects not drawn", { "DKR_SKIP_KINDS", NULL }, 0, (1 << 2) | (1 << 3), 1 },
    { "cost: sky not drawn", { "DKR_SKIP_KINDS", NULL }, 0, 1 << 4, 1 },
    { "cost: effects and shadows not drawn", { "DKR_SKIP_KINDS", NULL }, 0, (1 << 6) | (1 << 7), 1 },
    { NULL, { NULL, NULL, NULL }, 0, 0 },
};
/* New 3DS: switch bench #286 (2026-10-31) made the two-core renderer (P21) the default (renderer 7.4-8.4 ms to
 * 3.7-4.3 ms, Haunted Woods' 1% low 30 to 58 fps); the P38 cuts, the P40 cull and texture hashes kept 8 frames were
 * within noise. At the player's 800 pixels and 4x anti-aliasing the GPU is the limit instead (#289, #291: 15-16 ms of
 * a 16.7 ms frame, Crescent Island down to 45 fps), so the New 3DS races at those settings (sBenchRenderNew3ds) and
 * measures the GPU there. Bench #309 made adaptive anti-aliasing (P37) the player's default, but the usual race keeps
 * it off so the GPU's full cost stays measurable; its row shows what a player gets. Textures in VRAM (P17, 0.1-0.2 ms
 * in #309) left the list. The fixed cost (about 9-10 ms with nothing drawn) is split by the colour boost and
 * 400-pixel rows. Bench #320: the sky drawn last (P41) and a depth buffer without stencil (P42) saved nothing (left
 * off, rows dropped); the untextured track row looks for what makes a track pixel cost. */
static const BenchSwitch sSwitchesNew3ds[] = {
    // Trimmed 2026-11-01 (user): the cost rows were measured in #320/#332, adaptive AA is the default, and every
    // Old 3DS default the head flicker hunt bisected still flickered, as did per-queue wake-ups (now on by default).
    // What is left is the one Old 3DS difference not yet raced: the other karts' lower detail models.
    { "head: other karts at Old 3DS detail (racer LOD 2)", { "DKR_RACER_LOD", NULL }, 0, 2, 1, 1 },
    // The GPU misses some settings written right after a draw (drain_before_draw); only blend and depth changes are
    // written twice. A flicker that grows with GPU load (less with AA off, user 2026-11-01) fits a missed texture or
    // combiner setting: this writes every draw's settings twice.
    { "head: every draw's settings written twice", { "DKR_REWRITE", NULL }, 1, 2, 1, 1 },
    { NULL, { NULL, NULL, NULL }, 0, 0 },
};
/*
 * New 3DS: the shelved switches bench ("Shelved switches" in the Test run menu, 2026-11-01; the user asked whether
 * anything shelved earlier is worth another look now that the profiler times every part, the renderer runs on two
 * cores and adaptive anti-aliasing is the default). Every switch shelved for no gain on a New 3DS, at the bench's
 * usual settings. Left out: those shelved for graphics problems (P20 early depth, P30/P31/P34 GPU vertex maths and
 * meshes), those just re-tested at these settings (P17, P37, P41, P42), P40 (a row of the switch bench) and P33
 * (3D only). It is the switch bench with this list (sShort set), so its results go to the same file as a new run.
 */
static const BenchSwitch sSwitchesShelved[] = {
    { "cache flush as a system call (P24)", { "DKR_SVC_FLUSH", NULL }, 0, 1, 0 },
    { "VFP vector maths (P27)", { "DKR_VFP_VECTORS", NULL }, 0, 1, 0 },
    { "texture lookup memo (P38)", { "DKR_TEX_MEMO", NULL }, 0, 1, 0 },
    { "lean walk (P38)", { "DKR_WALK_LEAN", NULL }, 0, 1, 0 },
    { "lean draws (P38)", { "DKR_DRAW_LEAN", NULL }, 0, 1, 0 },
    { "texture hashes checked every 8 frames", { "DKR_TEX_REHASH", NULL }, 1, 8, 0 },
    { NULL, { NULL, NULL, NULL }, 0, 0 },
};
static s32 sShort; // (Defined again below: the whole game run's short variant, or the shelved switches bench)
#define sSwitches (plat_old_3ds() ? sSwitchesOld3ds : sShort ? sSwitchesShelved : sSwitchesNew3ds)
// The usual settings, then each switch flipped
#define SWITCH_CONFIGS                                                                                                 \
    (plat_old_3ds() ? (s32) ARRAY_COUNT(sSwitchesOld3ds)                                                               \
     : sShort       ? (s32) ARRAY_COUNT(sSwitchesShelved)                                                              \
                    : (s32) ARRAY_COUNT(sSwitchesNew3ds))
/* With two switches or more (not counting the cost rows), a last race flips them all together: gains don't always add
 * up. */
static s32 switch_all_config(void) {
    s32 i, settings = 0;
    for (i = 0; i < SWITCH_CONFIGS - 1; i++) {
        settings += !sSwitches[i].alone;
    }
    return settings >= 2 ? SWITCH_CONFIGS : -1;
}
#define SWITCH_ALL switch_all_config()

/* Laps for a switch bench race in config (0 = the usual settings): one for a bug check row, and for the usual races
 * when every row is one; else the track's own. */
static s32 switch_race_laps(s32 config) {
    s32 i, all = SWITCH_CONFIGS > 1;
    for (i = 0; i < SWITCH_CONFIGS - 1; i++) {
        all &= sSwitches[i].check != 0;
    }
    if (config >= 1 && config < SWITCH_CONFIGS) {
        return sSwitches[config - 1].check ? 1 : 0;
    }
    return all ? 1 : 0;
}
#define SWITCH_RACES (SWITCH_CONFIGS + (SWITCH_ALL >= 0))
#define MAX_CONFIGS 16

static s32 sMode; // enum Mode

static s32 config_count(void);
static const char *config_name(s32 config);

/* Sets the race's settings: config -1 puts back the player's render settings and every switch's own value. */
static void apply_config(s32 config) {
    s32 i, k;
    if (config < 0 || config >= config_count()) {
        plat_set_gpu_config(-1);
        plat_set_60fps(-1);
        plat_clear_switch_overrides();
        return;
    }
    if (sMode == MODE_SWITCH) {
        // The menus' settings have no say in the bench's races (user rule, 2026-10-29): every race draws with the
        // bench's own render settings: anti-aliasing 2x, 60 fps, Balanced widescreen and render ahead 1 on a New 3DS;
        // anti-aliasing 2x, 30 fps, Balanced widescreen and render ahead 1 on an Old 3DS;
        // 400 pixels, no 3D, mipmaps on, no colour boost, edge trimming on, GPU vertex maths off. Only
        // where the renderer runs (an Old 3DS's Renderer row) needs a restart, so that one is the player's, and the
        // results' graphics column says which. Then every variable at its usual setting, then the race's switches: a
        // variable several switches share (the GPU route's rows) keeps the race's test setting.
        static const struct {
            const char *name;
            s8 value;
        } sBenchRender[] = {
            { "BENCH_WIDE800", 0 },     { "BENCH_MIPMAPS", 1 },  { "BENCH_COLOUR", 0 },    { "BENCH_GUARD_BAND", 1 },
            { "DKR_STEREO3D", 0 },      { "DKR_GPU_TRANSFORM", 0 }, { "DKR_TRACK_MESH", 0 }, { "DKR_MODEL_MESH", 0 },
        };
        // A New 3DS races at the player's GPU-bound settings (2026-10-31): 800 pixels, 4x anti-aliasing, colour boost.
        static const struct {
            const char *name;
            s8 value;
        } sBenchRenderNew3ds[] = {
            { "BENCH_WIDE800", 1 }, { "BENCH_COLOUR", 1 }, { "BENCH_AA", PLAT_AA_4X },
        };
        plat_set_60fps(!plat_old_3ds());
        for (i = 0; i < (s32) ARRAY_COUNT(sBenchRender); i++) {
            plat_override_switch(sBenchRender[i].name, sBenchRender[i].value);
        }
        plat_override_switch("DKR_FPS_PIN", plat_old_3ds() ? 30 : 60);
        plat_override_switch("BENCH_RENDER_AHEAD", 1);
        plat_override_switch("BENCH_WIDESCREEN", 2);
        plat_override_switch("BENCH_AA", PLAT_AA_2X);
        if (!plat_old_3ds()) {
            for (i = 0; i < (s32) ARRAY_COUNT(sBenchRenderNew3ds); i++) {
                plat_override_switch(sBenchRenderNew3ds[i].name, sBenchRenderNew3ds[i].value);
            }
        }
        for (i = 0; i < SWITCH_CONFIGS - 1; i++) {
            for (k = 0; k < BENCH_SWITCH_VARS && sSwitches[i].env[k] != NULL; k++) {
                plat_override_switch(sSwitches[i].env[k], sSwitches[i].usual);
            }
        }
        for (i = 0; i < SWITCH_CONFIGS - 1; i++) {
            if (config == i + 1 || (config == SWITCH_ALL && !sSwitches[i].alone)) {
                for (k = 0; k < BENCH_SWITCH_VARS && sSwitches[i].env[k] != NULL; k++) {
                    plat_override_switch(sSwitches[i].env[k], sSwitches[i].test);
                }
            }
        }
        if (plat_switch("BENCH_400", 0)) {
            plat_override_switch("BENCH_WIDE800", 0);
            plat_override_switch("BENCH_AA", PLAT_AA_2X);
        }
        plat_set_gpu_config(plat_switch("BENCH_AA", PLAT_AA_OFF)); // The bench's anti-aliasing, or a row's
        return;
    }
    plat_set_gpu_config(sBenchConfigs[config].aa);
    plat_set_60fps(sBenchConfigs[config].fps60);
}

static void set_phase(s32 phase);
static void turn_off(const char *why);

enum Phase {
    PHASE_OFF,
    PHASE_TITLE,    // Waiting for the title screen to start the next race from.
    PHASE_LOADING,  // Race requested, waiting for it to load.
    PHASE_COUNTDOWN,
    PHASE_RACING,
    PHASE_FINISHED, // The kart crossed the line (or the race was stopped); lingering before leaving.
    PHASE_LEAVING   // Asked the game to go back to the title screen.
};

static const char *const sCharacterNames[NUMBER_OF_CHARACTERS] = {
    "Krunch", "Bumper", "Tiptup", "Conker", "Timber", "Banjo", "Drumstick", "Pipsy", "T.T.", "Diddy",
};
static const char *const sVehicleNames[3] = { "car", "hover", "plane" };

enum Request {
    REQUEST_NONE, REQUEST_START, REQUEST_STOP, REQUEST_BENCH, REQUEST_QUICK, REQUEST_BOSS, REQUEST_SWITCH,
    REQUEST_WHOLE, REQUEST_WHOLE_SHORT, REQUEST_SWITCH_SHELVED
};

typedef struct ModeInfo {
    const char *name;        // For the bottom screen and the results.
    const char *stateFile;
    const char *resultsFile;
    const char *extraColumn; // The results file's column after the full run's ones, or NULL.
    const char *env;         // Turns it on at every start.
} ModeInfo;

static const ModeInfo sModes[MODE_COUNT] = {
    { "Test run", STATE_FILE, RESULTS_FILE, NULL, "DKR_AUTORUN" },
    { "GPU benchmark", BENCH_STATE_FILE, BENCH_RESULTS_FILE, "config,pct_60fps", "DKR_AUTORUN_BENCH" },
    { "Quick test", QUICK_STATE_FILE, QUICK_RESULTS_FILE, "build", "DKR_AUTORUN_QUICK" },
    { "Boss fights", BOSS_STATE_FILE, BOSS_RESULTS_FILE, "build", "DKR_AUTORUN_BOSS" },
    { "Switch bench", SWITCH_STATE_FILE, SWITCH_RESULTS_FILE, "config,pct_60fps", "DKR_AUTORUN_SWITCH" },
    { "Whole game", WHOLE_STATE_FILE, WHOLE_RESULTS_FILE, "stop,part,build", "DKR_AUTORUN_WHOLE" },
};

/* The benchmarks: their races each have a config (an index into sBenchConfigs, or a switch set). */
static s32 is_bench(void) {
    return sMode == MODE_BENCH || sMode == MODE_SWITCH;
}

static s32 config_count(void) {
    return sMode == MODE_SWITCH ? SWITCH_RACES : BENCH_CONFIGS;
}

static const char *config_name(s32 config) {
    if (config < 0 || config >= config_count()) {
        return "?";
    }
    if (sMode == MODE_SWITCH) {
        return config == 0 ? "usual settings" : config == SWITCH_ALL ? "all switches together (not the cost rows)" : sSwitches[config - 1].name;
    }
    return sBenchConfigs[config].name;
}

static s32 sEnabled = -1;
static s32 sShort;          // The whole game run's short variant, or the switch bench's shelved switches
static s32 sReportedAt = -1; // The whole game run: the stop before which the last progress report went out
static unsigned long long sLastReportNs;
static unsigned long long sReportWaitNs; // When the run started waiting for a report to go out, or 0
static s32 sTourSegment;    // A hub tour's segment
static s32 sTourRetraces;   // Game time in it
static s32 sTourSegments;   // Segments toured so far
static s32 sInitialised;
static volatile s32 sRequest;
static s32 sQuitPending;
static double sFpsSum;
static s32 sFpsCount;
static s32 sPhase;
static Race sRaces[MAX_RACES];
static s32 sNumRaces;
static s32 sNext;  // Index of the race being run, or the next one.
static s32 sRun;   // Run number, counted up each time the list starts over.
static u32 sConfigHash;
static s32 sTimeoutSeconds;
static s32 sTitleFrames;
static unsigned long long sPhaseStartNs;
static unsigned long long sLoadNs;
static s32 sRaceRetraces;  // Game time while racing, in 60ths of a second.
static s32 sStuckRetraces; // Game time since the kart last got further than ever along the course.
// The most checkpoints passed this race. A kart that goes back past a checkpoint loses one, so a kart circling over
// a checkpoint passes it again and again without getting anywhere: only a new best counts as progress.
static s32 sBestProgress;
static f32 sLastPosition[3]; // The kart's position last frame, for saying where it left the race.
static s32 sDriving;         // The kart has got going (a boss first talks to the player for a while).
static s32 sRaceLevel;       // The level the kart got going in.
static s32 sUnsticks;        // Times this race the kart was put at the next checkpoint.
static s32 sLastUnstick;     // Lap * 1000 + the checkpoint the kart was last put at.
static char sUnstuckAt[64]; // Where, as "lap:checkpoint" pairs.
static s32 sLingerRetraces;
static const char *sOutcome;
static s32 sFinished, sProblems;
static ProfStats sRaceStats;
/* The sound's gap and drop counters when the race started (roadmap P9 is judged by them). */
static u32 sAudioGapsStart, sAudioDropsStart;

static s32 str_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void make_path(char *out, s32 size, const char *name) {
    plat_snprintf(out, size, "%s/%s", plat_data_dir(), name);
}

static const char *state_file(void) {
    return sModes[sMode].stateFile;
}

static const char *results_file(void) {
    return sModes[sMode].resultsFile;
}

/*
 * The results and state files are written by a thread of their own. On the 3DS every file opened, replaced or renamed
 * on the SD card can take 100 ms and more (looking up a name reads the whole data directory, which keeps every bug
 * report snapshot), and the results file used to be read and written whole: writing them as a race ended held the
 * game for most of a second at the finish line (test runs #250 and #254: "long game frame: 773 / 829 ms" right after
 * each race's results line). The writes keep their order; whatever reads the files back first waits for them
 * (io_flush).
 */
#define IO_JOBS 16

typedef struct IoJob {
    char path[256];
    char *text;
    char *header;       // Appends: written first if the file doesn't exist yet or is empty (or NULL).
    s32 append;         // Added to the end of the file, else it replaces the file.
} IoJob;

static IoJob sIoJobs[IO_JOBS];
static volatile u32 sIoQueued, sIoDone; // Jobs queued by the game thread, jobs the writer has finished.
static PlatSem *sIoWake;

static void io_thread(UNUSED void *arg) {
    for (;;) {
        IoJob *job;
        unsigned long long start;
        plat_sem_wait(sIoWake);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        job = &sIoJobs[sIoDone % IO_JOBS];
        start = plat_time_ns();
        if (job->append) {
            if (job->header != NULL) {
                char probe[1];
                if (plat_load_file_start(job->path, probe, sizeof(probe)) <= 0) { // Missing, or emptied for a new run
                    plat_append_file(job->path, job->header, strlen(job->header));
                }
            }
            plat_append_file(job->path, job->text, strlen(job->text));
        } else {
            plat_save_file(job->path, job->text, strlen(job->text));
        }
        if (plat_time_ns() - start > 50000000ULL) {
            port_log("autorun: writing %s took %d ms", job->path, (int) ((plat_time_ns() - start) / 1000000ULL));
        }
        plat_free(job->text);
        plat_free(job->header);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        sIoDone++;
    }
}

/* Waits until the writer has written everything queued. */
static void io_flush(void) {
    while (sIoDone != sIoQueued) {
        plat_sleep_ns(1000000);
    }
}

/* Hands a file write to the writer thread (text is copied). */
static void io_queue(const char *name, const char *text, const char *header, s32 append) {
    IoJob *job;
    char *copy, *headerCopy = NULL;
    u32 length = strlen(text);
    if (sIoWake == NULL) {
        sIoWake = plat_sem_create(0);
        // Below every game thread, on the game's core, which is idle most of each frame.
        plat_thread_create(io_thread, NULL, 2, PLAT_THREAD_GAME, "autorun files");
    }
    while (sIoQueued - sIoDone >= IO_JOBS) {
        plat_sleep_ns(1000000);
    }
    copy = plat_malloc(length + 1);
    if (copy == NULL) {
        return;
    }
    bcopy(text, copy, length + 1);
    if (header != NULL) {
        headerCopy = plat_malloc(strlen(header) + 1);
        if (headerCopy != NULL) {
            bcopy(header, headerCopy, strlen(header) + 1);
        }
    }
    job = &sIoJobs[sIoQueued % IO_JOBS];
    make_path(job->path, sizeof(job->path), name);
    job->text = copy;
    job->header = headerCopy;
    job->append = append;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    sIoQueued++;
    plat_sem_post(sIoWake);
}

/* Appends text to a file in the data directory, first writing header (if not NULL) when the file is new. */
static void append_file(const char *name, const char *text, const char *header) {
    io_queue(name, text, header, TRUE);
}

static void hash_text(const char *text) {
    if (text == NULL) {
        text = "-";
    }
    for (; *text; text++) {
        sConfigHash = sConfigHash * 31 + (u8) *text;
    }
    sConfigHash = sConfigHash * 31 + ';';
}

/* Parses "all" or a comma separated list into ids. Returns the count, or -1 for "all" / not set. */
static s32 parse_list(const char *list, s8 *ids, s32 maxIds) {
    s32 count = 0;
    if (list == NULL || list[0] == 'a') {
        return -1;
    }
    while (*list && count < maxIds) {
        ids[count++] = port_atoi(list);
        while (*list && *list != ',') {
            list++;
        }
        if (*list == ',') {
            list++;
        }
    }
    return count;
}

/*
 * The short runs: each track a number of times with one character. The benchmark races each track once with each of
 * its render settings; the quick test and the boss fights leave the settings as they are.
 */
static void build_short_list(void) {
    static const s8 sBenchTracks[] = { 29, 10, 31 };        // Jungle Falls, Crescent Island, Haunted Woods
    // The New 3DS switch bench: the tracks where 800 pixels and 4x anti-aliasing fall below 60 most (#289, #291).
    static const s8 sSwitchTracksNew3ds[] = { ASSET_LEVEL_HAUNTEDWOODS, ASSET_LEVEL_CRESCENTISLAND }; // head flicker
    static const s8 sQuickTracks[] = { 31, 29, 10, 8, 13 }; // Haunted Woods, Jungle Falls, Crescent Island,
                                                            // Whale Bay, EverFrost Peak
    static const s8 sBossTracks[] = {
        ASSET_LEVEL_TRICKYTOPS1, ASSET_LEVEL_BLUEY1, ASSET_LEVEL_BUBBLER1, ASSET_LEVEL_SMOKEY1, ASSET_LEVEL_WIZPIG1,
        ASSET_LEVEL_TRICKYTOPS2, ASSET_LEVEL_BLUEY2, ASSET_LEVEL_BUBBLER2, ASSET_LEVEL_SMOKEY2, ASSET_LEVEL_WIZPIG2,
    };
    s32 quick = sMode == MODE_QUICK, boss = sMode == MODE_BOSS;
    s32 numBenchConfigs = config_count();
    s32 newSwitch = sMode == MODE_SWITCH && !plat_old_3ds();
    const s8 *defaultTracks = boss ? sBossTracks : quick ? sQuickTracks : newSwitch ? sSwitchTracksNew3ds : sBenchTracks;
    s32 numDefault = boss        ? (s32) ARRAY_COUNT(sBossTracks)
                     : quick     ? (s32) ARRAY_COUNT(sQuickTracks)
                     : newSwitch ? (s32) ARRAY_COUNT(sSwitchTracksNew3ds)
                                 : (s32) ARRAY_COUNT(sBenchTracks);
    const char *prefix = boss                    ? "DKR_AUTORUN_BOSS"
                         : quick                 ? "DKR_AUTORUN_QUICK"
                         : sMode == MODE_SWITCH ? "DKR_AUTORUN_SWITCH"
                                                 : "DKR_AUTORUN_BENCH";
    char name[48];
    s8 tracks[MAX_TRACKS], character[1], configs[MAX_CONFIGS];
    s32 numTracks, numLevels, numWorlds, t, pass, passes, numConfigs;

    plat_snprintf(name, sizeof(name), "%s_TRACKS", prefix);
    numTracks = parse_list(plat_getenv(name), tracks, MAX_TRACKS);
    if (numTracks < 0) {
        for (numTracks = 0; numTracks < numDefault; numTracks++) {
            tracks[numTracks] = defaultTracks[numTracks];
        }
    }
    plat_snprintf(name, sizeof(name), "%s_CHARACTER", prefix);
    if (parse_list(plat_getenv(name), character, 1) != 1 || character[0] < 0 ||
        character[0] >= NUMBER_OF_CHARACTERS) {
        character[0] = 1; // Bumper
    }
    plat_snprintf(name, sizeof(name), "%s_CONFIGS", prefix);
    numConfigs = parse_list(plat_getenv(name), configs, numBenchConfigs);
    if (numConfigs < 0) {
        for (numConfigs = 0; numConfigs < numBenchConfigs; numConfigs++) {
            configs[numConfigs] = numConfigs;
        }
    }
    passes = numConfigs;
    if (quick || boss) {
        plat_snprintf(name, sizeof(name), "%s_PASSES", prefix);
        passes = plat_getenv(name) ? port_atoi(plat_getenv(name)) : boss ? BOSS_PASSES : QUICK_PASSES;
    }
    level_count(&numLevels, &numWorlds);
    sNumRaces = 0;
    for (t = 0; t < numTracks; t++) {
        if (tracks[t] < 0 || tracks[t] >= numLevels) {
            continue;
        }
        for (pass = 0; pass < passes && sNumRaces < MAX_RACES; pass++) {
            sRaces[sNumRaces].track = tracks[t];
            sRaces[sNumRaces].character = character[0];
            sRaces[sNumRaces].vehicle = leveltable_vehicle_default(tracks[t]);
            sRaces[sNumRaces].config = (quick || boss) ? -1 : configs[pass];
            sRaces[sNumRaces].kind = STOP_RACE;
            sRaces[sNumRaces].laps = sMode == MODE_SWITCH ? switch_race_laps(configs[pass]) : 0;
            sNumRaces++;
        }
    }
}

/* Adds a stop to the whole game run, if DKR_AUTORUN_WHOLE_PARTS and _LEVELS let it. */
static void add_stop(s32 kind, s32 level, s32 character, s32 vehicle, s32 laps) {
    const char *parts = plat_getenv("DKR_AUTORUN_WHOLE_PARTS");
    s8 levels[MAX_TRACKS];
    s32 numLevels = parse_list(plat_getenv("DKR_AUTORUN_WHOLE_LEVELS"), levels, MAX_TRACKS), i;
    if (sNumRaces >= MAX_RACES || (parts != NULL && strchr(parts, sPartLetters[kind]) == NULL)) {
        return;
    }
    for (i = 0; i < numLevels && levels[i] != level; i++) {}
    if (numLevels >= 0 && i == numLevels) {
        return;
    }
    sRaces[sNumRaces].track = level;
    sRaces[sNumRaces].character = character;
    sRaces[sNumRaces].vehicle = vehicle;
    sRaces[sNumRaces].config = -1;
    sRaces[sNumRaces].kind = kind;
    sRaces[sNumRaces].laps = laps;
    sNumRaces++;
}

/*
 * The whole game run: the menus, every race track with every vehicle it allows, the races again with their silver
 * coins (adventure), the battle arenas, the boss fights and the hub tours. The characters take turns (not Drumstick or
 * T.T., who need unlocking), so each is raced and drawn somewhere. The short variant keeps every part but plays less of
 * each: one lap of each track with its usual vehicle, two silver coin races, one boss and one hub.
 */
static void build_whole_list(void) {
    static const s8 sBosses[] = {
        ASSET_LEVEL_TRICKYTOPS1, ASSET_LEVEL_BLUEY1, ASSET_LEVEL_BUBBLER1, ASSET_LEVEL_SMOKEY1, ASSET_LEVEL_WIZPIG1,
        ASSET_LEVEL_TRICKYTOPS2, ASSET_LEVEL_BLUEY2, ASSET_LEVEL_BUBBLER2, ASSET_LEVEL_SMOKEY2, ASSET_LEVEL_WIZPIG2,
    };
    static const s8 sArenas[] = { ASSET_LEVEL_FIREMOUNTAIN, ASSET_LEVEL_ICICLEPYRAMID, ASSET_LEVEL_DARKWATERBEACH,
                                  ASSET_LEVEL_SMOKEYCASTLE };
    static const s8 sHubs[] = { ASSET_LEVEL_CENTRALAREAHUB,       ASSET_LEVEL_DINODOMAINHUB,
                                ASSET_LEVEL_SHERBETISLANDHUB,     ASSET_LEVEL_SNOWFLAKEMOUNTAINHUB,
                                ASSET_LEVEL_DRAGONFORESTHUB,      ASSET_LEVEL_FUTUREFUNLANDHUB };
    static const s8 sCast[] = { 1, 0, 2, 3, 4, 5, 7, 9 }; // Bumper first, then the rest without Drumstick and T.T.
    s8 *ids = (s8 *) get_misc_asset(ASSET_MISC_MAIN_TRACKS_IDS);
    s32 numTracks, t, i, v, usable, turn = 0;

    for (numTracks = 0; ids[numTracks] != -1 && numTracks < MAX_TRACKS; numTracks++) {}
    sNumRaces = 0;
    add_stop(STOP_MENUS, ASSET_LEVEL_FRONTEND, 1, 0, 0);
    for (t = 0; t < numTracks; t++) {
        usable = leveltable_vehicle_usable(ids[t]);
        for (i = 0; i < 3; i++) {
            v = (i == 0) ? leveltable_vehicle_default(ids[t]) : i - (i <= leveltable_vehicle_default(ids[t]));
            if (i > 0 && (sShort || !(usable & (1 << v)))) {
                continue;
            }
            add_stop(STOP_RACE, ids[t], sCast[turn++ % ARRAY_COUNT(sCast)], v, sShort ? 1 : 0);
        }
    }
    // The four worlds' tracks have silver coins (Future Fun Land's are last in the list).
    for (t = 0; t < numTracks && t < 16; t += sShort ? 9 : 1) {
        add_stop(STOP_COINS, ids[t], sCast[turn++ % ARRAY_COUNT(sCast)], leveltable_vehicle_default(ids[t]), 0);
    }
    for (t = 0; t < (s32) ARRAY_COUNT(sArenas); t++) {
        add_stop(STOP_ARENA, sArenas[t], sCast[turn++ % ARRAY_COUNT(sCast)], leveltable_vehicle_default(sArenas[t]), 0);
    }
    for (t = 0; t < (sShort ? 1 : (s32) ARRAY_COUNT(sBosses)); t++) {
        add_stop(STOP_BOSS, sBosses[t], 1, leveltable_vehicle_default(sBosses[t]), 0); // Bumper, as the boss test
    }
    for (t = 0; t < (sShort ? 1 : (s32) ARRAY_COUNT(sHubs)); t++) {
        add_stop(STOP_HUB, sHubs[t], sCast[turn++ % ARRAY_COUNT(sCast)], VEHICLE_CAR, 0);
    }
}

static void build_race_list(void) {
    s8 tracks[MAX_TRACKS], characters[NUMBER_OF_CHARACTERS];
    s32 numTracks, numCharacters, c, t, v, usable, i;
    s32 numLevels, numWorlds;
    s32 allVehicles = plat_getenv("DKR_AUTORUN_VEHICLES") != NULL && plat_getenv("DKR_AUTORUN_VEHICLES")[0] == 'a';

    if (sMode == MODE_WHOLE) {
        build_whole_list();
        return;
    }
    if (sMode != MODE_FULL) {
        build_short_list();
        return;
    }
    numTracks = parse_list(plat_getenv("DKR_AUTORUN_TRACKS"), tracks, MAX_TRACKS);
    if (numTracks < 0) {
        // The tracks menu's own list (every world's four tracks, Future Fun Land included).
        s8 *ids = (s8 *) get_misc_asset(ASSET_MISC_MAIN_TRACKS_IDS);
        for (numTracks = 0; ids[numTracks] != -1 && numTracks < MAX_TRACKS; numTracks++) {
            tracks[numTracks] = ids[numTracks];
        }
    }
    numCharacters = parse_list(plat_getenv("DKR_AUTORUN_CHARACTERS"), characters, NUMBER_OF_CHARACTERS);
    if (numCharacters < 0) {
        for (numCharacters = 0; numCharacters < NUMBER_OF_CHARACTERS; numCharacters++) {
            characters[numCharacters] = numCharacters;
        }
    }
    level_count(&numLevels, &numWorlds);
    sNumRaces = 0;
    for (c = 0; c < numCharacters; c++) {
        if (characters[c] < 0 || characters[c] >= NUMBER_OF_CHARACTERS) {
            continue;
        }
        for (t = 0; t < numTracks; t++) {
            if (tracks[t] < 0 || tracks[t] >= numLevels) {
                continue;
            }
            usable = leveltable_vehicle_usable(tracks[t]);
            for (i = 0; i < 3; i++) {
                // The usual vehicle first, then the others the track allows.
                v = (i == 0) ? leveltable_vehicle_default(tracks[t]) : i - (i <= leveltable_vehicle_default(tracks[t]));
                if (i > 0 && (!allVehicles || !(usable & (1 << v)))) {
                    continue;
                }
                if (sNumRaces < MAX_RACES) {
                    sRaces[sNumRaces].track = tracks[t];
                    sRaces[sNumRaces].character = characters[c];
                    sRaces[sNumRaces].vehicle = v;
                    sRaces[sNumRaces].config = -1;
                    sRaces[sNumRaces].kind = STOP_RACE;
                    sRaces[sNumRaces].laps = 0;
                    sNumRaces++;
                }
            }
        }
    }
}

/* running: the race under way (-1: none). recorded: its result is already in the results file. */
static void save_state_recorded(s32 running, s32 recorded) {
    char text[200];
    plat_snprintf(text, sizeof(text),
                  "active=%d\nconfig=%08x\nrun=%d\nnext=%d\nrunning=%d\nrecorded=%d\nfinished=%d\nproblems=%d\n"
                  "short=%d\nreported=%d\n",
                  (int) (sEnabled > 0), (unsigned int) sConfigHash, (int) sRun, (int) sNext, (int) running,
                  (int) recorded, (int) sFinished, (int) sProblems, (int) sShort, (int) sReportedAt);
    io_queue(state_file(), text, NULL, FALSE);
}

static void save_state(s32 running) {
    save_state_recorded(running, FALSE);
}

static s32 state_value(const char *text, const char *key, s32 fallback) {
    s32 keyLength = strlen(key);
    const char *line = text;
    while (line != NULL && *line) {
        s32 i;
        for (i = 0; i < keyLength && line[i] == key[i]; i++) {}
        if (i == keyLength && line[keyLength] == '=') {
            line += keyLength + 1;
            if (str_equal(key, "config")) {
                u32 v = 0;
                for (; (*line >= '0' && *line <= '9') || (*line >= 'a' && *line <= 'f'); line++) {
                    v = v * 16 + ((*line <= '9') ? *line - '0' : *line - 'a' + 10);
                }
                return (s32) v;
            }
            return port_atoi(line);
        }
        line = strchr(line, '\n');
        if (line != NULL) {
            line++;
        }
    }
    return fallback;
}

static const char *track_name(s32 track) {
    const char *name = level_name(track);
    return (name != NULL) ? name : "?";
}

/* The stop's name for the results: the level's, or what is done there. */
static const char *stop_name(const Race *race) {
    static char text[2][48];
    static s32 which;
    if (race->kind == STOP_MENUS) {
        return "Menus";
    }
    if (race->kind == STOP_HUB || race->kind == STOP_COINS) {
        which ^= 1;
        plat_snprintf(text[which], sizeof(text[which]), "%s %s", track_name(race->track),
                      race->kind == STOP_HUB ? "tour" : "coins");
        return text[which];
    }
    return track_name(race->track);
}

static void record_problem(const char *what) {
    char text[256];
    Race *race = &sRaces[sNext];
    plat_snprintf(text, sizeof(text), "run %d race %d/%d: %s, %s, %s: %s\n", (int) sRun, (int) sNext + 1,
                  (int) sNumRaces, stop_name(race), sCharacterNames[(s32) race->character],
                  sVehicleNames[(s32) race->vehicle], what);
    append_file(PROBLEMS_FILE, text, NULL);
    port_log("autorun: %s", text);
}

static double ms(const ProfStats *s, s32 stage, u32 frames) {
    return frames ? s->stageNs[stage] / 1e6 / frames : 0.0;
}

static s32 pct(u32 part, u32 whole) {
    return whole ? (s32) (((unsigned long long) part * 100 + whole / 2) / whole) : 0;
}

static const char *aa_label(void) {
    s32 aa = plat_antialias();
    return aa == PLAT_AA_4X ? "4x" : aa == PLAT_AA_2X ? "2x" : "off";
}

/* The graphics settings the frames are drawn with now, for the results (e.g. "aa4x/60fps/ahead2"; an Old 3DS
 * renderer on the system core adds its share, e.g. "/sys50"). */
static const char *graphics_setup(void) {
    static char text[48];
    s32 share = plat_renderer_syscore_share();
    plat_snprintf(text, sizeof(text), "aa%s/%dfps/ahead%d", aa_label(), 60 / plat_frame_retraces(),
                  plat_render_ahead());
    if (share > 0) {
        s32 len = (s32) strlen(text);
        plat_snprintf(text + len, sizeof(text) - len, "/sys%d", (int) share);
    }
    if (gfx_split_active()) {
        s32 len = (s32) strlen(text);
        plat_snprintf(text + len, sizeof(text) - len, "/split");
    }
    return text;
}

/*
 * The flight recorder's hitches (plat/recorder.h) since the race before ended, so the results screen, the menus and the
 * load before this race count too, each with what took the time and what happened then: written to HITCH_FILE, and
 * counted by cause for the run's issue, which also lists the run's worst.
 */
#define WORST_HITCHES 8
static unsigned int sHitchSince;
static unsigned int sRunHitches, sRunCauses[REC_CAUSE_COUNT];
static struct {
    unsigned int periodUs;
    char text[200];
} sWorstHitches[WORST_HITCHES];
static s32 sWorstHitchCount;

static void reset_hitches(void) {
    sHitchSince = rec_frame_now();
    sRunHitches = 0;
    bzero(sRunCauses, sizeof(sRunCauses));
    sWorstHitchCount = 0;
}

static void record_hitches(const char *outcome) {
    static char text[6 * 1024];
    const RecState *rec = rec_state();
    Race *race = &sRaces[sNext];
    unsigned int counts[REC_CAUSE_COUNT], found, n;
    s32 length, c, i;

    bzero(counts, sizeof(counts));
    length = plat_snprintf(text, sizeof(text), "\n## run %d race %d/%d: %s, %s, %s, %s", (int) sRun, (int) sNext + 1,
                           (int) sNumRaces, stop_name(race), sCharacterNames[(s32) race->character],
                           sVehicleNames[(s32) race->vehicle], graphics_setup());
    if (is_bench()) {
        length += plat_snprintf(text + length, sizeof(text) - length, ", config %d", (int) race->config);
    }
    length += plat_snprintf(text + length, sizeof(text) - length, ": %s\n\n", outcome);
    // At most 4 KB of them a race (about 15 lines), so 200 races stay within the issue's attachment budget.
    found = rec_hitches_since(sHitchSince, text + length, 4096, counts);
    length = strlen(text);
    length += plat_snprintf(text + length, sizeof(text) - length, "%u hitches since the race before ended:", found);
    for (c = 0; c < REC_CAUSE_COUNT; c++) {
        length += plat_snprintf(text + length, sizeof(text) - length, "%s %s %u", c > 0 ? "," : "",
                                rec_cause_name(c), counts[c]);
        sRunCauses[c] += counts[c];
    }
    plat_snprintf(text + length, sizeof(text) - length, "\n");
    sRunHitches += found;
    append_file(HITCH_FILE, text, "# Hitches in each race of the test runs: frames half as long again as usual, with what "
                                  "took the time and what happened then (port/src/plat/recorder.c).\n");

    // The run's worst, longest first.
    for (n = rec->hitchCount > REC_HITCHES ? rec->hitchCount - REC_HITCHES : 0; n < rec->hitchCount; n++) {
        const RecHitch *h = &rec->hitches[n % REC_HITCHES];
        if (h->frame < sHitchSince) {
            continue;
        }
        for (i = sWorstHitchCount; i > 0 && sWorstHitches[i - 1].periodUs < h->periodUs; i--) {
            if (i < WORST_HITCHES) {
                sWorstHitches[i] = sWorstHitches[i - 1];
            }
        }
        if (i < WORST_HITCHES) {
            sWorstHitches[i].periodUs = h->periodUs;
            plat_snprintf(sWorstHitches[i].text, sizeof(sWorstHitches[i].text), "race %d (%s): %.150s", (int) sNext + 1,
                          stop_name(race), h->text);
            if (sWorstHitchCount < WORST_HITCHES) {
                sWorstHitchCount++;
            }
        }
    }
    sHitchSince = rec_frame_now();
}

/*
 * Each race's draw mix and object mix (profile.h): one row per kind drawn, per object kind that cost anything, and one
 * for the scene, into DRAWMIX_FILE; and the run's totals for its issue. The window restarts as each race starts.
 */
#define DRAWMIX_TOP_OBJECTS 12
static ProfDrawMix sRunMix;
static ProfSceneCounts sRunScene;
static ProfObjMix sRunObj;

static void reset_draw_mix(void) {
    bzero(&sRunMix, sizeof(sRunMix));
    bzero(&sRunScene, sizeof(sRunScene));
    bzero(&sRunObj, sizeof(sRunObj));
}

static void record_draw_mix(const char *outcome) {
    static char text[8 * 1024];
    static ProfDrawMix mix;
    static ProfSceneCounts scene;
    const ProfObjMix *obj = prof_object_mix_window();
    Race *race = &sRaces[sNext];
    char prefix[160];
    double frames, timed, gameTimed, audited, objFrames, objTimed, objRenderTimed;
    u8 picked[PROF_OBJ_BUCKETS];
    s32 length = 0, k, n;

    prof_draw_mix_window(&mix, &scene, FALSE);
    if (mix.frames == 0) {
        return;
    }
    frames = (double) mix.frames;
    timed = mix.timedFrames ? (double) mix.timedFrames : 1.0;
    gameTimed = mix.gameTimedFrames ? (double) mix.gameTimedFrames : 1.0;
    audited = mix.auditedFrames ? (double) mix.auditedFrames : 1.0;
    plat_snprintf(prefix, sizeof(prefix), "%d,%d,%s,%s,%s,%s,%d,%s", (int) sRun, (int) sNext + 1,
                  stop_name(race), sCharacterNames[(s32) race->character], sVehicleNames[(s32) race->vehicle],
                  graphics_setup(), is_bench() ? (int) race->config : -1, outcome);
    for (k = 0; k < PORT_DRAW_KINDS; k++) {
        const ProfKindStats *st = &mix.kinds[k];
        double a = st->audited ? (double) st->audited : 1.0;
        if (st->draws == 0) {
            continue;
        }
        length += plat_snprintf(text + length, sizeof(text) - length,
                                "%s,kind,%s,%.1f,%.1f,%.1f,%.3f,%.3f,,,,%.1f,%.1f,%.1f,%.3f,,,,,\n", prefix,
                                prof_kind_name(k), st->tris / frames, st->draws / frames, st->verts / frames,
                                st->renderNs / 1e6 / timed, st->buildNs / 1e6 / gameTimed, st->offscreen * 100.0 / a,
                                st->backface * 100.0 / a, st->tiny * 100.0 / a,
                                st->areaPx / audited / (400.0 * 240.0));
    }
    if (scene.frames > 0) {
        length += plat_snprintf(text + length, sizeof(text) - length,
                                "%s,scene,level,,,,,,,,,,,,,%.1f,%.1f,%.1f,%.1f,%.1f\n", prefix,
                                (double) scene.segments / scene.frames, (double) scene.segmentsAllowed / scene.frames,
                                (double) scene.segmentsDrawn / scene.frames, (double) scene.objects / scene.frames,
                                (double) scene.objectsDrawn / scene.frames);
    }
    objFrames = obj->frames ? (double) obj->frames : 1.0;
    objTimed = obj->timedFrames ? (double) obj->timedFrames : 1.0;
    objRenderTimed = obj->renderTimedFrames ? (double) obj->renderTimedFrames : 1.0;
    for (k = 0; k < PROF_OBJ_BUCKETS; k++) {
        const ProfObjStats *o = &obj->objs[k];
        picked[k] = o->updates == 0 && o->drawn == 0 && o->tris == 0;
    }
    // The costliest object kinds (game logic + building + renderer), the rest of the race's in the run's totals.
    for (n = 0; n < DRAWMIX_TOP_OBJECTS && length < (s32) sizeof(text) - 256; n++) {
        s32 best = -1;
        double bestCost = -1.0;
        for (k = 0; k < PROF_OBJ_BUCKETS; k++) {
            const ProfObjStats *o = &obj->objs[k];
            double cost = (o->logicNs + o->buildNs) / objTimed + o->renderNs / objRenderTimed + o->tris * 1e-3;
            if (!picked[k] && cost > bestCost) {
                best = k;
                bestCost = cost;
            }
        }
        if (best < 0) {
            break;
        }
        picked[best] = TRUE;
        {
            const ProfObjStats *o = &obj->objs[best];
            length += plat_snprintf(text + length, sizeof(text) - length,
                                    "%s,object,%s,%.1f,,,%.3f,%.3f,%.3f,%.1f,%.1f,,,,,,,,,\n", prefix,
                                    prof_object_name(best), o->tris / objFrames, o->renderNs / 1e6 / objRenderTimed,
                                    o->buildNs / 1e6 / objTimed, o->logicNs / 1e6 / objTimed, o->updates / objFrames,
                                    o->drawn / objFrames);
        }
    }
    append_file(DRAWMIX_FILE, text,
                "run,race,track,character,vehicle,graphics,config,outcome,what,name,tris,draws,verts,rdp_ms,build_ms,"
                "logic_ms,updated,drawn,off_pct,back_pct,tiny_pct,overdraw,segments,segments_allowed,segments_drawn,"
                "objects,objects_drawn\n");

    // The run's totals.
    for (k = 0; k < PROF_KINDS; k++) {
        ProfKindStats *t = &sRunMix.kinds[k];
        const ProfKindStats *f = &mix.kinds[k];
        t->draws += f->draws;
        t->tris += f->tris;
        t->verts += f->verts;
        t->renderNs += f->renderNs;
        t->buildNs += f->buildNs;
        t->audited += f->audited;
        t->offscreen += f->offscreen;
        t->backface += f->backface;
        t->tiny += f->tiny;
        t->areaPx += f->areaPx;
    }
    sRunMix.frames += mix.frames;
    sRunMix.timedFrames += mix.timedFrames;
    sRunMix.gameTimedFrames += mix.gameTimedFrames;
    sRunMix.auditedFrames += mix.auditedFrames;
    sRunScene.frames += scene.frames;
    sRunScene.segments += scene.segments;
    sRunScene.segmentsAllowed += scene.segmentsAllowed;
    sRunScene.segmentsDrawn += scene.segmentsDrawn;
    sRunScene.objects += scene.objects;
    sRunScene.objectsDrawn += scene.objectsDrawn;
    for (k = 0; k < PROF_OBJ_BUCKETS; k++) {
        ProfObjStats *t = &sRunObj.objs[k];
        const ProfObjStats *f = &obj->objs[k];
        t->updates += f->updates;
        t->drawn += f->drawn;
        t->tris += f->tris;
        t->logicNs += f->logicNs;
        t->buildNs += f->buildNs;
        t->renderNs += f->renderNs;
    }
    sRunObj.frames += obj->frames;
    sRunObj.timedFrames += obj->timedFrames;
    sRunObj.renderTimedFrames += obj->renderTimedFrames;
}

/* The run's draw mix for its issue: per kind and the costliest object kinds, a frame. Returns the length written. */
static s32 draw_mix_summary(char *out, s32 size) {
    static char line[1024];
    s32 length;
    if (sRunMix.frames == 0) {
        return 0;
    }
    prof_format_draw_mix(&sRunMix, &sRunScene, line, sizeof(line));
    length = plat_snprintf(out, size,
                           "\n### Where the time goes\n\nThe races' frames on average (each race's are in `" DRAWMIX_FILE
                           "`). Per kind drawn: triangles/draws, renderer/game thread ms, wasted triangles "
                           "off screen/facing away/under a pixel, overdraw:\n\n%s\n\n",
                           line);
    if (length < size) {
        prof_format_object_mix(&sRunObj, 8, line, sizeof(line));
        length += plat_snprintf(out + length, size - length,
                                "Costliest object kinds (updated/drawn/triangles, logic/build/renderer ms):\n\n%s\n",
                                line);
    }
    return length;
}

/* The run's hitches for its issue: how many, by cause, and the worst. Returns the length written. */
static s32 hitch_summary(char *out, s32 size) {
    s32 length, c, i;
    length = plat_snprintf(out, size, "\n### Hitches\n\n%u since this launch's first race (all of them, race by race, "
                                      "are in `" HITCH_FILE "`):", sRunHitches);
    for (c = 0; c < REC_CAUSE_COUNT && length < size; c++) {
        length += plat_snprintf(out + length, size - length, "%s %s %u", c > 0 ? "," : "", rec_cause_name(c),
                                sRunCauses[c]);
    }
    if (length < size) {
        length += plat_snprintf(out + length, size - length, ".\n\n");
    }
    for (i = 0; i < sWorstHitchCount && length < size; i++) {
        length += plat_snprintf(out + length, size - length, "- **%u ms** %s\n", sWorstHitches[i].periodUs / 1000,
                                sWorstHitches[i].text);
    }
    return length < size ? length : size - 1;
}

/* Whether a stop ended as it should: a race finished, a battle arena played for its time, a hub toured. */
static s32 outcome_ok(const char *outcome) {
    return str_equal(outcome, "finished") || str_equal(outcome, "played") || str_equal(outcome, "toured");
}

/* Writes the results line of the race that just ended. stats may be NULL (the race never got going). */
static void record_result(const char *outcome, s32 position, const ProfStats *stats) {
    char header[640];
    char line[640];
    Race *race = &sRaces[sNext];
    ProfStats empty;
    u32 slow, i;
    s32 length;

    plat_snprintf(header, sizeof(header),
                  "run,race,track_id,track,character,vehicle,outcome,race_s,position,load_s,frames,fps,"
                  "pct_30fps,pct_20fps,pct_slower,worst_frame_ms,hitches,logic_ms,scene_ms,frame_wait_ms,"
                  "rdp_ms,texture_ms,gpu_wait_ms,gpu_draw_ms,audio_ms,worst_build_ms,worst_render_ms,"
                  "worst_gpu_ms,triangles,draws,texture_uploads,idle_pct,limit,unsticks,unstuck_at%s%s,"
                  "audio_gaps,audio_drops,sound,graphics,low1pct_fps,paced_60fps_pct,paced_30fps_pct,"
                  "paced_20fps_pct,paced_slower_pct\n",
                  sModes[sMode].extraColumn ? "," : "", sModes[sMode].extraColumn ? sModes[sMode].extraColumn : "");
    if (stats == NULL) {
        bzero(&empty, sizeof(empty));
        stats = &empty;
    }
    for (slow = 0, i = 4; i < PROF_RATE_BUCKETS; i++) {
        slow += stats->rateFrames[i];
    }
    plat_snprintf(
        line, sizeof(line),
        "%d,%d,%d,%s,%s,%s,%s,%.1f,%d,%.1f,%u,%.2f,%d,%d,%d,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%u,%u,%u,"
        "%u,%u,%d,%s,%d,%s",
        (int) sRun, (int) sNext + 1, (int) race->track, stop_name(race),
        sCharacterNames[(s32) race->character], sVehicleNames[(s32) race->vehicle], outcome, sRaceRetraces / 60.0,
        (int) position, sLoadNs / 1e9, stats->gameFrames,
        stats->elapsedNs ? stats->gameFrames * 1e9 / stats->elapsedNs : 0.0,
        (int) pct(stats->rateFrames[1] + stats->rateFrames[2], stats->gameFrames),
        (int) pct(stats->rateFrames[3], stats->gameFrames), (int) pct(slow, stats->gameFrames), stats->worstFrameUs / 1000, stats->hitches,
        ms(stats, PROF_LOGIC, stats->gameFrames), ms(stats, PROF_SCENE, stats->gameFrames),
        ms(stats, PROF_FRAME_WAIT, stats->gameFrames), ms(stats, PROF_RDP, stats->renderedFrames),
        ms(stats, PROF_TEXTURE, stats->renderedFrames), ms(stats, PROF_GPU_WAIT, stats->renderedFrames),
        ms(stats, PROF_GPU_DRAW, stats->renderedFrames), ms(stats, PROF_AUDIO, stats->gameFrames),
        stats->worstBuildUs / 1000, stats->worstRenderUs / 1000, stats->worstGpuUs / 1000,
        stats->renderedFrames ? stats->counters[PROF_TRIANGLES] / stats->renderedFrames : 0,
        stats->renderedFrames ? stats->counters[PROF_DRAWS] / stats->renderedFrames : 0,
        stats->counters[PROF_TEXTURE_UPLOADS], (int) pct((u32) (stats->idleNs / 1000), (u32) (stats->elapsedNs / 1000)),
        stats->gameFrames ? prof_limit(stats) : "-", (int) sUnsticks, sUnstuckAt);
    length = strlen(line);
    if (is_bench()) {
        // The settings the race was meant to have (the log says what the renderer made of them), and the frames at 60.
        plat_snprintf(line + length, sizeof(line) - length, ",%d,%d", (int) race->config,
                      (int) pct(stats->rateFrames[1], stats->gameFrames));
    } else if (sMode == MODE_QUICK || sMode == MODE_BOSS) {
        plat_snprintf(line + length, sizeof(line) - length, ",%s", plat_build_version());
    } else if (sMode == MODE_WHOLE) {
        plat_snprintf(line + length, sizeof(line) - length, ",%s,%s,%s", sStopKindNames[(s32) race->kind],
                      sPartNames[(s32) race->kind], plat_build_version());
    }
    length = strlen(line);
    // After the mode's own column, so the columns before keep their places in older results files.
    // Then the frame pacing: the 1% low, and the share of game frames that took one retrace, two, three or more (measured
    // between frames, so unlike pct_30fps it counts the frames the game meant to show at 60 but couldn't).
    plat_snprintf(line + length, sizeof(line) - length, ",%u,%u,%s,%s,%.1f,%d,%d,%d,%d\n",
                  stats == &empty ? 0 : (unsigned) (gPlatCounters[PLAT_COUNTER_AUDIO_UNDERRUN] - sAudioGapsStart),
                  stats == &empty ? 0 : (unsigned) (gPlatCounters[PLAT_COUNTER_AUDIO_DROPPED] - sAudioDropsStart),
                  plat_audio_setup(), graphics_setup(), stats->low1FrameUs ? 1e6 / stats->low1FrameUs : 0.0,
                  (int) pct(stats->pacedFrames[0], stats->gameFrames), (int) pct(stats->pacedFrames[1], stats->gameFrames),
                  (int) pct(stats->pacedFrames[2], stats->gameFrames), (int) pct(stats->pacedFrames[3], stats->gameFrames));
    append_file(results_file(), line, header);
    port_log("autorun: %s", line);
    if (outcome_ok(outcome)) {
        sFinished++;
    } else {
        sProblems++;
    }
}

/*
 * Whether the test run is on: DKR_AUTORUN is set, or the bottom screen's button started a run that hasn't ended
 * (autorun_state.txt says active=1, so it survives a crash). Safe to call at any time, before the game is up.
 */
s32 port_autorun_enabled(void) {
    if (sEnabled < 0) {
        const char *value = plat_getenv("DKR_AUTORUN");
        char path[256];
        char text[200];
        s32 length;

        s32 mode;

        // A short run first (asked for, or one that hasn't ended), then the full run.
        sMode = MODE_FULL;
        for (mode = MODE_FULL + 1; mode < MODE_COUNT && sMode == MODE_FULL; mode++) {
            const char *env = plat_getenv(sModes[mode].env);
            if (env != NULL && env[0] != '0') {
                sMode = mode;
                sShort = (mode == MODE_WHOLE || (mode == MODE_SWITCH && !plat_old_3ds())) && env[0] == '2';
            }
        }
        for (mode = MODE_FULL + 1; mode < MODE_COUNT && sMode == MODE_FULL; mode++) {
            make_path(path, sizeof(path), sModes[mode].stateFile);
            length = plat_load_file_start(path, text, sizeof(text) - 1);
            if (length > 0) {
                text[length] = '\0';
                if (state_value(text, "active", 0) != 0) {
                    sMode = mode;
                    sShort = state_value(text, "short", 0);
                }
            }
        }
        sEnabled = sMode != MODE_FULL || (value != NULL && value[0] != '0');
        make_path(path, sizeof(path), STATE_FILE);
        length = plat_load_file_start(path, text, sizeof(text) - 1);
        if (!sEnabled && length > 0) {
            text[length] = '\0';
            sEnabled = state_value(text, "active", 0) != 0;
        }
    }
    return sEnabled;
}

static void init(void) {
    char path[256];
    unsigned int size;
    char *state, *hang;
    const char *names[] = { "DKR_AUTORUN_TRACKS", "DKR_AUTORUN_CHARACTERS", "DKR_AUTORUN_VEHICLES" };
    const char *benchNames[] = { "DKR_AUTORUN_BENCH_TRACKS", "DKR_AUTORUN_BENCH_CHARACTER",
                                 "DKR_AUTORUN_BENCH_CONFIGS" };
    const char *quickNames[] = { "DKR_AUTORUN_QUICK_TRACKS", "DKR_AUTORUN_QUICK_CHARACTER",
                                 "DKR_AUTORUN_QUICK_PASSES" };
    const char *bossNames[] = { "DKR_AUTORUN_BOSS_TRACKS", "DKR_AUTORUN_BOSS_CHARACTER", "DKR_AUTORUN_BOSS_PASSES" };
    const char *switchNames[] = { "DKR_AUTORUN_SWITCH_TRACKS", "DKR_AUTORUN_SWITCH_CHARACTER",
                                  "DKR_AUTORUN_SWITCH_CONFIGS" };
    s32 i;

    sInitialised = TRUE;
    sConfigHash = 0;
    if (sMode == MODE_BENCH) {
        hash_text("gpu bench");
        for (i = 0; i < (s32) ARRAY_COUNT(benchNames); i++) {
            hash_text(plat_getenv(benchNames[i]));
        }
    } else if (sMode == MODE_SWITCH) {
        hash_text(sShort ? "shelved switches" : "switch bench");
        for (i = 0; i < (s32) ARRAY_COUNT(switchNames); i++) {
            hash_text(plat_getenv(switchNames[i]));
        }
        for (i = 1; i < SWITCH_RACES; i++) {
            hash_text(config_name(i)); // A new switch starts a new run.
        }
    } else if (sMode == MODE_QUICK) {
        hash_text("quick test");
        for (i = 0; i < (s32) ARRAY_COUNT(quickNames); i++) {
            hash_text(plat_getenv(quickNames[i]));
        }
    } else if (sMode == MODE_BOSS) {
        hash_text("boss fights");
        for (i = 0; i < (s32) ARRAY_COUNT(bossNames); i++) {
            hash_text(plat_getenv(bossNames[i]));
        }
    } else if (sMode == MODE_WHOLE) {
        hash_text(sShort ? "whole game short" : "whole game");
        hash_text(plat_getenv("DKR_AUTORUN_WHOLE_PARTS"));
        hash_text(plat_getenv("DKR_AUTORUN_WHOLE_LEVELS"));
    } else {
        for (i = 0; i < (s32) ARRAY_COUNT(names); i++) {
            hash_text(plat_getenv(names[i]));
        }
    }
    build_race_list();
    sTimeoutSeconds = plat_getenv("DKR_AUTORUN_TIMEOUT") ? port_atoi(plat_getenv("DKR_AUTORUN_TIMEOUT")) : 300;
    sRun = 1;
    sNext = 0;
    sFinished = 0;
    sProblems = 0;
    sFpsSum = 0.0;
    sFpsCount = 0;
    sReportedAt = -1;
    sReportWaitNs = 0;
    sLastReportNs = plat_time_ns();

    io_flush(); // A run that just ended may still be writing its state.
    make_path(path, sizeof(path), state_file());
    state = plat_load_file(path, &size);
    if (state != NULL && plat_getenv("DKR_AUTORUN_RESTART") == NULL) {
        char *text = plat_malloc(size + 1);
        s32 running;
        bcopy(state, text, size);
        text[size] = '\0';
        running = state_value(text, "running", -1);
        if ((u32) state_value(text, "config", 0) == sConfigHash) {
            sRun = state_value(text, "run", 1);
            sNext = state_value(text, "next", 0);
            sFinished = state_value(text, "finished", 0);
            sProblems = state_value(text, "problems", 0);
            sReportedAt = state_value(text, "reported", -1);
            if (running >= 0 && running < sNumRaces && state_value(text, "recorded", 0)) {
                // The last launch stopped after the race's result was written (the results screen): carry on after it.
                make_path(path, sizeof(path), HANG_FILE);
                plat_save_file(path, "", 0);
                sNext = running + 1;
                if (sNext >= sNumRaces) {
                    plat_free(text);
                    plat_free(state);
                    turn_off("finished");
                    return;
                }
            } else if (running >= 0 && running < sNumRaces) {
                // The last launch never got to the end of this race: it crashed, hung or was closed.
                const char *outcome = "crash_or_closed";
                sNext = running;
                make_path(path, sizeof(path), HANG_FILE);
                hang = plat_load_file(path, &size);
                if (hang != NULL) {
                    char what[200];
                    outcome = "hang";
                    plat_snprintf(what, sizeof(what), "hang: %.*s", (int) (size < 150 ? size : 150), hang);
                    for (i = 0; what[i]; i++) {
                        if (what[i] == '\n' || what[i] == '\r') {
                            what[i] = ' ';
                        }
                    }
                    record_problem(what);
                    plat_free(hang);
                    plat_save_file(path, "", 0);
                } else {
                    record_problem("the game crashed or was closed during this race");
                }
                record_result(outcome, 0, NULL);
                sNext = running + 1;
                if (sNext >= sNumRaces) {
                    // That was the last race: the run is over.
                    plat_free(text);
                    plat_free(state);
                    turn_off("finished");
                    return;
                }
            }
            if (sNext >= sNumRaces) {
                // The previous run had finished: start a new one.
                sNext = 0;
                sRun++;
                sFinished = 0;
                sProblems = 0;
            }
        } else {
            sRun = state_value(text, "run", 0) + 1;
        }
        plat_free(text);
    }
    plat_free(state);
    port_log("autorun: run %d, %d races, starting at %d", (int) sRun, (int) sNumRaces, (int) sNext + 1);
    reset_hitches();
    reset_draw_mix();
    if (sNext == 0) {
        io_queue(HITCH_FILE, "", NULL, FALSE); // A new run: the last one's hitches went with its issue.
        io_queue(DRAWMIX_FILE, "", NULL, FALSE);
        io_queue(RUN_ISSUE_FILE, "", NULL, FALSE); // Its progress reports make a new issue.
    }
    set_phase(PHASE_TITLE);
    if (sNumRaces == 0) {
        sPhase = PHASE_OFF;
        sEnabled = 0;
    }
    save_state(-1);
}

/* Nonzero while a test run is going (or about to start). */
s32 port_autorun_active(void) {
    if (!port_autorun_enabled()) {
        return FALSE;
    }
    if (!sInitialised) {
        init();
    }
    return sEnabled && sPhase != PHASE_OFF;
}

/* The switch bench's switches: 0 when it has nothing to test (the Test run menu says so). */
s32 port_autorun_switch_count(void) {
    return SWITCH_CONFIGS - 1;
}

/* The shelved switches bench's switches (a New 3DS only; 0 on an Old 3DS, where the button has nothing to run). */
s32 port_autorun_shelved_count(void) {
    return plat_old_3ds() ? 0 : (s32) ARRAY_COUNT(sSwitchesShelved) - 1;
}

/*
 * From any thread (the bottom screen's buttons): start the test run (1), the GPU benchmark (2), the quick test (3), the
 * boss fights (4), the switch bench (5), the whole game run (6, or 7 for its short variant) or the shelved switches
 * bench (8), or stop whichever is going (0), at the next game frame.
 */
void port_autorun_request(s32 start) {
    sRequest = start == 8   ? REQUEST_SWITCH_SHELVED
               : start == 7 ? REQUEST_WHOLE_SHORT
               : start == 6 ? REQUEST_WHOLE
               : start == 5 ? REQUEST_SWITCH
               : start == 4 ? REQUEST_BOSS
               : start == 3 ? REQUEST_QUICK
               : start == 2 ? REQUEST_BENCH
               : start      ? REQUEST_START
                            : REQUEST_STOP;
}

static void set_phase(s32 phase) {
    sPhase = phase;
    sPhaseStartNs = plat_time_ns();
}

/*
 * The whole game run's adventure levels need progress: the hub doors open, the bosses beaten (a race has its silver
 * coins once its world's boss is beaten and the race won, objects.c), the story's cutscenes seen. It is given in memory
 * only (a test run never writes the save files) and put back before any other stop and when the run ends.
 */
#define PROGRESS_LEVELS 128
#define PROGRESS_WORLDS 16
static s32 sProgressSaved;
static Settings sSavedSettings;
static s16 sSavedBalloons[PROGRESS_WORLDS];
static s32 sSavedCourseFlags[PROGRESS_LEVELS];

static void give_progress(void) {
    Settings *settings = get_settings();
    s32 numLevels, numWorlds, i;
    level_count(&numLevels, &numWorlds);
    numLevels = numLevels < PROGRESS_LEVELS ? numLevels : PROGRESS_LEVELS;
    numWorlds = numWorlds < PROGRESS_WORLDS ? numWorlds : PROGRESS_WORLDS;
    if (!sProgressSaved) {
        sSavedSettings = *settings;
        bcopy(settings->balloonsPtr, sSavedBalloons, numWorlds * sizeof(s16));
        bcopy(settings->courseFlagsPtr, sSavedCourseFlags, numLevels * sizeof(s32));
        sProgressSaved = TRUE;
    }
    settings->balloonsPtr[0] = 47;
    for (i = 1; i < numWorlds; i++) {
        settings->balloonsPtr[i] = 8;
    }
    for (i = 0; i < numLevels; i++) {
        // Visited and won, every hub door open, the silver coins still to collect.
        settings->courseFlagsPtr[i] = (settings->courseFlagsPtr[i] | 0xFFFF0003) & ~RACE_CLEARED_SILVER_COINS;
    }
    settings->keys = 0x1E;
    settings->bosses = 0x07BF;
    settings->trophies = 0x03FF;
    settings->cutsceneFlags |= 0x3FFFB; // Every cutscene seen but 0x4 (Adventure Two).
    settings->tajFlags = 0x3F;
    settings->ttAmulet = 4;
    settings->wizpigAmulet = 4;
    settings->newGame = FALSE;
}

static void put_back_progress(void) {
    Settings *settings = get_settings();
    s32 numLevels, numWorlds;
    if (!sProgressSaved) {
        return;
    }
    level_count(&numLevels, &numWorlds);
    numLevels = numLevels < PROGRESS_LEVELS ? numLevels : PROGRESS_LEVELS;
    numWorlds = numWorlds < PROGRESS_WORLDS ? numWorlds : PROGRESS_WORLDS;
    bcopy(sSavedBalloons, sSavedSettings.balloonsPtr, numWorlds * sizeof(s16));
    bcopy(sSavedCourseFlags, sSavedSettings.courseFlagsPtr, numLevels * sizeof(s32));
    *settings = sSavedSettings;
    sProgressSaved = FALSE;
}

/*
 * The hub tour: no AI course runs through a hub, so the kart is put in each segment of the level's model in turn (on
 * the vertex nearest the middle of the segment's bounds, where the floor usually is, and left to fall onto it) and
 * turned once all the way round there, so the camera behind it looks every way. The kart doesn't drive meanwhile.
 */
extern LevelModel *gCurrentLevelModel;
static f32 sTourX, sTourZ;
static s32 sTourYaw;

static void place_kart(Object *obj, Object_Racer *racer, f32 x, f32 y, f32 z, s32 yaw) {
    Camera *camera;
    s32 i;
    obj->trans.x_position = x;
    obj->trans.y_position = y;
    obj->trans.z_position = z;
    obj->segmentID = get_level_segment_index_from_position(x, y, z);
    racer->prev_x_position = x;
    racer->prev_y_position = y;
    racer->prev_z_position = z;
    racer->steerVisualRotation = yaw;
    obj->trans.rotation.y_rotation = yaw;
    obj->trans.rotation.x_rotation = 0;
    obj->trans.rotation.z_rotation = 0;
    for (i = 0; i < 4; i++) {
        racer->unkD8[i * 3 + 0] = x;
        racer->unkD8[i * 3 + 1] = y;
        racer->unkD8[i * 3 + 2] = z;
    }
    if (obj->interactObj != NULL) {
        obj->interactObj->x_position = x;
        obj->interactObj->y_position = y;
        obj->interactObj->z_position = z;
    }
    racer->velocity = 0.0f;
    racer->lateral_velocity = 0.0f;
    racer->y_rotation_vel = 0;
    racer->x_rotation_vel = 0;
    racer->z_rotation_vel = 0;
    obj->x_velocity = 0.0f;
    obj->y_velocity = 0.0f;
    obj->z_velocity = 0.0f;
    set_active_camera(racer->playerIndex);
    camera = cam_get_active_camera_no_cutscenes();
    camera->trans.x_position = x;
    camera->trans.y_position = y;
    camera->trans.z_position = z;
}

/* Moves the tour on to the next segment with something in it. Returns FALSE when every segment has been seen. */
static s32 tour_next_segment(Object *obj, Object_Racer *racer) {
    LevelModel *model = gCurrentLevelModel;
    while (model != NULL && ++sTourSegment < model->numberOfSegments) {
        LevelModelSegment *segment = &model->segments[sTourSegment];
        LevelModelSegmentBoundingBox *box = &model->segmentsBoundingBoxes[sTourSegment];
        f32 midX = (box->x1 + box->x2) * 0.5f, midZ = (box->z1 + box->z2) * 0.5f;
        f32 best = 1e30f;
        s32 i, pick = -1;
        for (i = 0; i < segment->numberOfVertices; i++) {
            f32 dx = segment->vertices[i].x - midX, dz = segment->vertices[i].z - midZ;
            if (dx * dx + dz * dz < best) {
                best = dx * dx + dz * dz;
                pick = i;
            }
        }
        if (pick < 0) {
            continue;
        }
        sTourX = segment->vertices[pick].x;
        sTourZ = segment->vertices[pick].z;
        sTourYaw = obj->trans.rotation.y_rotation;
        place_kart(obj, racer, sTourX, segment->vertices[pick].y + 20.0f, sTourZ, sTourYaw);
        sTourSegments++;
        return TRUE;
    }
    return FALSE;
}

/* Returns the outcome when the tour is over, else NULL. */
static const char *hub_tour_frame(Object *obj, Object_Racer *racer, s32 updateRate) {
    s32 stopRetraces = (sShort ? TOUR_SECONDS / 2 : TOUR_SECONDS) * 60;
    s32 yaw;
    if (!sDriving) {
        sDriving = TRUE;
        sRaceLevel = level_id();
        sTourSegment = -1;
        sTourSegments = 0;
        sTourRetraces = stopRetraces;
    }
    sTourRetraces += updateRate;
    if (sTourRetraces >= stopRetraces) {
        sTourRetraces = 0;
        if (!tour_next_segment(obj, racer)) {
            return "toured";
        }
    }
    // Held where it was put (it may still fall), turning once round in the time.
    yaw = sTourYaw + (s32) ((sTourRetraces * 0x10000LL) / stopRetraces);
    obj->trans.x_position = sTourX;
    obj->trans.z_position = sTourZ;
    obj->x_velocity = 0.0f;
    obj->z_velocity = 0.0f;
    racer->velocity = 0.0f;
    racer->lateral_velocity = 0.0f;
    racer->steerVisualRotation = (s16) yaw;
    obj->trans.rotation.y_rotation = (s16) yaw;
    return NULL;
}

/*
 * The menu pass: the buttons a player would press through the title screen, the options and their pages, character
 * select (every character), the adventure's file select and the tracks menu (every world and track), pressed when the
 * menu they are for is up, then B until the title screen is back. The menus' input comes from here meanwhile
 * (port_autorun_input). A step whose menu doesn't come up within MENU_STEP_TIMEOUT is skipped (and noted).
 */
enum { MENU_KEY_NONE, MENU_KEY_A, MENU_KEY_B, MENU_KEY_START, MENU_KEY_UP, MENU_KEY_DOWN, MENU_KEY_LEFT,
       MENU_KEY_RIGHT, MENU_KEY_REVEAL /* A, only while the title screen's intro scenes play (it shows the menu) */ };

typedef struct MenuStep {
    s8 menu;   // MENU_* the step is for
    s8 key;    // MENU_KEY_*
    u8 count;  // Presses
    u8 gap;    // Tenths of a second after each press
} MenuStep;

static const MenuStep sMenuSteps[] = {
    { MENU_TITLE, MENU_KEY_NONE, 1, 50 },     // The title screen's intro scenes, a moment
    { MENU_TITLE, MENU_KEY_REVEAL, 1, 40 },   // Then its menu
    { MENU_TITLE, MENU_KEY_DOWN, 1, 6 },      // OPTIONS
    { MENU_TITLE, MENU_KEY_A, 1, 25 },
    { MENU_OPTIONS, MENU_KEY_DOWN, 8, 6 },    // Down every row
    { MENU_OPTIONS, MENU_KEY_UP, 8, 4 },
    { MENU_OPTIONS, MENU_KEY_B, 1, 25 },      // Back to the title screen
    { MENU_TITLE, MENU_KEY_UP, 1, 6 },        // START
    { MENU_TITLE, MENU_KEY_A, 1, 30 },
    { MENU_CHARACTER_SELECT, MENU_KEY_RIGHT, 9, 12 }, // Every character
    { MENU_CHARACTER_SELECT, MENU_KEY_A, 1, 15 },
    { MENU_CHARACTER_SELECT, MENU_KEY_A, 1, 30 },     // OK
    { MENU_GAME_SELECT, MENU_KEY_A, 1, 30 },          // Adventure
    { MENU_FILE_SELECT, MENU_KEY_RIGHT, 2, 10 },      // The three files
    { MENU_FILE_SELECT, MENU_KEY_B, 1, 30 },
    { MENU_GAME_SELECT, MENU_KEY_DOWN, 1, 6 },        // Tracks
    { MENU_GAME_SELECT, MENU_KEY_A, 1, 40 },
    { MENU_TRACK_SELECT, MENU_KEY_DOWN, 3, 20 },      // The tracks of the first world
    { MENU_TRACK_SELECT, MENU_KEY_RIGHT, 4, 25 },     // The other worlds
    { MENU_TRACK_SELECT, MENU_KEY_UP, 3, 20 },
    { MENU_TRACK_SELECT, MENU_KEY_LEFT, 4, 15 },
};
#define MENU_STEPS ((s32) ARRAY_COUNT(sMenuSteps))
#define MENU_HOLD_RETRACES 5

extern s32 gCurrentMenuId;
extern s32 gTitleRevealTimer;
static s32 sMenuStep, sMenuPresses, sMenuTimer, sMenuHold, sMenuWait;
static volatile u16 sMenuButtons;
static volatile s8 sMenuStickX, sMenuStickY;

static void menu_press(s32 key) {
    static const u16 sButtons[] = { 0, 0x8000, 0x4000, 0x1000, 0, 0, 0, 0, 0x8000 };
    sMenuButtons = sButtons[key];
    sMenuStickX = key == MENU_KEY_LEFT ? -80 : key == MENU_KEY_RIGHT ? 80 : 0;
    sMenuStickY = key == MENU_KEY_DOWN ? -80 : key == MENU_KEY_UP ? 80 : 0;
    sMenuHold = MENU_HOLD_RETRACES;
}

static void menu_release(void) {
    sMenuButtons = 0;
    sMenuStickX = 0;
    sMenuStickY = 0;
}

static void start_menu_pass(void) {
    sMenuStep = 0;
    sMenuPresses = 0;
    sMenuTimer = 0;
    sMenuHold = 0;
    sMenuWait = 0;
    menu_release();
    sDriving = TRUE;
    sRaceLevel = level_id();
    prof_window_start(PROF_WINDOW_SESSION);
    prof_draw_mix_window(NULL, NULL, TRUE);
    sAudioGapsStart = gPlatCounters[PLAT_COUNTER_AUDIO_UNDERRUN];
    sAudioDropsStart = gPlatCounters[PLAT_COUNTER_AUDIO_DROPPED];
    set_phase(PHASE_RACING);
}

/* Returns the outcome when the pass is over, else NULL. */
static const char *menu_pass_frame(s32 updateRate) {
    const MenuStep *step;
    if (sMenuHold > 0) {
        sMenuHold -= updateRate;
        if (sMenuHold <= 0) {
            menu_release();
        }
    }
    if (sMenuTimer > 0) {
        sMenuTimer -= updateRate;
        return NULL;
    }
    if (sMenuStep >= MENU_STEPS || sRaceRetraces >= MENU_PASS_SECONDS * 60) {
        // Back to the title screen: B once a second until it is up.
        if (gCurrentMenuId == MENU_TITLE && get_game_mode() == GAMEMODE_MENU) {
            return sRaceRetraces >= MENU_PASS_SECONDS * 60 ? "menus_timeout" : "finished";
        }
        if (sRaceRetraces >= (MENU_PASS_SECONDS + 60) * 60) {
            return "menus_stuck";
        }
        menu_press(MENU_KEY_B);
        sMenuTimer = 60;
        return NULL;
    }
    step = &sMenuSteps[sMenuStep];
    if (gCurrentMenuId != step->menu || get_game_mode() != GAMEMODE_MENU) {
        sMenuWait += updateRate;
        if (sMenuWait >= MENU_STEP_TIMEOUT * 60) {
            char what[96];
            plat_snprintf(what, sizeof(what), "note: menu pass step %d waited for menu %d, menu %d is up",
                          (int) sMenuStep + 1, (int) step->menu, (int) gCurrentMenuId);
            record_problem(what);
            sMenuStep++;
            sMenuPresses = 0;
            sMenuWait = 0;
        }
        return NULL;
    }
    sMenuWait = 0;
    if (step->key != MENU_KEY_REVEAL || gTitleRevealTimer == 0) {
        menu_press(step->key);
    }
    sMenuTimer = step->gap * 6;
    if (++sMenuPresses >= step->count) {
        sMenuStep++;
        sMenuPresses = 0;
    }
    return NULL;
}

/* plat_input_script: the menu pass's buttons replace the player's while it runs. */
int port_autorun_input(unsigned short *buttons, signed char *stickX, signed char *stickY) {
    if (sEnabled <= 0 || sMode != MODE_WHOLE || sPhase != PHASE_RACING || sNext >= sNumRaces ||
        sRaces[sNext].kind != STOP_MENUS) {
        return 0;
    }
    *buttons = sMenuButtons;
    *stickX = sMenuStickX;
    *stickY = sMenuStickY;
    return 1;
}

/* An instrumented build (profile-guided compiling) saves its counters here; nothing otherwise. */
__attribute__((weak)) void port_autorun_section_done(const char *section) {
    (void) section;
}

static s32 whole_ready(void);

static void show_status(void) {
    char text[200];
    char line[3][48];
    char bar[160];
    Race *race = &sRaces[sNext < sNumRaces ? sNext : sNumRaces - 1];
    Object *obj = get_racer_object_by_port(PLAYER_ONE);
    LevelHeader *header = level_header();
    const char *what;
    char lap[24];
    s32 i, filled, length;

    if (sNumRaces == 0) {
        return;
    }
    lap[0] = '\0';
    switch (sPhase) {
        case PHASE_LOADING:
            what = "loading";
            break;
        case PHASE_COUNTDOWN:
            what = "countdown";
            break;
        case PHASE_RACING:
            what = lap;
            if (race->kind == STOP_MENUS) {
                plat_snprintf(lap, sizeof(lap), "step %d/%d", (int) sMenuStep + 1, (int) MENU_STEPS);
            } else if (race->kind == STOP_HUB) {
                plat_snprintf(lap, sizeof(lap), "segment %d", (int) sTourSegment + 1);
            } else if (obj != NULL && header != NULL) {
                plat_snprintf(lap, sizeof(lap), "lap %d/%d %d:%02d", obj->racer->lap + 1, header->laps,
                              (int) (sRaceRetraces / 3600), (int) (sRaceRetraces / 60 % 60));
            }
            break;
        case PHASE_FINISHED:
            what = sOutcome;
            break;
        default:
            what = (plat_time_ns() - sPhaseStartNs > 3000000000ULL) ? "(go to the title screen)" : "next";
            break;
    }
    plat_snprintf(line[0], sizeof(line[0]), "%s %d  race %d/%d  ok %d bad %d",
                  sMode == MODE_BENCH    ? "GPU BENCH"
                  : sMode == MODE_SWITCH ? (sShort ? "SHELVED SWITCHES" : "SWITCH BENCH")
                  : sMode == MODE_QUICK  ? "QUICK TEST"
                  : sMode == MODE_BOSS  ? "BOSS FIGHTS"
                  : sMode == MODE_WHOLE ? (sShort ? "WHOLE GAME (SHORT)" : "WHOLE GAME")
                                        : "TEST RUN",
                  (int) sRun,
                  (int) sNext + 1, (int) sNumRaces, (int) sFinished, (int) sProblems);
    if (is_bench()) {
        plat_snprintf(line[1], sizeof(line[1]), "%s, %s %s", stop_name(race), config_name(race->config),
                      what);
    } else {
        plat_snprintf(line[1], sizeof(line[1]), "%s %s %s %s", sCharacterNames[(s32) race->character],
                      stop_name(race), sVehicleNames[(s32) race->vehicle], what);
    }
    // The progress bar: green for the races done, grey for the rest, with the percentage after it.
    filled = sNext * 32 / sNumRaces;
    length = plat_snprintf(bar, sizeof(bar), "\x1b[42m");
    for (i = 0; i < 32; i++) {
        if (i == filled) {
            length += plat_snprintf(bar + length, sizeof(bar) - length, "\x1b[47m");
        }
        bar[length++] = ' ';
    }
    bar[length] = '\0';
    plat_snprintf(line[2], sizeof(line[2]), " %3d%%", (int) (sNext * 100 / sNumRaces));
    line[0][39] = line[1][39] = '\0';
    plat_snprintf(text, sizeof(text), "%s\n%s\n%s\x1b[0m%s", line[0], line[1], bar, line[2]);
    prof_set_status(text);
}

/*
 * Title screen: returns the menu result that starts the next race (as the tracks menu would), or 0. Waits a moment
 * first, so the title screen's own loading is over.
 */
s32 port_autorun_title(void) {
    Race *race;
    if (!port_autorun_active()) {
        return 0;
    }
    if (sPhase == PHASE_LEAVING) {
        set_phase(PHASE_TITLE);
    }
    if (sPhase != PHASE_TITLE) {
        return 0;
    }
    if (++sTitleFrames < 30) {
        return 0;
    }
    if (sMode == MODE_WHOLE && !whole_ready()) {
        sTitleFrames = 30; // A progress report is going out: no race until it is.
        return 0;
    }
    sTitleFrames = 0;
    race = &sRaces[sNext];
    port_log("autorun: race %d/%d: %s, %s, %s", (int) sNext + 1, (int) sNumRaces, stop_name(race),
             sCharacterNames[(s32) race->character], sVehicleNames[(s32) race->vehicle]);
    save_state(sNext);
    if (race->config >= 0) {
        apply_config(race->config); // The renderer switches while the race loads.
    }
    set_phase(PHASE_LOADING);
    sRaceRetraces = 0;
    sStuckRetraces = 0;
    sBestProgress = -0x10000;
    sDriving = FALSE;
    sUnsticks = 0;
    sUnstuckAt[0] = '\0';
    sLingerRetraces = 0;
    show_status();
    if (race->kind == STOP_MENUS) {
        give_progress(); // So the tracks menu shows every track's preview.
        start_menu_pass();
        return 0; // The title screen stays; the menu pass presses its buttons.
    }
    if (race->kind == STOP_COINS || race->kind == STOP_ARENA || race->kind == STOP_HUB) {
        give_progress();
        return port_autorun_setup_adventure(race->track, race->character, race->vehicle);
    }
    put_back_progress();
    return port_autorun_setup_race(race->track, race->character, race->vehicle);
}

/* DKR_AUTORUN_TIME_TRIAL: the races are time trials (src/menu.c, port_autorun_setup_race). */
s32 port_autorun_time_trial(void) {
    const char *env = plat_getenv("DKR_AUTORUN_TIME_TRIAL");
    return env != NULL && port_atoi(env) != 0;
}

/* Nonzero while the player's kart is driven by the AI. */
s32 port_autorun_drives(void) {
    // Not in the menus, nor on a hub tour, where the kart is put in each place and turned round instead.
    return sEnabled > 0 && sPhase >= PHASE_LOADING && sPhase <= PHASE_FINISHED &&
           (sNext >= sNumRaces || (sRaces[sNext].kind != STOP_HUB && sRaces[sNext].kind != STOP_MENUS));
}

/* A plain decimal number ("27.86"), as the results file has them. */
static double parse_decimal(const char *text) {
    double value = 0.0, scale = 1.0;
    s32 negative = *text == '-';
    if (negative) {
        text++;
    }
    for (; *text >= '0' && *text <= '9'; text++) {
        value = value * 10 + (*text - '0');
    }
    if (*text == '.') {
        for (text++; *text >= '0' && *text <= '9'; text++) {
            scale /= 10;
            value += (*text - '0') * scale;
        }
    }
    return negative ? -value : value;
}

/* Columns of the short runs' results files that their summaries use. */
enum BenchColumn {
    COL_RUN = 0,
    COL_TRACK = 3,
    COL_CHARACTER = 4,
    COL_OUTCOME = 6,
    COL_RACE_S = 7,
    COL_POSITION = 8,
    COL_FPS = 11,
    COL_PCT_30FPS = 12,
    COL_WORST_FRAME = 15, // ms
    COL_RDP = 20,
    COL_GPU_DRAW = 23,
    COL_AUDIO = 24,
    COL_WORST_GPU = 27,
    COL_LIMIT = 32,
    COL_UNSTICKS = 33,
    COL_EXTRA = 35, // After the test run's unsticks and unstuck_at: "config" (benchmark) or "build" (quick test)
    COL_COUNT,
    COL_AUDIO_GAPS = COL_COUNT, // Only in results written since the sound columns were added (the benchmark: one on).
    COL_PCT_60FPS = COL_COUNT,  // The benchmark's second column of its own.
};
/* The most columns a line has: the benchmark's two, the sound's three, the graphics settings and the frame pacing's
 * four. */
#define MAX_FIELDS (COL_COUNT + 12) // The whole game run has three columns of its own

#define BENCH_MAX_TRACKS 8
#define BENCH_LIMITS 3

typedef struct BenchGroup {
    char track[32];
    char build[40]; // Quick test: the run and its build.
    s32 races;
    double fps, pct30, pct60, rdp, gpu, audio, gaps;
    s32 worstFrame, worstGpu;
    s32 pacedRaces; // Races with the frame pacing columns (results written before they were added have none).
    double low1, paced[4];
    s32 limits[BENCH_LIMITS]; // GPU, renderer CPU, game CPU
} BenchGroup;

static const char *const sLimitNames[BENCH_LIMITS] = { "GPU", "renderer CPU", "game CPU" };

static double bench_avg(double sum, s32 count) {
    return count ? sum / count : 0.0;
}

/* A group's frame pacing cells: the 1% low and the share of frames that took one retrace, two, three or more. */
static const char *pacing_cells(const BenchGroup *g) {
    static char text[48];
    if (g->pacedRaces == 0) {
        return "- | -";
    }
    plat_snprintf(text, sizeof(text), "%.1f | %.0f/%.0f/%.0f/%.0f", bench_avg(g->low1, g->pacedRaces),
                  bench_avg(g->paced[0], g->pacedRaces), bench_avg(g->paced[1], g->pacedRaces),
                  bench_avg(g->paced[2], g->pacedRaces), bench_avg(g->paced[3], g->pacedRaces));
    return text;
}

/* Splits a results line into fields in place. Returns the number of fields. */
static s32 split_fields(char *line, const char **fields) {
    s32 numFields = 0;
    char *c;
    fields[numFields++] = line;
    for (c = line; *c && numFields < MAX_FIELDS; c++) {
        if (*c == ',') {
            *c = '\0';
            fields[numFields++] = c + 1;
        }
    }
    return numFields;
}

/*
 * A short run's summary, from the finished races in its results file (so races from before a crash count too), as
 * Markdown. The benchmark: one line per track and render setting, with the GPU time it saves. The quick
 * test: one line per track for the quick test before this one and one for this one, then the difference.
 */
static void bench_summary(char *out, s32 size) {
    static BenchGroup groups[BENCH_MAX_TRACKS][MAX_CONFIGS];
    char path[256];
    char *data, *copy, *line, *next;
    const char *fields[MAX_FIELDS];
    unsigned int dataSize;
    s32 numTracks = 0, length, t, k, i, run, previousRun = 0;
    s32 quick = sMode == MODE_QUICK;

    bzero(groups, sizeof(groups));
    make_path(path, sizeof(path), results_file());
    data = plat_load_file(path, &dataSize);
    if (data == NULL) {
        return;
    }
    // Made into one string, then split into lines and fields in place (twice for the quick test: first to find the
    // run before this one).
    line = plat_malloc(dataSize + 1);
    copy = plat_malloc(dataSize + 1);
    if (line == NULL || copy == NULL) {
        plat_free(line);
        plat_free(copy);
        plat_free(data);
        return;
    }
    bcopy(data, line, dataSize);
    line[dataSize] = '\0';
    bcopy(data, copy, dataSize);
    copy[dataSize] = '\0';
    plat_free(data);
    data = line;
    if (quick) {
        for (line = copy; line != NULL && *line; line = next) {
            next = strchr(line, '\n');
            if (next != NULL) {
                *next++ = '\0';
            }
            if (split_fields(line, fields) < COL_COUNT || !str_equal(fields[COL_OUTCOME], "finished")) {
                continue;
            }
            run = port_atoi(fields[COL_RUN]);
            if (run < sRun && run > previousRun) {
                previousRun = run;
            }
        }
    }
    plat_free(copy);
    for (line = data; line != NULL && *line; line = next) {
        BenchGroup *g;
        s32 numFields, gapsColumn;
        next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        numFields = split_fields(line, fields);
        if (numFields < COL_COUNT || !str_equal(fields[COL_OUTCOME], "finished")) {
            continue; // The header, or a race that didn't finish.
        }
        run = port_atoi(fields[COL_RUN]);
        if (quick) {
            if (run != sRun && (run != previousRun || previousRun == 0)) {
                continue;
            }
            k = run == sRun;
        } else {
            if (run != sRun) {
                continue;
            }
            k = port_atoi(fields[COL_EXTRA]);
            if (k < 0 || k >= config_count()) {
                continue;
            }
        }
        for (t = 0; t < numTracks && !str_equal(groups[t][0].track, fields[COL_TRACK]); t++) {}
        if (t == numTracks) {
            if (numTracks == BENCH_MAX_TRACKS) {
                continue;
            }
            numTracks++;
            plat_snprintf(groups[t][0].track, sizeof(groups[t][0].track), "%s", fields[COL_TRACK]);
        }
        g = &groups[t][k];
        if (quick) {
            for (i = 0; fields[COL_EXTRA][i] != '\0' && fields[COL_EXTRA][i] != '\r' && i < 24; i++) {}
            plat_snprintf(g->build, sizeof(g->build), "run %d, %.*s", (int) run, (int) i, fields[COL_EXTRA]);
        }
        g->races++;
        g->fps += parse_decimal(fields[COL_FPS]);
        g->pct30 += parse_decimal(fields[COL_PCT_30FPS]);
        if (!quick && numFields > COL_PCT_60FPS) {
            g->pct60 += parse_decimal(fields[COL_PCT_60FPS]);
        }
        g->rdp += parse_decimal(fields[COL_RDP]);
        g->gpu += parse_decimal(fields[COL_GPU_DRAW]);
        g->audio += parse_decimal(fields[COL_AUDIO]);
        gapsColumn = quick ? COL_AUDIO_GAPS : COL_AUDIO_GAPS + 1;
        g->gaps += numFields > gapsColumn ? port_atoi(fields[gapsColumn]) : 0;
        // The frame pacing, after the sound's columns and the graphics settings.
        if (numFields > gapsColumn + 8) {
            g->pacedRaces++;
            g->low1 += parse_decimal(fields[gapsColumn + 4]);
            for (i = 0; i < 4; i++) {
                g->paced[i] += parse_decimal(fields[gapsColumn + 5 + i]);
            }
        }
        if (port_atoi(fields[COL_WORST_FRAME]) > g->worstFrame) {
            g->worstFrame = port_atoi(fields[COL_WORST_FRAME]);
        }
        if (port_atoi(fields[COL_WORST_GPU]) > g->worstGpu) {
            g->worstGpu = port_atoi(fields[COL_WORST_GPU]);
        }
        for (i = 0; i < BENCH_LIMITS; i++) {
            if (str_equal(fields[COL_LIMIT], sLimitNames[i])) {
                g->limits[i]++;
            }
        }
    }
    plat_free(data);

    if (quick) {
        if (previousRun == 0) {
            length = plat_snprintf(out, size,
                                   "\n### Quick test (averages of the finished races)\n\n"
                                   "No earlier quick test on this console to compare with; the next one will be.\n\n"
                                   "| track | run, build | races | fps | 1%% low | frames 60/30/20/slower %% | 30 fps | GPU ms "
                                   "| worst GPU | rdp ms | worst frame | limit | audio ms | sound gaps |\n"
                                   "|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|--:|--:|\n");
        } else {
            length = plat_snprintf(out, size,
                                   "\n### Quick test: run %d against run %d (averages of the finished races)\n\n"
                                   "| track | run, build | races | fps | 1%% low | frames 60/30/20/slower %% | 30 fps | GPU ms "
                                   "| worst GPU | rdp ms | worst frame | limit | audio ms | sound gaps |\n"
                                   "|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|--:|--:|\n",
                                   (int) sRun, (int) previousRun);
        }
    } else if (sMode == MODE_SWITCH) {
        static const char *const sAaNames[3] = { "off", "4x", "2x" };
        s32 aa = plat_antialias();
        length = plat_snprintf(out, size,
                               "\n### Switches (averages of the finished races)\n\n"
                               "Each switch is flipped by itself; saved is against the usual settings' races. "
                               "Render settings, the bench's own in every race (see apply_config): %d fps cap, "
                               "anti-aliasing %s, %s, 3D off.\n\n"
                               "| track | switch | races | fps | 1%% low | frames 60/30/20/slower %% | 30 fps | 60 fps | GPU ms "
                               "| GPU saved | rdp ms | rdp saved | worst GPU | worst frame | limit |\n"
                               "|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|\n",
                               60 / plat_frame_retraces(), aa >= 0 && aa < 3 ? sAaNames[aa] : "?",
                               plat_old_3ds() ? "the N64's 4:3, 400 pixels"
                                              : "Balanced widescreen, 800 pixels (the 4x setting, drawn as 2x there), "
                                                "colour boost");
    } else {
        length = plat_snprintf(out, size,
                               "\n### Render settings (averages of the finished races)\n\n"
                               "GPU saved is against the first setting, %s.\n\n"
                               "| track | settings | races | fps | 1%% low | frames 60/30/20/slower %% | 30 fps | 60 fps | GPU ms "
                               "| GPU saved | worst GPU | rdp ms | worst frame | limit | audio ms | sound gaps |\n"
                               "|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|--:|--:|\n",
                               sBenchConfigs[0].name);
    }
    if (!quick) {
        for (t = 0; t < numTracks; t++) {
            for (k = 0; k < config_count(); k++) {
                BenchGroup *g = &groups[t][k], *base = &groups[t][0];
                s32 limit = 0;
                if (g->races == 0) {
                    continue;
                }
                for (i = 1; i < BENCH_LIMITS; i++) {
                    if (g->limits[i] > g->limits[limit]) {
                        limit = i;
                    }
                }
                if (length >= size) {
                    return;
                }
                if (sMode == MODE_SWITCH) {
                    length += plat_snprintf(
                        out + length, size - length,
                        "| %s | %s | %d | %.1f | %s | %.0f%% | %.0f%% | %.1f | %s%.1f | %.1f | %s%.1f | %d | %d | %s |\n",
                        groups[t][0].track, config_name(k), (int) g->races, bench_avg(g->fps, g->races),
                        pacing_cells(g), bench_avg(g->pct30, g->races), bench_avg(g->pct60, g->races), bench_avg(g->gpu, g->races),
                        base->races ? "" : "?",
                        base->races ? bench_avg(base->gpu, base->races) - bench_avg(g->gpu, g->races) : 0.0,
                        bench_avg(g->rdp, g->races), base->races ? "" : "?",
                        base->races ? bench_avg(base->rdp, base->races) - bench_avg(g->rdp, g->races) : 0.0,
                        (int) g->worstGpu, (int) g->worstFrame, sLimitNames[limit]);
                    continue;
                }
                length += plat_snprintf(
                    out + length, size - length,
                    "| %s | %s | %d | %.1f | %s | %.0f%% | %.0f%% | %.1f | %s%.1f | %d | %.1f | %d | %s | %.2f | %.0f |\n",
                    groups[t][0].track, config_name(k), (int) g->races, bench_avg(g->fps, g->races),
                    pacing_cells(g), bench_avg(g->pct30, g->races), bench_avg(g->pct60, g->races), bench_avg(g->gpu, g->races),
                    base->races ? "" : "?",
                    base->races ? bench_avg(base->gpu, base->races) - bench_avg(g->gpu, g->races) : 0.0,
                    (int) g->worstGpu, bench_avg(g->rdp, g->races), (int) g->worstFrame, sLimitNames[limit],
                    bench_avg(g->audio, g->races), g->gaps);
            }
        }
        return;
    }
    for (t = 0; t < numTracks; t++) {
        for (k = 0; k < 2; k++) {
            BenchGroup *g = &groups[t][k];
            s32 limit = 0;
            if (g->races == 0) {
                continue;
            }
            for (i = 1; i < BENCH_LIMITS; i++) {
                if (g->limits[i] > g->limits[limit]) {
                    limit = i;
                }
            }
            if (length >= size) {
                return;
            }
            length += plat_snprintf(out + length, size - length,
                                    "| %s | %s | %d | %.1f | %s | %.0f%% | %.1f | %d | %.1f | %d | %s | %.2f | %.0f |\n",
                                    groups[t][0].track, g->build, (int) g->races,
                                    bench_avg(g->fps, g->races), pacing_cells(g), bench_avg(g->pct30, g->races),
                                    bench_avg(g->gpu, g->races), (int) g->worstGpu, bench_avg(g->rdp, g->races),
                                    (int) g->worstFrame, sLimitNames[limit], bench_avg(g->audio, g->races),
                                    g->gaps);
        }
    }
    for (t = 0; t < numTracks && length < size; t++) {
        BenchGroup *off = &groups[t][0], *on = &groups[t][1];
        if (off->races == 0 || on->races == 0) {
            continue;
        }
        length += plat_snprintf(out + length, size - length,
                                "%s%s: %+.1f fps, 30 fps on %+.0f%% of frames, GPU %+.1f ms, rdp %+.1f ms, worst "
                                "frame %+d ms, audio %+.2f ms",
                                t == 0 ? "\n" : "", groups[t][0].track,
                                bench_avg(on->fps, on->races) - bench_avg(off->fps, off->races),
                                bench_avg(on->pct30, on->races) - bench_avg(off->pct30, off->races),
                                bench_avg(on->gpu, on->races) - bench_avg(off->gpu, off->races),
                                bench_avg(on->rdp, on->races) - bench_avg(off->rdp, off->races),
                                (int) (on->worstFrame - off->worstFrame),
                                bench_avg(on->audio, on->races) - bench_avg(off->audio, off->races));
        if (length < size && off->pacedRaces > 0 && on->pacedRaces > 0) {
            length += plat_snprintf(out + length, size - length, ", 1%% low %+.1f fps, frames slower than 30 fps %+.0f%%",
                                    bench_avg(on->low1, on->pacedRaces) - bench_avg(off->low1, off->pacedRaces),
                                    bench_avg(on->paced[2] + on->paced[3], on->pacedRaces) -
                                        bench_avg(off->paced[2] + off->paced[3], off->pacedRaces));
        }
        if (length < size) {
            length += plat_snprintf(out + length, size - length, ".\n");
        }
    }
}

/* The boss fights' summary: one line per race of this run, from its results file, as Markdown. */
static void boss_summary(char *out, s32 size) {
    char path[256];
    char *data, *text, *line, *next;
    const char *fields[MAX_FIELDS];
    unsigned int dataSize;
    s32 length;

    make_path(path, sizeof(path), results_file());
    data = plat_load_file(path, &dataSize);
    if (data == NULL) {
        return;
    }
    text = plat_malloc(dataSize + 1);
    if (text == NULL) {
        plat_free(data);
        return;
    }
    bcopy(data, text, dataSize);
    text[dataSize] = '\0';
    plat_free(data);
    length = plat_snprintf(out, size,
                           "\n### Boss fights\n\n| boss | character | outcome | time s | place | fps | 30 fps | worst frame "
                           "| unsticks |\n|---|---|---|--:|--:|--:|--:|--:|--:|\n");
    for (line = text; line != NULL && *line && length < size; line = next) {
        next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        if (split_fields(line, fields) < COL_COUNT || port_atoi(fields[COL_RUN]) != sRun) {
            continue; // The header, or an earlier run.
        }
        length += plat_snprintf(out + length, size - length, "| %s | %s | %s | %s | %s | %s | %s%% | %s | %s |\n",
                                fields[COL_TRACK], fields[COL_CHARACTER], fields[COL_OUTCOME], fields[COL_RACE_S],
                                fields[COL_POSITION], fields[COL_FPS], fields[COL_PCT_30FPS],
                                fields[COL_WORST_FRAME], fields[COL_UNSTICKS]);
    }
    plat_free(text);
}

/* The whole game run's own columns, after the common ones, then the sound's, the graphics and the frame pacing's. */
enum WholeColumn {
    WCOL_STOP = COL_EXTRA,
    WCOL_PART,
    WCOL_BUILD,
    WCOL_GAPS,
    WCOL_DROPS,
    WCOL_SOUND,
    WCOL_GRAPHICS,
    WCOL_LOW1,
    WCOL_PACED60,
    WCOL_PACED30,
    WCOL_PACED20,
    WCOL_PACED_SLOWER,
    WCOL_COUNT
};

/* This run's results file, as text to split (plat_free it), or NULL. */
static char *load_results(void) {
    char path[256];
    char *data, *text;
    unsigned int dataSize;
    io_flush();
    make_path(path, sizeof(path), results_file());
    data = plat_load_file(path, &dataSize);
    if (data == NULL) {
        return NULL;
    }
    text = plat_malloc(dataSize + 1);
    if (text != NULL) {
        bcopy(data, text, dataSize);
        text[dataSize] = '\0';
    }
    plat_free(data);
    return text;
}

/* The next line of this run with its fields, or FALSE at the end. *cursor walks the text. */
static s32 next_run_line(char **cursor, const char **fields) {
    while (*cursor != NULL && **cursor) {
        char *line = *cursor;
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        *cursor = next;
        if (split_fields(line, fields) >= WCOL_COUNT && port_atoi(fields[COL_RUN]) == sRun) {
            return TRUE;
        }
    }
    return FALSE;
}

/* The stops from..to-1 of this run, one row each, as Markdown. Returns the length written. */
static s32 whole_table(char *out, s32 size, s32 from, s32 to) {
    const char *fields[MAX_FIELDS];
    char *text = load_results(), *cursor = text;
    s32 length, number;
    if (text == NULL) {
        return 0;
    }
    length = plat_snprintf(out, size,
                           "| # | stop | character, vehicle | outcome | s | fps | 1%% low | frames 60/30/20/slower %% "
                           "| limit | worst ms | hitches |\n|--:|---|---|---|--:|--:|--:|---|---|--:|--:|\n");
    while (length < size && next_run_line(&cursor, fields)) {
        number = port_atoi(fields[1]);
        if (number <= from || number > to) {
            continue;
        }
        length += plat_snprintf(out + length, size - length,
                                "| %d | %s | %s, %s | %s | %s | %s | %s | %s/%s/%s/%s | %s | %s | %s |\n", (int) number,
                                fields[COL_TRACK], fields[COL_CHARACTER], fields[5], fields[COL_OUTCOME],
                                fields[COL_RACE_S], fields[COL_FPS], fields[WCOL_LOW1], fields[WCOL_PACED60],
                                fields[WCOL_PACED30], fields[WCOL_PACED20], fields[WCOL_PACED_SLOWER], fields[COL_LIMIT],
                                fields[COL_WORST_FRAME], fields[16]);
    }
    plat_free(text);
    return length < size ? length : size - 1;
}

/* Every part of this run so far: stops, how many went wrong, the average fps, the lowest 1% low and where, and the
 * stops with the lowest 1% lows. Returns the length written. */
#define WHOLE_WORST 10
static s32 whole_parts(char *out, s32 size) {
    const char *fields[MAX_FIELDS];
    char *text = load_results(), *cursor = text;
    struct {
        s32 stops, bad, timed;
        double fps, low;
        char lowStop[48];
    } parts[STOP_KINDS];
    struct {
        double low;
        char text[96];
    } worst[WHOLE_WORST];
    s32 numWorst = 0, length, k, i;
    if (text == NULL) {
        return 0;
    }
    bzero(parts, sizeof(parts));
    while (next_run_line(&cursor, fields)) {
        double fps = parse_decimal(fields[COL_FPS]), low = parse_decimal(fields[WCOL_LOW1]);
        for (k = 0; k < STOP_KINDS && !str_equal(fields[WCOL_STOP], sStopKindNames[k]); k++) {}
        if (k == STOP_KINDS) {
            continue;
        }
        parts[k].stops++;
        parts[k].bad += !outcome_ok(fields[COL_OUTCOME]);
        if (fps <= 0.0) {
            continue;
        }
        parts[k].timed++;
        parts[k].fps += fps;
        if (low > 0.0 && (parts[k].low == 0.0 || low < parts[k].low)) {
            parts[k].low = low;
            plat_snprintf(parts[k].lowStop, sizeof(parts[k].lowStop), "%s", fields[COL_TRACK]);
        }
        if (low > 0.0) {
            for (i = numWorst; i > 0 && worst[i - 1].low > low; i--) {
                if (i < WHOLE_WORST) {
                    worst[i] = worst[i - 1];
                }
            }
            if (i < WHOLE_WORST) {
                worst[i].low = low;
                plat_snprintf(worst[i].text, sizeof(worst[i].text), "%s (%s, %s): %.1f fps, limit %s", fields[COL_TRACK],
                              fields[COL_CHARACTER], fields[5], fps, fields[COL_LIMIT]);
                if (numWorst < WHOLE_WORST) {
                    numWorst++;
                }
            }
        }
    }
    plat_free(text);
    length = plat_snprintf(out, size, "| part | stops | problems | average fps | lowest 1%% low |\n|---|--:|--:|--:|---|\n");
    for (k = 0; k < STOP_KINDS && length < size; k++) {
        if (parts[k].stops == 0) {
            continue;
        }
        length += plat_snprintf(out + length, size - length, "| %s | %d | %d | %.1f | %.1f %s |\n", sPartNames[k],
                                (int) parts[k].stops, (int) parts[k].bad,
                                parts[k].timed ? parts[k].fps / parts[k].timed : 0.0, parts[k].low, parts[k].lowStop);
    }
    if (numWorst > 0 && length < size) {
        length += plat_snprintf(out + length, size - length, "\nLowest 1%% lows so far:\n\n");
        for (i = 0; i < numWorst && length < size; i++) {
            length += plat_snprintf(out + length, size - length, "%d. **%.1f** %s\n", (int) i + 1, worst[i].low,
                                    worst[i].text);
        }
    }
    return length < size ? length : size - 1;
}

static const char *whole_title(void) {
    return sShort ? "whole game (short)" : "whole game";
}

/* A progress report of the whole game run: the stops from..to-1, under heading, and the run so far. */
static void send_progress(s32 from, s32 to, const char *heading) {
    char title[128];
    static char summary[8192];
    s32 length;
    plat_snprintf(title, sizeof(title), "[3DS test run] %s run %d: %d of %d stops, %d problems (in progress)",
                  whole_title(), (int) sRun, (int) (sFinished + sProblems), (int) sNumRaces, (int) sProblems);
    length = plat_snprintf(summary, sizeof(summary),
                           "Whole game run %d%s in progress: %d of %d stops done, %d fine, %d with problems. "
                           "Next: %s.\n\n### %s (stops %d to %d)\n\n",
                           (int) sRun, sShort ? " (short)" : "", (int) (sFinished + sProblems), (int) sNumRaces,
                           (int) sFinished, (int) sProblems, to < sNumRaces ? sPartNames[(s32) sRaces[to].kind] : "the end",
                           heading, (int) from + 1, (int) to);
    length += whole_table(summary + length, 4096, from, to);
    if (length < (s32) sizeof(summary) - 1500) {
        length += plat_snprintf(summary + length, sizeof(summary) - length, "\n### The run so far\n\n");
        length += whole_parts(summary + length, 1400);
    }
    if (length < (s32) sizeof(summary) - 1200) {
        length += hitch_summary(summary + length, 1200);
    }
    if (length < (s32) sizeof(summary) - 512) {
        draw_mix_summary(summary + length, sizeof(summary) - length);
    }
    port_log("autorun: progress report after stop %d", (int) to);
    plat_status("%s: report after %d of %d stops", port_text(sModes[sMode].name), (int) to, (int) sNumRaces);
    plat_send_test_update(title, summary, PLAT_RESULTS_PROGRESS);
}

/*
 * The whole game run, at the title screen before each stop: as a part ends (and at least every
 * DKR_AUTORUN_REPORT_MINUTES), the part's hook runs and its progress report goes out; the next stop waits until the
 * report is out (or REPORT_WAIT_SECONDS), so no race is measured while the network is busy. Returns TRUE when the next
 * stop may start.
 */
static s32 whole_ready(void) {
    unsigned long long now = plat_time_ns();
    const char *env;
    s32 minutes, partEnd, timed, from, start;
    if (sReportWaitNs != 0) {
        if (plat_test_results_busy() && now - sReportWaitNs < REPORT_WAIT_SECONDS * 1000000000ULL) {
            return FALSE;
        }
        sReportWaitNs = 0;
        return TRUE;
    }
    if (sNext == 0 || sNext >= sNumRaces || sReportedAt == sNext) {
        return TRUE;
    }
    env = plat_getenv("DKR_AUTORUN_REPORT_MINUTES");
    minutes = env != NULL ? port_atoi(env) : 30;
    partEnd = sRaces[sNext].kind != sRaces[sNext - 1].kind;
    timed = minutes > 0 && now - sLastReportNs >= (unsigned long long) minutes * 60 * 1000000000ULL;
    if (!partEnd && !timed) {
        return TRUE;
    }
    for (start = sNext - 1; start > 0 && sRaces[start - 1].kind == sRaces[sNext - 1].kind; start--) {}
    from = (!partEnd && sReportedAt > start) ? sReportedAt : start;
    if (partEnd) {
        port_autorun_section_done(sPartNames[(s32) sRaces[sNext - 1].kind]);
    }
    {
        char heading[64];
        plat_snprintf(heading, sizeof(heading), "%s%s", sPartNames[(s32) sRaces[sNext - 1].kind],
                      partEnd ? "" : " so far");
        send_progress(from, sNext, heading);
    }
    sReportedAt = sNext;
    sLastReportNs = now;
    sReportWaitNs = now;
    save_state(-1);
    return FALSE;
}

/* Sends the results so far as a GitHub issue (when bug reports are set up), and says so on the bottom screen. */
static void send_results(const char *why) {
    char title[128];
    static char summary[8192];
    s32 length;
    io_flush(); // The summaries and the attached files read the results back.
    plat_snprintf(title, sizeof(title), "[3DS test run] %srun %d: %d of %d races, %d problems (%s)",
                  sMode == MODE_BENCH    ? "GPU benchmark "
                  : sMode == MODE_SWITCH ? (sShort ? "shelved switches " : "switch bench ")
                  : sMode == MODE_QUICK  ? "quick test "
                  : sMode == MODE_BOSS  ? "boss fights "
                  : sMode == MODE_WHOLE ? (sShort ? "whole game (short) " : "whole game ")
                                        : "",
                  (int) sRun, (int) (sFinished + sProblems), (int) sNumRaces,
                  (int) sProblems, why);
    length = plat_snprintf(summary, sizeof(summary),
                           "%s %d %s after %d of %d races: %d finished, %d with problems.\n"
                           "Average frame rate of the races finished since the game started: %.1f fps (%d races).\n",
                           sMode == MODE_BENCH    ? "GPU benchmark run"
                           : sMode == MODE_SWITCH ? (sShort ? "Shelved switches bench run" : "Switch bench run")
                           : sMode == MODE_QUICK  ? "Quick test run"
                           : sMode == MODE_BOSS  ? "Boss fights run"
                           : sMode == MODE_WHOLE ? "Whole game run"
                                                 : "Run",
                           (int) sRun, why,
                           (int) (sFinished + sProblems), (int) sNumRaces, (int) sFinished, (int) sProblems,
                           sFpsCount ? sFpsSum / sFpsCount : 0.0, (int) sFpsCount);
    if (length > 0 && length < (s32) sizeof(summary)) {
        length += plat_snprintf(summary + length, sizeof(summary) - length,
                                "Graphics when sent: anti-aliasing %s, %d fps cap, render ahead %d "
                                "(each race's are in the results file's graphics column).\n",
                                aa_label(), 60 / plat_frame_retraces(),
                                plat_render_ahead());
    }
    if (length > 0 && length < (s32) sizeof(summary) - 2048) {
        // Kept to 2 KB, so the mode's own tables below still fit.
        length += hitch_summary(summary + length, 2048);
    }
    if (sMode == MODE_WHOLE && length > 0 && length < (s32) sizeof(summary)) {
        length += plat_snprintf(summary + length, sizeof(summary) - length, "\n### Every part\n\n");
        if (sReportedAt < sNext && sNext > 0 && length < (s32) sizeof(summary) - 3000) {
            // The stops since the last progress report, which no report has listed yet.
            s32 from = sReportedAt > 0 ? sReportedAt : 0, to = sNext < sNumRaces ? sNext + 1 : sNumRaces;
            length += whole_parts(summary + length, 1600);
            length += plat_snprintf(summary + length, sizeof(summary) - length,
                                    "\n### Since the last report (stops %d to %d)\n\n", (int) from + 1, (int) to);
            whole_table(summary + length, sizeof(summary) - length - 512, from, to);
        } else {
            whole_parts(summary + length, sizeof(summary) - length);
        }
    } else if (sMode == MODE_BOSS && length > 0 && length < (s32) sizeof(summary)) {
        boss_summary(summary + length, sizeof(summary) - length);
    } else if (sMode != MODE_FULL && length > 0 && length < (s32) sizeof(summary)) {
        bench_summary(summary + length, sizeof(summary) - length);
    }
    length = (s32) strlen(summary);
    if (length < (s32) sizeof(summary) - 512) {
        draw_mix_summary(summary + length, sizeof(summary) - length);
    }
    plat_status("%s %s: %d races, %d problems", port_text(sModes[sMode].name), port_text(why), (int) (sFinished + sProblems), (int) sProblems);
    plat_send_test_update(title, summary, sMode == MODE_WHOLE ? PLAT_RESULTS_FINAL : PLAT_RESULTS_NEW);
}

/* Ends the test run: saves are written again and the profiler goes away (unless DKR_PROFILE or its button is on). */
static void turn_off(const char *why) {
    if (sMode == MODE_WHOLE) {
        put_back_progress();
        menu_release();
        if (sNext > 0 && sNext <= sNumRaces) {
            port_autorun_section_done(sPartNames[(s32) sRaces[sNext - 1].kind]);
        }
    }
    send_results(why);
    sEnabled = 0;
    sInitialised = FALSE;
    set_phase(PHASE_OFF);
    save_state(-1);
    if (is_bench()) {
        apply_config(-1); // Back to the player's settings.
    }
    sMode = MODE_FULL;
    prof_set_status(NULL);
}

/* Called in races: nonzero asks the game to quit to the title screen. */
s32 port_autorun_quit_race(void) {
    if (sQuitPending) {
        sQuitPending = FALSE;
        return TRUE;
    }
    if (sEnabled > 0 && sPhase == PHASE_FINISHED && sLingerRetraces >= FINISH_LINGER_SECONDS * 60) {
        set_phase(PHASE_LEAVING);
        sNext++;
        if (sNext >= sNumRaces) {
            if (plat_getenv("DKR_AUTORUN_LOOP") != NULL) {
                send_results("finished");
                sNext = 0;
                sRun++;
                sFinished = 0;
                sProblems = 0;
            } else {
                port_log("autorun: all %d races done, %d finished, %d problems", (int) sNumRaces, (int) sFinished,
                         (int) sProblems);
                turn_off("finished");
                return TRUE;
            }
        }
        save_state(-1);
        show_status();
        return TRUE;
    }
    return FALSE;
}

static void end_race(const char *outcome, s32 position) {
    prof_window_read(PROF_WINDOW_SESSION, &sRaceStats);
    sOutcome = outcome;
    record_result(outcome, position, sPhase == PHASE_RACING ? &sRaceStats : NULL);
    record_hitches(outcome);
    record_draw_mix(outcome);
    if (outcome_ok(outcome)) {
        if (sRaceStats.elapsedNs > 0) {
            sFpsSum += sRaceStats.gameFrames * 1e9 / sRaceStats.elapsedNs;
            sFpsCount++;
        }
    } else {
        char what[64];
        plat_snprintf(what, sizeof(what), "%s after %d s", outcome, (int) (sRaceRetraces / 60));
        record_problem(what);
    }
    set_phase(PHASE_FINISHED);
    // So that a crash or hang in the results screen isn't recorded as a second result for this race.
    save_state_recorded(sNext, TRUE);
    if (plat_getenv("DKR_AUTORUN_SNAPSHOT") != NULL) {
        gPlatSnapshotRequested = 1;
    }
}

static void handle_request(void) {
    s32 request = sRequest;
    sRequest = REQUEST_NONE;
    if ((request == REQUEST_START || request == REQUEST_BENCH || request == REQUEST_QUICK || request == REQUEST_BOSS ||
         request == REQUEST_SWITCH || request == REQUEST_SWITCH_SHELVED || request == REQUEST_WHOLE ||
         request == REQUEST_WHOLE_SHORT) &&
        !port_autorun_active()) {
        sShort = request == REQUEST_WHOLE_SHORT || (request == REQUEST_SWITCH_SHELVED && !plat_old_3ds());
        if (request == REQUEST_SWITCH_SHELVED) {
            if (plat_old_3ds()) {
                plat_status("Shelved switches: a New 3DS bench");
                return;
            }
            request = REQUEST_SWITCH;
        }
        if (request == REQUEST_SWITCH && SWITCH_CONFIGS == 1) {
            plat_status("Switch bench: no switches to test right now");
            return;
        }
        sMode = (request == REQUEST_WHOLE || request == REQUEST_WHOLE_SHORT) ? MODE_WHOLE
                : request == REQUEST_SWITCH  ? MODE_SWITCH
                : request == REQUEST_BENCH ? MODE_BENCH
                : request == REQUEST_QUICK ? MODE_QUICK
                : request == REQUEST_BOSS  ? MODE_BOSS
                                           : MODE_FULL;
        sEnabled = 1;
        sInitialised = FALSE;
        if (!port_autorun_active()) {
            return; // Nothing to race.
        }
        prof_window_start(PROF_WINDOW_OVERLAY);
        plat_status("%s started: race %d of %d", port_text(sModes[sMode].name), (int) sNext + 1,
                    (int) sNumRaces);
        if (get_game_mode() == GAMEMODE_INGAME) {
            sQuitPending = TRUE; // Out of the game, to the title screen where the races start.
        }
    } else if (request == REQUEST_STOP && port_autorun_active()) {
        if (sPhase >= PHASE_LOADING && sPhase <= PHASE_FINISHED && get_game_mode() == GAMEMODE_INGAME) {
            sQuitPending = TRUE;
        }
        port_log("autorun: stopped from the bottom screen at race %d", (int) sNext + 1);
        turn_off("stopped");
    }
}

/*
 * The AI that drives the player's kart sometimes wedges it somewhere a CPU racer never goes (CPU racers use simpler
 * physics, and the AI's own reverse-out only works on the ground). After UNSTICK_SECONDS without a new checkpoint
 * the kart is put just before the next checkpoint, facing along the course, the way the game resets a racer to the
 * start line (func_800230D0). If it gets stuck again before that checkpoint, the checkpoint is counted as passed and
 * the kart is put before the one after it. Each time is noted in the results and in autorun_problems.txt, so the spot
 * is known.
 */
static void unstick(Object *obj, Object_Racer *racer, const char *why) {
    s32 checkpoint = racer->nextCheckpoint;
    CheckpointNode *node;
    Camera *camera;
    char what[128];
    s32 i, length;

    // Put there before and it didn't help (the AI drives back into the same wall): count that checkpoint as passed
    // and put the kart before the one after it instead.
    if (sUnsticks > 0 && sLastUnstick == racer->lap * 1000 + checkpoint && get_checkpoint_count() > 0) {
        // Passed, the way the game counts it (racer.c, after checkpoint_is_passed).
        checkpoint = (checkpoint + 1) % get_checkpoint_count();
        racer->nextCheckpoint = checkpoint;
        if (checkpoint == 0 && racer->courseCheckpoint > 0 && racer->lap < 120) {
            racer->lap++;
        }
        racer->courseCheckpoint++;
    }
    sLastUnstick = racer->lap * 1000 + racer->nextCheckpoint;
    node = find_next_checkpoint_node(checkpoint, racer->isOnAlternateRoute);
    if (node == NULL) {
        return;
    }
    plat_snprintf(what, sizeof(what), "note: put at checkpoint %d of lap %d %s, from %.0f,%.0f,%.0f", (int) checkpoint,
                  (int) racer->lap + 1, why, obj->trans.x_position, obj->trans.y_position, obj->trans.z_position);
    record_problem(what);
    length = strlen(sUnstuckAt);
    plat_snprintf(sUnstuckAt + length, sizeof(sUnstuckAt) - length, "%s%d:%d", length ? " " : "", (int) racer->lap + 1,
                  (int) checkpoint);
    sUnsticks++;
    obj->trans.x_position = node->x - node->rotationXFrac * 20.0f;
    obj->trans.y_position = node->y;
    obj->trans.z_position = node->z - node->rotationZFrac * 20.0f;
    obj->segmentID = get_level_segment_index_from_position(obj->trans.x_position, obj->trans.y_position,
                                                           obj->trans.z_position);
    racer->prev_x_position = obj->trans.x_position;
    racer->prev_y_position = obj->trans.y_position;
    racer->prev_z_position = obj->trans.z_position;
    racer->steerVisualRotation = arctan2_f(node->rotationXFrac, node->rotationZFrac);
    obj->trans.rotation.y_rotation = racer->steerVisualRotation;
    obj->trans.rotation.x_rotation = 0;
    obj->trans.rotation.z_rotation = 0;
    for (i = 0; i < 4; i++) {
        racer->unkD8[i * 3 + 0] = obj->trans.x_position;
        racer->unkD8[i * 3 + 1] = obj->trans.y_position;
        racer->unkD8[i * 3 + 2] = obj->trans.z_position;
    }
    obj->interactObj->x_position = obj->trans.x_position;
    obj->interactObj->y_position = obj->trans.y_position;
    obj->interactObj->z_position = obj->trans.z_position;
    racer->velocity = 0.0f;
    racer->lateral_velocity = 0.0f;
    racer->y_rotation_vel = 0;
    racer->x_rotation_vel = 0;
    racer->z_rotation_vel = 0;
    obj->x_velocity = 0.0f;
    obj->y_velocity = 0.0f;
    obj->z_velocity = 0.0f;
    racer->vehicleID = racer->vehicleIDPrev;
    set_active_camera(racer->playerIndex);
    camera = cam_get_active_camera_no_cutscenes();
    camera->trans.x_position = obj->trans.x_position;
    camera->trans.y_position = obj->trans.y_position;
    camera->trans.z_position = obj->trans.z_position;
}

/*
 * Tricky's race sends the player back to the hub once they fall 400 units below the highest point they reached
 * (vehicle_tricky.c). The AI driving the kart, made for Tricky's own path, sometimes takes it off the side near the
 * summit, so a kart falling well below its best height is put back on the course before the game gives up on it.
 */
extern f32 gTrickyRacerPeakHeight;
static s32 falling_off_tricky(Object *obj) {
    s32 i, count;
    Object **racers;
    if (obj->y_velocity >= 0.0f || obj->trans.y_position + 250.0f >= gTrickyRacerPeakHeight) {
        return FALSE;
    }
    racers = get_racer_objects(&count);
    for (i = 0; i < count; i++) {
        if (racers[i]->racer->vehicleID == VEHICLE_TRICKY) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Called once per game frame, with the number of retraces it took. */
void port_autorun_frame(s32 updateRate) {
    Object *obj;
    Object_Racer *racer;
    s32 progress;
    static s32 sStatusCounter;

    if (sRequest != REQUEST_NONE) {
        handle_request();
    }
    obj = (get_game_mode() == GAMEMODE_INGAME) ? get_racer_object_by_port(PLAYER_ONE) : NULL;
    racer = (obj != NULL) ? obj->racer : NULL;
    {
        // What the player is doing, for bug reports (formatted only when one is made).
        PlatGameNote note = { 0 };
        if (racer != NULL) {
            note.racing = 1;
            note.lap = racer->lap + 1;
            note.checkpoint = racer->nextCheckpoint;
            note.bananas = racer->bananas;
            note.boost = racer->boostTimer;
            note.itemType = racer->balloon_type;
            note.itemCount = racer->balloon_quantity;
            note.shield = racer->shieldTimer;
            note.magnet = racer->magnetTimer;
            note.hit = racer->attackType;
            note.paused = is_game_paused() != 0;
        }
        plat_set_game_note(&note);
    }
    if (!port_autorun_active()) {
        return;
    }
    if (sPhase == PHASE_RACING && sNext < sNumRaces && sRaces[sNext].kind == STOP_MENUS) {
        const char *outcome;
        sRaceRetraces += updateRate;
        outcome = menu_pass_frame(updateRate);
        if (outcome != NULL) {
            menu_release();
            end_race(outcome, 0);
        }
        if (sStatusCounter++ % 30 == 0) {
            show_status();
        }
        return;
    }
    switch (sPhase) {
        case PHASE_LOADING:
            if (racer != NULL) {
                sLoadNs = plat_time_ns() - sPhaseStartNs;
                set_phase(PHASE_COUNTDOWN);
            } else if (plat_time_ns() - sPhaseStartNs > 120000000000ULL) {
                end_race("load_timeout", 0);
            }
            break;
        case PHASE_COUNTDOWN:
            if (racer != NULL && get_race_countdown() == 0) {
                if (sRaces[sNext].laps > 0 && level_header() != NULL) {
                    level_header()->laps = sRaces[sNext].laps; // The short whole game run: one lap
                }
                prof_window_start(PROF_WINDOW_SESSION);
                prof_draw_mix_window(NULL, NULL, TRUE);
                sAudioGapsStart = gPlatCounters[PLAT_COUNTER_AUDIO_UNDERRUN];
                sAudioDropsStart = gPlatCounters[PLAT_COUNTER_AUDIO_DROPPED];
                set_phase(PHASE_RACING);
            } else if (plat_time_ns() - sPhaseStartNs > 120000000000ULL) {
                // The game left the level, or never started the race (a boss's speech is far shorter than this).
                end_race(racer == NULL ? "left_before_start" : "countdown_timeout", 0);
            }
            break;
        case PHASE_RACING:
            if (racer == NULL) {
                end_race("left_race", 0);
                break;
            }
            sRaceRetraces += updateRate;
            if (sRaces[sNext].kind == STOP_HUB) {
                const char *outcome;
                if (sDriving && level_id() != sRaceLevel) {
                    char what[96];
                    plat_snprintf(what, sizeof(what), "note: the hub tour left for level %d at segment %d",
                                  (int) level_id(), (int) sTourSegment);
                    record_problem(what);
                    end_race("left_level", 0);
                    break;
                }
                outcome = hub_tour_frame(obj, racer, updateRate);
                if (outcome != NULL) {
                    end_race(outcome, 0);
                }
                break;
            }
            if (sRaces[sNext].kind == STOP_ARENA) {
                // No course to make progress along: played until it ends (it may go back to the hub) or time is up.
                if (!sDriving) {
                    sDriving = TRUE;
                    sRaceLevel = level_id();
                }
                if (racer->raceFinished) {
                    end_race("finished", racer->finishPosition);
                } else if (level_id() != sRaceLevel ||
                           sRaceRetraces >= (sShort ? ARENA_SECONDS * 2 / 5 : ARENA_SECONDS) * 60) {
                    end_race("played", racer->racePosition);
                }
                break;
            }
            if (sDriving && level_id() != sRaceLevel) {
                // The game took the player out of the race without finishing it (falling off a boss's course sends
                // them to the hub). Boss races start in another level, for the boss's speech, so the race's level is
                // the one the kart got going in.
                char what[96];
                plat_snprintf(what, sizeof(what), "note: the game left the race for level %d, last at %.0f,%.0f,%.0f",
                              (int) level_id(), sLastPosition[0], sLastPosition[1], sLastPosition[2]);
                record_problem(what);
                end_race("left_level", racer->racePosition);
                break;
            }
            sLastPosition[0] = obj->trans.x_position;
            sLastPosition[1] = obj->trans.y_position;
            sLastPosition[2] = obj->trans.z_position;
            progress = racer->courseCheckpoint;
            {
                // DKR_LOG_RACER: logs the kart's checkpoint, speed and place at each checkpoint and every half second,
                // to compare two runs (30 and 60 fps physics, say).
                static s32 sLogRacer = -1, sLastCp = -999, sLastHalf = -1;
                if (sLogRacer < 0) {
                    sLogRacer = plat_getenv("DKR_LOG_RACER") != NULL;
                }
                if (sLogRacer && (progress != sLastCp || sRaceRetraces / 30 != sLastHalf)) {
                    port_log("racer t=%.3f cp=%d v=%.3f lv=%.3f yv=%.3f pos=%.1f,%.1f,%.1f boost=%d bananas=%d",
                             sRaceRetraces / 60.0,
                             (int) progress, racer->velocity, racer->lateral_velocity, obj->y_velocity,
                             obj->trans.x_position, obj->trans.y_position, obj->trans.z_position,
                             (int) racer->boostTimer, (int) racer->bananas);
                    sLastCp = progress;
                    sLastHalf = sRaceRetraces / 30;
                }
            }
            if (!sDriving && (racer->velocity > 1.0f || racer->velocity < -1.0f)) {
                sDriving = TRUE;
                sRaceLevel = level_id();
            }
            if (!sDriving) {
                // Not going yet: no time without progress.
            } else if (progress > sBestProgress) {
                sBestProgress = progress;
                sStuckRetraces = 0;
            } else {
                sStuckRetraces += updateRate;
            }
            if (racer->raceFinished) {
                end_race("finished", racer->finishPosition);
            } else if (sStuckRetraces >= STUCK_SECONDS * 60) {
                end_race("stuck", racer->racePosition);
            } else if (sStuckRetraces >= UNSTICK_SECONDS * 60 &&
                       (sStuckRetraces - updateRate) / (UNSTICK_SECONDS * 60) < sStuckRetraces / (UNSTICK_SECONDS * 60)) {
                char why[48];
                plat_snprintf(why, sizeof(why), "after %d s without progress", UNSTICK_SECONDS);
                unstick(obj, racer, why);
            } else if (falling_off_tricky(obj)) {
                unstick(obj, racer, "as it fell off Tricky's mountain");
            } else if (sRaceRetraces >= sTimeoutSeconds * 60) {
                end_race("timeout", racer->racePosition);
            }
            break;
        case PHASE_FINISHED:
            sLingerRetraces += updateRate;
            if (get_game_mode() != GAMEMODE_INGAME) {
                // Not in a race (a load that never started): straight on to the next one.
                sLingerRetraces = FINISH_LINGER_SECONDS * 60;
                port_autorun_quit_race();
                if (sPhase == PHASE_LEAVING) {
                    set_phase(PHASE_TITLE);
                }
            }
            break;
    }
    if (sStatusCounter++ % 30 == 0) {
        show_status();
    }
}

/* The level and the active camera, for the host renderer's clipping sweep (DKR_CLIP_SWEEP, host/gfx_soft.c). */
void port_sweep_camera(int *level, float pos[3], int *segment) {
    Camera *camera = cam_get_active_camera();
    *level = level_id();
    pos[0] = camera->trans.x_position;
    pos[1] = camera->trans.y_position;
    pos[2] = camera->trans.z_position;
    *segment = camera->cameraSegmentID;
}

/* Whether the vertices are the level model's (track and scenery) rather than an object's, for the clipping sweep. */
s32 port_sweep_level_vertex(const void *vertices) {
    extern LevelModel *gCurrentLevelModel;
    s32 i;
    if (gCurrentLevelModel == NULL) {
        return FALSE;
    }
    for (i = 0; i < gCurrentLevelModel->numberOfSegments; i++) {
        LevelModelSegment *segment = &gCurrentLevelModel->segments[i];
        if ((const Vertex *) vertices >= segment->vertices &&
            (const Vertex *) vertices < segment->vertices + segment->numberOfVertices) {
            return TRUE;
        }
    }
    return FALSE;
}
