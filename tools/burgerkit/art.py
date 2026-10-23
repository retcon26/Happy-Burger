"""Every picture the game uses, drawn here with Pillow: the props' and characters' colour atlas, the map's
tiling textures, the signs, the logo and the XMB icons. The font is Pillow's own Aileron (public domain).
"""

import random
import zlib

from PIL import Image, ImageDraw, ImageFont

ATLAS = 256  # the props' and characters' shared texture
CELL = 16    # one colour or pattern per cell
INSET = 3    # pixels kept clear of each cell's edge, so mip levels don't bleed the neighbours in


def font(size, bold=False):
    f = ImageFont.load_default(size)
    return f, (max(1, size // 12) if bold else 0)


def text(draw, xy, s, size, fill, bold=False, anchor="la", outline=None):
    f, stroke = font(size, bold)
    if outline is not None:
        draw.text(xy, s, font=f, fill=outline, anchor=anchor, stroke_width=stroke + max(2, size // 8),
                  stroke_fill=outline)
    draw.text(xy, s, font=f, fill=fill, anchor=anchor, stroke_width=stroke, stroke_fill=fill)


def _jitter(img, amount, seed):
    """Speckles a cell or texture a little, so flat colours don't look like plastic."""
    rnd = random.Random(seed)
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]
            d = rnd.randint(-amount, amount)
            px[x, y] = (max(0, min(255, r + d)), max(0, min(255, g + d)), max(0, min(255, b + d)), a)


class Atlas:
    """Named cells of one 256x256 texture. uv(name) is the cell's inner rect (u0, v0, u1, v1)."""

    def __init__(self):
        self.img = Image.new("RGBA", (ATLAS, ATLAS), (255, 0, 255, 255))
        self.cells = {}

    def _slot(self, name):
        if name in self.cells:
            raise ValueError(f"atlas cell {name} twice")
        k = len(self.cells)
        per = ATLAS // CELL
        if k >= per * per:
            raise ValueError("atlas full")
        self.cells[name] = ((k % per) * CELL, (k // per) * CELL)
        return self.cells[name]

    def paint(self, name, fn, noise=5):
        x, y = self._slot(name)
        cell = Image.new("RGBA", (CELL, CELL))
        fn(cell, ImageDraw.Draw(cell))
        if noise:
            _jitter(cell, noise, zlib.crc32(name.encode()))
        self.img.paste(cell, (x, y))

    def solid(self, name, rgb, noise=5):
        self.paint(name, lambda c, d: d.rectangle((0, 0, CELL, CELL), fill=tuple(rgb) + (255,)), noise)

    def uv(self, name, inset=INSET):
        x, y = self.cells[name]
        return ((x + inset) / ATLAS, (y + inset) / ATLAS, (x + CELL - inset) / ATLAS, (y + CELL - inset) / ATLAS)


def _stripes(c1, c2, n=4, vertical=True):
    def fn(cell, d):
        w = CELL / n
        for i in range(n):
            box = (i * w, 0, (i + 1) * w, CELL) if vertical else (0, i * w, CELL, (i + 1) * w)
            d.rectangle(box, fill=(c1 if i % 2 == 0 else c2) + (255,))
    return fn


def _dots(base, dot, count, seed):
    def fn(cell, d):
        d.rectangle((0, 0, CELL, CELL), fill=base + (255,))
        rnd = random.Random(seed)
        for _ in range(count):
            x, y = rnd.randint(INSET, CELL - INSET - 2), rnd.randint(INSET, CELL - INSET - 2)
            d.rectangle((x, y, x + 1, y), fill=dot + (255,))
    return fn


def _face(skin, eyes=(30, 25, 25), mouth=(120, 50, 45), mood="smile", brows=None):
    """A head's front: two eyes and a mouth on the skin colour, inside the cell's inset area."""
    def fn(cell, d):
        d.rectangle((0, 0, CELL, CELL), fill=skin + (255,))
        d.rectangle((5, 6, 6, 7), fill=eyes + (255,))
        d.rectangle((9, 6, 10, 7), fill=eyes + (255,))
        if brows:
            d.line((4, 4, 6, 5) if mood == "angry" else (4, 4, 6, 4), fill=brows + (255,))
            d.line((9, 5, 11, 4) if mood == "angry" else (9, 4, 11, 4), fill=brows + (255,))
        if mood == "smile":
            d.line((5, 10, 6, 11), fill=mouth + (255,))
            d.line((7, 11, 8, 11), fill=mouth + (255,))
            d.line((9, 11, 10, 10), fill=mouth + (255,))
        elif mood == "grin":
            d.rectangle((5, 10, 10, 11), fill=(250, 250, 245, 255))
            d.line((5, 10, 10, 10), fill=mouth + (255,))
        else:
            d.line((5, 11, 10, 11), fill=mouth + (255,))
        if mood == "shades":
            d.rectangle((4, 5, 11, 7), fill=(15, 15, 20, 255))
    return fn


# Characters' colours: skins, hair, clothes
SKINS = {"skin1": (241, 200, 160), "skin2": (214, 160, 118), "skin3": (160, 108, 72), "skin4": (105, 70, 48)}
HAIRS = {"hair_black": (35, 30, 30), "hair_brown": (95, 60, 35), "hair_blond": (225, 190, 95),
         "hair_red": (170, 70, 35), "hair_grey": (170, 170, 170), "hair_green": (60, 210, 60)}
CLOTHES = {"shirt_blue": (60, 105, 190), "shirt_green": (70, 150, 80), "shirt_yellow": (235, 195, 60),
           "shirt_purple": (125, 75, 170), "shirt_orange": (230, 125, 45), "shirt_teal": (50, 160, 165),
           "shirt_white": (240, 240, 235), "shirt_pink": (230, 120, 160), "suit": (40, 42, 50),
           "pants_jeans": (55, 75, 120), "pants_khaki": (180, 160, 115), "pants_black": (35, 35, 40),
           "pants_white": (225, 225, 220), "shoes": (45, 35, 30), "shoes_white": (230, 230, 230),
           "tie": (200, 30, 40), "apron": (250, 250, 250), "chef_hat": (252, 252, 252), "cap_red": (205, 40, 40),
           "jacket": (30, 30, 35), "glasses": (20, 20, 25), "clipboard": (150, 110, 70), "badge": (230, 200, 60)}


def build_atlas():
    a = Atlas()
    # Food
    a.solid("bun", (218, 148, 64))
    a.paint("bun_top", _dots((206, 128, 46), (250, 240, 210), 9, 1))
    a.solid("bun_inside", (245, 215, 160))
    a.paint("patty", lambda c, d: (d.rectangle((0, 0, CELL, CELL), fill=(115, 62, 38, 255)),
                                   [d.line((i, INSET, i + 6, CELL - INSET), fill=(60, 30, 18, 255)) for i in (2, 7)]))
    a.solid("patty_side", (100, 55, 34))
    a.paint("patty_raw", lambda c, d: (d.rectangle((0, 0, CELL, CELL), fill=(232, 138, 128, 255)),
                                       [d.point((x, y), fill=(250, 200, 190, 255)) for x, y in
                                        [(4, 5), (9, 4), (6, 9), (11, 10), (5, 12), (10, 7)]]))
    a.solid("patty_raw_side", (215, 120, 112))
    a.solid("sausage_raw", (228, 140, 130))
    a.solid("sausage_end", (200, 110, 100))
    a.paint("board", lambda c, d: (d.rectangle((0, 0, CELL, CELL), fill=(205, 160, 105, 255)),
                                   [d.line((0, y, CELL, y), fill=(185, 140, 90, 255)) for y in (5, 9, 12)]))
    a.solid("board_edge", (170, 120, 70))
    a.solid("cardboard", (185, 145, 95))
    a.solid("cheese", (252, 196, 38))
    a.solid("lettuce", (95, 185, 60))
    a.solid("tomato", (215, 50, 40))
    a.solid("sausage", (190, 80, 55))
    a.solid("mustard", (245, 210, 30), noise=2)
    a.solid("ketchup", (200, 20, 25))
    a.solid("fry", (248, 208, 82))
    a.paint("carton", lambda c, d: (d.rectangle((0, 0, CELL, CELL), fill=(210, 30, 35, 255)),
                                    d.ellipse((5, 5, 10, 10), fill=(250, 200, 40, 255))))
    a.solid("carton_plain", (210, 30, 35))
    a.paint("cup", _stripes((235, 40, 40), (250, 250, 250), 4))
    a.solid("lid", (245, 245, 245))
    a.solid("straw", (250, 230, 60))
    # Kitchen
    a.solid("steel", (190, 195, 200), noise=3)
    a.solid("steel_dark", (85, 88, 95), noise=3)
    a.paint("grate", lambda c, d: (d.rectangle((0, 0, CELL, CELL), fill=(40, 40, 44, 255)),
                                   [d.line((0, i, CELL, i), fill=(110, 110, 115, 255)) for i in range(1, CELL, 3)]))
    a.solid("hot", (230, 90, 30))
    a.solid("oil", (205, 150, 40), noise=2)
    a.solid("basket", (150, 150, 150))
    a.solid("fridge", (236, 240, 244), noise=2)
    a.solid("handle", (160, 165, 175))
    a.paint("fridge_meat", _stripes((236, 240, 244), (200, 50, 50), 2, vertical=False))
    a.paint("fridge_dogs", _stripes((236, 240, 244), (240, 150, 40), 2, vertical=False))
    a.solid("soda_red", (200, 30, 40))
    a.paint("soda_panel", lambda c, d: (d.rectangle((0, 0, CELL, CELL), fill=(30, 30, 40, 255)),
                                        d.rectangle((4, 4, 6, 6), fill=(240, 60, 60, 255)),
                                        d.rectangle((9, 4, 11, 6), fill=(70, 200, 90, 255)),
                                        d.rectangle((4, 9, 6, 11), fill=(240, 180, 40, 255)),
                                        d.rectangle((9, 9, 11, 11), fill=(70, 140, 240, 255))))
    a.solid("trash", (85, 110, 95))
    a.solid("trash_lid", (60, 80, 70))
    a.paint("counter_front", lambda c, d: (d.rectangle((0, 0, CELL, CELL), fill=(200, 38, 38, 255)),
                                           d.rectangle((0, 6, CELL, 8), fill=(245, 245, 240, 255)),
                                           d.rectangle((0, 12, CELL, CELL), fill=(60, 60, 64, 255))))
    a.solid("counter_side", (205, 40, 40))
    a.solid("tray", (250, 205, 40), noise=2)
    a.solid("wood", (150, 100, 60))
    a.solid("wood_dark", (100, 65, 40))
    a.solid("vinyl_red", (190, 35, 40))
    a.solid("plant", (60, 150, 60))
    a.solid("pot_clay", (180, 95, 60))
    a.solid("cone", (245, 120, 30))
    a.paint("wet_sign", _stripes((250, 215, 30), (30, 30, 30), 4, vertical=False))
    a.solid("white", (250, 250, 250), noise=1)
    a.solid("black", (25, 25, 28), noise=1)
    # People
    for name, rgb in {**SKINS, **HAIRS, **CLOTHES}.items():
        a.solid(name, rgb, noise=3)
    for skin, rgb in SKINS.items():
        a.paint(f"face_{skin}", _face(rgb))
        a.paint(f"face_{skin}_grin", _face(rgb, mood="grin"))
        a.paint(f"face_{skin}_grump", _face(rgb, mood="flat", brows=(60, 40, 30)))
        a.paint(f"face_{skin}_shades", _face(rgb, mood="shades"))
    a.paint("face_minge", _face(SKINS["skin1"], mood="angry", brows=(40, 140, 40)))
    return a


# --- The map's textures: 64x64, tiling -------------------------------------------------------------

def _tile(fn, size=64, noise=4, seed=0):
    img = Image.new("RGBA", (size, size))
    fn(img, ImageDraw.Draw(img))
    if noise:
        _jitter(img, noise, seed)
    return img


def _checker(c1, c2, n):
    def fn(img, d):
        s = img.width // n
        for y in range(n):
            for x in range(n):
                d.rectangle((x * s, y * s, x * s + s - 1, y * s + s - 1), fill=(c1 if (x + y) % 2 == 0 else c2) + (255,))
    return fn


def _bricks(img, d):
    d.rectangle((0, 0, 64, 64), fill=(150, 150, 140, 255))
    for row in range(8):
        off = 8 if row % 2 else 0
        for col in range(-1, 4):
            x = col * 16 + off
            shade = 155 + (row * 7 + col * 13) % 30
            d.rectangle((x + 1, row * 8 + 1, x + 15, row * 8 + 7), fill=(shade, 70 + shade // 6, 50, 255))


def _grid(base, line, step):
    def fn(img, d):
        d.rectangle((0, 0, img.width, img.height), fill=base + (255,))
        for k in range(0, img.width, step):
            d.line((k, 0, k, img.height), fill=line + (255,))
            d.line((0, k, img.width, k), fill=line + (255,))
    return fn


def _facade(wall, window):
    def fn(img, d):
        d.rectangle((0, 0, 64, 64), fill=wall + (255,))
        for x in (8, 36):
            d.rectangle((x, 10, x + 20, 50), fill=(40, 40, 50, 255))
            d.rectangle((x + 2, 12, x + 18, 48), fill=window + (255,))
            d.line((x + 10, 12, x + 10, 48), fill=(40, 40, 50, 255))
    return fn


def _window(img, d):
    d.rectangle((0, 0, 64, 64), fill=(60, 60, 70, 255))
    d.rectangle((3, 3, 60, 60), fill=(140, 190, 225, 255))
    for k in range(8, 60, 14):
        d.line((k, 60, k + 20, 3), fill=(190, 225, 245, 255), width=3)
    d.line((32, 3, 32, 60), fill=(60, 60, 70, 255), width=2)


def _wall(img, d):
    d.rectangle((0, 0, 64, 64), fill=(228, 205, 160, 255))


def _wainscot(img, d):
    d.rectangle((0, 0, 64, 64), fill=(195, 40, 40, 255))
    for x in range(0, 64, 16):
        d.line((x, 0, x, 64), fill=(160, 30, 30, 255))


def _asphalt(img, d):
    d.rectangle((0, 0, 64, 64), fill=(60, 62, 66, 255))


def _road_line(img, d):
    d.rectangle((0, 0, 64, 64), fill=(60, 62, 66, 255))
    d.rectangle((0, 26, 40, 37), fill=(240, 200, 40, 255))


def _light(img, d):
    d.rectangle((0, 0, 64, 64), fill=(255, 252, 235, 255))
    d.rectangle((0, 0, 63, 63), outline=(200, 200, 200, 255), width=3)


def _door(img, d):
    d.rectangle((0, 0, 64, 64), fill=(90, 60, 40, 255))


def map_textures():
    """name -> (image, world units one repeat covers, or None to fit the face)"""
    return {
        "floor_dining": (_tile(_checker((240, 240, 235), (35, 35, 40), 2), noise=3, seed=1), 48),
        "floor_kitchen": (_tile(_checker((235, 235, 230), (200, 45, 45), 4), noise=3, seed=2), 48),
        "wall": (_tile(_wall, noise=3, seed=3), 64),
        "wainscot": (_tile(_wainscot, noise=3, seed=4), 64),
        "ceiling": (_tile(_grid((212, 210, 200), (170, 168, 160), 32), noise=2, seed=5), 64),
        "light": (_tile(_light, noise=0), None),
        "brick": (_tile(_bricks, noise=6, seed=6), 64),
        "sidewalk": (_tile(_grid((175, 175, 170), (135, 135, 130), 32), noise=6, seed=7), 64),
        "asphalt": (_tile(_asphalt, noise=8, seed=8), 64),
        "road_line": (_tile(_road_line, noise=6, seed=9), 128),
        "window": (_tile(_window, noise=0), None),
        "facade_blue": (_tile(_facade((90, 120, 160), (200, 225, 240)), noise=5, seed=10), 128),
        "facade_green": (_tile(_facade((110, 150, 100), (220, 230, 200)), noise=5, seed=11), 128),
        "facade_tan": (_tile(_facade((200, 175, 130), (230, 220, 190)), noise=5, seed=12), 128),
        "roof": (_tile(_grid((70, 70, 75), (55, 55, 60), 16), noise=5, seed=13), 64),
        "wood": (_tile(_grid((150, 100, 60), (120, 80, 48), 16), noise=6, seed=14), 64),
        "booth": (_tile(lambda i, d: d.rectangle((0, 0, 64, 64), fill=(190, 35, 40, 255)), noise=4, seed=15), 64),
        "door": (_tile(_door, noise=5, seed=16), 64),
        "grass": (_tile(lambda i, d: d.rectangle((0, 0, 64, 64), fill=(80, 150, 60, 255)), noise=12, seed=17), 64),
        "sign": (sign_image(), None),
        "menu": (menu_image(), None),
    }


# --- Signs and the logo ----------------------------------------------------------------------------------

def burger_icon(size, happy=True):
    """A cartoon burger, size x size, transparent background: Happy Burger's, smiling."""
    s = size
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    w = s * 0.9
    x0 = (s - w) / 2
    d.chord((x0, s * 0.12, x0 + w, s * 0.75), 180, 360, fill=(222, 150, 60, 255), outline=(90, 50, 20, 255),
            width=max(1, s // 32))
    for sx, sy in [(0.2, 0.36), (0.25, 0.24), (0.47, 0.17), (0.7, 0.23), (0.76, 0.35)]:
        d.ellipse((s * sx, s * sy, s * sx + s * 0.05, s * sy + s * 0.03), fill=(250, 240, 210, 255))
    d.rectangle((x0 + s * 0.02, s * 0.45, x0 + w - s * 0.02, s * 0.52), fill=(95, 185, 60, 255))
    d.polygon([(x0 + w * 0.1, s * 0.52), (x0 + w * 0.5, s * 0.52), (x0 + w * 0.3, s * 0.62)], fill=(252, 196, 38, 255))
    d.rounded_rectangle((x0, s * 0.52, x0 + w, s * 0.66), radius=s // 16, fill=(115, 62, 38, 255),
                        outline=(60, 30, 18, 255), width=max(1, s // 40))
    d.rounded_rectangle((x0 + s * 0.02, s * 0.67, x0 + w - s * 0.02, s * 0.8), radius=s // 14,
                        fill=(218, 148, 64, 255), outline=(90, 50, 20, 255), width=max(1, s // 32))
    if happy:
        # A smiling face on the top bun: two eyes, rosy cheeks and a grin
        ink = (60, 30, 15, 255)
        e = max(1.5, s * 0.045)
        for ex in (0.38, 0.62):
            d.ellipse((s * ex - e, s * 0.3 - e * 1.3, s * ex + e, s * 0.3 + e * 1.3), fill=ink)
        for cx in (0.27, 0.73):
            d.ellipse((s * cx - e * 1.4, s * 0.37 - e * 0.8, s * cx + e * 1.4, s * 0.37 + e * 0.8), fill=(235, 120, 90, 255))
        d.arc((s * 0.4, s * 0.25, s * 0.6, s * 0.42), 20, 160, fill=ink, width=max(1, s // 28))
    return img


def logo_image():
    """The menus' logo, 256x64: the burger and the name."""
    img = Image.new("RGBA", (256, 64), (0, 0, 0, 0))
    img.alpha_composite(burger_icon(60), (0, 2))
    d = ImageDraw.Draw(img)
    text(d, (64, 4), "HAPPY", 24, (255, 210, 60, 255), bold=True, outline=(120, 30, 20, 255))
    text(d, (64, 30), "BURGER", 30, (255, 255, 255, 255), bold=True, outline=(160, 30, 30, 255))
    return img


def sign_image():
    """The shop sign over the door, 256x64."""
    img = Image.new("RGBA", (256, 64), (200, 35, 35, 255))
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, 255, 63), outline=(250, 200, 40, 255), width=4)
    img.alpha_composite(burger_icon(54), (8, 4))
    text(d, (159, 32), "HAPPY BURGER", 19, (255, 230, 120, 255), bold=True, anchor="mm", outline=(90, 20, 20, 255))
    return img


def menu_image():
    """The menu board over the counter, 128x64."""
    img = Image.new("RGBA", (128, 64), (30, 30, 34, 255))
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, 127, 63), outline=(150, 100, 60, 255), width=3)
    text(d, (64, 9), "MENU", 13, (255, 210, 60, 255), bold=True, anchor="mm")
    rows = [("Burger", "$6"), ("Hot Dog", "$4"), ("Fries", "$3"), ("Soda", "$2")]
    for i, (name, price) in enumerate(rows):
        y = 18 + i * 10
        text(d, (10, y), name, 9, (240, 240, 240, 255))
        text(d, (118, y), price, 9, (120, 230, 120, 255), anchor="ra")
    return img


def decal_ketchup(seed):
    """A ketchup splat, laid on mid grey: the engine draws decals as 'modulate' ones (grey leaves the wall be)."""
    rnd = random.Random(seed)
    img = Image.new("RGB", (64, 64), (128, 128, 128))
    d = ImageDraw.Draw(img)
    col = (150, 20, 25)
    d.ellipse((18, 18, 46, 46), fill=col)
    for _ in range(9):
        r = rnd.randint(3, 8)
        x, y = rnd.randint(8, 52), rnd.randint(8, 52)
        d.ellipse((x - r, y - r, x + r, y + r), fill=col)
    return img


def xmb_icon():
    """ICON0.PNG: 144x80, the XMB's game icon."""
    img = Image.new("RGBA", (144, 80), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, 143, 79), radius=10, fill=(200, 35, 35, 255), outline=(250, 200, 40, 255), width=3)
    img.alpha_composite(burger_icon(64), (4, 8))
    text(d, (104, 26), "HAPPY", 15, (255, 220, 90, 255), bold=True, anchor="mm", outline=(90, 20, 20, 255))
    text(d, (104, 50), "BURGER", 18, (255, 255, 255, 255), bold=True, anchor="mm", outline=(90, 20, 20, 255))
    return img


def xmb_background():
    """PIC1.PNG: 480x272, behind the XMB when the game's picked."""
    img = Image.new("RGBA", (480, 272), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)
    for y in range(272):
        k = y / 271
        d.line((0, y, 480, y), fill=(int(230 - 60 * k), int(70 + 40 * k), int(40 + 20 * k), 255))
    for x in range(-272, 480, 40):
        d.line((x, 272, x + 272, 0), fill=(240, 100, 60, 255), width=10)
    img.alpha_composite(burger_icon(200), (260, 50))
    return img
