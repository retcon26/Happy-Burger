"""NPC pack (data/npcs.pnpc): skinned models with baked animations for psp/src/npc.c.

Each vertex follows one bone (rigid skinning). Each animation is baked at a low frame rate into
per-bone 3x4 matrices that take bind-pose vertices straight to the animated pose, so the PSP
only does one matrix multiply per vertex.

Sections (same container as .pmap):
  "NPCM": {char name[32]; char ai[16]; float health; float mins[3], maxs[3]; u32 first_mesh, num_meshes;
           u32 num_bones; u32 seq[5] (idle walk run attack death, 0xFFFFFFFF = none); u32 pad}
  "MESH": {u32 first_vertex, num_vertices, first_index, num_indices; u16 tex, flags}
  "VERT": {float u, v, nx, ny, nz, x, y, z; u32 bone}
  "INDX": u16
  "SEQS": {u32 num_frames; float fps; float speed; u32 loop; u32 first_matrix; u32 pad[3]}
  "MATS": float[12] per bone per frame (frame-major)
  "NPCB": {i32 parent; float pivot[3]} per palette bone, for ragdolls (parent is the nearest skinned ancestor,
          -1 for the root; pivot is the bone's bind-pose position in model space). NPCM's last u32 is the first.
  "NPCF": u32 flags per NPC: 1 ragdoll only (ai=ragdoll: no animations), 2 a player model,
          4 a headcrab jumps off when it dies (releasecrab), bits 8-15 its voice (index into VOICES)
  "NPCH": u64 per NPC: the palette bones of its headcrab (npcs.txt headcrabs: the headcrab bodygroups on)
  "NPCL": u64 per NPC: the palette bones below the waist (npcs.txt halves: a sawblade cuts it in two)
  NPCF bit 16: halves
  "TEXS" / "TXDT": as in .pmap
"""

import struct

from . import pspimage
from .animation import Skeleton
from .materials import MaterialLoader
from .models import load_model
from .pmap import BATCH_ALPHATEST, BATCH_TRANSLUCENT, PmapWriter
from .vpk import GameFS

PNPC_MAGIC = b"PNPC"
PNPC_VERSION = 1

# npcs.txt weapon=NAME: the world model it holds (also in the prop pack, hidden, under the same label)
WEAPONS = {
    "pistol": ("models/weapons/w_pistol.mdl", "_pistol"),
    "smg1": ("models/weapons/w_smg1.mdl", "_smg1"),
    "ar2": ("models/weapons/w_irifle.mdl", "_ar2"),
    "shotgun": ("models/weapons/w_shotgun.mdl", "_shotgun"),
    "357": ("models/weapons/w_357.mdl", "_357"),
    "stunstick": ("models/weapons/w_stunbaton.mdl", "_stunstick"),
    "crowbar": ("models/weapons/w_crowbar.mdl", "_crowbar"),
}
GRIP_BONES = ("valvebiped.weapon_bone", "valvebiped.anim_attachment_rh", "valvebiped.bip01_r_hand")

# Armed NPCs use the weapon-holding versions of their animations where the model has them
_RIFLE = {
    "idle": ["ACT_IDLE_SMG1", "ACT_IDLE_RIFLE", "ACT_IDLE_ANGRY_SMG1", "ACT_IDLE_ANGRY"],
    "walk": ["ACT_WALK_RIFLE", "ACT_WALK_AIM_RIFLE"],
    "run": ["ACT_RUN_RIFLE", "ACT_RUN_AIM_RIFLE"],
    "attack": ["ACT_RANGE_ATTACK_SMG1", "ACT_RANGE_ATTACK_AR2", "ACT_RANGE_ATTACK1"],
}
_PISTOL = {
    "idle": ["ACT_IDLE_PISTOL", "ACT_IDLE_ANGRY_PISTOL"],
    "walk": ["ACT_WALK_PISTOL", "ACT_WALK_AIM_PISTOL"],
    "run": ["ACT_RUN_PISTOL", "ACT_RUN_AIM_PISTOL"],
    "attack": ["ACT_RANGE_ATTACK_PISTOL", "ACT_RANGE_ATTACK1"],
}
_MELEE = {
    "idle": ["ACT_IDLE_ANGRY_MELEE", "ACT_IDLE_MELEE"],
    "attack": ["ACT_MELEE_ATTACK_SWING", "ACT_MELEE_ATTACK1"],
}
WEAPON_ACTS = {"smg1": _RIFLE, "ar2": _RIFLE, "shotgun": dict(_RIFLE, attack=["ACT_RANGE_ATTACK_SHOTGUN"] + _RIFLE["attack"]),
               "pistol": _PISTOL, "357": _PISTOL, "stunstick": _MELEE, "crowbar": _MELEE}


