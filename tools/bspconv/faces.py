"""Face Poser expressions and Finger Poser hands, baked for the NPC pack (npcbuild.py, psp/src/npc.c).

Faces: Source characters move their faces with flexes (vertex morphs). The model's flex controllers
("smile", "jaw_drop", TF2's "happybig"...) drive its flex rules, which give each flex a weight, and each
flex moves some of the face's vertices. Each expression below is a set of controller values; it's run
through the model's own rules and the vertex offsets it adds up to are kept, per vertex the game draws.

Hands: for each finger, the palette bones of its three segments and the axes it curls and spreads round,
in the bind pose's model space, worked out from where the knuckles are.

Layouts follow the Source SDK 2013 studio.h.
"""

import math
import struct

# The game's expression list (npc.c's k_faces): 0 is the model's own face
EXPRESSIONS = ["Neutral", "Smile", "Grin", "Angry", "Sad", "Surprised", "Scared", "Pain", "Shout", "Eyes Shut"]

# HL2's faces (FACS controllers; "right_"/"left_" versions of a name both get the value)
_HL2 = {
    "Smile": {"corner_puller": 0.8, "cheek_raiser": 0.45, "smile": 0.4, "lid_tightener": 0.2},
    "Grin": {"corner_puller": 1.0, "cheek_raiser": 0.8, "part": 0.8, "jaw_drop": 0.25, "upper_raiser": 0.4,
             "inner_raiser": 0.3, "outer_raiser": 0.3, "smile": 0.6},
    "Angry": {"lowerer": 1.0, "lid_tightener": 0.6, "upper_raiser": 0.6, "tightener": 0.5, "presser": 0.3,
              "chin_raiser": 0.4, "wrinkler": 0.6, "corner_depressor": 0.3},
    "Sad": {"inner_raiser": 1.0, "lowerer": 0.45, "corner_depressor": 0.9, "chin_raiser": 0.5, "lid_droop": 0.35},
    "Surprised": {"inner_raiser": 1.0, "outer_raiser": 1.0, "lid_raiser": 1.0, "jaw_drop": 0.5, "part": 0.6},
    "Scared": {"inner_raiser": 1.0, "outer_raiser": 0.5, "lowerer": 0.5, "lid_raiser": 1.0, "stretcher": 0.9,
               "part": 0.6, "jaw_drop": 0.2, "corner_depressor": 0.3},
    "Pain": {"lowerer": 1.0, "lid_closer": 0.6, "lid_tightener": 1.0, "cheek_raiser": 0.8, "upper_raiser": 0.8,
             "stretcher": 0.7, "wrinkler": 1.0, "part": 0.7, "jaw_clencher": 0.3},
    "Shout": {"jaw_drop": 0.9, "part": 1.0, "lowerer": 0.7, "upper_raiser": 0.6, "stretcher": 0.3,
              "lid_tightener": 0.4},
    "Eyes Shut": {"lid_closer": 1.0, "jaw_drop": 0.1},
}
# TF2's mercenaries have whole expressions as controllers, named differently from class to class: each
# expression lists every class's names (a model uses the ones it has)
_TF2 = {
    "Smile": {"happysmall": 1.0, "happysmallupper": 0.6, "happysmall02": 1.0, "happy1": 0.5, "smile01": 1.0,
              "happy": 0.7, "happyupper": 0.5, "evilsmile": 0.6},
    "Grin": {"happybig": 1.0, "happybigupper": 0.6},
    "Angry": {"mad": 1.0, "madupper": 0.8, "upperangry2": 0.8, "aggressive": 0.6},
    "Sad": {"upset1": 1.0, "uppersad1": 1.0, "painsmall": 0.7, "painsmallupper": 0.9},
    "Surprised": {"uppersuprise1": 1.0, "scaredupper": 1.0, "oh": 0.7, "openjaw": 0.4},
    "Scared": {"scared": 1.0, "scaredupper": 0.8, "uppersad3": 1.0, "upset2": 0.8, "painscared": 1.0},
    "Pain": {"painbig": 1.0, "painbigupper": 0.8, "pain": 1.0, "painupper": 0.8},
    "Shout": {"specialaction01": 1.0, "specialaction01upper": 0.6, "actionfire01": 1.0, "screamingupper": 0.8,
              "yellingupper": 0.8, "ah": 0.4},
    "Eyes Shut": {"right_closelid": 1.0, "left_closelid": 1.0, "dead01": 0.3, "dead": 0.3},
}

