"""Garry's Mod addon archives (.gma), as the Workshop downloads them.

Newer items are plain .gma files; older ("legacy") ones are LZMA-compressed .gma in a .bin.
Layout: "GMAD", version byte, SteamID (u64), timestamp (u64), [required content strings, ""], name,
description, author (C strings), addon version (i32), then file entries {u32 number (0 ends);
name; i64 size; u32 crc}, then the files' data back to back.
"""

import lzma
import os
import struct


def _cstr(data, pos):
    end = data.index(b"\0", pos)
    return data[pos:end].decode("utf-8", "replace"), end + 1


def read_gma(path):
    """(title, {lower-case path: bytes}) for a .gma or a legacy .bin, or None if it isn't one."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"GMAD"):
        # The Workshop's legacy items are .lzma ("alone") streams; some have a few bytes after the
        # data that lzma.decompress() rejects, so the stream is read with a decompressor that stops at its end
        try:
            data = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(data)
        except lzma.LZMAError:
            return None
        if not data.startswith(b"GMAD"):
            return None
    version = data[4]
    pos = 5 + 16
    if version > 1:
        while True:
            s, pos = _cstr(data, pos)
            if not s:
                break
    title, pos = _cstr(data, pos)
    _desc, pos = _cstr(data, pos)
    _author, pos = _cstr(data, pos)
    pos += 4
    entries = []
    while True:
        (num,) = struct.unpack_from("<I", data, pos)
        pos += 4
        if num == 0:
            break
        name, pos = _cstr(data, pos)
        size, _crc = struct.unpack_from("<qI", data, pos)
        pos += 12
        entries.append((name, size))
    files = {}
    for name, size in entries:
        files[name.lower().replace("\\", "/")] = data[pos:pos + size]
        pos += size
    return title, files


def workshop_items(workshop_dir):
    """(item id, archive path) for each downloaded GMod Workshop item."""
    out = []
    if not os.path.isdir(workshop_dir):
        return out
    for item in sorted(os.listdir(workshop_dir)):
        d = os.path.join(workshop_dir, item)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if name.endswith(".gma") or name.endswith(".bin"):
                out.append((item, os.path.join(d, name)))
    return out


CACHE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "workshop_cache")


def workshop_dirs():
    """The unpacked folders of your Workshop addons that have models (unpacked into tools/workshop_cache
    the first time), to search like a game's folder. Addons with no models (maps...) are only noted."""
    from .steam import workshop_dir

    out = []
    for item, path in workshop_items(workshop_dir()):
        d = os.path.join(CACHE, item)
        marker = os.path.join(d, ".extracted")
        if not os.path.exists(marker):
            got = read_gma(path)
            if got is None:
                continue
            title, files = got
            os.makedirs(d, exist_ok=True)
            if any(n.endswith(".mdl") for n in files):
                for name, blob in files.items():
                    dst = os.path.join(d, *name.split("/"))
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    with open(dst, "wb") as f:
                        f.write(blob)
            with open(marker, "w") as f:
                f.write(title)
        if os.path.isdir(os.path.join(d, "models")):
            out.append(d)
    return out
