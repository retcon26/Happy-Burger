"""Brush entities for the PSP: what moves (doors, buttons, fans), what's static, what triggers.

Moving entities are drawn and collided separately from the world, in their own local space
(Source stores brush entity geometry relative to the entity's origin). Static solid ones such as
func_brush are merged into world collision by collision.py. Their faces are already in the
world batches.
"""

import math
import struct

ENT_DOOR = 1
ENT_DOOR_ROTATING = 2
ENT_BUTTON = 3
ENT_ROTATING = 4
ENT_TRIGGER = 5
ENT_LIGHT = 6

# Source's built-in light style patterns ('a' = dark, 'm' = normal, 'z' = double), stepped at 10 Hz
DEFAULT_STYLE_PATTERNS = [
    "m",
    "mmnmmommommnonmmonqnmmo",
    "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba",
    "mmmmmaaaaammmmmaaaaaabcdefgabcdefg",
    "mamamamamama",
    "jklmnopqrstuvwxyzyxwvutsrqponmlkj",
    "nmonqnmomnmomomno",
    "mmmaaaabcdefgmmmmaaaammmaamm",
    "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",
    "aaaaaaaazzzzzzzz",
    "mmamammmmammamamaaamammma",
    "abcdefghijklmnopqrrqponmlkjihgfedcba",
    "mmnnmmnnnmmnn",
]

MOVING = {"func_door": ENT_DOOR, "func_door_rotating": ENT_DOOR_ROTATING, "func_button": ENT_BUTTON,
          "func_rotating": ENT_ROTATING}
TRIGGERS = {"trigger_multiple", "trigger_once"}
# Never drawn: TF2's spawn-room force fields (only enemies see them, close up) and its spawn-room volumes
HIDDEN = {"func_respawnroomvisualizer", "func_respawnroom", "func_nobuild", "func_regenerate", "func_capturezone"}
STATIC_SOLID = {"func_brush", "func_wall", "func_wall_toggle", "func_reflective_glass", "func_breakable",
                "func_physbox", "func_physbox_multiplayer", "func_movelinear", "func_tracktrain", "func_train",
                "func_breakable_surf", "func_detail_blocker", "func_monitor", "func_lod"}

# Output events and inputs understood by psp/src/ents.c
EV_ACTIVATE = 0  # button pressed / trigger touched
EV_OPEN = 1
EV_CLOSE = 2
EV_UNTOUCH = 3     # the player left a trigger
EV_STARTOPEN = 4   # a door began opening (a double door's other half follows)
EV_STARTCLOSE = 5
IN_OPEN, IN_CLOSE = 1, 2
INPUTS = {"open": 1, "close": 2, "toggle": 3, "start": 4, "stop": 5, "press": 6, "use": 6, "reverse": 7,
          "turnon": 4, "turnoff": 5, "lock": 0, "unlock": 0}
# TF2's spawn doors open on a trigger's OnStartTouchAll and close on its OnEndTouchAll
EVENTS = {"onpressed": EV_ACTIVATE, "onuse": EV_ACTIVATE, "onstarttouch": EV_ACTIVATE, "onstarttouchall": EV_ACTIVATE,
          "ontrigger": EV_ACTIVATE, "onendtouch": EV_UNTOUCH, "onendtouchall": EV_UNTOUCH,
          "onopen": EV_OPEN, "onfullyopen": EV_OPEN, "onclose": EV_CLOSE, "onfullyclosed": EV_CLOSE}


def parse_vec(s, default=(0.0, 0.0, 0.0)):
    try:
        v = [float(x) for x in s.split()]
        return tuple(v[:3]) if len(v) >= 3 else default
    except (AttributeError, ValueError):
        return default


def angle_vectors(pitch, yaw):
    p, y = math.radians(pitch), math.radians(yaw)
    return (math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), -math.sin(p))


def model_index(e):
    m = e.get("model", "")
    if m.startswith("*"):
        try:
            return int(m[1:])
        except ValueError:
            return -1
    return -1


def starts_off(e):
    """Brush entities that begin switched off: neither drawn nor solid until map logic turns them on
    (which the PSP doesn't run), e.g. a disabled func_brush blocking a doorway."""
    cls = e.get("classname", "")
    try:
        flags = int(float(e.get("spawnflags", "0") or 0))
    except ValueError:
        flags = 0
    if cls == "func_brush" and e.get("StartDisabled", "0") == "1":
        return True
    return cls == "func_wall_toggle" and flags & 1


