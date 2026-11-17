#!/usr/bin/env python3
"""
Builds port/src/plat/bottom_map_paths.h: each track map's centre line as a few smooth polylines, in map units (the
map sprite's pixels times 4, as RaceHudPoint), for the grid bottom screen to draw its maps from (bottom_screen.c,
map_from_paths).

Inputs, made on the host build (see README in this folder):
  skel  DKR_DUMP_SKEL file: the ROM map picture's centre line traced on the host (one pixel wide, per map sprite)
  paths DKR_DUMP_PATHS file: each track's checkpoints as map points, used only to orient each line in the direction
        of travel (the ROM picture is the shape the player knows; the checkpoints sit a few pixels off it)

The trace is cleaned here, once: junction clusters merged, loose strands (spurs) under 60 px dropped, each strand smoothed and
simplified, ends of strands meeting at a junction pinned to its centre.
"""
import sys, math
from collections import defaultdict

def read_skel(fn):
    maps, cur = {}, None
    for l in open(fn):
        t = l.split()
        if not t:
            continue
        if t[0] == 'map':
            sp = int(t[2])
            if int(t[4]) or sp < 0 or sp in maps:
                cur = None
                continue
            cur = dict(sprite=sp, scale=float(t[6]), tx=int(t[8]), ty=int(t[9]), px=set())
            maps[sp] = cur
        elif t[0] == 's' and cur is not None:
            cur['px'].add((int(t[1]), int(t[2])))
    return maps

def read_paths(fn):
    out, cur = {}, None
    for l in open(fn):
        t = l.split()
        if t[0] == 'level':
            cur = dict(level=int(t[1]), main=int(t[5]), cps=[])
            out[int(t[3])] = cur
        else:
            cur['cps'].append((int(t[1]), int(t[3]), int(t[5]), int(t[7]) / 4.0, int(t[9]) / 4.0))
    return out

N8 = [(0, -1), (-1, 0), (1, 0), (0, 1), (-1, -1), (1, -1), (-1, 1), (1, 1)]

def graph(px):
    ring = [(0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1)]
    def crossings(p):
        v = [(p[0] + dx, p[1] + dy) in px for dx, dy in ring]
        a = sum(1 for i in range(8) if not v[i] and v[(i + 1) % 8])
        return a if any(v) else 0
    # a node: a loose end (one way out) or a junction (three or more); a staircase's corner pixel is neither
    nodes = {p for p in px if crossings(p) != 2}
    # cluster node pixels
    cl, cid = {}, 0
    for p in nodes:
        if p in cl:
            continue
        st = [p]; cl[p] = cid
        while st:
            q = st.pop()
            for dx, dy in N8:
                r = (q[0] + dx, q[1] + dy)
                if r in nodes and r not in cl:
                    cl[r] = cid; st.append(r)
        cid += 1
    centre = defaultdict(list)
    for p, c in cl.items():
        centre[c].append(p)
    centre = {c: (sum(x for x, _ in v) / len(v), sum(y for _, y in v) / len(v)) for c, v in centre.items()}
    edges, seen = [], set()
    for p in nodes:
        for dx, dy in N8:
            q = (p[0] + dx, p[1] + dy)
            if q not in px or q in nodes or q in seen:
                continue
            path = [p, q]; seen.add(q); prev = p
            while True:
                cur = path[-1]
                nxt = [(cur[0] + a, cur[1] + b) for a, b in N8 if (cur[0] + a, cur[1] + b) in px and (cur[0] + a, cur[1] + b) != prev]
                nn = [r for r in nxt if r in nodes and cl[r] != cl[p] or (r in nodes and len(path) > 3)]
                if nn:
                    path.append(nn[0]); break
                free = [r for r in nxt if r not in nodes and r not in seen]
                if not free:
                    break
                prev = cur; path.append(free[0]); seen.add(free[0])
            a, b = cl[path[0]], cl.get(path[-1])
            edges.append([a, b, path])
    # pure loops (no node on them)
    rest = [p for p in px if p not in nodes and p not in seen]
    restset = set(rest)
    for p in rest:
        if p in seen:
            continue
        path = [p]; seen.add(p); prev = None
        while True:
            cur = path[-1]
            free = [(cur[0] + a, cur[1] + b) for a, b in N8 if (cur[0] + a, cur[1] + b) in restset and (cur[0] + a, cur[1] + b) not in seen]
            if not free:
                break
            path.append(free[0]); seen.add(free[0])
        edges.append([None, None, path + [path[0]]])
    return edges, centre, cl

def prune(edges, minlen=60):  # a track has no dead ends: any loose strand shorter than this is the trace's spur
    while True:
        cnt = defaultdict(int)
        for a, b, p in edges:
            if a is not None:
                cnt[a] += 1
            if b is not None:
                cnt[b] += 1
        drop = [e for e in edges if len(e[2]) < minlen and ((e[0] is not None and cnt[e[0]] == 1) or (e[1] is not None and cnt[e[1]] == 1) or e[1] is None)]
        if not drop or len(drop) == len(edges):
            return edges
        edges = [e for e in edges if e not in drop]

