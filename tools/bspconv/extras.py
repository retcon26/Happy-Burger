"""Extra props: thousands of physics props from the installed Source games, for psp/src/propx.c.

Each model becomes its own small prop pack (the same format as props.ppak, one model), and the
packs go back to back into data/props_x.ppak (full detail) and data/props_x.low.ppak. The game
keeps only data/props_x.idx in memory (a name, bounds, mass and category per model) and reads a
model's pack the first time one is spawned. Spawn icons (GMod's own where it has them, else drawn
here from the model) go in data/props_x.icons.

Index (props_x.idx): {"PXIX", u32 1, u32 count, u32 num_categories}, num_categories x char[24],
then per model {char name[24]; i16 mins[3], maxs[3]; f32 mass; u16 category; u8 shape, surface;
u32 offset[2], size[2]} with [0] the low pack and [1] the full one.
Icons (props_x.icons): {"PXIC", u32 count, 0, 0}, then per model a 1024-byte CLUT and a swizzled
64x64 T8 image.
"""

import math
import os
import re
import struct
import tempfile
from multiprocessing import Pool

import numpy as np
from PIL import Image

from . import pspimage
from .props import build_pack, read_prop_list

from .steam import game_dir

# (spawn menu prefix, the game's folders: its own first, then any it builds on)
GAMES = [
    ("HL2", ["Half-Life 2/hl2"]),
    ("Episode 1", ["Half-Life 2/episodic", "Half-Life 2/hl2"]),
    ("Episode 2", ["Half-Life 2/ep2", "Half-Life 2/episodic", "Half-Life 2/hl2"]),
    ("Lost Coast", ["Half-Life 2/lostcoast", "Half-Life 2/hl2"]),
    ("CS:S", ["Counter-Strike Source/cstrike"]),
    ("DoD:S", ["Day of Defeat Source/dod"]),
    ("Portal", ["Portal/portal"]),
    ("L4D", ["left 4 dead/left4dead"]),
    ("L4D2", ["Left 4 Dead 2/left4dead2"]),
    ("Portal 2", ["Portal 2/portal2"]),
    ("FoF", ["Fistful of Frags/fof"]),
    ("Black Mesa", ["Black Mesa/bms"]),
    ("TF2", ["Team Fortress 2/tf", "Team Fortress 2/hl2"]),
]
PER_GAME = 390   # most models taken from one game, so every game (TF2 last) fits under TOTAL
TOTAL = 3800     # the game's spawn menu holds 4096 entries, with the core props
MAX_SIZE = 320   # bigger than this is scenery, not a prop
MIN_CATEGORY = 6 # smaller folders go into the game's "Misc"

SKIP_NAME = re.compile(r"gib|chunk|break|broke|damage|_dmg|debris|shard|piece|_p\d|fragment|_lod|lod\d|skybox|_sky|"
                       r"ragdoll|_dm\d|destruct|rubble|_d\d*$|cable|wire|decal|trim|_end\d*$|corner|section|"
                       r"segment|nodraw|_clip|collision|_col$")
SKIP_FOLDER = {"props_xen", "props_rocks", "props_destruction", "props_windows", "props_buildables",
               "props_animsigns", "props_skybox", "props_foliage", "props_debris", "props_pipes", "props_cliffs",
               "props_canal", "props_rooftop", "props_building_details", "props_doors", "props_bts", "props_backstage",
               "props_underground", "props_highway", "props_residue", "props_interloper", "props_st"}
FOLDER_NAMES = {
    "props_c17": "City", "props_junk": "Junk", "props_interiors": "Interiors", "props_lab": "Lab",
    "props_wasteland": "Wasteland", "props_combine": "Combine", "props_trainstation": "Train Station",
    "props_docks": "Docks", "props_vehicles": "Vehicles", "props_borealis": "Borealis", "props_mining": "Mining",
    "props_silo": "Silo", "props_forest": "Forest", "props_outland": "Outland", "props_radiostation": "Radio Station",
    "props_furniture": "Furniture", "props_misc": "Misc", "props_urban": "Urban", "props_italian": "Italian",
    "props_fortifications": "Fortifications", "props_crates": "Crates", "props_normandy": "Normandy",
    "props_unique": "Unique", "props_equipment": "Equipment", "props_street": "Street", "props_exteriors": "Exteriors",
    "props_downtown": "Downtown", "props_fairgrounds": "Fairgrounds", "props_mall": "Mall", "props_mill": "Mill",
    "props_signs": "Signs", "props_office": "Office", "props_test_chamber": "Test Chamber",
    "props_gameplay": "Gameplay", "props_lambda": "Lambda", "props_blackmesa": "Black Mesa", "props_marines": "Marines",
    "props_generic": "Generic", "props_industrial": "Industrial", "props_questionableethics": "Q. Ethics",
    "props_am": "Anomalous", "props_desert": "Desert", "props_office": "Office", "props_lighting": "Lighting",
    "props_plants": "Plants", "props_swamp": "Swamp", "props_waterfront": "Waterfront", "props_shacks": "Shacks",
    "props_factory": "Factory", "props_lights": "Lights", "props_basement": "Basement", "props_explosive": "Explosive",
    "props_hive": "Hive", "props_citizen_tech": "Citizen Tech", "props_farm": "Farm", "props_cemetery": "Cemetery",
}


