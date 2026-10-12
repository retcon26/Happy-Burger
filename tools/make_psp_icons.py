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


def icon0():
    img = backdrop(144, 80, 12, 28)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 143, 79], outline=(90, 110, 130))
    text(img, (72, 33), "PSP Sandbox", font("bold", 20), (255, 255, 255))
    d.line([(34, 50), (110, 50)], fill=ACCENT, width=2)
    text(img, (72, 62), "by Retcon", font("regular", 11), (200, 210, 220), shadow=False)
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
