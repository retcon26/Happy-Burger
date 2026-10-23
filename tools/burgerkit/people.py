"""The characters: blocky low-poly people on one 11-bone skeleton, with idle, walk, run and attack
animations keyed here and baked into the engine's format, data/npcs.pnpc (psp/src/npc.c's structs).

Each vertex follows one bone (the parts are rigid), so a frame is just every bone's 3x4 skin matrix:
its posed transform times the inverse of its bind transform. The bind pose has no rotation: each bone's
bind transform is a move to its pivot.
"""

import math
import struct

from ..bspconv.pmap import PmapWriter
from .mesh import Mesh
from .props import pack_texs, texture_record

NPC_MAGIC = b"PNPC"
NPC_VERSION = 1
NO_SEQ = 0xFFFFFFFF
BAKE_FPS = 15

# name, parent, pivot (model space, feet at z = 0, facing +x)
BONES = [
    ("pelvis", -1, (0.0, 0.0, 37.0)),
    ("chest", 0, (0.0, 0.0, 40.0)),
    ("head", 1, (0.0, 0.0, 60.0)),
    ("l_arm", 1, (0.0, 11.4, 57.0)),
    ("l_fore", 3, (0.0, 11.4, 46.0)),
    ("r_arm", 1, (0.0, -11.4, 57.0)),
    ("r_fore", 5, (0.0, -11.4, 46.0)),
    ("l_thigh", 0, (0.0, 4.5, 36.0)),
    ("l_shin", 7, (0.0, 4.5, 19.0)),
    ("r_thigh", 0, (0.0, -4.5, 36.0)),
    ("r_shin", 9, (0.0, -4.5, 19.0)),
]
B = {name: i for i, (name, _p, _v) in enumerate(BONES)}


# --- Looks -----------------------------------------------------------------------------------------------

def body(a, look):
    """look: dict of atlas cells and extras -> the character's mesh."""
    m = Mesh()
    skin, shirt, pants, shoes = look["skin"], look["shirt"], look["pants"], look.get("shoes", "shoes")
    sleeves = look.get("sleeves", skin)  # forearms: bare unless it's a long-sleeved top
    belly = 2.5 if look.get("belly") else 0.0
    for side, sy in (("l", 1), ("r", -1)):
        y = 4.5 * sy
        m.box((-3.0, y - 2.7, 0.0), (5.5, y + 2.7, 3.5), a.uv(shoes), B[f"{side}_shin"])
        m.box((-2.6, y - 2.6, 3.5), (2.6, y + 2.6, 19.0), a.uv(pants), B[f"{side}_shin"])
        m.box((-3.0, y - 3.0, 19.0), (3.0, y + 3.0, 36.5), a.uv(pants), B[f"{side}_thigh"])
        ay = 11.4 * sy
        m.box((-2.4, ay - 2.4, 46.0), (2.4, ay + 2.4, 58.5), a.uv(shirt), B[f"{side}_arm"])
        m.box((-2.2, ay - 2.2, 36.0), (2.2, ay + 2.2, 46.0), a.uv(sleeves), B[f"{side}_fore"])
        m.box((-2.0, ay - 2.0, 31.5), (2.6, ay + 2.0, 36.0), a.uv(skin), B[f"{side}_fore"])
    m.box((-4.0, -8.0, 34.0), (4.0 + belly * 0.6, 8.0, 40.0), a.uv(pants), B["pelvis"])
    m.box((-5.0, -9.0, 40.0), (5.0 + belly, 9.0, 59.0), a.uv(shirt), B["chest"])
    m.box((-2.0, -2.0, 59.0), (2.0, 2.0, 61.0), a.uv(skin), B["chest"])
    m.box((-5.0, -5.0, 61.0), (5.0, 5.0, 72.0), {"front": a.uv(look["face"]), "side": a.uv(skin)}, B["head"])
    hair = look.get("hair")
    style = look.get("style", "short")
    if hair and style in ("short", "long"):
        m.box((-5.5, -5.5, 70.0), (5.5, 5.5, 73.5), a.uv(hair), B["head"])
        m.box((-5.5, -5.5, 63.0 if style == "short" else 56.0), (-4.2, 5.5, 70.0), a.uv(hair), B["head"])
        if style == "long":
            for sy in (1, -1):
                m.box((-5.5, 4.2 * sy - 1.3, 60.0), (2.0, 4.2 * sy + 1.3, 70.0), a.uv(hair), B["head"])
    if style == "mohawk":
        m.box((-5.0, -1.2, 72.0), (4.0, 1.2, 78.0), a.uv(hair), B["head"])
    if style == "cap":
        m.box((-5.5, -5.5, 70.0), (5.5, 5.5, 74.0), a.uv(look.get("cap", "cap_red")), B["head"])
        m.box((5.5, -4.5, 70.0), (10.0, 4.5, 71.0), a.uv(look.get("cap", "cap_red")), B["head"])
    if style == "chef":
        m.prism(8, 5.2, 5.6, 71.0, 72.5, a.uv("chef_hat"), bone=B["head"])
        m.prism(8, 5.6, 7.0, 72.5, 83.0, a.uv("chef_hat"), bone=B["head"])
    if look.get("glasses"):
        m.box((5.0, -4.6, 66.0), (5.6, 4.6, 67.6), a.uv("glasses"), B["head"])
    if look.get("tie"):
        m.box((5.0 + belly, -1.0, 46.0), (5.5 + belly, 1.0, 58.0), a.uv(look["tie"]), B["chest"])
    if look.get("apron"):
        m.box((5.0 + belly, -7.5, 41.0), (5.6 + belly, 7.5, 56.0), a.uv("apron"), B["chest"])
        m.box((4.0 + belly * 0.6, -7.5, 26.0), (4.6 + belly * 0.6, 7.5, 40.0), a.uv("apron"), B["pelvis"])
    if look.get("badge"):
        m.box((5.0 + belly, 4.0, 52.0), (5.5 + belly, 7.0, 55.0), a.uv("badge"), B["chest"])
    return m