def _pretty_folder(parts):
    """A category name from a model's folders: props_c17 -> City, props/cs_office -> Office."""
    folder = parts[1] if len(parts) > 2 else "misc"
    if folder == "props" and len(parts) > 3:
        folder = parts[2]
    if folder in FOLDER_NAMES:
        return FOLDER_NAMES[folder]
    name = re.sub(r"^(props_|props|cs_|de_|dod_|ar_|gm_)", "", folder).replace("_", " ").strip()
    return name.title() or "Misc"


def _game_files(dirs):
    from .vpk import Vpk

    files = set()
    root = game_dir(dirs[0])
    for name in sorted(os.listdir(root)):
        if name.endswith("_dir.vpk"):
            try:
                files |= set(Vpk(os.path.join(root, name)).entries)
            except (OSError, ValueError):
                pass
    md = os.path.join(root, "models")
    if os.path.isdir(md):
        for dp, _, fns in os.walk(md):
            for fn in fns:
                files.add(os.path.relpath(os.path.join(dp, fn), root).replace("\\", "/").lower())
    return files


def discover(core_models, log=print):
    """[(game index, model path, category)] for every candidate prop, deduplicated across games."""
    seen = set(core_models)
    out = []
    for gi, (prefix, dirs) in enumerate(GAMES):
        if not os.path.isdir(game_dir(dirs[0])):
            continue
        files = _game_files(dirs)
        cands = []
        for f in sorted(files):
            if not (f.startswith("models/") and f.endswith(".mdl") and f[:-4] + ".phy" in files) or f in seen:
                continue
            parts = f.split("/")
            if not any(p.startswith("props") for p in parts[1:2]) or parts[1] in SKIP_FOLDER:
                continue
            if SKIP_NAME.search(os.path.splitext(parts[-1])[0]):
                continue
            cands.append((gi, f, _pretty_folder(parts)))
        # Spread the per-game allowance over its folders
        by_cat = {}
        for c in cands:
            by_cat.setdefault(c[2], []).append(c)
        picked = []
        while len(picked) < PER_GAME and any(by_cat.values()):
            for cat in sorted(by_cat):
                if by_cat[cat] and len(picked) < PER_GAME:
                    picked.append(by_cat[cat].pop(0))
        for c in picked:
            seen.add(c[1])
        log(f"  {prefix}: {len(cands)} candidates, {len(picked)} taken")
        out += picked
    return out[:TOTAL]


# --- Worker ---------------------------------------------------------------------------------------

_FS = {}


def _fs_for(gi, gmod_root):
    from .vpk import GameFS

    if gi not in _FS:
        _FS[gi] = GameFS(gmod_root, [game_dir(d) for d in GAMES[gi][1]])
    return _FS[gi]


def _first_model(blob):
    """(name, mins, maxs, mass, flags, surface) of a pack's first model."""
    count = struct.unpack_from("<I", blob, 8)[0]
    for i in range(count):
        sid, o, _size, n = struct.unpack_from("<4sIII", blob, 16 + i * 16)
        if sid == b"MDLS" and n:
            r = struct.unpack_from("<32s6ff5I", blob, o)
            return r[0].split(b"\0")[0], r[1:4], r[4:7], r[7], r[11], r[12]
    return None


def _set_mass(blob, mass):
    out = bytearray(blob)
    count = struct.unpack_from("<I", out, 8)[0]
    for i in range(count):
        sid, o, _size, _n = struct.unpack_from("<4sIII", out, 16 + i * 16)
        if sid == b"MDLS":
            struct.pack_into("<f", out, o + 56, mass)  # after name[32], mins[3], maxs[3]
    return bytes(out)


