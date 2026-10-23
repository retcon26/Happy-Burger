"""The burger joint: a map built from boxes, written as data/maps/burgerjoint.pmap (psp/src/pmap_format.h).

The kitchen is at the origin, facing +x: burger.lua builds the counter, grill and the rest out of props on
this layout (the counter's line is x = 110), and the customers come in off the street through the door in
the front wall (x = 640). Everything is one visibility cluster (it's small), collision is the boxes as
box brushes in a little kd-tree, and lighting is baked into the vertex colours: the ceiling lamps inside,
the sun (with shadows) and the sky outside.
"""

import math
import struct

from ..bspconv import pspimage
from ..bspconv.pmap import PmapWriter
from . import art

MAP_NAME = "burgerjoint"
CONTENTS_SOLID = 1

# The building's inside, and where things are (burger.lua's layout matches)
IN_MIN = (-160.0, -250.0, 0.0)
IN_MAX = (640.0, 250.0, 150.0)
WALL = 16.0
DOOR_HALF = 44.0
DOOR_H = 104.0
COUNTER_X = (86.0, 134.0)
COUNTER_END = 216.0  # burger.lua's counter and end blocks reach this far either side

LAMPS = [(-70, -140), (-70, 0), (-70, 140), (40, 0), (230, -130), (230, 130), (420, -130), (420, 130),
         (330, 0), (560, 0)]
LAMP_Z = 149.0
LAMP_COLOUR = (1.0, 0.94, 0.82)
LAMP_RANGE = 360.0
INDOOR_AMBIENT = (0.36, 0.34, 0.32)
PROP_INDOOR = (0.55, 0.51, 0.46)  # the leaves' ambient light, for props and characters: warm inside
PROP_OUTDOOR = (0.42, 0.43, 0.46)
OUTDOOR_AMBIENT = (0.46, 0.48, 0.55)
SUN_DIR = (0.45, 0.33, 0.83)  # towards the sun
SUN_COLOUR = (1.0, 0.95, 0.85)
SKY_TOP = (0.35, 0.60, 0.95)
SKY_BOTTOM = (0.85, 0.92, 1.00)


def _norm(v):
    n = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / n for c in v)


SUN_DIR = _norm(SUN_DIR)


class Level:
    def __init__(self):
        self.solids = []  # (mins, maxs, {face: texture or None}, casts shadows)
        self.quads = []   # (corners counter-clockwise from the front, texture, fit, fullbright)
        self.spawns = []

    def box(self, mins, maxs, tex, shadow=True, visible=True):
        """A solid box. tex: a texture name for every face, or {face: name} with "side" the default.
        Faces: top, bottom, front (+x), back, left (+y), right."""
        if not isinstance(tex, dict):
            tex = {"side": tex}
        self.solids.append((tuple(map(float, mins)), tuple(map(float, maxs)), tex if visible else {}, shadow))

    def quad(self, corners, tex, fit=True, fullbright=False):
        self.quads.append(([tuple(map(float, c)) for c in corners], tex, fit, fullbright))

    def wall_quad(self, axis, at, facing, lo, hi, tex, fit=True, fullbright=False, nudge=0.6):
        """A picture on a wall: axis 0 (x = at) or 1 (y = at), facing +1 / -1 along it; lo / hi: the other
        horizontal coordinate and z, (h0, z0), (h1, z1)."""
        at += nudge * facing
        (h0, z0), (h1, z1) = lo, hi
        if axis == 0:
            pts = [(at, h0, z0), (at, h1, z0), (at, h1, z1), (at, h0, z1)]
        else:
            pts = [(h1, at, z0), (h0, at, z0), (h0, at, z1), (h1, at, z1)]
        if facing < 0:
            pts = [pts[1], pts[0], pts[3], pts[2]]
        self.quad(pts, tex, fit, fullbright)


