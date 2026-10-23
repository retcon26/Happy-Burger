"""The props: food, the kitchen's appliances and the dining room's furniture, as low-poly models on the
shared atlas, written as data/props.ppak (psp/src/ppak_format.h). burger.lua spawns them by these names.
"""

import struct

from ..bspconv import pspimage
from ..bspconv.pmap import PmapWriter
from .mesh import Mesh

PPAK_MAGIC = b"PPAK"
PPAK_VERSION = 1
SHAPE_BOX, SHAPE_SPHERE, SHAPE_CYLINDER = 0, 1, 2
SURF_GENERIC, SURF_WOOD, SURF_METAL, SURF_PLASTIC, SURF_CONCRETE, SURF_GLASS, SURF_CARDBOARD, SURF_FLESH = range(8)
CATEGORIES = ["Food", "Ingredients", "Kitchen", "Dining"]

# Kitchen units share the counter's footprint and height (burger.lua's C and H)
UNIT = 48.0
UNIT_H = 40.0


# Burger parts, each from z up; they return how thick they are, so a stack is built by adding them in turn
def bun_bottom_part(m, a, z):
    m.prism(10, 6.0, 6.2, z, z + 2.2, {"side": a.uv("bun"), "top": a.uv("bun_inside"), "bottom": a.uv("bun")})
    return 2.2


def patty_part(m, a, z, raw=False):
    top, side = ("patty_raw", "patty_raw_side") if raw else ("patty", "patty_side")
    m.prism(10, 6.6, 6.6, z, z + 1.8, {"side": a.uv(side), "top": a.uv(top), "bottom": a.uv(top)})
    return 1.8


def cheese_part(m, a, z):
    m.box((-5.0, -5.0, z), (5.0, 5.0, z + 0.8), a.uv("cheese"))  # a square slice, its corners poking out
    return 0.8


def lettuce_part(m, a, z):
    m.prism(9, 6.9, 6.9, z, z + 0.9, a.uv("lettuce"), turn=20)
    return 0.9


def tomato_part(m, a, z):
    m.prism(8, 5.2, 5.2, z, z + 0.9, a.uv("tomato"), turn=10)
    return 0.9


def bun_top_part(m, a, z):
    m.prism(10, 6.3, 5.4, z, z + 1.9, {"side": a.uv("bun_top"), "bottom": a.uv("bun_inside")}, caps=(True, False))
    m.prism(10, 5.4, 3.0, z + 1.9, z + 3.3, a.uv("bun_top"), caps=(False, False))
    m.prism(10, 3.0, 0.0, z + 3.3, z + 3.6, a.uv("bun_top"), caps=(False, False))
    return 3.6


TOPPINGS = {"C": cheese_part, "L": lettuce_part, "T": tomato_part}


def burger(a, toppings="CLT"):
    """An assembled burger with these toppings (C cheese, L lettuce, T tomato), as burger.lua makes them."""
    m = Mesh()
    z = bun_bottom_part(m, a, 0.0)
    z += patty_part(m, a, z)
    for t in "CLT":
        if t in toppings:
            z += TOPPINGS[t](m, a, z)
    bun_top_part(m, a, z)
    return m


def single(part, **kw):
    def make(a):
        m = Mesh()
        part(m, a, 0.0, **kw)
        return m
    return make


def sausage(a):
    m = Mesh()
    m.box((-7.6, -1.3, 0.0), (7.6, 1.3, 2.4), {"side": a.uv("sausage_raw"), "front": a.uv("sausage_end"),
                                               "back": a.uv("sausage_end")})
    return m


def hot_dog_bun(a):
    m = Mesh()
    m.box((-6.5, -2.6, 0.0), (6.5, -0.4, 2.8), {"side": a.uv("bun"), "left": a.uv("bun_inside")})
    m.box((-6.5, 0.4, 0.0), (6.5, 2.6, 2.8), {"side": a.uv("bun"), "right": a.uv("bun_inside")})
    m.box((-6.5, -2.6, 0.0), (6.5, 2.6, 0.6), a.uv("bun"))
    return m