def render_icon(fs, mats, mdl, size=64, skin=0):
    """The model drawn from GMod's spawn icon angle on white, as an RGB image (None if it won't load)."""
    from .models import load_model

    m = load_model(fs, mdl, 1500, skin=skin)
    if m is None or not m.meshes:
        return None
    ss = size * 2
    mins, maxs = np.array(m.mins, float), np.array(m.maxs, float)
    c = (mins + maxs) / 2
    # GMod's icon camera: pitch 25 down, yaw 220, so it sees the front-left from above
    yaw, pitch = math.radians(220), math.radians(25)
    f = np.array([math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), -math.sin(pitch)])
    r = np.cross(f, [0, 0, 1])
    r /= np.linalg.norm(r)
    u = np.cross(r, f)
    light = -f * 0.6 + u * 0.8
    light /= np.linalg.norm(light)
    allp = np.array([v[5:8] for mesh in m.meshes for v in mesh.verts], float) - c
    sx, sy = allp @ r, allp @ u
    ext = max(sx.max() - sx.min(), sy.max() - sy.min(), 1e-3)
    scale = ss * 0.86 / ext
    ox = ss / 2 - (sx.max() + sx.min()) / 2 * scale
    oy = ss / 2 + (sy.max() + sy.min()) / 2 * scale
    color = np.ones((ss, ss, 3), float)
    depth = np.full((ss, ss), np.inf)
    for mesh in m.meshes:
        mat = mats.get(mesh.material)
        img = mats.load_texture(mat, 128)
        tex = np.asarray(img.convert("RGBA"), float) / 255.0 if img is not None else np.full((1, 1, 4), 0.7)
        th, tw = tex.shape[:2]
        V = np.array(mesh.verts, float)
        P = V[:, 5:8] - c
        X = ox + (P @ r) * scale
        Y = oy - (P @ u) * scale
        Z = P @ f
        N = V[:, 2:5]
        UV = V[:, 0:2]
        for a, b, d in mesh.tris:
            xs, ys = X[[a, b, d]], Y[[a, b, d]]
            x0, x1 = max(int(xs.min()), 0), min(int(xs.max()) + 1, ss - 1)
            y0, y1 = max(int(ys.min()), 0), min(int(ys.max()) + 1, ss - 1)
            if x0 > x1 or y0 > y1:
                continue
            den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(den) < 1e-9:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            w0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / den
            w1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any():
                continue
            z = w0 * Z[a] + w1 * Z[b] + w2 * Z[d]
            zone = depth[y0:y1 + 1, x0:x1 + 1]
            vis = inside & (z < zone)
            if not vis.any():
                continue
            uu = w0 * UV[a, 0] + w1 * UV[b, 0] + w2 * UV[d, 0]
            vv = w0 * UV[a, 1] + w1 * UV[b, 1] + w2 * UV[d, 1]
            px = (np.mod(uu, 1.0) * tw).astype(int) % tw
            py = (np.mod(vv, 1.0) * th).astype(int) % th
            texel = tex[py, px]
            if mat.alphatest or mat.translucent:
                vis &= texel[..., 3] > 0.5
            n = N[a] * w0[..., None] + N[b] * w1[..., None] + N[d] * w2[..., None]
            n /= np.linalg.norm(n, axis=-1, keepdims=True) + 1e-9
            shade = 0.5 + 0.55 * np.abs(n @ light)
            col = np.clip(texel[..., :3] * shade[..., None], 0, 1)
            zone[vis] = z[vis]
            cz = color[y0:y1 + 1, x0:x1 + 1]
            cz[vis] = col[vis]
    out = Image.fromarray((color * 255).astype(np.uint8), "RGB")
    return out.resize((size, size), Image.Resampling.LANCZOS)


def _spawnicon(fs, mdl):
    import io

    data = fs.read("materials/spawnicons/" + mdl[:-4] + ".png")
    if data is None:
        return None
    img = Image.open(io.BytesIO(data)).convert("RGBA").resize((64, 64), Image.Resampling.LANCZOS)
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    return Image.alpha_composite(bg, img).convert("RGB")


