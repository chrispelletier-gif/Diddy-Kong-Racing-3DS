/*
 * DKR_BOTTOM_PREVIEW=dir: draws the 3DS bottom screen's views (src/plat/bottom_screen.c) with the ROM's fonts and
 * pictures to PNG files, to see them without a 3DS. DKR_MONO_FONT can name a file with the 8x8 font the 3DS uses for
 * dense text (libctru's, 2048 bytes); without it the small font stands in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat/bottom_screen.h"
#include "plat/plat.h"

int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

static unsigned short sImage[BS_WIDTH * BS_HEIGHT];
static unsigned char sRgba[BS_WIDTH * BS_HEIGHT * 4];

static void save(const char *dir, const char *name) {
    char path[512];
    int area, i;
    if (getenv("DKR_BOTTOM_LOG") != NULL) {
        bs_log_enable(1);
        bs_log_reset();
    }
    bs_take_changes();
    for (area = 0; area < BS_AREA_COUNT; area++) {
        bs_draw(sImage, area);
    }
    for (i = 0; i < BS_WIDTH * BS_HEIGHT; i++) {
        unsigned short p = sImage[i];
        sRgba[i * 4] = (p >> 11) << 3;
        sRgba[i * 4 + 1] = ((p >> 5) & 63) << 2;
        sRgba[i * 4 + 2] = (p & 31) << 3;
        sRgba[i * 4 + 3] = 255;
    }
    if (getenv("DKR_BOTTOM_LOG") != NULL) {
        snprintf(path, sizeof(path), "%s/%s.log", dir, name);
        bs_log_write(path);
    }
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    stbi_write_png(path, BS_WIDTH, BS_HEIGHT, 4, sRgba, BS_WIDTH * 4);
    printf("wrote %s\n", path);
}

static const char *sDumpDir;
static void preview_race(const char *dir);
static void preview_modes(const char *dir, RaceHud *race);

static void row(BsMenu *m, const char *label, const char *value, const char *hint) {
    BsMenuRow *r = &m->rows[m->rowCount++];
    snprintf(r->label, sizeof(r->label), "%s", label);
    snprintf(r->value, sizeof(r->value), "%s", value);
    snprintf(r->hint, sizeof(r->hint), "%s", hint);
}

/* The menu's tabs, as a player's build (count 8) or a debug build (10) has them, with tab picked. */
static void tabs(BsMenu *m, int picked, int count) {
    static const char *const sTabs[10] = { "Picture", "Screen",   "Controls", "Sound",    "Race HUD",
                                           "Camera",  "Start-up", "Saves",    "Test run", "Tools" };
    int i;
    for (i = 0; i < count; i++) {
        snprintf(m->tabs[i], sizeof(m->tabs[i]), "%s", sTabs[i]);
    }
    m->tabCount = count;
    m->tab = picked;
}

