"""Happy Burger's own game data, made from scratch: low-poly props, characters, the burger joint map,
sounds and the UI. Nothing comes from another game. Run: .venv/bin/python -m tools.burgerkit [out dir]

It writes the same files Retcon's Mod's converter (tools/bspconv) makes from Valve's games, in the same
formats, so the engine loads them unchanged: maps/burgerjoint.pmap, props.ppak, npcs.pnpc, sounds.psnd and
ui.pak. The formats' writers (pmap.PmapWriter, pspimage) are shared with bspconv.
"""