def _build_one(task):
    gi, mdl, gmod_root, tmpdir = task
    from .materials import MaterialLoader

    try:
        fs = _fs_for(gi, gmod_root)
        tmp = os.path.join(tmpdir, f"{os.getpid()}.ppak")
        blobs = []
        for tier in ("low", "high"):
            if not build_pack([(mdl, os.path.splitext(os.path.basename(mdl))[0][:23], -1, 0)], gmod_root, tmp, tier,
                              log=lambda *_: None, fs=fs):
                return None
            with open(tmp, "rb") as f:
                blobs.append(f.read())
        info = _first_model(blobs[1])
        if info is None:
            return None
        name, mins, maxs, mass, flags, surface = info
        size = [maxs[k] - mins[k] for k in range(3)]
        if max(size) > MAX_SIZE or sorted(size)[1] < 2.0:
            return None  # scenery, or flat as a sheet
        if not 2.0 <= mass <= 3000.0:
            # Static props' .phy masses are often nonsense (1 kg crates, 11 t vents): go by size
            mass = min(1000.0, max(2.0, size[0] * size[1] * size[2] * 0.0004))
            blobs = [_set_mass(b, mass) for b in blobs]
        icon = _spawnicon(fs, mdl) or render_icon(fs, MaterialLoader(fs), mdl)
        icon_bytes = None
        if icon is not None:
            pt = pspimage.convert(icon.convert("RGBA"), 64, False, max_levels=1)
            icon_bytes = pt.clut + pt.levels[0]
        return name, mins, maxs, mass, flags & 0xFF, surface, blobs, icon_bytes
    except Exception as e:  # one bad model shouldn't stop thousands
        return ("error", f"{mdl}: {type(e).__name__}: {e}")


def build_extras(gmod_root, core_list, out_dir, log=print, workers=None):
    core = {m for m, _l, _s, _f in read_prop_list(core_list)}
    log("extra props: finding models")
    cands = discover(core, log)
    log(f"  {len(cands)} candidates; building")
    tmpdir = tempfile.mkdtemp(prefix="propx_")
    results = []
    with Pool(workers or os.cpu_count()) as pool:
        for k, res in enumerate(pool.imap(_build_one, [(gi, mdl, gmod_root, tmpdir) for gi, mdl, _c in cands], chunksize=4)):
            if res and res[0] == "error":
                log("  skip " + res[1])
                res = None
            results.append(res)
            if (k + 1) % 200 == 0:
                log(f"  {k + 1}/{len(cands)}")

    # Categories: "<game> <folder>", with small folders merged into the game's Misc
    kept = [(c, r) for c, r in zip(cands, results) if r is not None]
    counts = {}
    for (gi, _m, cat), _r in kept:
        counts[(gi, cat)] = counts.get((gi, cat), 0) + 1
    cat_names = []
    entries = []
    paths = {"low": os.path.join(out_dir, "props_x.low.ppak"), "high": os.path.join(out_dir, "props_x.ppak")}
    files = {t: open(p, "wb") for t, p in paths.items()}
    icons = bytearray()
    blank = pspimage.convert(Image.new("RGBA", (64, 64), (255, 255, 255, 255)), 64, False, max_levels=1)
    for (gi, mdl, cat), (name, mins, maxs, mass, shape, surface, blobs, icon) in kept:
        label = f"{GAMES[gi][0]} {cat if counts[(gi, cat)] >= MIN_CATEGORY else 'Misc'}"[:23]
        if label not in cat_names:
            cat_names.append(label)
        ofs = []
        for t, blob in zip(("low", "high"), blobs):
            f = files[t]
            f.write(b"\0" * ((-f.tell()) % 64))
            ofs.append((f.tell(), len(blob)))
            f.write(blob)
        entries.append(struct.pack("<24s3h3hfHBB2I2I", name[:23], *[int(math.floor(v)) for v in mins],
                                   *[int(math.ceil(v)) for v in maxs], mass, cat_names.index(label), shape, surface,
                                   ofs[0][0], ofs[1][0], ofs[0][1], ofs[1][1]))
        icons += icon or (blank.clut + blank.levels[0])
    for f in files.values():
        f.close()
    with open(os.path.join(out_dir, "props_x.idx"), "wb") as f:
        f.write(struct.pack("<4sIII", b"PXIX", 1, len(entries), len(cat_names)))
        for c in cat_names:
            f.write(struct.pack("<24s", c.encode("latin-1")[:23]))
        f.write(b"".join(entries))
    with open(os.path.join(out_dir, "props_x.icons"), "wb") as f:
        f.write(struct.pack("<4sIII", b"PXIC", len(entries), 0, 0))
        f.write(icons)
    sizes = {t: os.path.getsize(p) / 1048576 for t, p in paths.items()}
    log(f"  wrote {len(entries)} extra props in {len(cat_names)} categories: "
        f"{sizes['high']:.0f} MB full, {sizes['low']:.0f} MB low, icons {len(icons) / 1048576:.0f} MB")
