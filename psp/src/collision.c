#include "collision.h"

#include <float.h>
#include <stddef.h>
#include <string.h>

#include "ents.h"

// Keep traces this far off surfaces so the next move doesn't start inside them (Quake's DIST_EPSILON).
#define DIST_EPSILON 0.03125f
#define MAX_LEAVES 512

typedef struct {
    vec3 start, end, mins, maxs;
    unsigned int mask;
    Trace *tr;
} TraceWork;

// Clips the swept box against one convex set of planes (Quake 3's CM_ClipBoxToBrush).
// A point is inside when it's behind every plane.
static void clip_to_planes(TraceWork *w, const PmapPlane *const *planes, int n, int hit_id) {
    float enter = -1.0f, leave = 1.0f;
    const PmapPlane *clip = NULL;
    int startout = 0, getout = 0;
    for (int i = 0; i < n; i++) {
        const PmapPlane *p = planes[i];
        // Box corner furthest into the plane
        float ofs = (p->nx < 0.0f ? w->maxs.x : w->mins.x) * p->nx + (p->ny < 0.0f ? w->maxs.y : w->mins.y) * p->ny +
                    (p->nz < 0.0f ? w->maxs.z : w->mins.z) * p->nz;
        float dist = p->dist - ofs;
        float d1 = p->nx * w->start.x + p->ny * w->start.y + p->nz * w->start.z - dist;
        float d2 = p->nx * w->end.x + p->ny * w->end.y + p->nz * w->end.z - dist;
        if (d2 > 0.0f) getout = 1;
        if (d1 > 0.0f) startout = 1;
        // Completely in front of this plane: no contact with the brush
        if (d1 > 0.0f && (d2 >= DIST_EPSILON || d2 >= d1)) {
            return;
        }
        if (d1 <= 0.0f && d2 <= 0.0f) {
            continue;
        }
        if (d1 > d2) { // entering
            float f = (d1 - DIST_EPSILON) / (d1 - d2);
            if (f < 0.0f) f = 0.0f;
            if (f > enter) {
                enter = f;
                clip = p;
            }
        } else { // leaving
            float f = (d1 + DIST_EPSILON) / (d1 - d2);
            if (f > 1.0f) f = 1.0f;
            if (f < leave) leave = f;
        }
    }
    Trace *tr = w->tr;
    // Starting inside a prop, NPC or player (one moved into us) shouldn't trap the mover: let it walk out
    if (!startout && hit_id != TRACE_HIT_WORLD) return;
    if (!startout) {
        tr->startsolid = 1;
        if (!getout) {
            tr->allsolid = 1;
            tr->fraction = 0.0f;
            tr->hit = hit_id;
        }
        return;
    }
    if (enter < leave && enter > -1.0f && enter < tr->fraction && clip) {
        tr->fraction = enter;
        tr->normal = v3(clip->nx, clip->ny, clip->nz);
        tr->hit = hit_id;
    }
}

typedef struct {
    TraceWork *w;
    Map *m;
    vec3 ext; // half-size of the swept box, centred
} TreeWork;

// A brush's planes for a box sweep: its sides, and for box-bevel brushes (v6 maps' displacement triangles)
// the 6 axis planes of its bounds, made here in box[] from the 6 floats stored after its sides
static int brush_planes(const Map *m, const PmapBrush *b, const PmapPlane **planes, PmapPlane *box) {
    int boxed = (b->num_sides & PMAP_BRUSH_BOXBEVELS) != 0;
    int real = (int)PMAP_BRUSH_SIDES(b->num_sides), ns = real > (boxed ? 58 : 64) ? (boxed ? 58 : 64) : real;
    const uint32_t *sd = &m->brush_sides[b->first_side];
    for (int s = 0; s < ns; s++) planes[s] = &m->planes[PMAP_SIDE_PLANE(sd[s])];
    if (!boxed) return ns;
    float bb[6]; // mins xyz, maxs xyz
    memcpy(bb, sd + real, sizeof(bb));
    box[0] = (PmapPlane){1, 0, 0, bb[3]};
    box[1] = (PmapPlane){-1, 0, 0, -bb[0]};
    box[2] = (PmapPlane){0, 1, 0, bb[4]};
    box[3] = (PmapPlane){0, -1, 0, -bb[1]};
    box[4] = (PmapPlane){0, 0, 1, bb[5]};
    box[5] = (PmapPlane){0, 0, -1, -bb[2]};
    for (int k = 0; k < 6; k++) planes[ns++] = &box[k];
    return ns;
}

