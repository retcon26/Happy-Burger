import argparse
import collections
import sys

from .bsp import SURF_NODRAW, SURF_SKIP, SURF_SKY, SURF_SKY2D, SURF_HINT, SURF_TRIGGER, Bsp

DEFAULT_GMOD = "/mnt/d/SteamLibrary/steamapps/common/GarrysMod"


def cmd_info(args):
    b = Bsp(args.bsp)
    print(f"BSP v{b.version} rev {b.revision}")
    print(f"planes {len(b.planes)}  verts {len(b.vertexes)}  edges {len(b.edges)}  faces {len(b.faces)}"
          f"  (hdr faces {len(b.faces_hdr) if b.faces_hdr else 0})")
    print(f"texinfo {len(b.texinfo)}  texdata {len(b.texdata)}  models {len(b.models)}")
    print(f"nodes {len(b.nodes)}  leafs {len(b.leafs)}  clusters {b.num_clusters()}")
    print(f"brushes {len(b.brushes)}  brushsides {len(b.brushsides)}")
    print(f"dispinfo {len(b.dispinfo)}  disp verts {len(b.disp_verts)}")
    print(f"lighting LDR {len(b.lighting)} bytes  HDR {len(b.lighting_hdr)} bytes")

    world = b.models[0]
    skip_flags = SURF_NODRAW | SURF_SKIP | SURF_SKY | SURF_SKY2D | SURF_HINT | SURF_TRIGGER
    drawn = 0
    tris = 0
    disp = 0
    by_tex = collections.Counter()
    for fi in range(world.firstface, world.firstface + world.numfaces):
        f = b.faces[fi]
        ti = b.texinfo[f.texinfo] if f.texinfo >= 0 else None
        if ti is None or ti.flags & skip_flags:
            continue
        name = b.texdata[ti.texdata].name
        if name.lower().startswith("tools/"):
            continue
        drawn += 1
        if f.dispinfo >= 0:
            disp += 1
            power = b.dispinfo[f.dispinfo].power
            tris += 2 * (1 << power) ** 2
        else:
            tris += f.numedges - 2
        by_tex[name] += 1
    print(f"world: {world.numfaces} faces, {drawn} drawable ({disp} displacements), ~{tris} tris before tessellation")
    print(f"world bounds {tuple(round(v) for v in world.mins)} .. {tuple(round(v) for v in world.maxs)}")
    print(f"{len(by_tex)} materials used by world; most common:")
    for name, n in by_tex.most_common(args.top):
        print(f"  {n:5d}  {name}")

    pak = b.pakfile()
    if pak:
        names = pak.namelist()
        print(f"pakfile: {len(names)} files")

    ents = b.entities()
    classes = collections.Counter(e.get("classname", "?") for e in ents)
    print(f"entities: {len(ents)}; " + ", ".join(f"{c} {n}" for c, n in classes.most_common(12)))
    gl = b.game_lumps()
    print("game lumps: " + ", ".join(f"{k} v{v[0]} {len(v[2])}B" for k, v in gl.items()))


def cmd_convert(args):
    import os

    from .convert import convert_map

    os.makedirs(args.out, exist_ok=True)
    from .goldsrc import is_goldsrc

    base = os.path.splitext(os.path.basename(args.bsp))[0]
    if is_goldsrc(args.bsp):
        base = "hl1_" + base  # Half-Life's maps share names with Half-Life: Source's
    tiers = ["high", "low"] if args.tier == "both" else [args.tier]
    for t in tiers:
        suffix = "" if t == "high" else ".low"
        out = os.path.join(args.out, f"{base}{suffix}.pmap")
        print(f"{base} [{t}]")
        convert_map(args.bsp, args.gmod, out, t)
    from .nav import nav_for_map

    print(nav_for_map(args.bsp, args.out))


