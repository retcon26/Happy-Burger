"""NPC navigation graphs (data/maps/<map>.pnav) for psp/src/nav.c.

Mappers place info_node entities where NPCs can walk; Source links them into a graph when the map
first runs. This does the same once, offline: each node is dropped to the floor, and two nearby nodes
are linked when a character-sized box can slide from one to the other with floor under it all the way.
The tests run against the converted map's own collision (its .pmap), so props baked into the world
count too.

File (same container as .pmap, magic "PNAV"):
  "NAVN": {float x, y, z (on the floor); u16 first_link; u8 num_links; u8 flags} per node
  "NAVL": u16 node index per link (each link is listed from both ends)
"""

import math
import os
import struct
import time

from .pmap import PmapWriter

PNAV_MAGIC = b"PNAV"
PNAV_VERSION = 1

NODE_CLASSES = ("info_node", "info_node_hint")  # ground nodes (not air or climb)
MAX_LINK = 1100.0      # longest link (open ground has its nodes far apart)
MAX_LINKS = 10         # per node
CANDIDATES = 14        # nearest nodes tried per node
STEP = 18.0            # stair step the box climbs
HULL = (13.0, 40.0)    # half width, height above the step
FLOOR_GAP = 40.0       # how far below the line the floor may drop
FLOOR_EVERY = 40.0

CONTENTS_SOLID, CONTENTS_WINDOW, CONTENTS_GRATE, CONTENTS_MOVEABLE, CONTENTS_PLAYERCLIP = 0x1, 0x2, 0x8, 0x4000, 0x10000
MASK = CONTENTS_SOLID | CONTENTS_WINDOW | CONTENTS_GRATE | CONTENTS_MOVEABLE | CONTENTS_PLAYERCLIP
DIST_EPSILON = 0.03125


