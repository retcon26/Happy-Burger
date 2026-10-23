"""data/ui.pak: the fonts (Pillow's Aileron, public domain), the logo and the decals (ketchup splats), in
the layout psp/src/ui.c reads (see tools/bspconv/ui.py, which this follows). Also the XMB's icons.
"""

import struct

import numpy as np
from PIL import Image, ImageDraw

from ..bspconv import pspimage
from ..bspconv.pmap import PmapWriter
from . import art

PUI_MAGIC = b"PUI "
PUI_VERSION = 1
# psp/src/ui.h FONT_*: small, bold, large, title
FONTS = [(11, False), (12, True), (18, True), (26, True)]


def _pow2(v):
    p = 16
    while p < v:
        p *= 2
    return p


def build_font(size, bold):
    f, stroke = art.font(size, bold)
    ascent, descent = f.getmetrics()
    placed, glyphs = [], []
    x = y = row_h = 0
    width = 256
    for code in range(32, 127):
        ch = chr(code)
        bbox = f.getbbox(ch, stroke_width=stroke)
        adv = int(round(f.getlength(ch))) + stroke
        if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            placed.append((ch, None, 0, 0, 0, 0, adv))
            continue
        w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
        if x + w + 1 > width:
            x, y, row_h = 0, y + row_h + 1, 0
        placed.append((ch, bbox, x, y, w, h, adv))
        x += w + 1
        row_h = max(row_h, h)
    height = _pow2(y + row_h + 1)
    atlas = Image.new("L", (width, height), 0)
    for ch, bbox, gx, gy, w, h, adv in placed:
        if bbox is None:
            glyphs.append((0, 0, 0, 0, 0, 0, adv))
            continue
        g = Image.new("L", (w, h), 0)
        ImageDraw.Draw(g).text((-bbox[0], -bbox[1]), ch, font=f, fill=255, stroke_width=stroke, stroke_fill=255)
        atlas.paste(g, (gx, gy))
        glyphs.append((gx, gy, w, h, bbox[0], bbox[1], adv))
    data = pspimage.swizzle(np.array(atlas, dtype=np.uint8))
    return width, height, ascent + descent, ascent, data, glyphs


def _fit_pow2(img, max_w, max_h):
    s = min(max_w / img.width, max_h / img.height, 1.0)
    w, h = max(1, int(img.width * s)), max(1, int(img.height * s))
    img = img.resize((w, h), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (_pow2(w), max(8, _pow2(h))), (0, 0, 0, 0))
    canvas.paste(img, (0, 0))
    return canvas, w, h


def build(out_path, log=print):
    data = bytearray()

    def blob(b):
        nonlocal data
        data += b"\0" * ((-len(data)) % 16)
        o = len(data)
        data += b
        return o

    font_recs, glyph_bytes = [], bytearray()
    for size, bold in FONTS:
        w, h, line, ascent, atlas, glyphs = build_font(size, bold)
        font_recs.append((w, h, line, ascent, blob(atlas), len(glyph_bytes)))
        for g in glyphs:
            glyph_bytes += struct.pack("<2H2B2bB3x", *g)
    images = []

    def add_image(name, img, max_w, max_h, alpha=True):
        canvas, w, h = _fit_pow2(img, max_w, max_h)
        pt = pspimage.convert(canvas, max(canvas.width, canvas.height), alpha, max_levels=1)
        images.append((name, pt.width, pt.height, w, h, blob(pt.clut), blob(pt.levels[0])))

    logo = art.logo_image()
    add_image("logo", logo.crop(logo.getbbox()), 256, 64)  # just the drawing, so it centres
    # game.c's blood decals: ketchup, as befits a burger joint
    for k in range(4):
        add_image(f"decal_blood{k + 1}", art.decal_ketchup(k), 64, 64, alpha=False)
    w = PmapWriter(PUI_MAGIC, PUI_VERSION)
    w.add("FNTS", bytes(16 * len(font_recs)), len(font_recs))
    w.add("GLYP", glyph_bytes, len(glyph_bytes) // 12)
    w.add("IMGS", bytes(64 * len(images)), len(images))
    w.add("DATA", data, len(data))
    base = w.section_offset("DATA")
    gbase = w.section_offset("GLYP")
    w.replace("FNTS", b"".join(struct.pack("<2H2BH2I", fw, fh, line, asc, 0, base + ao, gbase + go)
                               for fw, fh, line, asc, ao, go in font_recs))
    w.replace("IMGS", b"".join(struct.pack("<48s4H2I", n.encode()[:47], tw, th, iw, ih, base + co, base + do)
                               for n, tw, th, iw, ih, co, do in images))
    size = w.write(out_path)
    log(f"  wrote {out_path}: {len(FONTS)} fonts, {len(images)} images, {size / 1024:.0f} KB")


def build_xmb(psp_dir, log=print):
    art.xmb_icon().save(f"{psp_dir}/ICON0.PNG")
    art.xmb_background().convert("RGB").save(f"{psp_dir}/PIC1.PNG")
    log(f"  wrote {psp_dir}/ICON0.PNG and PIC1.PNG")
