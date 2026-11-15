# map_paths: the grid's track maps as data

`port/src/plat/bottom_map_paths.h` holds each track's centre line, drawn by the bottom screen's
grid looks as a lane (light) over a rim (the same line, 4 px wider). It is made once, offline, from
the ROM's own map pictures (the shape) and the tracks' checkpoints (which way round is travel).

Regenerate (host build, from the repo root, with the game data in `.`):

```sh
# 1. checkpoints, every track: one autorun pass with the race HUD on
DKR_DATA_DIR=. DKR_BOTTOM_RACE=/tmp/mp DKR_DUMP_PATHS=/tmp/mp/paths.txt DKR_AUTORUN=1 \
  DKR_AUTORUN_RESTART=1 DKR_AUTORUN_TRACKS=all DKR_AUTORUN_CHARACTERS=9 DKR_AUTORUN_TIMEOUT=2 \
  DKR_NULL_RENDER=1 ./port/build/host/dkr

# 2. the ROM map pictures traced to their centre lines (level 41, the Dragon boss, has no
#    checkpoints: its line is traced from the picture alone)
for lv in 5 3 29 7 8 4 10 30 13 6 9 28 19 18 20 31 17 32 33 15 41; do
  DKR_DATA_DIR=. DKR_BOTTOM_GRID=1 DKR_BOTTOM_PREVIEW=/tmp/mp/pv DKR_PREVIEW_TT_LEVEL=$lv \
    DKR_DUMP_SKEL=/tmp/mp/skel.txt ./port/build/host/dkr
done

# 3. graph, prune, smooth, simplify, orient; write the header
python3 port/tools/map_paths/make_map_paths.py /tmp/mp/skel.txt /tmp/mp/paths.txt \
  port/src/plat/bottom_map_paths.h
```
