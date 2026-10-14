"""Animated first-person weapons (data/vmodels.pvmd) for psp/src/vmodel.c.

GMod's c_ weapon models carry the animations (draw, idle, fire, reload...), and the player's arms
(c_arms_citizen.mdl) are a separate model bone-merged onto the weapon's skeleton: each arm bone follows
the weapon bone of the same name. This bakes both into one rigidly skinned mesh (each vertex follows its
heaviest bone) with per-frame bone matrices, like the NPC pack, so the game does one matrix multiply per
vertex. Everything is in view space as Source draws it: origin at the eye, +X forward, +Y left, +Z up.

The matrices are 16-bit fixed point (rotation x 16384, position x 64): half the size of floats, which
matters on the 32MB machines.

Sections (same container as .pmap):
  "VMDL": {char name[24] (props.txt label, "_vm_pistol"); u32 first_mesh, num_meshes; u32 num_bones;
           u32 seq[VM_KINDS] (0xFFFFFFFF = none); float mins[3], maxs[3]}
  "MESH": {u32 first_vertex, num_vertices, first_index, num_indices; u16 tex, flags}
  "VERT": {float u, v, nx, ny, nz, x, y, z; u32 bone}
  "INDX": u16
  "SEQS": {u32 num_frames; float fps; u32 loop; u32 first_matrix}
  "MATQ": s16[12] per bone per frame (frame-major): rows of 3x4, rotation / 16384, position / 64
  "TEXS" / "TXDT": as in .pmap
"""

import struct

from . import pspimage
from .animation import AnimDesc, Skeleton, StudioFile, mat_mul, quat_matrix
from .materials import MaterialLoader
from .models import load_model
from .pmap import BATCH_ALPHATEST, BATCH_TRANSLUCENT, PmapWriter
from .props import VM_ARMS, VM_OPTS, read_prop_list
from .vpk import GameFS

PVMD_MAGIC = b"PVMD"
PVMD_VERSION = 1

# What vmodel.c plays, in its order (VM_IDLE...), and the activities that provide each. The crowbar's
# fire is a miss and its alt a hit.
KINDS = ["idle", "draw", "fire", "alt", "reload", "reload_start", "reload_end", "pump"]
ACTS = {
    "idle": ["ACT_VM_IDLE"],
    "draw": ["ACT_VM_DRAW"],
    "fire": ["ACT_VM_PRIMARYATTACK", "ACT_VM_MISSCENTER"],
    "alt": ["ACT_VM_HITCENTER", "ACT_VM_SECONDARYATTACK"],
    "reload": ["ACT_VM_RELOAD"],
    "reload_start": ["ACT_SHOTGUN_RELOAD_START"],
    "reload_end": ["ACT_SHOTGUN_RELOAD_FINISH"],
    "pump": ["ACT_SHOTGUN_PUMP"],
}
MAX_PALETTE = 96  # vmodel.c's limit

# Baking rates: the long idles (breathing) are slow, the actions need more frames
TIERS = {
    "high": {"tex": 128, "max_tris": 700, "idle_fps": 10.0, "idle_max": 40, "fps": 20.0, "max": 48},
    "low": {"tex": 64, "max_tris": 300, "idle_fps": 5.0, "idle_max": 20, "fps": 12.0, "max": 30},
}


def _find(sk, kind):
    for act in ACTS[kind]:
        for fi, f in enumerate(sk.files):
            for _label, a, anim, flags in f.sequences():
                if a == act and 0 <= anim < f.num_anims:
                    desc = AnimDesc(f, anim)
                    if desc.flags & 0x0004:  # delta: a gesture layer, not a pose
                        continue
                    return fi, desc, bool(flags & 1)
    return None


def _world(sk, pose):
    world = []
    for bi, bone in enumerate(sk.bones):
        pos, quat = pose[bi]
        local = quat_matrix(quat, pos)
        world.append(mat_mul(world[bone.parent], local) if bone.parent >= 0 else local)
    return world


def _q16(m):
    out = []
    for r in range(3):
        for c in range(3):
            out.append(max(-32768, min(32767, int(round(m[r][c] * 16384.0)))))
        out.append(max(-32768, min(32767, int(round(m[r][3] * 64.0)))))
    return out


