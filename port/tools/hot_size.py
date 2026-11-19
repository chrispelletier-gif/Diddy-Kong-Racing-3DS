#!/usr/bin/env python3
"""Hot code size and memcpy check for the 3DS ELF (roadmap P56, kohjoh lessons K1 and K10).

The Old 3DS has a 16 KB instruction cache per core. The renderer's per-draw code is meant to fit in it (P38 laid the
PLAT_HOT functions out together in .text.hot at ~16 KB), but nothing stopped it from growing back. This script
measures three numbers from the linked ELF and fails CI when one grows past its budget:

  hot_span     lowest to highest address (+ size) of the functions the source marks PLAT_HOT / HOT
  walk_set     total size of the display list walk's per-command functions (the game core on the Old 3DS)
  backend_set  total size of the backend's per-draw functions and the citro3d calls they make (the system core)

It also checks that the port's memcpy (libultra/src/libc/string.c) makes no call to memcpy, memmove or memset: GCC
can turn its copy loops into such a call, which would recurse or copy overlapping bytes differently from the N64.

Usage: python3 port/tools/hot_size.py ELF [--nm PATH] [--objdump PATH] [--update] [--report-only]
  --update       rewrite BUDGETS below with this ELF's totals (commit the result when the growth is meant)
  --report-only  print the numbers, never fail on a budget (the memcpy check still fails)
"""

import argparse
import os
import re
import subprocess
import sys

# Budgets in bytes, from master's CI debug build. A total may exceed its budget by up to SLACK bytes.
BUDGETS = {"hot_span": None, "walk_set": None, "backend_set": None}
SLACK = 1024

WALK_SET = (
    "run_command_list draw_dkr_triangles load_vertices load_vertices_now make_draw_state lookup_texture "
    "command_off_screen frame_hash tex_memo_confirm load_matrix tex_axis_init handle_call call_display_list "
    "flush_cpu_batch forget_covered_loads shared_vertices_clear port_gfx_run_frame"
).split()

BACKEND_SET = (
    "gfxb_draw_indexed hang_draw_start test_and_write_vertices use_vertex_format bind_texture apply_fog "
    "apply_combiner drain_before_draw set_all_state write_settings_twice set_draw_state clip_cut_lean "
    "clip_cut_triangles C3Di_UpdateContext C3D_DrawElements C3D_DrawArrays C3D_UpdateUniforms C3Di_SetTex "
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


HOT_MARK = re.compile(r"(^|\s)(PLAT_HOT|HOT)(\s|$)")
FUNC_NAME = re.compile(r"([A-Za-z_]\w*)\s*\(")


def hot_names_from_source():
    """Every function the source marks PLAT_HOT (or gfx_rdp.c's HOT): the name before the first ( after the mark.

    Returns {name: source path relative to the repository}."""
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
                    m = FUNC_NAME.search(text[HOT_MARK.search(text).end():] if HOT_MARK.search(text) else text)
                    if m and m.group(1) not in names:
                        names[m.group(1)] = "/" + os.path.relpath(os.path.join(root, f), REPO).replace("\\", "/")
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
        found = find(syms, name, where)
        if not found:
            missing.append(name)
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
    hot = [s for name, path in hot_names.items() for s in find(syms, name, (path,))]
    hot_missing = [n for n, path in hot_names.items() if not find(syms, n, (path,))]
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

    walk_names = list(WALK_SET)
    note = ""
    if not find(syms, "port_gfx_run_frame", WALK_WHERE):
        walk_names[walk_names.index("port_gfx_run_frame")] = "frame_thread"
        note = "(port_gfx_run_frame inlined into frame_thread)"
    walk = measure_set(syms, walk_names, WALK_WHERE, "walk_set " + note, lines, missing)
    backend = measure_set(syms, BACKEND_SET, BACKEND_WHERE, "backend_set", lines, missing)
    if missing:
        lines.append("")
        lines.append("not in this ELF: " + ", ".join(missing))

    totals = {"hot_span": hot_span, "walk_set": walk, "backend_set": backend}
    lines.append("")
    lines.append("| total | bytes | budget |")
    lines.append("|---|---|---|")
    failures = []
    for key, value in totals.items():
        budget = BUDGETS.get(key)
        lines.append("| {} | {} | {} |".format(key, fmt(value), fmt(budget) if budget is not None else "not set"))
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
    return status


if __name__ == "__main__":
    sys.exit(main())
