#!/usr/bin/env python3
"""
P67 hidden track batch tables (proposals/3ds-hidden-batch-skip-plan.md): races the host build to log camera poses,
bakes the tables from them, writes port/src/game/hidden_tables.inc, and evaluates cell sizes on held-out races.

  hidden_bake.py poses    --exe EXE --rom ROM --work DIR [--set A|B] [--jobs 4] [--out port/data/hidden_poses.txt]
      Races the set's pose log races (DKR_HIDDEN_POSES=1) and merges their logs (set A: the bake's races).
  hidden_bake.py merge    FILE... --out FILE
      Merges pose logs (the same key: frames added, boxes and ranges joined).
  hidden_bake.py bake     --exe EXE --rom ROM --poses FILE --work DIR [--configs 2x8] [--jobs 4]
      Bakes every level of the pose file (one host process a level, DKR_HIDDEN_BAKE), tables in DIR/out/<NxS>/.
  hidden_bake.py gen      --tables DIR/out/2x8 [--out port/src/game/hidden_tables.inc]
      Writes the generated C file from one cell size's tables.
  hidden_bake.py evaluate --exe EXE --rom ROM --tables DIR/out --work DIR [--jobs 4] [--set B]
      Races the held-out set with every cell size's table at once (DKR_HIDDEN_EVAL: the first one skips, all are
      counted and self-checked) and prints the table of section 6 of the plan (skip share, frames with a row, check
      pixels, table bytes, by track and cell size).
  hidden_bake.py check    --exe EXE --rom ROM --work DIR [--tracks 8,20,13,29] [--break] [--laps 1]
      One race a track with the compiled-in tables, DKR_HIDDEN_SKIP=1 and the self-check; fails unless every level
      says PASS (with --break, DKR_HIDDEN_SKIP_BREAK=1: fails unless every level says FAILED).

Race sets (per track): characters Bumper (1), Krunch (0) and Tiptup (2) x camera zoom 0 (medium) and 1 (far), each
race with every vehicle the track allows. Set A (bake) = the even combinations, set B (held out) = the odd ones plus
Conker (3) at zoom 0, a character the bake never saw.
"""
import argparse
import concurrent.futures
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

BENCH_TRACKS = [8, 20, 13, 29]  # Whale Bay, Windmill Plains, EverFrost Peak, Jungle Falls (Old 3DS switch bench)
TRACK_NAMES = {8: "Whale Bay", 20: "Windmill Plains", 13: "EverFrost Peak", 29: "Jungle Falls"}
COMBOS = [(1, 0), (1, 1), (0, 1), (0, 0), (2, 0), (2, 1)]  # (character, zoom); even index: set A
EXTRA_B = [(3, 0)]


def race_set(name, tracks):
    runs = []
    for t in tracks:
        combos = [c for i, c in enumerate(COMBOS) if (i % 2 == 0) == (name == "A")]
        if name == "B":
            combos += EXTRA_B
        for character, zoom in combos:
            runs.append((t, character, zoom))
    return runs


def run_game(exe, rom, work, env_extra, done_pattern=None, timeout=7200, frames=10000000):
    """Runs the host build in a fresh data dir until it exits, or until done_pattern shows in its log."""
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    shutil.copy(rom, os.path.join(work, "baserom.z64"))
    env = dict(os.environ)
    env.update({"DKR_DATA_DIR": work, "DKR_WIDESCREEN": "1", "DKR_FRAMES": str(frames)})
    env.update(env_extra)
    log_path = os.path.join(work, "log.txt")
    start = time.time()
    with open(log_path, "w") as log:
        proc = subprocess.Popen([os.path.abspath(exe)], env=env, stdout=log, stderr=subprocess.STDOUT, cwd=work)
        while proc.poll() is None:
            time.sleep(2)
            if done_pattern is not None:
                with open(log_path, errors="replace") as f:
                    if re.search(done_pattern, f.read()):
                        time.sleep(3)
                        proc.terminate()
                        try:
                            proc.wait(20)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                        break
            if time.time() - start > timeout:
                proc.kill()
                break
    os.remove(os.path.join(work, "baserom.z64"))
    with open(log_path, errors="replace") as f:
        return f.read()


def race(exe, rom, work, track, character, zoom, env_extra, laps=None):
    env = {"DKR_AUTORUN": "1", "DKR_AUTORUN_TRACKS": str(track), "DKR_AUTORUN_CHARACTERS": str(character),
           "DKR_AUTORUN_VEHICLES": "a", "DKR_AUTORUN_ZOOM": str(zoom)}
    env.update(env_extra)
    return run_game(exe, rom, work, env, done_pattern=r"autorun: all \d+ races done")


