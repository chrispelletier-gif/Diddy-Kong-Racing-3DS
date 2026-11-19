#!/usr/bin/env python3
"""
Backend replay harness (roadmap P56 package D): checks the 3DS backend, port/3ds/gfx_c3d.c, on the PC.

The host build records what its renderer frontend hands the backend in the frame regression scenarios
(port/host/backend_record.c); port/host/c3d_replay/ plays those records through gfx_c3d.c and the real citro3d built
for the PC, and writes down the PICA200 command words of every draw and the GPU register state at it. Two checks:

  words  the same records through this commit's backend and the previous commit's, for a New and an Old 3DS: any
         draw whose command words or vertex data differ. Fails unless the head commit's message (or $PR_BODY)
         says "backend-change: expected", for a change meant to change the words (then it reports only).
  state  this commit's backend twice, with every setting sent for every draw (DKR_STATE_CACHE=0 DKR_FAST_STATE=0) and
         with the defaults (only changed settings sent): the registers must hold the same values at every draw. Any
         difference is a state cache bug. It doesn't see ordering (which registers a draw still reads while the next
         list is written): the Crescent Island hang was that kind, texture registers written right after a draw.

  backend_replay.py deps --dir DEPS
  backend_replay.py build --deps DEPS --port PORT_DIR --out DIR
  backend_replay.py record --exe port/build/host/dkr --rom baserom.z64 --out RECORDS
  backend_replay.py check --records RECORDS --head HEAD_REPLAY [--base BASE_REPLAY] --out DIR [--expected]
  backend_replay.py ci --deps DEPS --exe dkr --rom baserom.z64 --work DIR [--base-port OLD/port]

`ci` does all four (the "Backend replay" step in .github/workflows/Port.yml). Writes DIR/report.md (also appended to
$GITHUB_STEP_SUMMARY), DIR/words-diff.txt and DIR/state-diff.txt. Exit 1 when a check fails or something breaks.
"""
import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PORT_DIR = os.path.dirname(TOOLS_DIR)
HARNESS_DIR = os.path.join(PORT_DIR, "host", "c3d_replay")
sys.path.insert(0, TOOLS_DIR)
import frame_regression  # noqa: E402  (the scenarios)

# The versions the 3DS build's devkitARM image (Port.yml's build-3ds container) ships: its pacman database says
# libctru 2.7.0-1, citro3d 1.7.1-2, picasso 2.7.2-3. Update with the image.
DEPS = {
    "libctru": ("https://github.com/devkitPro/libctru", "36fe1ada5b7ebe53ba4decda36d764a55f8fefb6"),  # v2.7.0
    "citro3d": ("https://github.com/devkitPro/citro3d", "9f21cf7b380ce6f9e01a0420f19f0763e5443ca7"),  # v1.7.1
    "picasso": ("https://github.com/devkitPro/picasso", "c0275f4697e412d6dafb19487abd342bbc111eac"),  # v2.7.2
}

# Recorded frames: every RECORD_EVERY-th frame's draws (textures and meshes always), which keeps a record ~10 KB a
# frame and a replay's words ~20 KB.
RECORD_EVERY = 3
# One thread: the walk's and the backend's calls in the same order every run. The rest are the defaults, so the GPU
# vertex maths (DKR_GPU_TRANSFORM, off by default since #406) and its model and mesh draws are not in the records.
RECORD_ENV = {"DKR_SPLIT_RENDER": "0"}
# Every replay runs with the backend's defaults (plat_old_3ds() from REPLAY_OLD3DS) unless a check sets a switch.
REPLAY_ENV = {"REPLAY_QUIET": "1"}
CONSOLES = [("New 3DS", {"REPLAY_OLD3DS": "0"}), ("Old 3DS", {"REPLAY_OLD3DS": "1"})]
STATE_FULL = {"DKR_STATE_CACHE": "0", "DKR_FAST_STATE": "0"}
MAX_SHOWN = 8
MAX_DIFF_LINES = 2000

