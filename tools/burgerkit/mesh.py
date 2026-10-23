"""Low-poly geometry: boxes, prisms and cylinders built from flat-shaded faces.

Every face gets its own vertices and normal, so models look faceted (the low-poly look) and each face can
take its own atlas cell. Triangles wind counter-clockwise seen from outside, as the engine culls them.
Vertices are (u, v, nx, ny, nz, x, y, z, bone); props ignore the bone.
"""

import math


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def scale(a, k):
    return (a[0] * k, a[1] * k, a[2] * k)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def norm(a):
    n = math.sqrt(dot(a, a)) or 1.0
    return (a[0] / n, a[1] / n, a[2] / n)


def rot_z(p, deg):
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    return (p[0] * c - p[1] * s, p[0] * s + p[1] * c, p[2])


def rot_y(p, deg):
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    return (p[0] * c + p[2] * s, p[1], -p[0] * s + p[2] * c)


class Mesh:
    def __init__(self):
        self.verts = []
        self.tris = []

    def poly(self, pts, uv, bone=0):
        """A flat convex polygon (points counter-clockwise from outside) filling atlas rect uv=(u0, v0, u1, v1).

        The rect is mapped by the polygon's own 2D layout, so patterns (stripes, sesame) land the right way up.
        """
        n = norm(cross(sub(pts[1], pts[0]), sub(pts[2], pts[0])))
        # Project onto the face plane: the first edge is "right", the up is normal x right
        right = norm(sub(pts[1], pts[0]))
        up = cross(n, right)
        if abs(n[2]) < 0.7:
            # Walls: keep the texture's up pointing up the world
            right = norm(cross((0.0, 0.0, 1.0), n))
            up = cross(n, right)
        flat = [(dot(p, right), dot(p, up)) for p in pts]
        x0, x1 = min(f[0] for f in flat), max(f[0] for f in flat)
        y0, y1 = min(f[1] for f in flat), max(f[1] for f in flat)
        u0, v0, u1, v1 = uv
        base = len(self.verts)
        for p, (fx, fy) in zip(pts, flat):
            s = (fx - x0) / (x1 - x0) if x1 > x0 else 0.5
            t = (fy - y0) / (y1 - y0) if y1 > y0 else 0.5
            self.verts.append((u0 + (u1 - u0) * s, v1 - (v1 - v0) * t, n[0], n[1], n[2], p[0], p[1], p[2], bone))
        for k in range(1, len(pts) - 1):
            self.tris.append((base, base + k, base + k + 1))

    def box(self, mins, maxs, cells, bone=0):
        """An axis-aligned box. cells: one uv rect for every face, or a dict of faces
        ("top", "bottom", "front" (+x), "back", "left" (+y), "right") with "side" as the default."""
        x0, y0, z0 = mins
        x1, y1, z1 = maxs
        faces = {
            "top": [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)],
            "bottom": [(x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0)],
            "front": [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)],
            "back": [(x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1)],
            "left": [(x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1)],
            "right": [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)],
        }
        for name, pts in faces.items():
            uv = cells if not isinstance(cells, dict) else cells.get(name, cells.get("side"))
            if uv is not None:
                self.poly(pts, uv, bone)

    def prism(self, sides, r0, r1, z0, z1, cells, centre=(0.0, 0.0), turn=0.0, bone=0, caps=(True, True)):
        """A tapered n-sided prism along Z (a cylinder when sides is big enough): radius r0 at z0, r1 at z1.
        cells: one uv rect, or a dict with "top", "bottom", "side"."""
        get = (lambda k: cells) if not isinstance(cells, dict) else (lambda k: cells.get(k, cells.get("side")))
        cx, cy = centre
        ring = []
        for i in range(sides):
            a = math.radians(turn + 360.0 * i / sides)
            ring.append((math.cos(a), math.sin(a)))
        lo = [(cx + c * r0, cy + s * r0, z0) for c, s in ring]
        hi = [(cx + c * r1, cy + s * r1, z1) for c, s in ring]
        for i in range(sides):
            j = (i + 1) % sides
            self.poly([lo[i], lo[j], hi[j], hi[i]], get("side"), bone)
        if caps[1] and r1 > 0.01 and get("top") is not None:
            self.poly(hi, get("top"), bone)
        if caps[0] and r0 > 0.01 and get("bottom") is not None:
            self.poly(list(reversed(lo)), get("bottom"), bone)

    def merge(self, other, offset=(0.0, 0.0, 0.0), turn=0.0):
        """Adds another mesh, turned turn degrees round Z then moved by offset."""
        base = len(self.verts)
        for u, v, nx, ny, nz, x, y, z, b in other.verts:
            p = add(rot_z((x, y, z), turn), offset)
            n = rot_z((nx, ny, nz), turn)
            self.verts.append((u, v, n[0], n[1], n[2], p[0], p[1], p[2], b))
        for a, b_, c in other.tris:
            self.tris.append((a + base, b_ + base, c + base))

    def bounds(self):
        xs = [v[5] for v in self.verts]
        ys = [v[6] for v in self.verts]
        zs = [v[7] for v in self.verts]
        return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))

    def recentre(self):
        """Moves it so its bounding box is centred on the origin (the physics body sits at the box's centre)."""
        mn, mx = self.bounds()
        c = tuple((mn[k] + mx[k]) / 2 for k in range(3))
        self.verts = [(u, v, nx, ny, nz, x - c[0], y - c[1], z - c[2], b) for u, v, nx, ny, nz, x, y, z, b in self.verts]
        return c