def merge(paths, levels=None):
    keys = {}
    for path in paths:
        with open(path) as f:
            for line in f:
                v = [int(x) for x in line.split()]
                if len(v) != 17 or (levels is not None and v[0] not in levels):
                    continue
                k = tuple(v[:4])
                if k not in keys:
                    keys[k] = v[4:]
                    continue
                o = keys[k]
                o[0] += v[4]
                for i in range(3):
                    o[1 + i] = min(o[1 + i], v[5 + i])
                    o[4 + i] = max(o[4 + i], v[8 + i])
                o[7] = min(o[7], v[11])
                o[8] = max(o[8], v[12])
                o[9] = min(o[9], v[13])
                o[10] = max(o[10], v[14])
                o[11] = max(o[11], v[15])
                o[12] = max(o[12], v[16])
    return ["%s\n" % " ".join(str(x) for x in list(k) + keys[k]) for k in sorted(keys)]


def cmd_poses(args):
    runs = race_set(args.set, [int(t) for t in args.tracks.split(",")])
    os.makedirs(args.work, exist_ok=True)

    def one(r):
        track, character, zoom = r
        work = os.path.join(args.work, "%s_%d_%d_%d" % (args.set, track, character, zoom))
        log = race(args.exe, args.rom, work, track, character, zoom, {"DKR_HIDDEN_POSES": "1"})
        print("%s: %d races finished" % (os.path.basename(work), log.count(",finished,")), flush=True)
        return os.path.join(work, "hidden_poses.txt")

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        files = [f for f in pool.map(one, runs) if os.path.exists(f)]
    lines = merge(files, set(int(t) for t in args.tracks.split(",")))
    with open(args.out, "w") as f:
        f.writelines(lines)
    print("%d keys from %d runs -> %s" % (len(lines), len(files), args.out))
    return 0


def cmd_merge(args):
    lines = merge(args.files)
    with open(args.out, "w") as f:
        f.writelines(lines)
    print("%d keys -> %s" % (len(lines), args.out))
    return 0


def cmd_bake(args):
    levels = sorted({int(l.split()[0]) for l in open(args.poses) if l.strip()})
    out = os.path.abspath(os.path.join(args.work, "out"))
    os.makedirs(out, exist_ok=True)

    def one(level):
        done = glob.glob(os.path.join(out, "*", "hidden_%d.bin" % level))
        if done and not os.path.exists(os.path.join(out, "hidden_%d.ckpt" % level)):
            print("level %d: already baked (%d tables in %s), skipped" % (level, len(done), out), flush=True)
            return True  # (a restarted bake picks up where it stopped: done levels skipped, the rest resume)
        work = os.path.abspath(os.path.join(args.work, "L%d" % level))
        os.makedirs(work, exist_ok=True)
        poses = os.path.join(args.work, "poses_%d.txt" % level)
        with open(poses, "w") as f:
            f.writelines(l for l in open(args.poses) if l.split() and int(l.split()[0]) == level)
        env = {"DKR_WARP": str(level), "DKR_WARP_SECONDS": "5", "DKR_HIDDEN_BAKE": os.path.abspath(poses),
               "DKR_HIDDEN_BAKE_OUT": out, "DKR_HIDDEN_BAKE_THREADS": str(args.threads)}
        if args.configs:
            env["DKR_HIDDEN_BAKE_CONFIGS"] = args.configs
        log = run_game(args.exe, args.rom, work, env, timeout=7 * 24 * 3600)  # a dense bake takes hours
        lines = [l for l in log.splitlines() if l.startswith("hidden bake: level")]
        ok = "hidden bake: every level done" in log
        print("level %d: %s\n  %s" % (level, "done" if ok else "FAILED (see %s/log.txt)" % work,
                                      "\n  ".join(lines[-8:])), flush=True)
        return ok

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(one, levels))
    return 0 if all(results) else 1


