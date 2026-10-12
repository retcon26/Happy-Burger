"""BSP -> .pmap conversion: geometry, baked lighting, textures, PVS-culled batches and BSP tree."""

import math
import os
import re
import struct
import time
from collections import defaultdict

import numpy as np
from PIL import Image

from . import entities, pspimage, staticprops
from .bsp import (SURF_HINT, SURF_NODRAW, SURF_SKIP, SURF_SKY, SURF_SKY2D, SURF_TRIGGER, Bsp)
from .collision import build_collision, leaves_touching_box
from .collision import model_brushes as entities_model_brushes
from .geometry import build_displacement, build_face, decode_lighting, make_ccw
from .materials import MaterialLoader
from .pmap import BATCH_ALPHATEST, BATCH_NOCULL, BATCH_TRANSLUCENT, PmapWriter
from .vpk import GameFS

TIERS = {
    # tex: max texture size. grid_min/grid_max: tessellation range, chosen per face from how much its
    # lighting varies. light_error: allowed vertex-lighting error (0-255). disp_power: max displacement detail.
    "high": {"id": 1, "tex": 128, "grid_min": 128.0, "grid_max": 512.0, "light_error": 14.0, "disp_power": 3,
             "cell": 1024.0, "fog_end": 6000.0, "sky": 256},
    "low": {"id": 0, "tex": 64, "grid_min": 256.0, "grid_max": 512.0, "light_error": 24.0, "disp_power": 2,
            "cell": 1024.0, "fog_end": 4000.0, "sky": 128},
}

# Half-Life: Source keeps the original game's textures: 8-bit, nearly all 256 or smaller, so they fit the
# paletted format exactly and a whole map's worth is only 1-4MB. Maps from these games keep them at
# full size (up to 512, on the PS2 256) instead of the tier's usual limit.
FULL_RES_GAMES = {"hl1", "hl1mp", "valve", "gearbox", "bshift"}
FULL_RES_TEX = {"high": 512, "low": 128}
FULL_RES_PROP_TEX = {"high": 256, "low": 64}

SKIP_SURF = SURF_NODRAW | SURF_SKIP | SURF_SKY | SURF_SKY2D | SURF_HINT | SURF_TRIGGER
MAX_BATCH_VERTS = 65535
# GMod / HL2 spawn points first, then HL2DM and CS:S team spawns
SPAWN_CLASSES = ("info_player_start", "info_player_deathmatch", "info_player_combine", "info_player_rebel",
                 "info_player_counterterrorist", "info_player_terrorist")


def _gamma_abgr(rgb_linear, alpha=255):
    c = [min(255, int((max(0.0, v) ** (1 / 2.2)) * 255 + 0.5)) for v in rgb_linear]
    return (alpha << 24) | (c[2] << 16) | (c[1] << 8) | c[0]


def _parse_vec(s, default=(0.0, 0.0, 0.0)):
    try:
        v = [float(x) for x in s.split()]
        return tuple(v[:3]) if len(v) >= 3 else default
    except (AttributeError, ValueError):
        return default


class Batch:
    def __init__(self, mat):
        self.mat = mat
        self.verts = []
        self.vmap = {}
        self.indices = []
        self.clusters = set()
        self.mins = [math.inf] * 3
        self.maxs = [-math.inf] * 3


def content_dirs_for(bsp_path):
    """For maps from other Source games (.../<game>/<mod>/maps/x.bsp): that mod's folder and the
    game's hl2 folder, so its own materials and models are found before GMod's."""
    d = os.path.dirname(os.path.abspath(bsp_path))
    if os.path.basename(d).lower() != "maps":
        return []
    mod = os.path.dirname(d)
    if os.path.basename(mod).lower() == "garrysmod":
        return []
    out = [mod]
    root = os.path.dirname(mod)
    # The game's gameinfo.txt lists what else it mounts: HL:S Deathmatch, for one, takes most of its
    # textures from the Half-Life: Source folder (hl1) next to it, not from hl1mp
    for folder in _gameinfo_folders(os.path.join(mod, "gameinfo.txt")):
        p = os.path.join(root, folder)
        if os.path.isdir(p) and p not in out:
            out.append(p)
    hl2 = os.path.join(root, "hl2")
    if os.path.isdir(hl2) and hl2 not in out:
        out.append(hl2)
    return out


