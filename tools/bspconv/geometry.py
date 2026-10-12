"""Builds renderable triangles from BSP faces and displacements.

Lighting is baked into vertex colours, since the PSP can't afford a lightmap pass.
Big faces are cut on a world-space grid so the vertex lighting has enough points to follow
the lightmap, and so no triangle is huge (the GE drops triangles that reach too far off-screen).
Cutting on a *global* grid splits shared edges at the same points on both faces, so no cracks appear.
"""

import math

import numpy as np
from PIL import Image

from .bsp import SURF_BUMPLIGHT

EPS = 0.01


def decode_lighting(lump):
    """Whole lighting lump -> (N, 3) float32 linear RGB. Each ColorRGBExp32 is byte * 2^exponent / 255."""
    if not lump:
        return np.zeros((0, 3), np.float32)
    raw = np.frombuffer(lump, dtype=np.uint8).reshape(-1, 4)
    exp = raw[:, 3].astype(np.int8).astype(np.float32)
    return raw[:, :3].astype(np.float32) * (np.exp2(exp) / 255.0)[:, None]


def dot3(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def split_poly(poly, axis, value):
    """Splits a convex polygon on the plane coord[axis] == value. Returns (below, above)."""
    below, above = [], []
    n = len(poly)
    for i in range(n):
        a = poly[i]
        b = poly[(i + 1) % n]
        da = a[axis] - value
        db = b[axis] - value
        if da <= EPS:
            below.append(a)
        if da >= -EPS:
            above.append(a)
        if (da < -EPS and db > EPS) or (da > EPS and db < -EPS):
            t = da / (da - db)
            p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)
            p = tuple(value if k == axis else p[k] for k in range(3))
            below.append(p)
            above.append(p)
    return below, above


def tessellate(poly, grid):
    pieces = [poly]
    for axis in range(3):
        lo = min(p[axis] for p in poly)
        hi = max(p[axis] for p in poly)
        k0 = math.floor(lo / grid) + 1
        k1 = math.ceil(hi / grid) - 1
        for k in range(k0, k1 + 1):
            v = k * grid
            nxt = []
            for pc in pieces:
                a, b = split_poly(pc, axis, v)
                if len(a) >= 3:
                    nxt.append(a)
                if len(b) >= 3:
                    nxt.append(b)
            pieces = nxt
    return pieces


class Lightmap:
    """Bilinear lookup into one of a face's lightmaps (slot 0-3, one per light style)."""

    def __init__(self, bsp, face, lighting, slot=0):
        """lighting is the decoded lump from decode_lighting()."""
        self.ok = False
        if face.lightofs < 0 or face.styles[slot] == 255 or len(lighting) == 0:
            return
        ti = bsp.texinfo[face.texinfo]
        self.w = face.lm_size[0] + 1
        self.h = face.lm_size[1] + 1
        self.mins = face.lm_mins
        self.s_vec, self.t_vec = ti.lm_s, ti.lm_t
        n = self.w * self.h
        # Each light style has its own lightmap; bumped faces store 4 per style (the first is the plain one).
        per_style = n * (4 if ti.flags & SURF_BUMPLIGHT else 1)
        first = face.lightofs // 4 + slot * per_style
        if first + n > len(lighting):
            return
        self.texels = lighting[first : first + n].tolist()
        self.ok = True

    def luxel_coords(self, p):
        s = dot3(p, self.s_vec) + self.s_vec[3] - self.mins[0]
        t = dot3(p, self.t_vec) + self.t_vec[3] - self.mins[1]
        return s, t

    def sample_st(self, s, t):
        s = min(max(s, 0.0), self.w - 1.0)
        t = min(max(t, 0.0), self.h - 1.0)
        s0, t0 = int(s), int(t)
        s1, t1 = min(s0 + 1, self.w - 1), min(t0 + 1, self.h - 1)
        fs, ft = s - s0, t - t0
        tx = self.texels
        a, b = tx[t0 * self.w + s0], tx[t0 * self.w + s1]
        c, d = tx[t1 * self.w + s0], tx[t1 * self.w + s1]
        return tuple(
            (a[k] * (1 - fs) + b[k] * fs) * (1 - ft) + (c[k] * (1 - fs) + d[k] * fs) * ft for k in range(3)
        )

    def sample(self, p):
        return self.sample_st(*self.luxel_coords(p))

    def world_units_per_luxel(self):
        n = math.sqrt(self.s_vec[0] ** 2 + self.s_vec[1] ** 2 + self.s_vec[2] ** 2)
        return 1.0 / n if n > 1e-6 else 16.0

    def interp_error(self, step_luxels):
        """Worst-case colour error (0-255) if lighting were only sampled every step_luxels and interpolated.

        It roughly predicts how a tessellation grid of that spacing would look with vertex lighting.
        """
        if step_luxels <= 1.0 or (self.w <= 2 and self.h <= 2):
            return 0.0
        g = (np.asarray(self.texels, np.float32).reshape(self.h, self.w, 3) * 0.5) ** (1.0 / 2.2) * 255.0
        sw = max(2, int(math.ceil((self.w - 1) / step_luxels)) + 1)
        sh = max(2, int(math.ceil((self.h - 1) / step_luxels)) + 1)
        if sw >= self.w and sh >= self.h:
            return 0.0
        worst = 0.0
        for ch in range(3):
            img = Image.fromarray(np.ascontiguousarray(g[:, :, ch]), "F")
            back = img.resize((sw, sh), Image.Resampling.BILINEAR).resize((self.w, self.h), Image.Resampling.BILINEAR)
            worst = max(worst, float(np.abs(np.asarray(back) - g[:, :, ch]).max()))
        return worst


