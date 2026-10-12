"""Builds the prop pack (.ppak): the spawnable models, with textures and physics boxes.

Layout matches psp/src/ppak_format.h. It uses the same sectioned container as .pmap.
"""

import os
import struct

from . import pspimage
from .materials import MaterialLoader
from .models import load_model
from .viewmodels import load_arms_only, load_viewmodel
from .phy import hull_planes, read_convex_pieces, simplify
from .pmap import BATCH_ALPHATEST, BATCH_TRANSLUCENT, PmapWriter
from .vpk import GameFS

PPAK_MAGIC = b"PPAK"
PPAK_VERSION = 1

TIERS = {
    "high": {"tex": 128, "max_tris": 700},
    "low": {"tex": 64, "max_tris": 300},
}


SHAPES = {"box": 0, "sphere": 1, "cylinder": 2, "hull": 3}
SHAPE_AUTO = -1  # convex hulls from the model's .phy when it has one, else a box
MODEL_HIDDEN = 0x100
HULL_MAX_PIECES = 16
HULL_PIECE_POINTS = 20  # points per piece before building its planes
HULL_SAMPLE_POINTS = 32  # contact sample points per model

# Impact-sound class from the model's physics surfaceprop (psp/src/ppak_format.h SURF_*)
SURFACES = [("wood", 1), ("metal", 2), ("popcan", 2), ("plastic", 3), ("concrete", 4), ("rock", 4), ("brick", 4),
            ("tile", 4),
            ("glass", 5), ("cardboard", 6), ("paper", 6), ("flesh", 7), ("watermelon", 7), ("rubber", 8),
            ("tire", 8), ("computer", 3), ("pottery", 4), ("ceramic", 4)]