def hot_dog(a):
    m = Mesh()
    m.box((-6.5, -2.6, 0.0), (6.5, -0.4, 2.8), {"side": a.uv("bun"), "left": a.uv("bun_inside")})
    m.box((-6.5, 0.4, 0.0), (6.5, 2.6, 2.8), {"side": a.uv("bun"), "right": a.uv("bun_inside")})
    m.box((-6.5, -2.6, 0.0), (6.5, 2.6, 0.6), a.uv("bun"))
    m.box((-7.6, -1.3, 1.2), (7.6, 1.3, 3.6), a.uv("sausage"))
    for k in range(5):  # a mustard zigzag
        x = -6.0 + k * 2.6
        m.box((x, -0.9 if k % 2 else -0.1, 3.6), (x + 2.2, 0.1 if k % 2 else 0.9, 4.0), a.uv("mustard"))
    return m


def fries(a):
    m = Mesh()
    sides = {"front": a.uv("carton"), "back": a.uv("carton"), "side": a.uv("carton_plain")}
    m.box((-2.4, -3.2, 0.0), (2.4, 3.2, 6.5), sides)
    m.box((-2.8, -3.6, 6.5), (2.8, 3.6, 7.0), a.uv("carton_plain"))
    for i, (x, y, h) in enumerate([(-1.2, -2.0, 4), (0.4, -1.4, 5), (-0.6, 0.0, 3.5), (1.0, 0.8, 4.5),
                                   (-1.4, 1.8, 4), (0.2, 2.2, 3), (1.2, -2.6, 3.5)]):
        m.box((x - 0.5, y - 0.5, 4.0), (x + 0.5, y + 0.5, 7.0 + h), a.uv("fry"))
    return m


def soda(a):
    m = Mesh()
    m.prism(8, 2.6, 3.2, 0.0, 8.0, {"side": a.uv("cup"), "bottom": a.uv("white")}, caps=(True, False))
    m.prism(8, 3.4, 3.2, 8.0, 8.8, a.uv("lid"))
    m.box((0.6, -0.3, 8.8), (1.2, 0.3, 12.5), a.uv("straw"))
    return m


def counter(a):
    m = Mesh()
    m.box((-UNIT / 2 + 2, -UNIT / 2, 0.0), (UNIT / 2 - 2, UNIT / 2, UNIT_H - 2),
          {"front": a.uv("counter_front"), "back": a.uv("counter_front"), "side": a.uv("counter_side")})
    m.box((-UNIT / 2, -UNIT / 2, UNIT_H - 2), (UNIT / 2, UNIT / 2, UNIT_H), a.uv("steel"))
    return m


def grill(a):
    # Nothing above the grate: the physics box is the model's bounds, and food rests on its top
    m = Mesh()
    w = UNIT  # two units wide
    m.box((-UNIT / 2, -w, 0.0), (UNIT / 2, w, UNIT_H - 3), {"side": a.uv("steel_dark"), "front": a.uv("steel")})
    m.box((-UNIT / 2, -w, UNIT_H - 3), (UNIT / 2, w, UNIT_H), {"side": a.uv("steel"), "top": a.uv("grate")})
    for y in (-w + 10, -w / 3, w / 3, w - 10):  # knobs on the front
        m.box((UNIT / 2, y - 1.5, 26.0), (UNIT / 2 + 2.0, y + 1.5, 29.0), a.uv("black"))
    return m


def fryer(a):
    m = Mesh()
    m.box((-UNIT / 2, -UNIT / 2, 0.0), (UNIT / 2, UNIT / 2, UNIT_H - 4), {"side": a.uv("steel"), "front": a.uv("steel_dark")})
    for x0, x1, y0, y1 in [(-UNIT / 2, UNIT / 2, -UNIT / 2, -UNIT / 2 + 3), (-UNIT / 2, UNIT / 2, UNIT / 2 - 3, UNIT / 2),
                           (-UNIT / 2, -UNIT / 2 + 3, -UNIT / 2, UNIT / 2), (UNIT / 2 - 3, UNIT / 2, -UNIT / 2, UNIT / 2)]:
        m.box((x0, y0, UNIT_H - 4), (x1, y1, UNIT_H), a.uv("steel"))  # the rim round the oil
    m.box((-UNIT / 2 + 3, -UNIT / 2 + 3, UNIT_H - 6), (UNIT / 2 - 3, UNIT / 2 - 3, UNIT_H - 5), a.uv("oil"))
    m.box((-10, -14, UNIT_H - 5), (10, 4, UNIT_H - 1), a.uv("basket"))
    m.box((-2, 4, UNIT_H - 2), (2, 18, UNIT_H), a.uv("black"))  # the basket's handle
    return m


