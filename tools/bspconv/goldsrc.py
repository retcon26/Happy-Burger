"""Half-Life (GoldSrc, BSP version 30) maps, presented to convert.py as if they were Source maps.

GoldSrc maps differ from Source ones in a few ways this module papers over:
- No brushes: collision is in the BSP tree itself, every solid region ending in the shared leaf 0.
  Each path that ends in solid becomes a convex brush of its own (the planes along the path), in a
  new leaf of its own, so the PSP's brush tracing works unchanged.
- Visibility is per leaf rather than per cluster: world leaf i (1-based) is cluster i - 1.
- Lightmaps are plain 8-bit RGB with no exponent, at 16 units a luxel, with no lightmap vectors:
  they're rewritten as Source's ColorRGBExp32, with the texture vectors scaled down for lookups.
- Textures are 8-bit paletted, inside the map or in .wad files, not .vmt/.vtf: WadMaterials loads
  them with the same interface as MaterialLoader. '{' textures are see-through where the palette's
  last colour is used; the sky is six .tga files in gfx/env.
- Entities: doors move along "angle", buttons fire their "target" rather than outputs.
"""

import math
import os
import struct

import numpy as np
from PIL import Image

from .bsp import (CONTENTS_SOLID, CONTENTS_WATER, SURF_NODRAW, SURF_SKY, BrushSide, Brush, Face, Leaf, Model, Node,
                  Plane, TexData, TexInfo, parse_entities)
from .materials import Material

BSP30 = 30
# GoldSrc leaf contents
CONTENTS_EMPTY_Q, CONTENTS_SOLID_Q, CONTENTS_WATER_Q, CONTENTS_SLIME_Q, CONTENTS_LAVA_Q, CONTENTS_SKY_Q = -1, -2, -3, -4, -5, -6
TEX_SPECIAL = 1
# Texture names that never draw: compile tools, triggers, sky (the sky is drawn from the skybox)
INVISIBLE = ("sky", "aaatrigger", "clip", "origin", "null", "skip", "hint", "bevel", "solidhint", "noclip",
             "black_hidden")
WORLD_EXTENT = 32768.0


def is_goldsrc(path):
    with open(path, "rb") as f:
        head = f.read(4)
    return len(head) == 4 and struct.unpack("<i", head)[0] == BSP30