def design():
    lv = Level()
    x0, y0, _ = IN_MIN
    x1, y1, z1 = IN_MAX
    W = WALL
    # Ground: one slab under everything (its top is drawn in pieces below)
    lv.box((-420, -1000, -32), (1450, 1000, 0), {}, shadow=False)
    # The restaurant's shell: brick outside, painted wall inside (the inside faces are drawn as quads)
    # (each wall's face into the room is left out: the paint quads below are drawn there instead, and two
    # surfaces in the same place would flicker through each other)
    def outside(inward):
        return {"side": "brick", "top": "roof", inward: None}
    lv.box((x0 - W, y0 - W, 0), (x0, y1 + W, z1 + W), outside("front"))              # back
    lv.box((x0 - W, y1, 0), (x1 + W, y1 + W, z1 + W), outside("right"))              # left (+y)
    lv.box((x0 - W, y0 - W, 0), (x1 + W, y0, z1 + W), outside("left"))               # right
    lv.box((x1, DOOR_HALF, 0), (x1 + W, y1, z1 + W), outside("back"))                # front, left of the door
    lv.box((x1, y0, 0), (x1 + W, -DOOR_HALF, z1 + W), outside("back"))               # front, right of it
    lv.box((x1, -DOOR_HALF, DOOR_H), (x1 + W, DOOR_HALF, z1 + W), outside("back"))   # over the door
    lv.box((x0 - W, y0 - W, z1), (x1 + W, y1 + W, z1 + W), {"top": "roof", "bottom": None, "side": "brick"})
    # Inside walls: wainscot to 40, paint above; the ceiling with its lamps
    for axis, at, facing, h0, h1 in [(0, x0, 1, y0, y1), (1, y1, -1, x0, x1), (1, y0, 1, x0, x1)]:
        lv.wall_quad(axis, at, facing, (h0, 0), (h1, 40), "wainscot", fit=False, nudge=0.0)
        lv.wall_quad(axis, at, facing, (h0, 40), (h1, z1), "wall", fit=False, nudge=0.0)
    for h0, h1 in [(DOOR_HALF, y1), (y0, -DOOR_HALF)]:
        lv.wall_quad(0, x1, -1, (h0, 0), (h1, 40), "wainscot", fit=False, nudge=0.0)
        lv.wall_quad(0, x1, -1, (h0, 40), (h1, z1), "wall", fit=False, nudge=0.0)
    lv.wall_quad(0, x1, -1, (-DOOR_HALF, DOOR_H), (DOOR_HALF, z1), "wall", fit=False, nudge=0.0)
    lv.quad([(x0, y0, z1), (x0, y1, z1), (x1, y1, z1), (x1, y0, z1)], "ceiling", fit=False)
    for lx, ly in LAMPS:
        lv.quad([(lx - 20, ly - 12, LAMP_Z), (lx - 20, ly + 12, LAMP_Z), (lx + 20, ly + 12, LAMP_Z),
                 (lx + 20, ly - 12, LAMP_Z)], "light", fullbright=True)
    # Floors: tiles in the kitchen, checkers in the dining room
    cx = COUNTER_X[1]
    lv.quad([(x0, y0, 0), (cx, y0, 0), (cx, y1, 0), (x0, y1, 0)], "floor_kitchen", fit=False)
    lv.quad([(cx, y0, 0), (x1, y0, 0), (x1, y1, 0), (cx, y1, 0)], "floor_dining", fit=False)
    # Pony walls closing the gaps between the counter's ends and the side walls
    for sgn in (1, -1):
        a, b = sorted((COUNTER_END * sgn, y1 * sgn))
        lv.box((COUNTER_X[0], a, 0), (COUNTER_X[1], b, 40), {"top": "wood", "side": "wainscot"})
    # The menu board over the counter, on chains
    lv.box((114, -70, 102), (120, 70, 136), {"front": "menu", "back": "menu", "side": "wood"})
    for y in (-60, 60):
        lv.box((116, y - 0.5, 136), (118, y + 0.5, z1), "black_metal", shadow=False)
    # Front windows, inside and out
    for ya, yb in [(70, 220), (-220, -70)]:
        lv.wall_quad(0, x1, -1, (ya, 30), (yb, 112), "window")
        lv.wall_quad(0, x1 + W, 1, (ya, 30), (yb, 112), "window")
    # Booths down both sides of the dining room: a table between two benches
    for sgn in (1, -1):
        for bx in (300.0, 470.0):
            a, b = sorted((196.0 * sgn, y1 * sgn))
            lv.box((bx - 18, a, 28), (bx + 18, b, 31), {"top": "wood", "side": "wood"})
            lv.box((bx - 4, (a + b) / 2 - 4, 0), (bx + 4, (a + b) / 2 + 4, 28), "black_metal")
            for side in (-1, 1):
                sx = bx + side * 36
                lv.box((sx - 10, a, 0), (sx + 10, b, 17), "booth")
                bk = sx + side * 8
                lv.box((min(bk, bk + side * 4), a, 17), (max(bk, bk + side * 4), b, 42), "booth")
    # The street: sidewalk, road with its centre line, the far sidewalk, buildings all round
    lv.quad([(x1 + W, -1000, 0), (800, -1000, 0), (800, 1000, 0), (x1 + W, 1000, 0)], "sidewalk", fit=False)
    lv.quad([(800, -1000, 0), (1150, -1000, 0), (1150, 1000, 0), (800, 1000, 0)], "asphalt", fit=False)
    for k in range(-7, 8):
        ya = k * 128.0
        lv.quad([(966, ya, 0.3), (984, ya, 0.3), (984, ya + 64, 0.3), (966, ya + 64, 0.3)], "road_line")
    lv.quad([(1150, -1000, 0), (1300, -1000, 0), (1300, 1000, 0), (1150, 1000, 0)], "sidewalk", fit=False)
    lv.quad([(-420, -1000, 0), (x0 - W, -1000, 0), (x0 - W, 1000, 0), (-420, 1000, 0)], "grass", fit=False)
    for ya, yb, tex, h in [(-1000, -330, "facade_green", 260), (-330, 330, "facade_blue", 320),
                           (330, 1000, "facade_tan", 280)]:
        lv.box((1300, ya, 0), (1340, yb, h), {"side": tex, "top": "roof"})
    for sgn, tex, h in [(1, "facade_tan", 230), (-1, "facade_green", 210)]:
        a, b = sorted((y1 * sgn + W * sgn, 1000.0 * sgn))
        lv.box((-260, a, 0), (x1 + W - 10, b, h), {"side": tex, "top": "roof"})
    lv.box((-420, -1000, 0), (-260, 1000, 260), {"side": "facade_blue", "top": "roof"})
    for sgn in (1, -1):
        a, b = sorted((1000.0 * sgn, 1040.0 * sgn))
        lv.box((-420, a, 0), (1340, b, 300), {"side": "facade_tan", "top": "roof"})
    # The sign over the door, and an awning
    lv.box((x1 + W, -110, 112), (x1 + W + 6, 110, 158), {"front": "sign", "side": "black_metal"})
    lv.box((x1 + W, -70, 104), (x1 + W + 44, 70, 108), {"side": "awning", "bottom": "awning", "top": "awning"})
    # Spawns: the kitchen, then the menus' camera (in the dining room, looking at the counter)
    lv.spawns = [(0, 0, 0, 0), (0, 50, 0, 0), (0, -50, 0, 0), (560, 200, 60, 205)]
    return lv