def _bone_to_model(bone):
    """A bone's bind pose (3x4, column vectors) from its poseToBone, which is orthonormal."""
    m = bone.pose_to_bone
    rot = [[m[r][c] for r in range(3)] for c in range(3)]  # transpose
    org = [-sum(m[r][c] * m[r][3] for r in range(3)) for c in range(3)]
    return [rot[0] + [org[0]], rot[1] + [org[1]], rot[2] + [org[2]]]


def _invert(m):
    rot = [[m[r][c] for r in range(3)] for c in range(3)]
    t = [-sum(rot[r][k] * m[k][3] for k in range(3)) for r in range(3)]
    return [rot[0] + [t[0]], rot[1] + [t[1]], rot[2] + [t[2]]]


def _mul(a, b):
    out = []
    for r in range(3):
        row = [sum(a[r][k] * b[k][c] for k in range(3)) for c in range(4)]
        row[3] += a[r][3]
        out.append(row)
    return out


def weapon_grip(fs, sk, palette, weapon):
    """(palette bone, 3x4 matrix taking the weapon's model space to the NPC's bind pose), or None.

    Source bone-merges a weapon: its bones follow the NPC's bones of the same name. HL2's world models
    carry ValveBiped.Bip01_R_Hand (and Weapon_bone under it), so the gun is lined up by whichever of
    those both skeletons have, the same bone on each side."""
    from .animation import StudioFile

    names = {b.name: i for i, b in enumerate(sk.bones)}
    try:
        wf = StudioFile(fs, WEAPONS[weapon][0])
    except (ValueError, KeyError):
        return None
    wnames = {b.name: i for i, b in enumerate(wf.bones)}
    shared = next((n for n in GRIP_BONES if n in names and n in wnames), None)
    if shared is not None:
        grip, wbind = names[shared], _bone_to_model(wf.bones[wnames[shared]])
    else:
        # Nothing in common: the old way, the NPC's grip bone against the weapon's own origin
        grip = next((names[n] for n in GRIP_BONES if n in names), None)
        if grip is None:
            return None
        wbind = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]
    attach = _mul(_bone_to_model(sk.bones[grip]), _invert(wbind))
    b = grip
    while b >= 0 and b not in palette:  # the weapon bone itself isn't skinned: follow the hand
        b = sk.bones[b].parent
    return (palette[b], attach) if b >= 0 else None
KINDS = ["idle", "walk", "run", "attack", "death"]
BAKE_FPS = 15.0
MAX_FRAMES = 48  # longer animations are baked at a lower rate to fit
MAX_PALETTE = 64  # skinned bones the game keeps per character (npc.c)
# npcs.txt voice=NAME: the sounds it makes (vo_NAME_pain/_die/_alert in sounds.txt), in the order npc.c knows them
VOICES = ["", "male", "female", "combine", "metro", "zombie", "fastzombie", "poison", "antlion", "headcrab", "guard",
          "vort", "alyx", "barney", "zombine"]

TIERS = {"high": {"tex": 128, "max_tris": 900}, "low": {"tex": 64, "max_tris": 450},
         # The PS2's: low detail, but with the ragdoll-only characters (they have no animations, so they're small)
         "ps2": {"tex": 64, "max_tris": 450, "ragdolls": True}}
# npcs.txt game=NAME: the game folder searched first for its model, materials and physics (mounted games'
# models can share a name with HL2's, e.g. Half-Life: Source's models/barney.mdl)
from .steam import game_dir
GAME_DIRS = {
    "css": ["Counter-Strike Source/cstrike"],
    "tf": ["Team Fortress 2/tf", "Team Fortress 2/hl2"],
    "hls": ["Half-Life 2/hl1", "Half-Life 1 Source Deathmatch/hl1", "Half-Life 1 Source Deathmatch/hl1mp"],
}


def read_npc_list(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split("|")]
            opts = {}
            for o in (cols[2].split() if len(cols) > 2 else []):
                k, _, v = o.partition("=")
                opts[k] = v
            out.append((cols[0].lower(), cols[1] if len(cols) > 1 else cols[0], opts))
    return out