def fridge(a, stripe):
    m = Mesh()
    m.box((-17, -20, 0.0), (17, 20, 82.0), {"side": a.uv("fridge"), "front": a.uv(stripe)})
    m.box((17, -20, 40.0), (18.5, 20, 41.0), a.uv("handle"))  # the line between the doors
    m.box((17, 13, 48.0), (20, 15, 70.0), a.uv("handle"))
    m.box((17, 13, 14.0), (20, 15, 34.0), a.uv("handle"))
    return m


def soda_machine(a):
    m = Mesh()
    m.box((-15, -22, 0.0), (15, 22, 76.0), {"side": a.uv("soda_red"), "front": a.uv("soda_red")})
    m.box((15, -16, 44.0), (16, 16, 68.0), a.uv("soda_panel"))
    m.box((15, -10, 22.0), (24, 10, 26.0), a.uv("steel_dark"))  # the drip tray, where cups go
    m.box((15, -3, 36.0), (20, 3, 40.0), a.uv("steel"))  # the nozzle
    return m


def cutting_board(a):
    """Where burgers and hot dogs are put together (burger.lua's boards)."""
    m = Mesh()
    m.box((-17, -17, 0.0), (17, 17, 1.6), {"side": a.uv("board_edge"), "top": a.uv("board")})
    m.box((17, -3, 0.0), (22, 3, 1.6), a.uv("board_edge"))  # the handle
    return m


def bun_rack(a):
    m = Mesh()
    m.box((-16, -10, 0.0), (16, 10, 1.0), a.uv("steel"))
    for x in (-16, 15):  # the rack's ends
        m.box((x, -10, 0.0), (x + 1, 10, 13.0), a.uv("steel"))
    m.box((-16, -10, 12.0), (16, -9, 13.0), a.uv("steel"))
    m.box((-16, 9, 12.0), (16, 10, 13.0), a.uv("steel"))
    for x in (-9.0, 0.0, 9.0):  # buns waiting on it: bottoms below, tops on the shelf above
        mini = Mesh()
        bun_bottom_part(mini, a, 0.0)
        m.merge(_scaled(mini, 0.62), (x, -4.0, 1.0))
        top = Mesh()
        bun_top_part(top, a, 0.0)
        m.merge(_scaled(top, 0.62), (x, 4.0, 1.0))
    return m


def _scaled(mesh, k):
    out = Mesh()
    out.verts = [(u, v, nx, ny, nz, x * k, y * k, z * k, b) for u, v, nx, ny, nz, x, y, z, b in mesh.verts]
    out.tris = list(mesh.tris)
    return out


def dog_bun_box(a):
    m = Mesh()
    m.box((-16, -9, 0.0), (16, 9, 7.0), {"side": a.uv("cardboard"), "top": None})
    m.box((-15.5, -8.5, 0.5), (15.5, 8.5, 5.5), {"top": a.uv("bun_inside"), "side": None})
    for y in (-4.5, 0.0, 4.5):
        bun = Mesh()
        bun.box((-6.5, -1.8, 0.0), (6.5, 1.8, 2.4), a.uv("bun"))
        m.merge(bun, (-5.0, y, 5.5))
        m.merge(bun, (7.0, y, 5.5))
    return m


