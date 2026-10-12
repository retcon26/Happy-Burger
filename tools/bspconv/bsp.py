"""Parser for Source engine BSP maps (versions 19-21, which covers every GMod map).

Structure layouts follow the Source SDK 2013 bspfile.h.
"""

import io
import lzma
import struct
import zipfile

LUMP_ENTITIES = 0
LUMP_PLANES = 1
LUMP_TEXDATA = 2
LUMP_VERTEXES = 3
LUMP_VISIBILITY = 4
LUMP_NODES = 5
LUMP_TEXINFO = 6
LUMP_FACES = 7
LUMP_LIGHTING = 8
LUMP_LEAFS = 10
LUMP_EDGES = 12
LUMP_SURFEDGES = 13
LUMP_MODELS = 14
LUMP_LEAFFACES = 16
LUMP_LEAFBRUSHES = 17
LUMP_BRUSHES = 18
LUMP_BRUSHSIDES = 19
LUMP_DISPINFO = 26
LUMP_DISP_VERTS = 33
LUMP_GAME_LUMP = 35
LUMP_PAKFILE = 40
LUMP_TEXDATA_STRING_DATA = 43
LUMP_TEXDATA_STRING_TABLE = 44
LUMP_LEAF_AMBIENT_INDEX_HDR = 51
LUMP_LEAF_AMBIENT_INDEX = 52
LUMP_LIGHTING_HDR = 53
LUMP_LEAF_AMBIENT_LIGHTING_HDR = 55
LUMP_LEAF_AMBIENT_LIGHTING = 56
LUMP_FACES_HDR = 58

# texinfo flags
SURF_LIGHT = 0x1
SURF_SKY2D = 0x2
SURF_SKY = 0x4
SURF_WARP = 0x8
SURF_TRANS = 0x10
SURF_TRIGGER = 0x40
SURF_NODRAW = 0x80
SURF_HINT = 0x100
SURF_SKIP = 0x200
SURF_NOLIGHT = 0x400
SURF_BUMPLIGHT = 0x800

# brush contents
CONTENTS_SOLID = 0x1
CONTENTS_WINDOW = 0x2
CONTENTS_GRATE = 0x8
CONTENTS_WATER = 0x20
CONTENTS_MOVEABLE = 0x4000
CONTENTS_PLAYERCLIP = 0x10000
CONTENTS_MONSTERCLIP = 0x20000
CONTENTS_DETAIL = 0x8000000


class Plane:
    __slots__ = ("normal", "dist", "type")

    def __init__(self, normal, dist, type_):
        self.normal = normal
        self.dist = dist
        self.type = type_


class Face:
    __slots__ = ("planenum", "side", "firstedge", "numedges", "texinfo", "dispinfo", "styles", "lightofs",
                 "lm_mins", "lm_size", "orig_face")


class TexInfo:
    __slots__ = ("tex_s", "tex_t", "lm_s", "lm_t", "flags", "texdata")


class TexData:
    __slots__ = ("reflectivity", "name", "width", "height")


class Model:
    __slots__ = ("mins", "maxs", "origin", "headnode", "firstface", "numfaces")


class Node:
    __slots__ = ("planenum", "children", "mins", "maxs", "firstface", "numfaces")


class Leaf:
    __slots__ = ("contents", "cluster", "area", "mins", "maxs", "firstleafface", "numleaffaces",
                 "firstleafbrush", "numleafbrushes")


class Brush:
    __slots__ = ("firstside", "numsides", "contents")


class BrushSide:
    __slots__ = ("planenum", "texinfo", "dispinfo", "bevel")


class DispInfo:
    __slots__ = ("start_pos", "vert_start", "tri_start", "power", "contents", "map_face")