def read_prop_list(path):
    """Lines: model path | display name | options. Returns (path, label, shape, flags)."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split("|")]
            mdl = cols[0].lower()
            label = cols[1] if len(cols) > 1 and cols[1] else os.path.splitext(os.path.basename(mdl))[0]
            shape, flags, skin = SHAPE_AUTO, 0, 0
            for opt in (cols[2].split() if len(cols) > 2 else []):
                if opt.startswith("shape="):
                    shape = SHAPES.get(opt[6:], 0)
                elif opt.startswith("skin="):
                    skin = int(opt[5:])
                elif opt.startswith("cat="):
                    cat = opt[4:].replace("_", " ")
                    if cat not in CATEGORIES:
                        CATEGORIES.append(cat)
                    flags |= CATEGORIES.index(cat) << 16
                elif opt == "hidden":
                    flags |= MODEL_HIDDEN
                elif opt.startswith("break=") or opt.startswith("gib="):
                    # Breakables: break=GROUP shatters into the hidden gib=GROUP models (group index in flag bits 9-11)
                    group = opt.partition("=")[2]
                    if group not in BREAK_GROUPS:
                        BREAK_GROUPS.append(group)
                    k = BREAK_GROUPS.index(group) + 1
                    if k > 7:
                        raise ValueError(f"{path}: at most 7 break groups")
                    if opt.startswith("break="):
                        flags |= k << 9
                    else:
                        flags |= MODEL_HIDDEN
                        label = f"_gib{k}"
                elif opt in ("vm", "vmhands") or opt.startswith("hue="):
                    VM_OPTS.setdefault((mdl, label), {})[opt.split("=")[0]] = opt.partition("=")[2] or True
            # The skin rides in the flags' top byte, out of the way of PPAK_SHAPE_*, PPAK_HIDDEN and the category
            out.append((mdl, label, shape, flags | (skin << 24)))
    return out


# Spawn menu categories, in the order props.txt first mentions them (index stored in flag bits 16-23)
CATEGORIES = []
# Break groups (glass, melon...), in the order props.txt first mentions them; flag bits 9-11 hold index + 1
BREAK_GROUPS = []
# First-person models: (model, label) -> {"vm": weapon with arms, "vmhands": arms posed by this animation
# model, "hue": degrees to turn the colours (the physics gun is the gravity gun turned blue)}
VM_OPTS = {}
VM_ARMS = "models/weapons/c_arms_citizen.mdl"


def _surface_class(surfaceprop):
    s = (surfaceprop or "").lower()
    for key, cls in SURFACES:
        if key in s:
            return cls
    return 0


def build_pack(prop_list, gmod_root, out_path, tier_name, log=print, fs=None, max_tris=None, tex_size=None):
    """Writes a .ppak and returns {model path: index in the pack} for the models that made it in."""
    tier = dict(TIERS[tier_name])
    if max_tris:
        tier["max_tris"] = max_tris
    if tex_size:
        tier["tex"] = tex_size
    fs = fs or GameFS(gmod_root)
    mats = MaterialLoader(fs)
    index = {}

    tex_index = {}
    tex_records = []
    txdt = bytearray()
    model_data = bytearray()
    mesh_data = bytearray()
    vert_data = bytearray()
    idx_data = bytearray()
    nmodels = nmeshes = nverts = nidx = 0
    hull_data = bytearray()  # per model: first piece, piece count, first point, point count
    hull_pieces = []         # (first plane, plane count)
    hull_planes_out = []     # (nx, ny, nz, d), model space
    hull_points = []         # contact sample points, model space

    def texture_for(path):
        nonlocal txdt
        if path in tex_index:
            return tex_index[path]
        mat = mats.get(path)
        keep_alpha = mat.alphatest or mat.translucent
        img = mats.load_texture(mat, tier["tex"])
        pt = pspimage.convert(img, tier["tex"], keep_alpha) if img else pspimage.solid((180, 180, 180))
        flags = (BATCH_ALPHATEST if mat.alphatest else 0) | (BATCH_TRANSLUCENT if mat.translucent else 0)
        clut_ofs = len(txdt)
        txdt += pt.clut
        lv = []
        for lvl in pt.levels:
            txdt += b"\0" * ((-len(txdt)) % 16)
            lv.append(len(txdt))
            txdt += lvl
        txdt += b"\0" * ((-len(txdt)) % 16)
        tex_index[path] = len(tex_records)
        tex_records.append((pt.width, pt.height, len(pt.levels), flags, clut_ofs, lv))
        return tex_index[path]

    for mdl_path, label, shape, mflags in prop_list:
        skin = (mflags >> 24) & 0xFF
        mflags &= 0xFFFFFF
        vm = VM_OPTS.get((mdl_path, label))
        try:
            if vm and vm.get("vmhands"):
                m = load_arms_only(fs, mdl_path, VM_ARMS, int(tier["max_tris"] * 0.5))
            elif vm and vm.get("vm"):
                m = load_viewmodel(fs, mdl_path, VM_ARMS, int(tier["max_tris"] * 0.7), skin,
                                   arms_tris=int(tier["max_tris"] * 0.4))
            else:
                m = load_model(fs, mdl_path, tier["max_tris"], skin)
            if m is not None and vm and vm.get("hue"):
                for mesh in [x for x in m.meshes if not getattr(x, "is_arms", False)]:
                    mesh.material = f"hue{vm['hue']}:{mesh.material}"
        except (ValueError, struct.error, IndexError, KeyError) as e:
            log(f"  skip {mdl_path}: {e}")
            continue
        if m is None:
            log(f"  skip {mdl_path}: not found")
            continue
        size = [m.maxs[k] - m.mins[k] for k in range(3)]
        volume = max(1.0, size[0] * size[1] * size[2])
        mass = m.mass if m.mass else min(1000.0, max(2.0, volume * 0.0004))
        first_mesh = nmeshes
        for mesh in m.meshes:
            tex = texture_for(mesh.material)
            flags = tex_records[tex][3]
            mesh_data += struct.pack("<4I2H", nverts, len(mesh.verts), nidx, len(mesh.tris) * 3, tex, flags)
            for v in mesh.verts:
                vert_data += struct.pack("<8f", *v)
            for t in mesh.tris:
                idx_data += struct.pack("<3H", *t)
            nverts += len(mesh.verts)
            nidx += len(mesh.tris) * 3
            nmeshes += 1
        # Collision shape: the .phy's convex pieces unless props.txt asked for a primitive
        pieces = []
        if shape in (SHAPE_AUTO, SHAPES["hull"]) and not vm:
            pieces = read_convex_pieces(fs.read(mdl_path[:-4] + ".phy"))[:HULL_MAX_PIECES]
        first_piece, first_point = len(hull_pieces), len(hull_points)
        if pieces:
            shape = SHAPES["hull"]
            for pc in pieces:
                planes = hull_planes(simplify(pc, HULL_PIECE_POINTS))[:32]
                hull_pieces.append((len(hull_planes_out), len(planes)))
                hull_planes_out.extend(planes)
            hull_points.extend(simplify([p for pc in pieces for p in pc], HULL_SAMPLE_POINTS))
        elif shape < 0:
            shape = SHAPES["box"]
        hull_data.extend(struct.pack("<4I", first_piece, len(hull_pieces) - first_piece, first_point,
                                     len(hull_points) - first_point))

        name = label.encode("latin-1")[:31]
        surface = _surface_class(m.surfaceprop)
        model_data += struct.pack("<32s6ff5I", name, *m.mins, *m.maxs, mass, first_mesh, len(m.meshes),
                                  m.tris, shape | mflags, surface)
        index[mdl_path] = (nmodels, m.mins, m.maxs)
        nmodels += 1
        log(f"  {label:24s} {m.tris:4d} tris (lod {m.lod})  mass {mass:6.1f}  "
            f"size {size[0]:.0f}x{size[1]:.0f}x{size[2]:.0f}  {m.surfaceprop or '-'}"
            f"{f'  hull {len(pieces)} pieces' if pieces else ''}")

    w = PmapWriter(PPAK_MAGIC, PPAK_VERSION)
    w.add("MDLS", model_data, nmodels)
    w.add("MESH", mesh_data, nmeshes)
    w.add("VERT", vert_data, nverts)
    w.add("INDX", idx_data, nidx)
    w.add("TEXS", bytes(32 * len(tex_records)), len(tex_records))
    w.add("TXDT", txdt, len(txdt))
    w.add("CATS", b"".join(struct.pack("<24s", c.encode()[:23]) for c in CATEGORIES), len(CATEGORIES))
    w.add("HULL", hull_data, nmodels)
    w.add("HPCE", b"".join(struct.pack("<2I", *p) for p in hull_pieces), len(hull_pieces))
    w.add("HPLN", b"".join(struct.pack("<4f", *p) for p in hull_planes_out), len(hull_planes_out))
    w.add("HPNT", b"".join(struct.pack("<3f", *p) for p in hull_points), len(hull_points))
    base = w.section_offset("TXDT")
    tex_data = bytearray()
    for width, height, levels, flags, clut_ofs, level_ofs in tex_records:
        lv = [base + o for o in level_ofs] + [0] * (5 - len(level_ofs))
        tex_data += struct.pack("<HHBBHI5I", width, height, levels, flags, 0, base + clut_ofs, *lv)
    w.replace("TEXS", tex_data)
    size = w.write(out_path)
    log(f"  wrote {out_path}: {nmodels} props, {len(tex_records)} textures, {size / 1024:.0f} KB")
    return index
