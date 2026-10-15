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
