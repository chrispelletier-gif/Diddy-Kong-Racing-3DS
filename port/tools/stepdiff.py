#!/usr/bin/env python3
"""Reads the host 60 fps step check's output (port/host/host_stepdiff.c, DKR_STEPDIFF) and lists the game values that
move differently in two 60 fps half steps than in one 30 fps step, worst first.

    stepdiff.py <dkr binary> <stepdiff.bin> [more .bin files]
"""
import bisect, collections, math, struct, subprocess, sys


import glob, os, re

def behaviours():
    names, n = {}, 0
    for line in open('include/object_behaviors.h'):
        m = re.match(r'\s*(BHV_\w+)\s*(?:=\s*(\d+))?,', line)
        if m:
            if m.group(2):
                n = int(m.group(2))
            names[n] = m.group(1)[4:].lower()
            n += 1
    return names

def struct_fields():
    """Each struct's fields by the offsets the decomp's comments give: {struct: [(offset, name)]}."""
    out, cur, fields = {}, None, []
    text = open('include/structs.h').read()
    for line in text.splitlines():
        if re.match(r'\s*typedef struct', line) and line.rstrip().endswith('{'):
            cur, fields, depth = True, [], 0
        m = re.match(r'\s*/\* 0x([0-9A-Fa-f]+) \*/\s*(.*?);', line)
        if cur and m:
            name = re.split(r'[\s*]+', m.group(2).split('[')[0].strip())[-1]
            fields.append((int(m.group(1), 16), name))
        m = re.match(r'\} (\w+);', line)
        if cur and m:
            out[m.group(1)] = sorted(fields)
            cur = None
    # Which behaviour uses which struct, from Object's union comments.
    bhv_struct = {}
    for m in re.finditer(r'(\w+) \*\w+;\s*//\s*(BHV_[\w, ]+)', text):
        for b in re.findall(r'BHV_\w+', m.group(2)):
            bhv_struct[b[4:].lower()] = m.group(1)
    for m in re.finditer(r'//\s*(BHV_[\w, ]+)\n', text):
        pass
    return out, bhv_struct

def field_name(fields, off):
    best = None
    for o, n in fields or []:
        if o <= off:
            best = (o, n)
    if best is None:
        return '+0x%x' % off
    return best[1] + ('' if best[0] == off else '+%d' % (off - best[0]))

def game_symbols(objdir):
    names = set()
    for path in glob.glob(os.path.join(objdir, 'src', '*.o')) + glob.glob(os.path.join(objdir, 'src', '*', '*.o')):
        if '/plat/' in path or '/port' in os.path.basename(path):
            continue
        out = subprocess.run(['nm', path], capture_output=True, text=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in 'bBdD':
                names.add(p[2])
    return names

def syms(binary):
    out = subprocess.run(['nm', '-n', '-S', binary], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in 'bBdD':
            addrs.append(int(p[0], 16)); names.append((p[3], int(p[1], 16)))
    return addrs, names

def f32(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]

def plausible(u):
    if u == 0:
        return True
    x = f32(u)
    return math.isfinite(x) and 1e-5 < abs(x) < 1e6

def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v

def wrap16(d):
    d = (d + 0x8000) & 0xFFFF
    return d - 0x8000

def main():
    binary = sys.argv[1]
    addrs, names = syms(binary)
    bhvs = behaviours()
    structs, bhv_struct = struct_fields()
    game = game_symbols(os.path.join(os.path.dirname(binary), 'obj'))
    out = subprocess.run(['nm', binary], capture_output=True, text=True).stdout
    data_start = [int(l.split()[0], 16) for l in out.splitlines() if l.endswith(' __data_start')][0]
    stats = collections.defaultdict(lambda: [0, 0, 0, 0, [], collections.Counter()])  # samples, x2, only60, off, examples, where
    for path in sys.argv[2:]:
        data = open(path, 'rb').read()
        pos = 0
        while pos + 16 <= len(data):
            magic, sample, context, nobj = struct.unpack_from('<4I', data, pos); pos += 16
            if magic != 0x46494453:
                break
            objs = [struct.unpack_from('<3I', data, pos + i * 12) for i in range(nobj)]; pos += nobj * 12
            base = struct.unpack_from('<I', data, pos)[0]; pos += 4
            delta = base - data_start
            objs.sort()
            starts = [o[0] for o in objs]
            pool = None
            for i, a in enumerate(addrs):
                if names[i][0] == 'gPortMainPoolArea':
                    pool = (a + delta, a + delta + names[i][1])
            while True:
                addr, b, a3, a6 = struct.unpack_from('<4I', data, pos); pos += 16
                if addr == 0:
                    break
                if pool[0] <= addr < pool[1]:
                    i = bisect.bisect_right(starts, addr) - 1
                    if i < 0:
                        continue
                    obj, bhv, beh = objs[i]
                    off = addr - obj
                    bname = bhvs.get(bhv, 'bhv%d' % bhv)
                    if off < 0x78:
                        key = '%s obj.%s' % (bname, field_name(structs['Object'], off))
                    elif beh and beh <= addr:
                        st = bhv_struct.get(bname)
                        key = '%s %s.%s' % (bname, st or 'data', field_name(structs.get(st), addr - beh))
                    else:
                        key = '%s obj+0x%x' % (bname, off)
                else:
                    addr -= delta
                    i = bisect.bisect_right(addrs, addr) - 1
                    nm, size = names[i]
                    if nm.split('.')[0] not in game:
                        continue
                    key = '%s+0x%x' % (nm, addr - addrs[i])
                fields = []
                if plausible(b) and plausible(a3) and plausible(a6):
                    fields.append(('f', f32(b), f32(a3), f32(a6)))
                else:
                    for h in (0, 16):
                        fields.append(('h%d' % (h // 8), s16(b >> h), s16(a3 >> h), s16(a6 >> h)))
                for kind, vb, v3, v6 in fields:
                    if kind == 'f':
                        d3, d6 = v3 - vb, v6 - vb
                    else:
                        d3, d6 = wrap16(v3 - vb), wrap16(v6 - vb)
                    if d3 == d6:
                        continue
                    st = stats[key + ' ' + kind]
                    st[0] += 1
                    where = '%s%d' % ('m' if context >> 8 == 2 else 'L', context & 0xFF)
                    if d3 == 0:
                        st[2] += 1
                        st[5][where] += 1
                    elif 1.6 <= d6 / d3 <= 2.4:
                        st[1] += 1
                        st[5][where] += 1
                    elif abs(d6 / d3 - 1) > 0.25:
                        st[3] += 1
                    if len(st[4]) < 4:
                        st[4].append('%.4g:%.4g/%.4g' % (vb, d3, d6))
    rows = sorted(stats.items(), key=lambda kv: -(kv[1][1] * 3 + kv[1][2] + kv[1][3]))
    print('%-44s %6s %5s %5s %5s  examples (before: step30/step60)' % ('value', 'diffs', 'x2', 'only60', 'off'))
    for key, (n, x2, o60, off, ex, where) in rows:
        if x2 + o60 + off == 0:
            continue
        print('%-44s %6d %5d %5d %5d  %s  @%s' % (key, n, x2, o60, off, ' '.join(ex),
                                              ','.join('%s:%d' % w for w in where.most_common(4))))

main()