# --- Lighting ---------------------------------------------------------------------------------------------

def _indoors(p):
    return (IN_MIN[0] - 1 <= p[0] <= IN_MAX[0] + 1 and IN_MIN[1] - 1 <= p[1] <= IN_MAX[1] + 1
            and p[2] <= IN_MAX[2] + 1)


def _ray_box(o, d, mins, maxs):
    t0, t1 = 0.0, 1e9
    for k in range(3):
        if abs(d[k]) < 1e-9:
            if o[k] < mins[k] or o[k] > maxs[k]:
                return False
        else:
            a, b = (mins[k] - o[k]) / d[k], (maxs[k] - o[k]) / d[k]
            if a > b:
                a, b = b, a
            t0, t1 = max(t0, a), min(t1, b)
            if t0 > t1:
                return False
    return t1 > 0.0


def light_at(lv, p, n):
    if _indoors(p):
        c = list(INDOOR_AMBIENT)
        for lx, ly in LAMPS:
            d = (lx - p[0], ly - p[1], LAMP_Z - 2 - p[2])
            dist = math.sqrt(sum(x * x for x in d)) or 1.0
            att = max(0.0, 1.0 - dist / LAMP_RANGE) ** 2
            if att <= 0:
                continue
            lam = 0.35 + 0.65 * max(0.0, sum(n[k] * d[k] for k in range(3)) / dist)
            for k in range(3):
                c[k] += LAMP_COLOUR[k] * 0.42 * att * lam
        return c
    c = list(OUTDOOR_AMBIENT)
    lam = max(0.0, sum(n[k] * SUN_DIR[k] for k in range(3)))
    if lam > 0:
        o = tuple(p[k] + n[k] * 0.5 for k in range(3))
        for mins, maxs, _tex, shadow in lv.solids:
            if shadow and _ray_box(o, SUN_DIR, mins, maxs):
                lam = 0.0
                break
    for k in range(3):
        c[k] += SUN_COLOUR[k] * 0.62 * lam
    return c


