#!/usr/bin/env python3
"""Hot code size and memcpy check for the 3DS ELF (roadmap P56, kohjoh lessons K1 and K10).

The Old 3DS has a 16 KB instruction cache per core. The renderer's per-draw code is meant to fit in it (P38 laid the
PLAT_HOT functions out together in .text.hot at ~16 KB), but nothing stopped it from growing back. This script
measures three numbers from the linked ELF and fails CI when one grows past its budget:

  hot_span     lowest to highest address (+ size) of the functions the source marks PLAT_HOT / HOT (all hot sections)
  walk_set     total size of the display list walk's per-command functions (the game core on the Old 3DS)
  backend_set  total size of the backend's per-draw functions and the citro3d calls they make (the system core)

and, report-only until their budgets are set from CI (P57 D):

  game_set     total size of the functions marked for the game core's stretch (PLAT_HOT_GAME, HOT in gfx_rdp.c):
               the walk's per-draw code and the Old 3DS's native records path, which the game core runs every frame
  game_span    lowest to highest address of that stretch; walk_span and backend_span the same for PLAT_HOT_WALK
               (HOT_WALK) and PLAT_HOT

The stretches must not interleave (P57 D: the two cores have an instruction cache each, so a cache line of the game
core's stretch that holds backend code is wasted on both): a function of one group inside another group's span fails
the check, whatever the budgets.

It also checks that the port's memcpy (libultra/src/libc/string.c) makes no call to memcpy, memmove or memset: GCC
can turn its copy loops into such a call, which would recurse or copy overlapping bytes differently from the N64.

Usage: python3 port/tools/hot_size.py ELF [--nm PATH] [--objdump PATH] [--update] [--report-only]
  --update       rewrite BUDGETS below with this ELF's totals (commit the result when the growth is meant)
  --report-only  print the numbers, never fail on a budget (the memcpy and stretch checks still fail)
"""

import argparse
import os
import re
import subprocess
import sys

# Budgets in bytes, from the CI debug build of PR #565 (master f07de6f + this check), then PR #563 (P54 step 2): its
# backend_set adds walk_draw_ready (the backend's draw of the walk's finished vertices: with DKR_WALK_VERTS on it is
# used instead of gfxb_draw_indexed's lean path, so a frame's draws run one or the other, not both) and the cutting
# shared with the walk (clip_cut_k, clip_triangle_band, which were clip_cut_lean's and clip_triangle's code before).
# A total may exceed its budget by up to SLACK bytes.
# P57 D/G (2026-11-20): the native records path the Old 3DS's game core runs every frame is marked hot too
# (PLAT_HOT_GAME), so hot_span covers the game, walk and backend stretches together and grew by what was added;
# walk_set lost frame_thread (port_gfx_run_frame is out of line, its rare blocks cold); backend_set lost the GPU-draw
# profiler's and the first draw's fill code from gfxb_draw_indexed and walk_draw_ready.
BUDGETS = {"hot_span": 81096, "walk_set": 23100, "backend_set": 21240}
# Report-only totals (no budget yet: printed, never failed on).
REPORTED = ("game_set", "game_span", "walk_span", "backend_span")
SLACK = 1024

# A name ending in "?" is optional: GCC may inline it, or it only exists once a PR that adds it is merged (P54 step 2,
# PR #563: the walk's vertex conversion and the cutting it shares with the backend). Any other name missing from the
# ELF fails the check, so a renamed function can't quietly drop out of a total.
WALK_SET = (
    "run_command_list draw_dkr_triangles load_vertices load_vertices_now make_draw_state lookup_texture "
    "command_off_screen frame_hash tex_memo_confirm load_matrix tex_axis_init handle_call call_display_list "
    "flush_cpu_batch forget_covered_loads shared_vertices_clear port_gfx_run_frame gfxb_walk_draw_indexed?"
).split()

BACKEND_SET = (
    "gfxb_draw_indexed hang_draw_start test_and_write_vertices use_vertex_format bind_texture apply_fog "
    "apply_combiner drain_before_draw set_all_state write_settings_twice? set_draw_state? clip_cut_lean "
    "clip_cut_k? clip_triangle_band? walk_draw_ready? clip_cut_triangles C3Di_UpdateContext C3D_DrawElements C3D_DrawArrays C3D_UpdateUniforms C3Di_SetTex "
    "C3Di_TexEnvBind C3Di_EffectBind C3Di_BufInfoBind C3Di_AttrInfoBind prof_enabled"
).split()

