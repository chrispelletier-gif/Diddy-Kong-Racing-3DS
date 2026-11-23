# Profile-guided builds (roadmap P69)

`pgo_counts.bin` holds the branch counts of a training run on an Old 3DS: how often each branch of the game's code
went each way. A `PGO=use` build uses them to lay each function out with its usual path straight through and its rare
code moved away (hot/cold placement), so the game core fetches fewer instruction cache lines. Nothing about what the
game computes changes: no maths flags are added, so floats round exactly as in the usual build.

CI builds two extra debug builds on every push (job `build-3ds-pgo` in `.github/workflows/Port.yml`) and adds them
to the `3ds-newest` release next to the usual files:

| file | make | HOME menu title | what for |
|---|---|---|---|
| `dkr-pgo.cia` / `.3dsx` | `PGO=use PGO_LEAN=1` | DKR PGO (title 0xD4B53) | the faster build to bench against the usual one |
| `dkr-pgo-train.cia` / `.3dsx` | `PGO=gen` | DKR PGO training (title 0xD4B54) | makes new counts; runs ~25% slower while counting |

Both install next to the usual Diddy Kong Racing title and share its data folder (`sdmc:/3ds/dkr`: ROM, saves,
settings), so the same Test run buttons can be raced in each on one console. Their bug reports and bench results
say `-pgo` or `-pgotrain` after the build name. (Their Check for updates button updates the usual title, not
themselves; install a newer PGO build from the release with FBI.)

`PGO_LEAN=1` keeps the profile from growing the code: no extra unrolling, peeling, tracing or function clones. The
full PGO build of 2026-11-01 was 12% bigger and slower on the Old 3DS (#331); the lean one was faster on 23 of 28
stops (#338, +0.2 fps average, renderer about -0.5 ms).

## The counts in this folder

From training run #328 (2026-11-01, Whole game (short), Old 3DS, code of v48). Counts from older code still apply:
a function whose branches changed since is built without counts (a `coverage-mismatch` warning, not an error), one
that only moved keeps them, and files that are new since have none (`-fprofile-partial-training` builds them as
usual). On master 8679864 that is 107 functions without counts and 328 kept; the CI job's summary prints both
numbers. With these counts the build's code is 2% smaller and the hot code (hot_size.py hot_span) 12% smaller than
the usual build's (69,080 vs 78,288 bytes).

## Refreshing the counts

1. Install `dkr-pgo-train.cia` from `3ds-newest`, start "DKR PGO training", and press Test run > Whole game (short).
2. At the end of each part the game writes `sdmc:/3ds/dkr/pgo_counts.bin`; the run's report attaches it.
3. Commit that file here as `pgo_counts.bin` (and note the run here). The next push builds `dkr-pgo` with it.

Train on the code you want to ship: the closer the training build's commit is to the PGO build's, the more functions
keep their counts.

## By hand

```
python3 port/tools/pgo_unpack.py port/pgo/pgo_counts.bin --to port   # .gcda files into port/build/3ds-pgo/obj
make -C port TARGET=3ds PGO=use PGO_LEAN=1 -j8 && make -C port TARGET=3ds PGO=use PGO_LEAN=1 cia
rm -rf port/build/3ds-pgo                                              # before switching to the training build
make -C port TARGET=3ds PGO=gen -j8 && make -C port TARGET=3ds PGO=gen cia
```