class Bsp:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.raw = f.read()
        ident, self.version = struct.unpack_from("<4si", self.raw, 0)
        if ident != b"VBSP":
            raise ValueError(f"{path}: not a Source BSP (ident {ident!r})")
        if not 19 <= self.version <= 21:
            raise ValueError(f"{path}: unsupported BSP version {self.version}")
        self.lumps = [struct.unpack_from("<iii4s", self.raw, 8 + i * 16) for i in range(64)]
        self.revision = struct.unpack_from("<i", self.raw, 8 + 64 * 16)[0]
        self._parse()

    def lump(self, idx):
        ofs, length, _version, fourcc = self.lumps[idx]
        data = self.raw[ofs : ofs + length]
        uncompressed = struct.unpack("<i", fourcc)[0]
        if uncompressed and data[:4] == b"LZMA":
            data = _lzma_decompress(data, uncompressed)
        return data

    def lump_version(self, idx):
        return self.lumps[idx][2]

    def _parse(self):
        self.entities_text = self.lump(LUMP_ENTITIES).rstrip(b"\0").decode("latin-1")

        d = self.lump(LUMP_PLANES)
        self.planes = []
        for i in range(len(d) // 20):
            nx, ny, nz, dist, t = struct.unpack_from("<4fi", d, i * 20)
            self.planes.append(Plane((nx, ny, nz), dist, t))

        d = self.lump(LUMP_VERTEXES)
        self.vertexes = [struct.unpack_from("<3f", d, i * 12) for i in range(len(d) // 12)]

        d = self.lump(LUMP_EDGES)
        self.edges = [struct.unpack_from("<2H", d, i * 4) for i in range(len(d) // 4)]

        d = self.lump(LUMP_SURFEDGES)
        self.surfedges = list(struct.unpack_from(f"<{len(d) // 4}i", d, 0))

        self.faces = self._parse_faces(self.lump(LUMP_FACES))
        hdr = self.lump(LUMP_FACES_HDR)
        self.faces_hdr = self._parse_faces(hdr) if hdr else None

        d = self.lump(LUMP_TEXINFO)
        self.texinfo = []
        for i in range(len(d) // 72):
            v = struct.unpack_from("<16f2i", d, i * 72)
            ti = TexInfo()
            ti.tex_s, ti.tex_t = v[0:4], v[4:8]
            ti.lm_s, ti.lm_t = v[8:12], v[12:16]
            ti.flags, ti.texdata = v[16], v[17]
            self.texinfo.append(ti)

        strdata = self.lump(LUMP_TEXDATA_STRING_DATA)
        d = self.lump(LUMP_TEXDATA_STRING_TABLE)
        str_offsets = struct.unpack_from(f"<{len(d) // 4}i", d, 0)
        d = self.lump(LUMP_TEXDATA)
        self.texdata = []
        for i in range(len(d) // 32):
            rx, ry, rz, name_id, w, h, _vw, _vh = struct.unpack_from("<3f5i", d, i * 32)
            td = TexData()
            td.reflectivity = (rx, ry, rz)
            o = str_offsets[name_id]
            td.name = strdata[o : strdata.index(b"\0", o)].decode("latin-1")
            td.width, td.height = w, h
            self.texdata.append(td)

        d = self.lump(LUMP_MODELS)
        self.models = []
        for i in range(len(d) // 48):
            v = struct.unpack_from("<9f3i", d, i * 48)
            m = Model()
            m.mins, m.maxs, m.origin = v[0:3], v[3:6], v[6:9]
            m.headnode, m.firstface, m.numfaces = v[9], v[10], v[11]
            self.models.append(m)

        d = self.lump(LUMP_NODES)
        self.nodes = []
        for i in range(len(d) // 32):
            v = struct.unpack_from("<3i6h2H", d, i * 32)
            n = Node()
            n.planenum, n.children = v[0], (v[1], v[2])
            n.mins, n.maxs = v[3:6], v[6:9]
            n.firstface, n.numfaces = v[9], v[10]
            self.nodes.append(n)

        self.leafs = self._parse_leafs(self.lump(LUMP_LEAFS), self.lump_version(LUMP_LEAFS))

        d = self.lump(LUMP_LEAFFACES)
        self.leaffaces = list(struct.unpack_from(f"<{len(d) // 2}H", d, 0))
        d = self.lump(LUMP_LEAFBRUSHES)
        self.leafbrushes = list(struct.unpack_from(f"<{len(d) // 2}H", d, 0))

        d = self.lump(LUMP_BRUSHES)
        self.brushes = []
        for i in range(len(d) // 12):
            b = Brush()
            b.firstside, b.numsides, b.contents = struct.unpack_from("<3i", d, i * 12)
            self.brushes.append(b)

        d = self.lump(LUMP_BRUSHSIDES)
        self.brushsides = []
        for i in range(len(d) // 8):
            s = BrushSide()
            s.planenum, s.texinfo, s.dispinfo, s.bevel = struct.unpack_from("<Hhhh", d, i * 8)
            self.brushsides.append(s)

        d = self.lump(LUMP_DISPINFO)
        self.dispinfo = []
        for i in range(len(d) // 176):
            o = i * 176
            di = DispInfo()
            di.start_pos = struct.unpack_from("<3f", d, o)
            di.vert_start, di.tri_start, di.power = struct.unpack_from("<3i", d, o + 12)
            di.contents = struct.unpack_from("<i", d, o + 32)[0]
            di.map_face = struct.unpack_from("<H", d, o + 36)[0]
            self.dispinfo.append(di)

        d = self.lump(LUMP_DISP_VERTS)
        self.disp_verts = [struct.unpack_from("<5f", d, i * 20) for i in range(len(d) // 20)]

        self.lighting = self.lump(LUMP_LIGHTING)
        self.lighting_hdr = self.lump(LUMP_LIGHTING_HDR)
        self.visibility = self.lump(LUMP_VISIBILITY)

    def leaf_ambient(self):
        """Per-leaf average ambient light (linear RGB), or None where the map has no samples.

        It averages all 6 faces of every ambient cube sample in the leaf (LDR lumps, then HDR as a fallback).
        """
        for idx_lump, light_lump in ((LUMP_LEAF_AMBIENT_INDEX, LUMP_LEAF_AMBIENT_LIGHTING),
                                     (LUMP_LEAF_AMBIENT_INDEX_HDR, LUMP_LEAF_AMBIENT_LIGHTING_HDR)):
            idx = self.lump(idx_lump)
            light = self.lump(light_lump)
            if len(idx) >= 4 * len(self.leafs) and light:
                break
        else:
            return [None] * len(self.leafs)
        out = []
        for i in range(len(self.leafs)):
            count, first = struct.unpack_from("<HH", idx, i * 4)
            if count == 0:
                out.append(None)
                continue
            acc = [0.0, 0.0, 0.0]
            n = 0
            for s in range(first, first + count):
                o = s * 28
                if o + 24 > len(light):
                    break
                for face in range(6):
                    r, g, b, e = struct.unpack_from("<BBBb", light, o + face * 4)
                    # Unlike lightmaps, ambient cube samples carry no extra /255 (exponents run around -9).
                    sc = 2.0 ** e
                    acc[0] += r * sc
                    acc[1] += g * sc
                    acc[2] += b * sc
                    n += 1
            out.append(tuple(a / n for a in acc) if n else None)
        return out

    def _parse_faces(self, d):
        faces = []
        for i in range(len(d) // 56):
            v = struct.unpack_from("<HBBihhhh4Bif2i2i", d, i * 56)
            f = Face()
            f.planenum, f.side = v[0], v[1]
            f.firstedge, f.numedges, f.texinfo, f.dispinfo = v[3], v[4], v[5], v[6]
            f.styles = v[8:12]
            f.lightofs = v[12]
            f.lm_mins = (v[14], v[15])
            f.lm_size = (v[16], v[17])
            f.orig_face = struct.unpack_from("<i", d, i * 56 + 44)[0]
            faces.append(f)
        return faces

    def _parse_leafs(self, d, version):
        size = 56 if version == 0 else 32
        leafs = []
        for i in range(len(d) // size):
            o = i * size
            contents, cluster, area_flags = struct.unpack_from("<ihh", d, o)
            mins = struct.unpack_from("<3h", d, o + 8)
            maxs = struct.unpack_from("<3h", d, o + 14)
            flf, nlf, flb, nlb = struct.unpack_from("<4H", d, o + 20)
            lf = Leaf()
            lf.contents, lf.cluster, lf.area = contents, cluster, area_flags & 0x1FF
            lf.mins, lf.maxs = mins, maxs
            lf.firstleafface, lf.numleaffaces = flf, nlf
            lf.firstleafbrush, lf.numleafbrushes = flb, nlb
            leafs.append(lf)
        return leafs

    # Helpers

    def face_vertices(self, face):
        """Polygon corners in winding order."""
        out = []
        for i in range(face.numedges):
            se = self.surfedges[face.firstedge + i]
            if se >= 0:
                out.append(self.vertexes[self.edges[se][0]])
            else:
                out.append(self.vertexes[self.edges[-se][1]])
        return out

    def pakfile(self):
        data = self.lump(LUMP_PAKFILE)
        if not data:
            return None
        return zipfile.ZipFile(io.BytesIO(data))

    def entities(self):
        return parse_entities(self.entities_text)

    def num_clusters(self):
        if len(self.visibility) < 4:
            return 0
        return struct.unpack_from("<i", self.visibility, 0)[0]

    def cluster_pvs(self, cluster):
        """Set of clusters potentially visible from `cluster`, decompressed from the vis lump."""
        n = self.num_clusters()
        if n == 0 or cluster < 0:
            return None
        ofs = struct.unpack_from("<i", self.visibility, 4 + cluster * 8)[0]
        vis = self.visibility
        out = set()
        c = 0
        while c < n:
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
        return out

    def game_lumps(self):
        d = self.lump(LUMP_GAME_LUMP)
        if not d:
            return {}
        count = struct.unpack_from("<i", d, 0)[0]
        out = {}
        for i in range(count):
            lid, flags, ver, ofs, length = struct.unpack_from("<iHHii", d, 4 + i * 16)
            name = struct.pack(">i", lid).decode("latin-1")
            out[name] = (ver, flags, self.raw[ofs : ofs + length])
        return out


def _lzma_decompress(data, expected_size):
    actual_size, lzma_size = struct.unpack_from("<II", data, 4)
    props = data[12:17]
    pb_lp_lc = props[0]
    lc = pb_lp_lc % 9
    lp = (pb_lp_lc // 9) % 5
    pb = pb_lp_lc // 45
    dict_size = struct.unpack_from("<I", props, 1)[0]
    filters = [{"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": lc, "lp": lp, "pb": pb}]
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters)
    out = dec.decompress(data[17 : 17 + lzma_size], max_length=actual_size)
    if len(out) != actual_size:
        raise ValueError(f"LZMA lump decoded to {len(out)} bytes, expected {actual_size}")
    return out


def parse_entities(text):
    """Entity lump to a list of dicts.

    Outputs ("OnPressed" etc.) can repeat, so every key starting with "On" is also collected in
    "_outputs" as (key, value) pairs.
    """
    ents = []
    cur = None
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "{":
            cur = {"_outputs": []}
            i += 1
        elif c == "}":
            if cur is not None:
                ents.append(cur)
            cur = None
            i += 1
        elif c == '"':
            j = text.index('"', i + 1)
            key = text[i + 1 : j]
            k = text.index('"', j + 1)
            l = text.index('"', k + 1)
            if cur is not None:
                val = text[k + 1 : l]
                cur[key] = val
                if key.startswith("On"):
                    cur["_outputs"].append((key, val))
            i = l + 1
        else:
            i += 1
    return ents


def parse_static_props(gl):
    """'sprp' game lump -> (model names, [instances]). Works for lump versions 4-11 by reading
    only the fields they share and stepping by the measured entry size."""
    if "sprp" not in gl:
        return [], []
    _ver, _flags, d = gl["sprp"]
    o = 0
    nd = struct.unpack_from("<i", d, o)[0]
    o += 4
    names = [d[o + i * 128 : o + (i + 1) * 128].split(b"\0", 1)[0].decode("latin-1").lower() for i in range(nd)]
    o += nd * 128
    nl = struct.unpack_from("<i", d, o)[0]
    o += 4 + nl * 2
    count = struct.unpack_from("<i", d, o)[0]
    o += 4
    if count <= 0:
        return names, []
    size = (len(d) - o) // count
    props = []
    for i in range(count):
        p = o + i * size
        ox, oy, oz, ap, ay, ar = struct.unpack_from("<6f", d, p)
        ptype, _first_leaf, _leaf_count = struct.unpack_from("<3H", d, p + 24)
        solid, flags = d[p + 30], d[p + 31]
        _skin, fade_min, fade_max = struct.unpack_from("<iff", d, p + 32)
        props.append({"origin": (ox, oy, oz), "angles": (ap, ay, ar), "model": ptype, "solid": solid,
                      "fade_max": fade_max})
    return names, props
