"""UI pack (data/ui.pak): fonts, spawn icons, map thumbnails and the logo, for the PSP menus and HUD.

Layout, read by psp/src/ui.c (same sectioned container as .pmap):
  "FNTS": {u16 atlas_w, atlas_h; u8 line_height, ascent; u16 pad; u32 atlas_ofs; u32 glyph_ofs} per font
  "GLYP": {u16 x, y; u8 w, h; i8 xoff, yoff; u8 advance; u8 pad[3]} x 95 (ASCII 32..126) per font
  "IMGS": {char name[48]; u16 tex_w, tex_h; u16 w, h; u32 clut_ofs; u32 data_ofs} per image (T8, swizzled)
  "DATA": the atlases, CLUTs and image data. All offsets are absolute.
Font atlases are 8-bit coverage (T8 indexed into a white-with-alpha ramp built on the PSP).
"""

import io
import os
import struct

import numpy as np
from PIL import Image, ImageFont

from . import pspimage
from .pmap import PmapWriter
from .vpk import GameFS
from .vtf import Vtf

PUI_MAGIC = b"PUI "
PUI_VERSION = 1

# Tahoma is what GMod's Derma UI uses; DejaVu is the fallback inside Ubuntu.
FONT_CANDIDATES = {
    "regular": ["/mnt/c/Windows/Fonts/tahoma.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"],
    "bold": ["/mnt/c/Windows/Fonts/tahomabd.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"],
}
# Order matters: psp/src/ui.h FONT_* indices
FONTS = [("regular", 11), ("bold", 12), ("bold", 18), ("bold", 26)]


def _font_path(kind):
    for p in FONT_CANDIDATES[kind]:
        if os.path.exists(p):
            return p
    raise FileNotFoundError(f"no {kind} font found")


def _pow2(v):
    p = 16
    while p < v:
        p *= 2
    return p


def build_font(kind, size):
    font = ImageFont.truetype(_font_path(kind), size)
    ascent, descent = font.getmetrics()
    glyphs = []
    x = y = 0
    row_h = 0
    width = 256
    placed = []
    for code in range(32, 127):
        ch = chr(code)
        bbox = font.getbbox(ch)
        adv = int(round(font.getlength(ch)))
        if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            placed.append((ch, None, 0, 0, 0, 0, adv))
            continue
        w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
        if x + w + 1 > width:
            x = 0
            y += row_h + 1
            row_h = 0
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
        from PIL import ImageDraw
        ImageDraw.Draw(g).text((-bbox[0], -bbox[1]), ch, font=font, fill=255)
        atlas.paste(g, (gx, gy))
        glyphs.append((gx, gy, w, h, bbox[0], bbox[1], adv))
    data = pspimage.swizzle(np.array(atlas, dtype=np.uint8))
    return width, height, ascent + descent, ascent, data, glyphs


def _load_image(fs, path):
    data = fs.read(path)
    if data is None:
        return None
    if path.endswith(".vtf"):
        v = Vtf(data)
        return v.decode(0)
    return Image.open(io.BytesIO(data)).convert("RGBA")


def _fit_pow2(img, max_w, max_h):
    """Scales into max_w x max_h keeping aspect; returns (canvas RGBA pow2, content w, h)."""
    s = min(max_w / img.width, max_h / img.height, 1.0)
    w, h = max(1, int(img.width * s)), max(1, int(img.height * s))
    img = img.resize((w, h), Image.Resampling.LANCZOS)
    tw, th = _pow2(w), max(8, _pow2(h))
    canvas = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
    canvas.paste(img, (0, 0))
    return canvas, w, h


def build_ui(gmod_root, props, maps, out_path, log=print):
    """props: (model path, label) for the spawn menu; maps: map names that may need thumbnails."""
    fs = GameFS(gmod_root)
    data = bytearray()

    def blob(b):
        nonlocal data
        data += b"\0" * ((-len(data)) % 16)
        o = len(data)
        data += b
        return o

    font_recs = []
    glyph_bytes = bytearray()
    for kind, size in FONTS:
        w, h, line, ascent, atlas, glyphs = build_font(kind, size)
        font_recs.append((w, h, line, ascent, blob(atlas), len(glyph_bytes)))
        for g in glyphs:
            glyph_bytes += struct.pack("<2H2B2bB3x", *g)

    images = []  # (name, tex_w, tex_h, w, h, clut_ofs, data_ofs)

    def add_image(name, img, max_w, max_h, alpha=True):
        canvas, w, h = _fit_pow2(img, max_w, max_h)
        pt = pspimage.convert(canvas, max(canvas.width, canvas.height), alpha, max_levels=1)
        images.append((name, pt.width, pt.height, w, h, blob(pt.clut), blob(pt.levels[0])))

    found = 0
    game_fs = {}
    for item in props:
        mdl, label = item[:2]
        game = item[2] if len(item) > 2 else ""
        if game:
            # A mounted game's model (npcs.txt game=): GMod's icon of that name could be HL2's model
            # (Half-Life: Source's models/barney.mdl), so draw it
            from .extras import render_icon
            from .materials import MaterialLoader
            from .npcbuild import GAME_DIRS, STEAM

            if game not in game_fs:
                gfs = GameFS(gmod_root, [STEAM + d for d in GAME_DIRS.get(game, [])])
                game_fs[game] = (gfs, MaterialLoader(gfs))
            img = render_icon(*game_fs[game], mdl)
        else:
            img = _load_image(fs, "materials/spawnicons/" + mdl[:-4] + ".png")
        if img is not None:
            add_image("icon:" + label, img, 64, 64)
            found += 1
    # Map thumbnails go in a file of their own (thumbs.pak), read by the game only while they're
    # on screen: hundreds of maps' worth would take megabytes of RAM in this pack
    thumbs = []
    for name in maps:
        # Half-Life's maps (hl1_<name>) look like Half-Life: Source's of the same name
        src = name[4:] if name.startswith("hl1_") else name
        img = _load_image(fs, f"maps/thumb/{name}.png") or _load_image(fs, f"maps/thumb/{src}.png")
        if img is not None:
            sq = Image.new("RGBA", (128, 128), (0, 0, 0, 255))
            fit, w_, h_ = _fit_pow2(img.convert("RGBA"), 128, 128)
            sq.paste(fit.crop((0, 0, w_, h_)).resize((128, 128), Image.Resampling.LANCZOS), (0, 0))
            pt = pspimage.convert(sq, 128, False, max_levels=1)
            thumbs.append((name, pt.clut + pt.levels[0]))
    thumbs_path = os.path.join(os.path.dirname(out_path), "thumbs.pak")
    with open(thumbs_path, "wb") as f:
        f.write(struct.pack("<4sIII", b"PTHB", len(thumbs), 0, 0))
        ofs = 16 + 36 * len(thumbs)
        ofs += (-ofs) % 64
        for name, data_ in thumbs:
            f.write(struct.pack("<32sI", name.encode("latin-1")[:31], ofs))
            ofs += len(data_)
        f.write(b"\0" * ((-f.tell()) % 64))
        for _name, data_ in thumbs:
            f.write(data_)
    logo = _load_image(fs, "materials/gui/gmod_logo.vtf")
    if logo is not None:
        add_image("logo", logo, 256, 64)
    log(f"  ui: {len(FONTS)} fonts, {found}/{len(props)} spawn icons, "
        f"{len(thumbs)}/{len(maps)} map thumbnails (in {thumbs_path}), logo {'yes' if logo else 'no'}")

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
    log(f"  wrote {out_path}: {size / 1024:.0f} KB")