def topping_bin(a, cell):
    m = Mesh()
    m.box((-15, -7, 0.0), (15, 7, 6.0), {"side": a.uv("steel"), "top": None})
    m.box((-14.5, -6.5, 0.5), (14.5, 6.5, 5.0), {"top": a.uv(cell), "side": None})
    for x in (-8.0, 0.0, 8.0):  # a few pieces heaped up on top
        m.prism(6, 3.2, 2.4, 5.0, 6.4, a.uv(cell), centre=(x, 0.0), turn=x * 7)
    return m


def trash_can(a):
    m = Mesh()
    m.prism(10, 11.0, 12.5, 0.0, 30.0, {"side": a.uv("trash"), "bottom": a.uv("trash")}, caps=(True, False))
    m.prism(10, 13.0, 13.0, 30.0, 33.0, a.uv("trash_lid"))
    return m


def tray(a):
    m = Mesh()
    m.box((-14, -11, 0.0), (14, 11, 1.0), a.uv("tray"))
    for x0, y0, x1, y1 in [(-14, -11, 14, -10), (-14, 10, 14, 11), (-14, -11, -13, 11), (13, -11, 14, 11)]:
        m.box((x0, y0, 1.0), (x1, y1, 2.0), a.uv("tray"))
    return m


def stool(a):
    m = Mesh()
    m.prism(8, 8.0, 8.0, 26.0, 29.0, {"side": a.uv("vinyl_red"), "top": a.uv("vinyl_red"), "bottom": a.uv("steel")})
    m.prism(6, 1.4, 1.4, 1.0, 26.0, a.uv("steel"))
    m.prism(8, 7.0, 7.0, 0.0, 1.0, a.uv("steel"))
    return m


def table(a):
    m = Mesh()
    m.prism(10, 18.0, 18.0, 28.0, 30.0, {"side": a.uv("steel"), "top": a.uv("white"), "bottom": a.uv("steel")})
    m.prism(6, 2.0, 2.0, 1.5, 28.0, a.uv("steel"))
    m.prism(8, 10.0, 10.0, 0.0, 1.5, a.uv("steel"))
    return m


def squeeze_bottle(a, colour):
    m = Mesh()
    m.prism(6, 2.0, 2.0, 0.0, 6.5, a.uv(colour))
    m.prism(6, 2.0, 0.6, 6.5, 8.0, a.uv(colour))
    m.prism(6, 0.6, 0.2, 8.0, 9.5, a.uv("white"))
    return m


def plant(a):
    m = Mesh()
    m.prism(8, 7.0, 9.0, 0.0, 14.0, {"side": a.uv("pot_clay"), "bottom": a.uv("pot_clay"), "top": a.uv("hair_brown")})
    m.prism(4, 3.5, 0.0, 12.0, 34.0, a.uv("plant"), turn=45)  # a spike in the middle
    for k in range(6):  # big low-poly leaves round it
        m.merge(_tilted_leaf(a, 60 * k), (0.0, 0.0, 12.0))
    return m


def _tilted_leaf(a, yaw):
    leaf = Mesh()
    pts = [(0, -2.5, 0), (14, 0, 10), (0, 2.5, 0)]
    leaf.poly(pts, a.uv("plant"))
    leaf.poly(list(reversed(pts)), a.uv("plant"))
    out = Mesh()
    out.merge(leaf, (0, 0, 0), yaw)
    return out


def wet_floor_sign(a):
    m = Mesh()
    # An A-frame: two leaning boards
    for side in (1, -1):
        x = 5.0 * side
        pts_out = [(x, -6, 0), (x, 6, 0), (0.6 * side, 5, 24), (0.6 * side, -5, 24)]
        if side < 0:
            pts_out = list(reversed(pts_out))
        m.poly(pts_out, a.uv("wet_sign"))
        m.poly(list(reversed(pts_out)), a.uv("wet_sign"))
    return m


def traffic_cone(a):
    m = Mesh()
    m.box((-7, -7, 0), (7, 7, 1.5), a.uv("cone"))
    m.prism(8, 5.5, 0.8, 1.5, 22.0, a.uv("cone"))
    return m