def _abgr(c):
    r, g, b = (min(255, max(0, int(v * 255 + 0.5))) for v in c)
    return 0xFF000000 | (b << 16) | (g << 8) | r


# --- Faces into triangles ----------------------------------------------------------------------------------

FACES = {
    "top": lambda a, b: [(a[0], a[1], b[2]), (b[0], a[1], b[2]), (b[0], b[1], b[2]), (a[0], b[1], b[2])],
    "bottom": lambda a, b: [(a[0], a[1], a[2]), (a[0], b[1], a[2]), (b[0], b[1], a[2]), (b[0], a[1], a[2])],
    "front": lambda a, b: [(b[0], a[1], a[2]), (b[0], b[1], a[2]), (b[0], b[1], b[2]), (b[0], a[1], b[2])],
    "back": lambda a, b: [(a[0], b[1], a[2]), (a[0], a[1], a[2]), (a[0], a[1], b[2]), (a[0], b[1], b[2])],
    "left": lambda a, b: [(b[0], b[1], a[2]), (a[0], b[1], a[2]), (a[0], b[1], b[2]), (b[0], b[1], b[2])],
    "right": lambda a, b: [(a[0], a[1], a[2]), (b[0], a[1], a[2]), (b[0], a[1], b[2]), (a[0], a[1], b[2])],
}


def _uv_axes(n):
    """For a face with normal n: the world directions of the texture's right and down, as seen from in front."""
    if abs(n[2]) > 0.7:
        return (1.0, 0.0, 0.0), (0.0, -1.0 if n[2] > 0 else 1.0, 0.0)
    # Looking at the face (along -n), right is -n x up
    return _norm((-n[1], n[0], 0.0)), (0.0, 0.0, -1.0)


def tessellate(corners, tex, scale, fit, colour_of, max_cell=64.0):
    """A flat quad (corners counter-clockwise from the front) as a grid of vertices with world UVs.
    Returns (verts [(u, v, abgr, x, y, z)], tris [(a, b, c)])."""
    p0, p1, _p2, p3 = corners
    e1 = tuple(p1[k] - p0[k] for k in range(3))
    e2 = tuple(p3[k] - p0[k] for k in range(3))
    n = _norm((e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]))
    l1, l2 = math.sqrt(sum(c * c for c in e1)), math.sqrt(sum(c * c for c in e2))
    nu, nv = max(1, math.ceil(l1 / max_cell)), max(1, math.ceil(l2 / max_cell))
    right, down = _uv_axes(n)
    pts = [tuple(p0[k] + e1[k] * i / nu + e2[k] * j / nv for k in range(3)) for j in range(nv + 1) for i in range(nu + 1)]
    us = [sum(p[k] * right[k] for k in range(3)) for p in pts]
    vs = [sum(p[k] * down[k] for k in range(3)) for p in pts]
    if fit:
        umin, umax, vmin, vmax = min(us), max(us), min(vs), max(vs)
        us = [(u - umin) / ((umax - umin) or 1) for u in us]
        vs = [(v - vmin) / ((vmax - vmin) or 1) for v in vs]
    else:
        us = [u / scale for u in us]
        vs = [v / scale for v in vs]
    verts = [(us[k], vs[k], colour_of(pts[k], n), *pts[k]) for k in range(len(pts))]
    tris = []
    for j in range(nv):
        for i in range(nu):
            a = j * (nu + 1) + i
            b, c, d = a + 1, a + nu + 2, a + nu + 1
            tris += [(a, b, c), (a, c, d)]
    return verts, tris, n


