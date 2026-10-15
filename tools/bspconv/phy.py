"""Reader for Source .phy collision models: the convex pieces ("ledges") of each solid.

.phy files store Havok/IVP compact surfaces. Points are in metres with IVP's axes; Valve converts them
with x' = x, y' = z, z' = -y and 1 m = 39.37 in.
"""

import struct

METERS_TO_INCHES = 39.3701


def _ivp_to_source(x, y, z):
    return (x * METERS_TO_INCHES, z * METERS_TO_INCHES, -y * METERS_TO_INCHES)


def _ledge_points(data, pos):
    """Points of the convex ledge at pos: every vertex its triangles' edges start at."""
    point_ofs, _client, _bits, ntri = struct.unpack_from("<iiIh", data, pos)
    idx = set()
    for t in range(ntri):
        tp = pos + 16 + t * 16
        for e in range(3):
            idx.add(struct.unpack_from("<I", data, tp + 4 + e * 4)[0] & 0xFFFF)
    pts = []
    for i in sorted(idx):
        o = pos + point_ofs + i * 16
        if o + 12 > len(data):
            return []
        pts.append(_ivp_to_source(*struct.unpack_from("<3f", data, o)))
    return pts


def _ledges(data, surf):
    """Yields lists of points (Source units) for each convex ledge of a compact surface at offset surf.

    The ledges hang off a binary tree (IVP_Compact_Ledgetree_Node, 28 bytes): a node whose right-child
    offset is 0 is a leaf holding one ledge, otherwise its left child follows it directly.
    """
    if surf + 48 > len(data):
        return
    root = struct.unpack_from("<i", data, surf + 32)[0]
    if root <= 0:
        return
    stack = [surf + root]
    seen = set()
    while stack:
        node = stack.pop()
        if node in seen or node + 28 > len(data):
            continue
        seen.add(node)
        right, ledge = struct.unpack_from("<ii", data, node)
        if right == 0:
            pts = _ledge_points(data, node + ledge)
            if len(pts) >= 4:
                yield pts
        else:
            stack.append(node + 28)
            stack.append(node + right)


def read_convex_pieces(data):
    """All convex pieces of all solids in a .phy, each a list of (x, y, z) in model space. [] if unreadable."""
    if not data or len(data) < 16:
        return []
    header_size, _id, solid_count = struct.unpack_from("<iii", data, 0)
    pos = header_size
    pieces = []
    for _ in range(solid_count):
        if pos + 4 > len(data):
            break
        size = struct.unpack_from("<i", data, pos)[0]
        start = pos + 4
        if data[start : start + 4] == b"VPHY":
            surf = start + 28  # compactsurfaceheader_t
        else:
            surf = start  # older files: the compact surface directly
        try:
            pieces.extend(_ledges(data[: start + size], surf))
        except struct.error:
            pass
        pos = start + size
    return pieces


def align_to_mesh(pieces, mesh_points):
    """Turns the pieces to sit on the model, for the few whose .phy is turned about the vertical axis
    (models with a rotated root bone: HL2's Jeep 180 degrees, GMod's seats 90). Ordinary props' hulls
    already match their mesh and come back as they are."""
    pts = [p for pc in pieces for p in pc]
    if not pts or not mesh_points:
        return pieces

    def bounds(ps):
        return [min(p[k] for p in ps) for k in range(3)], [max(p[k] for p in ps) for k in range(3)]

    def turn(p, q):  # q quarter turns about +Z
        x, y, z = p
        for _ in range(q):
            x, y = -y, x
        return (x, y, z)

    mlo, mhi = bounds(mesh_points)
    size = max(max(mhi[k] - mlo[k] for k in range(3)), 1.0)
    hlo, hhi = bounds(pts)
    if sum(abs(hlo[k] - mlo[k]) + abs(hhi[k] - mhi[k]) for k in range(2)) < 0.15 * size:
        return pieces
    sample = mesh_points[:: max(1, len(mesh_points) // 300)]
    hull_sample = pts[:: max(1, len(pts) // 200)]

    def fit(q):  # mean distance from the turned hull's points to the nearest mesh vertex
        total = 0.0
        for p in hull_sample:
            t = turn(p, q)
            total += min((t[0] - s[0]) ** 2 + (t[1] - s[1]) ** 2 + (t[2] - s[2]) ** 2 for s in sample) ** 0.5
        return total / len(hull_sample)

    best = min(range(4), key=lambda q: (fit(q), q))
    if best == 0:
        return pieces
    return [[turn(p, best) for p in pc] for pc in pieces]


def hull_planes(points):
    """Outward planes (nx, ny, nz, d) of a convex point set, via a brute-force hull (point sets are small)."""
    n = len(points)
    cx = sum(p[0] for p in points) / n
    cy = sum(p[1] for p in points) / n
    cz = sum(p[2] for p in points) / n
    planes = []
    eps = 0.05
    for i in range(n):
        for j in range(i + 1, n):
            for k in range(j + 1, n):
                a, b, c = points[i], points[j], points[k]
                u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
                v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
                nx, ny, nz = u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]
                ln = (nx * nx + ny * ny + nz * nz) ** 0.5
                if ln < 1e-6:
                    continue
                nx, ny, nz = nx / ln, ny / ln, nz / ln
                d = nx * a[0] + ny * a[1] + nz * a[2]
                if nx * cx + ny * cy + nz * cz > d:  # face the centroid away
                    nx, ny, nz, d = -nx, -ny, -nz, -d
                if all(nx * p[0] + ny * p[1] + nz * p[2] <= d + eps for p in points):
                    if not any(abs(nx - q[0]) < 1e-3 and abs(ny - q[1]) < 1e-3 and abs(nz - q[2]) < 1e-3
                               and abs(d - q[3]) < 0.1 for q in planes):
                        planes.append((nx, ny, nz, d))
    return planes


def simplify(points, max_points):
    """Farthest-point sampling down to max_points, keeping the hull's extremes."""
    if len(points) <= max_points:
        return list(points)
    chosen = [max(points, key=lambda p: p[2])]
    dist = [sum((p[k] - chosen[0][k]) ** 2 for k in range(3)) for p in points]
    while len(chosen) < max_points:
        i = max(range(len(points)), key=lambda j: dist[j])
        chosen.append(points[i])
        for j, p in enumerate(points):
            dist[j] = min(dist[j], sum((p[k] - points[i][k]) ** 2 for k in range(3)))
    return chosen