void host_bottom_preview(const char *dir) {
    static unsigned char sMono[2048];
    const char *monoPath = getenv("DKR_MONO_FONT");
    const unsigned char *mono = NULL;
    BsMenu menu;

    if (monoPath != NULL) {
        FILE *f = fopen(monoPath, "rb");
        if (f != NULL && fread(sMono, 1, sizeof(sMono), f) == sizeof(sMono)) {
            mono = sMono;
        }
        if (f != NULL) {
            fclose(f);
        }
    }
    if (getenv("DKR_BOTTOM_GRID") != NULL) {
        bs_set_grid(1); /* before the first frame: the screen before the ROM is on the grid too */
    }
    bs_init(mono);
    /* the look from the first frame, as on the 3DS (plat_3ds_console_init) */
    if (getenv("DKR_BOTTOM_STYLE") != NULL) {
        bs_set_style(atoi(getenv("DKR_BOTTOM_STYLE")));
    }
    if (getenv("DKR_BOTTOM_VARIANT") != NULL) {
        bs_set_variant(atoi(getenv("DKR_BOTTOM_VARIANT")));
    }
    {
        static const char *const sSettings[1] = { "Settings" };
        bs_set_buttons(sSettings, 1);
    }
    bs_set_status_line(0, " 42s vi +60 gfx 1260/1260 aud +60 sys+62");
    bs_set_status_line(1, "gap 0 drop 0 textures 0 GPU 12.3M heap 20.1M");
    bs_add_message("New 3DS, renderer on core 2");
    bs_add_message("Loading the ROM...");
    bs_add_message("ROM loaded in 1.2 s");
    save(dir, "0_before_rom");

    bs_load_assets();
    if (getenv("DKR_BOTTOM_DUMP") != NULL) {
        void bs_dump_art(const char *dir);
        /* the race pictures load with the first race view: draw one, then dump */
        sDumpDir = getenv("DKR_BOTTOM_DUMP");
    }
    if (getenv("DKR_BOTTOM_STYLE") != NULL) {
        bs_set_style(atoi(getenv("DKR_BOTTOM_STYLE")));
    }
    if (getenv("DKR_BOTTOM_VARIANT") != NULL) {
        bs_set_variant(atoi(getenv("DKR_BOTTOM_VARIANT")));
    }
    if (getenv("DKR_BOTTOM_GRID") != NULL) {
        bs_set_grid(1);
    }
    if (getenv("DKR_BOTTOM_METRICS") != NULL) {
        void bs_grid_metrics(void);
        bs_grid_metrics();
        exit(0);
    }
    if (getenv("DKR_BOTTOM_SMOOTH") != NULL) {
        bs_set_smooth_art(atoi(getenv("DKR_BOTTOM_SMOOTH")));
    }
    {
        static const char *const sHome[3] = { "Settings", "Controls", "Updates" };
        bs_set_buttons(sHome, 3);
    }
    bs_grid_glyph_sheet(1);
    save(dir, "00_glyphs");
    bs_grid_glyph_sheet(0);
    bs_add_message("Starting the game...");
    bs_add_message("Saves and settings: sdmc:/3ds/dkr/");
    save(dir, "1_home");
    /* the same home with no debug message, as a player build shows it (the logo 1:1) */
    bs_clear_messages();
    bs_preview_player_build(1);
    save(dir, "1_home_player");
    bs_preview_player_build(0);
    bs_add_message("Saves and settings: sdmc:/3ds/dkr/");

    /*
     * No "test run" readout page: during a test run the profiler keeps measuring and logging but the readout stays off
     * (port_profile.c show_overlay), so the bottom screen keeps the race HUD. The live monitor's pages follow.
     */
    {
        static const char *const sRunning[2] = { "Settings", "Stop test" };
        bs_set_buttons(sRunning, 2);
    }
    {
        // The live monitor: the frame graph over a page, made up to match its text: 30 fps held back by the renderer's
        // CPU (most frames 28-30.5 ms, under the 30 fps line, renderer yellow), 5% of the frames a retrace late (50 ms) at
        // uneven times, one of those a texture upload (the hitch), and a few frames led by
        // logic, the scene build, the GPU or the retrace wait.
        unsigned char height[BS_GRAPH_FRAMES], kind[BS_GRAPH_FRAMES];
        unsigned seed = 7;
        int i;
        for (i = 0; i < BS_GRAPH_FRAMES; i++) {
            int r;
            seed = seed * 1103515245u + 12345u;
            r = (int) ((seed >> 16) % 100);
            height[i] = (unsigned char) (56 + (seed >> 8) % 6); /* 28-30.5 ms in half milliseconds: clear under the 30 fps line */
            kind[i] = r < 70 ? BS_GRAPH_RENDERER : r < 80 ? BS_GRAPH_SCENE : r < 88 ? BS_GRAPH_LOGIC : r < 94 ? BS_GRAPH_RETRACE
                                                                                                        : BS_GRAPH_GPU;
        }
        {
            /* the late frames (5% of those shown, each a retrace late: 49-50 ms) where the renderer ran long, at uneven times */
            static const int sLate[] = { 44, 59, 66, 97, 121, 150 };
            unsigned j;
            for (j = 0; j < sizeof(sLate) / sizeof(sLate[0]); j++) {
                height[sLate[j]] = (unsigned char) (98 + j % 3);
                kind[sLate[j]] = BS_GRAPH_RENDERER;
            }
        }
        kind[97] = BS_GRAPH_TEXTURES | BS_GRAPH_HITCH; /* the worst: a late frame spent on a texture upload */
        bs_set_frame_graph(height, kind, BS_GRAPH_FRAMES);
    }
    /*
     * One made-up second, the same on every page (budget 33.3 ms at 30 fps): the game core busy 27.0 (logic 15.2 +
     * scene 9.8 + sound 2.0), waiting 5.0, idle 1.3 (4%); the render core busy 29.0 (the kinds' rdp 28.6 + textures
     * 0.4), waiting 2.2, idle 2.1 (6%): idle = budget - busy - waiting on both. 95% of frames on time and 5% a retrace
     * late (50 ms): 1 / (0.95 x 33.3 + 0.05 x 50) ms = 29.3 fps. The kinds (2b) add up to the page totals (2d) and the
     * objects (2c) to the kinds' racers and objects rows.
     */
    bs_set_overlay("PROFILER 1/5  tap: page\n"
                   "29.3/30 fps held back by renderer CPU\n"
                   "on time 95%, 1 vsync late 5%, 2+ late 0%\n"
                   "worst 50 ms (budget 33) hitches 1\n"
                   "steady: frame to frame 1.8 ms\n"
                   "graph: green logic cyan build yellow render\n"
                   "orange textures red GPU grey retrace; tap: freeze");
    save(dir, "2a_monitor_summary");
    bs_set_overlay("FRAME TIME 2/5  tap: page\n"
                   "time a frame, budget 33.3 ms:\n"
                   " game    27.0  81% logic 15.2 scene 9.8 sound 2.0\n"
                   " render  29.0  87% textures 0.4\n"
                   " GPU     12.1  36%\n"
                   "waiting: game 5.0 render 2.2\n"
                   "idle: game core 4% render core 6%\n"
                   "tris 1058 draws 137 texture loads 3\n");
    save(dir, "2d_monitor_time");
    bs_set_overlay("DRAW KINDS 3/5  tap: page\n"
                   "a frame:     tris drw  rdp   bld  waste\n"
                   "track         458  56 15.20  3.60   83%\n"
                   "racers        280  36  5.40  2.40   43%\n"
                   "objects       238  28  4.10  1.30   94%\n"
                   "sky            38  10  2.10  0.50   71%\n"
                   "shadows        44   7  1.80  2.00   65%\n"
                   "segments 11/24 (table 17)\n"
                   "objects 13/62 drawn\n");
    save(dir, "2b_monitor_kinds");
    bs_set_overlay("OBJECTS 4/5 FROZEN  tap: page\n"
                   "a frame:    upd vis  tris logic  bld  rdp\n"
                   "racer        8   4   280  9.80 2.40 5.40\n"
                   "scenery     22   5    30  1.20 0.60 2.30\n"
                   "dino whale   1   1   166  0.90 0.30 0.90\n"
                   "banana      14   0     0  1.10 0.00 0.00\n"
                   "exit         2   1    24  0.40 0.10 0.50\n"
                   "weapon       3   2    18  0.60 0.30 0.40\n"
                   "checkpoint  12   0     0  1.20 0.00 0.00\n");
    save(dir, "2c_monitor_objects");
    bs_set_frame_graph(NULL, NULL, 0);
    bs_set_overlay("");

    // The Settings menu: a player's tabs, then a debug build's.
    memset(&menu, 0, sizeof(menu));
    tabs(&menu, 0, 8);
    snprintf(menu.title, sizeof(menu.title), "Picture");
    row(&menu, "Anti-aliasing", "4x", "Smoother edges: each pixel drawn 4 times");
    row(&menu, "Frame rate", "30 fps", "60 when a frame fits (try AA off)");
    row(&menu, "Mipmaps", "On", "No shimmer on distant textures");
    row(&menu, "Colour boost", "Medium", "Richer colours on the 3DS screen");
    menu.picked = 0;
    snprintf(menu.note, sizeof(menu.note), "Tap a value's arrows to change it");
    bs_show_menu(&menu);
    {
        static const char *const sMenuButtons[1] = { "Close" }; /* one page: no paging buttons */
        bs_set_buttons(sMenuButtons, 1);
    }
    save(dir, "3_menu");

    memset(&menu, 0, sizeof(menu));
    tabs(&menu, 2, 10);
    snprintf(menu.title, sizeof(menu.title), "Controls");
    snprintf(menu.pages, sizeof(menu.pages), "1/3");
    row(&menu, "Layout", "Standard", "");
    row(&menu, "A button", "A", "");
    row(&menu, "B button", "B", "");
    row(&menu, "Deadzone", "6%", "");
    row(&menu, "Response curve", "Linear", "");
    menu.picked = 3;
    snprintf(menu.note, sizeof(menu.note), "Tap a value's arrows to change it");
    bs_show_menu(&menu);
    {
        static const char *const sMenuButtons[3] = { "Close", "-Prev", "Next" };
        bs_set_buttons(sMenuButtons, 3);
    }
    save(dir, "4_controls");

    memset(&menu, 0, sizeof(menu));
    tabs(&menu, 9, 10);
    snprintf(menu.title, sizeof(menu.title), "Tools");
    row(&menu, "Profiler", "On", "Frame times on this screen");
    row(&menu, "Check for updates", "", "Install a newer build from GitHub");
    row(&menu, "Send a bug report", "", "Log and snapshot (also SELECT+touch)");
    menu.picked = -1;
    snprintf(menu.note, sizeof(menu.note), port_text("Build %.32s"), plat_build_version());
    bs_show_menu(&menu);
    {
        static const char *const sMenuButtons[1] = { "Close" }; /* one page: no paging buttons */
        bs_set_buttons(sMenuButtons, 1);
    }
    save(dir, "4_tools");

    memset(&menu, 0, sizeof(menu));
    tabs(&menu, 7, 10);
    snprintf(menu.title, sizeof(menu.title), "Saves");
    row(&menu, "Back up now", "", "Saves a dated copy, ghosts included");
    row(&menu, "Saved on", "10-26 23:45", "1 of 3 in sdmc:/3ds/dkr/backup");
    row(&menu, "Tap again to restore", "", "Overwrites your save, then quits");
    menu.picked = 2;
    snprintf(menu.note, sizeof(menu.note), "Saved as N64 dkr.eep and dkr.mpk");
    bs_show_menu(&menu);
    {
        static const char *const sMenuButtons[1] = { "Close" }; /* one page: no paging buttons */
        bs_set_buttons(sMenuButtons, 1);
    }
    save(dir, "4_saves");
    menu.picked = -1;
    tabs(&menu, 7, 8);
    snprintf(menu.rows[2].label, sizeof(menu.rows[2].label), "Restore backup");
    bs_show_menu(&menu);
    save(dir, "4_saves_player");

    bs_show_menu(NULL);
    {
        // The update choices in words a player reads: the build's date, not its branch or commit.
        bs_add_message("Stable: 23 Oct 2026");
        bs_add_message("This build is the newest (23 Oct 2026)"); /* why NEWEST is greyed */
    }
    {
        static const char *const sChoices[3] = { "Stable", "-Newest", "Cancel" };
        bs_set_buttons(sChoices, 3);
    }
    save(dir, "5_update_choices");
    {
        char line[64];
        snprintf(line, sizeof(line), port_text("Downloading the %s build... %d%%"), "stable", 45);
        bs_set_panel_text(line);
    }
    {
        static const char *const sBusy[1] = { "Cancel" }; /* a download stops between chunks; the old build stays */
        bs_set_buttons(sBusy, 1);
    }
    save(dir, "6_busy");
    {
        /* the update installed: the panel says so, the one button closes the game to finish it */
        static const char *const sDone[1] = { "Quit" };
        bs_set_panel_text(port_text("Update installed: quit to finish it"));
        bs_set_buttons(sDone, 1);
    }
    save(dir, "6b_installed");
    {
        static const char *const sConfirm[3] = { "Stop test", "-", "cancel" };
        bs_set_buttons(sConfirm, 3);
    }
    bs_add_message("Stop the test run? The results so far are sent as a report.");
    save(dir, "7_confirm");
    {
        /* a real screen with a button held down: the home bar as SETTINGS is tapped */
        static const char *const sHomePressed[3] = { "*Settings", "Controls", "Updates" };
        bs_set_buttons(sHomePressed, 3);
        bs_clear_messages();
    }
    save(dir, "7b_button_states");
    {
        /* not a screen: a labelled sheet of one button in its states, for review (a bar holds four, so "on" is
           shown on its own sheet next) */
        static const char *const sStates[4] = { "Prev", "^Prev", "*Prev", "-Prev" };
        bs_set_buttons(sStates, 4);
        bs_add_message("State sheet (not a screen): normal, selected, pressed, disabled");
    }
    save(dir, "7c_state_sheet");
    bs_clear_messages();
    {
        static const char *const sOn[2] = { "Prev", "+Prev" };
        bs_set_buttons(sOn, 2);
        bs_add_message("State sheet (not a screen): normal, on");
    }
    save(dir, "7d_state_sheet_on");
    bs_clear_messages();
    bs_show_menu(NULL);
    preview_race(dir);
    if (sDumpDir != NULL) {
        void bs_dump_art(const char *dir);
        bs_dump_art(sDumpDir);
    }
}

