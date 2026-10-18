"""Reader for Source studio models (.mdl + .vvd + .dx90.vtx) with prop physics info from .phy.

It extracts one static mesh per model (body part 0 / sub-model 0 / skin 0), picking the most
detailed LOD that fits a triangle budget, since the PSP can't afford full-detail props.
"""

import re
import struct

MDL_VERTEX_SIZE = 48
MDL_MODEL_SIZE = 148  # mstudiomodel_t: a bodypart's sub-models follow one another


def _cstr(data, ofs):
    end = data.index(b"\0", ofs)
    return data[ofs:end].decode("latin-1")


class ModelMesh:
    def __init__(self, material):
        self.material = material  # materials/ path without extension
        self.group = ""           # the bodypart it came from
        self.verts = []           # (u, v, nx, ny, nz, x, y, z)
        self.bones = []           # per vertex: the bone it follows (rigid skinning)
        self.tris = []
        self.src = []             # per vertex: its index in the MDL mesh (what face flexes refer to)
        self.mdl_ofs = None       # the mstudiomesh_t in the .mdl, for its flexes (faces.py)
        self.key = None           # (bodypart, mesh): the same mesh in another LOD has the same key


class StudioModel:
    def __init__(self, name):
        self.name = name
        self.meshes = []
        self.mins = None
        self.maxs = None
        self.mass = None
        self.surfaceprop = None
        self.lod = 0
        self.num_lods = 1
        self.tris = 0


def _vvd_vertices(vvd):
    if vvd[:4] != b"IDSV":
        raise ValueError("bad VVD")
    num_lods = struct.unpack_from("<i", vvd, 12)[0]
    lod_counts = struct.unpack_from("<8i", vvd, 16)
    num_fixups, fixup_start, vert_start = struct.unpack_from("<3i", vvd, 48)
    raw = []
    count = lod_counts[0]
    for i in range(count):
        o = vert_start + i * MDL_VERTEX_SIZE
        weights = struct.unpack_from("<3f", vvd, o)
        bones = vvd[o + 12 : o + 15]
        nbones = max(1, min(3, vvd[o + 15]))
        bone = bones[max(range(nbones), key=lambda k: weights[k])]  # rigid skinning: the strongest bone
        pos = struct.unpack_from("<3f", vvd, o + 16)
        nrm = struct.unpack_from("<3f", vvd, o + 28)
        uv = struct.unpack_from("<2f", vvd, o + 40)
        raw.append((uv, nrm, pos, bone))
    if num_fixups <= 0:
        return raw
    # The LOD 0 buffer is every fixup range in order. Lower LODs index into the same buffer.
    out = []
    for f in range(num_fixups):
        lod, src, n = struct.unpack_from("<3i", vvd, fixup_start + f * 12)
        if lod >= 0:
            out.extend(raw[src : src + n])
    return out


class _Vtx:
    """VTX layout walker. Strip-group header size differs between model versions (25 or 33 bytes)."""

    def __init__(self, data, sg_size):
        self.d = data
        self.sg_size = sg_size
        self.num_lods = struct.unpack_from("<i", data, 20)[0]
        self.num_bp, self.bp_ofs = struct.unpack_from("<ii", data, 28)

    def lod_meshes(self, bp, model, lod):
        """Yields, per mesh, a list of (orig_mesh_vert_id, ...) triangles."""
        d = self.d
        bpo = self.bp_ofs + bp * 8
        num_models, mofs = struct.unpack_from("<ii", d, bpo)
        mo = bpo + mofs + model * 8
        num_lods, lofs = struct.unpack_from("<ii", d, mo)
        lo = mo + lofs + lod * 12
        num_meshes, meofs = struct.unpack_from("<ii", d, lo)
        meshes = []
        for k in range(num_meshes):
            meo = lo + meofs + k * 9
            num_sg, sgofs = struct.unpack_from("<ii", d, meo)
            tris = []
            for g in range(num_sg):
                sgo = meo + sgofs + g * self.sg_size
                nverts, vofs, nidx, iofs, nstrips, sofs = struct.unpack_from("<6i", d, sgo)
                vids = [struct.unpack_from("<H", d, sgo + vofs + v * 9 + 4)[0] for v in range(nverts)]
                idx = struct.unpack_from(f"<{nidx}H", d, sgo + iofs)
                strip_size = 27 if self.sg_size == 25 else 35
                for s in range(nstrips):
                    so = sgo + sofs + s * strip_size
                    s_nidx, s_iofs = struct.unpack_from("<ii", d, so)
                    flags = d[so + 18]
                    sub = idx[s_iofs : s_iofs + s_nidx]
                    if flags & 2:  # triangle strip
                        for t in range(len(sub) - 2):
                            a, b, c = sub[t], sub[t + 1], sub[t + 2]
                            tri = (a, b, c) if t % 2 == 0 else (b, a, c)
                            tris.append(tuple(vids[i] for i in tri))
                    else:
                        for t in range(0, len(sub) - 2, 3):
                            tris.append((vids[sub[t]], vids[sub[t + 1]], vids[sub[t + 2]]))
            meshes.append(tris)
        return meshes


