"""Static props (the 'sprp' game lump) and physics props placed in the map.

Their models go into a per-map prop pack (data/maps/<map>.ppak). The map file lists the instances:
static props with a precomputed world matrix and per-cluster visibility, and physics props as
spawn records. Solid static props also get a box brush in the world collision.
"""

import math
import struct

from .bsp import parse_static_props
from .props import build_pack

PHYS_CLASSES = {"prop_physics", "prop_physics_multiplayer", "prop_physics_override", "prop_physics_respawnable"}
DYNAMIC_STATIC = {"prop_dynamic", "prop_dynamic_override", "prop_door_rotating"}


def angle_matrix(pitch, yaw, roll):
    """Source AngleMatrix: columns are the model's X (forward), Y (left), Z (up) axes in world space."""
    p, y, r = (math.radians(a) for a in (pitch, yaw, roll))
    sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(r), math.cos(r)
    x = (cp * cy, cp * sy, -sp)
    yv = (sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp)
    z = (cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp)
    return x, yv, z


def matrix_to_quat(x, y, z):
    m00, m10, m20 = x
    m01, m11, m21 = y
    m02, m12, m22 = z
    tr = m00 + m11 + m22
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        return ((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s)
    if m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2
        return (0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s)
    if m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2
        return ((m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s)
    s = math.sqrt(1.0 + m22 - m00 - m11) * 2
    return ((m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s)


def guess_shape(path):
    """Round things roll better as primitives; everything else uses its .phy hull (-1 = automatic)."""
    p = path.lower()
    if any(k in p for k in ("barrel", "drum", "can0", "popcan", "bottle", "bucket", "cone", "keg", "canister",
                            "pipe", "tire", "wheel")):
        return 2
    if any(k in p for k in ("melon", "ball", "skull", "globe")):
        return 1
    return -1


class MapProps:
    def __init__(self):
        self.statics = []   # (lib index, origin, (x, y, z) axes, solid, fade, path)
        self.physics = []   # (lib index, origin, quat)
        self.bounds = {}    # lib index -> (mins, maxs)
        self.lib_count = 0


def collect(bsp, fs, out_pack_path, tier, log=print, tex_size=None):
    """Builds the map's prop pack and returns MapProps, or None if the map uses no props.
    tex_size: largest texture side, if not the tier's usual one."""
    names, sprops = parse_static_props(bsp.game_lumps())
    phys_ents = [e for e in bsp.entities() if e.get("classname", "") in PHYS_CLASSES | DYNAMIC_STATIC]
    wanted = []
    seen = set()

    def want(path, shape):
        if path and path not in seen:
            seen.add(path)
            wanted.append((path, path.split("/")[-1][:31], shape, 0x100))  # hidden: not in the spawn menu

    for sp in sprops:
        if 0 <= sp["model"] < len(names):
            want(names[sp["model"]], 0)
    for e in phys_ents:
        want(e.get("model", "").lower(), guess_shape(e.get("model", "")))
    if not wanted:
        return None

    budget = {"high": 450, "low": 180}[tier]
    found = build_pack(wanted, None, out_pack_path, tier, log=lambda *_: None, fs=fs, max_tris=budget,
                       tex_size=tex_size or {"high": 64, "low": 32}[tier])
    index = {path: v[0] for path, v in found.items()}
    mp = MapProps()
    mp.lib_count = len(index)
    mp.bounds = {v[0]: (v[1], v[2]) for v in found.values()}
    for sp in sprops:
        if not 0 <= sp["model"] < len(names):
            continue
        li = index.get(names[sp["model"]])
        if li is None:
            continue
        axes = angle_matrix(*sp["angles"])
        mp.statics.append((li, sp["origin"], axes, sp["solid"], sp["fade_max"], names[sp["model"]]))
    for e in phys_ents:
        path = e.get("model", "").lower()
        li = index.get(path)
        if li is None:
            continue
        o = tuple(float(v) for v in e.get("origin", "0 0 0").split()[:3])
        a = [float(v) for v in e.get("angles", "0 0 0").split()[:3]] + [0.0, 0.0, 0.0]
        axes = angle_matrix(a[0], a[1], a[2])
        if e.get("classname") in PHYS_CLASSES:
            mp.physics.append((li, o, matrix_to_quat(*axes)))
        else:
            mp.statics.append((li, o, axes, 6, 0.0, path))
    log(f"  props: {len(index)} models, {len(mp.statics)} static, {len(mp.physics)} physics -> {out_pack_path}")
    return mp


def static_collision_box(lib_model_bounds, path):
    """Local box used for a solid static prop's collision, or None for foliage."""
    mins, maxs = lib_model_bounds
    p = path.lower()
    if any(k in p for k in ("foliage", "bush", "grass", "shrub", "plant", "ivy", "leaves", "fern")):
        return None
    if "tree" in p:  # just the trunk
        cx, cy = (mins[0] + maxs[0]) / 2, (mins[1] + maxs[1]) / 2
        return (cx - 10, cy - 10, mins[2]), (cx + 10, cy + 10, maxs[2])
    return mins, maxs


COLLISION_PIECES = 16
COLLISION_PIECE_POINTS = 20


def static_collision_pieces(fs, path, cache):
    """The model's .phy convex pieces as [(planes, points)] in model space, or [] without one.

    A door frame or an archway is several pieces round an opening: one box round the whole model
    would fill the doorway. Trees keep their trunk box (their .phy can include the canopy)."""
    if path in cache:
        return cache[path]
    from .phy import hull_planes, read_convex_pieces, simplify

    out = []
    if "tree" not in path.lower():
        for pc in read_convex_pieces(fs.read(path[:-4] + ".phy"))[:COLLISION_PIECES]:
            pts = simplify(pc, COLLISION_PIECE_POINTS)
            if len(pts) < 4:
                continue
            planes = hull_planes(pts)[:32]
            if len(planes) >= 4:
                out.append((planes, pts))
    cache[path] = out
    return out


def pack_statics(mp):
    """SPRP records: 3x4 matrix (axes as columns, then origin), lib model, flags, fade distance, pad."""
    data = bytearray()
    for li, o, (x, y, z), solid, fade, _path in mp.statics:
        data += struct.pack("<12f2If I", *x, *y, *z, *o, li, 1 if solid else 0, fade, 0)
    return data


def pack_physics(mp):
    return b"".join(struct.pack("<I3f4f", li, *o, *q) for li, o, q in mp.physics)