# name, builder, shape, mass (kg), surface, category
PROPS = [
    ("Patty", single(patty_part, raw=True), SHAPE_CYLINDER, 0.2, SURF_FLESH, "Ingredients"),
    ("Bottom Bun", single(bun_bottom_part), SHAPE_CYLINDER, 0.15, SURF_GENERIC, "Ingredients"),
    ("Top Bun", single(bun_top_part), SHAPE_CYLINDER, 0.15, SURF_GENERIC, "Ingredients"),
    ("Cheese", single(cheese_part), SHAPE_BOX, 0.05, SURF_GENERIC, "Ingredients"),
    ("Lettuce", single(lettuce_part), SHAPE_CYLINDER, 0.05, SURF_GENERIC, "Ingredients"),
    ("Tomato", single(tomato_part), SHAPE_CYLINDER, 0.05, SURF_FLESH, "Ingredients"),
    ("Sausage", sausage, SHAPE_BOX, 0.15, SURF_FLESH, "Ingredients"),
    ("Hot Dog Bun", hot_dog_bun, SHAPE_BOX, 0.15, SURF_GENERIC, "Ingredients"),
]
# Every burger burger.lua can put together: "Burger" plain, "Burger C", "Burger CL"... (letters in C L T order)
for _n in range(8):
    _t = "".join(t for k, t in enumerate("CLT") if _n & (1 << k))
    PROPS.append(("Burger" + (" " + _t if _t else ""), (lambda t: lambda a: burger(a, t))(_t), SHAPE_CYLINDER, 0.6,
                  SURF_FLESH, "Food"))
PROPS += [
    ("Hot Dog", hot_dog, SHAPE_BOX, 0.4, SURF_FLESH, "Food"),
    ("Fries", fries, SHAPE_BOX, 0.4, SURF_CARDBOARD, "Food"),
    ("Soda", soda, SHAPE_CYLINDER, 0.5, SURF_PLASTIC, "Food"),
    ("Ketchup", lambda a: squeeze_bottle(a, "ketchup"), SHAPE_CYLINDER, 0.5, SURF_PLASTIC, "Food"),
    ("Mustard", lambda a: squeeze_bottle(a, "mustard"), SHAPE_CYLINDER, 0.5, SURF_PLASTIC, "Food"),
    ("Counter", counter, SHAPE_BOX, 250.0, SURF_METAL, "Kitchen"),
    ("Grill", grill, SHAPE_BOX, 300.0, SURF_METAL, "Kitchen"),
    ("Fryer", fryer, SHAPE_BOX, 200.0, SURF_METAL, "Kitchen"),
    ("Fridge", lambda a: fridge(a, "fridge_meat"), SHAPE_BOX, 300.0, SURF_METAL, "Kitchen"),
    ("Freezer", lambda a: fridge(a, "fridge_dogs"), SHAPE_BOX, 300.0, SURF_METAL, "Kitchen"),
    ("Soda Machine", soda_machine, SHAPE_BOX, 250.0, SURF_METAL, "Kitchen"),
    ("Trash Can", trash_can, SHAPE_CYLINDER, 40.0, SURF_METAL, "Kitchen"),
    ("Tray", tray, SHAPE_BOX, 1.0, SURF_PLASTIC, "Kitchen"),
    ("Cutting Board", cutting_board, SHAPE_BOX, 3.0, SURF_WOOD, "Kitchen"),
    ("Bun Rack", bun_rack, SHAPE_BOX, 4.0, SURF_METAL, "Kitchen"),
    ("Dog Bun Box", dog_bun_box, SHAPE_BOX, 2.0, SURF_CARDBOARD, "Kitchen"),
    ("Cheese Bin", lambda a: topping_bin(a, "cheese"), SHAPE_BOX, 3.0, SURF_METAL, "Kitchen"),
    ("Lettuce Bin", lambda a: topping_bin(a, "lettuce"), SHAPE_BOX, 3.0, SURF_METAL, "Kitchen"),
    ("Tomato Bin", lambda a: topping_bin(a, "tomato"), SHAPE_BOX, 3.0, SURF_METAL, "Kitchen"),
    ("Stool", stool, SHAPE_CYLINDER, 7.0, SURF_METAL, "Dining"),
    ("Table", table, SHAPE_CYLINDER, 30.0, SURF_METAL, "Dining"),
    ("Plant", plant, SHAPE_CYLINDER, 9.0, SURF_GENERIC, "Dining"),
    ("Wet Floor Sign", wet_floor_sign, SHAPE_BOX, 2.0, SURF_PLASTIC, "Dining"),
    ("Traffic Cone", traffic_cone, SHAPE_CYLINDER, 2.0, SURF_PLASTIC, "Dining"),
]


