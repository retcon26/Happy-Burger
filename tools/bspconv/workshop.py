"""Maps from your Garry's Mod Workshop downloads, converted like any other map.

Each Workshop addon with maps in it (maps/*.bsp in its .gma) is unpacked once into tools/workshop_cache/<item>,
and its maps are converted into the game's maps folder. A map finds its textures and models the way GMod does:
its own pakfile, its addon, your other Workshop addons (content packs), GMod's files, then the games GMod mounts
(Counter-Strike: Source, the HL2 episodes, Team Fortress 2, Half-Life: Source) where you have them.

The game lists them under Workshop: the maps folder gets workshop.txt, a line per map ("name|addon title|item").

  tools/bspconv.sh workshop            convert the Workshop maps not converted yet
  tools/bspconv.sh workshop --list     just list them
  tools/bspconv.sh workshop --all      convert them all again
"""

import os

from .gma import CACHE, read_gma, workshop_items

LIST_NAME = "workshop.txt"
MAX_NAME = 26  # the game keeps map names in 32 bytes, with ".low" after them
MOUNTED = ["Counter-Strike Source/cstrike", "Half-Life 2/episodic", "Half-Life 2/ep2", "Team Fortress 2/tf",
           "Half-Life 2/hl1"]


def _scan(item, path):
    """(title, [bsp names]) for an addon, unpacking it into the cache if it has maps. What it found is
    noted in the cache (.maps), and looked at again only when Steam updates the addon."""
    d = os.path.join(CACHE, item)
    note = os.path.join(d, ".maps")
    if os.path.exists(note) and os.path.getmtime(note) >= os.path.getmtime(path):
        with open(note, encoding="utf-8") as f:
            lines = f.read().splitlines()
        if lines:
            return lines[0], [n for n in lines[1:] if n]
    got = read_gma(path)
    if got is None:
        return None, []
    title, files = got
    bsps = sorted(n[5:-4] for n in files if n.startswith("maps/") and n.endswith(".bsp") and n.count("/") == 1)
    os.makedirs(d, exist_ok=True)
    if bsps:
        # The whole addon: the maps, and the materials and models they use
        for name, blob in files.items():
            dst = os.path.join(d, *name.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(blob)
        marker = os.path.join(d, ".extracted")
        if not os.path.exists(marker):
            with open(marker, "w", encoding="utf-8") as f:
                f.write(title)
    with open(note, "w", encoding="utf-8") as f:
        f.write("\n".join([title] + bsps) + "\n")
    return title, bsps


def find_maps(log=print):
    """[(name, bsp path, item, title)] for every map in your Workshop downloads."""
    from .steam import workshop_dir

    out = []
    for item, path in workshop_items(workshop_dir()):
        title, bsps = _scan(item, path)
        for name in bsps:
            out.append((name, os.path.join(CACHE, item, "maps", name + ".bsp"), item, title or item))
    return out


def content_dirs(item):
    """Where a Workshop map's files are looked for before GMod's own: its addon, then your other addons
    that have materials or models (GMod mounts them all)."""
    out = [os.path.join(CACHE, item)]
    if os.path.isdir(CACHE):
        for other in sorted(os.listdir(CACHE)):
            d = os.path.join(CACHE, other)
            if other != item and (os.path.isdir(os.path.join(d, "materials")) or os.path.isdir(os.path.join(d, "models"))):
                out.append(d)
    return out


def mounted_dirs():
    from .steam import game_dir

    return [p for p in (game_dir(g) for g in MOUNTED) if os.path.isdir(p)]


def convert_one(name, bsp, item, gmod, out_dir):
    """Both detail levels and the NPC graph for one Workshop map; a line for the log."""
    from .convert import convert_map
    from .nav import nav_for_map

    lines = []
    try:
        for tier, suffix in (("high", ""), ("low", ".low")):
            convert_map(bsp, gmod, os.path.join(out_dir, f"{name}{suffix}.pmap"), tier, log=lines.append,
                        content_dirs=content_dirs(item), fallback_dirs=mounted_dirs())
    except Exception as e:  # one map failing doesn't stop the rest
        return f"FAIL {name}: {type(e).__name__}: {e}"
    wrote = next((l for l in lines if "wrote" in l), "").strip()
    return f"OK   {name}  {wrote}  |  {nav_for_map(bsp, out_dir)}"


def stock_names(maps_txt):
    """The maps maps.txt converts: a Workshop map with the same name is left alone."""
    out = set()
    if os.path.exists(maps_txt):
        with open(maps_txt) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    base = os.path.splitext(os.path.basename(line))[0].lower()
                    out.add(("hl1_" + base) if "/Half-Life/" in line else base)
    return out


def write_list(maps, out_dir):
    """data/maps/workshop.txt: the Workshop maps that are converted, for the game's menus."""
    lines = []
    for name, _bsp, item, title in sorted(maps):
        if os.path.exists(os.path.join(out_dir, name + ".pmap")) or os.path.exists(os.path.join(out_dir, name + ".low.pmap")):
            lines.append(f"{name}|{title.replace('|', '/')}|{item}")
    path = os.path.join(out_dir, LIST_NAME)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Maps from the GMod Workshop (tools/bspconv.sh workshop): name|addon|item\n")
        f.write("\n".join(lines) + ("\n" if lines else ""))
    return len(lines)


def thumbnails():
    """{map name: its picture in its addon (maps/thumb/<name>.png)} for the UI pack."""
    out = {}
    if not os.path.isdir(CACHE):
        return out
    for item in os.listdir(CACHE):
        d = os.path.join(CACHE, item, "maps", "thumb")
        if os.path.isdir(d):
            for n in os.listdir(d):
                if n.lower().endswith(".png"):
                    out[n[:-4].lower()] = os.path.join(d, n)
    return out


def run(gmod, out_dir, maps_txt, list_only=False, all_maps=False, jobs=1, log=print):
    maps = find_maps(log)
    stock = stock_names(maps_txt)
    todo = []
    for name, bsp, item, title in maps:
        done = os.path.exists(os.path.join(out_dir, name + ".pmap")) and os.path.exists(os.path.join(out_dir, name + ".low.pmap"))
        size = os.path.getsize(bsp) // (1024 * 1024) if os.path.exists(bsp) else 0
        why = ""
        if len(name) > MAX_NAME:
            why = f"name longer than {MAX_NAME} characters"
        elif name in stock:
            why = "maps.txt has a map of that name"
        log(f"{name:28s} {size:4d} MB  {'converted' if done else '-':9s}  {title} ({item}){'  SKIPPED: ' + why if why else ''}")
        if not why and (all_maps or not done):
            todo.append((name, bsp, item))
    usable = [m for m in maps if len(m[0]) <= MAX_NAME and m[0] not in stock]
    if list_only:
        return
    os.makedirs(out_dir, exist_ok=True)
    log(f"{len(todo)} to convert ({jobs} at a time)")
    if jobs > 1:
        from multiprocessing import Pool

        with Pool(jobs) as pool:
            for msg in pool.starmap(convert_one, [(n, b, i, gmod, out_dir) for n, b, i in todo]):
                log(msg)
    else:
        for n, b, i in todo:
            log(convert_one(n, b, i, gmod, out_dir))
    log(f"{write_list(usable, out_dir)} Workshop maps listed in {os.path.join(out_dir, LIST_NAME)}")