def smooth(pts, closed, it=3):
    for _ in range(it):
        n = len(pts); out = []
        for i in range(n):
            if not closed and (i == 0 or i == n - 1):
                out.append(pts[i]); continue
            a, b, c = pts[(i - 1) % n], pts[i], pts[(i + 1) % n]
            out.append(((a[0] + 2 * b[0] + c[0]) / 4, (a[1] + 2 * b[1] + c[1]) / 4))
        pts = out
    return pts

def dp(pts, eps):
    if len(pts) < 3:
        return pts
    a, b = pts[0], pts[-1]
    dx, dy = b[0] - a[0], b[1] - a[1]; L = math.hypot(dx, dy) or 1e-9
    best, bi = -1, 0
    for i in range(1, len(pts) - 1):
        d = abs(dy * (pts[i][0] - a[0]) - dx * (pts[i][1] - a[1])) / L
        if d > best:
            best, bi = d, i
    if best <= eps:
        return [a, b]
    return dp(pts[:bi + 1], eps)[:-1] + dp(pts[bi:], eps)

def main(skel_fn, paths_fn, out_fn):
    maps, paths = read_skel(skel_fn), read_paths(paths_fn)
    entries = []
    for sp in sorted(maps):
        m = maps[sp]
        edges, centre, cl = graph(m['px'])
        # a small loop off a junction is the trace round a pond or a speck, not a road
        edges = prune([e for e in edges if not (e[0] is not None and e[0] == e[1] and len(e[2]) < 80)])
        sc = m['scale']
        def tomap(p):  # sMap pixel (centre) -> map units, the inverse of map_point (sprite maps)
            u = (p[0] + 0.5 - 3) / sc; v = (p[1] + 0.5 - 3) / sc
            return (u + m['tx'] + 1.5, v + m['ty'] + 1.0)
        cps = sorted([c for c in paths.get(sp, {'cps': []})['cps'] if c[0] < paths[sp]['main']], key=lambda c: c[1]) if sp in paths else []
        polys = []
        for a, b, p in edges:
            closed = a is None
            pts = [tomap(q) for q in (p[:-1] if closed else p)]
            if not closed:
                if a is not None: pts[0] = tomap(centre[a])
                if b is not None: pts[-1] = tomap(centre[b])
            pts = smooth(pts, closed)
            if closed:
                h = len(pts) // 2  # a loop simplified as its two halves (Douglas-Peucker needs two ends apart)
                pts = dp(pts[:h + 1], 0.35)[:-1] + dp(pts[h:] + [pts[0]], 0.35)[:-1]
            else:
                pts = dp(pts, 0.35)
            # the direction of travel: the checkpoints near this strand, in their order, should run along it
            if len(cps) >= 3:
                def param(c):
                    best, bi = 1e9, 0
                    for i, q in enumerate(pts):
                        d = (q[0] - c[3]) ** 2 + (q[1] - c[4]) ** 2
                        if d < best:
                            best, bi = d, i
                    return best, bi
                near = [(c[1], param(c)[1]) for c in cps if param(c)[0] < 6 ** 2]
                score = 0
                for (i0, p0), (i1, p1) in zip(near, near[1:]):
                    d = p1 - p0
                    if closed and abs(d) > len(pts) / 2:
                        d = -d
                    score += (d > 0) - (d < 0)
                if score < 0:
                    pts = pts[::-1]
            polys.append((closed, pts))
        entries.append((sp, polys))
        print(f"sprite {sp}: {len(polys)} strands, {sum(len(p) for _, p in polys)} points", file=sys.stderr)
    L = ["/* Generated by port/tools/map_paths/make_map_paths.py: each track map's centre line (map units x4, as",
         "   RaceHudPoint), traced from the ROM's map picture and oriented by the checkpoints. Do not edit. */",
         "typedef struct { short sprite, first, count; } MapPathTrack;",
         "typedef struct { short first, count, closed; } MapPathLine;"]
    pts_all, lines, tracks = [], [], []
    for sp, polys in entries:
        tracks.append((sp, len(lines), len(polys)))
        for closed, pts in polys:
            lines.append((len(pts_all), len(pts), 1 if closed else 0))
            pts_all += [(int(round(x * 4)), int(round(y * 4))) for x, y in pts]
    L.append("static const short kMapPathPoints[%d][2] = {" % len(pts_all))
    for i in range(0, len(pts_all), 8):
        L.append("    " + " ".join("{ %d, %d }," % p for p in pts_all[i:i + 8]))
    L.append("};")
    L.append("static const MapPathLine kMapPathLines[%d] = {" % len(lines))
    L += ["    { %d, %d, %d }," % l for l in lines]
    L.append("};")
    L.append("static const MapPathTrack kMapPathTracks[%d] = {" % len(tracks))
    L += ["    { %d, %d, %d }," % t for t in tracks]
    L.append("};")
    open(out_fn, 'w').write("\n".join(L) + "\n")

if __name__ == '__main__':
    main(*sys.argv[1:4])
