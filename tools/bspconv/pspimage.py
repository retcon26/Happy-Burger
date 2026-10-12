"""Turns RGBA images into PSP-native paletted textures: T8 (8-bit indexed), swizzled, with mip levels."""

import numpy as np
from PIL import Image

MIN_W = 16  # swizzle works on 16-byte x 8-row blocks, so a T8 level must be >= 16x8
MIN_H = 8
MAX_LEVELS = 5


class PspTexture:
    def __init__(self, width, height, clut, levels):
        self.width = width
        self.height = height
        self.clut = clut      # 256 * 4 bytes, RGBA (the PSP's 8888 byte order)
        self.levels = levels  # list of swizzled index bytes, largest first


def _pow2_floor(v):
    p = 1
    while p * 2 <= v:
        p *= 2
    return p


def swizzle(indices):
    """Reorders a 2D uint8 array into the PSP's 16x8 block layout, which the GE reads faster."""
    h, w = indices.shape
    return indices.reshape(h // 8, 8, w // 16, 16).transpose(0, 2, 1, 3).tobytes()


def _nearest(pixels, palette):
    """Index of the closest palette entry for each RGBA pixel. pixels (N,4), palette (K,4)."""
    p = pixels.astype(np.int32)
    pal = palette.astype(np.int32)
    out = np.empty(len(p), dtype=np.uint8)
    step = 8192
    for i in range(0, len(p), step):
        d = ((p[i : i + step, None, :] - pal[None, :, :]) ** 2).sum(axis=2)
        out[i : i + step] = d.argmin(axis=1)
    return out


def convert(img, max_dim, keep_alpha, max_levels=MAX_LEVELS):
    """RGBA PIL image -> PspTexture no larger than max_dim, with power-of-two sides."""
    w = max(MIN_W, min(max_dim, _pow2_floor(img.width)))
    h = max(MIN_H, min(max_dim, _pow2_floor(img.height)))
    base = img.convert("RGBA")
    if base.size != (w, h):
        base = base.resize((w, h), Image.Resampling.LANCZOS)
    if not keep_alpha:
        # Source often hides specular masks in alpha. Drop it so opaque surfaces stay opaque.
        base.putalpha(255)

    if keep_alpha:
        q = base.quantize(256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
        pal = np.array(q.getpalette("RGBA"), dtype=np.uint8).reshape(-1, 4)
    else:
        q = base.convert("RGB").quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        pal_rgb = np.array(q.getpalette("RGB"), dtype=np.uint8).reshape(-1, 3)
        pal = np.concatenate([pal_rgb, np.full((len(pal_rgb), 1), 255, np.uint8)], axis=1)
    clut = np.zeros((256, 4), dtype=np.uint8)
    clut[: min(256, len(pal))] = pal[:256]

    levels = [swizzle(np.array(q, dtype=np.uint8))]
    lw, lh = w, h
    while len(levels) < max_levels and lw // 2 >= MIN_W and lh // 2 >= MIN_H:
        lw, lh = lw // 2, lh // 2
        small = base.resize((lw, lh), Image.Resampling.BOX)
        idx = _nearest(np.array(small, dtype=np.uint8).reshape(-1, 4), clut).reshape(lh, lw)
        levels.append(swizzle(idx))
    return PspTexture(w, h, clut.tobytes(), levels)


def solid(rgb, alpha=255):
    """16x8 single-colour texture: the fallback for missing textures, and the water surface colour."""
    img = Image.new("RGBA", (MIN_W, MIN_H), tuple(int(c) for c in rgb) + (int(alpha),))
    return convert(img, MIN_W, alpha < 255)