static void trace_leaf(TreeWork *t, int leaf_index) {
    Map *m = t->m;
    const PmapLeaf *leaf = &m->leafs[leaf_index];
    const PmapPlane *planes[64];
    PmapPlane box[6];
    for (uint32_t k = 0; k < leaf->num_brushes; k++) {
        uint32_t bi = m->leaf_brushes[leaf->first_brush + k];
        if (m->brush_check[bi] == m->check_count) continue;
        m->brush_check[bi] = m->check_count;
        const PmapBrush *b = &m->brushes[bi];
        if (!(b->contents & t->w->mask)) continue;
        int ns = brush_planes(m, b, planes, box);
        clip_to_planes(t->w, planes, ns, TRACE_HIT_WORLD);
        if (t->w->tr->allsolid) return;
    }
}

// Walks the BSP along the swept box (Quake 3's CM_TraceThroughTree), so only the leaves the move
// really passes through get tested, however long the trace is.
static void trace_tree(TreeWork *t, int node, float f1, float f2, vec3 p1, vec3 p2) {
    for (;;) {
        Trace *tr = t->w->tr;
        if (tr->allsolid || tr->fraction <= f1) return;
        if (node < 0) {
            trace_leaf(t, -1 - node);
            return;
        }
        const PmapNode *nd = &t->m->nodes[node];
        const PmapPlane *p = &t->m->planes[nd->plane];
        float d1 = p->nx * p1.x + p->ny * p1.y + p->nz * p1.z - p->dist;
        float d2 = p->nx * p2.x + p->ny * p2.y + p->nz * p2.z - p->dist;
        float ofs = fabsf(p->nx) * t->ext.x + fabsf(p->ny) * t->ext.y + fabsf(p->nz) * t->ext.z;
        if (d1 >= ofs + 1.0f && d2 >= ofs + 1.0f) {
            node = nd->children[0];
            continue;
        }
        if (d1 < -ofs - 1.0f && d2 < -ofs - 1.0f) {
            node = nd->children[1];
            continue;
        }
        // The move crosses the plane: near side up to the crossing, then far side from it
        int side;
        float fa, fb;
        if (d1 < d2) {
            float id = 1.0f / (d1 - d2);
            side = 1;
            fb = (d1 + ofs + DIST_EPSILON) * id;
            fa = (d1 - ofs + DIST_EPSILON) * id;
        } else if (d1 > d2) {
            float id = 1.0f / (d1 - d2);
            side = 0;
            fb = (d1 - ofs - DIST_EPSILON) * id;
            fa = (d1 + ofs + DIST_EPSILON) * id;
        } else {
            side = 0;
            fa = 1.0f;
            fb = 0.0f;
        }
        fa = fa < 0.0f ? 0.0f : fa > 1.0f ? 1.0f : fa;
        fb = fb < 0.0f ? 0.0f : fb > 1.0f ? 1.0f : fb;
        trace_tree(t, nd->children[side], f1, f1 + (f2 - f1) * fa, p1, v3_lerp(p1, p2, fa));
        vec3 mid = v3_lerp(p1, p2, fb);
        f1 = f1 + (f2 - f1) * fb;
        p1 = mid;
        node = nd->children[side ^ 1];
    }
}

