"""Where the Steam games are: each can be in any Steam library (Team Fortress 2 on C:, the rest on D:...)."""

import os

LIBRARIES = [
    "/mnt/d/SteamLibrary/steamapps/common/",
    "/mnt/c/Program Files (x86)/Steam/steamapps/common/",
]


def game_dir(rel):
    """The full path of a game folder ("Team Fortress 2/tf") in whichever library has it (the first
    library's path if none does, so callers' "is it installed" checks just fail)."""
    for lib in LIBRARIES:
        if os.path.isdir(lib + rel):
            return lib + rel
    return LIBRARIES[0] + rel


def workshop_dir():
    """Where Steam keeps Garry's Mod's Workshop downloads (app 4000), in the library GMod is in."""
    for lib in LIBRARIES:
        if os.path.isdir(lib + "GarrysMod"):
            return os.path.normpath(lib + "../workshop/content/4000")
    return os.path.normpath(LIBRARIES[0] + "../workshop/content/4000")