# name, ai, voice, health, look
CAST = [
    ("Chef", "citizen", "male", 100, dict(skin="skin1", face="face_skin1_grin", shirt="shirt_white", pants="pants_black",
                                         sleeves="shirt_white", apron=True, style="chef")),
    ("Customer", "citizen", "male", 40, dict(skin="skin1", face="face_skin1", shirt="shirt_blue", pants="pants_jeans",
                                             hair="hair_brown")),
    ("Customer 2", "citizen", "male", 40, dict(skin="skin3", face="face_skin3", shirt="shirt_green",
                                               pants="pants_khaki", style="cap", cap="cap_red")),
    ("Customer 3", "citizen", "female", 40, dict(skin="skin2", face="face_skin2", shirt="shirt_pink",
                                                 pants="pants_jeans", hair="hair_blond", style="long")),
    ("Customer 4", "citizen", "female", 40, dict(skin="skin4", face="face_skin4", shirt="shirt_teal",
                                                 pants="pants_black", hair="hair_black", style="long")),
    ("Customer 5", "citizen", "male", 50, dict(skin="skin2", face="face_skin2_grump", shirt="shirt_orange",
                                               pants="pants_jeans", glasses=True, belly=True, style="bald")),
    ("Customer 6", "citizen", "female", 40, dict(skin="skin1", face="face_skin1", shirt="shirt_purple",
                                                 pants="pants_khaki", hair="hair_red", style="long")),
    ("Customer 7", "citizen", "male", 40, dict(skin="skin4", face="face_skin4_grin", shirt="shirt_yellow",
                                               pants="pants_black", hair="hair_black", shoes="shoes_white")),
    ("Grandpa", "citizen", "male", 30, dict(skin="skin1", face="face_skin1_grump", shirt="shirt_white",
                                            pants="pants_khaki", sleeves="shirt_white", hair="hair_grey",
                                            glasses=True)),
    ("Food Critic", "citizen", "male", 40, dict(skin="skin3", face="face_skin3_shades", shirt="suit",
                                                pants="pants_black", sleeves="suit", tie="tie", hair="hair_black")),
    ("Big Boss", "citizen", "male", 60, dict(skin="skin1", face="face_skin1_grump", shirt="suit", pants="pants_black",
                                             sleeves="suit", tie="cap_red", hair="hair_grey", belly=True,
                                             badge=True)),
    ("Mingebag", "minge", "male", 60, dict(skin="skin1", face="face_minge", shirt="jacket", pants="pants_jeans",
                                           sleeves="jacket", hair="hair_green", style="mohawk")),
]
VOICES = {"male": 1, "female": 2}  # psp/src/npc.c k_voices


