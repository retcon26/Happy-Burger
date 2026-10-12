"""Source studio model skeletons and animations (MDL v44-48, with .ani blocks and $includemodel files).

Layouts follow the Source SDK 2013 studio.h. Animations are decoded to per-bone (position, quaternion)
poses, then baked into skinning matrices: model-space bone transform x poseToBone, ready to apply to
bind-pose vertices.
"""

import math
import struct


def _cstr(data, ofs):
    end = data.index(b"\0", ofs)
    return data[ofs:end].decode("latin-1")


# --- Small quaternion / matrix helpers (3x4 matrices as row lists) -------------------------------

def quat_from_euler(x, y, z):
    """Source RadianEuler -> Quaternion (AngleQuaternion with roll=x, pitch=y, yaw=z)."""
    sr, cr = math.sin(x * 0.5), math.cos(x * 0.5)
    sp, cp = math.sin(y * 0.5), math.cos(y * 0.5)
    sy, cy = math.sin(z * 0.5), math.cos(z * 0.5)
    return (sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy, cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy)


def quat_matrix(q, p):
    x, y, z, w = q
    return [
        [1 - 2 * y * y - 2 * z * z, 2 * x * y - 2 * w * z, 2 * x * z + 2 * w * y, p[0]],
        [2 * x * y + 2 * w * z, 1 - 2 * x * x - 2 * z * z, 2 * y * z - 2 * w * x, p[1]],
        [2 * x * z - 2 * w * y, 2 * y * z + 2 * w * x, 1 - 2 * x * x - 2 * y * y, p[2]],
    ]


def mat_mul(a, b):
    out = []
    for r in range(3):
        row = [a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] for c in range(3)]
        row.append(a[r][0] * b[0][3] + a[r][1] * b[1][3] + a[r][2] * b[2][3] + a[r][3])
        out.append(row)
    return out


def nlerp(a, b, t):
    if a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0:
        b = tuple(-c for c in b)
    q = [a[k] + (b[k] - a[k]) * t for k in range(4)]
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


def _half(h):
    return struct.unpack("<e", struct.pack("<H", h))[0]


# --- Model files --------------------------------------------------------------------------------

class Bone:
    __slots__ = ("name", "parent", "pos", "quat", "rot", "posscale", "rotscale", "pose_to_bone")