# --- Collision: a kd-tree over the boxes --------------------------------------------------------------------

def kd_tree(boxes, world_min, world_max, forced=(), max_leaf=6, max_depth=14):
    """forced: (axis, value) planes split on first wherever they cross a node, so no leaf straddles them
    (the building's walls: a leaf's ambient light is either the inside's or the outside's)."""
    planes, nodes, leafs = [], [], []

    def build(ids, lo, hi, depth):
        cut = next(((a, v) for a, v in forced if lo[a] + 1.0 < v < hi[a] - 1.0), None)
        if cut is None and (len(ids) <= max_leaf or depth >= max_depth):
            leafs.append((ids, lo, hi))
            return -len(leafs)  # leaf k is -1 - k
        if cut:
            axis, split = cut
        else:
            axis = max(range(3), key=lambda k: hi[k] - lo[k])
            centres = sorted((boxes[i][0][axis] + boxes[i][1][axis]) / 2 for i in ids)
            split = centres[len(centres) // 2]
            split = min(max(split, lo[axis] + 1.0), hi[axis] - 1.0)
        front = [i for i in ids if boxes[i][1][axis] > split - 0.5]
        back = [i for i in ids if boxes[i][0][axis] < split + 0.5]
        if not cut and len(front) == len(ids) and len(back) == len(ids):
            leafs.append((ids, lo, hi))
            return -len(leafs)
        node = len(nodes)
        normal = [0.0, 0.0, 0.0]
        normal[axis] = 1.0
        planes.append((*normal, split))
        nodes.append([len(planes) - 1, 0, 0])
        fhi, blo = list(hi), list(lo)
        flo, bhi = list(lo), list(hi)
        flo[axis] = split
        bhi[axis] = split
        nodes[node][1] = build(front, flo, fhi, depth + 1)
        nodes[node][2] = build(back, blo, bhi, depth + 1)
        return node

    build(list(range(len(boxes))), list(world_min), list(world_max), 0)
    return planes, nodes, leafs


def code_rows(data, row_bytes, count):
    """As bspconv's convert.code_rows: the visibility rows, run-length coded."""
    head = bytearray(4 * count)
    body = bytearray()
    for c in range(count):
        row = bytes(data[c * row_bytes:(c + 1) * row_bytes])
        struct.pack_into("<I", head, 4 * c, 4 * count + len(body))
        i = 0
        while i < len(row):
            b = row[i]
            if b in (0x00, 0xFF):
                j = i
                while j < len(row) and row[j] == b and j - i < 255:
                    j += 1
                body += bytes((b, j - i))
                i = j
            else:
                body.append(b)
                i += 1
    return bytes(head + body)


def build(out_path, log=print):
    lv = design()
    textures = art.map_textures()
    textures["black_metal"] = (art._tile(lambda i, d: d.rectangle((0, 0, 64, 64), fill=(40, 40, 45, 255)), noise=3), 64)
    textures["awning"] = (art._tile(art._stripes((200, 35, 35), (245, 245, 240), 4), size=16, noise=0), 32)
    names = sorted(textures)
    tex_ids = {n: i for i, n in enumerate(names)}

    # Faces, grouped into batches by texture and a 512-unit grid cell (for culling)
    groups = {}

    def add_face(corners, tex, fit, fullbright):
        img, scale = textures[tex]
        fit = fit or scale is None
        colour = (lambda p, n: 0xFFFFFFFF) if fullbright else (lambda p, n: _abgr(light_at(lv, p, n)))
        verts, tris, _n = tessellate(corners, tex, scale or 64.0, fit, colour)
        cx = sum(c[0] for c in corners) / 4
        cy = sum(c[1] for c in corners) / 4
        key = (tex_ids[tex], int(math.floor(cx / 512)), int(math.floor(cy / 512)))
        g = groups.setdefault(key, ([], []))
        base = len(g[0])
        g[0].extend(verts)
        g[1].extend((a + base, b + base, c + base) for a, b, c in tris)

    for mins, maxs, tex, _shadow in lv.solids:
        for face, make in FACES.items():
            t = (tex[face] if face in tex else tex.get("side")) if tex else None
            if t is None or (face == "bottom" and mins[2] <= 0.0):
                continue
            add_face(make(mins, maxs), t, False, False)
    for corners, tex, fit, fullbright in lv.quads:
        add_face(corners, tex, fit, fullbright)

    # Textures
    txdt = bytearray()
    tex_records = []
    for n in names:
        img, _scale = textures[n]
        pt = pspimage.convert(img, max(img.width, img.height), False, max_levels=3)
        clut_ofs = len(txdt)
        txdt += pt.clut
        lvls = []
        for lvl in pt.levels:
            txdt += b"\0" * ((-len(txdt)) % 16)
            lvls.append(len(txdt))
            txdt += lvl
        txdt += b"\0" * ((-len(txdt)) % 16)
        tex_records.append((pt.width, pt.height, len(pt.levels), 0, clut_ofs, lvls))

    # Vertices and batches (as convert.py's emit_batch: 16-bit positions and UVs, decoded per batch)
    vert_data, idx_data, batch_data = bytearray(), bytearray(), bytearray()
    nverts = nidx = 0
    nbatches = 0
    wmin, wmax = [1e9] * 3, [-1e9] * 3
    for key in sorted(groups):
        verts, tris = groups[key]
        for start in range(0, len(tris), 20000):  # keep each batch's vertices within 16-bit indices
            chunk = tris[start:start + 20000]
            used = sorted({i for t in chunk for i in t})
            remap = {old: k for k, old in enumerate(used)}
            bv = [verts[i] for i in used]
            mins = [min(v[3 + k] for v in bv) for k in range(3)]
            maxs = [max(v[3 + k] for v in bv) for k in range(3)]
            for k in range(3):
                wmin[k], wmax[k] = min(wmin[k], mins[k]), max(wmax[k], maxs[k])
            extent = max(maxs[k] - mins[k] for k in range(3))
            step = 1.0 / 16.0
            while extent / step > 32000:
                step *= 2.0
            org = [math.floor(mins[k] / step) * step for k in range(3)]
            umin, vmin = math.floor(min(v[0] for v in bv)), math.floor(min(v[1] for v in bv))
            urange = max(max(v[0] for v in bv) - umin, 1e-3)
            vrange = max(max(v[1] for v in bv) - vmin, 1e-3)
            batch_data += struct.pack("<3I2H6f3f2f2f3I", nverts, nidx, len(chunk) * 3, key[0], 0, *mins, *maxs, *org,
                                      umin, vmin, urange, vrange, int(step * 65536), 0, 0)
            for u, v, col, x, y, z in bv:
                qu = min(65535, max(0, int(round((u - umin) / urange * 32768.0))))
                qv = min(65535, max(0, int(round((v - vmin) / vrange * 32768.0))))
                vert_data += struct.pack("<2HI3hh", qu, qv, col, int(round((x - org[0]) / step)),
                                         int(round((y - org[1]) / step)), int(round((z - org[2]) / step)), 0)
            for t in chunk:
                idx_data += struct.pack("<3H", *(remap[i] for i in t))
            nverts += len(bv)
            nidx += len(chunk) * 3
            nbatches += 1

    # Collision
    boxes = [(mins, maxs) for mins, maxs, _t, _s in lv.solids]
    walls = [(0, IN_MIN[0]), (0, IN_MAX[0]), (1, IN_MIN[1]), (1, IN_MAX[1]), (2, IN_MAX[2])]
    planes, nodes, leafs = kd_tree(boxes, [-500.0, -1100.0, -64.0], [1500.0, 1100.0, 600.0], walls)
    # Each box a brush of six real planes (not the converter's compact box-bevel form, which the physics'
    # point tests skip: props would fall through the floor)
    brush_data, sides = bytearray(), []
    for mins, maxs in boxes:
        brush_data += struct.pack("<3I", len(sides), 6, CONTENTS_SOLID)
        for axis in range(3):
            n = [0.0, 0.0, 0.0]
            n[axis] = 1.0
            sides.append(len(planes))
            planes.append((*n, maxs[axis]))
            n[axis] = -1.0
            sides.append(len(planes))
            planes.append((*n, -mins[axis]))
    side_data = struct.pack(f"<{len(sides)}I", *sides)
    leaf_data, leafbrush = bytearray(), []
    for ids, lo, hi in leafs:
        centre = tuple((lo[k] + hi[k]) / 2 for k in range(3))
        # Props and characters are lit by this (x 1.4 out of the sun, game.c make_light), plus the sun if they see it
        inside = all(IN_MIN[k] - 1 <= centre[k] <= IN_MAX[k] + 1 for k in range(3))
        leaf_data += struct.pack("<iIIIII", 0, 0, len(leafbrush), len(ids), _abgr(PROP_INDOOR if inside else PROP_OUTDOOR), 0)
        leafbrush.extend(ids)
    row_words = (nbatches + 31) // 32
    pvs = b"\xff" * (row_words * 4)

    def rgb(c):
        return _abgr(c)

    w = PmapWriter()
    w.add("INFO", struct.pack("<4I2f6f2I3f2I5I", 1, row_words, rgb(SKY_BOTTOM), rgb(SKY_BOTTOM), 1400.0, 3600.0,
                              *wmin, *wmax, rgb(SKY_TOP), rgb(SKY_BOTTOM), *SUN_DIR, rgb(SUN_COLOUR),
                              rgb(INDOOR_AMBIENT), 0, 0, 0, 0, 0), 1)
    w.add("SPWN", b"".join(struct.pack("<4f", x, y, z + 1.0, yaw) for x, y, z, yaw in lv.spawns), len(lv.spawns))
    w.add("TEXS", bytes(32 * len(tex_records)), len(tex_records))
    w.add("TXDT", txdt, len(txdt))
    w.add("VERT", vert_data, nverts)
    w.add("INDX", idx_data, nidx)
    w.add("BTCH", batch_data, nbatches)
    w.add("PLAN", b"".join(struct.pack("<4f", *p) for p in planes), len(planes))
    w.add("NODE", b"".join(struct.pack("<3i", *n) for n in nodes), len(nodes))
    w.add("LEAF", leaf_data, len(leafs))
    w.add("PVSZ", code_rows(pvs, row_words * 4, 1), 1)
    w.add("BRSH", brush_data, len(boxes))
    w.add("BSID", side_data, len(side_data) // 4)
    w.add("LBRS", struct.pack(f"<{len(leafbrush)}I", *leafbrush), len(leafbrush))
    w.add("ENTS", b"", 0)
    w.add("EBAT", b"", 0)
    w.add("EBRL", b"", 0)
    w.add("EOUT", b"", 0)
    w.add("SPRP", b"", 0)
    w.add("SPVZ", b"", 0)
    w.add("PHYS", b"", 0)
    w.add("LSTY", b"", 0)
    w.add("LSTP", bytes(64 * 64), 64)
    w.add("SKYT", struct.pack("<i", -1), 1)
    base = w.section_offset("TXDT")
    tex_data = bytearray()
    for width, height, levels, flags, clut_ofs, level_ofs in tex_records:
        lvs = [base + o for o in level_ofs] + [0] * (5 - len(level_ofs))
        tex_data += struct.pack("<HHBBHI5I", width, height, levels, flags, 0, base + clut_ofs, *lvs)
    w.replace("TEXS", tex_data)
    size = w.write(out_path)
    log(f"  wrote {out_path}: {nverts} verts, {nidx // 3} tris, {nbatches} batches, {len(tex_records)} textures, "
        f"{len(boxes)} brushes in {len(leafs)} leaves, {size / 1024:.0f} KB")
    return lv