# PICA200 register names for the state report (as /mnt/project-files/3ds/tools/pica_decode.py has them).
NAMES = {
    0x040: "CULL", 0x041: "VIEWPORT_WIDTH", 0x043: "VIEWPORT_HEIGHT", 0x04D: "DEPTHMAP_SCALE",
    0x04E: "DEPTHMAP_OFFSET", 0x065: "SCISSOR", 0x066: "SCISSOR_POS", 0x067: "SCISSOR_DIM", 0x068: "VIEWPORT_XY",
    0x06D: "DEPTHMAP_ENABLE", 0x080: "TEXUNIT_CONFIG", 0x081: "T0_BORDER", 0x082: "T0_DIM", 0x083: "T0_PARAM",
    0x084: "T0_LOD", 0x085: "T0_ADDR", 0x08E: "T0_TYPE", 0x091: "T1_BORDER", 0x092: "T1_DIM", 0x093: "T1_PARAM",
    0x094: "T1_LOD", 0x095: "T1_ADDR", 0x096: "T1_TYPE", 0x0E0: "TEXENV_BUF / FOG_MODE", 0x0E1: "FOG_COLOR",
    0x0FD: "TEXENV_BUF_COLOR", 0x100: "COLOR_OP", 0x101: "BLEND_FUNC", 0x102: "LOGIC_OP", 0x103: "BLEND_COLOR",
    0x104: "ALPHA_TEST", 0x105: "STENCIL", 0x106: "STENCIL_OP", 0x107: "DEPTH_COLOR_MASK", 0x116: "COLORBUFFER_FORMAT",
    0x117: "DEPTHBUFFER_FORMAT", 0x11C: "DEPTHBUFFER_LOC", 0x11D: "COLORBUFFER_LOC", 0x11E: "FRAMEBUFFER_DIM",
    0x200: "ATTR_BASE", 0x201: "ATTR_FMT_LO", 0x202: "ATTR_FMT_HI", 0x227: "INDEXBUFFER_CONFIG",
    0x228: "NUM_VERTICES", 0x22A: "VERTEX_OFFSET", 0x2C0: "VSH_FLOATUNIFORM_INDEX",
}
for _i, _b in enumerate([0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8]):
    for _j, _n in enumerate(["SRC", "OP", "FUNC", "COLOR", "SCALE"]):
        NAMES[_b + _j] = "TEV%d_%s" % (_i, _n)
for _k in range(12):
    NAMES[0x203 + 3 * _k] = "BUF%d_OFFSET" % _k
    NAMES[0x204 + 3 * _k] = "BUF%d_CFG1" % _k
    NAMES[0x205 + 3 * _k] = "BUF%d_CFG2" % _k
# The replay's port memories, in replay.c's order.
PORTS = ["fog table", "proctex table", "light table", "fixed attributes", "gs uniforms", "gs code", "gs opdesc",
         "vs uniforms", "vs code", "vs opdesc"]