def texture_record(txdt, img, max_dim, levels):
    """Converts an image into the TXDT blob; returns its TEXS fields (offsets relative to the blob)."""
    pt = pspimage.convert(img, max_dim, False, max_levels=levels)
    clut_ofs = len(txdt)
    txdt += pt.clut
    lv = []
    for lvl in pt.levels:
        txdt += b"\0" * ((-len(txdt)) % 16)
        lv.append(len(txdt))
        txdt += lvl
    txdt += b"\0" * ((-len(txdt)) % 16)
    return (pt.width, pt.height, len(pt.levels), 0, clut_ofs, lv)


def pack_texs(w, tex_records):
    base = w.section_offset("TXDT")
    out = bytearray()
    for width, height, levels, flags, clut_ofs, level_ofs in tex_records:
        lv = [base + o for o in level_ofs] + [0] * (5 - len(level_ofs))
        out += struct.pack("<HHBBHI5I", width, height, levels, flags, 0, base + clut_ofs, *lv)
    w.replace("TEXS", out)


def build(atlas, out_path, log=print):
    txdt = bytearray()
    tex_records = [texture_record(txdt, atlas.img, 256, 3)]
    model_data, mesh_data, vert_data, idx_data, hull_data = bytearray(), bytearray(), bytearray(), bytearray(), bytearray()
    nverts = nidx = 0
    sizes = {}
    for k, (name, make, shape, mass, surface, cat) in enumerate(PROPS):
        m = make(atlas)
        m.recentre()
        mn, mx = m.bounds()
        sizes[name] = tuple(mx[i] - mn[i] for i in range(3))
        if len(m.verts) > 65535:
            raise ValueError(f"{name}: too many vertices")
        mesh_data += struct.pack("<4I2H", nverts, len(m.verts), nidx, len(m.tris) * 3, 0, 0)
        for u, v, nx, ny, nz, x, y, z, _b in m.verts:
            vert_data += struct.pack("<8f", u, v, nx, ny, nz, x, y, z)
        for t in m.tris:
            idx_data += struct.pack("<3H", *t)
        nverts += len(m.verts)
        nidx += len(m.tris) * 3
        flags = shape | (CATEGORIES.index(cat) << 16)
        model_data += struct.pack("<32s6ff5I", name.encode()[:31], *mn, *mx, mass, k, 1, len(m.tris), flags, surface)
        hull_data += struct.pack("<4I", 0, 0, 0, 0)
        log(f"  {name:16s} {len(m.tris):4d} tris  {sizes[name][0]:.0f} x {sizes[name][1]:.0f} x {sizes[name][2]:.0f}")
    w = PmapWriter(PPAK_MAGIC, PPAK_VERSION)
    w.add("MDLS", model_data, len(PROPS))
    w.add("MESH", mesh_data, len(PROPS))
    w.add("VERT", vert_data, nverts)
    w.add("INDX", idx_data, nidx)
    w.add("TEXS", bytes(32 * len(tex_records)), len(tex_records))
    w.add("TXDT", txdt, len(txdt))
    w.add("CATS", b"".join(struct.pack("<24s", c.encode()[:23]) for c in CATEGORIES), len(CATEGORIES))
    w.add("HULL", hull_data, len(PROPS))
    w.add("HPCE", b"", 0)
    w.add("HPLN", b"", 0)
    w.add("HPNT", b"", 0)
    pack_texs(w, tex_records)
    size = w.write(out_path)
    log(f"  wrote {out_path}: {len(PROPS)} props, {size / 1024:.0f} KB")
    return sizes