/* A race as the mockups showed it ("standings tower"), for the preview. */
static void preview_race(const char *dir) {
    static const signed char sOrder[8] = { 0, 5, 9, 3, 2, 4, 7, 1 }; // Krunch, Banjo, Diddy (the player), Conker...
    static const int sGaps[8] = { 85, 23, 0, -37, -114, -183, -286, -367 };
    static const char *const sButtons[2] = { "Pause", "Settings" };
    RaceHud hud;
    int i;
    memset(&hud, 0, sizeof(hud));
    hud.mapSprite = -1;
    hud.level = 10; // Crescent Island's map, for the looks that show one.
    hud.silverCoins = -1;
    hud.racerCount = 8;
    for (i = 0; i < 8; i++) {
        hud.racers[i].character = sOrder[i];
        hud.racers[i].special = RACE_HUD_NOT_SPECIAL;
        hud.racers[i].player = i == 2;
        hud.racers[i].gap = i == 2 ? RACE_HUD_NO_GAP : sGaps[i];
    }
    hud.position = 3;
    hud.lap = 2;
    hud.laps = 3;
    /* (every time a multiple of 3 frames, so the hundredths add up exactly) lap 2 under way: lap 1 done in 31.00, lap 2 running for 18.75 (the last timed lap is the one being driven) */
    hud.lapTimes[0] = 1860;
    hud.lapTimes[1] = 1125;
    hud.lapsTimed = 2;
    hud.raceTime = 1860 + 1125;
    hud.item = 1;
    hud.itemLevel = 1;
    hud.itemCount = 3;
    hud.bananas = 7;
    bs_set_buttons(sButtons, 2);
    {
        /* round the track in race order (the leader furthest on), you third */
        static const short sDeg[8] = { 150, 128, 112, 96, 72, 48, 24, 0 };
        bs_preview_place_racers(&hud, sDeg);
    }
    bs_set_race_hud(&hud);
    save(dir, "8_race");
    /* the final lap: lap 2 done in 31.30 (the best stays lap 1's 31.00), lap 3 running for 15.10 */
    {
        /* a lap later the field has moved: Krunch pulled away, Banjo closed in, the rest spread out */
        static const int sFinalGaps[8] = { 121, 9, 0, -52, -96, -240, -330, -451 };
        for (i = 0; i < 8; i++) {
            hud.racers[i].gap = i == 2 ? RACE_HUD_NO_GAP : sFinalGaps[i];
        }
    }
    hud.lap = 3;
    hud.lapTimes[1] = 1878;
    hud.lapTimes[2] = 906;
    hud.lapsTimed = 3;
    hud.raceTime = 1860 + 1878 + 906;
    hud.item = getenv("DKR_PREVIEW_ITEM") != NULL ? 0 : -1;
    hud.itemLevel = 0;
    hud.itemCount = 1;
    hud.position = 3;
    {
        static const short sDeg[8] = { 250, 214, 206, 176, 140, 96, 60, 20 };
        bs_preview_place_racers(&hud, sDeg);
    }
    bs_set_race_hud(&hud);
    save(dir, "9_race_final_lap");
    for (i = 0; i < 8; i++) {
        hud.racers[i].gap = i == 2 ? RACE_HUD_NO_GAP : sGaps[i];
    }
    hud.lap = 2;
    hud.lapTimes[1] = 1125;
    hud.lapsTimed = 2;
    hud.raceTime = 1860 + 1125;
    hud.position = 3;
    hud.silverCoins = 5;
    hud.level = 8; /* each screen its own track */
    {
        static const short sDeg[8] = { 150, 128, 112, 96, 72, 48, 24, 0 };
        bs_preview_place_racers(&hud, sDeg);
    }
    bs_set_race_hud(&hud);
    save(dir, "10_silver_coins");
    preview_modes(dir, &hud);
    bs_set_race_hud(NULL);
}