def cmd_gen(args):
    blocks = []
    for path in sorted(glob.glob(os.path.join(args.tables, "hidden_*.bin")),
                       key=lambda p: int(re.search(r"hidden_(\d+)\.bin", p).group(1))):
        level = int(re.search(r"hidden_(\d+)\.bin", path).group(1))
        with open(path, "rb") as f:
            blocks.append((level, f.read()))
    config = os.path.basename(os.path.normpath(args.tables))
    out = ["/* GENERATED by port/tools/hidden_bake.py gen from port/data/hidden_poses.txt and the US 1.0 ROM (cell size",
           " * %s): do not edit. P67 hidden track batch tables; port/include/port_hidden_skip.h has the block format." % config,
           " * Regenerate: hidden_bake.py bake (then gen) with the committed pose log; CI checks it is not stale. */",
           "/* poses sha256 %s */" % poses_sha(args.poses)]
    total = 0
    for level, data in blocks:
        out.append("static const u8 sHiddenTable%d[%d] = {" % (level, len(data)))
        for i in range(0, len(data), 24):
            out.append("    " + " ".join("0x%02X," % b for b in data[i:i + 24]))
        out.append("};")
        total += len(data)
    out.append("static const struct {")
    out.append("    s32 level;")
    out.append("    const u8 *data;")
    out.append("    s32 size;")
    out.append("} sHiddenTables[] = {")
    for level, data in blocks:
        out.append("    { %d, sHiddenTable%d, %d }," % (level, level, len(data)))
    out.append("    { -1, NULL, 0 },")
    out.append("};")
    with open(args.out, "w") as f:
        f.write("\n".join(out) + "\n")
    print("%d tables, %d bytes -> %s" % (len(blocks), total, args.out))
    return 0


def poses_sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def cmd_stale(args):
    # A full re-bake takes hours, so CI checks the cheap half: the tables were generated from the committed pose log
    # (its sha256 is in the .inc). The other half, that they still fit the ROM's model, is the geometry hash the game
    # checks at level load (the CI skip run fails if no row engages).
    m = re.search(r"poses sha256 ([0-9a-f]{64})", open(args.inc).read())
    have = poses_sha(args.poses)
    if m is None or m.group(1) != have:
        print("hidden tables stale: %s was not generated from %s (sha256 %s); re-run hidden_bake.py bake and gen"
              % (args.inc, args.poses, have))
        return 1
    print("hidden tables: generated from the committed pose log (sha256 %s)" % have)
    return 0


SKIP_RE = re.compile(r"hidden skip (\S+): level (\d+), (\d+) frames, (\d+) with a row \(([\d.]+)%\), walked tris ([\d.]+) "
                     r"a frame, skipped ([\d.]+) \(([\d.]+)%\)")
CHECK_RE = re.compile(r"hidden check (\S+): level (\d+), frames (\d+), frames with pixels (\d+), most pixels (\d+), "
                      r"pixels (\d+), (PASS|FAILED)")


def cmd_evaluate(args):
    configs = sorted(d for d in os.listdir(args.tables) if re.match(r"^\d+x\d+$", d))
    # The likely pick first (it is the one the frames skip with); the rest only counted and checked
    configs.sort(key=lambda c: (c != args.first, c))
    dirs = ",".join(os.path.abspath(os.path.join(args.tables, c)) for c in configs)
    runs = race_set(args.set, [int(t) for t in args.tracks.split(",")])
    os.makedirs(args.work, exist_ok=True)
    env = {"DKR_HIDDEN_SKIP": "1", "DKR_HIDDEN_SKIP_CHECK": "1", "DKR_HIDDEN_EVAL": dirs}
    if args.break_check:
        env["DKR_HIDDEN_SKIP_BREAK"] = "1"

    def one(r):
        track, character, zoom = r
        work = os.path.join(args.work, "%s_%d_%d_%d" % (args.set, track, character, zoom))
        log = race(args.exe, args.rom, work, track, character, zoom, env)
        print("%s: %d races finished" % (os.path.basename(work), log.count(",finished,")), flush=True)
        return track, log

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        logs = list(pool.map(one, runs))
    # Sums by (config, level): frames, frames with a row, walked and skipped tris, check frames and pixels
    sums = {}
    for track, log in logs:
        for m in SKIP_RE.finditer(log):
            config, level = m.group(1), int(m.group(2))
            if level != track:
                continue
            frames, row = int(m.group(3)), int(m.group(4))
            s = sums.setdefault((config, level), [0, 0, 0.0, 0.0, 0, 0, 0])
            s[0] += frames
            s[1] += row
            s[2] += float(m.group(6)) * frames
            s[3] += float(m.group(7)) * frames
        for m in CHECK_RE.finditer(log):
            config, level = m.group(1), int(m.group(2))
            if level != track:
                continue
            s = sums.setdefault((config, level), [0, 0, 0.0, 0.0, 0, 0, 0])
            s[4] += int(m.group(3))
            s[5] += int(m.group(4))
            s[6] += int(m.group(6))
    tracks = [int(t) for t in args.tracks.split(",")]
    lines = ["| Cell size | Track | Frames | With a row | Walked tris a frame | Skipped tris | Check frames with "
             "pixels | Check pixels | Table bytes |", "|---|---|---|---|---|---|---|---|---|"]
    for c in sorted(configs):
        for t in tracks:
            s = sums.get((c, t))
            path = os.path.join(args.tables, c, "hidden_%d.bin" % t)
            size = os.path.getsize(path) if os.path.exists(path) else 0
            if s is None or s[0] == 0:
                lines.append("| %s | %s | - | - | - | - | - | - | %d |" % (c, TRACK_NAMES.get(t, t), size))
                continue
            lines.append("| %s | %s | %d | %.1f%% | %.0f | %.1f%% | %d of %d | %d | %d |" % (
                c, TRACK_NAMES.get(t, t), s[0], 100.0 * s[1] / s[0], s[2] / s[0],
                100.0 * s[3] / s[2] if s[2] else 0.0, s[5], s[4], s[6], size))
    text = "\n".join(lines) + "\n"
    print(text)
    with open(os.path.join(args.work, "evaluate.md"), "w") as f:
        f.write(text)
    return 0


