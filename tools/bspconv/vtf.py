"""Reader for Valve Texture Format (.vtf) files. It decodes just one mip level, as a PIL image."""

import struct

import numpy as np
from PIL import Image

FMT_RGBA8888 = 0
FMT_ABGR8888 = 1
FMT_RGB888 = 2
FMT_BGR888 = 3
FMT_RGB565 = 4
FMT_I8 = 5
FMT_IA88 = 6
FMT_A8 = 8
FMT_RGB888_BLUESCREEN = 9
FMT_BGR888_BLUESCREEN = 10
FMT_ARGB8888 = 11
FMT_BGRA8888 = 12
FMT_DXT1 = 13
FMT_DXT3 = 14
FMT_DXT5 = 15
FMT_BGRX8888 = 16
FMT_BGR565 = 17
FMT_BGRX5551 = 18
FMT_BGRA4444 = 19
FMT_DXT1_ONEBITALPHA = 20
FMT_BGRA5551 = 21
FMT_RGBA16161616F = 24

_BPP = {
    FMT_RGBA8888: 4, FMT_ABGR8888: 4, FMT_RGB888: 3, FMT_BGR888: 3, FMT_RGB565: 2, FMT_I8: 1, FMT_IA88: 2,
    FMT_A8: 1, FMT_RGB888_BLUESCREEN: 3, FMT_BGR888_BLUESCREEN: 3, FMT_ARGB8888: 4, FMT_BGRA8888: 4,
    FMT_BGRX8888: 4, FMT_BGR565: 2, FMT_BGRX5551: 2, FMT_BGRA4444: 2, FMT_BGRA5551: 2, FMT_RGBA16161616F: 8,
}
_DXT_BLOCK = {FMT_DXT1: 8, FMT_DXT1_ONEBITALPHA: 8, FMT_DXT3: 16, FMT_DXT5: 16}

# 7.3+ resource tags
RSRC_HIGH_RES = b"\x30\x00\x00"

TEXTUREFLAGS_ENVMAP = 0x4000


def _mip_size(fmt, w, h):
    if fmt in _DXT_BLOCK:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * _DXT_BLOCK[fmt]
    if fmt not in _BPP:
        raise ValueError(f"unsupported VTF format {fmt}")
    return w * h * _BPP[fmt]


class Vtf:
    def __init__(self, data):
        if data[:4] != b"VTF\0":
            raise ValueError("not a VTF file")
        self.data = data
        major, minor, header_size = struct.unpack_from("<3I", data, 4)
        self.version = (major, minor)
        self.width, self.height, self.flags, self.frames, self.first_frame = struct.unpack_from("<HHIHH", data, 16)
        self.reflectivity = struct.unpack_from("<3f", data, 32)
        self.format = struct.unpack_from("<i", data, 52)[0]
        self.mip_count = data[56]
        self.low_format = struct.unpack_from("<i", data, 57)[0]
        self.low_w, self.low_h = data[61], data[62]
        self.depth = struct.unpack_from("<H", data, 63)[0] if minor >= 2 else 1
        self.depth = max(1, self.depth)
        self.faces = 1
        if self.flags & TEXTUREFLAGS_ENVMAP:
            # Cubemaps carry 6 faces, or 7 (with a sphere map) in files before 7.5.
            self.faces = 6 if (minor >= 5 or self.first_frame == 0xFFFF) else 7

        self.high_offset = None
        if minor >= 3:
            num_res = struct.unpack_from("<I", data, 68)[0]
            for i in range(num_res):
                tag = data[80 + i * 8 : 83 + i * 8]
                ofs = struct.unpack_from("<I", data, 84 + i * 8)[0]
                if tag == RSRC_HIGH_RES:
                    self.high_offset = ofs
        if self.high_offset is None:
            low = _mip_size(self.low_format, self.low_w, self.low_h) if self.low_format >= 0 and self.low_w else 0
            self.high_offset = header_size + low

    def mip_dims(self, level):
        return max(1, self.width >> level), max(1, self.height >> level)

    def pick_level(self, max_dim):
        """Smallest mip index whose larger side fits within max_dim."""
        for level in range(self.mip_count):
            w, h = self.mip_dims(level)
            if max(w, h) <= max_dim:
                return level
        return self.mip_count - 1

    def _level_offset(self, level):
        # Mips are stored smallest first. Each holds frames x faces x depth slices.
        ofs = self.high_offset
        per = self.frames * self.faces * self.depth
        for m in range(self.mip_count - 1, level, -1):
            w, h = self.mip_dims(m)
            ofs += _mip_size(self.format, w, h) * per
        return ofs

    def decode(self, level=0, frame=0):
        """RGBA PIL image of one mip level (frame 0, face 0, slice 0)."""
        w, h = self.mip_dims(level)
        size = _mip_size(self.format, w, h)
        ofs = self._level_offset(level) + frame * self.faces * self.depth * size
        raw = self.data[ofs : ofs + size]
        if len(raw) < size:
            raise ValueError("VTF truncated")
        return _decode(self.format, raw, w, h)


