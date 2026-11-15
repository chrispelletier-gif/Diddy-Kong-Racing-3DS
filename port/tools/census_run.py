#!/usr/bin/env python3
"""
Plays the walk census scenarios (port/host/scenarios/*.scn) on the host build and merges their census.csv files.

Usage: census_run.py --rom baserom.z64 --exe port/build/host/dkr --out DIR [--only a,b] [--jobs N] [--dump-every N]

Each scenario file is "key = value" lines (# starts a comment):
  about        a sentence for the report
  frames       the frames to play, or "auto" (with warp: the level list's length)
  warp         DKR_WARP (all, or level ids)         warp_seconds  DKR_WARP_SECONDS
  save         a file in port/tools/testdata used as the EEPROM save
  input        DKR_INPUT (the scripted buttons, see port/src/plat/input_script.c)
  every        write a frame every N frames (default 0: none)
  env.NAME     any other environment variable
Runs are DKR_LOCKSTEP=1 with DKR_WALK_CENSUS=1. Writes DIR/<scenario>/{census.csv,log.txt,frames/} and DIR/census.csv
(every scenario's rows together). Feed that to census_report.py.
"""
import argparse
import concurrent.futures
import csv
import glob
import os
import shutil
import subprocess
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
SCENARIO_DIR = os.path.join(TOOLS_DIR, "..", "host", "scenarios")
TIMEOUT_SECONDS = 1800
BOOT_FRAMES = 450
LEVEL_LOAD_FRAMES = 30


def read_scenario(path):
    s = {"name": os.path.splitext(os.path.basename(path))[0], "env": {}}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, _, value = line.partition("=")
            key, value = key.strip(), value.strip()
            if key.startswith("env."):
                s["env"][key[4:]] = value
            else:
                s[key] = value
    return s


def frames_of(s):
    if s.get("frames", "auto") != "auto":
        return int(s["frames"])
    levels = len(s["warp"].split(","))
    seconds = int(s.get("warp_seconds", "20"))
    return BOOT_FRAMES + (seconds * 30 + LEVEL_LOAD_FRAMES) * levels


def run(exe, rom, s, out_dir, dump_every):
    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    frames_dir = os.path.join(out_dir, "frames")
    os.makedirs(frames_dir)
    os.symlink(os.path.abspath(rom), os.path.join(out_dir, "baserom.z64"))
    if "save" in s:
        shutil.copy(os.path.join(TOOLS_DIR, "testdata", s["save"]), os.path.join(out_dir, "eeprom.bin"))
    env = dict(os.environ)
    env.update(s["env"])
    every = int(s.get("every", dump_every))
    env.update({
        "DKR_LOCKSTEP": "1",
        "DKR_WALK_CENSUS": "1",
        "DKR_CENSUS_SCENARIO": s["name"],
        "DKR_DATA_DIR": out_dir,
        "DKR_FRAMES": str(frames_of(s)),
    })
    if every > 0:
        env["DKR_DUMP_EVERY"] = str(every)
        env["DKR_DUMP_DIR"] = frames_dir
    if "warp" in s:
        env["DKR_WARP"] = s["warp"]
        env["DKR_WARP_SECONDS"] = s.get("warp_seconds", "20")
    if "input" in s:
        env["DKR_INPUT"] = s["input"]
    start = time.time()
    with open(os.path.join(out_dir, "log.txt"), "w") as log:
        try:
            result = subprocess.run([os.path.abspath(exe)], env=env, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=TIMEOUT_SECONDS, cwd=out_dir)
        except subprocess.TimeoutExpired:
            return s["name"], False, "did not finish within %d s" % TIMEOUT_SECONDS, time.time() - start
    if result.returncode != 0:
        return s["name"], False, "exited with code %d" % result.returncode, time.time() - start
    if not os.path.exists(os.path.join(out_dir, "census.csv")):
        return s["name"], False, "wrote no census.csv", time.time() - start
    if not os.listdir(frames_dir):
        os.rmdir(frames_dir)
    return s["name"], True, "", time.time() - start


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--exe", required=True, help="the host build (port/build/host/dkr)")
    parser.add_argument("--out", required=True)
    parser.add_argument("--only", help="comma separated scenario names")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--dump-every", type=int, default=0, help="also write a frame every N frames")
    args = parser.parse_args()

    scenarios = [read_scenario(p) for p in sorted(glob.glob(os.path.join(SCENARIO_DIR, "*.scn")))]
    if args.only:
        names = args.only.split(",")
        scenarios = [s for s in scenarios if s["name"] in names]
    os.makedirs(args.out, exist_ok=True)
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run, args.exe, args.rom, s, os.path.join(args.out, s["name"]), args.dump_every)
                   for s in scenarios]
        for future in concurrent.futures.as_completed(futures):
            name, ok, message, seconds = future.result()
            print("%-16s %s in %.0f s %s" % (name, "ok" if ok else "FAILED", seconds, message), flush=True)
            if not ok:
                failed.append(name)

    rows = 0
    with open(os.path.join(args.out, "census.csv"), "w", newline="") as out:
        writer = None
        for s in scenarios:
            path = os.path.join(args.out, s["name"], "census.csv")
            if not os.path.exists(path):
                continue
            with open(path, newline="") as f:
                reader = csv.reader(f)
                header = next(reader)
                if writer is None:
                    writer = csv.writer(out)
                    writer.writerow(header)
                for row in reader:
                    writer.writerow(row)
                    rows += 1
    print("%d rows in %s" % (rows, os.path.join(args.out, "census.csv")))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