static void set_racer(RaceHudRacer *r, int character, int special, int player, int gap) {
    memset(r, 0, sizeof(*r));
    r->character = character;
    r->special = special;
    r->player = player;
    r->gap = gap;
}

/* The challenge arenas, drawn for the preview as the game's own map pictures would be (a mask of the ground you can
   drive on, 80 x 64, to fill the panel): the level ids it had picked showed a ROM sprite that is no map (a blank square, a blob). */
enum { ARENA_W = 80, ARENA_H = 64 };
static unsigned char sArena[3][ARENA_W * ARENA_H * 4];
static int arena_in(int kind, int x, int y) {
    /* the pixel's centre, in the 64-square arena (drawn 5/4 wide), symmetric about the middle */
    float cx = (x + 0.5f - 40.0f) * 0.8f, cy = y + 0.5f - 32.0f, ax = cx < 0 ? -cx : cx, ay = cy < 0 ? -cy : cy;
    float d2 = cx * cx + cy * cy;
    switch (kind) {
        case 0: { /* Smokey Castle: a walled courtyard track, a tower at each corner, the keep in the middle */
            float tx = ax - 23, ty = ay - 23;
            int ring = (ax <= 25 && ay <= 25) && !(ax <= 15 && ay <= 15), tower = tx * tx + ty * ty <= 36;
            int keep = ax <= 6 && ay <= 6, bridge = ax <= 3 && ay <= 16 && ay >= 6;
            return ring || tower || keep || bridge;
        }
        case 1: { /* Fire Mountain: a ring round the crater, three straight ramps up to the island in its middle */
            static const float sDir[3][2] = { { 0.0f, -1.0f }, { 0.866f, 0.5f }, { -0.866f, 0.5f } };
            int ring = d2 <= 28 * 28 && d2 >= 18 * 18, island = d2 <= 6 * 6, ramp = 0, r;
            for (r = 0; r < 3 && d2 < 19 * 19; r++) {
                float along = cx * sDir[r][0] + cy * sDir[r][1], across = cx * sDir[r][1] - cy * sDir[r][0];
                ramp |= along > 0 && across <= 2.5f && across >= -2.5f; /* 5 px wide */
            }
            return ring || island || ramp;
        }
        default: { /* Icicle Pyramid: a diamond of ice round the pyramid, a ledge on each side of it */
            float m = ax + ay;
            int ring = m <= 30 && m >= 20, pyr = ax <= 8 && ay <= 8;
            int ledge = (ax <= 2 && ay <= 20) || (ay <= 2 && ax <= 20);
            return ring || pyr || ledge;
        }
    }
}
static void arena_map(RaceHud *hud, int kind) {
    static const short sAt[3][4][3] = {
        /* x, y, heading (a quarter turn 0x4000, counter-clockwise from up) per racer, in standings order */
        { { 52, 20, 0x0000 }, { 20, 52, 0x4000 }, { 12, 24, 0x0000 }, { 44, 12, 0xC000 } },
        { { 32, 9, 0x4000 }, { 10, 32, 0x0000 }, { 52, 43, 0xA000 }, { 32, 32, 0 } }, /* on the ring's middle */
        { { 32, 7, 0x4000 }, { 34, 31, 0x2000 }, { 8, 32, 0x8000 }, { 32, 57, 0xC000 } }, /* you on the pyramid, where the arrow fits */
    };
    unsigned char *m = sArena[kind];
    int x, y, i;
    for (y = 0; y < ARENA_H; y++) {
        for (x = 0; x < ARENA_W; x++) {
            unsigned char *o = m + (y * ARENA_W + x) * 4;
            o[0] = o[1] = o[2] = 255;
            o[3] = arena_in(kind, x, y) ? 255 : 0;
        }
    }
    hud->mapSprite = -1;
    hud->mapBitmap = m;
    hud->mapBitmapW = ARENA_W;
    hud->mapBitmapH = ARENA_H;
    hud->mapBitmapId = 100 + kind;
    for (i = 0; i < 4 && i < hud->racerCount; i++) {
        hud->racers[i].at.onMap = !(kind == 2 && hud->racers[i].out);
        hud->racers[i].at.mapX = (40 + (sAt[kind][i][0] - 32) * 5 / 4) * 4; /* the shapes are drawn 5/4 wide */
        hud->racers[i].at.mapY = sAt[kind][i][1] * 4;
        hud->racers[i].at.heading = sAt[kind][i][2];
    }
}

