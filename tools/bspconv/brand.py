"""Retcon's Mod's own logo for the menus (ui.pak's "logo" image), drawn here so the game carries no
other game's branding: an orange badge with an R, and the name beside it. 256x64, transparent.

    python3 -m bspconv.brand logo.png     (writes a preview)
"""

import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

_FONTS = ["/mnt/c/Windows/Fonts/tahomabd.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"]
ORANGE_TOP = (250, 150, 50)
ORANGE_BOTTOM = (214, 92, 24)
OUTLINE = (60, 30, 10)


def _font(size):
    for path in _FONTS:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def draw_logo(width=256, height=64):
    k = 4  # drawn big, then scaled down for smooth edges
    W, H = width * k, height * k
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))

    # The badge: a rounded square with an orange gradient
    s = H - 6 * k
    x0, y0 = 3 * k, 3 * k
    grad = Image.new("RGBA", (s, s))
    gd = ImageDraw.Draw(grad)
    for y in range(s):
        t = y / (s - 1)
        c = tuple(int(a + (b - a) * t) for a, b in zip(ORANGE_TOP, ORANGE_BOTTOM))
        gd.line([(0, y), (s, y)], fill=c + (255,))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=s // 5, fill=255)
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 140), (x0 + k, y0 + 2 * k), mask)
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(2 * k)))
    img.paste(grad, (x0, y0), mask)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([x0, y0, x0 + s - 1, y0 + s - 1], radius=s // 5, outline=OUTLINE + (255,), width=k)
    # The R, white with a dark edge
    f = _font(int(s * 0.78))
    cx, cy = x0 + s // 2, y0 + s // 2 + k
    d.text((cx + k, cy + 2 * k), "R", font=f, fill=(90, 40, 10, 200), anchor="mm")
    d.text((cx, cy), "R", font=f, fill=(255, 255, 255, 255), anchor="mm", stroke_width=k, stroke_fill=OUTLINE + (255,))

    # The name: "Retcon's" over "MOD"
    tx = x0 + s + 7 * k
    d.text((tx, 2 * k), "Retcon's", font=_font(29 * k), fill=(255, 255, 255, 255), anchor="la",
           stroke_width=2 * k, stroke_fill=(20, 20, 20, 255))
    d.text((tx + k, 34 * k), "MOD", font=_font(24 * k), fill=ORANGE_TOP + (255,), anchor="la",
           stroke_width=2 * k, stroke_fill=(20, 20, 20, 255))
    return img.resize((width, height), Image.Resampling.LANCZOS)


if __name__ == "__main__":
    draw_logo().save(sys.argv[1] if len(sys.argv) > 1 else "logo.png")
