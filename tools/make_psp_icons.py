"""Draws the PSP game menu (XMB) images that go into EBOOT.PBP: ICON0.PNG (144x80, the game's icon)
and PIC1.PNG (480x272, the background behind the menu while the game is selected). Without them the
menu shows a blank default icon.

    python3 make_psp_icons.py     (writes psp/ICON0.PNG and psp/PIC1.PNG)
"""

import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONTS = {
    "bold": ["/mnt/c/Windows/Fonts/tahomabd.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"],
    "regular": ["/mnt/c/Windows/Fonts/tahoma.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"],
}
TOP, BOTTOM = (46, 66, 86), (14, 20, 28)  # GMod-menu blue-grey
GRID = (255, 255, 255)
ACCENT = (61, 160, 230)  # the menus' highlight blue


def font(kind, size):
    for p in FONTS[kind]:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    raise FileNotFoundError(f"no {kind} font found")


def backdrop(w, h, cell, grid_alpha):
    """Vertical gradient with a faint construction grid, darker towards the edges."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(TOP[k] + (BOTTOM[k] - TOP[k]) * t) for k in range(3))
        for x in range(w):
            px[x, y] = c
    grid = Image.new("L", (w, h), 0)
    g = ImageDraw.Draw(grid)
    for x in range(0, w, cell):
        g.line([(x, 0), (x, h)], fill=grid_alpha)
    for y in range(0, h, cell):
        g.line([(0, y), (w, y)], fill=grid_alpha)
    img.paste(Image.new("RGB", (w, h), GRID), (0, 0), grid)
    # Vignette
    shade = Image.new("L", (w, h), 0)
    ImageDraw.Draw(shade).ellipse([-w * 0.25, -h * 0.4, w * 1.25, h * 1.4], fill=255)
    shade = shade.filter(ImageFilter.GaussianBlur(min(w, h) * 0.25))
    return Image.composite(img, Image.new("RGB", (w, h), BOTTOM), shade)


def text(img, xy, s, f, fill, anchor="mm", shadow=True):
    d = ImageDraw.Draw(img)
    if shadow:
        d.text((xy[0] + 1, xy[1] + 2), s, font=f, fill=(0, 0, 0), anchor=anchor)
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def cube(d, cx, cy, s, top, left, right, edge=(20, 24, 30)):
    """An isometric box: (cx, cy) is its top corner, s the edge length."""
    h = s * 0.5
    t = [(cx, cy), (cx + s, cy + h), (cx, cy + 2 * h), (cx - s, cy + h)]
    l = [(cx - s, cy + h), (cx, cy + 2 * h), (cx, cy + 2 * h + s), (cx - s, cy + h + s)]
    r = [(cx, cy + 2 * h), (cx + s, cy + h), (cx + s, cy + h + s), (cx, cy + 2 * h + s)]
    for poly, col in ((l, left), (r, right), (t, top)):
        d.polygon(poly, fill=col, outline=edge)


def crate(d, cx, cy, s):
    """A wooden crate (the sandbox's classic prop), with its planks drawn on the two visible sides."""
    cube(d, cx, cy, s, (214, 160, 92), (150, 100, 52), (178, 122, 64))
    h = s * 0.5
    plank = (110, 70, 34)
    w = max(1, int(s / 9))
    # Diagonal braces on the side faces
    d.line([(cx - s, cy + h), (cx, cy + 2 * h + s)], fill=plank, width=w)
    d.line([(cx + s, cy + h), (cx, cy + 2 * h + s)], fill=plank, width=w)


def icon0():
    W, H, K = 144, 80, 4  # drawn at 4x, then scaled down for smooth edges
    img = backdrop(W * K, H * K, 12 * K, 28).convert("RGB")
    d = ImageDraw.Draw(img)
    # A little pile of props on the left: two crates, a blue barrel-ish cube and a floating one
    crate(d, 30 * K, 40 * K, 15 * K)
    crate(d, 55 * K, 46 * K, 12 * K)
    cube(d, 44 * K, 22 * K, 10 * K, (120, 190, 240), (40, 100, 160), (60, 130, 200))
    # The physics gun's beam holding the floating box
    beam = Image.new("L", img.size, 0)
    ImageDraw.Draw(beam).line([(4 * K, 76 * K), (44 * K, 32 * K)], fill=255, width=3 * K)
    beam = beam.filter(ImageFilter.GaussianBlur(2 * K))
    img.paste(Image.new("RGB", img.size, ACCENT), (0, 0), beam)
    ImageDraw.Draw(img).line([(4 * K, 76 * K), (44 * K, 32 * K)], fill=(220, 240, 255), width=K)
    img = img.resize((W, H), Image.Resampling.LANCZOS)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W - 1, H - 1], outline=(90, 110, 130))
    text(img, (107, 21), "Retcon's", font("bold", 15), (255, 255, 255))
    text(img, (107, 42), "Mod", font("bold", 22), (255, 255, 255))
    d.line([(83, 57), (131, 57)], fill=ACCENT, width=2)
    text(img, (107, 67), "for PSP", font("regular", 10), (200, 210, 220), shadow=False)
    return img


def pic1():
    img = backdrop(480, 272, 32, 18)
    # The menu's own text sits on the left; a quiet mark bottom right
    text(img, (464, 252), "by Retcon", font("regular", 13), (150, 165, 180), anchor="rs", shadow=False)
    return img


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "psp")
    icon0().save(os.path.join(out, "ICON0.PNG"), optimize=True)
    pic1().save(os.path.join(out, "PIC1.PNG"), optimize=True)
    print("wrote psp/ICON0.PNG (144x80) and psp/PIC1.PNG (480x272)")


if __name__ == "__main__":
    main()