# --- Animation -------------------------------------------------------------------------------------------

def _rx(d):
    r = math.radians(d)
    c, s = math.cos(r), math.sin(r)
    return [[1, 0, 0], [0, c, -s], [0, s, c]]


def _ry(d):
    r = math.radians(d)
    c, s = math.cos(r), math.sin(r)
    return [[c, 0, s], [0, 1, 0], [-s, 0, c]]


def _rz(d):
    r = math.radians(d)
    c, s = math.cos(r), math.sin(r)
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def _mm(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def _rot(pose):
    """pose: (swing, roll, yaw) degrees. Swing turns a limb hanging down forward (+x); roll lifts it sideways."""
    swing, roll, yaw = pose
    return _mm(_rz(yaw), _mm(_rx(roll), _ry(-swing)))


def skin_matrices(pose, root=(0.0, 0.0, 0.0)):
    """pose: {bone name: (swing, roll, yaw)}; root: pelvis offset. Returns 3x4 rows per bone."""
    world = []
    for i, (name, parent, pivot) in enumerate(BONES):
        r = _rot(pose.get(name, (0.0, 0.0, 0.0)))
        if parent < 0:
            t = (pivot[0] + root[0], pivot[1] + root[1], pivot[2] + root[2])
            world.append((r, t))
        else:
            pr, pt = world[parent]
            off = tuple(pivot[k] - BONES[parent][2][k] for k in range(3))
            t = tuple(pt[k] + sum(pr[k][j] * off[j] for j in range(3)) for k in range(3))
            world.append((_mm(pr, r), t))
    out = []
    for i, (r, t) in enumerate(world):
        p = BONES[i][2]
        # world x T(-pivot): the rotation stays, the translation becomes t - R p
        tt = tuple(t[k] - sum(r[k][j] * p[j] for j in range(3)) for k in range(3))
        out.append([r[0] + [tt[0]], r[1] + [tt[1]], r[2] + [tt[2]]])
    return out


def idle(p):
    s = math.sin(2 * math.pi * p)
    return {"chest": (1.5 * s, 0, 0), "head": (0, 0, 12 * math.sin(2 * math.pi * p + 1.0)),
            "l_arm": (2 * s, 6, 0), "r_arm": (2 * s, -6, 0), "l_fore": (8, 0, 0), "r_fore": (8, 0, 0)}, (0, 0, 0.3 * s)


def gait(p, legs, knees, arms, elbows, lean, bob):
    s = math.sin(2 * math.pi * p)
    c = math.cos(2 * math.pi * p)
    pose = {
        "l_thigh": (legs * s, 0, 0), "r_thigh": (-legs * s, 0, 0),
        "l_shin": (-knees * max(0.0, -c), 0, 0), "r_shin": (-knees * max(0.0, c), 0, 0),
        "l_arm": (-arms * s, 5, 0), "r_arm": (arms * s, -5, 0),
        "l_fore": (elbows, 0, 0), "r_fore": (elbows, 0, 0),
        "chest": (lean, 0, 6 * s), "head": (-lean * 0.6, 0, -4 * s),
    }
    return pose, (0, 0, bob * abs(c) - bob)


def walk(p):
    return gait(p, 24, 35, 20, 15, 3, 1.2)


def run(p):
    return gait(p, 42, 75, 42, 75, 12, 2.5)


def attack(p):
    # The right arm winds up over the head and swings down; the body twists into it
    if p < 0.4:
        k = p / 0.4
        arm, twist = 170 * k, -20 * k
    elif p < 0.7:
        k = (p - 0.4) / 0.3
        arm, twist = 170 - 200 * k, -20 + 40 * k
    else:
        k = (p - 0.7) / 0.3
        arm, twist = -30 + 30 * k, 20 - 20 * k
    return {"r_arm": (arm, -8, 0), "r_fore": (20, 0, 0), "chest": (5, 0, twist), "l_arm": (-10, 10, 0)}, (0, 0, 0)


# kind (npc.c SEQ_ order), keyer, seconds, speed (units/s), loops
SEQS = [("idle", idle, 2.4, 0.0, True), ("walk", walk, 1.0, 72.0, True), ("run", run, 0.6, 190.0, True),
        ("attack", attack, 0.7, 0.0, False)]


def build(atlas, out_path, log=print):
    txdt = bytearray()
    tex_records = [texture_record(txdt, atlas.img, 256, 3)]
    npc_data, mesh_data, vert_data, idx_data = bytearray(), bytearray(), bytearray(), bytearray()
    seq_data, mat_data, rag_data, flag_data = bytearray(), bytearray(), bytearray(), bytearray()
    nverts = nidx = nmats = nseqs = nrag = 0
    # The animations are the same for everyone: baked once, shared by every type's sequences
    seq_ids = []
    for kind, keyer, secs, speed, loop in SEQS:
        frames = max(2, int(round(secs * BAKE_FPS)) + (0 if loop else 1))
        first = nmats
        for f in range(frames):
            pose, root = keyer(f / frames if loop else f / (frames - 1))
            for rows in skin_matrices(pose, root):
                mat_data += struct.pack("<12f", *rows[0], *rows[1], *rows[2])
            nmats += len(BONES)
        seq_data += struct.pack("<IffII3I", frames, frames / secs, speed, 1 if loop else 0, first, 0, 0, 0)
        seq_ids.append(nseqs)
        nseqs += 1
    seq_ids.append(NO_SEQ)  # no death animation: they go limp as ragdolls
    for k, (name, ai, voice, health, look) in enumerate(CAST):
        m = body(atlas, look)
        mn, mx = m.bounds()
        mesh_data += struct.pack("<4I2H", nverts, len(m.verts), nidx, len(m.tris) * 3, 0, 0)
        for u, v, nx, ny, nz, x, y, z, bone in m.verts:
            vert_data += struct.pack("<8fI", u, v, nx, ny, nz, x, y, z, bone)
        for t in m.tris:
            idx_data += struct.pack("<3H", *t)
        nverts += len(m.verts)
        nidx += len(m.tris) * 3
        first_rag = nrag
        for _bn, parent, pivot in BONES:
            rag_data += struct.pack("<i3f", parent, *pivot)
            nrag += 1
        npc_data += struct.pack("<32s16sf6f2II5II", name.encode()[:31], ai.encode()[:15], float(health), *mn, *mx, k, 1,
                                len(BONES), *seq_ids, first_rag)
        flag_data += struct.pack("<I", 2 | (VOICES[voice] << 8))  # 2: a player model too
        log(f"  {name:12s} {len(m.tris):4d} tris")
    w = PmapWriter(NPC_MAGIC, NPC_VERSION)
    w.add("NPCM", npc_data, len(CAST))
    w.add("MESH", mesh_data, len(CAST))
    w.add("VERT", vert_data, nverts)
    w.add("INDX", idx_data, nidx)
    w.add("SEQS", seq_data, nseqs)
    w.add("MATS", mat_data, nmats)
    w.add("NPCB", rag_data, nrag)
    w.add("NPCF", flag_data, len(CAST))
    w.add("TEXS", bytes(32 * len(tex_records)), len(tex_records))
    w.add("TXDT", txdt, len(txdt))
    pack_texs(w, tex_records)
    size = w.write(out_path)
    log(f"  wrote {out_path}: {len(CAST)} characters, {nseqs} animations, {size / 1024:.0f} KB")
