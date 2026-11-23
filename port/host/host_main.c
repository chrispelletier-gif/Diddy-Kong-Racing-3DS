/*
 * Host (PC) frontend used to test the port without 3DS hardware.
 * It runs the game headless with a software renderer that can dump frames to image files.
 *
 * Environment variables:
 *   DKR_DATA_DIR    directory containing baserom.z64 and save files (default: current directory)
 *   DKR_FRAMES      exit after rendering this many frames (default: run forever)
 *   DKR_DUMP_EVERY  write every Nth frame to DKR_DUMP_DIR/frame_XXXXX.ppm (default: 0 = never)
 *   DKR_INPUT       scripted input, see host_input.c
 *   DKR_PROF_TERM_CHECK  check that every profiler page fits the terminal font's 47 columns and 14 lines, then exit
 *   DKR_BENCH_OVERRIDE_CHECK  check that every switch bench race's switches reach the game (port_autorun.c), then exit
 *   DKR_CHECKS_CSV_TEST  check that the self-checks' totals reach the test run's results line (port_autorun.c), then exit
 *   DKR_TTBEST_TEST  check P58's time trial bests index against its files (port_race_hud.c; DKR_DATA_DIR an empty
 *                    folder), then exit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plat/controls.h"
#include "plat/options.h"
#include "plat/plat.h"

void port_boot(void);
int port_autorun_override_check(char *report, int size);
void port_si_flush(void);
int host_frames_rendered(void);
int plat_60fps_setting(void);
void plat_set_60fps_setting(int on);
int camera_option_count(void);
const char *camera_option_label(int i);
void camera_option_value(int i, char *out, int size);
void camera_option_change(int i, int step);
int intro_option_count(void);
const char *intro_option_label(int i);
void intro_option_value(int i, char *out, int size);
void intro_option_change(int i, int step);
const char *intro_option_hint(int i);
int intro_option_in_race(int i);

/* The game menus' VIDEO OPTIONS on PC: the frame rate (the 3DS has its GPU's settings too, port/3ds/menu_3ds.c). */
static int video_count(void) {
    return 1;
}

static const char *video_label(int i) {
    (void) i;
    return "Frame rate";
}

static void video_value(int i, char *out, int size) {
    (void) i;
    snprintf(out, size, "%s", plat_60fps_setting() ? "60 fps" : "30 fps");
}

static void video_change(int i, int step) {
    (void) i;
    (void) step;
    plat_set_60fps_setting(!plat_60fps_setting());
}

/* The settings the game's Options and pause menus show (plat/options.h), so they can be tried on PC. */
static void add_game_menu_options(void) {
    static const OptionsSection sVideo = { video_count, video_label, video_value, video_change, NULL, NULL };
    static const OptionsSection sCamera = { camera_option_count, camera_option_label, camera_option_value,
                                            camera_option_change, NULL, NULL };
    static const OptionsSection sControls = { controls_option_count, controls_option_label, controls_option_value,
                                              controls_option_change, NULL, NULL };
    static const OptionsSection sIntro = { intro_option_count, intro_option_label, intro_option_value,
                                           intro_option_change, intro_option_hint, intro_option_in_race };
    options_add(OPTIONS_VIDEO, &sVideo);
    options_add(OPTIONS_GENERAL, &sIntro);
    options_add(OPTIONS_CAMERA, &sCamera);
    options_add(OPTIONS_CONTROLS, &sControls);
}

static volatile int sQuitRequested;

void plat_request_quit(void) {
    sQuitRequested = 1;
}

int plat_main_loop_iteration(void) {
    static int maxFrames = -1;
    if (maxFrames == -1) {
        const char *env = getenv("DKR_FRAMES");
        maxFrames = env != NULL ? atoi(env) : 0;
    }
    usleep(10000);
    if (sQuitRequested || (maxFrames > 0 && host_frames_rendered() >= maxFrames)) {
        return 0;
    }
    return 1;
}

int prof_term_check(char *report, int size);

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    if (getenv("DKR_PROF_TERM_CHECK") != NULL) {
        /* CI: every profiler page in the terminal font fits the bottom screen (port_profile.c) */
        static char report[32768];
        int failed = prof_term_check(report, (int) sizeof(report));
        fputs(report, stdout);
        return failed;
    }
    if (getenv("DKR_TTBEST_TEST") != NULL) {
        /* CI (P58, rule K6): the time trial bests index and its self-check, broken once on purpose */
        int port_race_hud_ttbest_test(char *report, int size);
        static char report[2048];
        int failed = port_race_hud_ttbest_test(report, (int) sizeof(report));
        fputs(report, stdout);
        printf("time trial bests test: %d failed\n", failed);
        return failed;
    }
    if (getenv("DKR_CHECKS_CSV_TEST") != NULL) {
        /* CI (P59 E, rule K6): the self-checks' totals reach the test run's results line */
        /* a fake check reported right and one reported wrong must reach the cells as chk,2,1,<its name> */
        void port_autorun_result_cells(char *out, int size);
        char cells[160], name[40] = "";
        const char *chk, *first;
        unsigned run = 99, wrong = 99;
        plat_checks_take(&run, &wrong, &first); /* start from nothing */
        plat_check_report("fake check", 0);
        plat_check_report("fake check", 1);
        port_autorun_result_cells(cells, (int) sizeof(cells));
        cells[strcspn(cells, "\n")] = '\0';
        chk = strstr(cells, ",chk,");
        run = wrong = 99;
        if (chk == NULL || sscanf(chk, ",chk,%u,%u,%39[^,]", &run, &wrong, name) != 3 || run != 2 || wrong != 1 ||
            strcmp(name, "fake check") != 0) {
            printf("results checks test: FAILED: cells \"%s\" (want chk,2,1,fake check)\n", cells);
            return 1;
        }
        printf("results checks test: ok (%s)\n", cells);
        return 0;
    }
    if (getenv("DKR_BENCH_OVERRIDE_CHECK") != NULL) {
        /* CI: no switch bench override is turned away by a full table, on either console's list */
        static char report[32768];
        int failed = port_autorun_override_check(report, (int) sizeof(report));
        fputs(report, stdout);
        return failed != 0;
    }
    add_game_menu_options();
    port_boot();
    while (plat_main_loop_iteration()) {
    }
    port_si_flush();
    fprintf(stderr, "Exiting after %d frames\n", host_frames_rendered());
    return 0;
}
