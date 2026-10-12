"""Writer for .pmap, the PSP runtime map format. The C side is psp/src/pmap_format.h. Keep the two in sync.

The PSP loads the whole file into one buffer and uses the sections in place, so every section is
16-byte aligned and all offsets are absolute file offsets. Everything is little-endian, like the PSP.
"""

import struct

PMAP_MAGIC = b"PMAP"
PMAP_VERSION = 5
MAX_TEX_LEVELS = 5

BATCH_ALPHATEST = 1
BATCH_TRANSLUCENT = 2
BATCH_NOCULL = 4


def _align(n, a=16):
    return (n + a - 1) & ~(a - 1)


class PmapWriter:
    """Sectioned container writer, also used for prop packs (magic "PPAK")."""

    def __init__(self, magic=PMAP_MAGIC, version=PMAP_VERSION):
        self.magic = magic
        self.version = version
        self.sections = []  # (id, bytes, count)

    def add(self, sid, data, count):
        assert len(sid) == 4
        self.sections.append((sid.encode("ascii"), bytes(data), count))

    def replace(self, sid, data):
        """Swaps a section's bytes for same-sized ones, e.g. to patch in offsets known only at the end."""
        key = sid.encode("ascii")
        for i, (s, old, count) in enumerate(self.sections):
            if s == key:
                assert len(old) == len(data)
                self.sections[i] = (s, bytes(data), count)
                return
        raise KeyError(sid)

    def write(self, path):
        header_size = 16 + 16 * len(self.sections)
        ofs = _align(header_size)
        table = []
        blobs = []
        for sid, data, count in self.sections:
            table.append((sid, ofs, len(data), count))
            blobs.append((ofs, data))
            ofs = _align(ofs + len(data))
        out = bytearray(ofs)
        struct.pack_into("<4sIII", out, 0, self.magic, self.version, len(self.sections), 0)
        for i, (sid, o, size, count) in enumerate(table):
            struct.pack_into("<4sIII", out, 16 + i * 16, sid, o, size, count)
        for o, data in blobs:
            out[o : o + len(data)] = data
        with open(path, "wb") as f:
            f.write(out)
        return len(out)

    def section_offset(self, sid):
        """Absolute offset a section will land at. Only valid once every section has been added."""
        header_size = 16 + 16 * len(self.sections)
        ofs = _align(header_size)
        for s, data, _count in self.sections:
            if s == sid.encode("ascii"):
                return ofs
            ofs = _align(ofs + len(data))
        raise KeyError(sid)
