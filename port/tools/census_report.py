#!/usr/bin/env python3
"""
Turns census.csv (port/tools/census_run.py, DKR_WALK_CENSUS=1 host runs) into the to-do lists for the native renderer's
finish stages (docs: proposals/3ds-native-renderer-finish-plan.md): G1 menus, G2 race leftovers, G3 camera, matrices and
billboards, R2 records for the leftovers, R3 texture handles, R4 the flat record stream.

Usage: census_report.py census.csv [--binary port/build/host/dkr] [--out report.md] [--min 0.01]

Counts are commands per frame, averaged over the frames of the scenario's steady phase (90 frames after each level
load are the "load" phase, listed apart). A site is the game's source line (file:line) that wrote the command. A
command in a list of constants has no line of its own: it is named "static@+0xOFFSET via file:line" (a list in the
program, called from that line; --binary resolves the offset to a symbol with nm) or "heap list via file:line" (a
texture's load list and the like).
"""
import argparse
import collections
import csv
import re
import subprocess
import sys

LEVELS = """CENTRALAREAHUB BLUEY1 DRAGONFORESTHUB FOSSILCANYON PIRATELAGOON ANCIENTLAKE WALRUSCOVE HOTTOPVOLCANO WHALEBAY
SNOWBALLVALLEY CRESCENTISLAND FIREMOUNTAIN DINODOMAINHUB EVERFROSTPEAK SHERBETISLANDHUB SPACEPORTALPHA HORSESHOEGULCH
SPACEDUSTALLEY GREENWOODVILLAGE BOULDERCANYON WINDMILLPLAINS FRONTEND CHARACTERSELECT TITLESCREENSEQUENCE
SNOWFLAKEMOUNTAINHUB SMOKEYCASTLE DARKWATERBEACH ICICLEPYRAMID FROSTYVILLAGE JUNGLEFALLS TREASURECAVES HAUNTEDWOODS
DARKMOONCAVERNS STARCITY TROPHYRACE FUTUREFUNLANDHUB OPENINGSEQUENCE WIZPIG1 TRICKYTOPS1 OPTIONSBACKGROUND BUBBLER1
SMOKEY1 WIZPIGMOUTHSEQUENCE WIZPIGAMULETSEQUENCE TTAMULETSEQUENCE ROCKETSEQUENCE TRICKYTOPS2 DINODOMAINTROPHYANIM
SNOWFLAKEMOUNTAINTROPHYANIM SHERBETISLANDTROPHYANIM DRAGONFORESTTROPHYANIM FUTUREFUNLANDTROPHYANIM BLUEY2 BUBBLER2
SMOKEY2 WIZPIG2 PARTYSEQUENCE TRICKTOPSANIM SMOKEYANIM BLUEYANIM WIZPIG1ANIM BUBBLERANIM WIZPIG2ANIM LASTBIT
LASTBITB""".split()

# The leftover classes, and what each one means for the stages.
LEFTOVER = ["vertex_slot", "tmem", "matrix", "list_call", "raw_n64", "rect"]
RECORD_NOTES = ["record_slots", "record_dropped"]
ALL_CLASSES = ["record", "state", "vertex_slot", "tmem", "matrix", "list_call", "raw_n64", "rect", "record_slots",
               "record_dropped"]

MENU_FILES = ("src/menu.c", "src/font.c", "src/borders.c")
RACE_FILES = ("src/objects.c", "src/game_ui.c", "src/particles.c", "src/waves.c", "src/weather.c", "src/tracks.c")
CAMERA_FILES = ("src/camera.c", "src/rcp_dkr.c", "src/fade_transition.c", "src/thread3_main.c")
MENU_SCENARIOS = ("menus-", "cutscenes", "intro")


def level_name(n):
    return LEVELS[n] if 0 <= n < len(LEVELS) else str(n)