// The exact shape of an oriented CmBox: its world-space planes, plus its bounding box's axis planes,
// which act as the bevels a box sweep needs to not catch on edges.
static void clip_oriented(TraceWork *w, const CmBox *bx) {
    PmapPlane pl[64 + 6];
    const PmapPlane *pp[64 + 6];
    vec3 ax = q_rotate(bx->rot, v3(1, 0, 0)), ay = q_rotate(bx->rot, v3(0, 1, 0)), az = q_rotate(bx->rot, v3(0, 0, 1));
    vec3 origin = v3_sub(bx->pos, q_rotate(bx->rot, bx->offset)); // model-space origin in the world
    int pieces = bx->num_pieces > 0 ? bx->num_pieces : 1;
    for (int k = 0; k < pieces; k++) {
        int n = 0;
        if (bx->num_pieces > 0) {
            const PpakHullPiece *pc = &bx->pieces[k];
            int np = pc->num_planes > 64 ? 64 : (int)pc->num_planes;
            for (int j = 0; j < np; j++) {
                const PmapPlane *mp = &bx->planes[pc->first_plane + j];
                vec3 nw = v3_add(v3_add(v3_scale(ax, mp->nx), v3_scale(ay, mp->ny)), v3_scale(az, mp->nz));
                pl[n] = (PmapPlane){nw.x, nw.y, nw.z, mp->dist + v3_dot(nw, origin)};
                n++;
            }
        } else {
            const vec3 axes[3] = {ax, ay, az};
            const float h[3] = {bx->half.x, bx->half.y, bx->half.z};
            for (int a = 0; a < 3; a++) {
                vec3 d = axes[a];
                float c = v3_dot(d, bx->pos);
                pl[n++] = (PmapPlane){d.x, d.y, d.z, c + h[a]};
                pl[n++] = (PmapPlane){-d.x, -d.y, -d.z, -c + h[a]};
            }
        }
        pl[n++] = (PmapPlane){1, 0, 0, bx->maxs.x};
        pl[n++] = (PmapPlane){-1, 0, 0, -bx->mins.x};
        pl[n++] = (PmapPlane){0, 1, 0, bx->maxs.y};
        pl[n++] = (PmapPlane){0, -1, 0, -bx->mins.y};
        pl[n++] = (PmapPlane){0, 0, 1, bx->maxs.z};
        pl[n++] = (PmapPlane){0, 0, -1, -bx->mins.z};
        for (int j = 0; j < n; j++) pp[j] = &pl[j];
        clip_to_planes(w, pp, n, bx->id);
    }
}