class StudioFile:
    """One .mdl (plus its .ani if it has animation blocks)."""

    def __init__(self, fs, path):
        self.path = path
        self.d = fs.read(path)
        if not self.d or self.d[:4] != b"IDST":
            raise ValueError(f"{path}: not a studio model")
        d = self.d
        self.version = struct.unpack_from("<i", d, 4)[0]
        nb, bi = struct.unpack_from("<2i", d, 156)
        self.bones = []
        for i in range(nb):
            o = bi + i * 216
            b = Bone()
            b.name = _cstr(d, o + struct.unpack_from("<i", d, o)[0]).lower()
            b.parent = struct.unpack_from("<i", d, o + 4)[0]
            b.pos = struct.unpack_from("<3f", d, o + 32)
            b.quat = struct.unpack_from("<4f", d, o + 44)
            b.rot = struct.unpack_from("<3f", d, o + 60)
            b.posscale = struct.unpack_from("<3f", d, o + 72)
            b.rotscale = struct.unpack_from("<3f", d, o + 84)
            m = struct.unpack_from("<12f", d, o + 96)
            b.pose_to_bone = [list(m[0:4]), list(m[4:8]), list(m[8:12])]
            self.bones.append(b)
        self.num_anims, self.anim_index = struct.unpack_from("<2i", d, 180)
        self.num_seqs, self.seq_index = struct.unpack_from("<2i", d, 188)
        self.num_includes, self.include_index = struct.unpack_from("<2i", d, 336)
        name_idx, self.num_blocks, self.block_index = struct.unpack_from("<3i", d, 348)
        self.ani = None
        if self.num_blocks > 0 and name_idx:
            self.ani = fs.read(_cstr(d, name_idx)) or fs.read(path[:-4] + ".ani")

    def includes(self):
        out = []
        for i in range(self.num_includes):
            o = self.include_index + i * 8
            out.append(_cstr(self.d, o + struct.unpack_from("<i", self.d, o + 4)[0]).replace("\\", "/").lower())
        return out

    def sequences(self):
        """(label, activity name, first anim index, flags) per sequence."""
        out = []
        for i in range(self.num_seqs):
            o = self.seq_index + i * 212
            label = _cstr(self.d, o + struct.unpack_from("<i", self.d, o + 4)[0])
            act = _cstr(self.d, o + struct.unpack_from("<i", self.d, o + 8)[0]).upper()
            flags = struct.unpack_from("<i", self.d, o + 12)[0]
            anim_index_index = struct.unpack_from("<i", self.d, o + 60)[0]
            # Blended sequences (aim grids) list groupsize[0] x groupsize[1] animations; the corners are
            # extreme poses, so take the middle one (aiming straight ahead)
            g0, g1 = struct.unpack_from("<2i", self.d, o + 68)
            g0, g1 = max(1, g0), max(1, g1)
            mid = (g1 // 2) * g0 + g0 // 2
            anim = struct.unpack_from("<h", self.d, o + anim_index_index + 2 * mid)[0]
            out.append((label, act, anim, flags))
        return out


class AnimDesc:
    def __init__(self, f, index):
        self.f = f
        d = f.d
        self.o = o = f.anim_index + index * 100
        self.fps = struct.unpack_from("<f", d, o + 8)[0] or 30.0
        self.flags, self.num_frames, self.num_moves, self.move_index = struct.unpack_from("<4i", d, o + 12)
        self.block, self.index = struct.unpack_from("<2i", d, o + 52)
        self.section_index, self.section_frames = struct.unpack_from("<2i", d, o + 80)

    def speed(self):
        """Ground speed from the movement data (units/s), 0 if it doesn't move."""
        if self.num_moves <= 0 or self.num_frames <= 1:
            return 0.0
        mo = self.o + self.move_index + (self.num_moves - 1) * 44
        endframe = struct.unpack_from("<i", self.f.d, mo)[0]
        pos = struct.unpack_from("<3f", self.f.d, mo + 32)
        dist = math.sqrt(pos[0] ** 2 + pos[1] ** 2)
        dur = max(1, endframe) / self.fps
        return dist / dur

    def _stream(self, frame):
        """(bytes, offset of the mstudioanim_t stream, frame within that stream)."""
        block, index = self.block, self.index
        if self.section_frames:
            sec = frame // self.section_frames
            frame -= sec * self.section_frames
            so = self.o + self.section_index + sec * 8
            block, index = struct.unpack_from("<2i", self.f.d, so)
        if block == 0:
            return self.f.d, self.o + index, frame
        if not self.f.ani:
            return None, 0, frame
        start = struct.unpack_from("<i", self.f.d, self.f.block_index + block * 8)[0]
        return self.f.ani, start + index, frame

    def pose(self, frame):
        """{bone index in its file: (pos, quat)} for the bones this animation moves at an integer frame."""
        frame = max(0, min(self.num_frames - 1, frame))
        d, o, frame = self._stream(frame)
        out = {}
        if d is None:
            return out
        while True:
            bone, flags, nxt = struct.unpack_from("<BBh", d, o)
            if bone >= len(self.f.bones):
                break
            b = self.f.bones[bone]
            p = o + 4
            quat = b.quat
            pos = b.pos
            delta = flags & 0x10
            if flags & 0x02:  # RAWROT: Quaternion48
                xs, ys, zw = struct.unpack_from("<3H", d, p)
                x = (xs - 32768) / 32768.0
                y = (ys - 32768) / 32768.0
                z = ((zw & 0x7FFF) - 16384) / 16384.0
                w = math.sqrt(max(0.0, 1.0 - x * x - y * y - z * z))
                quat = (x, y, z, -w if zw & 0x8000 else w)
            if flags & 0x20:  # RAWROT2: Quaternion64
                v = struct.unpack_from("<Q", d, p + (6 if flags & 0x02 else 0))[0]
                x = ((v & 0x1FFFFF) - 1048576) / 1048576.5
                y = (((v >> 21) & 0x1FFFFF) - 1048576) / 1048576.5
                z = (((v >> 42) & 0x1FFFFF) - 1048576) / 1048576.5
                w = math.sqrt(max(0.0, 1.0 - x * x - y * y - z * z))
                quat = (x, y, z, -w if v >> 63 else w)
            if flags & 0x01:  # RAWPOS: Vector48
                po = p + (6 if flags & 0x02 else 0) + (8 if flags & 0x20 else 0)
                pos = tuple(_half(h) for h in struct.unpack_from("<3H", d, po))
            if flags & 0x08:  # ANIMROT
                ang = [self._value(d, p, k, frame) * b.rotscale[k] for k in range(3)]
                if not delta:
                    ang = [ang[k] + b.rot[k] for k in range(3)]
                quat = quat_from_euler(*ang)
            if flags & 0x04:  # ANIMPOS
                vp = p + (6 if flags & 0x08 else 0)
                pv = [self._value(d, vp, k, frame) * b.posscale[k] for k in range(3)]
                pos = tuple(pv[k] + (0.0 if delta else b.pos[k]) for k in range(3))
            out[bone] = (pos, quat)
            if nxt == 0:
                break
            o += nxt
        return out

    @staticmethod
    def _value(d, valueptr, axis, frame):
        """RLE-compressed animated value (mstudioanimvalue_t) for one axis at a frame."""
        ofs = struct.unpack_from("<h", d, valueptr + axis * 2)[0]
        if ofs == 0:
            return 0.0
        p = valueptr + ofs
        k = frame
        while True:
            valid, total = d[p], d[p + 1]
            if total == 0:
                return 0.0
            if total > k:
                break
            k -= total
            p += (valid + 1) * 2
        if valid > k:
            return float(struct.unpack_from("<h", d, p + (k + 1) * 2)[0])
        return float(struct.unpack_from("<h", d, p + valid * 2)[0])


# --- Skeleton + sequences across include files --------------------------------------------------

ACTIVITIES = {
    "idle": ["ACT_IDLE", "ACT_IDLE_RIFLE"],
    "walk": ["ACT_WALK", "ACT_WALK_RIFLE", "ACT_WALK_AIM_RIFLE", "ACT_WALK_EASY"],
    "run": ["ACT_RUN", "ACT_RUN_RIFLE", "ACT_RUN_AIM_RIFLE", "ACT_WALK", "ACT_WALK_RIFLE"],
    "attack": ["ACT_MELEE_ATTACK1", "ACT_RANGE_ATTACK1", "ACT_MELEE_ATTACK_SWING"],
    "death": ["ACT_DIESIMPLE", "ACT_DIEFORWARD", "ACT_DIEBACKWARD", "ACT_DIE_GUTSHOT"],
}


class Skeleton:
    def __init__(self, fs, mdl_path):
        self.base = StudioFile(fs, mdl_path)
        self.files = [self.base]
        for inc in self.base.includes():
            try:
                self.files.append(StudioFile(fs, inc))
            except (ValueError, struct.error):
                pass
        self.bones = self.base.bones
        # For each file, its bone index for each base bone (by name), or -1
        self.maps = []
        for f in self.files:
            names = {b.name: i for i, b in enumerate(f.bones)}
            self.maps.append([names.get(b.name, -1) for b in self.bones])

    def find(self, kind, prefer=()):
        """(file index, AnimDesc, looping) for the first sequence with one of the activity's names.
        prefer: activities to try first (an armed NPC's weapon-holding versions)."""
        for act in list(prefer) + ACTIVITIES[kind]:
            for fi, f in enumerate(self.files):
                for label, a, anim, flags in f.sequences():
                    if a == act and 0 <= anim < f.num_anims:
                        desc = AnimDesc(f, anim)
                        if desc.flags & 0x0004:  # delta animation: a gesture layer, not a full pose
                            continue
                        return fi, desc, bool(flags & 0x0001)
        return None

    def local_pose(self, fi, desc, frame_f):
        """Per base bone (pos, quat) at a fractional frame (blended between neighbours)."""
        f0 = int(math.floor(frame_f))
        t = frame_f - f0
        a = desc.pose(f0)
        b = desc.pose(min(f0 + 1, desc.num_frames - 1)) if t > 1e-3 else a
        fmap = self.maps[fi]
        out = []
        for bi, bone in enumerate(self.bones):
            fb = fmap[bi]
            if fb < 0 or (fb not in a):
                fbone = self.files[fi].bones[fb] if fb >= 0 else bone
                out.append((fbone.pos, fbone.quat))
                continue
            pa, qa = a[fb]
            pb, qb = b.get(fb, a[fb])
            out.append((tuple(pa[k] + (pb[k] - pa[k]) * t for k in range(3)), nlerp(qa, qb, t)))
        return out

    def skin_matrices(self, pose):
        """Model-space bone transform x poseToBone, per bone: maps bind-pose vertices to the pose."""
        world = []
        for bi, bone in enumerate(self.bones):
            pos, quat = pose[bi]
            local = quat_matrix(quat, pos)
            world.append(mat_mul(world[bone.parent], local) if bone.parent >= 0 else local)
        return [mat_mul(world[bi], self.bones[bi].pose_to_bone) for bi in range(len(self.bones))]

    def bind_matrices(self):
        return self.skin_matrices([(b.pos, b.quat) for b in self.bones])