def read_rows(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            r["level"] = int(r["level"])
            r["total"] = int(r["total"])
            r["frames"] = int(r["frames"])
            rows.append(r)
    return rows


def symbols(binary):
    """(address, size, name) from nm, sorted, for static@+offset sites."""
    out = subprocess.run(["nm", "-n", "-S", "--defined-only", binary], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
        elif len(p) == 3:
            syms.append((int(p[0], 16), 0, p[2]))
    return syms


def resolve(site, syms):
    m = re.match(r"static@\+0x([0-9a-f]+) via (.*)", site)
    if not m or not syms:
        return site
    off = int(m.group(1), 16)
    best = None
    for a, s, n in syms:
        if a > off:
            break
        if off < a + max(s, 1) or best is None:
            best = (a, s, n)
    if best is not None:
        a, s, n = best
        return "static %s+0x%x via %s" % (n, off - a, m.group(2)) if off - a else "static %s via %s" % (n, m.group(2))
    return site


def site_file(site):
    m = re.search(r"((?:src|include|port)/[A-Za-z0-9_./]+\.[ch]):\d+", site)
    return m.group(1) if m else None


def stage_of(row, site):
    """Which stage clears this leftover (G1/G2/G3), by the game file that wrote or called it."""
    f = site_file(site)
    cls = row["class"]
    scenario = row["scenario"]
    menu = row["kind"] in ("other",) and scenario.startswith(MENU_SCENARIOS)
    if f is None:
        return "?"
    if f in CAMERA_FILES or (f == "src/weather.c" and cls == "matrix"):
        return "G3"
    if f in MENU_FILES:
        return "G1"
    if f == "src/textures_sprites.c":
        return "G1" if menu else "G2"
    if f in RACE_FILES:
        return "G2"
    if f.startswith("port/") or f.startswith("include/"):
        return "port"
    return "other:" + f


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--binary")
    ap.add_argument("--out", default="-")
    ap.add_argument("--min", type=float, default=0.01, help="leave out sites below this many commands a frame")
    args = ap.parse_args()
    rows = read_rows(args.csv)
    syms = symbols(args.binary) if args.binary else None

    # Frames per (scenario, level, phase)
    frames = {}
    for r in rows:
        frames[(r["scenario"], r["level"], r["phase"])] = r["frames"]
    scen_frames = collections.defaultdict(int)  # steady frames per scenario
    scen_load = collections.defaultdict(int)
    for (s, lv, ph), n in frames.items():
        (scen_frames if ph == "steady" else scen_load)[s] += n
    scenarios = sorted({r["scenario"] for r in rows})

    out = []
    w = out.append
    w("# Walk census\n")
    w("Commands the renderer's display list walk executes, per frame (steady frames only; the first 90 frames after "
      "each level load are counted apart as \"load\"). Frames: %s.\n" % ", ".join(
          "%s %d (+%d load)" % (s, scen_frames[s], scen_load[s]) for s in scenarios))

    # 1. Class totals per scenario
    w("## Classes per scenario (commands per steady frame)\n")
    head = "| scenario | " + " | ".join(ALL_CLASSES) + " |"
    w(head)
    w("|" + " --- |" * (len(ALL_CLASSES) + 1))
    for s in scenarios:
        tot = collections.Counter()
        for r in rows:
            if r["scenario"] == s and r["phase"] == "steady" and r["class"] in ALL_CLASSES:
                tot[r["class"]] += r["total"] if "+material" not in r["op"] or r["class"] not in ("tmem", "list_call", "state") else 0
        n = max(scen_frames[s], 1)
        w("| %s | %s |" % (s, " | ".join("%.1f" % (tot[c] / n) for c in ALL_CLASSES)))
    w("\nTexture and list ops inside a material capture (the first use of a material) are left out of the table "
      "above and counted in the R3 section below.\n")

    # 2. Leftovers by site
    # key: (stage, resolved site, class, op) -> per scenario totals
    sites = collections.defaultdict(lambda: {"scen": collections.Counter(), "load": 0, "kinds": collections.Counter(),
                                              "levels": collections.Counter(), "total": 0})
    for r in rows:
        cls = r["class"]
        material = "+material" in r["op"]
        if cls not in LEFTOVER + RECORD_NOTES:
            continue
        if r["op"].startswith("CALL") and not material and False:
            continue
        site = resolve(r["site"], syms)
        op = r["op"].replace("+material", "")
        stage = stage_of(r, r["site"])
        if material:
            stage = "R3-material"
        elif cls in RECORD_NOTES:
            stage = "R2"
        elif cls == "tmem":
            stage = stage + "+R3"
        key = (stage, site, cls, op)
        e = sites[key]
        if r["phase"] == "steady":
            e["scen"][r["scenario"]] += r["total"]
            e["kinds"][r["kind"]] += r["total"]
            e["levels"][r["level"]] += r["total"]
        else:
            e["load"] += r["total"]
        e["total"] += r["total"]

    def pf(e):
        # The most a frame in any one scenario (steady), the scenario, and the busiest level
        best, bs = 0.0, ""
        for s, t in e["scen"].items():
            v = t / max(scen_frames[s], 1)
            if v > best:
                best, bs = v, s
        return best, bs

    stages = collections.defaultdict(list)
    for key, e in sites.items():
        best, bs = pf(e)
        if best >= args.min or e["load"] > 0 and best == 0:
            stages[key[0]].append((best, bs, key, e))

    order = [("G1", "G1 Menus to records (DKR_NATIVE_MENUS): src/menu.c, font.c, borders.c, menu-only textures_sprites.c"),
             ("G2", "G2 Race leftovers to records (DKR_NATIVE_LEFTOVERS)"),
             ("G3", "G3 Frame set-up, camera, matrices and billboards (DKR_NATIVE_CAMERA)"),
             ("R2", "R2 Renderer support: records that still go through the vertex slots, and dropped records"),
             ("G1+R3", "Texture ops written directly by menu code (G1 removes them; R3 handles the rest)"),
             ("G2+R3", "Texture ops written directly by race code (G2 removes them; R3 handles the rest)"),
             ("G3+R3", "Texture ops written directly by camera/frame code"),
             ("R3-material", "R3 Texture handles: texture and list ops run inside material captures (first use of a material)"),
             ("port", "Written by the port's own code (not a game site)"),
             ("?", "No site known (written outside the gbi macros, or before the census could see it)")]
    other = sorted(k for k in stages if k not in {o[0] for o in order})
    for k in other:
        order.append((k, "Other: " + k))

    for stage, title in order:
        items = stages.get(stage)
        if not items:
            continue
        items.sort(key=lambda x: -x[0])
        w("## " + title + "\n")
        w("| a frame (steady, busiest scenario) | scenario | site | class | op | kinds | busiest levels | load total |")
        w("| --- | --- | --- | --- | --- | --- | --- | --- |")
        for best, bs, key, e in items[:200]:
            kinds = ", ".join("%s %d%%" % (k, 100 * v // max(sum(e["kinds"].values()), 1))
                              for k, v in e["kinds"].most_common(3))
            levels = ", ".join(level_name(lv) for lv, _ in e["levels"].most_common(3))
            w("| %.2f | %s | `%s` | %s | %s | %s | %s | %d |" % (best, bs, key[1], key[2], key[3], kinds, levels, e["load"]))
        if len(items) > 200:
            w("\n(%d more rows below the cut)\n" % (len(items) - 200))
        w("")

    # 3. Zero check for the finish line: every scenario with a leftover class above zero
    w("## Finish-line check\n")
    w("The finish line (plan) is zero vertex-slot draws, zero texture ops, zero fixed matrices and zero list calls "
      "outside materials, in every scenario, with all the new switches on. Today:\n")
    w("| class | commands a frame (all scenarios, steady) | scenarios with any |")
    w("| --- | --- | --- |")
    for c in ["vertex_slot", "tmem", "matrix", "list_call", "raw_n64"]:
        tot = collections.Counter()
        for r in rows:
            if r["class"] == c and r["phase"] == "steady" and "+material" not in r["op"]:
                tot[r["scenario"]] += r["total"]
        n = sum(scen_frames.values())
        w("| %s | %.2f | %s |" % (c, sum(tot.values()) / max(n, 1), ", ".join(sorted(s for s in tot if tot[s] > 0)) or "none"))
    text = "\n".join(out) + "\n"
    if args.out == "-":
        sys.stdout.write(text)
    else:
        with open(args.out, "w") as f:
            f.write(text)
        print("wrote", args.out)


if __name__ == "__main__":
    main()