def build_vmodels(prop_list, gmod_root, out_path, tier_name, log=print):
    tier = TIERS[tier_name]
    fs = GameFS(gmod_root)
    mats = MaterialLoader(fs)
    tex_index, tex_records = {}, []
    txdt = bytearray()
    vm_data, mesh_data, vert_data, idx_data, seq_data, mat_data = (bytearray() for _ in range(6))
    counts = {"vm": 0, "mesh": 0, "vert": 0, "idx": 0, "seq": 0, "mat": 0}

    def texture_for(path):
        nonlocal txdt
        if path in tex_index:
            return tex_index[path]
        mat = mats.get(path)
        keep_alpha = mat.alphatest or mat.translucent
        img = mats.load_texture(mat, tier["tex"])
        pt = pspimage.convert(img, tier["tex"], keep_alpha) if img else pspimage.solid((180, 180, 180))
        flags = (BATCH_ALPHATEST if mat.alphatest else 0) | (BATCH_TRANSLUCENT if mat.translucent else 0)
        clut = len(txdt)
        txdt += pt.clut
        lv = []
        for lvl in pt.levels:
            txdt += b"\0" * ((-len(txdt)) % 16)
            lv.append(len(txdt))
            txdt += lvl
        txdt += b"\0" * ((-len(txdt)) % 16)
        tex_index[path] = len(tex_records)
        tex_records.append((pt.width, pt.height, len(pt.levels), flags, clut, lv))
        return tex_index[path]

    arms_file = StudioFile(fs, VM_ARMS)
    for mdl, label, _shape, flags in prop_list:
        vm = VM_OPTS.get((mdl, label))
        if not vm or not vm.get("vm"):
            continue
        skin = (flags >> 24) & 0xFF
        try:
            sk = Skeleton(fs, mdl)
            weapon = load_model(fs, mdl, int(tier["max_tris"] * 0.7), skin)
            arms = load_model(fs, VM_ARMS, int(tier["max_tris"] * 0.4), 0)
        except (ValueError, struct.error, IndexError, KeyError) as e:
            log(f"  skip {label}: {e}")
            continue
        if weapon is None:
            log(f"  skip {label}: not found")
            continue

        # Palette entries: ("w", weapon bone) or ("a", arm bone, the weapon bone it follows)
        by_name = {b.name: i for i, b in enumerate(sk.bones)}
        follow = []
        for ab, bone in enumerate(arms_file.bones):
            b = ab
            while b >= 0 and arms_file.bones[b].name not in by_name:
                b = arms_file.bones[b].parent
            follow.append(by_name[arms_file.bones[b].name] if b >= 0 else -1)
        entries, index = [], {}

        def entry(key):
            if key not in index:
                index[key] = len(entries)
                entries.append(key)
            return index[key]

        meshes = []  # (material, [(vertex, palette index)], tris)
        for mesh in weapon.meshes:
            material = f"hue{vm['hue']}:{mesh.material}" if vm.get("hue") else mesh.material
            meshes.append((material, [(v, entry(("w", b))) for v, b in zip(mesh.verts, mesh.bones)], mesh.tris))
        if arms is not None:
            for mesh in arms.meshes:
                keep, verts = {}, []
                for i, (v, ab) in enumerate(zip(mesh.verts, mesh.bones)):
                    if ab < len(follow) and follow[ab] >= 0:
                        keep[i] = len(verts)
                        verts.append((v, entry(("a", ab, follow[ab]))))
                tris = [tuple(keep[i] for i in t) for t in mesh.tris if all(i in keep for i in t)]
                if tris:
                    meshes.append((mesh.material, verts, tris))
        if len(entries) > MAX_PALETTE:
            log(f"  skip {label}: {len(entries)} skinned bones (the game handles {MAX_PALETTE})")
            continue

        def palette(world):
            out = []
            for e in entries:
                if e[0] == "w":
                    out.append(mat_mul(world[e[1]], sk.bones[e[1]].pose_to_bone))
                else:
                    out.append(mat_mul(world[e[2]], arms_file.bones[e[1]].pose_to_bone))
            return out

        first_mesh = counts["mesh"]
        for material, verts, tris in meshes:
            tex = texture_for(material)
            mesh_data += struct.pack("<4I2H", counts["vert"], len(verts), counts["idx"], len(tris) * 3, tex,
                                     tex_records[tex][3])
            for v, p in verts:
                vert_data += struct.pack("<8fI", *v, p)
            for t in tris:
                idx_data += struct.pack("<3H", *t)
            counts["vert"] += len(verts)
            counts["idx"] += len(tris) * 3
            counts["mesh"] += 1

        seq_ids, baked, bounds_pal = [], [], None
        for kind in KINDS:
            found = _find(sk, kind)
            if not found:
                seq_ids.append(0xFFFFFFFF)
                continue
            fi, desc, loop = found
            if kind == "idle":
                loop = True
            rate, cap = (tier["idle_fps"], tier["idle_max"]) if kind == "idle" else (tier["fps"], tier["max"])
            duration = max(1, desc.num_frames - 1) / desc.fps
            nframes = max(1, min(cap, int(round(duration * rate)) + (0 if loop else 1)))
            fps = (nframes - (0 if loop else 1)) / duration if duration > 0 and nframes > 1 else rate
            first = counts["mat"]
            for i in range(nframes):
                frame_f = min(desc.num_frames - 1, i / fps * desc.fps) if fps > 0 else 0.0
                pal = palette(_world(sk, sk.local_pose(fi, desc, frame_f)))
                if kind == "idle" and i == 0:
                    bounds_pal = pal
                for m in pal:
                    mat_data += struct.pack("<12h", *_q16(m))
                counts["mat"] += len(pal)
            seq_data += struct.pack("<IfII", nframes, fps, 1 if loop else 0, first)
            seq_ids.append(counts["seq"])
            counts["seq"] += 1
            baked.append(f"{kind} {nframes}f")
        if seq_ids[0] == 0xFFFFFFFF:
            log(f"  {label}: no idle animation")

        # Bounds of the idle pose, for culling and the PS2's VU1 path
        lo, hi = [1e9] * 3, [-1e9] * 3
        for _mat, verts, _t in meshes:
            for v, p in verts:
                m = bounds_pal[p] if bounds_pal else [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]
                q = [m[r][0] * v[5] + m[r][1] * v[6] + m[r][2] * v[7] + m[r][3] for r in range(3)]
                lo = [min(lo[k], q[k]) for k in range(3)]
                hi = [max(hi[k], q[k]) for k in range(3)]
        vm_data += struct.pack(f"<24s3I{len(KINDS)}I6f", label.encode()[:23], first_mesh, len(meshes), len(entries),
                               *seq_ids, *lo, *hi)
        counts["vm"] += 1
        tris = sum(len(t) for _m, _v, t in meshes)
        log(f"  {label:14s} {tris:4d} tris, {len(entries):2d} bones: {', '.join(baked)}")

    w = PmapWriter(PVMD_MAGIC, PVMD_VERSION)
    w.add("VMDL", vm_data, counts["vm"])
    w.add("MESH", mesh_data, counts["mesh"])
    w.add("VERT", vert_data, counts["vert"])
    w.add("INDX", idx_data, counts["idx"])
    w.add("SEQS", seq_data, counts["seq"])
    w.add("MATQ", mat_data, counts["mat"])
    w.add("TEXS", bytes(32 * len(tex_records)), len(tex_records))
    w.add("TXDT", txdt, len(txdt))
    base = w.section_offset("TXDT")
    w.replace("TEXS", b"".join(struct.pack("<HHBBHI5I", tw, th, lv, fl, 0, base + co,
                                           *([base + o for o in lo_] + [0] * (5 - len(lo_))))
                               for tw, th, lv, fl, co, lo_ in tex_records))
    size = w.write(out_path)
    log(f"  wrote {out_path}: {counts['vm']} weapons, {size / 1024:.0f} KB")


def build_from_list(list_path, gmod_root, out_dir, tier_names, log=print):
    import os

    plist = read_prop_list(list_path)  # also fills VM_OPTS
    for t in tier_names:
        log(f"vmodels [{t}]")
        build_vmodels(plist, gmod_root, os.path.join(out_dir, f"vmodels{'' if t == 'high' else '.' + t}.pvmd"), t, log)
