"""First-person weapon models with hands, as static meshes.

GMod's c_ weapon models are animated, and the player's arms (c_arms_citizen.mdl) are a separate model
bone-merged onto the weapon's skeleton. Animating them every frame is beyond the PSP/PS2, so this poses
both at the first frame of the weapon's idle animation and bakes the result into one mesh. The result is
in view space as Source draws it: origin at the eye, +X forward, +Y left, +Z up.
"""

import math
import struct

from .animation import AnimDesc, Skeleton, StudioFile, mat_mul, quat_matrix
from .models import ModelMesh, StudioModel, load_model

IDLE_ACTS = ("ACT_VM_IDLE", "ACT_VM_IDLE_LOWERED", "ACT_VM_DRAW")


def _idle(sk):
    for act in IDLE_ACTS:
        for fi, f in enumerate(sk.files):
            for label, a, anim, _flags in f.sequences():
                if a == act and 0 <= anim < f.num_anims:
                    return fi, AnimDesc(f, anim)
    for fi, f in enumerate(sk.files):  # fall back to anything called idle
        for label, a, anim, _flags in f.sequences():
            if "idle" in label.lower() and 0 <= anim < f.num_anims:
                return fi, AnimDesc(f, anim)
    return None


def _world_matrices(sk, pose):
    world = []
    for bi, bone in enumerate(sk.bones):
        pos, quat = pose[bi]
        local = quat_matrix(quat, pos)
        world.append(mat_mul(world[bone.parent], local) if bone.parent >= 0 else local)
    return world


def _apply(mat, v):
    """Moves a vertex (u, v, nx, ny, nz, x, y, z) by a 3x4 matrix."""
    n = [sum(mat[r][k] * v[2 + k] for k in range(3)) for r in range(3)]
    ln = math.sqrt(sum(c * c for c in n)) or 1.0
    p = [sum(mat[r][k] * v[5 + k] for k in range(3)) + mat[r][3] for r in range(3)]
    return (v[0], v[1], n[0] / ln, n[1] / ln, n[2] / ln, p[0], p[1], p[2])


def load_viewmodel(fs, mdl_path, arms_path, max_tris, skin=0, arms_tris=None):
    """The weapon (and arms, if given) posed at its idle frame. None if the model can't be read."""
    weapon = load_model(fs, mdl_path, max_tris, skin)
    if weapon is None:
        return None
    sk = Skeleton(fs, mdl_path)
    found = _idle(sk)
    pose = sk.local_pose(found[0], found[1], 0.0) if found else [(b.pos, b.quat) for b in sk.bones]
    world = _world_matrices(sk, pose)
    skin_m = [mat_mul(world[b], sk.bones[b].pose_to_bone) for b in range(len(sk.bones))]

    out = StudioModel(mdl_path)
    out.lod, out.surfaceprop, out.mass = weapon.lod, weapon.surfaceprop, 1.0
    for mesh in weapon.meshes:
        nm = ModelMesh(mesh.material)
        nm.tris = mesh.tris
        nm.verts = [_apply(skin_m[b] if b < len(skin_m) else skin_m[0], v) for v, b in zip(mesh.verts, mesh.bones)]
        nm.bones = mesh.bones
        out.meshes.append(nm)

    if arms_path:
        arms = load_model(fs, arms_path, arms_tris or max_tris, 0)
        if arms is not None:
            af = StudioFile(fs, arms_path)
            by_name = {b.name: i for i, b in enumerate(sk.bones)}
            # Each arm bone follows the weapon bone of the same name (or its nearest named ancestor)
            amats = []
            for ab, bone in enumerate(af.bones):
                b = ab
                while b >= 0 and af.bones[b].name not in by_name:
                    b = af.bones[b].parent
                if b < 0:
                    amats.append(None)
                    continue
                amats.append(mat_mul(world[by_name[af.bones[b].name]], af.bones[ab].pose_to_bone))
            for mesh in arms.meshes:
                nm = ModelMesh(mesh.material)
                keep = {}
                for i, (v, b) in enumerate(zip(mesh.verts, mesh.bones)):
                    if b < len(amats) and amats[b] is not None:
                        keep[i] = len(nm.verts)
                        nm.verts.append(_apply(amats[b], v))
                        nm.bones.append(b)
                nm.is_arms = True
                nm.tris = [tuple(keep[i] for i in t) for t in mesh.tris if all(i in keep for i in t)]
                if nm.tris:
                    out.meshes.append(nm)

    out.tris = sum(len(m.tris) for m in out.meshes)
    pts = [v[5:8] for m in out.meshes for v in m.verts]
    out.mins = tuple(min(p[k] for p in pts) for k in range(3))
    out.maxs = tuple(max(p[k] for p in pts) for k in range(3))
    return out


def load_arms_only(fs, anim_path, arms_path, max_tris):
    """Just the arms, posed by an animation-only model (GMod's c_arms.mdl and its fists idle)."""
    sk = Skeleton(fs, anim_path)
    found = _idle(sk)
    if not found:
        return None
    return _pose_arms(fs, sk, found, arms_path, max_tris)


def _pose_arms(fs, sk, found, arms_path, max_tris):
    pose = sk.local_pose(found[0], found[1], 0.0)
    world = _world_matrices(sk, pose)
    arms = load_model(fs, arms_path, max_tris, 0)
    if arms is None:
        return None
    af = StudioFile(fs, arms_path)
    by_name = {b.name: i for i, b in enumerate(sk.bones)}
    out = StudioModel(arms_path)
    out.lod, out.surfaceprop, out.mass = arms.lod, "flesh", 1.0
    for mesh in arms.meshes:
        nm = ModelMesh(mesh.material)
        for v, ab in zip(mesh.verts, mesh.bones):
            b = ab
            while b >= 0 and af.bones[b].name not in by_name:
                b = af.bones[b].parent
            mat = mat_mul(world[by_name[af.bones[b].name]], af.bones[ab].pose_to_bone) if b >= 0 else None
            nm.verts.append(_apply(mat, v) if mat else v)
            nm.bones.append(ab)
        nm.tris = mesh.tris
        out.meshes.append(nm)
    out.tris = sum(len(m.tris) for m in out.meshes)
    pts = [v[5:8] for m in out.meshes for v in m.verts]
    out.mins = tuple(min(p[k] for p in pts) for k in range(3))
    out.maxs = tuple(max(p[k] for p in pts) for k in range(3))
    return out