MEMCPY_FORBIDDEN = ("memcpy", "memmove", "memset")

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SOURCE_DIRS = (("port/src", True), ("port/3ds", False))  # (directory, search subdirectories)


def default_tool(name):
    devkitarm = os.environ.get("DEVKITARM", "/opt/devkitpro/devkitARM")
    path = os.path.join(devkitarm, "bin", "arm-none-eabi-" + name)
    return path if os.path.exists(path) else "arm-none-eabi-" + name


def read_symbols(nm, elf):
    """Function symbols with a size: list of (address, size, name, source path or None).

    nm -l adds each symbol's source file from the debug info, which tells apart two static functions with one name
    (the game's tracks.c has an apply_fog too). Without debug info every match counts.
    """
    out = subprocess.run([nm, "-n", "-S", "-l", elf], check=True, capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        head, _, where = line.partition("\t")
        parts = head.split()
        if len(parts) != 4 or parts[2] not in ("t", "T", "w", "W"):
            continue
        path = os.path.normpath(where.rsplit(":", 1)[0]).replace("\\", "/") if where else None
        syms.append((int(parts[0], 16), int(parts[1], 16), parts[3], path))
    return syms


def matches(symbol, name):
    """A symbol matches a name when it equals it or is an LTO / GCC copy of it (name.constprop.0, name.part.0...)."""
    return symbol == name or symbol.startswith(name + ".")


def find(syms, name, where=None):
    """Symbols for a name. where: path fragments the symbol's source file must contain (when nm knows the file)."""
    return [s for s in syms if matches(s[2], name) and
            (where is None or s[3] is None or any(w in s[3] for w in where))]


# Where each set's functions live (the port's renderer and backend, citro3d), so same-named game functions don't count.
WALK_WHERE = ("/port/src/",)
BACKEND_WHERE = ("/port/3ds/", "/port/src/", "citro3d")


def strip_attributes(text):
    """Remove __attribute__((...)) groups, which hold parentheses of their own."""
    while True:
        i = text.find("__attribute__")
        if i < 0:
            return text
        j = text.find("(", i)
        if j < 0:
            return text[:i]
        depth = 0
        k = j
        while k < len(text):
            if text[k] == "(":
                depth += 1
            elif text[k] == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        text = text[:i] + " " + text[k + 1:]


HOT_MARK = re.compile(r"(^|\s)(PLAT_HOT_GAME|PLAT_HOT_WALK|PLAT_HOT|HOT_WALK|HOT)(\s|$)")
# Which stretch each mark puts a function in. HOT is gfx_rdp.c's name for PLAT_HOT_GAME, HOT_WALK for PLAT_HOT_WALK.
MARK_GROUP = {"PLAT_HOT_GAME": "game", "HOT": "game", "PLAT_HOT_WALK": "walk", "HOT_WALK": "walk", "PLAT_HOT": "backend"}
FUNC_NAME = re.compile(r"([A-Za-z_]\w*)\s*\(")


def hot_names_from_source():
    """Every function the source marks PLAT_HOT (or gfx_rdp.c's HOT): the name before the first ( after the mark.

    Returns {name: (source path relative to the repository, group: game, walk or backend)}."""
    names = {}
    for directory, recurse in SOURCE_DIRS:
        root_dir = os.path.join(REPO, directory)
        for root, dirs, files in os.walk(root_dir):
            if not recurse:
                dirs[:] = []
            for f in sorted(files):
                if not f.endswith(".c"):
                    continue
                with open(os.path.join(root, f), encoding="utf-8", errors="replace") as fh:
                    lines = fh.readlines()
                for i, line in enumerate(lines):
                    s = line.strip()
                    if s.startswith(("*", "/*", "//", "#")) or not HOT_MARK.search(s):
                        continue
                    # The declaration can run onto the next lines (an attribute, then the name).
                    text = strip_attributes(" ".join(l.strip() for l in lines[i:i + 3]))
                    mark = HOT_MARK.search(text)
                    m = FUNC_NAME.search(text[mark.end():] if mark else text)
                    if m and m.group(1) not in names:
                        group = MARK_GROUP[mark.group(2) if mark else HOT_MARK.search(s).group(2)]
                        names[m.group(1)] = ("/" + os.path.relpath(os.path.join(root, f), REPO).replace("\\", "/"),
                                             group)
    return names


def fmt(n):
    return "{:,}".format(n)


def measure_set(syms, names, where, title, lines, missing):
    total = 0
    lines.append("")
    lines.append("### " + title)
    lines.append("")
    lines.append("| function | address | size |")
    lines.append("|---|---|---|")
    for name in names:
        optional = name.endswith("?")
        name = name.rstrip("?")
        found = find(syms, name, where)
        if not found:
            missing.append(name + ("?" if optional else ""))
            continue
        for addr, size, sym, _ in found:
            lines.append("| {} | 0x{:x} | {} |".format(sym, addr, fmt(size)))
            total += size
    lines.append("| **total** | | **{}** |".format(fmt(total)))
    return total


def check_memcpy(objdump, elf, syms):
    """Problems found in memcpy and memcpy.part*: calls or tail branches to memcpy / memmove / memset."""
    targets = [s[:3] for s in syms if s[2] == "memcpy" or s[2].startswith("memcpy.part")]
    if not targets:
        return ["memcpy not found in the ELF"], []
    problems, checked = [], []
    branch = re.compile(r"\s(bl|blx|b)(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?(?:\.\w+)?\s+"
                        r"(?:0x)?([0-9a-f]+)\s*<([^>]+)>")
    for addr, size, name in targets:
        cmd = [objdump, "-d", "--start-address=0x{:x}".format(addr), "--stop-address=0x{:x}".format(addr + size)]
        if "llvm" in os.path.basename(objdump):
            cmd.append("--triple=armv6k-none-eabi")
        out = subprocess.run(cmd + [elf], check=True, capture_output=True, text=True).stdout
        n = 0
        for line in out.splitlines():
            m = branch.search(line)
            if not m:
                continue
            n += 1
            op, target, label = m.group(1), int(m.group(2), 16), m.group(3)
            base = label.split("+")[0]
            inside = addr <= target < addr + size
            if base.startswith("memcpy.part"):
                continue
            if inside and not (op.startswith("bl") and target == addr):
                continue  # a branch inside the function (a loop), not a call to itself
            if any(base == f or base.startswith(f + ".") for f in MEMCPY_FORBIDDEN):
                problems.append("{} {} {}".format(name, op, label) + " (calls itself)" * (target == addr))
        checked.append("{} (0x{:x}, {} bytes, {} branches)".format(name, addr, size, n))
    return problems, checked


def update_budgets(totals):
    path = os.path.abspath(__file__)
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    new = "BUDGETS = {{\"hot_span\": {hot_span}, \"walk_set\": {walk_set}, \"backend_set\": {backend_set}}}".format(
        **totals)
    src, n = re.subn(r"^BUDGETS = \{.*\}$", new, src, count=1, flags=re.M)
    if n != 1:
        sys.exit("could not find the BUDGETS line in " + path)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(src)
    print("budgets updated: " + new)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("elf")
    ap.add_argument("--nm", default=default_tool("nm"))
    ap.add_argument("--objdump", default=default_tool("objdump"))
    ap.add_argument("--update", action="store_true", help="rewrite BUDGETS with this ELF's totals")
    ap.add_argument("--report-only", action="store_true", help="print only, never fail on a budget")
    args = ap.parse_args()

    syms = read_symbols(args.nm, args.elf)
    lines = ["## Hot code size (" + os.path.basename(args.elf) + ")"]
    missing = []

    hot_names = hot_names_from_source()
    found = {name: find(syms, name, (path,)) for name, (path, _) in hot_names.items()}
    hot = [sym for syms_of in found.values() for sym in syms_of]
    hot_missing = [n for n, syms_of in found.items() if not syms_of]
    if hot:
        lo = min(a for a, _, _, _ in hot)
        hi = max(a + sz for a, sz, _, _ in hot)
        hot_span = hi - lo
        lines.append("")
        lines.append("hot_span: 0x{:x}..0x{:x} = {} bytes, {} marked functions found ({} inlined or not in "
                     "this ELF: {})".format(lo, hi, fmt(hot_span), len(hot_names) - len(hot_missing),
                                            len(hot_missing), ", ".join(hot_missing) or "none"))
    else:
        hot_span = 0
        lines.append("")
        lines.append("hot_span: no marked function found")

    # The stretches by group (P57 D), and the check that none lies inside another's span.
    groups = {}
    for name, (_, group) in hot_names.items():
        for sym in found[name]:
            groups.setdefault(group, []).append(sym)
    spans = {}
    interleaved = []
    lines.append("")
    lines.append("| stretch | from | to | span | functions | total size |")
    lines.append("|---|---|---|---|---|---|")
    for group in ("game", "walk", "backend"):
        members = groups.get(group, [])
        if not members:
            spans[group] = (0, 0, 0)
            lines.append("| {} | | | 0 | 0 | 0 |".format(group))
            continue
        g_lo = min(a for a, _, _, _ in members)
        g_hi = max(a + sz for a, sz, _, _ in members)
        spans[group] = (g_lo, g_hi, sum(sz for _, sz, _, _ in members))
        lines.append("| {} | 0x{:x} | 0x{:x} | {} | {} | {} |".format(group, g_lo, g_hi, fmt(g_hi - g_lo), len(members),
                                                                       fmt(spans[group][2])))
    for group, members in groups.items():
        for other, (o_lo, o_hi, _) in spans.items():
            if other == group or o_hi == o_lo:
                continue
            for addr, size, sym, _ in members:
                if o_lo < addr + size and addr < o_hi:
                    interleaved.append("{} ({}) inside the {} stretch".format(sym, group, other))
    lines.append("")
    lines.append("stretches interleaved: " + ("; ".join(sorted(interleaved)) if interleaved else "none"))

    walk_names = list(WALK_SET)
    note = ""
    if not find(syms, "port_gfx_run_frame", WALK_WHERE):
        walk_names[walk_names.index("port_gfx_run_frame")] = "frame_thread"
        note = "(port_gfx_run_frame inlined into frame_thread)"
    walk = measure_set(syms, walk_names, WALK_WHERE, "walk_set " + note, lines, missing)
    backend = measure_set(syms, BACKEND_SET, BACKEND_WHERE, "backend_set", lines, missing)
    required_missing = [n for n in missing if not n.endswith("?")]
    if missing:
        lines.append("")
        lines.append("not in this ELF (? = optional): " + ", ".join(missing))

    totals = {"hot_span": hot_span, "walk_set": walk, "backend_set": backend,
              "game_set": spans["game"][2], "game_span": spans["game"][1] - spans["game"][0],
              "walk_span": spans["walk"][1] - spans["walk"][0],
              "backend_span": spans["backend"][1] - spans["backend"][0]}
    lines.append("")
    lines.append("| total | bytes | budget |")
    lines.append("|---|---|---|")
    failures = []
    if required_missing:
        failures.append("measured functions not found in the ELF (renamed or inlined?): " + ", ".join(required_missing) +
                        "; fix the name in hot_size.py, or mark it optional with a trailing ?")
    for key, value in totals.items():
        budget = BUDGETS.get(key)
        lines.append("| {} | {} | {} |".format(key, fmt(value), fmt(budget) if budget is not None else
                                               "report only" if key in REPORTED else "not set"))
        if budget is not None and value > budget + SLACK:
            failures.append("hot code grew: {} {} > budget {} + 1 KB; if this is meant, run with --update and "
                            "commit".format(key, fmt(value), fmt(budget)))

    problems, checked = check_memcpy(args.objdump, args.elf, syms)
    lines.append("")
    lines.append("memcpy check: " + ("FAILED: " + "; ".join(problems) if problems else "clean") +
                 " (" + "; ".join(checked) + ")")

    report = "\n".join(lines)
    print(report)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(report + "\n")

    if args.update:
        update_budgets(totals)
        failures = []
    status = 0
    for f in failures:
        print(f, file=sys.stderr)
        if not args.report_only:
            status = 1
    for p in problems:
        print("memcpy makes a call it must not: " + p, file=sys.stderr)
        status = 1
    if interleaved:
        # Like the memcpy check, not a budget: --update and --report-only don't excuse it.
        print("hot stretches interleave (plat.h PLAT_HOT_GAME): " + "; ".join(sorted(interleaved)), file=sys.stderr)
        status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
