"""Gestures for quick chat (psp/src/npc.c): GMod's player gestures and taunts, from its shared player
animations (models/m_anm.mdl), baked once as the local rotations of the main ValveBiped bones. Every player
character built on that skeleton (HL2's people, the Combine) also gets its own skeleton, so the game poses
it with those rotations: by bone name, as Source plays an animation from an included model.

  "GSTH": per gesture {char name[16]; u32 frames; float fps; u32 first_frame}
  "GSTF": per frame {i16 quat[NUM_CANON][4] (x, y, z, w times 32767); float pelvis[3]} (the pelvis's offset
          from where it sits in the bind pose)
  "SKLI": per NPC {u32 first_bone; u32 num_bones} (no bones: it can't gesture, e.g. TF2's mercenaries)
  "SKLB": per bone, parents first {i16 parent; i8 canon (CANON index, or -1); u8 palette (255: not skinned);
          float pos[3]; float quat[4]} (its bind pose, from its parent)
"""
import struct

from .animation import AnimDesc, Skeleton

REFERENCE = "models/m_anm.mdl"
# (what the quick chat wheel calls it, GMod's sequence)
GESTURES = [
    ("Wave", "gesture_wave_original"),
    ("Salute", "gesture_salute_original"),
    ("Yes", "gesture_agree_original"),
    ("No", "gesture_disagree_original"),
    ("Come here", "gesture_becon_original"),
    ("Cheer", "taunt_cheer_base"),
    ("Laugh", "taunt_laugh_base"),
    ("Dance", "taunt_dance_base"),
]
# The bones a gesture moves (npc.c's NUM_CANON must match); the rest keep their bind pose
CANON = ["pelvis", "spine", "spine1", "spine2", "spine4", "neck1", "head1",
         "l_clavicle", "l_upperarm", "l_forearm", "l_hand", "r_clavicle", "r_upperarm", "r_forearm", "r_hand",
         "l_thigh", "l_calf", "l_foot", "l_toe0", "r_thigh", "r_calf", "r_foot", "r_toe0"]
CANON_NAMES = ["valvebiped.bip01_" + n for n in CANON]
MIN_CANON = 15  # a skeleton with fewer of them isn't one these animations fit
MAX_FRAMES = 150


def bake(fs, fps, log):
    """(GSTH bytes, GSTF bytes, gesture count, frame count), or None without GMod's player animations."""
    try:
        sk = Skeleton(fs, REFERENCE)
    except (ValueError, struct.error):
        log(f"  gestures: no {REFERENCE}")
        return None
    names = {b.name: i for i, b in enumerate(sk.bones)}
    canon = [names.get(n, -1) for n in CANON_NAMES]
    pelvis = canon[0]
    bind = sk.bones[pelvis].pos if pelvis >= 0 else (0.0, 0.0, 0.0)
    f = sk.files[0]
    seqs = {label: anim for label, act, anim, flags in f.sequences()}
    head, frames = bytearray(), bytearray()
    total = 0
    for name, label in GESTURES:
        anim = seqs.get(label, -1)
        if not 0 <= anim < f.num_anims:
            log(f"  gestures: no {label}")
            head += struct.pack("<16sIfI", name.encode()[:15], 0, fps, total)
            continue
        desc = AnimDesc(f, anim)
        duration = max(1, desc.num_frames - 1) / desc.fps
        n = max(2, min(MAX_FRAMES, int(round(duration * fps)) + 1))
        rate = (n - 1) / duration
        head += struct.pack("<16sIfI", name.encode()[:15], n, rate, total)
        for i in range(n):
            pose = sk.local_pose(0, desc, min(desc.num_frames - 1, i / rate * desc.fps))
            for c in canon:
                q = pose[c][1] if c >= 0 else (0.0, 0.0, 0.0, 1.0)
                frames += struct.pack("<4h", *(max(-32767, min(32767, int(round(v * 32767)))) for v in q))
            p = pose[pelvis][0] if pelvis >= 0 else bind
            frames += struct.pack("<3f", p[0] - bind[0], p[1] - bind[1], p[2] - bind[2])
        total += n
    log(f"  gestures: {len(GESTURES)} from {REFERENCE}, {total} frames at {fps:g} fps, {len(frames) // 1024} KB")
    return bytes(head), bytes(frames), len(GESTURES), total


def skeleton(sk, palette):
    """SKLB bytes for a character the gestures fit, else b''."""
    names = [b.name for b in sk.bones]
    if sum(1 for n in CANON_NAMES if n in names) < MIN_CANON:
        return b""
    out = bytearray()
    for i, b in enumerate(sk.bones):
        c = CANON_NAMES.index(b.name) if b.name in CANON_NAMES else -1
        out += struct.pack("<hbB3f4f", b.parent, c, palette.get(i, 255), *b.pos, *b.quat)
    return bytes(out)
