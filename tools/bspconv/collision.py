"""Collision data for the PSP: convex brushes, which the player and props are traced against.

Map brushes are kept as they are, including the bevel planes vbsp adds so boxes slide smoothly.
Displacements have no brushes, so each displacement triangle becomes a thin prism brush (the triangle,
a back face and three edge planes), with its bounds instead of six axial bevel planes: the game makes those
when it traces (BOX_BEVELS). One trace routine on the PSP then handles both.
"""

import math
import struct

import numpy as np

from .bsp import (CONTENTS_GRATE, CONTENTS_MOVEABLE, CONTENTS_PLAYERCLIP, CONTENTS_SOLID, CONTENTS_WATER,
                  CONTENTS_WINDOW)
from .geometry import build_displacement, make_ccw

KEEP_CONTENTS = (CONTENTS_SOLID | CONTENTS_WINDOW | CONTENTS_GRATE | CONTENTS_MOVEABLE | CONTENTS_PLAYERCLIP
                 | CONTENTS_WATER)
DISP_THICKNESS = 16.0
# Set on side plane indices that are bevels: needed for smooth box sweeps, but not real surfaces.
BEVEL_BIT = 0x80000000
# Set on a brush's side count: after its sides come 6 floats (its mins and maxs, as raw bits), which stand for
# the 6 axis bevel planes of its box (pmap_format.h PMAP_BRUSH_BOXBEVELS). Terrain maps had hundreds of
# thousands of those planes, each different: about a third of gm_valley's and pl_goldrush's files.
BOX_BEVELS = 0x80000000


def leaves_touching_box(bsp, node, mins, maxs, out):
    """Collects the leaves a box overlaps by walking the BSP tree."""
    stack = [node]
    while stack:
        n = stack.pop()
        if n < 0:
            out.append(-1 - n)
            continue
        nd = bsp.nodes[n]
        pl = bsp.planes[nd.planenum]
        dmin = dmax = -pl.dist
        for k in range(3):
            a, b = pl.normal[k] * mins[k], pl.normal[k] * maxs[k]
            dmin += min(a, b)
            dmax += max(a, b)
        if dmax >= -1.0:
            stack.append(nd.children[0])
        if dmin <= 1.0:
            stack.append(nd.children[1])