def _load_vtx(fs, base):
    for ext in (".dx90.vtx", ".dx80.vtx", ".sw.vtx", ".vtx"):
        d = fs.read(base + ext)
        if d:
            return d
    return None


def _phy_mass(phy):
    if not phy:
        return None
    masses = re.findall(rb'"mass"\s*"([0-9.]+)"', phy)
    if not masses:
        return None
    try:
        return sum(float(m) for m in masses)
    except ValueError:
        return None


def load_model(fs, mdl_path, max_tris, skin=0, decimate=True, bodygroups=None, force_lod=None):
    """bodygroups: {bodypart name prefix: sub-model index} to show other than each bodypart's first
    (e.g. {"headcrab": 1} puts the headcrab on a zombie). Meshes remember their bodypart in .group.
    force_lod: that LOD whatever its size (None if the model has no such LOD), never decimated."""
    mdl = fs.read(mdl_path)
    if mdl is None or mdl[:4] != b"IDST":
        return None
    base = mdl_path[:-4]
    vvd = fs.read(base + ".vvd")
    vtx_data = _load_vtx(fs, base)
    if not vvd or not vtx_data:
        return None
    version = struct.unpack_from("<i", mdl, 4)[0]

    num_tex, tex_ofs, num_cd, cd_ofs = struct.unpack_from("<4i", mdl, 204)
    num_skinref, num_skinfam, skin_ofs = struct.unpack_from("<3i", mdl, 220)
    num_bp, bp_ofs = struct.unpack_from("<2i", mdl, 232)
    textures = []
    for i in range(num_tex):
        o = tex_ofs + i * 64
        textures.append(_cstr(mdl, o + struct.unpack_from("<i", mdl, o)[0]))
    cdmats = [_cstr(mdl, struct.unpack_from("<i", mdl, cd_ofs + 4 * i)[0]) for i in range(num_cd)]
    family = skin if 0 <= skin < num_skinfam else 0
    skin0 = list(struct.unpack_from(f"<{num_skinref}h", mdl, skin_ofs + family * num_skinref * 2)) if num_skinref else []

    verts = _vvd_vertices(vvd)
    vtx = None
    for sg_size in ((25, 33) if version < 49 else (33, 25)):
        try:
            v = _Vtx(vtx_data, sg_size)
            v.lod_meshes(0, 0, 0)
            vtx = v
            break
        except (struct.error, IndexError):
            continue
    if vtx is None:
        return None

    m = StudioModel(mdl_path)
    phy = fs.read(base + ".phy")
    m.mass = _phy_mass(phy)
    sp = re.findall(rb'"surfaceprop"\s*"([^"]+)"', phy or b"")
    if sp:
        m.surfaceprop = sp[0].decode("latin-1")
    else:
        try:
            m.surfaceprop = _cstr(mdl, struct.unpack_from("<i", mdl, 308)[0])
        except (ValueError, struct.error):
            m.surfaceprop = None

    # Choose the most detailed LOD within the triangle budget, otherwise the one with fewest triangles
    # (not simply the last: some models' last LOD has more than the one before it)
    def submodel(bp):
        name_ofs = struct.unpack_from("<i", mdl, bp_ofs + bp * 16)[0]
        name = _cstr(mdl, bp_ofs + bp * 16 + name_ofs).lower()
        for prefix, k in (bodygroups or {}).items():
            if name.startswith(prefix):
                return name, k
        return name, 0

    def lod_tris(lod):
        total = 0
        for bp in range(num_bp):
            total += sum(len(t) for t in vtx.lod_meshes(bp, submodel(bp)[1], lod))
        return total

    lod_counts = []
    for lod in range(max(1, vtx.num_lods)):
        try:
            lod_counts.append(lod_tris(lod))
        except (struct.error, IndexError):
            break
    fitting = [lod for lod, n in enumerate(lod_counts) if n <= max_tris]
    if fitting:
        chosen = fitting[0]
    else:
        chosen = min(range(len(lod_counts)), key=lod_counts.__getitem__) if lod_counts else 0
    if force_lod is not None:
        if force_lod >= len(lod_counts):
            return None
        chosen = force_lod
        decimate = False
    m.lod = chosen
    m.num_lods = len(lod_counts)

    for bp in range(num_bp):
        bpo = bp_ofs + bp * 16
        _name, num_models, _base, model_ofs = struct.unpack_from("<4i", mdl, bpo)
        group, sub = submodel(bp)
        if num_models <= 0 or sub >= num_models:
            continue
        mo = bpo + model_ofs + sub * MDL_MODEL_SIZE
        num_meshes, mesh_ofs, _nv, vert_index = struct.unpack_from("<4i", mdl, mo + 72)
        num_eyes, eye_ofs = struct.unpack_from("<2i", mdl, mo + 100)
        model_vert_base = vert_index // MDL_VERTEX_SIZE
        try:
            vtx_meshes = vtx.lod_meshes(bp, sub, chosen)
        except (struct.error, IndexError):
            continue
        vtx_lod0 = None
        for k in range(min(num_meshes, len(vtx_meshes))):
            meo = mo + mesh_ofs + k * 116
            material, _midx, _nverts, vert_ofs = struct.unpack_from("<4i", mdl, meo)
            mat_type, mat_param = struct.unpack_from("<2i", mdl, meo + 24)
            if mat_type == 1 and chosen > 0:
                # Valve's low LODs drop the eyeballs, leaving empty sockets. They're tiny, so keep the full ones.
                if vtx_lod0 is None:
                    vtx_lod0 = vtx.lod_meshes(bp, sub, 0)
                if k < len(vtx_lod0):
                    vtx_meshes[k] = vtx_lod0[k]
            tex_idx = skin0[material] if material < len(skin0) else material
            tex_name = textures[tex_idx] if tex_idx < len(textures) else ""
            mesh = ModelMesh(_resolve_material(fs, cdmats, tex_name))
            mesh.group = group
            mesh.mdl_ofs = meo
            mesh.key = (bp, k)
            remap = {}
            for tri in vtx_meshes[k]:
                out = []
                for vid in tri:
                    gi = model_vert_base + vert_ofs + vid
                    if gi >= len(verts):
                        break
                    li = remap.get(gi)
                    if li is None:
                        uv, n, p, bone = verts[gi]
                        li = len(mesh.verts)
                        remap[gi] = li
                        mesh.verts.append((uv[0], uv[1], n[0], n[1], n[2], p[0], p[1], p[2]))
                        mesh.bones.append(bone)
                        mesh.src.append(vid)
                    out.append(li)
                if len(out) == 3:
                    mesh.tris.append(tuple(out))
            if mat_type == 1 and 0 <= mat_param < num_eyes and mesh.verts:
                _project_eyeball(mdl, mo + eye_ofs + mat_param * 172, mesh)
            if mesh.tris:
                m.meshes.append(mesh)
                m.tris += len(mesh.tris)

    # A mesh drawn twice (some ports put the whole body in two bodyparts) only costs triangles and z-fights
    seen = set()
    unique = []
    for mesh in m.meshes:
        sig = (mesh.material, tuple(mesh.tris), tuple(v[5:8] for v in mesh.verts))
        if sig not in seen:
            seen.add(sig)
            unique.append(mesh)
    m.meshes = unique
    m.tris = sum(len(mesh.tris) for mesh in m.meshes)
    if not m.meshes:
        return None
    pts = [v[5:8] for mesh in m.meshes for v in mesh.verts]
    m.mins = tuple(min(p[k] for p in pts) for k in range(3))
    m.maxs = tuple(max(p[k] for p in pts) for k in range(3))
    if m.tris > max_tris * 1.5 and decimate == "qem":
        _qem_decimate(m, max_tris)
    elif m.tris > max_tris * 1.5 and decimate:
        _decimate(m, max_tris)
    _fix_winding(m)
    return m