def cmd_nav(args):
    """NPC navigation graphs for already converted maps (the maps list, or one BSP)."""
    from multiprocessing import Pool

    from .nav import nav_for_map

    if args.bsp:
        paths = [args.bsp]
    else:
        paths = []
        with open(args.maps) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    paths.append(line)
    with Pool(args.jobs) as pool:
        for msg in pool.starmap(nav_for_map, [(p, args.out) for p in paths]):
            print(msg, flush=True)


def cmd_props(args):
    import os

    from .props import build_pack, read_prop_list

    from .npcbuild import GAME_DIRS
    from .steam import game_dir
    from .vpk import GameFS

    os.makedirs(args.out, exist_ok=True)
    plist = read_prop_list(args.list)
    tiers = ["high", "low"] if args.tier == "both" else [args.tier]
    for t in tiers:
        suffix = "" if t == "high" else ".low"
        print(f"props [{t}]")
        # TF2's weapons (what its mercenaries hold) come from TF2, searched only after GMod's own files
        fs = GameFS(args.gmod, fallback_dirs=[game_dir(d) for d in GAME_DIRS["tf"]])
        build_pack(plist, args.gmod, os.path.join(args.out, f"props{suffix}.ppak"), t, fs=fs)


def cmd_sounds(args):
    import os

    from .sounds import build_pack, read_sound_list

    os.makedirs(args.out, exist_ok=True)
    slist = read_sound_list(args.list)
    # Full detail matches the PSP's 44.1 kHz output; the 32MB PSP-1000 gets a smaller pack
    for tier, rate in (("high", 44100), ("low", 16000)):
        if args.tier not in (tier, "both"):
            continue
        suffix = "" if tier == "high" else ".low"
        print(f"sounds [{tier}]")
        build_pack(slist, args.gmod, os.path.join(args.out, f"sounds{suffix}.psnd"), rate)


def cmd_npcs(args):
    import os

    from .npcbuild import build_npcs, read_npc_list

    os.makedirs(args.out, exist_ok=True)
    nlist = read_npc_list(args.list)
    for t in (["high", "low", "ps2"] if args.tier == "both" else [args.tier]):
        print(f"npcs [{t}]")
        build_npcs(nlist, args.gmod, os.path.join(args.out, f"npcs{'' if t == 'high' else '.' + t}.pnpc"), t)


def cmd_vmodels(args):
    import os

    from .vmbuild import build_from_list

    os.makedirs(args.out, exist_ok=True)
    build_from_list(args.list, args.gmod, args.out, ["high", "low"] if args.tier == "both" else [args.tier])


def cmd_extras(args):
    import os

    from .extras import build_extras

    os.makedirs(args.out, exist_ok=True)
    build_extras(args.gmod, args.props, args.out, workers=args.workers)


def cmd_ui(args):
    import os

    from .props import read_prop_list
    from .ui import build_ui

    props = [(m, label) for m, label, _s, flags in read_prop_list(args.props) if not flags & 0x100]
    if os.path.exists(args.npcs):
        from .npcbuild import read_npc_list

        props += [(m, label, o.get("game", ""), int(o.get("skin", "0") or 0)) for m, label, o in read_npc_list(args.npcs)]
    # Thumbnails for the maps that have been converted (as the game names them: hl1_<name> for Half-Life's)
    maps = []
    for name in sorted(os.listdir(os.path.join(args.out, "maps")) if os.path.isdir(os.path.join(args.out, "maps")) else []):
        stem = name.split(".")[0]
        if name.endswith(".pmap") and stem not in maps:
            maps.append(stem)
    print("ui")
    build_ui(args.gmod, props, maps, os.path.join(args.out, "ui.pak"))