def build_npcs(npc_list, gmod_root, out_path, tier_name, log=print):
    tier = TIERS[tier_name]
    fss = {"": GameFS(gmod_root)}
    for game, dirs in GAME_DIRS.items():
        if any(game == o.get("game") for _m, _l, o in npc_list):
            fss[game] = GameFS(gmod_root, [game_dir(d) for d in dirs])
    fs = fss[""]
    mats = MaterialLoader(fs)
    mats_for = {"": mats}
    tex_index, tex_records = {}, []
    txdt = bytearray()
    npc_data, mesh_data, vert_data, idx_data, seq_data, mat_data, rag_data, wpn_data, flag_data, crab_data, leg_data = (
        bytearray() for _ in range(11))
    counts = {"npc": 0, "mesh": 0, "vert": 0, "idx": 0, "seq": 0, "mat": 0, "rag": 0}

    def texture_for(path, game=""):
        nonlocal txdt
        key = (game, path)
        if key in tex_index:
            return tex_index[key]
        ml = mats_for.setdefault(game, MaterialLoader(fss[game]))
        mat = ml.get(path)
        img = ml.load_texture(mat, tier["tex"])
        keep_alpha = mat.alphatest or mat.translucent
        pt = pspimage.convert(img, tier["tex"], keep_alpha) if img else pspimage.solid((150, 150, 150))
        flags = (BATCH_ALPHATEST if mat.alphatest else 0) | (BATCH_TRANSLUCENT if mat.translucent else 0)
        clut = len(txdt)
        txdt += pt.clut
        lv = []
        for lvl in pt.levels:
            txdt += b"\0" * ((-len(txdt)) % 16)
            lv.append(len(txdt))
            txdt += lvl
        txdt += b"\0" * ((-len(txdt)) % 16)
        tex_index[key] = len(tex_records)
        tex_records.append((pt.width, pt.height, len(pt.levels), flags, clut, lv))
        return tex_index[key]

    for mdl, label, opts in npc_list:
        game = opts.get("game", "")
        if game not in fss:
            log(f"  skip {mdl}: unknown game={game}")
            continue
        fs = fss[game]
        try:
            sk = Skeleton(fs, mdl)
            # npcs.txt decimate: a model with no low LODs of its own gets simplified to the budget
            m = load_model(fs, mdl, tier["max_tris"], decimate="decimate" in opts,
                           bodygroups={"headcrab": 1} if "headcrabs" in opts else None)
        except (ValueError, struct.error, IndexError, KeyError) as e:
            log(f"  skip {mdl}: {e}")
            continue
        if m is None:
            log(f"  skip {mdl}: not found")
            continue
        # Only bones that vertices follow need matrices; renumber them compactly
        used = sorted({b for mesh in m.meshes for b in mesh.bones})
        if len(used) > MAX_PALETTE:
            log(f"  skip {mdl}: {len(used)} skinned bones (the game handles {MAX_PALETTE})")
            continue
        palette = {b: i for i, b in enumerate(used)}
        ragdoll_only = opts.get("ai") == "ragdoll"
        if ragdoll_only and not tier.get("ragdolls", tier_name == "high"):
            # The 32MB PSP has no room for them. They come after every NPC in npcs.txt, so the other
            # characters keep the same numbers in both packs (multiplayer sends them by number)
            log(f"  skip {label} (ragdoll only: full-detail pack only)")
            continue

        # Ragdoll skeleton: each skinned bone's nearest skinned ancestor and its bind-pose pivot
        first_rag = counts["rag"]
        for b in used:
            m2b = sk.bones[b].pose_to_bone  # inverse of the bone's bind transform
            pivot = [-sum(m2b[r][i] * m2b[r][3] for r in range(3)) for i in range(3)]
            par = sk.bones[b].parent
            while par >= 0 and par not in palette:
                par = sk.bones[par].parent
            rag_data += struct.pack("<i3f", palette[par] if par >= 0 else -1, *pivot)
            counts["rag"] += 1

        first_mesh = counts["mesh"]
        for mesh in m.meshes:
            tex = texture_for(mesh.material, game)
            mesh_data += struct.pack("<4I2H", counts["vert"], len(mesh.verts), counts["idx"], len(mesh.tris) * 3, tex,
                                     tex_records[tex][3])
            for v, bone in zip(mesh.verts, mesh.bones):
                vert_data += struct.pack("<8fI", *v, palette[bone])
            for t in mesh.tris:
                idx_data += struct.pack("<3H", *t)
            counts["vert"] += len(mesh.verts)
            counts["idx"] += len(mesh.tris) * 3
            counts["mesh"] += 1

        seq_ids = []
        baked = []
        acts = WEAPON_ACTS.get(opts.get("weapon", ""), {})
        for kind in KINDS:
            # Ragdoll-only characters are only ever posed by the ragdoll (from the bind pose)
            found = None if ragdoll_only else sk.find(kind, acts.get(kind, ()))
            if not found:
                seq_ids.append(0xFFFFFFFF)
                continue
            fi, desc, loop = found
            # No walk of its own (TF2's mercenaries only run): walk is the run played at half speed,
            # or they wander off at a sprint
            rate = 1.0
            if kind == "walk":
                run = sk.find("run", acts.get("run", ()))
                if run and run[0] == fi and run[1].o == desc.o:
                    rate = 0.5
            duration = max(1, desc.num_frames - 1) / desc.fps
            nframes = max(1, min(MAX_FRAMES, int(round(duration * BAKE_FPS)) + (0 if loop else 1)))
            fps = nframes / duration if duration > 0 else BAKE_FPS
            first = counts["mat"]
            for i in range(nframes):
                frame_f = min(desc.num_frames - 1, i / fps * desc.fps)
                skin = sk.skin_matrices(sk.local_pose(fi, desc, frame_f))
                for b in used:
                    mat_data += struct.pack("<12f", *skin[b][0], *skin[b][1], *skin[b][2])
                counts["mat"] += len(used)
            speed = desc.speed() * rate
            seq_data += struct.pack("<If f I I 3I", nframes, fps * rate, speed, 1 if loop else 0, first, 0, 0, 0)
            seq_ids.append(counts["seq"])
            counts["seq"] += 1
            baked.append(f"{kind} {nframes}f {speed:.0f}u/s")

        # The weapon in its hand
        weapon = opts.get("weapon", "")
        grip = weapon_grip(fs, sk, palette, weapon) if weapon in WEAPONS else None
        if grip:
            bone, mat = grip
            wpn_data.extend(struct.pack("<16si12f", WEAPONS[weapon][1].encode(), bone, *mat[0], *mat[1], *mat[2]))
        else:
            if weapon:
                log(f"  {label}: no hand to hold {weapon}")
            wpn_data.extend(struct.pack("<16si12f", b"", -1, *([0.0] * 12)))

        health = float(opts.get("health", "50"))
        npc_data += struct.pack("<32s16sf6f2II5II", label.encode()[:31], opts.get("ai", "citizen").encode()[:15],
                                health, *m.mins, *m.maxs, first_mesh, len(m.meshes), len(used), *seq_ids, first_rag)
        # The bones only the headcrab's vertices follow: hidden on the ragdoll once it has jumped off
        crab_mask = 0
        if "headcrabs" in opts:
            crab = {b for mesh in m.meshes if mesh.group.startswith("headcrab") for b in mesh.bones}
            body = {b for mesh in m.meshes if not mesh.group.startswith("headcrab") for b in mesh.bones}
            for b in crab - body:
                crab_mask |= 1 << palette[b]
        crab_data += struct.pack("<Q", crab_mask)
        # npcs.txt halves: the bones below the waist, which stay with the legs when a sawblade cuts it
        leg_mask = 0
        if "halves" in opts:
            for b in used:
                if any(k in sk.bones[b].name for k in ("pelvis", "thigh", "calf", "foot", "toe")):
                    leg_mask |= 1 << palette[b]
        leg_data += struct.pack("<Q", leg_mask)
        voice = VOICES.index(opts["voice"]) if opts.get("voice") in VOICES else 0
        flag_data += struct.pack("<I", (1 if ragdoll_only else 0) | (2 if "player" in opts else 0) | (4 if "releasecrab" in opts else 0)
                                 | (16 if "halves" in opts else 0) | (voice << 8))
        counts["npc"] += 1
        log(f"  {label:14s} {m.tris:4d} tris, {len(used):2d} bones: {', '.join(baked) or 'ragdoll only'}")

    w = PmapWriter(PNPC_MAGIC, PNPC_VERSION)
    w.add("NPCM", npc_data, counts["npc"])
    w.add("MESH", mesh_data, counts["mesh"])
    w.add("VERT", vert_data, counts["vert"])
    w.add("INDX", idx_data, counts["idx"])
    w.add("SEQS", seq_data, counts["seq"])
    w.add("MATS", mat_data, counts["mat"])
    w.add("NPCB", rag_data, counts["rag"])
    # "NPCW": per NPC, {char weapon[16] (prop pack label, "" = none); i32 palette bone; float m[12]}
    w.add("NPCW", wpn_data, counts["npc"])
    w.add("NPCF", flag_data, counts["npc"])
    w.add("NPCH", crab_data, counts["npc"])
    w.add("NPCL", leg_data, counts["npc"])
    w.add("TEXS", bytes(32 * len(tex_records)), len(tex_records))
    w.add("TXDT", txdt, len(txdt))
    base = w.section_offset("TXDT")
    w.replace("TEXS", b"".join(struct.pack("<HHBBHI5I", tw, th, lv, fl, 0, base + co,
                                           *([base + o for o in lo] + [0] * (5 - len(lo))))
                               for tw, th, lv, fl, co, lo in tex_records))
    size = w.write(out_path)
    log(f"  wrote {out_path}: {counts['npc']} NPCs, {size / 1024:.0f} KB")