# Hand poses (npc.c's k_hands): per finger (thumb, index, middle, ring, pinky) how far it curls, 0 the bind
# pose's own hand and 1 a fist, and its spread in degrees. Pose 0 is the natural hand (nothing applied).
HANDS = ["Natural", "Fist", "Point", "Thumbs Up", "Peace", "Rock On", "Open", "OK"]
FINGER_COUNT = 5
SEGMENTS = 3
# Expressions a little stronger than Source's, so they read on the PSP's small screen and simple faces
EXAGGERATE = 1.5

_EPS = 1e-6


def _cstr(d, o):
    return d[o:d.index(b"\0", o)].decode("latin-1")


def _sub(a, b):
    return [a[k] - b[k] for k in range(3)]


def _add(a, b):
    return [a[k] + b[k] for k in range(3)]


def _scale(a, s):
    return [c * s for c in a]


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def _norm(a):
    ln = math.sqrt(_dot(a, a))
    return [c / ln for c in a] if ln > _EPS else [0.0, 0.0, 0.0]


def _remap_clamped(v, a, b, c, d):
    if abs(b - a) < _EPS:
        return d if v >= b else c
    t = min(1.0, max(0.0, (v - a) / (b - a)))
    return c + (d - c) * t


class FlexModel:
    """A model's flex controllers, descriptors and rules."""

    def __init__(self, mdl):
        self.d = mdl
        nfd, fdi, nfc, fci, nfr, fri = struct.unpack_from("<6i", mdl, 260)
        self.flags = struct.unpack_from("<i", mdl, 152)[0]
        self.fixed_scale = struct.unpack_from("<f", mdl, 392)[0]
        self.descs = [_cstr(mdl, fdi + i * 4 + struct.unpack_from("<i", mdl, fdi + i * 4)[0]) for i in range(nfd)]
        self.controllers = []  # (name lowercased, min, max)
        for i in range(nfc):
            o = fci + i * 20
            _t, n, _l2g, mn, mx = struct.unpack_from("<3i2f", mdl, o)
            self.controllers.append((_cstr(mdl, o + n).lower(), mn, mx))
        self.rules = []  # (flex desc, [(op, value)])
        for i in range(nfr):
            o = fri + i * 12
            flex, nops, opi = struct.unpack_from("<3i", mdl, o)
            ops = []
            for k in range(nops):
                oo = o + opi + k * 8
                op = struct.unpack_from("<i", mdl, oo)[0]
                val = struct.unpack_from("<f" if op == 1 else "<i", mdl, oo + 4)[0]
                ops.append((op, val))
            self.rules.append((flex, ops))
        # Eyelids: each eyeball's upper and lower lid flex takes the lid's height, which Source works out
        # from its raiser, neutral and lowerer flexes (mstudioeyeball_t), not from a flex rule
        self.lids = []  # (lid flex desc, [(flex desc, target)])
        num_bp, bp_ofs = struct.unpack_from("<2i", mdl, 232)
        for bp in range(num_bp):
            bpo = bp_ofs + bp * 16
            _name, num_models, _base, model_ofs = struct.unpack_from("<4i", mdl, bpo)
            for k in range(num_models):
                mo = bpo + model_ofs + k * 148
                num_eyes, eye_ofs = struct.unpack_from("<2i", mdl, mo + 100)
                for e in range(num_eyes):
                    eo = mo + eye_ofs + e * 172
                    upper = struct.unpack_from("<3i", mdl, eo + 68)
                    lower = struct.unpack_from("<3i", mdl, eo + 80)
                    ut = struct.unpack_from("<3f", mdl, eo + 92)
                    lt = struct.unpack_from("<3f", mdl, eo + 104)
                    ulid, llid = struct.unpack_from("<2i", mdl, eo + 116)
                    non_facs = mdl[eo + 140]
                    if non_facs:
                        continue
                    for lid, descs, targets in ((ulid, upper, ut), (llid, lower, lt)):
                        if 0 <= lid < nfd and all(0 <= d < nfd for d in descs):
                            self.lids.append((lid, list(zip(descs, targets))))

    def controller_values(self, wanted):
        """Controller values for an expression: each name's value, plus its right_/left_ versions."""
        src = [0.0] * len(self.controllers)
        hit = 0
        for i, (name, mn, mx) in enumerate(self.controllers):
            # right_/left_ halves both take a name's value (the Vortigaunt has one centre eye: center_)
            base = name
            for side in ("right_", "left_", "center_"):
                if name.startswith(side):
                    base = name[len(side):]
            v = wanted.get(name, wanted.get(base))
            if v is None:
                continue
            src[i] = min(mx, max(mn, v))
            hit += 1
        return src, hit

    def weights(self, src):
        """Runs the flex rules (CStudioHdr::RunFlexRules) on controller values."""
        dest = [0.0] * len(self.descs)
        ctl = self.controllers
        for flex, ops in self.rules:
            st = []
            for op, v in ops:
                try:
                    if op == 1:  # CONST
                        st.append(v)
                    elif op == 2:  # FETCH1: a controller
                        st.append(src[v] if 0 <= v < len(src) else 0.0)
                    elif op == 3:  # FETCH2: a flex already worked out
                        st.append(dest[v] if 0 <= v < len(dest) else 0.0)
                    elif op in (4, 5, 6, 7, 13, 14):
                        b = st.pop()
                        a = st.pop()
                        st.append(a + b if op == 4 else a - b if op == 5 else a * b if op == 6 else
                                  (a / b if b > 0.0001 else 0.0) if op == 7 else max(a, b) if op == 13 else min(a, b))
                    elif op == 8:  # NEG
                        st[-1] = -st[-1]
                    elif op == 15:  # 2WAY_0
                        st.append(_remap_clamped(src[v], -1.0, 0.0, 1.0, 0.0))
                    elif op == 16:  # 2WAY_1
                        st.append(_remap_clamped(src[v], 0.0, 1.0, 0.0, 1.0))
                    elif op == 17:  # NWAY
                        value = src[int(st[-1])]
                        x, y, z, w = st[-5], st[-4], st[-3], st[-2]
                        if value <= x or value >= w:
                            value = 0.0
                        elif value < y:
                            value = _remap_clamped(value, x, y, 0.0, 1.0)
                        elif value > z:
                            value = _remap_clamped(value, z, w, 1.0, 0.0)
                        else:
                            value = 1.0
                        del st[-5:]
                        st.append(value * src[v])
                    elif op == 18:  # COMBO: multiply the last v values
                        vals = st[-v:]
                        del st[-v:]
                        p = 1.0
                        for x in vals:
                            p *= x
                        st.append(p)
                    elif op == 19:  # DOMINATE
                        vals = st[-v:]
                        del st[-v:]
                        p = 1.0
                        for x in vals:
                            p *= x
                        st[-1] *= 1.0 - p
                    elif op in (20, 21):  # DME_LOWER_EYELID / DME_UPPER_EYELID
                        def ctl01(i, lo=0.0, hi=1.0):
                            if i < 0 or i >= len(ctl):
                                return 0.0
                            return _remap_clamped(src[i], ctl[i][1], ctl[i][2], lo, hi)
                        close_v = ctl01(v)
                        close = ctl01(int(st[-1]))
                        blink = ctl01(int(st[-2]))  # unused by Source either, but it's on the stack
                        updown = ctl01(int(st[-3]), -1.0, 1.0) if int(st[-3]) >= 0 else 0.0
                        del st[-3:]
                        if op == 20:
                            r = (1.0 - updown) * (1.0 - close_v) * close if updown > 0 else (1.0 - close_v) * close
                        else:
                            r = (1.0 + updown) * close_v * close if updown < 0 else close_v * close
                        st.append(r)
                        del blink
                    # OPEN, CLOSE, COMMA, EXP: not in compiled rules
                except (IndexError, ValueError):
                    st = [0.0]
                    break
            if 0 <= flex < len(dest) and st:
                dest[flex] = st[0]
        for lid, parts in self.lids:
            dest[lid] = sum(dest[d] * t for d, t in parts)
        return dest

    def mesh_deltas(self, mesh_ofs, weights):
        """{mesh vertex index: [dx, dy, dz]} for one mstudiomesh_t at these flex weights."""
        d = self.d
        numflexes, flexindex = struct.unpack_from("<2i", d, mesh_ofs + 16)
        fixed = bool(self.flags & 0x200000)  # STUDIOHDR_FLAGS_VERT_ANIM_FIXED_POINT_SCALE
        scale = self.fixed_scale if fixed and self.fixed_scale else 1.0 / 4096.0
        out = {}

        def ramp(t, w):
            t0, t1, t2, t3 = t
            if w <= t0 or w >= t3:
                return 0.0
            if w < t1:
                return (w - t0) / max(_EPS, t1 - t0)
            if w > t2:
                return (t3 - w) / max(_EPS, t3 - t2)
            return 1.0

        for i in range(numflexes):
            fo = mesh_ofs + flexindex + i * 60
            desc, t0, t1, t2, t3, numverts, vertindex, pair = struct.unpack_from("<i4f3i", d, fo)
            animtype = d[fo + 32]
            if not 0 <= desc < len(weights):
                continue
            t = (t0, t1, t2, t3)
            w1 = ramp(t, weights[desc])
            w3 = ramp(t, weights[pair]) if pair and 0 <= pair < len(weights) else w1
            if abs(w1) < 0.001 and abs(w3) < 0.001:
                continue
            size = 18 if animtype == 1 else 16
            for k in range(numverts):
                vo = fo + vertindex + k * size
                idx, _speed, side = struct.unpack_from("<HBB", d, vo)
                if fixed:
                    dl = [c * scale for c in struct.unpack_from("<3h", d, vo + 4)]
                else:
                    dl = struct.unpack_from("<3e", d, vo + 4)
                b = side / 255.0
                w = w1 * (1.0 - b) + w3 * b
                acc = out.setdefault(idx, [0.0, 0.0, 0.0])
                for c in range(3):
                    acc[c] += dl[c] * w
        return out


