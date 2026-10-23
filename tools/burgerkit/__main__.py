"""Builds all of Happy Burger's data: .venv/bin/python -m tools.burgerkit [data dir] [--wavs DIR]

The data dir defaults to psp/data, where make_release.ps1 and the PSP build look for it. --wavs also
writes every sound as a .wav, to listen to.
"""

import os
import sys
import time

from . import art, audio, level, people, props, ui


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    wavs = argv[argv.index("--wavs") + 1] if "--wavs" in argv else None
    args = [a for a in argv if not a.startswith("--") and a != wavs]
    out = args[0] if args else os.path.join(root, "psp", "data")
    os.makedirs(os.path.join(out, "maps"), exist_ok=True)
    t0 = time.time()
    print("Atlas")
    atlas = art.build_atlas()
    print("Props")
    props.build(atlas, os.path.join(out, "props.ppak"))
    print("Characters")
    people.build(atlas, os.path.join(out, "npcs.pnpc"))
    print("Map")
    level.build(os.path.join(out, "maps", level.MAP_NAME + ".pmap"))
    print("Sounds")
    audio.build(os.path.join(out, "sounds.psnd"))
    print("UI")
    ui.build(os.path.join(out, "ui.pak"))
    ui.build_xmb(os.path.join(root, "psp"))
    if wavs:
        audio.preview_wavs(wavs)
        print(f"  sounds as .wav in {wavs}")
    print(f"Done in {time.time() - t0:.1f}s: {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