void cm_trace(Map *m, vec3 start, vec3 end, vec3 mins, vec3 maxs, unsigned int mask, const CmBox *boxes,
              int num_boxes, Trace *tr) {
    tr->fraction = 1.0f;
    tr->startsolid = tr->allsolid = 0;
    tr->normal = v3(0.0f, 0.0f, 0.0f);
    tr->hit = TRACE_HIT_NONE;

    TraceWork w = {start, end, mins, maxs, mask, tr};

    if (m && m->num_nodes > 0) {
        // The tree is walked with the box centred on the moving point
        vec3 c = v3_scale(v3_add(mins, maxs), 0.5f);
        TreeWork t = {&w, m, v3_scale(v3_sub(maxs, mins), 0.5f)};
        m->check_count++;
        trace_tree(&t, 0, 0.0f, 1.0f, v3_add(start, c), v3_add(end, c));
    }

    // Moving brush entities (doors, fans): trace in each entity's own space
    if (m && m->ent_state) {
        vec3 smin = v3((start.x < end.x ? start.x : end.x) + mins.x, (start.y < end.y ? start.y : end.y) + mins.y,
                       (start.z < end.z ? start.z : end.z) + mins.z);
        vec3 smax = v3((start.x > end.x ? start.x : end.x) + maxs.x, (start.y > end.y ? start.y : end.y) + maxs.y,
                       (start.z > end.z ? start.z : end.z) + maxs.z);
        for (int i = 0; i < m->num_ents && !tr->allsolid; i++) {
            const PmapEnt *e = &m->ents[i];
            const EntState *s = &m->ent_state[i];
            if (!e->solid || !e->num_brushes || e->type == PMAP_ENT_TRIGGER) continue;
            if (smin.x > s->wmax.x || smax.x < s->wmin.x || smin.y > s->wmax.y || smax.y < s->wmin.y ||
                smin.z > s->wmax.z || smax.z < s->wmin.z)
                continue;
            TraceWork lw = w;
            lw.start = q_rotate_inv(s->rot, v3_sub(start, s->pos));
            lw.end = q_rotate_inv(s->rot, v3_sub(end, s->pos));
            if (s->rotated) {
                // The box in the entity's frame: bounding box of the rotated box
                vec3 c = v3_scale(v3_add(mins, maxs), 0.5f), h = v3_scale(v3_sub(maxs, mins), 0.5f);
                vec3 ax = q_rotate_inv(s->rot, v3(1, 0, 0)), ay = q_rotate_inv(s->rot, v3(0, 1, 0)),
                     az = q_rotate_inv(s->rot, v3(0, 0, 1));
                vec3 lh = v3(fabsf(ax.x) * h.x + fabsf(ay.x) * h.y + fabsf(az.x) * h.z,
                             fabsf(ax.y) * h.x + fabsf(ay.y) * h.y + fabsf(az.y) * h.z,
                             fabsf(ax.z) * h.x + fabsf(ay.z) * h.y + fabsf(az.z) * h.z);
                vec3 lc = q_rotate_inv(s->rot, c);
                lw.mins = v3_sub(lc, lh);
                lw.maxs = v3_add(lc, lh);
            }
            float before = tr->fraction;
            const PmapPlane *planes[64];
            PmapPlane box[6];
            for (uint32_t k = 0; k < e->num_brushes; k++) {
                const PmapBrush *b = &m->brushes[m->ent_brushes[e->first_brush + k]];
                if (!(b->contents & mask)) continue;
                int ns = brush_planes(m, b, planes, box);
                clip_to_planes(&lw, planes, ns, TRACE_HIT_WORLD);
            }
            if (tr->fraction < before) tr->normal = q_rotate(s->rot, tr->normal);
        }
    }

    // Extra boxes, each treated as a 6-plane brush (or its real rotated shape)
    vec3 smin = v3((start.x < end.x ? start.x : end.x) + mins.x, (start.y < end.y ? start.y : end.y) + mins.y,
                   (start.z < end.z ? start.z : end.z) + mins.z);
    vec3 smax = v3((start.x > end.x ? start.x : end.x) + maxs.x, (start.y > end.y ? start.y : end.y) + maxs.y,
                   (start.z > end.z ? start.z : end.z) + maxs.z);
    for (int i = 0; i < num_boxes && !tr->allsolid; i++) {
        const CmBox *bx = &boxes[i];
        if (smin.x > bx->maxs.x + 1.0f || smax.x < bx->mins.x - 1.0f || smin.y > bx->maxs.y + 1.0f ||
            smax.y < bx->mins.y - 1.0f || smin.z > bx->maxs.z + 1.0f || smax.z < bx->mins.z - 1.0f)
            continue;
        if (bx->oriented) {
            clip_oriented(&w, bx);
            continue;
        }
        PmapPlane bp[6] = {
            {1, 0, 0, bx->maxs.x}, {-1, 0, 0, -bx->mins.x}, {0, 1, 0, bx->maxs.y},
            {0, -1, 0, -bx->mins.y}, {0, 0, 1, bx->maxs.z}, {0, 0, -1, -bx->mins.z},
        };
        const PmapPlane *pp[6] = {&bp[0], &bp[1], &bp[2], &bp[3], &bp[4], &bp[5]};
        clip_to_planes(&w, pp, 6, bx->id);
    }

    if (tr->fraction >= 1.0f) {
        tr->endpos = end;
    } else {
        tr->endpos = v3_lerp(start, end, tr->fraction);
    }
}

static int point_leaf(const Map *m, vec3 p) {
    return map_point_leaf(m, &p);
}