def bake_expressions(mdl, meshes, log=print, label=""):
    """Per expression (EXPRESSIONS[1:]), the sorted [(vertex index in this model's draw order, dx, dy, dz)]
    it moves, or [] if the model can't make it. meshes: models.ModelMesh in the order they're packed."""
    try:
        fm = FlexModel(mdl)
    except (struct.error, ValueError, IndexError):
        return [[] for _ in EXPRESSIONS[1:]]
    if not fm.controllers or not any(struct.unpack_from("<i", mdl, m.mdl_ofs + 16)[0] for m in meshes if m.mdl_ofs):
        return [[] for _ in EXPRESSIONS[1:]]
    names = {c[0] for c in fm.controllers}
    table = _TF2 if names & {"happybig", "painbig", "mad"} else _HL2
    # The game draws Neutral as the plain mesh, so each expression is kept as its change from the face
    # with every controller at rest (whose eyelids already sit off the plain mesh's)
    rest = fm.weights([0.0] * len(fm.controllers))
    rest_d = [fm.mesh_deltas(m.mdl_ofs, rest) if m.mdl_ofs is not None else {} for m in meshes]
    out = []
    made = []
    for expr in EXPRESSIONS[1:]:
        src, hit = fm.controller_values(table[expr])
        if not hit:
            out.append([])
            continue
        w = fm.weights(src)
        deltas = []
        base = 0
        for m, r in zip(meshes, rest_d):
            md = fm.mesh_deltas(m.mdl_ofs, w) if m.mdl_ofs is not None else {}
            for li, vid in enumerate(m.src):
                dv, rv = md.get(vid), r.get(vid)
                if not dv and not rv:
                    continue
                dv = [((dv[c] if dv else 0.0) - (rv[c] if rv else 0.0)) * EXAGGERATE for c in range(3)]
                if max(abs(c) for c in dv) > 0.01:
                    deltas.append((base + li, dv[0], dv[1], dv[2]))
            base += len(m.verts)
        out.append(deltas)
        if deltas:
            made.append(f"{expr} {len(deltas)}")
    if made and log:
        log(f"  {label}: faces {', '.join(made)}")
    return out