def cmd_check(args):
    env = {"DKR_HIDDEN_SKIP": "1", "DKR_HIDDEN_SKIP_CHECK": "1", "DKR_AUTORUN_QUICK": "1",
           "DKR_AUTORUN_QUICK_PASSES": "1", "DKR_AUTORUN_LAPS": str(args.laps)}
    if args.break_check:
        env["DKR_HIDDEN_SKIP_BREAK"] = "1"
    failed = 0
    for t in [int(x) for x in args.tracks.split(",")]:
        e = dict(env)
        e["DKR_AUTORUN_QUICK_TRACKS"] = str(t)
        log = run_game(args.exe, args.rom, os.path.join(args.work, "check_%d" % t), e,
                       done_pattern=r"autorun: all \d+ races done")
        results = [m for m in re.finditer(r"hidden check: level (\d+), .*?(PASS|FAILED)", log) if int(m.group(1)) == t]
        skip = [l for l in log.splitlines() if l.startswith("hidden skip: level %d," % t)]
        print("\n".join(skip + [m.group(0) for m in results]) or "track %d: no hidden check line" % t)
        want = "FAILED" if args.break_check else "PASS"
        if not results or any(m.group(2) != want for m in results):
            failed += 1
    print("hidden check %s: %s" % ("(broken on purpose)" if args.break_check else "",
                                   "ok" if failed == 0 else "%d tracks wrong" % failed))
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("poses", "bake", "evaluate", "check"):
        s = sub.add_parser(name)
        s.add_argument("--exe", required=True)
        s.add_argument("--rom", required=True)
        s.add_argument("--work", required=True)
        s.add_argument("--jobs", type=int, default=4)
        s.add_argument("--tracks", default=",".join(str(t) for t in BENCH_TRACKS))
        if name == "poses":
            s.add_argument("--set", default="A", choices=["A", "B"])
            s.add_argument("--out", default="port/data/hidden_poses.txt")
        if name == "bake":
            s.add_argument("--poses", default="port/data/hidden_poses.txt")
            s.add_argument("--configs", help="cell sizes, e.g. 2x8 (default: all six)")
            s.add_argument("--threads", type=int, default=1, help="threads in each level's process")
        if name == "evaluate":
            s.add_argument("--tables", required=True, help="the bake's out dir (one subdir a cell size)")
            s.add_argument("--set", default="B", choices=["A", "B"])
            s.add_argument("--first", default="2x8", help="the cell size the frames skip with")
            s.add_argument("--break", dest="break_check", action="store_true")
        if name == "check":
            s.add_argument("--break", dest="break_check", action="store_true")
            s.add_argument("--laps", type=int, default=1)
    s = sub.add_parser("merge")
    s.add_argument("files", nargs="+")
    s.add_argument("--out", required=True)
    s = sub.add_parser("gen")
    s.add_argument("--tables", required=True)
    s.add_argument("--out", default="port/src/game/hidden_tables.inc")
    s.add_argument("--poses", default="port/data/hidden_poses.txt")
    s = sub.add_parser("stale")
    s.add_argument("--inc", default="port/src/game/hidden_tables.inc")
    s.add_argument("--poses", default="port/data/hidden_poses.txt")
    args = p.parse_args()
    return globals()["cmd_" + args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