/* The other modes, as the mockups in 3ds/bottom-ui/mode-mockups showed them. */
static void preview_modes(const char *dir, RaceHud *race) {
    static const signed char sChallengers[4] = { 0, 9, 7, 4 }; // Krunch, Diddy (the player), Pipsy, Timber
    static const signed char sScores[4] = { 9, 7, 4, 2 }; // in standings order: you (second in the list) are 2nd
    static const signed char sEggs[4] = { 2, 1, 0, 0 };
    RaceHud hud;
    int i;

    hud = *race;
    hud.mode = RACE_HUD_MODE_TIME_TRIAL;
    hud.silverCoins = -1;
    /* the tables list each lap: laps 1 and 2 done (31.00, 31.30), lap 3 just started (4.50 so far), so no running
       time stands among the finished ones looking faster than the best */
    hud.lap = 3;
    hud.lapTimes[0] = 1860;
    hud.lapTimes[1] = 1878;
    hud.lapTimes[2] = 270;
    hud.lapsTimed = 3;
    hud.raceTime = 1860 + 1878 + 270;
    hud.racerCount = 1;
    set_racer(&hud.racers[0], 9, RACE_HUD_NOT_SPECIAL, 1, RACE_HUD_NO_GAP);
    hud.item = getenv("DKR_PREVIEW_ITEM") != NULL ? 0 : -1;
    hud.itemLevel = 0;
    hud.itemCount = 1;
    hud.record = 5499;
    hud.recordLap = 1788;
    hud.staffTime = 5490;
    hud.bestGap = -42;
    hud.bestLaps[0] = 1890;
    hud.bestLaps[1] = 1869;
    hud.bestLaps[2] = 1851;
    hud.bestLapCount = 3;
    hud.bestTotal = 5610;
    hud.level = 30;
    if (getenv("DKR_PREVIEW_TT_LEVEL") != NULL) {
        hud.level = atoi(getenv("DKR_PREVIEW_TT_LEVEL")); /* host: a contact sheet of the levels' maps */
    }
    {
        static const short sDeg[1] = { 200 };
        bs_preview_place_racers(&hud, sDeg);
    }
    bs_set_race_hud(&hud);
    save(dir, "11_time_trial");

    hud = *race;
    hud.mode = RACE_HUD_MODE_BOSS;
    /* each screen its own race: the boss lap 42.10 then 41.60, the third 12.35 in */
    hud.lap = 3;
    hud.lapTimes[0] = 2526;
    hud.lapTimes[1] = 2496;
    hud.lapTimes[2] = 741;
    hud.lapsTimed = 3;
    hud.raceTime = 2526 + 2496 + 741;
    hud.silverCoins = -1;
    hud.round = 2;
    hud.racerCount = 2;
    set_racer(&hud.racers[0], 9, RACE_HUD_NOT_SPECIAL, 1, RACE_HUD_NO_GAP);
    set_racer(&hud.racers[1], 0, RACE_HUD_TRICKY, 0, -95);
    hud.position = 1;
    hud.level = 20;
    {
        static const short sDeg[2] = { 230, 200 };
        bs_preview_place_racers(&hud, sDeg);
    }
    bs_set_race_hud(&hud);
    save(dir, "12_boss");

    hud.mode = RACE_HUD_MODE_TAJ;
    hud.vehicle = 2;
    /* Taj's short course: 24.80 and 25.15, the third 6.40 in */
    hud.lapTimes[0] = 1488;
    hud.lapTimes[1] = 1509;
    hud.lapTimes[2] = 384;
    hud.raceTime = 1488 + 1509 + 384;
    set_racer(&hud.racers[0], 0, RACE_HUD_TAJ, 0, 140);
    set_racer(&hud.racers[1], 9, RACE_HUD_NOT_SPECIAL, 1, RACE_HUD_NO_GAP);
    hud.position = 2;
    hud.level = 41;
    {
        static const short sDeg[2] = { 120, 90 };
        bs_preview_place_racers(&hud, sDeg);
    }
    bs_set_race_hud(&hud);
    save(dir, "13_taj");

    hud = *race;
    hud.silverCoins = -1;
    hud.laps = 0;
    hud.lapsTimed = 0;
    hud.racerCount = 4;
    for (i = 0; i < 4; i++) {
        set_racer(&hud.racers[i], sChallengers[i], RACE_HUD_NOT_SPECIAL, i == 1, RACE_HUD_NO_GAP);
        hud.racers[i].score = sScores[i];
        hud.racers[i].carrying = i == 1;
        hud.racers[i].character = sChallengers[i];
    }
    hud.mode = RACE_HUD_MODE_BANANAS;
    hud.goal = 10;
    hud.bananas = 2; /* carrying 2 with 7 banked: 9 if banked, still short of the 10 to win */
    hud.level = 0; /* the challenges' own arenas, drawn as maps: Smokey Castle, Fire Mountain, Icicle Pyramid */
    arena_map(&hud, 0);
    bs_set_race_hud(&hud);
    save(dir, "14_bananas");
    hud.mode = RACE_HUD_MODE_EGGS;
    hud.goal = 3;
    for (i = 0; i < 4; i++) {
        hud.racers[i].score = sEggs[i];
    }
    arena_map(&hud, 1);
    bs_set_race_hud(&hud);
    save(dir, "15_eggs");
    hud.mode = RACE_HUD_MODE_BATTLE;
    hud.goal = 0;
    for (i = 0; i < 4; i++) {
        hud.racers[i].score = 3 - i;
        hud.racers[i].out = i == 3;
    }
    hud.racersLeft = 3;
    arena_map(&hud, 2);
    hud.bananas = hud.racers[1].score; // battle health is your bananas: the panel and your row show the same 2
    bs_set_race_hud(&hud);
    save(dir, "16_battle");

    memset(&hud, 0, sizeof(hud));
    hud.mode = RACE_HUD_MODE_HUB;
    hud.mapSprite = -1;
    hud.world = 0;
    hud.balloons = 23;
    for (i = 0; i < RACE_HUD_WORLDS; i++) {
        hud.worldBalloons[i] = i < 2 ? 8 - i : i == 2 ? 3 : 0;
        hud.worldTrophy[i] = i == 0 ? 3 : i == 1 ? 1 : 0;
    }
    hud.worldKeys = 3;
    hud.worldBosses = 3; // the same save as the Adventure menu (18): green = the world's boss beaten
    hud.ttAmulet = 2;
    hud.wizpigAmulet = 1;
    hud.racerCount = 1;
    set_racer(&hud.racers[0], 9, RACE_HUD_NOT_SPECIAL, 1, RACE_HUD_NO_GAP);
    bs_set_race_hud(&hud);
    save(dir, "17_hub");

    // The menus' companions, with the idle buttons.
    {
        static const char *const sHome[3] = { "Settings", "Controls", "Updates" };
        bs_set_buttons(sHome, 3);
    }
    hud.mode = RACE_HUD_MODE_MENU_SAVE;
    hud.world = -1;
    hud.racerCount = 0;
    hud.worldBosses = 3;
    snprintf(hud.name, sizeof(hud.name), "BEN"); /* a player's three letters */
    bs_set_race_hud(&hud);
    save(dir, "18_menu_save");
    memset(&hud, 0, sizeof(hud));
    hud.mode = RACE_HUD_MODE_MENU_TRACK;
    hud.mapSprite = -1;
    hud.level = 10; // Crescent Island.
    hud.record = 5499;
    hud.recordLap = 1788;
    hud.bestTotal = 5610;
    snprintf(hud.name, sizeof(hud.name), "Crescent Island");
    bs_set_race_hud(&hud);
    save(dir, "19_menu_track");
}