# --- Hands --------------------------------------------------------------------------------------
# The PSP's characters use Valve's low LODs, where the fingers are merged into the hand (or the hand into
# the forearm). For the Finger Poser the hands come from the coarsest LOD that still has finger bones,
# thinned out finger by finger. The finger bones aren't animated: on the PSP they follow their hand, and
# a posed ragdoll's fingers turn round their knuckles from there.

EXTRA_SLOTS = 2 * FINGER_COUNT * SEGMENTS  # palette slots after the animated bones: hand * 15 + finger * 3 + segment
HAND_TRIS = 110  # per hand, after thinning


def _pivot(sk, b):
    m2b = sk.bones[b].pose_to_bone
    return [-sum(m2b[r][i] * m2b[r][3] for r in range(3)) for i in range(3)]


def finger_rig(sk):
    """Per hand (left, right): (hand bone, [per finger, thumb first: its segments' bones, -1 if missing]),
    or None for a hand without fingers. None if neither hand has any (HL2's and TF2's skeletons)."""
    names = {b.name: i for i, b in enumerate(sk.bones)}
    rig = []
    for s in ("l", "r"):
        hl2 = [[f"valvebiped.bip01_{s}_finger{f}{k}" for k in ("", "1", "2")] for f in range(FINGER_COUNT)]
        tf2 = [[f"bip_{n}_{k}_{s}" for k in range(SEGMENTS)] for n in ("thumb", "index", "middle", "ring", "pinky")]
        chains = max((hl2, tf2), key=lambda c: sum(1 for f in c for n in f if n in names))
        hand = next((names[n] for n in (f"valvebiped.bip01_{s}_hand", f"bip_hand_{s}") if n in names), None)
        fingers = [[names.get(n, -1) for n in f] for f in chains]
        if hand is None or sum(1 for f in fingers if f[0] >= 0) < 3:
            rig.append(None)
        else:
            rig.append((hand, fingers))
    return rig if any(rig) else None