class MapCollision:
    """Box traces against a .pmap's collision (a port of psp/src/collision.c's world part)."""

    def __init__(self, path):
        with open(path, "rb") as f:
            d = f.read()
        if d[:4] != b"PMAP":
            raise ValueError(f"{path}: not a pmap")
        nsec = struct.unpack_from("<I", d, 8)[0]
        sec = {}
        for i in range(nsec):
            sid, ofs, size, count = struct.unpack_from("<4sIII", d, 16 + i * 16)
            sec[sid] = (ofs, size, count)

        def arr(sid, fmt):
            ofs, _size, count = sec[sid]
            sz = struct.calcsize(fmt)
            return [struct.unpack_from(fmt, d, ofs + i * sz) for i in range(count)]

        self.planes = arr(b"PLAN", "<4f")
        self.nodes = arr(b"NODE", "<3i")
        leafs = arr(b"LEAF", "<iIIIII")
        brushes = arr(b"BRSH", "<3I")
        sides = [s[0] for s in arr(b"BSID", "<I")]
        lbrs = [s[0] for s in arr(b"LBRS", "<I")]
        self.brushes = []
        for first, num, contents in brushes:
            real = num & 0x7FFFFFFF
            pl = [self.planes[sides[first + k] & 0x7FFFFFFF] for k in range(min(real, 64))]
            if num & 0x80000000:
                # A box-bevel brush (v6): its bounds follow its sides, standing for 6 axis planes
                mn = struct.unpack("<3f", struct.pack("<3I", *sides[first + real:first + real + 3]))
                mx = struct.unpack("<3f", struct.pack("<3I", *sides[first + real + 3:first + real + 6]))
                pl += [(1.0, 0.0, 0.0, mx[0]), (-1.0, 0.0, 0.0, -mn[0]), (0.0, 1.0, 0.0, mx[1]),
                       (0.0, -1.0, 0.0, -mn[1]), (0.0, 0.0, 1.0, mx[2]), (0.0, 0.0, -1.0, -mn[2])]
            self.brushes.append((contents, tuple(pl)))
        self.leaf_brushes = [tuple(lbrs[l[2] + k] for k in range(l[3])) for l in leafs]
        self.stamp = [0] * len(self.brushes)
        self.check = 0

    def trace(self, start, end, mins=(0.0, 0.0, 0.0), maxs=(0.0, 0.0, 0.0), mask=MASK):
        """(fraction, startsolid, hit normal)"""
        self.check += 1
        self.frac = 1.0
        self.startsolid = False
        self.allsolid = False
        self.normal = (0.0, 0.0, 0.0)
        self.w = (start, end, mins, maxs, mask)
        c = [(mins[k] + maxs[k]) * 0.5 for k in range(3)]
        self.ext = [(maxs[k] - mins[k]) * 0.5 for k in range(3)]
        if self.nodes:
            self._tree(0, 0.0, 1.0, [start[k] + c[k] for k in range(3)], [end[k] + c[k] for k in range(3)])
        return self.frac, self.startsolid, self.normal

    def _clip(self, planes):
        start, end, mins, maxs, _mask = self.w
        enter, leave = -1.0, 1.0
        clip = None
        startout = getout = False
        for p in planes:
            nx, ny, nz, dist = p
            ofs = ((maxs[0] if nx < 0 else mins[0]) * nx + (maxs[1] if ny < 0 else mins[1]) * ny +
                   (maxs[2] if nz < 0 else mins[2]) * nz)
            dist -= ofs
            d1 = nx * start[0] + ny * start[1] + nz * start[2] - dist
            d2 = nx * end[0] + ny * end[1] + nz * end[2] - dist
            if d2 > 0.0:
                getout = True
            if d1 > 0.0:
                startout = True
            if d1 > 0.0 and (d2 >= DIST_EPSILON or d2 >= d1):
                return
            if d1 <= 0.0 and d2 <= 0.0:
                continue
            if d1 > d2:
                f = max(0.0, (d1 - DIST_EPSILON) / (d1 - d2))
                if f > enter:
                    enter = f
                    clip = p
            else:
                f = min(1.0, (d1 + DIST_EPSILON) / (d1 - d2))
                if f < leave:
                    leave = f
        if not startout:
            self.startsolid = True
            if not getout:
                self.allsolid = True
                self.frac = 0.0
            return
        if enter < leave and enter > -1.0 and enter < self.frac and clip:
            self.frac = enter
            self.normal = clip[:3]

    def _leaf(self, leaf):
        mask = self.w[4]
        stamp, check = self.stamp, self.check
        for bi in self.leaf_brushes[leaf]:
            if stamp[bi] == check:
                continue
            stamp[bi] = check
            contents, planes = self.brushes[bi]
            if not contents & mask:
                continue
            self._clip(planes)
            if self.allsolid:
                return

    def _tree(self, node, f1, f2, p1, p2):
        ext = self.ext
        while True:
            if self.allsolid or self.frac <= f1:
                return
            if node < 0:
                self._leaf(-1 - node)
                return
            plane, c0, c1 = self.nodes[node]
            nx, ny, nz, dist = self.planes[plane]
            d1 = nx * p1[0] + ny * p1[1] + nz * p1[2] - dist
            d2 = nx * p2[0] + ny * p2[1] + nz * p2[2] - dist
            ofs = abs(nx) * ext[0] + abs(ny) * ext[1] + abs(nz) * ext[2]
            if d1 >= ofs + 1.0 and d2 >= ofs + 1.0:
                node = c0
                continue
            if d1 < -ofs - 1.0 and d2 < -ofs - 1.0:
                node = c1
                continue
            if d1 < d2:
                idist = 1.0 / (d1 - d2)
                side = 1
                fb = (d1 + ofs + DIST_EPSILON) * idist
                fa = (d1 - ofs + DIST_EPSILON) * idist
            elif d1 > d2:
                idist = 1.0 / (d1 - d2)
                side = 0
                fb = (d1 - ofs - DIST_EPSILON) * idist
                fa = (d1 + ofs + DIST_EPSILON) * idist
            else:
                side = 0
                fa, fb = 1.0, 0.0
            fa = min(1.0, max(0.0, fa))
            fb = min(1.0, max(0.0, fb))
            kids = (c0, c1)
            mid_a = [p1[k] + (p2[k] - p1[k]) * fa for k in range(3)]
            self._tree(kids[side], f1, f1 + (f2 - f1) * fa, p1, mid_a)
            p1 = [p1[k] + (p2[k] - p1[k]) * fb for k in range(3)]
            f1 = f1 + (f2 - f1) * fb
            node = kids[side ^ 1]


def _floor(cm, p, below=200.0):
    """The floor under p, or None."""
    f, ss, n = cm.trace((p[0], p[1], p[2]), (p[0], p[1], p[2] - below))
    if ss or f >= 1.0 or n[2] < 0.6:
        return None
    return (p[0], p[1], p[2] - below * f + 0.5)