def light_to_abgr(lin, alpha=255):
    """Linear lightmap colour -> vertex colour, for drawing with the GE's 2x colour doubling.

    It matches Source's LDR path: the lightmap is stored as gamma(linear / 2) and the shader doubles it.
    """
    out = []
    for c in lin:
        v = (max(c, 0.0) * 0.5) ** (1.0 / 2.2)
        out.append(min(255, int(v * 255.0 + 0.5)))
    return (alpha << 24) | (out[2] << 16) | (out[1] << 8) | out[0]


FULLBRIGHT = light_to_abgr((1.0, 1.0, 1.0))


class FaceLight:
    """All of a face's lightmaps: style 0 (always on) makes the vertex colour; every other style
    (switchable or flickering lights) becomes a per-vertex colour delta the PSP adds at runtime."""

    def __init__(self, bsp, face, lighting):
        self.base = []
        self.extra = []
        for slot in range(4):
            style = face.styles[slot]
            if style == 255:
                break
            lm = Lightmap(bsp, face, lighting, slot)
            if lm.ok:
                (self.base if style == 0 else self.extra).append((style, lm))
        self.ok = bool(self.base or self.extra)
        self.lm = self.base[0][1] if self.base else (self.extra[0][1] if self.extra else None)

    def at(self, p):
        """(vertex colour, ((style, dr, dg, db), ...)) at point p."""
        if not self.ok:
            return FULLBRIGHT, ()
        l0 = [0.0, 0.0, 0.0]
        for _s, lm in self.base:
            c = lm.sample(p)
            l0 = [l0[k] + c[k] for k in range(3)]
        base = light_to_abgr(l0)
        styles = []
        for style, lm in self.extra:
            ls = lm.sample(p)
            if max(ls) < 1e-4:
                continue
            lit = light_to_abgr([l0[k] + ls[k] for k in range(3)])
            d = tuple(((lit >> s) & 255) - ((base >> s) & 255) for s in (0, 8, 16))
            if any(d):
                styles.append((style,) + d)
        return base, tuple(styles)


class FaceMesh:
    """Triangles for one face: verts are (u, v, abgr, x, y, z, styles), tris are index triples.
    styles holds (style, dr, dg, db) colour deltas for switchable/animated lights."""

    def __init__(self):
        self.verts = []
        self.tris = []


def _tex_uv(ti, td, p):
    u = (dot3(p, ti.tex_s) + ti.tex_s[3]) / max(1, td.width)
    v = (dot3(p, ti.tex_t) + ti.tex_t[3]) / max(1, td.height)
    return u, v


def choose_grid(lm, grid_min, grid_max, max_error):
    """Coarsest power-of-two grid (between grid_min and grid_max) whose vertex lighting stays within max_error."""
    if not lm.ok:
        return grid_max
    upl = lm.world_units_per_luxel()
    g = grid_max
    while g > grid_min and lm.interp_error(g / upl) > max_error:
        g /= 2
    return g