def key_name(key):
    if key < 0x400:
        return "reg 0x%03x %s" % (key, NAMES.get(key, ""))
    port, word = (key >> 16) - 1, key & 0xFFFF
    if PORTS[port].endswith("uniforms"):
        return "%s c%d.%s" % (PORTS[port], word // 4, "xyzw"[word % 4])
    return "%s [%d]" % (PORTS[port], word)


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def fetch_deps(dest):
    """Checks out the pinned libctru, citro3d and picasso into dest and builds picasso. Reuses a checkout at the
    right commit (a cache)."""
    os.makedirs(dest, exist_ok=True)
    for name, (url, commit) in DEPS.items():
        path = os.path.join(dest, name)
        have = None
        if os.path.isdir(os.path.join(path, ".git")):
            have = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        if have != commit:
            if not os.path.isdir(os.path.join(path, ".git")):
                run(["git", "clone", "-q", url, path])
            if subprocess.run(["git", "-C", path, "checkout", "-q", "--detach", commit]).returncode != 0:
                run(["git", "-C", path, "fetch", "-q", "origin", commit])
                run(["git", "-C", path, "checkout", "-q", "--detach", commit])
        have = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        if have != commit:
            raise SystemExit("%s is at %s, not the pinned %s" % (name, have, commit))
    picasso = os.path.join(dest, "picasso", "picasso")
    if not os.path.exists(picasso):
        src = os.path.join(dest, "picasso", "source")
        run(["g++", "-O2", "-std=gnu++11", '-DPACKAGE_STRING="picasso 2.7.2"', "-o", picasso,
             os.path.join(src, "picasso_assembler.cpp"), os.path.join(src, "picasso_frontend.cpp")])


def build(deps, port, out):
    run(["make", "-s", "-C", HARNESS_DIR, "-j%d" % (os.cpu_count() or 2), "DEPS=" + os.path.abspath(deps),
         "PORT=" + os.path.abspath(port), "OUT=" + os.path.abspath(out)])
    return os.path.join(os.path.abspath(out), "c3d_replay")


def record_one(exe, rom, scenario, out_dir, every):
    run_dir = os.path.join(out_dir, "run-" + scenario["name"])
    os.makedirs(run_dir, exist_ok=True)
    link = os.path.join(run_dir, "baserom.z64")
    if not os.path.exists(link):
        os.symlink(os.path.abspath(rom), link)
    if "save" in scenario:
        with open(os.path.join(frame_regression.TOOLS_DIR, "testdata", scenario["save"]), "rb") as src, \
                open(os.path.join(run_dir, "eeprom.bin"), "wb") as dst:
            dst.write(src.read())
    path = os.path.join(out_dir, scenario["name"] + ".rec")
    env = dict(os.environ)
    env.update(scenario["env"])
    env.update(RECORD_ENV)
    env.update({"DKR_LOCKSTEP": "1", "DKR_DATA_DIR": run_dir, "DKR_FRAMES": str(scenario["frames"]),
                "DKR_BACKEND_RECORD": path, "DKR_BACKEND_RECORD_EVERY": str(every)})
    with open(os.path.join(run_dir, "log.txt"), "w") as log:
        result = subprocess.run([os.path.abspath(exe)], env=env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=frame_regression.TIMEOUT_SECONDS)
    if result.returncode != 0:
        raise SystemExit("recording %s: the host build exited with %d (log %s/log.txt)"
                         % (scenario["name"], result.returncode, run_dir))
    return path


def record(exe, rom, out_dir, every=RECORD_EVERY):
    frame_regression.prepare_scenarios()
    os.makedirs(out_dir, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(frame_regression.SCENARIOS)) as pool:
        jobs = [pool.submit(record_one, exe, rom, s, out_dir, every) for s in frame_regression.SCENARIOS]
        return [j.result() for j in jobs]


def replay(exe, rec, env, words=None, state=None):
    cmd = [exe, rec] + (["--words", words] if words else []) + (["--state", state] if state else [])
    full_env = dict(os.environ)
    full_env.update(REPLAY_ENV)
    full_env.update(env)
    result = subprocess.run(cmd, env=full_env, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("%s %s: exit %d\n%s" % (exe, rec, result.returncode, result.stderr[-2000:]))
    # "frames N draws N words N words/draw X jobs N"
    f = result.stdout.split()
    return {f[i]: float(f[i + 1]) for i in range(0, len(f) - 1, 2)}


def first_difference(a, b):
    wa, wb = a.split(), b.split()
    for i in range(min(len(wa), len(wb))):
        if wa[i] != wb[i]:
            return i
    return min(len(wa), len(wb))


def compare_words(base_path, head_path, label, diff_out):
    """Line by line (a draw or a GPU job a line). Returns (lines, differing, shown) with shown the first few."""
    lines = differing = 0
    shown = []
    with open(base_path) as fa, open(head_path) as fb:
        while True:
            a, b = fa.readline(), fb.readline()
            if not a and not b:
                break
            lines += 1
            if a == b:
                continue
            differing += 1
            where = (a or b).split(":")[0] if ":" in (a or b) else (a or b).split(" job")[0]
            if not a or not b:
                text = "%s: frame/draw %s only in %s" % (label, where.strip(), "head" if not a else "base")
            else:
                i = first_difference(a, b)
                wa, wb = a.split(), b.split()
                text = "%s: frame %s, first difference at word %d of %d/%d:\n  base ...%s\n  head ...%s" % (
                    label, where.strip(), i, len(wa), len(wb), " ".join(wa[max(0, i - 2):i + 8]),
                    " ".join(wb[max(0, i - 2):i + 8]))
            if len(shown) < MAX_SHOWN:
                shown.append(text)
            if differing <= MAX_DIFF_LINES:
                diff_out.write(text + "\n")
    return lines, differing, shown


def read_states(path):
    with open(path, "rb") as f:
        data = f.read()
    o = 0
    while o < len(data):
        frame, draw, n = struct.unpack_from("<3I", data, o)
        o += 12
        pairs = struct.unpack_from("<%dI" % (2 * n), data, o)
        o += 8 * n
        yield frame, draw, pairs


def compare_states(full_path, cached_path, label, diff_out):
    """At every draw, every register and port word either run has set. Returns (draws, differing, shown)."""
    va, vb, bad = {}, {}, set()
    draws = differing = 0
    shown = []
    ia, ib = read_states(full_path), read_states(cached_path)
    while True:
        a, b = next(ia, None), next(ib, None)
        if a is None or b is None:
            if a is not None or b is not None:
                text = "%s: the runs have different numbers of draws (after %d)" % (label, draws)
                shown.append(text)
                diff_out.write(text + "\n")
                differing += 1
            break
        if a[:2] != b[:2]:
            text = "%s: draw %d is frame %d draw %d in one run, frame %d draw %d in the other" % (
                label, draws, a[0], a[1], b[0], b[1])
            shown.append(text)
            diff_out.write(text + "\n")
            differing += 1
            break
        for vals, pairs in ((va, a[2]), (vb, b[2])):
            for i in range(0, len(pairs), 2):
                vals[pairs[i]] = pairs[i + 1]
        for k in set(a[2][0::2]) | set(b[2][0::2]):
            if va.get(k, 0) != vb.get(k, 0):
                bad.add(k)
            else:
                bad.discard(k)
        draws += 1
        if bad:
            differing += 1
            text = "%s: frame %d draw %d: %s" % (label, a[0], a[1], "; ".join(
                "%s all-set 0x%08x, cached 0x%08x" % (key_name(k), va.get(k, 0), vb.get(k, 0)) for k in sorted(bad)[:6]))
            if len(shown) < MAX_SHOWN:
                shown.append(text)
            if differing <= MAX_DIFF_LINES:
                diff_out.write(text + "\n")
    return draws, differing, shown


def check(records, head, base, out_dir, expected):
    """Runs both checks over every record; writes the report. Returns True when both pass."""
    os.makedirs(out_dir, exist_ok=True)
    report = ["## Backend replay (3DS backend on the PC, P56 D)", ""]
    ok = True
    jobs = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for rec in records:
            name = os.path.basename(rec)[:-len(".rec")]
            for console, env in CONSOLES:
                tag = "%s-%s" % (name, env["REPLAY_OLD3DS"])
                jobs[("head", tag)] = pool.submit(replay, head, rec, env, os.path.join(out_dir, tag + ".head.words"),
                                                  os.path.join(out_dir, tag + ".cached.state"))
                full = dict(env)
                full.update(STATE_FULL)
                jobs[("full", tag)] = pool.submit(replay, head, rec, full, None,
                                                  os.path.join(out_dir, tag + ".full.state"))
                if base:
                    jobs[("base", tag)] = pool.submit(replay, base, rec, env,
                                                      os.path.join(out_dir, tag + ".base.words"))
        results = {}
        for k, j in jobs.items():
            try:
                results[k] = j.result()
            except RuntimeError as e:
                results[k] = None
                report.append("- replay failed (%s %s):\n\n```\n%s\n```" % (k[0], k[1], e))
                if k[0] != "base":
                    ok = False

    words_rows, state_rows = [], []
    words_shown, state_shown = [], []
    words_bad = state_bad = 0
    with open(os.path.join(out_dir, "words-diff.txt"), "w") as wd, \
            open(os.path.join(out_dir, "state-diff.txt"), "w") as sd:
        for rec in records:
            name = os.path.basename(rec)[:-len(".rec")]
            for console, env in CONSOLES:
                tag = "%s-%s" % (name, env["REPLAY_OLD3DS"])
                label = "%s, %s" % (name, console)
                h, f = results.get(("head", tag)), results.get(("full", tag))
                if base and h is not None and results.get(("base", tag)) is not None:
                    b = results[("base", tag)]
                    lines, diff, shown = compare_words(os.path.join(out_dir, tag + ".base.words"),
                                                       os.path.join(out_dir, tag + ".head.words"), label, wd)
                    words_bad += diff
                    words_shown += shown
                    words_rows.append("| %s | %d | %d | %d | %.1f | %.1f |" % (
                        label, b["draws"], h["draws"], diff, b["words/draw"], h["words/draw"]))
                if h is not None and f is not None:
                    draws, diff, shown = compare_states(os.path.join(out_dir, tag + ".full.state"),
                                                        os.path.join(out_dir, tag + ".cached.state"), label, sd)
                    state_bad += diff
                    state_shown += shown
                    state_rows.append("| %s | %d | %d | %.1f | %.1f |" % (
                        label, draws, diff, f["words/draw"], h["words/draw"]))

    report.append("### Words: this commit's backend against the previous commit's, same records")
    report.append("")
    if not base:
        report.append("No previous commit's backend to compare with (it doesn't build here, or this is the first).")
    else:
        report.append("| record | draws before | draws now | draws differing | words/draw before | words/draw now |")
        report.append("|---|---|---|---|---|---|")
        report += words_rows
        report.append("")
        if words_bad:
            if expected:
                report.append("**%d draws differ; expected (\"backend-change: expected\"), so reported only.**"
                              % words_bad)
            else:
                report.append("**%d draws differ.** If the change is meant to change the command words, put "
                              "\"backend-change: expected\" in the commit message (or the PR description)." % words_bad)
                ok = False
            report.append("")
            report.append("```")
            report += words_shown[:MAX_SHOWN]
            report.append("```")
        else:
            report.append("Every draw's command words and vertex data are the same.")
    report.append("")
    report.append("### State: only changed settings sent (the defaults) against everything sent for every draw")
    report.append("")
    report.append("| record | draws | draws whose registers differ | words/draw all set | words/draw cached |")
    report.append("|---|---|---|---|---|")
    report += state_rows
    report.append("")
    if state_bad:
        ok = False
        report.append("**%d draws see different GPU registers: a state cache bug.**" % state_bad)
        report.append("")
        report.append("```")
        report += state_shown[:MAX_SHOWN]
        report.append("```")
    else:
        report.append("The GPU registers are the same at every draw. (Ordering against the GPU, such as texture "
                      "registers written right after a draw, is not modelled.)")
    text = "\n".join(report) + "\n"
    with open(os.path.join(out_dir, "report.md"), "w") as f:
        f.write(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(text)
    print(text)
    return ok


def expected_change():
    text = os.environ.get("PR_BODY", "")
    try:
        text += subprocess.run(["git", "-C", PORT_DIR, "log", "-1", "--format=%B"], capture_output=True,
                               text=True).stdout
    except OSError:
        pass
    return "backend-change: expected" in text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("deps")
    p.add_argument("--dir", required=True)
    p = sub.add_parser("build")
    p.add_argument("--deps", required=True)
    p.add_argument("--port", default=PORT_DIR)
    p.add_argument("--out", required=True)
    p = sub.add_parser("record")
    p.add_argument("--exe", required=True)
    p.add_argument("--rom", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--every", type=int, default=RECORD_EVERY)
    p = sub.add_parser("check")
    p.add_argument("--records", required=True)
    p.add_argument("--head", required=True)
    p.add_argument("--base")
    p.add_argument("--out", required=True)
    p.add_argument("--expected", action="store_true")
    p = sub.add_parser("ci")
    p.add_argument("--deps", required=True)
    p.add_argument("--exe", required=True)
    p.add_argument("--rom", required=True)
    p.add_argument("--work", required=True)
    p.add_argument("--base-port")
    p.add_argument("--every", type=int, default=RECORD_EVERY)
    args = ap.parse_args()

    if args.cmd == "deps":
        fetch_deps(args.dir)
    elif args.cmd == "build":
        print(build(args.deps, args.port, args.out))
    elif args.cmd == "record":
        for r in record(args.exe, args.rom, args.out, args.every):
            print(r, os.path.getsize(r))
    elif args.cmd == "check":
        recs = sorted(os.path.join(args.records, n) for n in os.listdir(args.records) if n.endswith(".rec"))
        sys.exit(0 if check(recs, args.head, args.base, args.out, args.expected or expected_change()) else 1)
    else:
        fetch_deps(args.deps)
        head = build(args.deps, PORT_DIR, os.path.join(args.work, "replay-head"))
        base = None
        if args.base_port:
            try:
                base = build(args.deps, args.base_port, os.path.join(args.work, "replay-base"))
            except subprocess.CalledProcessError:
                print("::warning::The previous commit's backend doesn't build in the replay; words not compared.")
        recs = record(args.exe, args.rom, os.path.join(args.work, "records"), args.every)
        out = os.path.join(args.work, "out")
        ok = check(recs, head, base, out, expected_change())
        # The replays' outputs are hundreds of MB; the report and the diffs stay.
        for name in os.listdir(out):
            if name.endswith(".words") or name.endswith(".state"):
                os.remove(os.path.join(out, name))
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