def _gameinfo_folders(path):
    """Game folders named in a gameinfo.txt's SearchPaths ("game+mod hl1/hl1_pak.vpk" -> "hl1"), in order."""
    try:
        with open(path, encoding="latin-1") as f:
            text = f.read()
    except OSError:
        return []
    out = []
    for m in re.finditer(r"^\s*game[\w+]*\s+\"?([^\s\"]+)", text, re.MULTILINE | re.IGNORECASE):
        value = re.sub(r"\|[^|]*\|", "", m.group(1)).replace("\\", "/").strip("/")
        folder = value.split("/")[0]
        if folder and folder not in out:
            out.append(folder)
    return out


def game_folder(bsp_path):
    """The game folder a map is in (".../hl1mp/maps/crossfire.bsp" -> "hl1mp"), lower case, or ""."""
    d = os.path.dirname(os.path.abspath(bsp_path))
    return os.path.basename(os.path.dirname(d)).lower() if os.path.basename(d).lower() == "maps" else ""


def open_bsp(path):
    """A Source map, or a Half-Life one dressed up as one (goldsrc.py)."""
    from .goldsrc import GoldSrcBsp, is_goldsrc

    return GoldSrcBsp(path) if is_goldsrc(path) else Bsp(path)


def convert_map(bsp_path, gmod_root, out_path, tier_name, log=print):
    tier = dict(TIERS[tier_name])
    prop_tex = None
    if game_folder(bsp_path) in FULL_RES_GAMES:
        tier["tex"] = FULL_RES_TEX[tier_name]
        prop_tex = FULL_RES_PROP_TEX[tier_name]
        log(f"  full-resolution textures (up to {tier['tex']})")
    t0 = time.time()
    bsp = open_bsp(bsp_path)
    fs = GameFS(gmod_root, content_dirs_for(bsp_path))
    fs.set_pak(bsp.pakfile())
    # Half-Life maps bring their own textures (.wad); Source maps use .vmt/.vtf
    mats = bsp.material_loader() if hasattr(bsp, "material_loader") else MaterialLoader(fs)

    if bsp.lighting:
        faces, lighting = bsp.faces, decode_lighting(bsp.lighting)
    else:
        faces, lighting = (bsp.faces_hdr or bsp.faces), decode_lighting(bsp.lighting_hdr)

    face_clusters = defaultdict(set)
    for leaf in bsp.leafs:
        if leaf.cluster >= 0:
            for i in range(leaf.firstleafface, leaf.firstleafface + leaf.numleaffaces):
                face_clusters[bsp.leaffaces[i]].add(leaf.cluster)

    # World faces, plus faces of static brush entities (func_brush, ...) placed at their origin.
    # Moving entities (doors, buttons, fans) get batches of their own in local space.
    ents = bsp.entities()
    moving, static_ents, triggers = entities.classify(bsp)
    moving_slot = {mi: i for i, (_e, mi) in enumerate(moving)}
    skip_models = {mi for _e, mi in triggers}
    jobs = [(0, (0.0, 0.0, 0.0))]
    for e in ents:
        idx = entities.model_index(e)
        if (0 < idx < len(bsp.models) and idx not in skip_models and not e.get("classname", "").startswith("trigger_")
                and not entities.starts_off(e)):
            jobs.append((idx, (0.0, 0.0, 0.0) if idx in moving_slot else _parse_vec(e.get("origin", ""))))

    mat_index = {}
    mat_list = []
    batches = {}
    ent_batches = defaultdict(dict)  # moving entity slot -> {material: Batch}
    stats = {"faces": 0, "disp": 0, "tris": 0}

    for model_idx, offset in jobs:
        model = bsp.models[model_idx]
        ent_slot = moving_slot.get(model_idx)
        for fi in range(model.firstface, model.firstface + model.numfaces):
            f = faces[fi]
            if f.texinfo < 0:
                continue
            ti = bsp.texinfo[f.texinfo]
            if ti.flags & SKIP_SURF:
                continue
            name = bsp.texdata[ti.texdata].name
            lname = name.lower()
            if lname.startswith("tools/"):
                continue
            mat = mats.get(name)

            if f.dispinfo >= 0:
                mesh = build_displacement(bsp, f, lighting, tier["disp_power"])
                stats["disp"] += 1
            else:
                mesh = build_face(bsp, f, lighting, tier["grid_min"], tier["grid_max"], tier["light_error"], offset)
            make_ccw(bsp, f, mesh)
            if not mesh.tris:
                continue
            stats["faces"] += 1

            clusters = face_clusters.get(fi) if model_idx == 0 else None
            if not clusters:
                xs = [v[3] for v in mesh.verts]
                ys = [v[4] for v in mesh.verts]
                zs = [v[5] for v in mesh.verts]
                leaves = []
                leaves_touching_box(bsp, bsp.models[0].headnode, (min(xs), min(ys), min(zs)),
                                     (max(xs), max(ys), max(zs)), leaves)
                clusters = {bsp.leafs[l].cluster for l in leaves if bsp.leafs[l].cluster >= 0}

            mi = mat_index.get(lname)
            if mi is None:
                mi = len(mat_list)
                mat_index[lname] = mi
                mat_list.append((mat, bsp.texdata[ti.texdata]))

            cell = tier["cell"]
            for tri in mesh.tris:
                if ent_slot is not None:
                    eb = ent_batches[ent_slot]
                    b = eb.get(mi)
                    if b is None:
                        b = eb[mi] = Batch(mi)
                else:
                    cx = sum(mesh.verts[i][3] for i in tri) / 3.0
                    cy = sum(mesh.verts[i][4] for i in tri) / 3.0
                    cz = sum(mesh.verts[i][5] for i in tri) / 3.0
                    key = (mi, int(math.floor(cx / cell)), int(math.floor(cy / cell)), int(math.floor(cz / cell)))
                    b = batches.get(key)
                    if b is None or len(b.verts) > MAX_BATCH_VERTS - 3:
                        if b is not None:
                            # Batch full: retire it under a unique key and start a fresh one.
                            batches[key + (len(batches),)] = b
                        b = Batch(mi)
                        batches[key] = b
                for li in tri:
                    vk = (fi, model_idx, li)
                    bi = b.vmap.get(vk)
                    if bi is None:
                        bi = len(b.verts)
                        b.vmap[vk] = bi
                        v = mesh.verts[li]
                        b.verts.append(v)
                        for k in range(3):
                            b.mins[k] = min(b.mins[k], v[3 + k])
                            b.maxs[k] = max(b.maxs[k], v[3 + k])
                    b.indices.append(bi)
                b.clusters |= clusters
                stats["tris"] += 1

    log(f"  {stats['faces']} faces ({stats['disp']} displacements) -> {stats['tris']} triangles, "
        f"{len(batches)} batches, {len(mat_list)} materials  [{time.time() - t0:.1f}s]")

    # Textures
    tex_records = []
    txdt = bytearray()
    mat_flags = []
    missing = 0
    for mat, td in mat_list:
        keep_alpha = mat.alphatest or mat.translucent
        img = mats.load_texture(mat, tier["tex"]) if not mat.water else None
        if mat.water:
            # Water: the material's $fogcolor (e.g. "{38 52 46}") tinted see-through
            fc = mat.params.get("$fogcolor", "")
            nums = [float(x) for x in fc.replace("{", " ").replace("}", " ").replace("[", " ").replace("]", " ").split()
                    if x.replace(".", "", 1).isdigit()]
            if len(nums) >= 3:
                rgb = [v * 255 if max(nums[:3]) <= 1.0 else v for v in nums[:3]]
                rgb = [min(255, c * 1.6 + 30) for c in rgb]  # fog colours are dark; brighten for a surface
            else:
                rgb = [min(255, (c ** (1 / 2.2)) * 255) for c in td.reflectivity]
            pt = pspimage.solid(rgb, 150)
        elif img is None:
            missing += 1
            pt = pspimage.solid([min(255, (c ** (1 / 2.2)) * 255) for c in td.reflectivity])
        else:
            pt = pspimage.convert(img, tier["tex"], keep_alpha)
        flags = 0
        if mat.alphatest:
            flags |= BATCH_ALPHATEST
        if mat.translucent or mat.water:
            flags |= BATCH_TRANSLUCENT
        if mat.nocull or mat.water:  # water is also seen from underneath
            flags |= BATCH_NOCULL
        mat_flags.append(flags)

        clut_ofs = len(txdt)
        txdt += pt.clut
        level_ofs = []
        for lvl in pt.levels:
            txdt += b"\0" * ((-len(txdt)) % 16)
            level_ofs.append(len(txdt))
            txdt += lvl
        txdt += b"\0" * ((-len(txdt)) % 16)
        tex_records.append((pt.width, pt.height, len(pt.levels), flags, clut_ofs, level_ofs))
    tex_bytes = sum(len(x) for x in [txdt])
    log(f"  textures: {len(tex_records)} ({missing} missing, flat colour used), {tex_bytes / 1024:.0f} KB")

    # 2D skybox: six faces in Quake/Source order rt bk lf ft up dn, appended after the map's textures.
    # GMod's "painted" sky is drawn from env_skypaint colours instead.
    sky_first = -1
    sky_horizon = None  # average colour just above the horizon, used for fog so distant walls blend in
    skyname = (ents[0].get("skyname", "") if ents else "").lower()
    if skyname and skyname != "painted":
        faces_img = []
        for suffix in ("rt", "bk", "lf", "ft", "up", "dn"):
            mat = mats.get(f"skybox/{skyname}{suffix}")
            img = mats.load_texture(mat, tier["sky"])
            if img is None and suffix == "dn" and faces_img:
                # Many skies skip the bottom face: fill it with the sides' lowest colour
                low = np.array(faces_img[0].convert("RGB"))[-4:].reshape(-1, 3).mean(axis=0)
                img = Image.new("RGBA", (64, 64), tuple(int(c) for c in low) + (255,))
            if img is None:
                faces_img = None
                break
            faces_img.append(img)
        if faces_img:
            band = [np.array(f.convert("RGB").resize((32, 32)))[14:20].reshape(-1, 3) for f in faces_img[:4]]
            sky_horizon = tuple(float(c) / 255.0 for c in np.concatenate(band).mean(axis=0))
            sky_first = len(tex_records)
            for img in faces_img:
                pt = pspimage.convert(img.convert("RGB").convert("RGBA"), tier["sky"], False, max_levels=2)
                clut_ofs = len(txdt)
                txdt += pt.clut
                level_ofs = []
                for lvl in pt.levels:
                    txdt += b"\0" * ((-len(txdt)) % 16)
                    level_ofs.append(len(txdt))
                    txdt += lvl
                txdt += b"\0" * ((-len(txdt)) % 16)
                tex_records.append((pt.width, pt.height, len(pt.levels), 0, clut_ofs, level_ofs))
            log(f"  skybox: {skyname}")
        else:
            log(f"  skybox: {skyname} not found, using a gradient")

    # Batches sorted so the PSP switches texture as rarely as possible; translucent last.
    blist = sorted(batches.values(), key=lambda b: (bool(mat_flags[b.mat] & BATCH_TRANSLUCENT), b.mat,
                                                    b.mins[0], b.mins[1], b.mins[2]))
    vert_data = bytearray()
    idx_data = bytearray()
    batch_data = bytearray()
    nverts = nidx = 0
    lstyle_records = []  # (vertex index, base colour, style, dr, dg, db)

    def emit_batch(b):
        """Packs one batch as 16-byte vertices (16-bit UV and position) plus its decode parameters.

        Positions snap to a 1/16-unit grid shared by every batch, so neighbours stay crack-free;
        a batch too big for 16 bits at that grid uses a coarser power-of-two step.
        UVs are stored relative to the batch's own range, which the GE expands with TexScale/Offset.
        """
        nonlocal nverts, nidx
        extent = max(b.maxs[k] - b.mins[k] for k in range(3))
        step = 1.0 / 16.0
        while extent / step > 32000:
            step *= 2.0
        org = [math.floor(b.mins[k] / step) * step for k in range(3)]
        us = [v[0] for v in b.verts]
        vs = [v[1] for v in b.verts]
        umin, vmin = math.floor(min(us)), math.floor(min(vs))
        urange = max(max(us) - umin, 1e-3)
        vrange = max(max(vs) - vmin, 1e-3)
        rec = struct.pack("<3I2H6f3f2f2f3I", nverts, nidx, len(b.indices), b.mat, mat_flags[b.mat], *b.mins, *b.maxs,
                          *org, umin, vmin, urange, vrange, int(step * 65536), 0, 0)
        for u, v, col, x, y, z, styles in b.verts:
            for style, dr, dg, db in styles:
                # vert_data holds every vertex written so far, 16 bytes each: this one's global index
                lstyle_records.append((len(vert_data) // 16, col, style, dr, dg, db))
            qu = min(65535, max(0, int(round((u - umin) / urange * 32768.0))))
            qv = min(65535, max(0, int(round((v - vmin) / vrange * 32768.0))))
            qx = int(round((x - org[0]) / step))
            qy = int(round((y - org[1]) / step))
            qz = int(round((z - org[2]) / step))
            vert_data.extend(struct.pack("<2HI3hh", qu, qv, col, qx, qy, qz, 0))
        idx_data.extend(struct.pack(f"<{len(b.indices)}H", *b.indices))
        nverts += len(b.verts)
        nidx += len(b.indices)
        return rec

    for b in blist:
        batch_data += emit_batch(b)

    # Moving entities' batches follow the world's in the same vertex and index arrays
    ebatch_data = bytearray()
    ent_batch_ranges = {}
    neb = 0
    for slot in range(len(moving)):
        first = neb
        for mi_, b in sorted(ent_batches.get(slot, {}).items()):
            ebatch_data += emit_batch(b)
            neb += 1
        ent_batch_ranges[slot] = (first, neb - first)

    # Static props and physics props placed in the map: their own model pack next to the map
    pack_path = out_path[: -len(".pmap")] + ".ppak" if out_path.endswith(".pmap") else out_path + ".ppak"
    mprops = staticprops.collect(bsp, fs, pack_path, tier_name, log, tex_size=prop_tex)
    # Solid props collide as their .phy's convex pieces (so a door frame leaves its doorway open),
    # or as a box when the model has none
    prop_boxes = []
    prop_hulls = []
    if mprops:
        hull_cache = {}
        for li, o, axes, solid, _fade, path in mprops.statics:
            if solid and li in mprops.bounds:
                box = staticprops.static_collision_box(mprops.bounds[li], path)
                if not box:
                    continue
                pieces = staticprops.static_collision_pieces(fs, path, hull_cache)
                if pieces:
                    prop_hulls.append((axes, o, pieces))
                else:
                    prop_boxes.append((axes, o, box[0], box[1]))

    # Per-cluster visibility as bitsets over batches
    num_clusters = bsp.num_clusters()
    row_words = (len(blist) + 31) // 32
    cluster_batches = defaultdict(list)
    for bi, b in enumerate(blist):
        for c in b.clusters:
            cluster_batches[c].append(bi)
    pvs_data = bytearray()
    vis_counts = []
    for c in range(num_clusters):
        row = [0] * row_words
        visible = bsp.cluster_pvs(c) or set()
        count = 0
        seen = set()
        for vc in visible:
            for bi in cluster_batches.get(vc, ()):
                if bi not in seen:
                    seen.add(bi)
                    row[bi >> 5] |= 1 << (bi & 31)
                    count += 1
        vis_counts.append(count)
        pvs_data += struct.pack(f"<{row_words}I", *row)
    if vis_counts:
        log(f"  PVS: {num_clusters} clusters, visible batches avg {sum(vis_counts) / len(vis_counts):.0f}"
            f" max {max(vis_counts)} of {len(blist)}")

    # Static props: per-cluster visibility bitsets too
    sprp_data = b""
    spvs_data = bytearray()
    sprp_words = 0
    if mprops and mprops.statics:
        sprp_data = staticprops.pack_statics(mprops)
        sprp_words = (len(mprops.statics) + 31) // 32
        cluster_props = defaultdict(list)
        for pi, (li, o, (ax, ay, az), _s, _f, _p) in enumerate(mprops.statics):
            mn, mx = mprops.bounds.get(li, ((-16, -16, -16), (16, 16, 16)))
            lc = [(mn[k] + mx[k]) / 2 for k in range(3)]
            half = [(mx[k] - mn[k]) / 2 for k in range(3)]
            c = [o[k] + ax[k] * lc[0] + ay[k] * lc[1] + az[k] * lc[2] for k in range(3)]
            ext = [abs(ax[k]) * half[0] + abs(ay[k]) * half[1] + abs(az[k]) * half[2] for k in range(3)]
            leaves = []
            leaves_touching_box(bsp, bsp.models[0].headnode, [c[k] - ext[k] for k in range(3)],
                                [c[k] + ext[k] for k in range(3)], leaves)
            for cl in {bsp.leafs[l].cluster for l in leaves if bsp.leafs[l].cluster >= 0}:
                cluster_props[cl].append(pi)
        for c in range(num_clusters):
            row = [0] * sprp_words
            for vc in bsp.cluster_pvs(c) or set():
                for pi in cluster_props.get(vc, ()):
                    row[pi >> 5] |= 1 << (pi & 31)
            spvs_data += struct.pack(f"<{sprp_words}I", *row)

    # Collision: map brushes, displacement prisms, static brush entities and solid static props
    static_list = [(mi, _parse_vec(e.get("origin", ""))) for e, mi in static_ents]
    cd = build_collision(bsp, faces, min(2, tier["disp_power"]), static_list, prop_boxes, prop_hulls)
    log(f"  collision: {len(cd.brushes)} brushes ({cd.disp_tris} from displacements, "
        f"{cd.static_ent_brushes} from brush entities, {len(prop_boxes)} prop boxes, {len(prop_hulls)} prop hulls"
        f" in {cd.prop_hull_pieces} pieces), {len(cd.planes)} planes")

    # Entities: moving ones and triggers, with their outputs
    ent_records = []
    # Named lights with a switchable style (vrad gives them style >= 32) can be turned on and off by outputs
    light_ents = [e for e in ents if e.get("classname", "").startswith("light") and e.get("targetname")
                  and int(float(e.get("style", "0") or 0)) >= 32]
    ent_list = ([(e, mi, "moving") for e, mi in moving] + [(e, mi, "trigger") for e, mi in triggers]
                + [(e, None, "light") for e in light_ents])
    names_to_index = defaultdict(list)
    for i, (e, _mi, _k) in enumerate(ent_list):
        tn = e.get("targetname", "").lower()
        if tn:
            names_to_index[tn].append(i)
    ebrush_list = []
    outputs = []
    for i, (e, mi, kind) in enumerate(ent_list):
        origin = _parse_vec(e.get("origin", ""))
        flags = int(float(e.get("spawnflags", "0") or 0))
        if kind == "light":
            ent_records.append({
                "type": entities.ENT_LIGHT, "flags": flags, "name": e.get("targetname", ""), "origin": origin,
                "axis": (0.0, 0.0, 0.0), "distance": float(e.get("style", "0")), "speed": 0.0, "wait": -1.0,
                "solid": 0, "mins": (0.0, 0.0, 0.0), "maxs": (0.0, 0.0, 0.0), "first_batch": 0, "num_batches": 0,
                "first_brush": 0, "num_brushes": 0, "first_output": len(outputs), "num_outputs": 0})
            continue
        md = bsp.models[mi]
        if kind == "moving":
            etype, axis, dist, speed, wait = entities.entity_params(e, bsp, mi)
            fb, nbt = ent_batch_ranges.get(i, (0, 0))
        else:
            etype, axis, dist, speed, wait = entities.ENT_TRIGGER, (0.0, 0.0, 1.0), 0.0, 0.0, -1.0
            fb, nbt = 0, 0
            if e.get("classname") == "trigger_once":
                flags |= 0x80000000
        first_brush = len(ebrush_list)
        if kind == "moving":
            for b in entities_model_brushes(bsp, mi):
                nb = cd.remap.get(b)
                if nb is not None:
                    ebrush_list.append(nb)
        outs = entities.resolve_outputs(e, names_to_index)
        ent_records.append({
            "type": etype, "flags": flags, "name": e.get("targetname", ""), "origin": origin, "axis": axis,
            "distance": dist, "speed": speed, "wait": wait,
            "solid": 0 if kind == "trigger" or e.get("_nonsolid") else 1,
            "mins": md.mins, "maxs": md.maxs, "first_batch": fb, "num_batches": nbt,
            "first_brush": first_brush, "num_brushes": len(ebrush_list) - first_brush,
            "first_output": len(outputs), "num_outputs": len(outs)})
        outputs.extend(outs)
    ents_data, outs_data = entities.pack_entities(ent_records, outputs)
    if ent_records:
        kinds = defaultdict(int)
        for r in ent_records:
            kinds[r["type"]] += 1
        log(f"  entities: {dict(kinds)} (1 door, 2 rotating door, 3 button, 4 rotating, 5 trigger, 6 light), "
            f"{len(outputs)} outputs")

    # Light styles: per-vertex deltas, plus the flicker pattern of each style
    lstyle_records.sort(key=lambda r: r[0])
    lsty_data = b"".join(struct.pack("<IIH3h", v, col, s, dr, dg, db) for v, col, s, dr, dg, db in lstyle_records)
    patterns = list(entities.DEFAULT_STYLE_PATTERNS) + [""] * (64 - len(entities.DEFAULT_STYLE_PATTERNS))
    for e in ents:
        if e.get("classname", "").startswith("light") and e.get("pattern"):
            s = int(float(e.get("style", "0") or 0))
            if 0 < s < 64:
                patterns[s] = e["pattern"][:63]
    lstp_data = b"".join(struct.pack("<64s", p.encode("latin-1")[:63]) for p in patterns)
    if lstyle_records:
        log(f"  light styles: {len(lstyle_records)} vertex deltas, styles "
            f"{sorted({r[2] for r in lstyle_records})}")
    plane_data = b"".join(struct.pack("<4f", *p) for p in cd.planes)
    brush_data = b"".join(struct.pack("<3I", *b) for b in cd.brushes)
    side_data = struct.pack(f"<{len(cd.sides)}I", *cd.sides)
    node_data = b"".join(struct.pack("<3i", n.planenum, *n.children) for n in bsp.nodes)

    ambient = bsp.leaf_ambient()
    leafbrush_list = []
    leaf_data = bytearray()
    default_amb = (0.3, 0.3, 0.3)
    for li, l in enumerate(bsp.leafs):
        lb = cd.leaf_brushes[li]
        amb = ambient[li] or default_amb
        leaf_data += struct.pack("<iIIIII", l.cluster, l.contents & 0xFFFFFFFF, len(leafbrush_list), len(lb),
                                 _gamma_abgr(amb), 0)
        leafbrush_list.extend(lb)
    leafbrush_data = struct.pack(f"<{len(leafbrush_list)}I", *leafbrush_list)

    spawns = []
    fog = skypaint = sunlight = None
    for e in ents:
        cls = e.get("classname", "")
        if cls in SPAWN_CLASSES:
            o = _parse_vec(e.get("origin", ""))
            ang = _parse_vec(e.get("angles", ""))
            spawns.append((SPAWN_CLASSES.index(cls), struct.pack("<4f", o[0], o[1], o[2] + 1.0, ang[1])))
        elif cls == "env_fog_controller" and fog is None:
            fog = e
        elif cls == "env_skypaint" and skypaint is None:
            skypaint = e
        elif cls == "light_environment" and sunlight is None:
            sunlight = e
    spawns = [s for _rank, s in sorted(spawns, key=lambda r: r[0])]
    if not spawns:
        spawns.append(struct.pack("<4f", 0.0, 0.0, 1.0, 0.0))

    sky_top = (0.2, 0.5, 1.0)
    sky_bottom = (0.8, 1.0, 1.0)
    if skypaint is not None:
        sky_top = _parse_vec(skypaint.get("topcolor", ""), sky_top)
        sky_bottom = _parse_vec(skypaint.get("bottomcolor", ""), sky_bottom)
    if sky_horizon is not None and skypaint is None:
        sky_bottom = sky_top = sky_horizon
    fog_col = sky_bottom
    fog_end = tier["fog_end"]
    fog_start = fog_end * 0.4
    if fog is not None and fog.get("fogenable", "1") != "0":
        col = _parse_vec(fog.get("fogcolor", ""), None)
        if col and skypaint is None:
            fog_col = tuple(c / 255.0 for c in col)
        try:
            fog_end = min(fog_end, float(fog.get("fogend", fog_end)))
            fog_start = min(fog_end * 0.9, float(fog.get("fogstart", fog_start)))
        except ValueError:
            pass

    # Sun: light_environment pitch is the light's downward angle (-90 = straight down)
    sun_dir = (0.4, 0.3, 0.866)
    sun_col = (0.9, 0.85, 0.75)
    amb_col = (0.35, 0.38, 0.45)
    if sunlight is not None:
        ang = _parse_vec(sunlight.get("angles", ""))
        try:
            pitch = float(sunlight.get("pitch", ang[0]))
        except ValueError:
            pitch = ang[0]
        p, y = math.radians(pitch), math.radians(ang[1])
        sun_dir = (-math.cos(p) * math.cos(y), -math.cos(p) * math.sin(y), -math.sin(p))
        lt = _parse_vec(sunlight.get("_light", ""), None)
        if lt:
            sun_col = tuple(min(1.0, c / 255.0) for c in lt)
        am = _parse_vec(sunlight.get("_ambient", ""), None)
        if am:
            amb_col = tuple(min(1.0, c / 255.0) for c in am)

    def rgb_abgr(c):
        c = [min(255, max(0, int(v * 255 + 0.5))) for v in c]
        return 0xFF000000 | (c[2] << 16) | (c[1] << 8) | c[0]

    world = bsp.models[0]
    w = PmapWriter()
    w.add("INFO", struct.pack("<4I2f6f2I3f2I5I", tier["id"], row_words, rgb_abgr(sky_bottom), rgb_abgr(fog_col),
                              fog_start, fog_end, *world.mins, *world.maxs, rgb_abgr(sky_top),
                              rgb_abgr(sky_bottom), *sun_dir, rgb_abgr(sun_col), rgb_abgr(amb_col),
                              sprp_words, 0, 0, 0, 0), 1)
    w.add("SPWN", b"".join(spawns), len(spawns))
    w.add("TEXS", bytes(32 * len(tex_records)), len(tex_records))
    w.add("TXDT", txdt, len(txdt))
    w.add("VERT", vert_data, nverts)
    w.add("INDX", idx_data, nidx)
    w.add("BTCH", batch_data, len(blist))
    w.add("PLAN", plane_data, len(cd.planes))
    w.add("NODE", node_data, len(bsp.nodes))
    w.add("LEAF", leaf_data, len(bsp.leafs))
    w.add("PVS ", pvs_data, num_clusters)
    w.add("BRSH", brush_data, len(cd.brushes))
    w.add("BSID", side_data, len(cd.sides))
    w.add("LBRS", leafbrush_data, len(leafbrush_list))
    w.add("ENTS", ents_data, len(ent_records))
    w.add("EBAT", ebatch_data, neb)
    w.add("EBRL", struct.pack(f"<{len(ebrush_list)}I", *ebrush_list), len(ebrush_list))
    w.add("EOUT", outs_data, len(outputs))
    w.add("SPRP", sprp_data, len(mprops.statics) if mprops else 0)
    w.add("SPVS", spvs_data, num_clusters if sprp_words else 0)
    w.add("PHYS", staticprops.pack_physics(mprops) if mprops else b"", len(mprops.physics) if mprops else 0)
    w.add("LSTY", lsty_data, len(lstyle_records))
    w.add("LSTP", lstp_data, 64)
    w.add("SKYT", struct.pack("<i", sky_first), 1)

    base = w.section_offset("TXDT")
    tex_data = bytearray()
    for width, height, levels, flags, clut_ofs, level_ofs in tex_records:
        lv = [base + o for o in level_ofs] + [0] * (5 - len(level_ofs))
        tex_data += struct.pack("<HHBBHI5I", width, height, levels, flags, 0, base + clut_ofs, *lv)
    w.replace("TEXS", tex_data)

    size = w.write(out_path)
    log(f"  wrote {out_path}: {size / 1024 / 1024:.2f} MB  (verts {nverts}, tris {nidx // 3})"
        f"  [{time.time() - t0:.1f}s]")
    return size