def _decode(fmt, raw, w, h):
    if fmt in _DXT_BLOCK:
        n = {FMT_DXT1: 1, FMT_DXT1_ONEBITALPHA: 1, FMT_DXT3: 2, FMT_DXT5: 3}[fmt]
        bw, bh = max(4, (w + 3) // 4 * 4), max(4, (h + 3) // 4 * 4)
        img = Image.frombytes("RGBA", (bw, bh), raw, "bcn", n)
        return img.crop((0, 0, w, h)) if (bw, bh) != (w, h) else img

    if fmt == FMT_RGBA8888:
        return Image.frombytes("RGBA", (w, h), raw)
    if fmt == FMT_ABGR8888:
        return Image.frombytes("RGBA", (w, h), raw, "raw", "ABGR")
    if fmt == FMT_ARGB8888:
        return Image.frombytes("RGBA", (w, h), raw, "raw", "ARGB")
    if fmt == FMT_BGRA8888:
        return Image.frombytes("RGBA", (w, h), raw, "raw", "BGRA")
    if fmt == FMT_BGRX8888:
        return Image.frombytes("RGB", (w, h), raw, "raw", "BGRX").convert("RGBA")
    if fmt in (FMT_RGB888, FMT_RGB888_BLUESCREEN):
        return Image.frombytes("RGB", (w, h), raw).convert("RGBA")
    if fmt in (FMT_BGR888, FMT_BGR888_BLUESCREEN):
        return Image.frombytes("RGB", (w, h), raw, "raw", "BGR").convert("RGBA")
    if fmt == FMT_I8:
        return Image.frombytes("L", (w, h), raw).convert("RGBA")
    if fmt == FMT_IA88:
        return Image.frombytes("LA", (w, h), raw).convert("RGBA")
    if fmt == FMT_A8:
        a = Image.frombytes("L", (w, h), raw)
        img = Image.new("RGBA", (w, h), (255, 255, 255, 255))
        img.putalpha(a)
        return img

    px = np.frombuffer(raw, dtype="<u2").reshape(h, w).astype(np.uint32)
    if fmt in (FMT_RGB565, FMT_BGR565):
        hi = ((px >> 11) & 31) * 255 // 31
        mid = ((px >> 5) & 63) * 255 // 63
        lo = (px & 31) * 255 // 31
        r, b = (lo, hi) if fmt == FMT_RGB565 else (hi, lo)
        a = np.full_like(px, 255)
        return _from_channels(r, mid, b, a)
    if fmt in (FMT_BGRX5551, FMT_BGRA5551):
        b = (px & 31) * 255 // 31
        g = ((px >> 5) & 31) * 255 // 31
        r = ((px >> 10) & 31) * 255 // 31
        a = np.where(px >> 15, 255, 0) if fmt == FMT_BGRA5551 else np.full_like(px, 255)
        return _from_channels(r, g, b, a)
    if fmt == FMT_BGRA4444:
        b = (px & 15) * 17
        g = ((px >> 4) & 15) * 17
        r = ((px >> 8) & 15) * 17
        a = ((px >> 12) & 15) * 17
        return _from_channels(r, g, b, a)
    if fmt == FMT_RGBA16161616F:
        f = np.frombuffer(raw, dtype="<f2").reshape(h, w, 4).astype(np.float32)
        f = np.clip(f, 0.0, 1.0) ** (1.0 / 2.2) * 255.0
        return Image.fromarray(f.astype(np.uint8), "RGBA")
    raise ValueError(f"unsupported VTF format {fmt}")


def _from_channels(r, g, b, a):
    arr = np.stack([r, g, b, a], axis=-1).astype(np.uint8)
    return Image.fromarray(arr, "RGBA")