def main(argv=None):
    p = argparse.ArgumentParser(prog="bspconv")
    sub = p.add_subparsers(dest="cmd", required=True)
    pi = sub.add_parser("info", help="print statistics about a BSP")
    pi.add_argument("bsp")
    pi.add_argument("--top", type=int, default=15)
    pi.set_defaults(func=cmd_info)

    pc = sub.add_parser("convert", help="convert a BSP to .pmap for the PSP")
    pc.add_argument("bsp")
    pc.add_argument("-o", "--out", default="../psp/data/maps")
    pc.add_argument("--tier", choices=["high", "low", "both"], default="both")
    pc.add_argument("--gmod", default=DEFAULT_GMOD, help="GarrysMod install folder (as seen from WSL)")
    pc.set_defaults(func=cmd_convert)

    pg = sub.add_parser("nav", help="build NPC navigation graphs (.pnav) for converted maps, from their info_nodes")
    pg.add_argument("bsp", nargs="?", help="one map (default: every map in --maps)")
    pg.add_argument("--maps", default="maps.txt")
    pg.add_argument("-o", "--out", default="../psp/data/maps")
    pg.add_argument("--jobs", type=int, default=None, help="maps at once (default: every CPU core)")
    pg.set_defaults(func=cmd_nav)

    pp = sub.add_parser("props", help="build the spawnable prop pack from a list of models")
    pp.add_argument("--list", default="props.txt")
    pp.add_argument("-o", "--out", default="../psp/data")
    pp.add_argument("--tier", choices=["high", "low", "both"], default="both")
    pp.add_argument("--gmod", default=DEFAULT_GMOD)
    pp.set_defaults(func=cmd_props)

    ps = sub.add_parser("sounds", help="build the sound pack from a list of .wav files")
    ps.add_argument("--list", default="sounds.txt")
    ps.add_argument("-o", "--out", default="../psp/data")
    ps.add_argument("--tier", choices=["high", "low", "both"], default="both")
    ps.add_argument("--gmod", default=DEFAULT_GMOD)
    ps.set_defaults(func=cmd_sounds)

    pn = sub.add_parser("npcs", help="build the NPC pack (skinned models with baked animations)")
    pn.add_argument("--list", default="npcs.txt")
    pn.add_argument("-o", "--out", default="../psp/data")
    pn.add_argument("--tier", choices=["high", "low", "ps2", "both"], default="both")
    pn.add_argument("--gmod", default=DEFAULT_GMOD)
    pn.set_defaults(func=cmd_npcs)

    pv = sub.add_parser("vmodels", help="build the animated first-person weapons (props.txt's vm models)")
    pv.add_argument("--list", default="props.txt")
    pv.add_argument("-o", "--out", default="../psp/data")
    pv.add_argument("--tier", choices=["high", "low", "both"], default="both")
    pv.add_argument("--gmod", default=DEFAULT_GMOD)
    pv.set_defaults(func=cmd_vmodels)

    px = sub.add_parser("extras", help="build the extra props (thousands of models from the Source games, loaded on demand)")
    px.add_argument("--props", default="props.txt", help="the core prop list (its models are left out)")
    px.add_argument("-o", "--out", default="../psp/data")
    px.add_argument("--workers", type=int, default=None)
    px.add_argument("--gmod", default=DEFAULT_GMOD)
    px.set_defaults(func=cmd_extras)

    pu = sub.add_parser("ui", help="build the UI pack: fonts, spawn icons, map thumbnails, logo")
    pu.add_argument("--props", default="props.txt")
    pu.add_argument("--maps", default="maps.txt")
    pu.add_argument("--npcs", default="npcs.txt")
    pu.add_argument("-o", "--out", default="../psp/data")
    pu.add_argument("--gmod", default=DEFAULT_GMOD)
    pu.set_defaults(func=cmd_ui)

    args = p.parse_args(argv)
    import os

    # Without the game files every pack comes out empty and would overwrite good data
    gmod = getattr(args, "gmod", None)
    if gmod and not os.path.isdir(os.path.join(gmod, "garrysmod")):
        sys.exit(f"GarrysMod not found at {gmod} (is the drive mounted? sudo mount -t drvfs D: /mnt/d)")
    args.func(args)


if __name__ == "__main__":
    sys.exit(main())