def _cluster(mesh, cell, uv_cells):
    """Vertex clustering: vertices in the same grid cell (and roughly the same UV) merge into one."""
    keymap = {}
    verts = []
    bones = []
    src = []
    remap = []
    for i, v in enumerate(mesh.verts):
        key = (int(v[5] // cell), int(v[6] // cell), int(v[7] // cell), int(v[0] * uv_cells), int(v[1] * uv_cells))
        idx = keymap.get(key)
        if idx is None:
            idx = len(verts)
            keymap[key] = idx
            verts.append(v)
            bones.append(mesh.bones[i] if i < len(mesh.bones) else 0)
            src.append(mesh.src[i] if i < len(mesh.src) else -1)
        remap.append(idx)
    tris = []
    seen = set()
    for a, b, c in mesh.tris:
        t = (remap[a], remap[b], remap[c])
        if t[0] == t[1] or t[1] == t[2] or t[0] == t[2]:
            continue
        key = tuple(sorted(t))
        if key in seen:
            continue
        seen.add(key)
        tris.append(t)
    return verts, bones, tris, src


def _decimate(m, target):
    """Shrinks a model without its own low LODs (e.g. the tool gun) by growing the cluster size until it fits."""
    size = max(m.maxs[k] - m.mins[k] for k in range(3))
    cell = size / 200.0
    for _ in range(24):
        results = [_cluster(mesh, cell, 4) for mesh in m.meshes]
        total = sum(len(r[2]) for r in results)
        if total <= target:
            break
        cell *= 1.25
    for mesh, (verts, bones, tris, src) in zip(m.meshes, results):
        mesh.verts, mesh.bones, mesh.tris, mesh.src = verts, bones, tris, src
    m.meshes = [mesh for mesh in m.meshes if mesh.tris]
    m.tris = sum(len(mesh.tris) for mesh in m.meshes)


def _qem_decimate(m, target):
    """Quadric edge collapse (Garland & Heckbert) across all of the model's meshes, for characters with no low
    LODs of their own (a 30,000-triangle Workshop player model down to the PSP's 900). Clustering tears such
    models apart at their UV seams; this keeps each part's shape. Vertices are welded by position; each
    triangle corner keeps its own UV, normal and bone, so only positions move (onto the edge's other end).
    Open edges and the borders between materials are held by extra planes, and a collapse that would turn a
    triangle over is refused."""
    import heapq

    import numpy as np

    pos_of = {}
    P = []
    corner_pos = []  # per mesh: its vertices' welded position ids
    for mesh in m.meshes:
        ids = []
        for v in mesh.verts:
            key = (round(v[5], 3), round(v[6], 3), round(v[7], 3))
            i = pos_of.get(key)
            if i is None:
                i = pos_of[key] = len(P)
                P.append(v[5:8])
            ids.append(i)
        corner_pos.append(ids)
    P = np.array(P, float)
    tris = []  # [pa, pb, pc, mesh, (va, vb, vc)]
    for mi, mesh in enumerate(m.meshes):
        ids = corner_pos[mi]
        for t in mesh.tris:
            p = [ids[t[0]], ids[t[1]], ids[t[2]]]
            if len(set(p)) == 3:
                tris.append([p[0], p[1], p[2], mi, t])
    if len(tris) <= target:
        return
    n = len(P)
    Q = np.zeros((n, 4, 4))
    vt = [set() for _ in range(n)]
    edge_tris = {}
    for ti, (a, b, c, mi, _t) in enumerate(tris):
        nrm = np.cross(P[b] - P[a], P[c] - P[a])
        area = np.linalg.norm(nrm)
        if area > 1e-12:
            pl = np.append(nrm / area, -np.dot(nrm / area, P[a]))
            Q[a] += np.outer(pl, pl) * area
            Q[b] += np.outer(pl, pl) * area
            Q[c] += np.outer(pl, pl) * area
        for x in (a, b, c):
            vt[x].add(ti)
        for e in ((a, b), (b, c), (c, a)):
            edge_tris.setdefault((min(e), max(e)), []).append(ti)
    # Open edges and material borders: a plane through the edge, upright to its triangle, weighted heavily
    for (a, b), ts in edge_tris.items():
        if len(ts) == 1 or len({tris[t][3] for t in ts}) > 1:
            t = tris[ts[0]]
            tn = np.cross(P[t[1]] - P[t[0]], P[t[2]] - P[t[0]])
            d = P[b] - P[a]
            side = np.cross(d, tn)
            ln = np.linalg.norm(side)
            if ln > 1e-12:
                side /= ln
                pl = np.append(side, -np.dot(side, P[a]))
                w = np.outer(pl, pl) * float(np.dot(d, d)) * 20.0
                Q[a] += w
                Q[b] += w

    def cost(u, v):  # u moves onto v
        h = np.append(P[v], 1.0)
        return float(h @ (Q[u] + Q[v]) @ h)

    heap = []
    for a, b in edge_tris:
        heapq.heappush(heap, (cost(a, b), a, b))
        heapq.heappush(heap, (cost(b, a), b, a))
    alive_v = [True] * n
    alive_t = [True] * len(tris)
    live = len(tris)

    def flips(u, v):
        for ti in vt[u]:
            t = tris[ti]
            if v in t[:3]:
                continue
            p = [P[x] for x in t[:3]]
            before = np.cross(p[1] - p[0], p[2] - p[0])
            p = [P[v] if x == u else P[x] for x in t[:3]]
            after = np.cross(p[1] - p[0], p[2] - p[0])
            lb, la = np.linalg.norm(before), np.linalg.norm(after)
            if la < 1e-9 or (lb > 1e-9 and np.dot(before, after) < 0.2 * lb * la):
                return True
        return False

    while live > target and heap:
        c, u, v = heapq.heappop(heap)
        if not (alive_v[u] and alive_v[v]) or not any(v in tris[ti][:3] for ti in vt[u]):
            continue
        if abs(c - cost(u, v)) > 1e-6 * max(1.0, abs(c)):
            continue  # stale: the quadrics changed since this was queued
        if flips(u, v):
            continue
        for ti in list(vt[u]):
            t = tris[ti]
            if v in t[:3]:
                alive_t[ti] = False
                live -= 1
                for x in t[:3]:
                    vt[x].discard(ti)
            else:
                for k in range(3):
                    if t[k] == u:
                        t[k] = v
                vt[v].add(ti)
        vt[u].clear()
        alive_v[u] = False
        Q[v] += Q[u]
        nbrs = {x for ti in vt[v] for x in tris[ti][:3]} - {v}
        for x in nbrs:
            heapq.heappush(heap, (cost(x, v), x, v))
            heapq.heappush(heap, (cost(v, x), v, x))

    # Back into meshes: a vertex per (corner's own vertex, where it now is)
    for mi, mesh in enumerate(m.meshes):
        out_of = {}
        verts, bones, src, out = [], [], [], []
        for ti, t in enumerate(tris):
            if not alive_t[ti] or t[3] != mi:
                continue
            tri = []
            for k in range(3):
                key = (t[4][k], t[k])
                idx = out_of.get(key)
                if idx is None:
                    idx = out_of[key] = len(verts)
                    ov = mesh.verts[t[4][k]]
                    verts.append(tuple(ov[:5]) + tuple(float(x) for x in P[t[k]]))
                    bones.append(mesh.bones[t[4][k]] if t[4][k] < len(mesh.bones) else 0)
                    src.append(mesh.src[t[4][k]] if t[4][k] < len(mesh.src) else -1)
                tri.append(idx)
            out.append(tuple(tri))
        mesh.verts, mesh.bones, mesh.tris, mesh.src = verts, bones, out, src
    m.meshes = [mesh for mesh in m.meshes if mesh.tris]
    m.tris = sum(len(mesh.tris) for mesh in m.meshes)


def _bone_to_model(mdl, bone):
    """The bone's bind pose as (rotation rows, origin) in model space, from its poseToBone matrix."""
    nb, bi = struct.unpack_from("<2i", mdl, 156)
    if not 0 <= bone < nb:
        return None
    m = struct.unpack_from("<12f", mdl, bi + bone * 216 + 96)
    rows = [m[0:4], m[4:8], m[8:12]]
    # poseToBone is orthonormal, so its inverse is the transpose
    rot = [[rows[r][i] for r in range(3)] for i in range(3)]
    org = [-sum(rows[r][i] * rows[r][3] for r in range(3)) for i in range(3)]
    return rot, org


def _project_eyeball(mdl, eo, mesh):
    """Eyes in Source get their iris by a planar projection along the eyeball's forward axis, not from
    the mesh's UVs. This bakes that projection into the UVs, against an "eye:" texture that has the iris
    in its centre half (see MaterialLoader), so the whole front of the eyeball stays inside the texture."""
    bone = struct.unpack_from("<i", mdl, eo + 4)[0]
    org = struct.unpack_from("<3f", mdl, eo + 8)
    up = struct.unpack_from("<3f", mdl, eo + 28)
    fwd = struct.unpack_from("<3f", mdl, eo + 40)
    iris_scale = struct.unpack_from("<f", mdl, eo + 60)[0]
    xf = _bone_to_model(mdl, bone)
    if xf is None or not 0.01 < iris_scale < 100:
        return
    rot, ofs = xf
    tr = lambda v: [sum(rot[i][k] * v[k] for k in range(3)) for i in range(3)]
    org = [a + b for a, b in zip(tr(org), ofs)]
    up, fwd = tr(up), tr(fwd)
    right = [fwd[1] * up[2] - fwd[2] * up[1], fwd[2] * up[0] - fwd[0] * up[2], fwd[0] * up[1] - fwd[1] * up[0]]
    ln = max(1e-6, sum(c * c for c in right) ** 0.5)
    right = [c / ln for c in right]
    up = [right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0]]
    # The sign of "forward" isn't reliable in the data; the front is the side away from the head bone
    if sum((org[k] - ofs[k]) * fwd[k] for k in range(3)) < 0:
        fwd = [-c for c in fwd]
    dots = [sum(v[2 + k] * fwd[k] for k in range(3)) for v in mesh.verts]
    s = iris_scale * 0.5  # the iris spans the centre half of the texture
    for i, v in enumerate(mesh.verts):
        d = [v[5 + k] - org[k] for k in range(3)]
        if dots[i] < -0.2:
            u, t = 0.02, 0.02  # back of the eyeball: plain white
        else:
            u = 0.5 - s * sum(d[k] * right[k] for k in range(3))
            t = 0.5 - s * sum(d[k] * up[k] for k in range(3))
            u, t = min(0.98, max(0.02, u)), min(0.98, max(0.02, t))
        mesh.verts[i] = (u, t) + tuple(v[2:])
    mesh.material = "eye:" + mesh.material


def _resolve_material(fs, cdmats, name):
    name = name.replace("\\", "/").lower()
    for cd in cdmats or [""]:
        # Some models store the folder with doubled separators ("models\props_c17\\"), so tidy them up
        cd = re.sub(r"/+", "/", cd.replace("\\", "/").lower()).lstrip("/")
        if cd and not cd.endswith("/"):
            cd += "/"
        path = f"{cd}{name}"
        if fs.read(f"materials/{path}.vmt") is not None:
            return path
    return name


def _fix_winding(m):
    """Makes triangles counter-clockwise seen from outside, to match the world's culling convention.

    Source models wind clockwise. The vertex normals decide, rather than assuming.
    """
    score = 0.0
    for mesh in m.meshes:
        for a, b, c in mesh.tris:
            pa, pb, pc = mesh.verts[a][5:], mesh.verts[b][5:], mesh.verts[c][5:]
            e1 = [pb[k] - pa[k] for k in range(3)]
            e2 = [pc[k] - pa[k] for k in range(3)]
            cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            n = [mesh.verts[a][2 + k] + mesh.verts[b][2 + k] + mesh.verts[c][2 + k] for k in range(3)]
            score += cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2]
    if score < 0:
        for mesh in m.meshes:
            mesh.tris = [(a, c, b) for a, b, c in mesh.tris]