unsigned int cm_point_contents(Map *m, vec3 p) {
    if (!m || m->num_nodes == 0) return 0;
    const PmapLeaf *leaf = &m->leafs[point_leaf(m, p)];
    unsigned int contents = 0;
    for (uint32_t k = 0; k < leaf->num_brushes; k++) {
        const PmapBrush *b = &m->brushes[m->leaf_brushes[leaf->first_brush + k]];
        int inside = 1;
        for (uint32_t s = 0; s < PMAP_BRUSH_SIDES(b->num_sides) && inside; s++) {
            const PmapPlane *pl = &m->planes[PMAP_SIDE_PLANE(m->brush_sides[b->first_side + s])];
            if (pl->nx * p.x + pl->ny * p.y + pl->nz * p.z - pl->dist > 0.0f) inside = 0;
        }
        if (inside) contents |= b->contents;
    }
    return contents;
}

int cm_point_penetration(Map *m, vec3 p, unsigned int mask, vec3 *normal, float *depth) {
    if (!m || m->num_nodes == 0) return 0;
    const PmapLeaf *leaf = &m->leafs[point_leaf(m, p)];
    float best = FLT_MAX;
    for (uint32_t k = 0; k < leaf->num_brushes; k++) {
        const PmapBrush *b = &m->brushes[m->leaf_brushes[leaf->first_brush + k]];
        if (!(b->contents & mask)) continue;
        float shallow = FLT_MAX;
        vec3 n = {0, 0, 1};
        int inside = 1;
        for (uint32_t s = 0; s < PMAP_BRUSH_SIDES(b->num_sides); s++) {
            uint32_t side = m->brush_sides[b->first_side + s];
            const PmapPlane *pl = &m->planes[PMAP_SIDE_PLANE(side)];
            float d = pl->nx * p.x + pl->ny * p.y + pl->nz * p.z - pl->dist;
            if (d > 0.0f) {
                inside = 0;
                break;
            }
            // Push out through the nearest real face, never a bevel.
            if (!(side & PMAP_SIDE_BEVEL) && -d < shallow) {
                shallow = -d;
                n = v3(pl->nx, pl->ny, pl->nz);
            }
        }
        if (inside && shallow < best) {
            best = shallow;
            *normal = n;
        }
    }
    // Doors and other moving brushes push props too
    for (int i = 0; m->ent_state && i < m->num_ents; i++) {
        const PmapEnt *e = &m->ents[i];
        const EntState *s = &m->ent_state[i];
        if (!e->solid || !e->num_brushes || e->type == PMAP_ENT_TRIGGER) continue;
        if (p.x < s->wmin.x || p.x > s->wmax.x || p.y < s->wmin.y || p.y > s->wmax.y || p.z < s->wmin.z || p.z > s->wmax.z)
            continue;
        vec3 lp = q_rotate_inv(s->rot, v3_sub(p, s->pos));
        for (uint32_t k = 0; k < e->num_brushes; k++) {
            const PmapBrush *b = &m->brushes[m->ent_brushes[e->first_brush + k]];
            if (!(b->contents & mask)) continue;
            float shallow = FLT_MAX;
            vec3 n = {0, 0, 1};
            int inside = 1;
            for (uint32_t sd = 0; sd < PMAP_BRUSH_SIDES(b->num_sides); sd++) {
                uint32_t side = m->brush_sides[b->first_side + sd];
                const PmapPlane *pl = &m->planes[PMAP_SIDE_PLANE(side)];
                float d = pl->nx * lp.x + pl->ny * lp.y + pl->nz * lp.z - pl->dist;
                if (d > 0.0f) {
                    inside = 0;
                    break;
                }
                if (!(side & PMAP_SIDE_BEVEL) && -d < shallow) {
                    shallow = -d;
                    n = v3(pl->nx, pl->ny, pl->nz);
                }
            }
            if (inside && shallow < best) {
                best = shallow;
                *normal = q_rotate(s->rot, n);
            }
        }
    }
    if (best == FLT_MAX) return 0;
    *depth = best;
    return 1;
}