class CollisionData:
    def __init__(self):
        self.planes = []       # (nx, ny, nz, dist); indices 0..len(bsp.planes)-1 match the BSP's planes
        self.brushes = []      # (first_side, num_sides, contents)
        self.sides = []        # plane index per side
        self.leaf_brushes = []  # per leaf: list of brush indices
        self.disp_tris = 0


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _norm(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return (v[0] / l, v[1] / l, v[2] / l) if l > 1e-6 else None


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def model_brushes(bsp, model_idx):
    """BSP brush indices used by a brush entity's model (from the leaves of its own subtree)."""
    out = []
    seen = set()
    stack = [bsp.models[model_idx].headnode]
    while stack:
        n = stack.pop()
        if n < 0:
            leaf = bsp.leafs[-1 - n]
            for i in range(leaf.firstleafbrush, leaf.firstleafbrush + leaf.numleafbrushes):
                b = bsp.leafbrushes[i]
                if b not in seen:
                    seen.add(b)
                    out.append(b)
            continue
        stack.extend(bsp.nodes[n].children)
    return out


def build_collision(bsp, faces, disp_power, static_ents=(), prop_boxes=(), prop_hulls=(), ent_boxes=()):
    """static_ents: (model index, origin) of brush entities that never move (func_brush...), merged
    into the world. prop_boxes: (x, y, z axes, origin, local mins, local maxs) for solid static props.
    prop_hulls: (x, y, z axes, origin, [(local planes, local points)]) for props with .phy pieces."""
    cd = CollisionData()
    cd.planes = [(p.normal[0], p.normal[1], p.normal[2], p.dist) for p in bsp.planes]
    plane_lookup = {}

    def plane_index(n, d):
        key = (round(n[0], 4), round(n[1], 4), round(n[2], 4), round(d, 2))
        i = plane_lookup.get(key)
        if i is None:
            i = len(cd.planes)
            cd.planes.append((n[0], n[1], n[2], d))
            plane_lookup[key] = i
        return i

    remap = {}
    for bi, b in enumerate(bsp.brushes):
        if not b.contents & KEEP_CONTENTS:
            continue
        remap[bi] = len(cd.brushes)
        first = len(cd.sides)
        for s in range(b.firstside, b.firstside + b.numsides):
            bs = bsp.brushsides[s]
            cd.sides.append(bs.planenum | (BEVEL_BIT if bs.bevel else 0))
        cd.brushes.append((first, b.numsides, b.contents & 0xFFFFFFFF))

    for leaf in bsp.leafs:
        lst = []
        for i in range(leaf.firstleafbrush, leaf.firstleafbrush + leaf.numleafbrushes):
            nb = remap.get(bsp.leafbrushes[i])
            if nb is not None:
                lst.append(nb)
        cd.leaf_brushes.append(lst)

    no_light = np.zeros((0, 3), np.float32)
    world = bsp.models[0]
    for fi in range(world.firstface, world.firstface + world.numfaces):
        f = faces[fi]
        if f.dispinfo < 0:
            continue
        di = bsp.dispinfo[f.dispinfo]
        if di.contents and not di.contents & KEEP_CONTENTS:
            continue
        mesh = build_displacement(bsp, f, no_light, disp_power)
        make_ccw(bsp, f, mesh)
        for a, b, c in mesh.tris:
            pa, pb, pc = mesh.verts[a][3:6], mesh.verts[b][3:6], mesh.verts[c][3:6]
            n = _norm(_cross(tuple(pb[k] - pa[k] for k in range(3)), tuple(pc[k] - pa[k] for k in range(3))))
            if n is None:
                continue
            top = [pa, pb, pc]
            bot = [tuple(p[k] - n[k] * DISP_THICKNESS for k in range(3)) for p in top]
            sides = [plane_index(n, _dot(n, pa)), plane_index((-n[0], -n[1], -n[2]), -_dot(n, bot[0]))]
            for p0, p1 in ((pa, pb), (pb, pc), (pc, pa)):
                en = _norm(_cross(tuple(p1[k] - p0[k] for k in range(3)), n))
                if en is not None:
                    sides.append(plane_index(en, _dot(en, p0)))
            pts = top + bot
            mins = [min(p[k] for p in pts) for k in range(3)]
            maxs = [max(p[k] for p in pts) for k in range(3)]
            bidx = len(cd.brushes)
            cd.brushes.append((len(cd.sides), len(sides) | BOX_BEVELS, CONTENTS_SOLID))
            cd.sides.extend(sides)
            cd.sides.extend(struct.unpack("<6I", struct.pack("<6f", *mins, *maxs)))
            leaves = []
            leaves_touching_box(bsp, world.headnode, mins, maxs, leaves)
            for l in leaves:
                cd.leaf_brushes[l].append(bidx)
            cd.disp_tris += 1

    cd.remap = remap

    # Static brush entities: copies of their brushes moved to the entity origin, added to the world
    cd.static_ent_brushes = 0
    for model_idx, origin in static_ents:
        md = bsp.models[model_idx]
        mins = [md.mins[k] + origin[k] - 1 for k in range(3)]
        maxs = [md.maxs[k] + origin[k] + 1 for k in range(3)]
        leaves = []
        leaves_touching_box(bsp, world.headnode, mins, maxs, leaves)
        for bi in model_brushes(bsp, model_idx):
            b = bsp.brushes[bi]
            if not b.contents & KEEP_CONTENTS:
                continue
            sides = []
            for s in range(b.firstside, b.firstside + b.numsides):
                bs = bsp.brushsides[s]
                p = bsp.planes[bs.planenum]
                d = p.dist + _dot(p.normal, origin)
                sides.append(plane_index(p.normal, d) | (BEVEL_BIT if bs.bevel else 0))
            nb = len(cd.brushes)
            cd.brushes.append((len(cd.sides), len(sides), b.contents & 0xFFFFFFFF))
            cd.sides.extend(sides)
            for l in leaves:
                cd.leaf_brushes[l].append(nb)
            cd.static_ent_brushes += 1

    # Solid static props: an oriented box each, with axial bevels for smooth sliding
    for (ax, ay, az), origin, lmin, lmax in prop_boxes:
        axes = (ax, ay, az)
        lc = [(lmin[k] + lmax[k]) / 2 for k in range(3)]
        half = [(lmax[k] - lmin[k]) / 2 for k in range(3)]
        c = tuple(origin[k] + ax[k] * lc[0] + ay[k] * lc[1] + az[k] * lc[2] for k in range(3))
        sides = []
        for i in range(3):
            a = axes[i]
            sides.append(plane_index(a, _dot(a, c) + half[i]))
            na = (-a[0], -a[1], -a[2])
            sides.append(plane_index(na, _dot(na, c) + half[i]))
        ext = [sum(abs(axes[i][k]) * half[i] for i in range(3)) for k in range(3)]
        mins = [c[k] - ext[k] for k in range(3)]
        maxs = [c[k] + ext[k] for k in range(3)]
        for k in range(3):
            v = [0.0, 0.0, 0.0]
            v[k] = 1.0
            sides.append(plane_index(tuple(v), maxs[k]) | BEVEL_BIT)
            v[k] = -1.0
            sides.append(plane_index(tuple(v), -mins[k]) | BEVEL_BIT)
        nb = len(cd.brushes)
        cd.brushes.append((len(cd.sides), len(sides), CONTENTS_SOLID))
        cd.sides.extend(sides)
        leaves = []
        leaves_touching_box(bsp, world.headnode, mins, maxs, leaves)
        for l in leaves:
            cd.leaf_brushes[l].append(nb)

    # Props with collision models: a brush per convex piece, its planes turned into world space
    cd.prop_hull_pieces = 0
    for (ax, ay, az), origin, pieces in prop_hulls:
        for planes, pts in pieces:
            sides = []
            for nx, ny, nz, d in planes:
                n = tuple(ax[k] * nx + ay[k] * ny + az[k] * nz for k in range(3))
                sides.append(plane_index(n, d + _dot(n, origin)))
            wp = [tuple(origin[k] + ax[k] * p[0] + ay[k] * p[1] + az[k] * p[2] for k in range(3)) for p in pts]
            mins = [min(p[k] for p in wp) for k in range(3)]
            maxs = [max(p[k] for p in wp) for k in range(3)]
            for k in range(3):
                v = [0.0, 0.0, 0.0]
                v[k] = 1.0
                sides.append(plane_index(tuple(v), maxs[k]) | BEVEL_BIT)
                v[k] = -1.0
                sides.append(plane_index(tuple(v), -mins[k]) | BEVEL_BIT)
            nb = len(cd.brushes)
            cd.brushes.append((len(cd.sides), len(sides), CONTENTS_SOLID))
            cd.sides.extend(sides)
            leaves = []
            leaves_touching_box(bsp, world.headnode, mins, maxs, leaves)
            for l in leaves:
                cd.leaf_brushes[l].append(nb)
            cd.prop_hull_pieces += 1

    # Model doors (prop_door_rotating): a box each in the door entity's own frame, in no leaf (the
    # entity's brush list finds it)
    cd.ent_box_brushes = []
    for (ax, ay, az), lmin, lmax in ent_boxes:
        axes = (ax, ay, az)
        lc = [(lmin[k] + lmax[k]) / 2 for k in range(3)]
        half = [(lmax[k] - lmin[k]) / 2 for k in range(3)]
        c = tuple(ax[k] * lc[0] + ay[k] * lc[1] + az[k] * lc[2] for k in range(3))
        sides = []
        for i in range(3):
            a = axes[i]
            sides.append(plane_index(a, _dot(a, c) + half[i]))
            na = (-a[0], -a[1], -a[2])
            sides.append(plane_index(na, _dot(na, c) + half[i]))
        ext = [sum(abs(axes[i][k]) * half[i] for i in range(3)) for k in range(3)]
        for k in range(3):
            v = [0.0, 0.0, 0.0]
            v[k] = 1.0
            sides.append(plane_index(tuple(v), c[k] + ext[k]) | BEVEL_BIT)
            v[k] = -1.0
            sides.append(plane_index(tuple(v), -(c[k] - ext[k])) | BEVEL_BIT)
        cd.ent_box_brushes.append(len(cd.brushes))
        cd.brushes.append((len(cd.sides), len(sides), CONTENTS_SOLID))
        cd.sides.extend(sides)
    return cd
