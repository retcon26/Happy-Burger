"""Reader for Valve VPK archives (v1 and v2), the packed asset files that ship with Source games."""

import os
import struct

VPK_SIGNATURE = 0x55AA1234
DIR_ARCHIVE = 0x7FFF


class VpkEntry:
    __slots__ = ("crc", "preload", "archive", "offset", "length")

    def __init__(self, crc, preload, archive, offset, length):
        self.crc = crc
        self.preload = preload
        self.archive = archive
        self.offset = offset
        self.length = length


class Vpk:
    """Read-only view of one `<name>_dir.vpk` and its numbered `<name>_NNN.vpk` data files."""

    def __init__(self, dir_path):
        self.dir_path = dir_path
        self.prefix = dir_path[: -len("_dir.vpk")] if dir_path.endswith("_dir.vpk") else None
        self.entries = {}
        self._parse()

    def _parse(self):
        with open(self.dir_path, "rb") as f:
            data = f.read()
        sig, version, tree_size = struct.unpack_from("<III", data, 0)
        if sig != VPK_SIGNATURE:
            raise ValueError(f"{self.dir_path}: not a VPK file")
        header_size = 12 if version == 1 else 28
        self.data_start = header_size + tree_size

        pos = header_size

        def read_str():
            nonlocal pos
            end = data.index(b"\0", pos)
            s = data[pos:end].decode("latin-1")
            pos = end + 1
            return s

        while True:
            ext = read_str()
            if not ext:
                break
            while True:
                path = read_str()
                if not path:
                    break
                while True:
                    name = read_str()
                    if not name:
                        break
                    crc, preload_len, archive, offset, length, term = struct.unpack_from("<IHHIIH", data, pos)
                    pos += 18
                    if term != 0xFFFF:
                        raise ValueError(f"{self.dir_path}: corrupt directory entry for {name}")
                    preload = data[pos : pos + preload_len]
                    pos += preload_len
                    full = f"{name}.{ext}" if path.strip() == "" else f"{path}/{name}.{ext}"
                    self.entries[full.lower()] = VpkEntry(crc, preload, archive, offset, length)

    def __contains__(self, path):
        return _norm(path) in self.entries

    def read(self, path):
        e = self.entries.get(_norm(path))
        if e is None:
            return None
        if e.length == 0:
            return e.preload
        if e.archive == DIR_ARCHIVE:
            archive_path, offset = self.dir_path, self.data_start + e.offset
        else:
            archive_path, offset = f"{self.prefix}_{e.archive:03d}.vpk", e.offset
        with open(archive_path, "rb") as f:
            f.seek(offset)
            return e.preload + f.read(e.length)


def _norm(path):
    return path.replace("\\", "/").lower().lstrip("/")


class GameFS:
    """Search path over loose files, VPKs and an optional BSP pakfile.

    The order roughly follows GMod's: the map's own pakfile first, then garrysmod, then mounted games.
    """

    def __init__(self, gmod_root, extra_dirs=(), fallback_dirs=()):
        """extra_dirs: other game folders (e.g. .../Counter-Strike Source/cstrike) to search first,
        loose files and their *_dir.vpk archives, for maps from games other than GMod.
        fallback_dirs: game folders searched only after GMod's own files (TF2's weapons for the prop pack)."""
        self.loose_roots = []
        self.vpks = []
        self.pak = None  # zipfile.ZipFile from the BSP, set per map

        def add_dir(d):
            if not os.path.isdir(d):
                return
            self.loose_roots.append(d)
            for name in sorted(os.listdir(d)):
                if name.endswith("_dir.vpk"):
                    try:
                        self.vpks.append(Vpk(os.path.join(d, name)))
                    except (OSError, ValueError):
                        pass

        for d in extra_dirs:
            add_dir(d)

        gm = os.path.join(gmod_root, "garrysmod")
        self.loose_roots.append(gm)
        for name in ("garrysmod_dir.vpk", "fallbacks_dir.vpk"):
            p = os.path.join(gm, name)
            if os.path.exists(p):
                self.vpks.append(Vpk(p))
        src = os.path.join(gmod_root, "sourceengine")
        if os.path.isdir(src):
            for name in sorted(os.listdir(src)):
                if name.endswith("_dir.vpk"):
                    self.vpks.append(Vpk(os.path.join(src, name)))
        for d in fallback_dirs:
            add_dir(d)

    def set_pak(self, zipf):
        self.pak = zipf
        self._pak_names = {n.lower(): n for n in zipf.namelist()} if zipf else {}

    def read(self, path):
        key = _norm(path)
        if self.pak is not None:
            real = self._pak_names.get(key)
            if real is not None:
                return self.pak.read(real)
        for root in self.loose_roots:
            p = os.path.join(root, *key.split("/"))
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    return f.read()
        for v in self.vpks:
            d = v.read(key)
            if d is not None:
                return d
        return None