def classify(bsp):
    """Returns (moving, static_solid, triggers) as lists of (entity dict, model index)."""
    moving, static, trig = [], [], []
    for e in bsp.entities():
        cls = e.get("classname", "")
        mi = model_index(e)
        if mi <= 0 or mi >= len(bsp.models):
            continue
        if cls in MOVING:
            moving.append((e, mi))
        elif cls in TRIGGERS:
            trig.append((e, mi))
        elif cls in STATIC_SOLID:
            if (cls == "func_brush" and e.get("Solidity", "0") == "1") or starts_off(e):  # 1 = never solid
                continue
            static.append((e, mi))
    return moving, static, trig


def entity_params(e, bsp, mi):
    """Movement parameters for one moving entity: (type, axis, distance, speed, wait)."""
    cls = e.get("classname", "")
    etype = MOVING[cls]
    flags = int(float(e.get("spawnflags", "0") or 0))
    m = bsp.models[mi]
    size = [m.maxs[k] - m.mins[k] for k in range(3)]
    speed = float(e.get("speed", "100") or 100)
    wait = float(e.get("wait", "3") or 3)
    if etype in (ENT_DOOR, ENT_BUTTON):
        md = parse_vec(e.get("movedir", "0 0 0"))
        axis = angle_vectors(md[0], md[1])
        lip = float(e.get("lip", "0") or 0)
        dist = sum(abs(axis[k]) * size[k] for k in range(3)) - lip
        if etype == ENT_BUTTON and flags & 1:  # "don't move"
            dist = 0.0
        return etype, axis, max(0.0, dist), speed, wait
    if etype == ENT_DOOR_ROTATING:
        axis = (1.0, 0.0, 0.0) if flags & 64 else ((0.0, 1.0, 0.0) if flags & 128 else (0.0, 0.0, 1.0))
        dist = float(e.get("distance", "90") or 90)
        if flags & 2:  # reverse direction
            dist = -dist
        return etype, axis, dist, speed, wait
    # func_rotating: spins about an axis at maxspeed deg/s
    axis = (1.0, 0.0, 0.0) if flags & 4 else ((0.0, 1.0, 0.0) if flags & 8 else (0.0, 0.0, 1.0))
    speed = float(e.get("maxspeed", "100") or 100)
    if flags & 2:
        speed = -speed
    return etype, axis, 0.0, speed, -1.0


def pack_entities(records, outputs):
    """records: dicts with the PmapEnt fields; outputs: (event, target, input, delay) tuples."""
    ents = bytearray()
    for r in records:
        ents += struct.pack("<2I32s6f3fI6f6I", r["type"], r["flags"], r["name"].encode("latin-1")[:31],
                            *r["origin"], *r["axis"], r["distance"], r["speed"], r["wait"], r["solid"],
                            *r["mins"], *r["maxs"], r["first_batch"], r["num_batches"], r["first_brush"],
                            r["num_brushes"], r["first_output"], r["num_outputs"])
    outs = b"".join(struct.pack("<IiIf", ev, tgt, inp, delay) for ev, tgt, inp, delay in outputs)
    return ents, outs


def targets(name, names_to_index):
    """Entity indices a target name refers to; a trailing * matches any name starting with the rest."""
    if name.endswith("*"):
        prefix = name[:-1]
        return [i for n, lst in names_to_index.items() if n.startswith(prefix) for i in lst]
    return names_to_index.get(name, [])


def resolve_outputs(e, names_to_index):
    out = []
    # A double door's halves (prop_door_rotating's slavename) open and close together
    slave = e.get("slavename", "").strip().lower()
    if slave:
        for ti in targets(slave, names_to_index):
            out.append((EV_STARTOPEN, ti, IN_OPEN, 0.0))
            out.append((EV_STARTCLOSE, ti, IN_CLOSE, 0.0))
    for key, val in e.get("_outputs", []):
        ev = EVENTS.get(key.lower())
        if ev is None:
            continue
        parts = val.replace("\x1b", ",").split(",")
        if len(parts) < 2:
            continue
        target, inp = parts[0].strip().lower(), parts[1].strip().lower()
        code = INPUTS.get(inp)
        if not code:
            continue
        try:
            delay = float(parts[3]) if len(parts) > 3 and parts[3].strip() else 0.0
        except ValueError:
            delay = 0.0
        for ti in targets(target, names_to_index):
            out.append((ev, ti, code, delay))
    return out