def finger_slots(rig):
    """{finger bone: its extra palette slot (0..EXTRA_SLOTS-1)}"""
    out = {}
    for h, hand in enumerate(rig or []):
        if hand:
            for f, segs in enumerate(hand[1]):
                for k, b in enumerate(segs):
                    if b >= 0:
                        out[b] = h * FINGER_COUNT * SEGMENTS + f * SEGMENTS + k
    return out


def _thin(verts, bones, src, tris, target):
    """Vertex clustering that never merges two bones' vertices, so the fingers stay apart."""
    if len(tris) <= target or not verts:
        return verts, bones, src, tris
    lo = [min(v[5 + k] for v in verts) for k in range(3)]
    hi = [max(v[5 + k] for v in verts) for k in range(3)]
    cell = max(hi[k] - lo[k] for k in range(3)) / 60.0
    best = (verts, bones, src, tris)
    for _ in range(30):
        keymap, nv, nb, ns, remap = {}, [], [], [], []
        for i, v in enumerate(verts):
            key = (int(v[5] // cell), int(v[6] // cell), int(v[7] // cell), int(v[0] * 8), int(v[1] * 8), bones[i])
            j = keymap.get(key)
            if j is None:
                j = keymap[key] = len(nv)
                nv.append(v)
                nb.append(bones[i])
                ns.append(src[i])
            remap.append(j)
        nt, seen = [], set()
        for a, b, c in tris:
            t = (remap[a], remap[b], remap[c])
            if len(set(t)) < 3 or tuple(sorted(t)) in seen:
                continue
            seen.add(tuple(sorted(t)))
            nt.append(t)
        best = (nv, nb, ns, nt)
        if len(nt) <= target:
            break
        cell *= 1.2
    return best


def graft_hands(fs, mdl_path, m, sk, rig, skin=0, bodygroups=None, log=print, label=""):
    """Swaps the hands of m (the LOD the game draws) for the fingered hands of a finer LOD. Returns the
    number of finger bones the new hands use (0: nothing done)."""
    from .models import ModelMesh, load_model

    sides = [h for h in range(2) if rig[h]]
    hand_set = {h: {rig[h][0]} | {b for f in rig[h][1] for b in f if b >= 0} for h in sides}
    finger_set = {h: hand_set[h] - {rig[h][0]} for h in sides}

    def hand_tris(mesh, h):
        return [t for t in mesh.tris if all(mesh.bones[v] in hand_set[h] for v in t)]

    def finger_bones(model, h):
        return {mesh.bones[v] for mesh in model.meshes for t in hand_tris(mesh, h) for v in t} & finger_set[h]

    if all(len(finger_bones(m, h)) >= 6 for h in sides):
        return sum(len(finger_bones(m, h)) for h in sides)  # its own hands have fingers already
    src_m = None
    for lod in range(m.lod - 1, -1, -1):
        fine = load_model(fs, mdl_path, 10 ** 6, skin=skin, bodygroups=bodygroups, force_lod=lod)
        if fine is None:
            continue
        if all(len(finger_bones(fine, h)) >= 6 for h in sides):
            src_m = fine
            break
    if src_m is None:
        return 0

    # Off with the old hands: triangles past each wrist (and close to it)
    wrists = []
    for h in sides:
        hb = rig[h][0]
        H = _pivot(sk, hb)
        par = sk.bones[hb].parent
        F = _pivot(sk, par) if par >= 0 else _sub(H, [0.0, 0.0, 1.0])
        wrists.append((H, _norm(_sub(H, F))))

    def past_wrist(p):
        for H, nrm in wrists:
            d = _sub(p, H)
            if _dot(d, nrm) > 1.0 and _dot(d, d) < 16.0 * 16.0:
                return True
        return False

    removed = 0
    for mesh in m.meshes:
        keep = []
        for t in mesh.tris:
            c = [sum(mesh.verts[v][5 + k] for v in t) / 3.0 for k in range(3)]
            if past_wrist(c):
                removed += 1
            else:
                keep.append(t)
        mesh.tris = keep

    # On with the new: each hand's triangles from the finer LOD, thinned, into the same mesh (material)
    by_key = {mesh.key: mesh for mesh in m.meshes}
    added = 0
    fingers_used = set()
    # A hand can span several meshes (skin and glove): they share its triangle budget
    total = {h: sum(len(hand_tris(fm, h)) for fm in src_m.meshes) for h in sides}
    for fmesh in src_m.meshes:
        for h in sides:
            tris = hand_tris(fmesh, h)
            if not tris:
                continue
            target = max(8, int(HAND_TRIS * len(tris) / max(1, total[h])))
            ids = sorted({v for t in tris for v in t})
            local = {v: i for i, v in enumerate(ids)}
            verts = [fmesh.verts[v] for v in ids]
            bones = [fmesh.bones[v] for v in ids]
            srcs = [fmesh.src[v] if v < len(fmesh.src) else -1 for v in ids]
            tl = [(local[a], local[b], local[c]) for a, b, c in tris]
            verts, bones, srcs, tl = _thin(verts, bones, srcs, tl, target)
            dst = by_key.get(fmesh.key)
            if dst is None:
                dst = ModelMesh(fmesh.material)
                dst.group, dst.mdl_ofs, dst.key = fmesh.group, fmesh.mdl_ofs, fmesh.key
                m.meshes.append(dst)
                by_key[fmesh.key] = dst
            base = len(dst.verts)
            dst.verts.extend(verts)
            dst.bones.extend(bones)
            dst.src.extend(srcs)
            dst.tris.extend((a + base, b + base, c + base) for a, b, c in tl)
            added += len(tl)
            fingers_used |= set(bones) & finger_set[h]

    # Drop the vertices nothing uses any more
    for mesh in m.meshes:
        used = sorted({v for t in mesh.tris for v in t})
        if len(used) == len(mesh.verts):
            continue
        remap = {v: i for i, v in enumerate(used)}
        mesh.verts = [mesh.verts[v] for v in used]
        mesh.bones = [mesh.bones[v] for v in used]
        mesh.src = [mesh.src[v] if v < len(mesh.src) else -1 for v in used]
        mesh.tris = [(remap[a], remap[b], remap[c]) for a, b, c in mesh.tris]
    m.meshes = [mesh for mesh in m.meshes if mesh.tris]
    m.tris = sum(len(mesh.tris) for mesh in m.meshes)
    if log:
        log(f"  {label}: hands from LOD {src_m.lod} (-{removed} +{added} tris, {len(fingers_used)} finger bones)")
    return len(fingers_used)


def hand_records(sk, rig, palette):
    """"NPCK" record: per hand (left, right) {i32 hand (palette bone, -1: can't be posed); per finger, thumb
    first, {float pivot[3][3] (each segment's knuckle, bind pose); float curl[3], spread[3] (axes);
    i32 segments}} - 648 bytes. curl turns a finger towards the palm, spread towards the thumb."""
    out = bytearray()
    for h in range(2):
        hand = rig[h] if rig else None
        if not hand or hand[0] not in palette:
            out += struct.pack("<i", -1) + bytes(FINGER_COUNT * 64)
            continue
        hb, fingers = hand
        H = _pivot(sk, hb)
        P = [[_pivot(sk, b) if b >= 0 else None for b in f] for f in fingers]
        base = [p[0] for p in P]
        idx_b = base[1] or base[2]
        pky_b = base[4] or base[3]
        n = _norm(_cross(_sub(idx_b, H), _sub(pky_b, H))) if idx_b and pky_b else [0.0, 0.0, 1.0]
        # Which side is the palm: the way the fingers bend at rest (towards it), else where the thumb sits
        bend = [0.0, 0.0, 0.0]
        for f in range(1, FINGER_COUNT):
            p0, p1, p2 = P[f]
            if p0 and p1 and p2:
                bend = _add(bend, _sub(_scale(_add(p0, p2), 0.5), p1))
        side = _dot(bend, n)
        if abs(side) < 0.02 and base[0]:
            side = _dot(_sub(base[0], H), n)
        if side < 0.0:
            n = _scale(n, -1.0)
        lateral = _norm(_sub(idx_b, pky_b)) if idx_b and pky_b else [1.0, 0.0, 0.0]
        out += struct.pack("<i", palette[hb])
        for f in range(FINGER_COUNT):
            p = P[f]
            nseg = 0
            while nseg < SEGMENTS and p[nseg] is not None:
                nseg += 1
            if nseg < 2:
                out += bytes(64)
                continue
            d = _norm(_sub(p[1], p[0]))
            if f == 0:
                # The thumb folds across the palm, towards the little finger
                mid = base[2] or base[1]
                curl = _norm(_cross(d, _norm(_add(n, _scale(_norm(_sub(mid, p[0])), 0.8)))))
            else:
                curl = _norm(_cross(d, n))
            spread = _norm(_cross(d, lateral))
            piv = [c for k in range(SEGMENTS) for c in (p[k] if k < nseg else p[nseg - 1])]
            out += struct.pack("<9f3f3fi", *piv, *curl, *spread, nseg)
    return bytes(out)