static int sRaceFrames;

void host_bottom_race_start(void) {
    static const char *const sButtons[2] = { "Pause", "Settings" };
    bs_init(NULL);
    bs_load_assets();
    if (getenv("DKR_BOTTOM_STYLE") != NULL) {
        bs_set_style(atoi(getenv("DKR_BOTTOM_STYLE")));
    }
    if (getenv("DKR_BOTTOM_VARIANT") != NULL) {
        bs_set_variant(atoi(getenv("DKR_BOTTOM_VARIANT")));
    }
    if (getenv("DKR_BOTTOM_GRID") != NULL) {
        bs_set_grid(1);
    }
    if (getenv("DKR_BOTTOM_SMOOTH") != NULL) {
        bs_set_smooth_art(atoi(getenv("DKR_BOTTOM_SMOOTH")));
    }
    bs_set_buttons(sButtons, 2);
}

/* DKR_BOTTOM_RACE: the race HUD every 60 game frames, as race_NNNN.png. */
void host_bottom_race(const char *dir, const RaceHud *hud) {
    char name[32];
    if (getenv("DKR_BOTTOM_RACE_LOG") != NULL) {
        // The player's map position and heading each frame, to check them against the track's map.
        int i;
        for (i = 0; i < hud->racerCount; i++) {
            if (hud->racers[i].player) {
                printf("player %d %d %u\n", hud->racers[i].at.mapX, hud->racers[i].at.mapY, hud->racers[i].at.heading);
            }
        }
    }
    if (sRaceFrames++ % 60 != 0) {
        return;
    }
    bs_set_race_hud(hud);
    snprintf(name, sizeof(name), "race_%04d", sRaceFrames / 60);
    save(dir, name);
}

/* DKR_SETUP_SHOTS=dir: each page of the first-run setup (port_setup.c) as it changes, as setup_NNN.png. */
void host_bottom_setup(const char *dir, const SetupScreen *screen) {
    static int sStarted, sShots;
    char name[32];
    if (!sStarted) {
        sStarted = 1;
        bs_init(NULL);
        bs_load_assets();
    }
    bs_set_setup(screen);
    if (screen != NULL) {
        snprintf(name, sizeof(name), "setup_%03d", sShots++);
        save(dir, name);
    }
}