class GoldSrcBsp:
    """The subset of bsp.Bsp that convert.py, geometry.py and collision.py use."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.raw = f.read()
        self.path = path
        self.version = struct.unpack_from("<i", self.raw, 0)[0]
        if self.version != BSP30:
            raise ValueError(f"{path}: not a Half-Life BSP (version {self.version})")
        self.revision = 0
        self.lumps = [struct.unpack_from("<ii", self.raw, 4 + i * 8) for i in range(15)]
        self._parse()

    def lump(self, i):
        o, n = self.lumps[i]
        return self.raw[o:o + n]

    def _parse(self):
        self.entities_text = self.lump(0).rstrip(b"\0").decode("latin-1")

        d = self.lump(1)
        self.planes = []
        for i in range(len(d) // 20):
            nx, ny, nz, dist, t = struct.unpack_from("<4fi", d, i * 20)
            self.planes.append(Plane((nx, ny, nz), dist, t))

        # Textures (the miptex lump): names, sizes, and the pixels of the ones stored in the map
        d = self.lump(2)
        self.miptex = []  # (name, width, height, offset of the miptex in d or None)
        self.miptex_data = d
        if d:
            n = struct.unpack_from("<i", d, 0)[0]
            for i in range(n):
                o = struct.unpack_from("<i", d, 4 + i * 4)[0]
                if o < 0:
                    self.miptex.append(("", 16, 16, None))
                    continue
                name = d[o:o + 16].split(b"\0")[0].decode("latin-1").lower()
                w, h = struct.unpack_from("<2I", d, o + 16)
                offs = struct.unpack_from("<4I", d, o + 24)
                self.miptex.append((name, w, h, o if offs[0] else None))

        d = self.lump(3)
        self.vertexes = [struct.unpack_from("<3f", d, i * 12) for i in range(len(d) // 12)]
        self.visibility = self.lump(4)

        d = self.lump(5)
        self.nodes = []
        for i in range(len(d) // 24):
            v = struct.unpack_from("<i2h6h2H", d, i * 24)
            nd = Node()
            nd.planenum, nd.children = v[0], [v[1], v[2]]
            nd.mins, nd.maxs = v[3:6], v[6:9]
            nd.firstface, nd.numfaces = v[9], v[10]
            self.nodes.append(nd)

        d = self.lump(6)
        self.texinfo = []
        for i in range(len(d) // 40):
            v = struct.unpack_from("<8f2i", d, i * 40)
            ti = TexInfo()
            ti.tex_s, ti.tex_t = v[0:4], v[4:8]
            # Lightmaps are a luxel per 16 texels, from the same vectors
            ti.lm_s = tuple(x / 16.0 for x in v[0:4])
            ti.lm_t = tuple(x / 16.0 for x in v[4:8])
            ti.texdata = v[8]
            ti.flags = 0
            name = self.miptex[v[8]][0] if 0 <= v[8] < len(self.miptex) else ""
            if name == "sky":
                ti.flags |= SURF_SKY
            elif name in INVISIBLE or name.startswith("aaatrigger"):
                ti.flags |= SURF_NODRAW
            self.texinfo.append(ti)

        self.texdata = []
        self._wads = None
        for name, w, h, _o in self.miptex:
            td = TexData()
            td.name, td.width, td.height = name, w, h
            td.reflectivity = (0.5, 0.5, 0.5)
            self.texdata.append(td)

        d = self.lump(7)
        self.faces = []
        raw_light = self.lump(8)
        for i in range(len(d) // 20):
            planenum, side, firstedge, numedges, texinfo = struct.unpack_from("<2HiHH", d, i * 20)
            styles = tuple(d[i * 20 + 12:i * 20 + 16])
            lightofs = struct.unpack_from("<i", d, i * 20 + 16)[0]
            f = Face()
            f.planenum, f.side, f.firstedge, f.numedges, f.texinfo = planenum, side, firstedge, numedges, texinfo
            f.dispinfo = -1
            f.styles = styles
            f.lightofs = lightofs
            f.orig_face = i
            f.lm_mins, f.lm_size = (0, 0), (0, 0)
            self.faces.append(f)
        self.faces_hdr = None

        d = self.lump(9)
        self.clipnodes = d  # unused: hull 0 is what's drawn and traced

        d = self.lump(10)
        self.leafs = []
        self._visofs = []
        for i in range(len(d) // 28):
            contents, visofs = struct.unpack_from("<2i", d, i * 28)
            mins = struct.unpack_from("<3h", d, i * 28 + 8)
            maxs = struct.unpack_from("<3h", d, i * 28 + 14)
            flf, nlf = struct.unpack_from("<2H", d, i * 28 + 20)
            lf = Leaf()
            lf.contents = contents
            lf.cluster = -1
            lf.area = 0
            lf.mins, lf.maxs = mins, maxs
            lf.firstleafface, lf.numleaffaces = flf, nlf
            lf.firstleafbrush, lf.numleafbrushes = 0, 0
            self._visofs.append(visofs)
            self.leafs.append(lf)

        d = self.lump(11)
        self.leaffaces = list(struct.unpack_from(f"<{len(d) // 2}H", d, 0))
        d = self.lump(12)
        self.edges = [struct.unpack_from("<2H", d, i * 4) for i in range(len(d) // 4)]
        d = self.lump(13)
        self.surfedges = list(struct.unpack_from(f"<{len(d) // 4}i", d, 0))

        d = self.lump(14)
        self.models = []
        self.num_visleafs = 0
        for i in range(len(d) // 64):
            v = struct.unpack_from("<9f4i3i", d, i * 64)
            m = Model()
            m.mins, m.maxs, m.origin = v[0:3], v[3:6], v[6:9]
            m.headnode = v[9]
            m.firstface, m.numfaces = v[14], v[15]
            self.models.append(m)
            if i == 0:
                self.num_visleafs = v[13]

        self.dispinfo = []
        self.disp_verts = []
        self.lighting_hdr = b""
        self._fix_lightmaps(raw_light)
        self._number_clusters()
        self._make_brushes()

    # --- Lighting ---------------------------------------------------------------------------------

    def _fix_lightmaps(self, raw):
        """Face lightmap extents (as Source stores them) and the lighting lump as ColorRGBExp32.

        GoldSrc's 8-bit lightmap value is what the screen shows (texture x light / 255); convert.py
        draws Source lighting as gamma(linear / 2) doubled, so the value is mapped back through that."""
        for f in self.faces:
            ti = self.texinfo[f.texinfo]
            if f.lightofs < 0:
                continue
            vs = self.face_vertices(f)
            ext = []
            for vec in (ti.tex_s, ti.tex_t):
                vals = [p[0] * vec[0] + p[1] * vec[1] + p[2] * vec[2] + vec[3] for p in vs]
                lo, hi = math.floor(min(vals) / 16.0), math.ceil(max(vals) / 16.0)
                ext.append((lo, hi - lo))
            f.lm_mins = (ext[0][0], ext[1][0])
            f.lm_size = (ext[0][1], ext[1][1])
            f.lightofs = f.lightofs // 3 * 4
        g = np.frombuffer(raw, dtype=np.uint8)[: len(raw) // 3 * 3].reshape(-1, 3).astype(np.float64)
        # A little brighter than the raw value, like Half-Life's own light gamma
        shown = np.minimum(255.0, 270.0 * (g / 255.0) ** 0.65)
        lin = 2.0 * (shown / 510.0) ** 2.2
        out = np.zeros((len(g), 4), np.uint8)
        peak = lin.max(axis=1)
        e = np.where(peak > 0, np.ceil(np.log2(np.maximum(peak, 1e-12))), 0).clip(-60, 7)
        out[:, :3] = np.clip(np.round(lin * 255.0 / np.exp2(e)[:, None]), 0, 255)
        out[:, 3] = e.astype(np.int8).view(np.uint8)
        self.lighting = out.tobytes()
        self._lin = lin

    def leaf_ambient(self):
        """Average lighting of the faces in each leaf (linear), for lighting props and NPCs."""
        out = []
        for lf in self.leafs:
            acc, n = np.zeros(3), 0
            for i in range(lf.firstleafface, lf.firstleafface + lf.numleaffaces):
                f = self.faces[self.leaffaces[i]] if i < len(self.leaffaces) else None
                if f is None or f.lightofs < 0 or f.styles[0] == 255:
                    continue
                first = f.lightofs // 4
                cnt = (f.lm_size[0] + 1) * (f.lm_size[1] + 1)
                block = self._lin[first:first + cnt]
                if len(block):
                    acc += block.mean(axis=0)
                    n += 1
            out.append(tuple(acc / n) if n else None)
        return out

    # --- Visibility -------------------------------------------------------------------------------

    def _number_clusters(self):
        for i, lf in enumerate(self.leafs):
            lf.cluster = i - 1 if 1 <= i <= self.num_visleafs else -1

    def num_clusters(self):
        return self.num_visleafs

    def cluster_pvs(self, cluster):
        if cluster < 0 or cluster >= self.num_visleafs:
            return None
        ofs = self._visofs[cluster + 1]
        n = self.num_visleafs
        if ofs < 0 or not self.visibility:
            return set(range(n))
        vis = self.visibility
        out = set()
        c = 0
        while c < n and ofs < len(vis):
            b = vis[ofs]
            if b == 0:
                c += 8 * vis[ofs + 1]
                ofs += 2
                continue
            for bit in range(8):
                if b & (1 << bit) and c + bit < n:
                    out.add(c + bit)
            c += 8
            ofs += 1
        out.add(cluster)
        return out

    # --- Collision --------------------------------------------------------------------------------

    def _make_brushes(self):
        """A convex brush for every solid region of the tree, each in a leaf of its own.

        Walks each model's tree carrying the region it covers (a convex polytope, as face polygons
        tagged with the plane they lie on), cut by each node's plane on the way down. A region that
        ends in solid becomes a brush with the planes of its remaining faces, plus axial bevels."""
        self.brushes, self.brushsides, self.leafbrushes = [], [], []
        plane_index = {}
        for i, p in enumerate(self.planes):
            plane_index.setdefault(_plane_key(p.normal, p.dist), i)

        def add_plane(n, d):
            key = _plane_key(n, d)
            i = plane_index.get(key)
            if i is None:
                i = len(self.planes)
                self.planes.append(Plane(tuple(n), d, 0))
                plane_index[key] = i
            return i

        mapped = {CONTENTS_SOLID_Q: CONTENTS_SOLID, CONTENTS_SKY_Q: CONTENTS_SOLID, CONTENTS_WATER_Q: CONTENTS_WATER,
                  CONTENTS_SLIME_Q: CONTENTS_WATER, CONTENTS_LAVA_Q: CONTENTS_WATER}
        orig_leafs = len(self.leafs)
        for lf in self.leafs:
            lf.contents = mapped.get(lf.contents, 0)

        def make_brush(polys, contents):
            pts = [p for _tag, poly in polys for p in poly]
            if len(pts) < 4:
                return None
            mins = [min(p[k] for p in pts) for k in range(3)]
            maxs = [max(p[k] for p in pts) for k in range(3)]
            if any(maxs[k] - mins[k] < 0.05 for k in range(3)):
                return None
            sides = []
            seen = set()
            for tag, _poly in polys:
                if tag is not None and tag not in seen:
                    seen.add(tag)
                    sides.append((add_plane(*tag), 0))
            axial = {(round(n[0], 4), round(n[1], 4), round(n[2], 4)) for n, _d in seen}
            for k in range(3):
                for sgn in (1.0, -1.0):
                    n = [0.0, 0.0, 0.0]
                    n[k] = sgn
                    if tuple(n) in axial:
                        continue
                    sides.append((add_plane(n, maxs[k] if sgn > 0 else -mins[k]), 1))
            first = len(self.brushsides)
            for pi, bevel in sides:
                s = BrushSide()
                s.planenum, s.texinfo, s.dispinfo, s.bevel = pi, -1, -1, bevel
                self.brushsides.append(s)
            b = Brush()
            b.firstside, b.numsides, b.contents = first, len(sides), contents
            self.brushes.append(b)
            return len(self.brushes) - 1

        def new_leaf(brush, contents):
            lf = Leaf()
            lf.contents, lf.cluster, lf.area = contents, -1, 0
            lf.mins, lf.maxs = (0, 0, 0), (0, 0, 0)
            lf.firstleafface, lf.numleaffaces = 0, 0
            lf.firstleafbrush = len(self.leafbrushes)
            lf.numleafbrushes = 1 if brush is not None else 0
            if brush is not None:
                self.leafbrushes.append(brush)
            self.leafs.append(lf)
            return len(self.leafs) - 1

        for m in self.models:
            if m.headnode < 0 or m.headnode >= len(self.nodes):
                continue
            stack = [(m.headnode, _box_polys(WORLD_EXTENT))]
            while stack:
                node, polys = stack.pop()
                nd = self.nodes[node]
                pl = self.planes[nd.planenum]
                n, d = pl.normal, pl.dist
                for side in (0, 1):
                    # The front child is where n.p >= d: as an outward brush side, -n.p <= -d
                    tag = ((-n[0], -n[1], -n[2]), -d) if side == 0 else (tuple(n), d)
                    sub = _clip_polytope(polys, tag)
                    if not sub:
                        continue
                    child = nd.children[side]
                    if child >= 0:
                        stack.append((child, sub))
                        continue
                    li = -1 - child
                    if li >= orig_leafs:
                        continue
                    contents = CONTENTS_SOLID if li == 0 else self.leafs[li].contents
                    if not contents & (CONTENTS_SOLID | CONTENTS_WATER):
                        continue
                    b = make_brush(sub, contents)
                    if li == 0:
                        # All solid space shares leaf 0: give this piece a leaf of its own
                        nd.children[side] = -1 - new_leaf(b, contents)
                    elif b is not None:
                        lf = self.leafs[li]
                        lf.firstleafbrush = len(self.leafbrushes)
                        lf.numleafbrushes = 1
                        self.leafbrushes.append(b)
    # --- The rest of Bsp's interface ----------------------------------------------------------------

    def face_vertices(self, face):
        out = []
        for i in range(face.numedges):
            se = self.surfedges[face.firstedge + i]
            out.append(self.vertexes[self.edges[se][0]] if se >= 0 else self.vertexes[self.edges[-se][1]])
        return out

    def entities(self):
        ents = parse_entities(self.entities_text)
        for e in ents:
            cls = e.get("classname", "")
            if "angle" in e and "angles" not in e:
                a = float(e["angle"] or 0)
                e["angles"] = "-90 0 0" if a == -1 else "90 0 0" if a == -2 else f"0 {a} 0"
            if cls.startswith("info_player_") and "origin" in e:
                # Half-Life puts the spawn at the player's middle, Source at the feet
                o = [float(x) for x in e["origin"].split()]
                if len(o) == 3:
                    e["origin"] = f"{o[0]} {o[1]} {o[2] - 36.0}"
            if cls in ("func_door", "func_button", "func_water") and "movedir" not in e:
                e["movedir"] = e.get("angles", "0 0 0")
            if cls in ("func_door", "func_door_rotating", "func_water"):
                # Half-Life doors open when walked into unless they're "use only" (256) or named
                # (opened by a button or trigger); Source wants "touch opens" (1024) spelled out.
                # "Passable" (8) doors aren't solid.
                try:
                    flags = int(float(e.get("spawnflags", "0") or 0))
                except ValueError:
                    flags = 0
                if not flags & 256 and not e.get("targetname"):
                    flags |= 1024
                if flags & 8:
                    e["_nonsolid"] = "1"
                e["spawnflags"] = str(flags & ~8)
            # Buttons and triggers fire their target; Source does it with outputs
            tgt = e.get("target")
            if tgt and (cls == "func_button" or cls.startswith("trigger_")):
                ev = "OnPressed" if cls == "func_button" else "OnTrigger"
                e["_outputs"].append((ev, f"{tgt},Toggle,,{e.get('delay', '0') or '0'},-1"))
        return ents

    def pakfile(self):
        return None

    def game_lumps(self):
        return {}

    def wad_names(self):
        ws = parse_entities(self.entities_text)
        wad = ws[0].get("wad", "") if ws else ""
        return [os.path.basename(w.replace("\\", "/")).lower() for w in wad.split(";") if w.strip()]

    def material_loader(self):
        return WadMaterials(self)


def _plane_key(n, d):
    return (round(n[0], 5), round(n[1], 5), round(n[2], 5), round(d, 3))


def _box_polys(e):
    c = [(x, y, z) for x in (-e, e) for y in (-e, e) for z in (-e, e)]
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    return [(None, [c[i] for i in q]) for q in quads]


def _clip_polytope(polys, tag, eps=0.01):
    """The part of a convex polytope (its faces as (plane tag, polygon)) behind the plane
    tag = (n, d), i.e. n.p <= d, with the cut capped by a new face tagged with the plane.
    An empty list when nothing is left."""
    n, d = tag
    out = []
    cap = []
    cut = False
    for ptag, poly in polys:
        dists = [p[0] * n[0] + p[1] * n[1] + p[2] * n[2] - d for p in poly]
        if all(x <= eps for x in dists):
            out.append((ptag, poly))
            for p, x in zip(poly, dists):
                if x >= -eps:
                    cap.append(p)
            continue
        cut = True
        if all(x >= -eps for x in dists):
            continue
        kept = []
        for i in range(len(poly)):
            a, b = poly[i], poly[(i + 1) % len(poly)]
            da, db = dists[i], dists[(i + 1) % len(poly)]
            if da <= eps:
                kept.append(a)
                if da >= -eps:
                    cap.append(a)
            if (da < -eps and db > eps) or (da > eps and db < -eps):
                t = da / (da - db)
                p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)
                kept.append(p)
                cap.append(p)
        if len(kept) >= 3:
            out.append((ptag, kept))
    if not out:
        return []
    if cut:
        face = _order_on_plane(cap, n)
        if len(face) >= 3:
            out.append(((tuple(n), d), face))
    return out

def _order_on_plane(pts, n):
    """The distinct points, in order round their centre on the plane with normal n."""
    uniq = []
    for p in pts:
        if all(abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]) > 0.01 for q in uniq):
            uniq.append(p)
    if len(uniq) < 3:
        return uniq
    c = [sum(p[k] for p in uniq) / len(uniq) for k in range(3)]
    a = (1.0, 0.0, 0.0) if abs(n[0]) < 0.9 else (0.0, 1.0, 0.0)
    u = (n[1] * a[2] - n[2] * a[1], n[2] * a[0] - n[0] * a[2], n[0] * a[1] - n[1] * a[0])
    v = (n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0])
    return sorted(uniq, key=lambda p: math.atan2(sum((p[k] - c[k]) * v[k] for k in range(3)),
                                                 sum((p[k] - c[k]) * u[k] for k in range(3))))

# --- Textures ---------------------------------------------------------------------------------------

def _miptex_image(data, o, transparent):
    """RGBA image of the miptex at offset o of data (mip 0, palette after the last mip)."""
    w, h = struct.unpack_from("<2I", data, o + 16)
    offs = struct.unpack_from("<4I", data, o + 24)
    pix = np.frombuffer(data, np.uint8, w * h, o + offs[0]).reshape(h, w)
    pal_ofs = o + offs[3] + (w // 8) * (h // 8) + 2
    pal = np.frombuffer(data, np.uint8, 768, pal_ofs).reshape(256, 3)
    rgba = np.empty((h, w, 4), np.uint8)
    rgba[..., :3] = pal[pix]
    rgba[..., 3] = 255
    if transparent:
        rgba[pix == 255, 3] = 0
    return Image.fromarray(rgba, "RGBA")


class Wad:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        magic, n, ofs = struct.unpack_from("<4sii", self.data, 0)
        if magic not in (b"WAD3", b"WAD2"):
            raise ValueError(path)
        self.entries = {}
        for i in range(n):
            filepos, _disk, _size, typ, _comp, _pad1, _pad2, name = struct.unpack_from("<3i4B16s", self.data, ofs + i * 32)
            if typ == 0x43:
                self.entries[name.split(b"\0")[0].decode("latin-1").lower()] = filepos


_WAD_CACHE = {}


def _wads_for(game_dir):
    if game_dir not in _WAD_CACHE:
        wads = []
        dirs = [game_dir, os.path.join(os.path.dirname(game_dir), "valve")]
        for d in dirs[:1] if os.path.samefile(dirs[0], dirs[1]) else dirs:
            if not os.path.isdir(d):
                continue
            for name in sorted(os.listdir(d)):
                if name.lower().endswith(".wad"):
                    try:
                        wads.append(Wad(os.path.join(d, name)))
                    except (OSError, ValueError, struct.error):
                        pass
        _WAD_CACHE[game_dir] = wads
    return _WAD_CACHE[game_dir]


class WadMaterials:
    """MaterialLoader's interface for a GoldSrc map: textures from the map itself or the game's .wads."""

    def __init__(self, bsp):
        self.bsp = bsp
        self.game_dir = os.path.dirname(os.path.dirname(os.path.abspath(bsp.path)))
        self.wads = _wads_for(self.game_dir)
        self._cache = {}
        self._embedded = {name: o for name, _w, _h, o in bsp.miptex if o is not None}
        # Average colours, for flat-shaded fallbacks
        for td in bsp.texdata:
            img = self._image(td.name)
            if img is not None:
                a = np.asarray(img.convert("RGB").resize((8, 8)), np.float64).reshape(-1, 3).mean(axis=0) / 255.0
                td.reflectivity = tuple(float(c) ** 2.2 for c in a)

    def get(self, name):
        key = name.lower()
        m = self._cache.get(key)
        if m is None:
            m = Material(key)
            m.base_texture = key
            m.alphatest = key.startswith("{")
            # Water: drawn as its texture, seen from below too (Source water is a flat colour)
            m.nocull = key.startswith("!") or key.startswith("*")
            m.params = {}
            self._cache[key] = m
        return m

    def _image(self, name):
        name = name.lower()
        if name.startswith("skybox/"):
            p = os.path.join(self.game_dir, "gfx", "env", name[7:] + ".tga")
            if not os.path.exists(p):
                p = os.path.join(os.path.dirname(self.game_dir), "valve", "gfx", "env", name[7:] + ".tga")
            return Image.open(p).convert("RGBA") if os.path.exists(p) else None
        o = self._embedded.get(name)
        if o is not None:
            return _miptex_image(self.bsp.miptex_data, o, name.startswith("{"))
        for w in self.wads:
            o = w.entries.get(name)
            if o is not None:
                return _miptex_image(w.data, o, name.startswith("{"))
        return None

    def load_texture(self, mat, max_dim):
        try:
            img = self._image(mat.base_texture or "")
        except (OSError, ValueError, struct.error):
            return None
        if img is None:
            return None
        if max(img.size) > max_dim:
            s = max_dim / max(img.size)
            img = img.resize((max(1, int(img.width * s)), max(1, int(img.height * s))), Image.Resampling.LANCZOS)
        return img
