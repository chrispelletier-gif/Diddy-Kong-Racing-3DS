# Diddy Kong Racing for Nintendo 3DS

A native port of the Diddy Kong Racing decompilation to the Nintendo 3DS. The game code is the decompiled C from
this repository, compiled for ARM. The N64 hardware it expects is provided by a small emulation layer:

- the libultra OS calls, mapped onto threads
- ROM DMA and saves
- an interpreter for the F3DDKR display lists, drawn with the 3DS GPU
- a C version of the RSP audio microcode

Nothing from the game is distributed. The port reads every asset from your own ROM at runtime.

This file is for developers: building, the debug build's tools, how the port works and how it is tested. Players
should read the [main README](../README.md) instead, which covers installing and playing a release.

## What you need

- A Diddy Kong Racing ROM. The US 1.0 version (`baserom.us.v77.z64`, SHA1
  `0cb115d8716dbbc2922fda38e533b9fe63bb9670`) is the one that is tested. `.z64`, `.n64` and `.v64` byte orders all
  work.
- To build: [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS packages
  (`dkp-pacman -S 3ds-dev`, which includes devkitARM, libctru, citro3d, picasso and 3dsxtool).
- To play: a 3DS running homebrew (the Homebrew Launcher), or the Azahar emulator. A New 3DS is recommended; on a
  New 3DS the renderer runs on its own CPU core.

## Building

The build needs `include/asset_enums.h`, which the decomp's asset tool generates from your ROM:

```sh
cp /path/to/your/rom.z64 baseroms/baserom.us.v77.z64
make -C tools dkr_assets_tool
./tools/dkr_assets_tool extract -dkrv us.v77
make -C port TARGET=3ds -j8
```

The result is `port/build/3ds/dkr.3dsx`, a debug build: its Settings menu has the profiler, the test runs, bug
reports, updates and the performance switches (render ahead, edge trimming, the sound engine's
switches). `make -C port TARGET=3ds RELEASE=1` builds the one for players, `port/build/3ds-release/dkr.3dsx`, whose
menus offer only the settings a player needs; the hidden switches keep their defaults. The debug tools are left out of
it altogether (`PLAT_DEBUG_TOOLS` in `port/src/plat/plat.h`, `port/src/release/`): no profiler, test runs, bug reports,
updater or reference renderer, so it links without curl, mbedTLS and zlib and is about a third smaller. A crash still
writes `sdmc:/3ds/dkr/crash.txt`. CI builds both; the updater installs debug builds.

## Installing

Installing a release, the ROM, the DSP firmware, saves, the optional SD portraits, the controls and the player
settings are described for players in the [main README](../README.md). For a build of your own, copy
`port/build/3ds/dkr.3dsx` (or `3ds-release/dkr.3dsx`) to `sdmc:/3ds/dkr.3dsx`, or send it with `3dslink`. The port
looks for the ROM as `sdmc:/3ds/dkr/baserom.z64`, then any `.z64`, `.n64` or `.v64` file in `sdmc:/3ds/dkr/`,
`sdmc:/3ds/` and the root of the SD card (`find_rom` in `src/port_main.c`), and says on the top screen when it finds
none.

Saves go to `sdmc:/3ds/dkr/` as `dkr.eep` (the cartridge EEPROM) and `dkr.mpk` (a virtual Controller Pak for ghosts
and copied saves), byte for byte the `.eep` and `.mpk` files of a real cartridge and N64 emulators. In memory they
are in the order the game's code reads; `src/os/save_convert.c` converts them on load and save, and has the layout.
Builds before that kept `eeprom.bin` and `controllerpak.bin` in memory order; a newer build converts them once and
leaves the old files alone. Settings > Saves (`3ds/saves_3ds.c`) writes dated backups to `sdmc:/3ds/dkr/backup/` and
restores any folder there holding an `.eep` and/or `.mpk`; a restore keeps the replaced save as a `-undo` backup and
closes the game, which still holds the old progress. `tools/n64_save_check.py` checks files against the N64 layout.

## Controls

The default layout is in the [main README](../README.md#playing). Remapping, the presets and the circle pad's deadzone,
sensitivity and response curve are in `src/plat/controls.c` and saved in `sdmc:/3ds/dkr/controls.txt` (`version=2`
moved ZL from Z to Look back). START, the D-Pad and the C-Stick are fixed. The look-back button is a port-only pad bit
that `src/os/os_si.c` strips before the game sees the controller.

The touch screen does nothing outside its buttons, so a stray tap can't interrupt a race. Debug build only: Stop test
while a test run is on, and SELECT + touch sends a bug report.

## Race HUD on the bottom screen

The race HUD is drawn by `src/plat/bottom_screen.c` from the state `src/game/port_race_hud.c` fills in each frame
(`src/plat/race_hud.h`); `src/game_ui.c` (the game's own, under `TARGET_PORT`) leaves off the top screen what it
replaces (`port_race_hud_hides`). There is a layout for every mode (race, time trial, boss, Taj, the challenges, hub)
and for the title screen, file select and Tracks menu. It uses only the game's own HUD art from the ROM; `racehud=` is
1 Bottom (default), 2 Both or 0 Top. The profiler's readout (debug build) takes its place while it is on.

Boss and Taj portraits come from `sdmc:/3ds/dkr/portraits/NAME.png` (40x40 PNG, loaded with stb_image); anything
else is skipped with a log line and the racer gets a box with its initial. `port/tools/portraits` makes a set from
the game's models in your ROM. Keep them out of the repository: they are Rare's characters.

The game is one player only, and DOUBLEVISION and JOINTVENTURE are answered like wrong codes. The 2 to 4 player code
is still there for local wireless play later (roadmap O9): `make -C port TARGET=3ds PORT_MULTIPLAYER=1` brings it
back. Test runs and CI never play multiplayer.

## Settings

Settings are saved in `sdmc:/3ds/dkr/settings.txt` (`src/plat/settings.c`). The player settings (picture, screen,
race HUD, camera, controls, sound, start-up) are listed in the [main README](../README.md#playing). They are in the
bottom screen's Settings menu and in the game's own menus: an INTRO LOGOS row and VIDEO, SCREEN, CAMERA and CONTROL
OPTIONS in Options, MORE AUDIO OPTIONS in Audio Options, and an OPTIONS row in the pause menu without the settings
that only apply after a restart (`src/menu.c`, categories registered in `src/plat/options.c`). Both change the same
saved values. The game's menus show the port's text in French when the game is set to FRANCAIS
(`src/game/port_text.c`): a new row's label, values and hint each need a line there, or they show in English. The
bottom screen stays English.

The bottom screen is drawn in the game's own style (`src/plat/bottom_screen.c`, menu in `3ds/menu_3ds.c`), with its
fonts, logo, arrows and menu sounds read from the ROM; until the ROM is loaded it uses a plain 8x8 font. Other parts
of the port add a page with `menu_3ds_add_page` (`3ds/menu_3ds.h`): its rows are a label, a value and a change
function.

### Debug build only

`make RELEASE=1` hides these rows and keeps their defaults; a value for them in `settings.txt` is ignored. Any new
debug or performance row must be added to the hidden lists in `3ds/menu_3ds.c` and to `is_debug_switch` in
`src/plat/settings.c`.
- **Mipmaps: Show**: mipmaps on, with the levels tinted red, green, blue and yellow, to see where the GPU uses them.
- **Render ahead**: 1 frame (the N64's timing: the game builds a frame while the renderer draws the one before, and
  waits for it) or 2 frames (the game runs one frame further ahead, so a slow frame in the renderer is absorbed
  instead of dropped, at the cost of one frame more input lag; the game keeps a third display list heap for it).
  `DKR_RENDER_AHEAD=2` in `env.txt` tries it without the menu; it also works on the host build.
- **Edge trimming**: clip triangles to the GPU's guard band (on by default).
- **Profiler**: frame times on the bottom screen (see Profiler).
- **Test run**: start a full test, a quick test, the boss fights, the GPU bench, the switch bench or the whole game run
  (full or short), or stop the one running.
- **Sound** (roadmap Phase 7; the player build keeps only Smooth resampling):
  - *Sound engine*: DSP voices (default) plays every voice of the game's music and sound effects on one of the 3DS
    DSP's 24 channels, which decode, resample, envelope and mix it; the CPU only runs the game's sequencer and the
    N64 reverbs (see Sound below). CPU mixer runs the N64 audio microcode on the CPU instead, as before. Applies on
    the next start. The mixing core, rate and buffer rows only apply to the CPU mixer; with the DSP voices they
    read "CPU mixer only".
  - *Mixing core*: Renderer (default) mixes the sound next to the renderer. System mixes it on the 3DS's system
    core instead, with the application's share of it raised to 80% and below the system's own services there
    (the first try, at the renderer's priority, held up the GPU's service and doubled frame times). Applies on the
    next start.
  - *Mixer rate*: 22 kHz (the game's own) or 32 kHz, which keeps more of the high frequencies when sounds are
    pitched up, for about 45% more mixing work. Pitches and reverb lengths are scaled to sound the same. Applies on
    the next start.
  - *Sound buffer*: Large keeps 50 ms more sound queued, so a late audio frame doesn't leave a gap, at the cost of
    the sound coming 50 ms later.
- **Check for updates** and **Send a bug report** (see below).

## How it works

| Path                       | What it does |
| -------------------------- | ------------ |
| `src/os/`                  | libultra replacement: threads, message queues, events, timers, PI (ROM), SI (controllers, EEPROM, Controller Pak), VI, AI and the RCP. Every N64 thread is a real thread; on the 3DS they all share one core with strict priorities, as on the N64. |
| `src/game/asset_swap.c`    | The ROM is big endian, so each asset is converted to native byte order as it is loaded. |
| `src/game/`                | Other per-port game support: C versions of the few hand-written assembly routines, and zlib inflate. |
| `src/gfx/gfx_rdp.c`        | F3DDKR display list interpreter. Transforms vertices on the CPU, decodes and caches textures, decodes the RDP colour combiner and blender. |
| `3ds/gfx_c3d.c`            | citro3d backend. Compiles the combiner to TEV stages. |
| `host/gfx_soft.c`          | Software rasteriser backend for the host build. |
| `src/audio/audio_rsp.c`    | C implementation of the aspMain audio microcode (the CPU mixer). The resampler table is read from the ROM. |
| `src/audio/voices.c`       | The DSP voices: the synthesizer's voice changes as DSP channel settings, the samples decoded, the N64 reverbs on the aux buses (see Sound). |
| `3ds/`                     | Other 3DS frontend parts: input, ndsp audio output, the main loop. |
| `host/`                    | Other host frontend parts. |

Changes to the game code itself are all under `#ifdef TARGET_PORT` or `#ifdef AVOID_UB`. A handful of original bugs
were harmless on the N64 but corrupt memory with a different layout (out of bounds writes and reads); those are fixed
under `AVOID_UB`.

## Host build (for development)

`make -C port TARGET=host` builds a headless Linux version (32 bit, so `gcc-multilib` is needed). It runs the same
game and interpreter with the software renderer, which makes debugging much easier. It is configured with
environment variables:

| Variable            | Effect |
| ------------------- | ------ |
| `DKR_DATA_DIR`      | Directory with `baserom.z64` and the saves (default: current directory). |
| `DKR_FRAMES`        | Exit after this many frames. |
| `DKR_DUMP_EVERY`    | Write every Nth frame to `DKR_DUMP_DIR/frame_NNNNN.png`. |
| `DKR_INPUT`         | Scripted input, e.g. `"700:START 710: 900:A 910: 1250:STICK=0,-80 1262:"`: each step holds the listed buttons from that poll number until the next step. |
| `DKR_WALK_CENSUS=1` | Counts the display list walk's commands per frame by kind and class (records, state, vertex-slot draws, texture ops, matrices, list calls, raw N64). On the host also writes `census.csv` with the game's `file:line` for each; `port/tools/census_run.py` plays the scenarios in `port/host/scenarios/` and `port/tools/census_report.py` turns the CSV into the to-do lists for the native-renderer stages. On the 3DS it adds a `census` line to the profiler log. |
| `DKR_DUMP_AUDIO`    | Write the audio output to this file as raw 16 bit stereo PCM (22050 Hz with the CPU mixer, 32728 Hz with the DSP voices). |
| `DKR_AUDIO_DSP`     | `0` runs the CPU mixer instead of the DSP voices (see Sound). |
| `DKR_TRACE_FRAME`   | Log every display list command and draw of this frame. |
| `DKR_BOTTOM_PREVIEW`| Draw the 3DS bottom screen's views (home, profiler, menu pages, updater) to PNG files in this directory and exit. `DKR_MONO_FONT` can name a copy of libctru's 8x8 font (2048 bytes) for the dense text. |
| `DKR_BOTTOM_TIME`   | With `DKR_BOTTOM_PREVIEW`: also time 50 redraws of each area of every view and print `TIME <view> areaN us` lines (`DKR_BOTTOM_ONLY=name`: 400 of the views whose name holds it, none of the rest). |
| `DKR_BOTTOM_RACE`   | Draw the 3DS bottom screen's race HUD to `race_NNNN.png` in this directory every 60 frames of a race, and leave those HUD pieces off the top screen as the 3DS does (`DKR_RACE_HUD=2` keeps them on both). `DKR_BOTTOM_RACE_LOG=1` also prints the player's map position and heading each frame. |

`make -C port TARGET=host DEBUG=asan BUILD_DIR=build/asan` builds with AddressSanitizer, which is how the original
memory bugs above were found.

## Graphics

The top screen is anti-aliased by default: frames are drawn at twice the resolution in each direction and
downsampled when they are sent to the screen. It only costs GPU time (the CPU work is unchanged). Turn it off in the
Settings menu (until it has been set there, `DKR_AA=0` in `sdmc:/3ds/dkr/env.txt` also turns it off).

Mipmaps (on by default) stop distant track textures from shimmering: every texture that is opaque or repeats both
ways (ground, walls, cliffs, water) also gets smaller, pre-filtered copies when it is loaded, and the GPU reads the one that
fits the distance. That takes a third more memory for those textures. Clamped textures with transparency (shadows,
start lines, track borders) and point sampled ones (text) are left as they were. Textures clamped on both axes, which
are drawn once rather than repeated (the signs with track names, door numbers, banners, faces), switch to each smaller
copy one step later, so their writing stays readable; `DKR_MIP_SIGN_BIAS` in `env.txt` changes how many steps (0 as
before, -1 the default, -2 sharper still).

Colour boost (off by default) makes colours more saturated, as the 3DS screen looks washed out next to a CRT. It is
done in the GPU's colour combiner for every draw, so it costs no time; greys, black and white stay as they were.
High is c + 0.5 (c - grey), Medium 0.3 and Low 0.15.

The host build can preview both: `DKR_MIPMAP=1` and `DKR_COLOUR=1` to `3` (both off by default there, so frames stay
comparable between commits). It picks mipmaps as the 3DS does without anti-aliasing; `DKR_MIP_AA=1` picks them as with
it, and `DKR_MIP_SIGN_BIAS` works there too.

The Settings menu's Screen page has the top screen's modes, all on by default:
- Widescreen fills the 400 pixel wide screen instead of a 4:3 picture in the middle. The game's camera sees more at
  the sides; the HUD, text and menus keep their 4:3 shape and place in the middle, except what reaches the edge of the
  N64 screen (fades, backgrounds, full screen fills), which is stretched to the edge.
- 800 pixels shows the screen's 800x240 mode while 3D is off (not on an original 2DS). The picture is drawn 800
  pixels wide, so it costs the GPU what anti-aliasing does; with anti-aliasing on it is the same 800x480 picture as
  before, only halved in height, so the extra detail across reaches the screen. Azahar doesn't show this mode (it
  shows the left half of the picture), so turn it off there.
- Stereo 3D follows the 3D slider. Each frame's GPU commands run twice, once for each eye with the picture moved
  across by an amount that grows with distance (the vertex shader's one addition), and the HUD sits at the depth of
  the screen. Anti-aliasing and the 800 pixel mode are off while the slider is up. `DKR_STEREO_SEP` (how far apart
  the eyes' pictures are for the farthest things, in thousandths of the half screen width, default 25) and
  `DKR_STEREO_CONV` (the distance in front of the camera that stays at the screen's depth, default 130, about where
  the kart is) in `env.txt` tune it.

The host build previews widescreen with `DKR_WIDESCREEN=1` (frames are then 400 pixels wide).

In Azahar you can also raise the internal resolution in the graphics settings; the port renders on the 3DS GPU, so
that applies to it as well.

## Debug settings on the 3DS (debug build only)

The start-up is always logged to `sdmc:/3ds/dkr/log.txt`, and its progress is shown on the bottom screen, so if
the game does not start, the last line says how far it got. Create `sdmc:/3ds/dkr/debug` (an empty file) to keep
logging after start-up. Settings go in
`sdmc:/3ds/dkr/env.txt` as `NAME=VALUE` lines. `DKR_TRACE_FRAME` and `DKR_INPUT` (which replaces the real
controls) work there, as do these renderer switches:
- `DKR_NO_CULL=1`
- `DKR_NO_SCISSOR=1`
- `DKR_NO_FOG=1`
- `DKR_VERTEX_FOG=1` (the old per vertex fog, for comparison)
- `DKR_FOG_SKIP=1` (roadmap P60 C: the walk works out a fogged vertex's fog factor, a divide each, only where the
  draw's combiner reads the shade alpha; the GPU fogs per pixel, so frames are the same. On by default on an Old 3DS
  (bench run 50), `DKR_FOG_SKIP=0` for the old way. Off with `DKR_VERTEX_FOG=1`.
  Logs `fog skip: N fog factors left out` every 600 frames)
- Old 3DS defaults from bench run 50 (2026-11-22), each `=0` for the old way: `DKR_RAW_REGS` (each draw's GPU
  settings, uniforms and draw command as raw register words made once per draw state; off in stereo 3D;
  `DKR_RAW_REGS_CHECK=1` compares every reuse with citro3d), `DKR_NATIVE_GPU` (1: the karts' vertex maths on the GPU;
  2 adds the objects) and `DKR_VERTEX_ASM` (the native draws' vertex passes as one VFP loop; `DKR_VERTEX_ASM_CHECK=1`
  compares it with the C). The checks count in the test run results' chk cells.
- `DKR_GFX_TEST=1` (draws a reference quad)
- `DKR_STATE_CACHE=0` (sends every GPU setting for every batch again, instead of only the ones that changed)
- `DKR_STATE_CACHE=2` (only the settings that changed, even when the texture changes; this hung the GPU on hardware)
- `DKR_VERTEX_FORMAT=48` (writes every vertex as 48 bytes of floats again, instead of 28 bytes with byte colours and
  the second texture's coordinates only when a draw uses one)
- `DKR_INDEXED=0` (writes a vertex for every triangle corner again, instead of one for each vertex that the
  triangles of a command share, drawn by index; about half the corners in the game's models are such repeats)
- `DKR_TEX_NATIVE=0` (stores every texture on the GPU as 32 bit RGBA8 again, instead of the 16 and 8 bit formats that hold it exactly)
- `DKR_MIPMAP=0` (never builds mipmaps, so they take no memory; the Settings switch then does nothing)
- `DKR_MIP_FILTER=0` (reads the nearest mipmap instead of blending the two nearest)
- `DKR_SINGLE_CORE=1` (on a New 3DS, keeps the renderer on the main core as on an Old 3DS, and the sound with it)
- `DKR_AUDIO_DSP` and `DKR_DUMP_AUDIO` (see the host table above) work here too; the dump file goes inside `sdmc:/3ds/dkr/`
- `DKR_AUDIO_DEBUG=1` (with the DSP voices, logs how many voices play and how late their changes were applied)
- `DKR_AUDIO_NOFX=1` (with the DSP voices, no reverb, for comparison)
- `DKR_AUDIO_SIMD=0` (mixes the sound with the plain C loops instead of the ARM SIMD ones; the sound is the same)
- `DKR_AUDIO_RATE=n` (runs the mixer at n Hz, whatever the Sound page says; also works on the host build)
- `DKR_SYSCORE_LIMIT=n` (the share of the system core, in percent, asked for when the sound is mixed there; 80)
- `DKR_SYSCORE_PRIO=n` (the sound thread's priority on the system core, 0x18 highest to 0x3F lowest; 0x30)

## Bug reports (debug build only)

The 3DS build can send a bug report over Wi-Fi as an issue on this repository, labelled `3ds-bug-report`:
- Tap Settings, then Send a bug report, or hold SELECT and touch the screen anywhere. The game saves a
  snapshot of the frame and sends it with the log. This also works when the game has frozen, without the snapshot
  if the renderer has stopped.
- If the game crashes it writes `sdmc:/3ds/dkr/crash.txt` (the registers, the stack and the recent log) and goes
  back to the Homebrew Launcher. The next time it starts, and also after a freeze that needed the power button, it
  offers to send the last run's log and crash report (A sends, B skips).

It needs a GitHub token that may only create issues, in `sdmc:/3ds/dkr/bugreport.txt`:
1. On GitHub open Settings, Developer settings, Personal access tokens, Fine-grained tokens, and choose
   Generate new token.
2. Name it (for example "DKR 3DS bug reports") and pick an expiry date.
3. Under Repository access choose Only select repositories, and pick this repository.
4. Under Permissions, Repository permissions, set Issues to Read and write. Leave everything else as it is.
5. Generate the token and copy it (it starts with `github_pat_`).
6. Create `sdmc:/3ds/dkr/bugreport.txt` containing one line: `token=github_pat_...`. A `repo=owner/name` line sends
   the reports to another repository.

The token never leaves the console except to GitHub, and is not part of the reports. A report that cannot be sent
is kept as `sdmc:/3ds/dkr/bugreport_unsent.txt`.

The issue body has the build, the console, the counters and the end of the log. The files (the snapshot's
`diff_N_ref.png`, `diff_N_gpu.bin` and `diff_N_log.txt`, `log.txt`, `env.txt`, or `last_log.txt` and
`last_crash.txt` after a crash) are posted as compressed comments on the issue. `log.txt` stops after the start-up
lines; the body has the latest ones. When the GPU stops responding, the game saves `gpu_hang.txt` (the GPU's command
queue, marking the command list it stopped in, and every draw of the stuck frame, those in that list starred) and
`gpu_hang_cmd.bin` (the frame's command list), and 20 seconds into any hang `hang_log.txt` (the latest log lines).
`gpu_hang.txt` also has a "GPU and system state at the hang" section (`DKR_GPU_HANG_INFO`, default 1): the GPU's busy
register (P3D, PPF, PSC0/1) and its P3D interrupt and command list registers, each read twice 100 ms apart (an idle P3D
with the queue entry not done points to a lost interrupt, a busy one to a GPU really stuck), the renderer's last cache
flush, what the bottom screen was doing (the area it was drawing and for how long, its last draw and flush, its locks,
the profiler's page and last tap) and each watched thread's core and priority. `DKR_GPU_HANG_TEST=1` writes the file
once at frame 120 with no hang, to see the section (it then won't write a real hang's in that session).
They go with a report sent right away, or as `last_*` files with the report on the next launch.

A debug build also keeps a flight recorder going all the time (`port/src/plat/recorder.h`), whether the Profiler row
is on or not: a row for each of the last 1024 game frames (the game thread's stages, the renderer's, texture decodes,
the GPU's time, triangles, sound gaps, and what the game was doing), the last 512 events (every log line, settings
changes, every SD card read and write with how long it took, the watchdog's counters, the moment the report was asked
for) and the last 256 hitches (frames half as long again as the usual pace) with what took the time and what happened
then. Every report attaches it as `timeline.csv` and sums it up in the body (the slowest frames and their causes), with
the game's state in words and every saved setting. A crash or a stall of 3 seconds saves it too, and the next
launch's report has it as `last_timeline.csv`. The test runs and benches write every race's hitches to
`autorun_hitches.txt` and list the run's worst in their issue. To get them back:

```sh
python3 port/tools/bugreport_fetch.py --list        # open reports
python3 port/tools/bugreport_fetch.py 12             # saves issue 12 to bugreport_12/
python3 port/tools/diff_frames.py bugreport_12 bugreport_12/diff
python3 port/tools/timeline_report.py bugreport_12/timeline.csv   # the frames around the press, the slowest ones
```

On the host build, `DKR_REC_TEST=n` presses the shortcut at game frame n and writes `rec_timeline.csv`,
`rec_summary.txt` and `rec_game.txt` to the data directory 30 frames later.

A crash report gives `pc` and `lr`; with the `dkr.elf` of the same build (the `Build` line is its commit),
`arm-none-eabi-addr2line -f -e dkr.elf <pc> <lr>` names the functions.

## Updates

CI publishes every 3DS build as a pre-release: `3ds-newest` is the debug build of the latest push to any branch,
`3ds-master-debug` the debug build of master, and `3ds-master` the player build of master (the one to download; it has
no Check for updates button). Public releases are separate, fixed releases tagged by version (`3ds-v0.1...`),
with the player build's `.3dsx` and `.cia`; they don't change when master does. The updater only installs the two
debug builds. Tap Settings, then Check for updates; the game lists both builds (the
one running is marked "this") and offers master, newest or cancel. The chosen build is downloaded next to the
running `.3dsx`, checked against its size and sha256, and replaces it; tap Quit to finish the update, or keep
playing and the new build runs next time. It only works when the game was started from the SD card (not over
3dslink).

While the repository is public no token is needed. For a private repository the token in
`sdmc:/3ds/dkr/bugreport.txt` is used, and it needs Contents set to Read-only as well (step 4 above).

## Sound

By default the sound plays on the 3DS DSP as the system's own games do (roadmap Phase 7, "DSP voices"). The game's
sequence players and sound player run unchanged on the N64 synthesizer driver (libultra's `alAudioFrame`), which
allocates physical voices and queues their parameter changes (start, stop, volume, pan, pitch, effect mix) with
sample-accurate times. On the N64 the driver then builds a command list for the RSP, which the port runs on the CPU
(`src/audio/audio_rsp.c`). With the DSP voices no command list is built: each of the 24 physical voices is one DSP
channel (`src/audio/voices.c`), and its changes become channel settings, applied by the DSP's own frame clock (every
160 samples, 4.9 ms) so they keep their timing. The DSP decodes, resamples, envelopes and mixes every voice and
outputs at its own rate (about 32.7 kHz), so nothing is downsampled to 22 kHz on the way. The samples are decoded
from the ROM's VADPCM to 16 bit PCM once at start-up (12 MB of linear memory, about a second).
The HOME menu and sleep unload the DSP; when it is back the engine restarts its looping voices from their loops
and drops the one-shot sounds that were playing, since the firmware does not reliably carry the channels over.

The reverbs are the N64's own: the game's two effect buses (its custom one and the "big room") are the DSP's two
aux buses, and the voices' effect sends come back to the CPU every frame, where the same delay networks the RSP ran
(`alFxPull`) are applied as plain arithmetic (`pv_aux_process`) before the DSP mixes the returns in. libctru does not
expose that aux path, so `3ds/audio_ndsp.c` reads and writes the DSP's shared memory region for it directly.

What differs from the N64: the DSP's resampler, envelope steps every 4.9 ms instead of every 8 samples, 24 physical
voices instead of 40 (the driver steals the quietest, as it did when the N64 ran out), and the reverb's chorus taps
use linear interpolation. The CPU mixer stays as a Settings > Sound fallback. The host build has a software
stand-in for the channels (`host/audio_soft.c`), so `DKR_DUMP_AUDIO` records the same engine on a PC.

### GS hang ladder (debug build only)

`DKR_GS_LADDER=1` in `env.txt` (P65 rank 4) runs, before the game starts, six steps of 30 frames each that find what
makes the PICA geometry shader freeze a console (the sprite geometry shader froze an Old 3DS in bench run 50): (1) the
geometry shader's program alone, (2) a geometry shader draw then ordinary draws, (3) ordinary draws then a geometry
shader draw, (4) 20 program switches a frame, (5) ordinary draws through a pass-through geometry shader (a control),
(6) 20 switches with that pass-through on the ordinary program too (citro3d issue #56's workaround). Each step writes
`gs_ladder_N.txt` (`testing`, then `passed`) before it draws, so after a freeze, power the console off and start it
again: the step that froze is recorded as hung and the ladder goes on with the next one. A GPU stall the console
survives is written `gpu-hung`. The answers are on the bottom screen for a moment, in the log (`GS ladder: 1 passed,
...`) and in `gs_ladder.txt`. Steps already answered are not run again; `DKR_GS_LADDER=2` runs them all again. Remove
the line from `env.txt` when done.

## Profiler (debug build only)

Turn Profiler on in the Settings menu to show the profiler on the bottom screen, updated every second (it appears
when the menu closes); the game remembers the setting. `DKR_PROFILE=1` in `env.txt` and the test run
below turn it on as well. While it is off it costs nothing measurable (each timing hook returns after one check).

| Line | Meaning |
| ---- | ------- |
| `PROFILER 29.8 fps limit: GPU` | Frames per second, and which part limits them: the game core, the renderer core or the GPU, or the retrace when all three have time to spare. |
| `30fps 97% 20fps 3% slower 0%` | Share of frames shown at 30 fps, at 20 fps, and slower. |
| `worst frame 61 ms hitches 2` | The longest frame, and frames over 50 ms. |
| `game logic / scene / wait` | Game thread, ms per frame: object updates (racers, AI), building the display list, waiting for the renderer. |
| `rend rdp / tex / gpuwait` | Renderer thread: interpreting the display list and sending it to the GPU, decoding textures, waiting for the GPU. |
| `gpu draw / audio / idle` | The GPU's drawing time, the audio mixing, and how much of the main core is idle. |
| `tris / draws / tex+` | Triangles and draw batches per frame, textures decoded in the last second. |

On a New 3DS the game thread, the renderer and the GPU work in parallel, so whichever of `logic + scene`,
`rdp + tex` and `gpu draw` is largest sets the frame rate (33.3 ms per frame is 30 fps). Every few seconds a summary
line also goes to the log, followed on the 3DS by a `renderer parts` line that splits `rdp` into the backend's draw
calls (what is left is the display list walk) and, within them, the guard band clipping, the GPU settings and the
vertex writes, in ms per frame.

On an Old 3DS the renderer's display list walk runs on the game's core, above the game, so it interrupts the game
while it builds a frame. That time is shown apart from `scene` (and `logic`) as `+ renderer on this core` under the
`game` line, `shared` in the log line and `shared_core_ms` in the test run's results, so `scene` is the game's own
work; the `game` total and the `held back by` answer still count it, as it is the game core's load. A New 3DS's walk
has a core of its own, so nothing changes there. `DKR_PROF_SHARED_CORE=0` in `env.txt` counts it in `scene` again,
as before 2026-11-15 (older Old 3DS results' `scene_ms` is about today's `scene_ms + shared_core_ms`).

The limit is judged by core (`DKR_PROF_CORES`, on by default since 2026-11-17), as the uncapped bench counts them:
the game core is `logic + scene + shared`, the renderer core is the two-core renderer's backend (an Old 3DS's system
core; a New 3DS's renderer whole), and the GPU its drawing; `retrace` means all three are under 90% of the frame's
budget. The last line, `spare ms`, gives what each of them has left of the budget (negative when it runs over); it is
the first line left out when a test run's status needs the room. In the draw mix (log, `walk_ms` in
`autorun_drawmix.csv`, Old 3DS) the walk's time is taken out of the kind being built and shown as
`renderer walk on this core`, so `menus/other` is the game's own work. `DKR_PROF_CORES=0` gives the view before
(`game CPU` / `renderer CPU`, the walk counted with the scene in the graph and with the kind in the draw mix).

CPU counters and the timings beside them (roadmap P56, 2026-11-19). `DKR_PMU=1` in `env.txt` (debug builds, off by
default) reads the ARM11's own performance counters once a frame on the game's core and, when it is another core, on
the renderer's (`port/3ds/pmu_3ds.c`): cycles, the cycles the core waited for instructions (the fetch stall, which
an instruction cache that is too small for the hot code shows as) and instruction cache misses; `DKR_PMU=2` counts
data cache read misses instead of the instruction cache's. The counters belong to a core, not a thread, so the
numbers are everything that ran on that core. A CPU line after the `spare ms` line (and given way before it) shows
`stall game% / renderer%` when they are on, the DSP sound callback's ms a frame (`sound cb`, the
`audio_ndsp.c` callback on the game's core) and the clock reads a frame (`clocks`, `plat_time_ns` calls: each is a
system call on an Old 3DS). Each race's results line ends with them (`,pmu,...,shown,...,sndcb,...,clk,...`, see
`pmu_cells` in `port_autorun.c`), along with the pictures replaced before they reached the screen and the longest gap
between two that did (`port/src/plat/shown.h`). The switch bench's table has the fetch stall and the replaced count.

Where the game core's time goes, in more detail (roadmap P61 M, 2026-11-22; off unless set, so the usual races are
untouched). With `DKR_PMU` on, the counters are also read at each game core section change on the timed frames (an Old
3DS's 1 in 8): the log's and `autorun_cores.txt`'s `game core sections (PMU n, ...)` give, per section, the M cycles,
the share of them spent waiting for instructions and the thousands of events (I-cache misses for 1, D-cache read
misses for 2) a frame; `top` holds the walk on an Old 3DS. `DKR_WALK_STEPS=1` times the display list walk's steps on
one frame in 16 while the profiler is on: inside each native record its entry and material checks, the mesh lookup,
the track's box test, the vertex transform, the colour/fog loop, the screen edges, state and axes, the triangle loop
and its bucket finds and draws, and records that leave by another road; outside them the sprite records, the state
commands, the split ring's copy, the opaque flush and the rest. The log line `walk steps (ms a frame, racers / track
/ objects / all)` and the all-kinds part of `autorun_cores.txt` give them, less the clock reads' own cost. Since P65 the
sprite records are split into their set-up, `sprite corners` and `sprite tris`, and billboard records' corners and
triangles are timed apart from the other roads (`billboard corners`, `billboard tris`); a line past the log's length
goes on in `walk steps (more):` lines. `DKR_KART_COUNTS=1` (P65 rank 2) logs every 600 frames `kart batches (a frame,
...)`: the native kart batches by road (one vertex load, two, made anew each frame, billboard, no tables), their
corners, vertices and triangles, and the share of vertices that moved since that vertex buffer was last drawn (what
animation rewrites). On the
backend mix's timed frames (`DKR_BACKEND_MIX`) the backend's model and mesh draws (GPU vertex maths: `DKR_NATIVE_GPU`,
the native track) are also split into their parts (`backend model/mesh draws: N a frame, drain, state, settings
twice, uniforms, copies, indices, draw, other`), after the `backend records` line.

What the data side costs (roadmap P60 A, 2026-11-22). `DKR_PMU=3` counts, in the stall column, the cycles the core
stalled on a data hazard (a load's or the VFP's result not ready yet) and, in place of the misses, the instructions
executed, so the summary line gives the cycles per instruction; `DKR_PMU=4` the cycles the load/store unit was full
and the data cache write misses. The results' `pmn1_kind` cell says which (`datastall`, `lsufull`); the switch bench
table marks those stalls `D` and `L`. `DKR_VFP_RUNFAST=1` (default 0) runs the game thread, the walk and the backend
with the VFP in RunFast mode (flush-to-zero and default NaN on, exception traps off), which keeps denormals and NaNs
off the kernel's slow support code; each of those threads logs its starting mode once (`VFP mode, game thread:
FPSCR ...`).

The walk's exact data cuts (roadmap P61 W, 2026-11-22), each its own switch, `0` the old way:
`DKR_WALK_BACKFACE` (default 1) reads each triangle's back-face flag from the mesh's 1-byte table instead of its
16-byte triangle copy; `DKR_NMAT_ONCE` (default 1) checks a material's textures (still in the cache, the same
generation) once a frame rather than at every record, again after any cache entry is freed; `DKR_WALK_PLD` (Old 3DS
default 3, elsewhere 0) prefetches ahead in the vertex transform (bit 1) and the triangle loop (bit 2), bit 4 also the
next record's texture entry. Neither W1 nor W2 changes a picture. `DKR_NMAT_ONCE_CHECK=1` (test builds) checks every
skipped material the old way too and logs `material once check: N skipped, M would have dropped` (a wrong one counts in
the results' `checks_wrong` and is dropped as before); `=2` also frees, every 30th frame, a texture of a material
already checked in that frame, as an eviction mid-frame would, so the check has something to catch.

### Live monitor

While the readout is on, the bottom screen is a live monitor: a graph of the last 158 frames (the newest on the
right; the top is 50 ms, with lines at 60 and 30 fps), each bar coloured by what took its time (green game logic,
cyan building the display list, yellow the renderer (on an Old 3DS also the display list walk on the game's core),
orange texture decoding, red the GPU, grey waiting for the
retrace; a white tip marks a hitch), over one of four pages. Tap under the graph for the next page; tap the graph to
freeze it and the page, to read them.

| Page | What it shows, a frame |
| ---- | ---------------------- |
| PROFILER | The readout above. |
| DRAW KINDS | Per kind of thing drawn (menus and other, track, racers, objects, sky, see-through, effects, shadows, HUD): triangles, draws, the renderer's and the game thread's ms, and the share of its triangles wasted. Then the level segments drawn, of all and of those the visibility table allowed, and the objects drawn of those active. |
| OBJECTS | The 13 costliest kinds of object (by behaviour; particles together): updated, drawn, triangles, and game logic, display list building and renderer ms. |
| WASTE | Per kind, of its triangles: off the screen, facing away (the GPU culls them), under a pixel; and its overdraw (the screen area its triangles cover, x1.00 being the screen once). |

The pages are set in a small fixed-width terminal font (5x7 glyphs in 6x9 cells, 47 columns by 14 lines), which fits
them on three pages: PROFILER (the whole readout and the graph's key in the bars' colours), DRAW KINDS (every kind
drawn, its waste columns included: `off`, `bck`, `tny` as per cent of its triangles and `over` in screens) and OBJECTS
(the 12 costliest). `DKR_PROF_TERM=0` in `env.txt` goes back to the look's own fonts and the five pages above. A
line too long for the page wraps at a space onto the next row (indented by 2) rather than being cut, so a test run's
status shows the bench row's whole name; when the page runs out of rows the graph's key gives way first. The host
build checks this (`DKR_PROF_TERM_CHECK=1 port/build/host/dkr`, run by CI): every page, with the longest status of
each kind of run, on both consoles' layouts, must fit with no line lost.

A page flip and the once-a-second update redraw the readout from a cache of its panels (`DKR_BS_PANEL_CACHE`, default
1): the background, the panels, the graph's dim fill and its 60/30 fps labels are drawn once per look, and each redraw
copies them and draws only the bars and the text over them. The panels' gradients were most of a redraw's time (on a
PC about 400-1500 µs a redraw before, about 50-75 µs after). `DKR_BS_PANEL_CACHE=0` draws everything every time, as
before. `DKR_BS_PANEL_CACHE_CHECK=1` draws each cached redraw the old way too and logs `bottom screen panel cache: N
pixels differ` while any do; the host build checks every look and page this way in CI (`DKR_BS_PANEL_CACHE_CHECK=1`
with `DKR_BOTTOM_PREVIEW`). On an Old 3DS the screen thread on the system core holds the bottom screen's lock at a
priority above the renderer's backend (`DKR_BS_LOCK_CEILING`, default 1 there, 0 on a New 3DS), so the game never waits
for it while the backend runs; the `bottom screen:` log line says how long the game waited for that lock (`game waited
X ms a frame (max Y ms)`), and test run results carry it as `screen_lock_wait_ms` and `screen_lock_wait_max_ms`.
Test run results also carry the bottom screen thread's ms per area drawn (`screen_ms_area`) and, after a `chk` mark,
the self-checks (`*_CHECK` switches) made in each race, how many went wrong and the first one's name (`checks_run`,
`checks_wrong`, `first_wrong_check`). `DKR_PROF_PAGE_FLIP=N` (test runs only) shows the readout and turns its page every
N game frames, on the results and menus between races too: the switch bench's 1-lap "page flip stress" rows.

With the two-core renderer (the default on both consoles) the renderer's time per kind is the display list walk's;
the GPU hand-off on the other core is in the log's `two-core renderer` line. The renderer's and the game thread's
times per kind are taken while the profiler is on (an Old 3DS, whose clock reads
are system calls, times one frame in 8); the waste on one frame in 8. The log gets the same as `draw mix a frame` and
`objects a frame` lines every few seconds. The GPU can't time its draws one by one, so on one frame in a few the
command list is cut where the kind of draw changes and each kind's GPU time is read from the GSP's events
(`DKR_GPU_KINDS`, with the GPU timeline).

## Unattended test run (debug build only)

Tap Settings, Test run, then Full test, and leave the console on its charger. From the
title screen, the game races every track with every character (200 races, some 8 hours), with the AI driving your
kart. Each race is profiled and checked: a race that doesn't finish within 5 minutes, a kart that stops reaching
new checkpoints for a minute, a crash or a freeze are all recorded. When your kart gets no further along the course
for 10 s (wedged, circling, or fallen under the track), it is put at the next checkpoint and the race goes on; the spot is noted in the results. The bottom screen shows the profiler, the race, and a
progress bar. Your saves are not written while it runs. Tap Stop test (next to Settings) to end it early.

- Results go to `sdmc:/3ds/dkr/`: `autorun_results.csv` (one line per race: outcome, frame rate, the profiler's
  stages, worst frames), `autorun_problems.txt`, `autorun_hitches.txt` and `autorun_drawmix.csv` (every test run and
  bench: per race a row per kind drawn, per costly object kind and for the scene, as on the monitor's pages); the
  issue sums the draw mix up under "Where the time goes".
- When the run ends or is stopped, and bug reports are set up, the results are sent as an issue titled
  "[3DS test run]". `python3 port/tools/bugreport_fetch.py <issue>` saves its files, and
  `python3 port/tools/autorun_report.py bugreport_<issue>/autorun_results.csv` turns them into tables by track and
  by character, the slowest races and the problems.
- If the game crashes or freezes, start it again: it sends the crash report without asking and carries on with the
  next race. (A freeze is noticed when nothing has been drawn for 20 seconds.)

`env.txt` settings change what it runs (the same settings work on the host build):

| Setting | Effect |
| ------- | ------ |
| `DKR_AUTORUN=1` | Run at every start, without the menu. |
| `DKR_AUTORUN_TRACKS=5,3` | Only these tracks (`ASSET_LEVEL_*` ids; default: the 20 of the tracks menu). |
| `DKR_AUTORUN_CHARACTERS=3,9` | Only these characters: Krunch 0, Bumper 1, Tiptup 2, Conker 3, Timber 4, Banjo 5, Drumstick 6, Pipsy 7, T.T. 8, Diddy 9. |
| `DKR_AUTORUN_VEHICLES=all` | Also each other vehicle a track allows. |
| `DKR_AUTORUN_TIMEOUT=300` | Seconds before an unfinished race counts as a timeout. |
| `DKR_AUTORUN_LOOP=1` | Start over after the last race. |
| `DKR_AUTORUN_RESTART=1` | Start from the first race instead of resuming. |
| `DKR_AUTORUN_SNAPSHOT=1` | Save a snapshot (GPU and reference images) after each race, for `diff_frames.py`. |

### Quick test

Tap Settings, Test run, then Quick test. It races only the slowest tracks of the full run (Haunted Woods, Jungle Falls, Crescent
Island, Whale Bay and EverFrost Peak, from hardware test run #12), twice each with Bumper and your own anti-aliasing
setting: about 20 minutes. Use it to tell whether a new build made the slow spots faster: run it on the old build,
install the new one, run it again. The results issue ("[3DS test run] quick test ...") compares each track with the
quick test before it on the same console, with both builds named, and gives the change in frame rate, share of
frames at 30 fps, GPU and renderer time and worst frame. The races are in `quick_test_results.csv` (the test run's
columns plus `build`), which `autorun_report.py` also splits by track and build. `DKR_AUTORUN_QUICK=1` runs it at
every start; `DKR_AUTORUN_QUICK_TRACKS`, `DKR_AUTORUN_QUICK_CHARACTER` and `DKR_AUTORUN_QUICK_PASSES` change the
tracks, the character and the races per track.

### Boss fights

Tap Settings, Test run, then Boss fights. Boss levels aren't on the tracks menu, so the full run never races them;
this races each of the ten boss fights once (Tricky, Bluey, Bubbler, Smokey and Wizpig, first and second fight) with
Bumper and the AI driving: about half an hour. The results issue ("[3DS test run] boss fights ...") has one line per
fight with its outcome, time, place, frame rate and how often the kart had to be put back on the course; a fall
through the ground shows up as unsticks or as a race that never finishes. The races are in `boss_test_results.csv`
(the test run's columns plus `build`). `DKR_AUTORUN_BOSS=1` runs it at every start; `DKR_AUTORUN_BOSS_TRACKS`,
`DKR_AUTORUN_BOSS_CHARACTER` and `DKR_AUTORUN_BOSS_PASSES` change the levels, the character and the races per boss.

### Whole game

Tap Settings, Test run, then Whole game (about 5 hours) or Whole game (short) (about an hour). It plays as much of the
game as works without a person, in six parts: a pass through the menus (title screen, options, character select, the
adventure's file select and the tracks menu, pressing the buttons a player would), every race track with every vehicle
it allows (the characters taking turns), each track's silver coin race in its adventure form, the four battle arenas
(played for 2.5 minutes or until they end), the ten boss fights, and a tour of each hub of the overworld: no AI course
runs there, so the kart is put in every segment of the hub's map in turn and turned once round, so everything is drawn
from everywhere. The adventure levels play with every door, boss and cutscene done, in memory only (the saves are never
written and the progress is put back afterwards). The short run plays every part with less of each (one lap of each
track with its usual vehicle, two silver coin races, one boss, one hub); it is also the run that gathers data for
profile-guided compiling. With bug reports set up, the run's issue ("[3DS test run] whole game ...") is made after the
first part, and each later part (and every 30 minutes, `DKR_AUTORUN_REPORT_MINUTES`) adds a comment with that part's
stops (frame rate, 1% low, frames at 60/30/20/slower, what limited it, worst frame, hitches), the run so far and its
lowest 1% lows; the next stop waits until the comment is out, so the network never runs during a measurement. The last
comment brings the files. Results are in `wholegame_results.csv` (the test run's columns plus `stop`, `part` and
`build`). `DKR_AUTORUN_WHOLE=1` (or `2` for the short run) runs it at every start; `DKR_AUTORUN_WHOLE_PARTS` (letters
`mrcabh`) and `DKR_AUTORUN_WHOLE_LEVELS` keep only some of it.

### GPU benchmark

Tap Settings, Test run, then GPU bench. It is a short test run (about 30 minutes) that measures the render settings on
the heaviest races: Jungle Falls, Crescent Island and Haunted Woods, the slowest tracks of the quick tests, raced by
Bumper once with each of 5 settings: anti-aliasing 4x, 2x or off, and 60 fps with AA off or 2x.
The game switches the settings itself before each race and puts your own back at the end. The results issue
("[3DS test run] GPU benchmark ...") has one line per track and setting, with the GPU time it saves against AA 4x and
how many frames made 60 fps; the races are in `gpu_bench_results.csv` (the test run's columns plus `config` and
`pct_60fps`), which `autorun_report.py` also splits by track and setting. It keeps its own progress file, so a full test
run it interrupts carries on afterwards. `DKR_AUTORUN_BENCH=1` runs it at every start instead;
`DKR_AUTORUN_BENCH_TRACKS`, `DKR_AUTORUN_BENCH_CHARACTER` and `DKR_AUTORUN_BENCH_CONFIGS` (indexes into the 5 settings)
change the tracks, the character and the settings. It replaced the anti-aliasing benchmark of roadmap item G0.

## Automated testing

These settings work on both builds (in `env.txt` on the 3DS, as environment variables on the host):

| Setting                  | Effect |
| ------------------------ | ------ |
| `DKR_WARP=all` or `DKR_WARP=4,7,12` | The title screen's attract demos load these levels instead (ids in `ASSET_LEVEL_*` order), one after another, with the AI driving and the title overlay hidden. |
| `DKR_WARP_SECONDS=n`     | How long each warped level runs (default 20). |
| `DKR_DIFF_EVERY=n`       | 3DS only: every nth frame is also drawn by the software renderer, and both images are written to `sdmc:/3ds/dkr/` (`diff_N_ref.png`, `diff_N_gpu.bin`). `port/tools/diff_frames.py <dir> <out>` compares them and writes side by side images of the frames that differ. |
| `DKR_TEST_SAVE=1`        | Starting a save file gives it complete progress: all balloons, keys, trophies, amulets and Taj challenges, and every boss and door. The progress is written to the save, so the file keeps it without the setting. |

`port/tools/testdata/eeprom_complete.bin` is a save made this way (file A, "AAA"), in the old `eeprom.bin` byte
order (the frame regression scenarios use it as one, so builds from before the N64 format read it too). Copied to
`eeprom.bin` in a data folder without a `dkr.eep`, it replaces that folder's saves.

`DKR_LOCKSTEP=1` (meant for the host build) makes runs repeatable: the N64 threads take turns like on the console's
single CPU, each retrace waits until every thread is done with the previous one, and the game's clock advances by
exactly 1/60 s per retrace, so the same build always draws the same frames, as fast as the machine allows.

`port/tools/frame_regression.py` uses it to compare two host builds:

```
python3 port/tools/frame_regression.py --rom baserom.z64 --head port/build/host/dkr --base old/dkr --out /tmp/regression
```

It plays a few scripted scenarios (the intro and title, a spread of levels through `DKR_WARP`, and the Adventure hub
from the complete save) on both builds, and writes `report.md` and side by side images (base, head, difference) of
every frame that differs. CI runs it on every push with `--max-images 0` and uploads only the report and logs (the
repository is public, so no game frames), and publishes the 3DS build only once it passes.

CI also plays the same scenarios once more with the Old 3DS's texture lookup memo forced on and checked
(`DKR_TEX_MEMO=1 DKR_TEX_MEMO_CHECK=1`, roadmap P56): at every memo hit the full lookup runs too
(`tex_memo_check_answer` in `gfx_rdp.c`), and a different texture is a memo bug, which fails the run with the tile
in the log. The same texture found in another cache entry (a duplicate in the cache, the same picture) is counted
apart and only reported. The memo is off on a PC by default, so this is the only place a PC tests it.

`port/tools/backend_replay.py` checks the 3DS backend (`port/3ds/gfx_c3d.c`) on the PC. With
`DKR_BACKEND_RECORD=file` the host build writes every call its renderer makes into the backend (`gfxb_*`, with their
vertices, states and textures; `port/host/backend_record.c`, `DKR_BACKEND_RECORD_EVERY=n` keeps every nth frame's
draws). `port/host/c3d_replay/` builds `gfx_c3d.c` with the real citro3d and libctru's command list code for the PC
(32-bit), libctru's system calls faked, and plays a record through it, decoding the command lists it hands to the GPU
into the PICA200's registers. The script records the frame regression scenarios, then runs two checks for a New and
an Old 3DS: the command words of every draw against the previous commit's backend (fails unless the commit message
says `backend-change: expected`), and the GPU registers at every draw with only changed settings sent (the default)
against every setting sent for every draw (`DKR_STATE_CACHE=0 DKR_FAST_STATE=0`), where any difference is a state
cache bug. It fetches libctru, citro3d and picasso at the commits the 3DS build's devkitARM image ships:

```
python3 port/tools/backend_replay.py ci --deps /tmp/replay-deps --exe port/build/host/dkr --rom baserom.z64 \
    --work /tmp/replay --base-port old/port
```

It doesn't see timing or ordering against the GPU (a hang from texture registers written right after a draw), stereo
3D, or the GPU vertex maths' model and mesh draws (off by default, so not in the records). CI runs it after the frame
regression.

`port/tools/n64_compare/` checks the renderer against a real N64 instead: an accurate emulator (ares, running the
game's own F3DDKR microcode on its emulated RSP and paraLLEl-RDP for the RDP) plays the ROM and saves chosen frames'
display lists with the pictures it drew from them, and the port's renderer replays the same display lists, so both
sides draw exactly the same frame. See its README for how to run it and what it found.

### CI

`.github/workflows/Port.yml` runs on every push: it builds `dkr.3dsx` and the host build (both are uploaded as
workflow artifacts), then runs the frame regression and the backend replay between this commit and the previous one,
with their reports on the run's summary page. It needs the `ROMURL` repository secret, a private download URL for the US 1.0 ROM (the
same secret the N64 workflows use).

## Testing in Azahar

The port runs in the Azahar emulator.

1. Open the emulator's SD card folder with *File > Open Azahar Folder*, then go into `sdmc`. It is usually
   `%APPDATA%\Azahar\sdmc` on Windows, `~/.local/share/azahar-emu/sdmc` on Linux or
   `~/Library/Application Support/Azahar/sdmc` on macOS.
2. Put the ROM in `3ds/dkr/` inside that folder.
3. Load `dkr.3dsx` with *File > Load File*.

For sound in Azahar, put any file at `sdmc/3ds/dspfirm.cdc`; Azahar's emulated DSP does not need the real firmware.

## Status

- **Real hardware:** played on a New 3DS and an Old 3DS. Per track frame rates are in the release notes
  (`port/release-notes/`).
- **Tested by machine:** every track with every character (the full test run), the ten boss fights, and the host
  build's frame regression on every push.
- **Not yet checked end to end:** a complete Adventure playthrough.
- **Known gaps:**
  - Only the US 1.0 ROM is supported; other versions are planned.
  - The Old 3DS runs at about 30 fps and needs its system core for the renderer's second half.
  - One player only (see above); rumble isn't possible on a 3DS.

Player defaults (the release build): anti-aliasing 4x, 30 fps, Balanced widescreen, 800 pixels and stereo 3D on,
HUD at the screen edges, race HUD on the bottom screen, mipmaps on, colour boost off, Modern camera with the wall
check, smooth resampling.
