/*
 * The platform (port/src/plat) and profiler functions the 3DS backend calls, for the replay on the PC. Switches
 * (plat_switch) read environment variables like the game's do, with no settings file: the replay runs with each
 * switch's default unless the caller sets it, and plat_old_3ds() is REPLAY_OLD3DS (port/tools/backend_replay.py runs
 * both). Logs go to stderr, never into the replay's output.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat/plat.h"
#include "plat/profile.h"
#include "plat/settings.h"
#include "replay.h"

volatile unsigned int gPlatCounters[PLAT_COUNTER_COUNT];
const char *volatile gPlatGfxStage;
volatile int gPlatSnapshotRequested;
volatile int gPlatSnapshotSaved;
volatile int gPlatSwitchEpoch;

int replay_old_3ds(void) {
    const char *v = getenv("REPLAY_OLD3DS");
    return v != NULL && atoi(v) != 0;
}

void replay_log(const char *fmt, ...) {
    va_list ap;
    if (getenv("REPLAY_QUIET") != NULL) {
        return;
    }
    va_start(ap, fmt);
    fputs("replay: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

int plat_old_3ds(void) {
    return replay_old_3ds();
}

const char *plat_getenv(const char *name) {
    return getenv(name);
}

int plat_switch(const char *name, int fallback) {
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' ? atoi(v) : fallback;
}

int settings_get(const char *key, int fallback) {
    (void) key;
    return fallback;
}
void settings_set(const char *key, int value) {
    (void) key;
    (void) value;
}

static void vlog(const char *fmt, va_list ap) {
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, ap);
    replay_log("%s", line);
}
void plat_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}
void plat_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}
int plat_snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
int plat_log_recent(char *out, int size) {
    if (size > 0) {
        out[0] = '\0';
    }
    return 0;
}
void plat_log_copy_to(const char *path) {
    (void) path;
}

/* A clock that moves 50 us each read: time based choices come out the same from run to run. */
static unsigned long long sNs;
unsigned long long plat_time_ns(void) {
    sNs += 50000;
    return sNs;
}
unsigned long long plat_ticks(void) {
    return plat_time_ns();
}
unsigned long long plat_ticks_per_second(void) {
    return 1000000000ULL;
}
void plat_sleep_ns(unsigned long long ns) {
    (void) ns;
}

const char *plat_data_dir(void) {
    const char *d = getenv("REPLAY_DATA_DIR");
    return d != NULL ? d : "/nonexistent";
}
int plat_save_file(const char *path, const void *data, unsigned int size) {
    (void) path;
    (void) data;
    (void) size;
    return 0;
}
void plat_save_file_later(const char *path, void *data, unsigned int size) {
    (void) path;
    (void) data;
    (void) size;
}
int plat_load_file_start(const char *path, void *data, unsigned int size) {
    (void) path;
    (void) data;
    (void) size;
    return 0;
}
int plat_get_game_note(PlatGameNote *out) {
    (void) out;
    return 0;
}
void plat_format_game_note(const PlatGameNote *note, char *out, int size) {
    (void) note;
    if (size > 0) {
        out[0] = '\0';
    }
}

/* The settings the game would read from settings.txt: their defaults (plat_common.c). */
static int sFps60 = -1;
int plat_60fps_setting(void) {
    if (sFps60 < 0) {
        sFps60 = plat_switch("DKR_60FPS", 0) != 0;
    }
    return sFps60;
}
void plat_set_60fps_setting(int on) {
    sFps60 = on;
}
int plat_frame_retraces(void) {
    int pinned = plat_switch("DKR_FPS_PIN", 0);
    if (pinned == 20 || pinned == 30 || pinned == 60) {
        return 60 / pinned;
    }
    return plat_60fps_setting() ? 1 : 2;
}
int plat_frame_step(void) {
    return plat_frame_retraces();
}
float plat_camera_near(void) {
    return plat_aspect_scale() > 1.0f ? 2.5f : 10.0f;
}
int plat_gpu_maths_setting(void) {
    return GFX_SURVEY_DEFAULT ? 3 : 0;
}
int plat_stereo_once_setting(void) {
    return 1;
}

void plat_3ds_quit_point(int parkedState) {
    (void) parkedState;
}
int plat_3ds_renderer_should_park(void) {
    return 0;
}

/* The two-core renderer: on the 3DS the backend runs on its own core (DKR_SPLIT_RENDER, on by default). */
int gfx_split_active(void) {
    return plat_switch("REPLAY_SPLIT", 1);
}

/*
 * The two-core renderer's call records (gfx_split.c), as DKR_WALK_VERTS (PR #563) queues a draw the walk made: here
 * the record runs as soon as it is committed, on the one thread, in the order the walk made it. Records over a
 * quarter of the 3DS's ring are refused, as there.
 */
#define REPLAY_CALL_BYTES (256 * 1024)
static void (*sCallFn)(void *);
static unsigned long long sCallRoom[REPLAY_CALL_BYTES / 8];

void *gfx_split_call_reserve(void (*fn)(void *), unsigned int bytes) {
    if (bytes > REPLAY_CALL_BYTES) {
        return NULL;
    }
    sCallFn = fn;
    return sCallRoom;
}

void gfx_split_call_commit(void *payload) {
    sCallFn(payload);
}

void gfx_split_call_commit_shrink(void *payload, unsigned int bytes) {
    (void) bytes;
    sCallFn(payload);
}

void bugreport_request(void) {
}

/* The profiler is off: nothing is timed. */
int prof_enabled(void) {
    return 0;
}
int prof_recording(void) {
    return 0;
}
void prof_add(int stage, unsigned long long ns) {
    (void) stage;
    (void) ns;
}
void prof_part(int part, unsigned long long ns) {
    (void) part;
    (void) ns;
}
unsigned long long prof_clock_read_cost(void) {
    return 0;
}
const char *prof_kind_name(int kind) {
    (void) kind;
    return "kind";
}

/* P56 K2 (#567): the backend reads the CPU counters at frame start on the renderer's core; none here. */
int plat_pmu_on(void) {
    return 0;
}
void plat_pmu_frame(int slot) {
    (void) slot;
}

/* P59 C: no bottom screen in the replay */
void plat_screen_hang_info(char *out, int size) {
    snprintf(out, (size_t) size, "  bottom screen: n/a\n");
}

/* P59 E: the self-checks' totals are the test run's; the replay only reports them */
void plat_check_report(const char *name, int wrong) {
    (void) name;
    (void) wrong;
}