def _walkable(cm, a, b):
    """A character can walk from a to b (both on the floor)."""
    dx, dy, dz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    d2 = math.hypot(dx, dy)
    if abs(dz) > d2 + STEP:  # steeper than 45 degrees (stairs are about that)
        return False
    h, top = HULL
    mins, maxs = (-h, -h, 0.0), (h, h, top)
    up = lambda p, z: (p[0], p[1], p[2] + z)
    f, ss, _n = cm.trace(up(a, STEP), up(b, STEP), mins, maxs)
    if ss or f < 1.0:
        # A taller step up (a kerb) or a low bit of ceiling: try the box higher, then lower
        f, ss, _n = cm.trace(up(a, STEP + 16.0), up(b, STEP + 16.0), mins, (h, h, top - 16.0))
        if ss or f < 1.0:
            return False
    # Floor all the way: no gaps, pits or drops off ledges
    n = max(1, int(d2 / FLOOR_EVERY))
    for i in range(1, n):
        t = i / n
        p = (a[0] + dx * t, a[1] + dy * t, a[2] + dz * t + STEP)
        f, ss, nrm = cm.trace(p, (p[0], p[1], p[2] - STEP - FLOOR_GAP))
        if ss or f >= 1.0 or nrm[2] < 0.6:
            return False
    return True


def build_nav(bsp, pmap_path, out_path, log=print):
    """Writes out_path from the map's info_nodes; returns the node count (0: no file written)."""
    t0 = time.time()
    raw = []
    for e in bsp.entities():
        if e.get("classname", "") in NODE_CLASSES:
            try:
                raw.append(tuple(float(v) for v in e.get("origin", "").split()[:3]))
            except ValueError:
                continue
    raw = [p for p in raw if len(p) == 3]
    if not raw:
        if os.path.exists(out_path):
            os.remove(out_path)
        return 0
    cm = MapCollision(pmap_path)
    nodes = []
    for p in raw:
        f = _floor(cm, (p[0], p[1], p[2] + 16.0), 256.0) or _floor(cm, (p[0], p[1], p[2] + 2.0), 256.0)
        if f and not any(abs(f[0] - q[0]) < 8 and abs(f[1] - q[1]) < 8 and abs(f[2] - q[2]) < 16 for q in nodes):
            nodes.append(f)
    links = [set() for _ in nodes]
    tested = set()
    for i, a in enumerate(nodes):
        near = []
        for j, b in enumerate(nodes):
            if i != j:
                d = math.hypot(b[0] - a[0], b[1] - a[1])
                if d <= MAX_LINK:
                    near.append((d, j))
        near.sort()
        for _d, j in near[:CANDIDATES]:
            if len(links[i]) >= MAX_LINKS:
                break
            key = (min(i, j), max(i, j))
            if key in tested:
                continue
            tested.add(key)
            if len(links[j]) < MAX_LINKS and _walkable(cm, a, nodes[j]):
                links[i].add(j)
                links[j].add(i)
    node_data = bytearray()
    link_data = bytearray()
    first = 0
    for p, ls in zip(nodes, links):
        node_data += struct.pack("<3fHBB", p[0], p[1], p[2], first, len(ls), 0)
        for j in sorted(ls):
            link_data += struct.pack("<H", j)
        first += len(ls)
    w = PmapWriter(PNAV_MAGIC, PNAV_VERSION)
    w.add("NAVN", node_data, len(nodes))
    w.add("NAVL", link_data, first)
    w.write(out_path)
    alone = sum(1 for ls in links if not ls)
    log(f"  nav: {len(nodes)} nodes ({len(raw) - len(nodes)} dropped), {first // 2} links, {alone} unlinked"
        f"  [{time.time() - t0:.1f}s]")
    return len(nodes)


def nav_for_map(bsp_path, out_dir):
    """The graph for one converted map, next to its .pmap. Returns a line for the log."""
    from .convert import open_bsp
    from .goldsrc import is_goldsrc

    base = os.path.splitext(os.path.basename(bsp_path))[0]
    if is_goldsrc(bsp_path):
        base = "hl1_" + base  # as cmd_convert names it
    pmaps = [os.path.join(out_dir, base + s + ".pmap") for s in ("", ".low")]
    pmap = next((p for p in pmaps if os.path.exists(p)), None)
    if pmap is None:
        return base + ": not converted"
    lines = []
    try:
        n = build_nav(open_bsp(bsp_path), pmap, os.path.join(out_dir, base + ".pnav"), log=lines.append)
    except (OSError, ValueError, KeyError, struct.error) as e:
        return f"{base}: failed ({e})"
    return f"{base}: {lines[0].strip()[5:]}" if n and lines else base + ": no nodes"