def build_face(bsp, face, lighting, grid_min, grid_max, max_error, offset=(0.0, 0.0, 0.0)):
    ti = bsp.texinfo[face.texinfo]
    td = bsp.texdata[ti.texdata]
    fl = FaceLight(bsp, face, lighting)
    grid = choose_grid(fl.lm, grid_min, grid_max, max_error) if fl.lm else grid_max
    poly = bsp.face_vertices(face)
    mesh = FaceMesh()
    index = {}

    def vert(p):
        key = (round(p[0], 2), round(p[1], 2), round(p[2], 2))
        i = index.get(key)
        if i is None:
            u, v = _tex_uv(ti, td, p)
            col, styles = fl.at(p)
            i = len(mesh.verts)
            index[key] = i
            mesh.verts.append((u, v, col, p[0] + offset[0], p[1] + offset[1], p[2] + offset[2], styles))
        return i

    for piece in tessellate(poly, grid):
        ids = [vert(p) for p in piece]
        for k in range(1, len(ids) - 1):
            if ids[0] != ids[k] and ids[k] != ids[k + 1] and ids[0] != ids[k + 1]:
                mesh.tris.append((ids[0], ids[k], ids[k + 1]))
    return mesh


def build_displacement(bsp, face, lighting, max_power=4):
    """Displacement grid mesh. max_power below the map's power keeps every 2nd/4th row and column."""
    di = bsp.dispinfo[face.dispinfo]
    ti = bsp.texinfo[face.texinfo]
    td = bsp.texdata[ti.texdata]
    corners = bsp.face_vertices(face)
    if len(corners) != 4:
        return FaceMesh()
    # The displacement grid starts at whichever corner is nearest start_pos.
    sp = di.start_pos
    start = min(range(4), key=lambda i: sum((corners[i][k] - sp[k]) ** 2 for k in range(3)))
    c = corners[start:] + corners[:start]
    full = (1 << di.power) + 1
    power = min(di.power, max_power)
    stride = 1 << (di.power - power)
    n = (1 << power) + 1
    fl = FaceLight(bsp, face, lighting)

    mesh = FaceMesh()
    for i in range(n):
        fi = i / (n - 1)
        e0 = tuple(c[0][k] + (c[1][k] - c[0][k]) * fi for k in range(3))
        e1 = tuple(c[3][k] + (c[2][k] - c[3][k]) * fi for k in range(3))
        for j in range(n):
            fj = j / (n - 1)
            base = tuple(e0[k] + (e1[k] - e0[k]) * fj for k in range(3))
            dv = bsp.disp_verts[di.vert_start + (i * stride) * full + j * stride]
            pos = tuple(base[k] + dv[k] * dv[3] for k in range(3))
            # Texture and lightmap coordinates come from the flat base surface, as in Source.
            u, v = _tex_uv(ti, td, base)
            col, styles = fl.at(base)
            mesh.verts.append((u, v, col, pos[0], pos[1], pos[2], styles))

    for i in range(n - 1):
        for j in range(n - 1):
            a = i * n + j
            b = a + 1
            cc = a + n
            d = cc + 1
            # Alternating diagonals, as Source does, keep the surface symmetric.
            if (i + j) % 2 == 0:
                mesh.tris.append((a, cc, d))
                mesh.tris.append((a, d, b))
            else:
                mesh.tris.append((a, cc, b))
                mesh.tris.append((b, cc, d))

    return mesh


def face_front_normal(bsp, face):
    """The direction a face is visible from, taken from its vertex order.

    Source winds every face clockwise seen from the front, so this is the negated right-hand-rule normal.
    Don't use planenum + side: dface_t.side refers to the BSP node, and flipping by it turned about
    45% of faces inside out.
    """
    vs = bsp.face_vertices(face)
    nx = ny = nz = 0.0
    for i in range(len(vs)):
        a, c = vs[i], vs[(i + 1) % len(vs)]
        nx += (a[1] - c[1]) * (a[2] + c[2])
        ny += (a[2] - c[2]) * (a[0] + c[0])
        nz += (a[0] - c[0]) * (a[1] + c[1])
    return (-nx, -ny, -nz)


def make_ccw(bsp, face, mesh):
    """Flips triangles so they wind counter-clockwise seen from the face's front.

    Brush faces (clockwise in Source) and displacements then share one convention for culling.
    """
    normal = face_front_normal(bsp, face)
    if mesh.tris and _winding_sign(mesh, normal) < 0:
        mesh.tris = [(t[0], t[2], t[1]) for t in mesh.tris]


def _winding_sign(mesh, normal):
    total = 0.0
    for a, b, c in mesh.tris:
        pa, pb, pc = mesh.verts[a][3:6], mesh.verts[b][3:6], mesh.verts[c][3:6]
        e1 = [pb[k] - pa[k] for k in range(3)]
        e2 = [pc[k] - pa[k] for k in range(3)]
        cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        total += dot3(cr, normal)
    return total
