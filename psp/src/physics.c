#include "physics.h"

#include <string.h>

#define STEP (1.0f / 60.0f)
#define GRAVITY 600.0f
#define MAX_CONTACTS 768
#define FRICTION 0.6f
#define RESTITUTION 0.15f
#define RESTITUTION_MIN_SPEED 60.0f
#define BAUMGARTE 0.25f
#define SLOP 0.5f
#define MAX_CORRECTION 250.0f
#define MAX_LINEAR 2500.0f
#define MAX_ANGULAR 25.0f
#define SLEEP_LINEAR 5.0f
#define SLEEP_ANGULAR 0.2f
#define SLEEP_TIME 0.5f
#define MIN_HALF 1.5f
#define WATER_DENSITY 0.0164f // kg per cubic inch
#define WATER_DRAG 1.5f
#define JOINT_BETA 0.2f
#define WHEEL_TORQUE 600000.0f // kg u^2/s^2: drives a heavy cart, as long as its wheels grip

// Options > Physics quality: solver passes per step, and how many steps a slow frame may catch up
static int g_iterations = 6;
static int g_max_steps = 3;
static int g_frame_steps = 1; // fixed steps in a frame at the target frame rate

void phys_set_frame_rate(int fps) {
    g_frame_steps = fps > 0 && fps < 60 ? (60 + fps - 1) / fps : 1;
}

void phys_set_quality(int q) {
    static const int iters[3] = {3, 4, 6}, steps[3] = {1, 2, 3};
    if (q < 1) q = 1;
    if (q > 3) q = 3;
    g_iterations = iters[q - 1];
    g_max_steps = steps[q - 1];
}

typedef struct {
    short a, b; // b = -1 for the world
    vec3 n;     // pushes a along +n, b along -n
    vec3 ra, rb, t1, t2;
    float depth, mn, mt1, mt2, bias, jn, jt1, jt2;
} Contact;

static Contact g_contacts[MAX_CONTACTS];
static int g_num_contacts;

// --- 3x3 matrix helpers for joints (row-major) ----------------------------------------------------

typedef struct {
    float m[3][3];
} mat3;

static mat3 m3_zero(void) {
    mat3 r;
    memset(&r, 0, sizeof(r));
    return r;
}

static vec3 m3_mul(const mat3 *a, vec3 v) {
    return v3(a->m[0][0] * v.x + a->m[0][1] * v.y + a->m[0][2] * v.z, a->m[1][0] * v.x + a->m[1][1] * v.y + a->m[1][2] * v.z,
              a->m[2][0] * v.x + a->m[2][1] * v.y + a->m[2][2] * v.z);
}

static int m3_inverse(const mat3 *a, mat3 *out) {
    const float(*m)[3] = a->m;
    float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (fabsf(det) < 1e-12f) return 0;
    float id = 1.0f / det;
    out->m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
    out->m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    out->m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    out->m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
    out->m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    out->m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    out->m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
    out->m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    out->m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    return 1;
}

// --- Bodies ---------------------------------------------------------------------------------------

void phys_init(PhysWorld *w) {
    memset(w, 0, sizeof(*w));
}

void phys_wake(PhysWorld *w, int i) {
    if (i < 0 || i >= MAX_BODIES) return;
    Body *b = &w->bodies[i];
    if (!b->asleep) {
        b->idle_time = 0.0f;
        return;
    }
    b->asleep = 0;
    b->idle_time = 0.0f;
    // Welded or roped neighbours wake together
    for (int j = 0; j < MAX_JOINTS; j++) {
        const Joint *jt = &w->joints[j];
        if (!jt->active) continue;
        if (jt->a == i && jt->b >= 0) phys_wake(w, jt->b);
        if (jt->b == i) phys_wake(w, jt->a);
    }
}

int phys_spawn(PhysWorld *w, int model, int shape, vec3 mins, vec3 maxs, float mass, vec3 pos, quat rot) {
    return phys_spawn_at(w, -1, model, shape, mins, maxs, mass, pos, rot);
}

int phys_spawn_at(PhysWorld *w, int slot, int model, int shape, vec3 mins, vec3 maxs, float mass, vec3 pos, quat rot) {
    int first = slot < 0 ? 0 : slot, last = slot < 0 ? MAX_BODIES : slot + 1;
    if (last > MAX_BODIES) return -1;
    for (int i = first; i < last; i++) {
        Body *b = &w->bodies[i];
        if (b->active) continue;
        memset(b, 0, sizeof(*b));
        b->active = 1;
        b->model = model;
        b->shape = shape;
        vec3 h = v3((maxs.x - mins.x) * 0.5f, (maxs.y - mins.y) * 0.5f, (maxs.z - mins.z) * 0.5f);
        if (h.x < MIN_HALF) h.x = MIN_HALF;
        if (h.y < MIN_HALF) h.y = MIN_HALF;
        if (h.z < MIN_HALF) h.z = MIN_HALF;
        if (shape == SHAPE_SPHERE) {
            float r = (h.x + h.y + h.z) / 3.0f;
            h = v3(r, r, r);
        } else if (shape == SHAPE_CYLINDER) {
            float r = (h.x + h.y) * 0.5f;
            h = v3(r, r, h.z);
        }
        b->half = h;
        b->offset = v3((maxs.x + mins.x) * 0.5f, (maxs.y + mins.y) * 0.5f, (maxs.z + mins.z) * 0.5f);
        b->pos = pos;
        b->rot = rot;
        b->mass = mass < 1.0f ? 1.0f : mass;
        b->inv_mass = 1.0f / b->mass;
        float volume;
        if (shape == SHAPE_SPHERE) {
            float r2 = h.x * h.x;
            float i = 0.4f * b->mass * r2;
            b->inv_inertia = v3(1.0f / i, 1.0f / i, 1.0f / i);
            volume = 4.18879f * r2 * h.x;
        } else if (shape == SHAPE_CYLINDER) {
            float r2 = h.x * h.x, hh = 4.0f * h.z * h.z;
            float ixy = b->mass * (3.0f * r2 + hh) / 12.0f, iz = 0.5f * b->mass * r2;
            b->inv_inertia = v3(1.0f / ixy, 1.0f / ixy, 1.0f / iz);
            volume = 3.14159f * r2 * 2.0f * h.z;
        } else {
            float hx2 = h.x * h.x, hy2 = h.y * h.y, hz2 = h.z * h.z;
            b->inv_inertia = v3(3.0f / (b->mass * (hy2 + hz2)), 3.0f / (b->mass * (hx2 + hz2)), 3.0f / (b->mass * (hx2 + hy2)));
            volume = 8.0f * h.x * h.y * h.z;
        }
        b->buoyancy = clampf(WATER_DENSITY * volume / b->mass, 0.0f, 2.5f);
        b->tint_abgr = 0xFFFFFFFF;
        b->light_age = 1000;
        return i;
    }
    return -1;
}

void phys_remove(PhysWorld *w, int i) {
    if (i < 0 || i >= MAX_BODIES) return;
    w->bodies[i].active = 0;
    for (int j = 0; j < MAX_JOINTS; j++) {
        Joint *jt = &w->joints[j];
        if (jt->active && (jt->a == i || jt->b == i)) {
            jt->active = 0;
            if (jt->a != i) phys_wake(w, jt->a);
            if (jt->b >= 0 && jt->b != i) phys_wake(w, jt->b);
        }
    }
    for (int f = 0; f < MAX_FORCES; f++) {
        if (w->forces[f].body == i) w->forces[f].active = 0;
    }
}

static int is_static(const Body *b) {
    return b->frozen || b->asleep;
}

static float body_inv_mass(const Body *b) {
    return (!b || is_static(b)) ? 0.0f : b->inv_mass;
}

static vec3 apply_inv_inertia(const Body *b, vec3 v) {
    if (!b || is_static(b)) return v3(0, 0, 0);
    vec3 l = q_rotate_inv(b->rot, v);
    l = v3(l.x * b->inv_inertia.x, l.y * b->inv_inertia.y, l.z * b->inv_inertia.z);
    return q_rotate(b->rot, l);
}

static vec3 shape_extent(const Body *b) {
    return b->half; // conservative box for every shape
}

void phys_body_aabb(const Body *b, vec3 *mins, vec3 *maxs) {
    vec3 h = shape_extent(b);
    vec3 ax = q_rotate(b->rot, v3(1, 0, 0)), ay = q_rotate(b->rot, v3(0, 1, 0)), az = q_rotate(b->rot, v3(0, 0, 1));
    vec3 e = v3(fabsf(ax.x) * h.x + fabsf(ay.x) * h.y + fabsf(az.x) * h.z, fabsf(ax.y) * h.x + fabsf(ay.y) * h.y + fabsf(az.y) * h.z,
                fabsf(ax.z) * h.x + fabsf(ay.z) * h.y + fabsf(az.z) * h.z);
    if (b->shape == SHAPE_SPHERE) e = h;
    *mins = v3_sub(b->pos, e);
    *maxs = v3_add(b->pos, e);
}

void phys_body_cmbox(const Body *b, CmBox *out, int id) {
    vec3 mins, maxs;
    phys_body_aabb(b, &mins, &maxs);
    cm_box_aabb(out, mins, maxs, id);
    if (b->shape == SHAPE_SPHERE) return; // its bounding box is close enough
    out->oriented = 1;
    out->pos = b->pos;
    out->rot = b->rot;
    out->half = b->half;
    out->offset = b->offset;
    if (b->shape == SHAPE_HULL && b->hull.num_pieces) {
        out->pieces = b->hull.pieces;
        out->num_pieces = b->hull.num_pieces;
        out->planes = b->hull.planes;
    }
}

void phys_body_matrix(const Body *b, ScePspFMatrix4 *m) {
    vec3 ax = q_rotate(b->rot, v3(1, 0, 0)), ay = q_rotate(b->rot, v3(0, 1, 0)), az = q_rotate(b->rot, v3(0, 0, 1));
    vec3 t = v3_sub(b->pos, q_rotate(b->rot, b->offset));
    m->x.x = ax.x; m->x.y = ax.y; m->x.z = ax.z; m->x.w = 0.0f;
    m->y.x = ay.x; m->y.y = ay.y; m->y.z = ay.z; m->y.w = 0.0f;
    m->z.x = az.x; m->z.y = az.y; m->z.z = az.z; m->z.w = 0.0f;
    m->w.x = t.x;  m->w.y = t.y;  m->w.z = t.z;  m->w.w = 1.0f;
}

// Surface sample points used for contacts, in local space.
static int local_points(const Body *b, vec3 *out) {
    int n = 0;
    vec3 h = b->half;
    if (b->shape == SHAPE_SPHERE) {
        for (int x = -1; x <= 1; x++)
            for (int y = -1; y <= 1; y++)
                for (int z = -1; z <= 1; z++) {
                    if (!x && !y && !z) continue;
                    out[n++] = v3_scale(v3_norm(v3((float)x, (float)y, (float)z)), h.x);
                }
    } else if (b->shape == SHAPE_CYLINDER) {
        for (int i = 0; i < 10; i++) {
            float a = (float)i * (6.2831853f / 10.0f);
            float c = cosf(a) * h.x, s = sinf(a) * h.x;
            out[n++] = v3(c, s, h.z);
            out[n++] = v3(c, s, -h.z);
        }
        out[n++] = v3(0, 0, h.z);
        out[n++] = v3(0, 0, -h.z);
        // Mid-height ring points keep a lying barrel from sagging through edges
        out[n++] = v3(h.x, 0, 0);
        out[n++] = v3(-h.x, 0, 0);
        out[n++] = v3(0, h.x, 0);
        out[n++] = v3(0, -h.x, 0);
    } else if (b->shape == SHAPE_HULL && b->hull.num_points) {
        // The hull's own vertices, moved from model space to the body's centre
        int np = b->hull.num_points > MAX_SAMPLE_POINTS ? MAX_SAMPLE_POINTS : b->hull.num_points;
        for (int i = 0; i < np; i++) {
            const float *p = b->hull.points[i];
            out[n++] = v3(p[0] - b->offset.x, p[1] - b->offset.y, p[2] - b->offset.z);
        }
    } else {
        for (int i = 0; i < 8; i++) {
            out[n++] = v3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
        }
        out[n++] = v3(h.x, 0, 0);
        out[n++] = v3(-h.x, 0, 0);
        out[n++] = v3(0, h.y, 0);
        out[n++] = v3(0, -h.y, 0);
        out[n++] = v3(0, 0, h.z);
        out[n++] = v3(0, 0, -h.z);
    }
    return n;
}

static int world_points(const Body *b, vec3 *out) {
    int n = local_points(b, out);
    for (int i = 0; i < n; i++) out[i] = v3_add(b->pos, q_rotate(b->rot, out[i]));
    return n;
}

// Is world point p inside body b? Writes the outward push normal and depth.
static int point_inside(const Body *b, vec3 p, vec3 *n, float *depth) {
    vec3 l = q_rotate_inv(b->rot, v3_sub(p, b->pos));
    vec3 h = b->half;
    vec3 ln;
    if (b->shape == SHAPE_HULL && b->hull.num_pieces) {
        // Inside the union of convex pieces: push out of the piece the point is deepest in,
        // through that piece's nearest face.
        vec3 m = v3_add(l, b->offset); // model space
        float best = 0.0f;
        ln = v3(0, 0, 1);
        for (int k = 0; k < b->hull.num_pieces; k++) {
            const PpakHullPiece *pc = &b->hull.pieces[k];
            float exit = -1e9f;
            const PmapPlane *exit_pl = NULL;
            for (uint32_t j = 0; j < pc->num_planes; j++) {
                const PmapPlane *pl = &b->hull.planes[pc->first_plane + j];
                float d = pl->nx * m.x + pl->ny * m.y + pl->nz * m.z - pl->dist;
                if (d > exit) {
                    exit = d;
                    exit_pl = pl;
                }
            }
            if (exit < 0.0f && -exit > best && exit_pl) {
                best = -exit;
                ln = v3(exit_pl->nx, exit_pl->ny, exit_pl->nz);
            }
        }
        if (best <= 0.0f) return 0;
        *depth = best;
        *n = q_rotate(b->rot, ln);
        return 1;
    }
    if (b->shape == SHAPE_SPHERE) {
        float d = v3_len(l);
        if (d >= h.x) return 0;
        *depth = h.x - d;
        ln = d > 1e-4f ? v3_scale(l, 1.0f / d) : v3(0, 0, 1);
    } else if (b->shape == SHAPE_CYLINDER) {
        float rd = sqrtf(l.x * l.x + l.y * l.y);
        float pr = h.x - rd, pz = h.z - fabsf(l.z);
        if (pr <= 0.0f || pz <= 0.0f) return 0;
        if (pz < pr || rd < 1e-4f) {
            ln = v3(0, 0, l.z < 0.0f ? -1.0f : 1.0f);
            *depth = pz;
        } else {
            ln = v3(l.x / rd, l.y / rd, 0.0f);
            *depth = pr;
        }
    } else {
        float px = h.x - fabsf(l.x), py = h.y - fabsf(l.y), pz = h.z - fabsf(l.z);
        if (px <= 0.0f || py <= 0.0f || pz <= 0.0f) return 0;
        if (px < py && px < pz) {
            ln = v3(l.x < 0.0f ? -1.0f : 1.0f, 0, 0);
            *depth = px;
        } else if (py < pz) {
            ln = v3(0, l.y < 0.0f ? -1.0f : 1.0f, 0);
            *depth = py;
        } else {
            ln = v3(0, 0, l.z < 0.0f ? -1.0f : 1.0f);
            *depth = pz;
        }
    }
    *n = q_rotate(b->rot, ln);
    return 1;
}

static void add_contact(int a, int b, vec3 p, vec3 n, float depth, const PhysWorld *w) {
    if (g_num_contacts >= MAX_CONTACTS) return;
    Contact *c = &g_contacts[g_num_contacts++];
    memset(c, 0, sizeof(*c));
    c->a = (short)a;
    c->b = (short)b;
    c->n = n;
    c->depth = depth;
    c->ra = v3_sub(p, w->bodies[a].pos);
    if (b >= 0) c->rb = v3_sub(p, w->bodies[b].pos);
}

static int aabb_overlap(vec3 amin, vec3 amax, vec3 bmin, vec3 bmax) {
    return amin.x <= bmax.x && amax.x >= bmin.x && amin.y <= bmax.y && amax.y >= bmin.y && amin.z <= bmax.z &&
           amax.z >= bmin.z;
}

static int welded(const PhysWorld *w, int a, int b) {
    for (int j = 0; j < MAX_JOINTS; j++) {
        const Joint *jt = &w->joints[j];
        // Welded and ball-socketed props touch at the joint, so they don't collide with each other;
        // slider props usually rest on each other and would grind
        if (jt->active && (jt->type == JOINT_WELD || jt->type == JOINT_BALL || jt->type == JOINT_SLIDER ||
                           jt->type == JOINT_AXIS) &&
            ((jt->a == a && jt->b == b) || (jt->a == b && jt->b == a)))
            return 1;
    }
    return 0;
}

// Points of body ia inside body ib become contacts pushing ia out through ib's surface.
static void body_vs_body(PhysWorld *w, int ia, int ib) {
    vec3 pts[MAX_SAMPLE_POINTS];
    int np = world_points(&w->bodies[ia], pts);
    for (int i = 0; i < np; i++) {
        vec3 n;
        float depth;
        if (point_inside(&w->bodies[ib], pts[i], &n, &depth)) add_contact(ia, ib, pts[i], n, depth, w);
    }
}

static void generate_contacts(PhysWorld *w, Map *m) {
    g_num_contacts = 0;
    vec3 mins[MAX_BODIES], maxs[MAX_BODIES];
    for (int i = 0; i < MAX_BODIES; i++) {
        if (w->bodies[i].active) phys_body_aabb(&w->bodies[i], &mins[i], &maxs[i]);
    }
    for (int i = 0; i < MAX_BODIES; i++) {
        Body *b = &w->bodies[i];
        if (!b->active || is_static(b)) continue;
        vec3 pts[MAX_SAMPLE_POINTS];
        int np = world_points(b, pts);
        int wet = 0;
        for (int k = 0; k < np; k++) {
            vec3 n;
            float depth;
            if (cm_point_penetration(m, pts[k], MASK_PROPSOLID, &n, &depth)) {
                add_contact(i, -1, pts[k], n, depth > 32.0f ? 32.0f : depth, w);
            }
            if (cm_point_contents(m, pts[k]) & PMAP_CONTENTS_WATER) wet++;
        }
        b->in_water = wet;
        // Buoyancy: lift proportional to the submerged share of the sample points, plus drag
        if (wet && !b->held) {
            float frac = (float)wet / (float)np;
            b->vel.z += GRAVITY * b->buoyancy * frac * STEP;
            float drag = 1.0f - clampf(WATER_DRAG * frac * STEP, 0.0f, 0.5f);
            b->vel = v3_scale(b->vel, drag);
            b->angvel = v3_scale(b->angvel, drag);
        }
        for (int j = 0; j < MAX_BODIES; j++) {
            if (j == i || !w->bodies[j].active) continue;
            Body *o = &w->bodies[j];
            if (!is_static(o) && j < i) continue; // moving pair already handled from the lower index
            if (b->nocollide || o->nocollide) continue;
            if (!aabb_overlap(mins[i], maxs[i], mins[j], maxs[j])) continue;
            if (welded(w, i, j)) continue;
            if (o->asleep && !o->frozen) {
                if (v3_len(b->vel) > SLEEP_LINEAR * 2.0f || b->held) phys_wake(w, j);
            }
            body_vs_body(w, i, j);
            body_vs_body(w, j, i);
        }
    }
}

static vec3 point_velocity(const Body *b, vec3 r) {
    if (!b || is_static(b)) return v3(0, 0, 0);
    return v3_add(b->vel, v3_cross(b->angvel, r));
}

static void apply_impulse(Body *b, vec3 r, vec3 p) {
    if (!b || is_static(b)) return;
    b->vel = v3_mad(b->vel, p, b->inv_mass);
    b->angvel = v3_add(b->angvel, apply_inv_inertia(b, v3_cross(r, p)));
}

static void apply_angular_impulse(Body *b, vec3 l) {
    if (!b || is_static(b)) return;
    b->angvel = v3_add(b->angvel, apply_inv_inertia(b, l));
}

static float effective_mass(const Body *a, const Body *b, vec3 ra, vec3 rb, vec3 n) {
    float k = body_inv_mass(a) + v3_dot(n, v3_cross(apply_inv_inertia(a, v3_cross(ra, n)), ra));
    if (b) k += body_inv_mass(b) + v3_dot(n, v3_cross(apply_inv_inertia(b, v3_cross(rb, n)), rb));
    return k > 1e-8f ? 1.0f / k : 0.0f;
}

// K matrix for a point-to-point constraint: how a 3D impulse at the anchors changes their relative velocity.
// K = (1/ma + 1/mb) I - [ra]x Ia^-1 [ra]x - [rb]x Ib^-1 [rb]x, built one column at a time.
static mat3 point_k(const Body *a, const Body *b, vec3 ra, vec3 rb) {
    mat3 k = m3_zero();
    const vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int c = 0; c < 3; c++) {
        vec3 e = axes[c];
        vec3 col = v3_scale(e, body_inv_mass(a) + body_inv_mass(b));
        col = v3_sub(col, v3_cross(ra, apply_inv_inertia(a, v3_cross(ra, e))));
        if (b) col = v3_sub(col, v3_cross(rb, apply_inv_inertia(b, v3_cross(rb, e))));
        k.m[0][c] = col.x;
        k.m[1][c] = col.y;
        k.m[2][c] = col.z;
    }
    return k;
}

static void solve_joints(PhysWorld *w, float dt) {
    for (int j = 0; j < MAX_JOINTS; j++) {
        Joint *jt = &w->joints[j];
        if (!jt->active) continue;
        Body *a = &w->bodies[jt->a];
        Body *b = jt->b >= 0 ? &w->bodies[jt->b] : NULL;
        if (!a->active || (b && !b->active)) {
            jt->active = 0;
            continue;
        }
        if (is_static(a) && (!b || is_static(b))) continue;
        vec3 ra = q_rotate(a->rot, jt->la);
        vec3 pa = v3_add(a->pos, ra);
        vec3 rb = b ? q_rotate(b->rot, jt->lb) : v3(0, 0, 0);
        vec3 pb = b ? v3_add(b->pos, rb) : jt->lb;
        vec3 err = v3_sub(pa, pb);
        vec3 dv = v3_sub(point_velocity(a, ra), point_velocity(b, rb));

        if (jt->type == JOINT_ELASTIC) continue; // a spring force, applied once per step (apply_springs)
        if (jt->type == JOINT_ROD) {
            // A hydraulic: the anchors stay exactly jt->length apart, pushing as well as pulling
            float len = v3_len(err);
            if (len < 1e-3f) continue;
            vec3 n = v3_scale(err, 1.0f / len);
            float lambda = -effective_mass(a, b, ra, rb, n) * (v3_dot(dv, n) + JOINT_BETA / dt * (len - jt->length));
            vec3 p = v3_scale(n, lambda);
            apply_impulse(a, ra, p);
            if (b) apply_impulse(b, rb, v3_scale(p, -1.0f));
            continue;
        }
        if (jt->type == JOINT_ROPE) {
            float len = v3_len(err);
            if (len <= jt->length || len < 1e-3f) continue;
            vec3 n = v3_scale(err, 1.0f / len);
            float mn = effective_mass(a, b, ra, rb, n);
            float vn = v3_dot(dv, n);
            float bias = JOINT_BETA / dt * (len - jt->length);
            float lambda = -mn * (vn + bias);
            if (lambda > 0.0f) lambda = 0.0f; // ropes only pull
            vec3 p = v3_scale(n, lambda);
            apply_impulse(a, ra, p);
            if (b) apply_impulse(b, rb, v3_scale(p, -1.0f));
            continue;
        }

        mat3 kinv;
        if (jt->type == JOINT_SLIDER) {
            // Slider, part 1: a's anchor stays on the line through b's, free along it. b is pushed
            // where a's anchor is now, not at its own anchor.
            vec3 n = b ? q_rotate(b->rot, jt->axis) : jt->axis;
            vec3 rs = b ? v3_sub(pa, b->pos) : v3(0, 0, 0);
            vec3 t1 = v3_norm(v3_cross(n, fabsf(n.z) < 0.9f ? v3(0, 0, 1) : v3(1, 0, 0)));
            vec3 t2 = v3_cross(n, t1);
            for (int k = 0; k < 2; k++) {
                vec3 t = k ? t2 : t1;
                vec3 dvs = v3_sub(point_velocity(a, ra), point_velocity(b, rs));
                float lambda = -effective_mass(a, b, ra, rs, t) * (v3_dot(dvs, t) + JOINT_BETA / dt * v3_dot(err, t));
                vec3 p = v3_scale(t, lambda);
                apply_impulse(a, ra, p);
                if (b) apply_impulse(b, rs, v3_scale(p, -1.0f));
            }
        } else {
            // Weld, part 1: anchors stay together
            mat3 k = point_k(a, b, ra, rb);
            if (m3_inverse(&k, &kinv)) {
                vec3 rhs = v3_add(dv, v3_scale(err, JOINT_BETA / dt));
                vec3 p = v3_scale(m3_mul(&kinv, rhs), -1.0f);
                apply_impulse(a, ra, p);
                if (b) apply_impulse(b, rb, v3_scale(p, -1.0f));
            }
            if (jt->type == JOINT_BALL) continue; // a ball socket stops here: it turns freely
        }
        if (jt->type == JOINT_AXIS) {
            // A wheel, part 2: a's axle stays lined up with b's (it only turns about it), then the motor
            vec3 na = q_rotate(a->rot, jt->axis_a);
            vec3 nb = b ? q_rotate(b->rot, jt->axis) : jt->axis;
            vec3 aerr = v3_cross(nb, na); // how far a's axle has tipped away from b's
            vec3 t1 = v3_norm(v3_cross(nb, fabsf(nb.z) < 0.9f ? v3(0, 0, 1) : v3(1, 0, 0)));
            vec3 t2 = v3_cross(nb, t1);
            for (int k = 0; k < 3; k++) {
                vec3 t = k == 0 ? t1 : k == 1 ? t2 : nb;
                float kk = v3_dot(t, apply_inv_inertia(a, t)) + v3_dot(t, apply_inv_inertia(b, t));
                if (kk <= 1e-9f) continue;
                vec3 dw = v3_sub(is_static(a) ? v3(0, 0, 0) : a->angvel, (b && !is_static(b)) ? b->angvel : v3(0, 0, 0));
                float lambda;
                if (k < 2) {
                    lambda = -(v3_dot(dw, t) + JOINT_BETA / dt * v3_dot(aerr, t)) / kk;
                } else {
                    // The motor: drives a's spin about the axle towards its speed, with limited torque
                    if (!jt->on) continue;
                    float most = WHEEL_TORQUE * dt / (float)g_iterations;
                    lambda = clampf((jt->motor - v3_dot(dw, t)) / kk, -most, most);
                }
                vec3 l = v3_scale(t, lambda);
                apply_angular_impulse(a, l);
                if (b) apply_angular_impulse(b, v3_scale(l, -1.0f));
            }
            continue;
        }
        // Weld and slider, part 2: relative orientation stays fixed
        quat qb = b ? b->rot : q_identity();
        quat target = q_mul(qb, jt->rel); // where a's orientation should be
        quat qe = q_mul(a->rot, q_conj(target));
        if (qe.w < 0.0f) qe = (quat){-qe.x, -qe.y, -qe.z, -qe.w};
        vec3 aerr = v3_scale(v3(qe.x, qe.y, qe.z), 2.0f);
        vec3 dw = v3_sub(is_static(a) ? v3(0, 0, 0) : a->angvel, (b && !is_static(b)) ? b->angvel : v3(0, 0, 0));
        // Angular K = Ia^-1 + Ib^-1 (world); build column by column
        mat3 ka = m3_zero();
        const vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int c = 0; c < 3; c++) {
            vec3 col = v3_add(apply_inv_inertia(a, axes[c]), apply_inv_inertia(b, axes[c]));
            ka.m[0][c] = col.x;
            ka.m[1][c] = col.y;
            ka.m[2][c] = col.z;
        }
        if (m3_inverse(&ka, &kinv)) {
            vec3 rhs = v3_add(dw, v3_scale(aerr, JOINT_BETA / dt));
            vec3 l = v3_scale(m3_mul(&kinv, rhs), -1.0f);
            apply_angular_impulse(a, l);
            if (b) apply_angular_impulse(b, v3_scale(l, -1.0f));
        }
    }
}

static void solve(PhysWorld *w, float dt) {
    for (int i = 0; i < g_num_contacts; i++) {
        Contact *c = &g_contacts[i];
        Body *a = &w->bodies[c->a];
        Body *b = c->b >= 0 ? &w->bodies[c->b] : NULL;
        vec3 t = fabsf(c->n.z) < 0.9f ? v3(0, 0, 1) : v3(1, 0, 0);
        c->t1 = v3_norm(v3_cross(c->n, t));
        c->t2 = v3_cross(c->n, c->t1);
        c->mn = effective_mass(a, b, c->ra, c->rb, c->n);
        c->mt1 = effective_mass(a, b, c->ra, c->rb, c->t1);
        c->mt2 = effective_mass(a, b, c->ra, c->rb, c->t2);
        float bias = BAUMGARTE / dt * (c->depth - SLOP);
        if (bias < 0.0f) bias = 0.0f;
        if (bias > MAX_CORRECTION) bias = MAX_CORRECTION;
        vec3 dv = v3_sub(point_velocity(a, c->ra), point_velocity(b, c->rb));
        float vn = v3_dot(dv, c->n);
        if (vn < -RESTITUTION_MIN_SPEED) {
            float bounce = -RESTITUTION * vn;
            if (bounce > bias) bias = bounce;
            // Remember the hit for impact sounds
            if (-vn > a->impact) a->impact = -vn;
            if (b && -vn > b->impact) b->impact = -vn;
        }
        c->bias = bias;
    }

    for (int it = 0; it < g_iterations; it++) {
        solve_joints(w, dt);
        for (int i = 0; i < g_num_contacts; i++) {
            Contact *c = &g_contacts[i];
            Body *a = &w->bodies[c->a];
            Body *b = c->b >= 0 ? &w->bodies[c->b] : NULL;

            vec3 dv = v3_sub(point_velocity(a, c->ra), point_velocity(b, c->rb));
            float vn = v3_dot(dv, c->n);
            float dj = c->mn * (c->bias - vn);
            float jn = c->jn + dj;
            if (jn < 0.0f) jn = 0.0f;
            dj = jn - c->jn;
            c->jn = jn;
            vec3 p = v3_scale(c->n, dj);
            apply_impulse(a, c->ra, p);
            apply_impulse(b, c->rb, v3_scale(p, -1.0f));

            float maxf = FRICTION * c->jn;
            for (int k = 0; k < 2; k++) {
                vec3 tk = k == 0 ? c->t1 : c->t2;
                float mt = k == 0 ? c->mt1 : c->mt2;
                float *acc = k == 0 ? &c->jt1 : &c->jt2;
                dv = v3_sub(point_velocity(a, c->ra), point_velocity(b, c->rb));
                float dt_j = -mt * v3_dot(dv, tk);
                float nj = clampf(*acc + dt_j, -maxf, maxf);
                dt_j = nj - *acc;
                *acc = nj;
                vec3 pt = v3_scale(tk, dt_j);
                apply_impulse(a, c->ra, pt);
                apply_impulse(b, c->rb, v3_scale(pt, -1.0f));
            }
        }
    }
}

static int joint_holds_up(const PhysWorld *w, int i) {
    for (int j = 0; j < MAX_JOINTS; j++) {
        const Joint *jt = &w->joints[j];
        if (jt->active && (jt->a == i || jt->b == i)) return 1;
    }
    return 0;
}

#define ELASTIC_OMEGA 6.0f  // spring stiffness as a natural frequency (rad/s): bouncy but not jittery
#define ELASTIC_DAMPING 0.3f
#define HOVER_OMEGA 4.0f    // how firmly a hoverball holds its height
#define HOVER_DAMPING 0.8f

// Thrusters push along their direction; hoverballs hold their height against gravity
static void apply_forces(PhysWorld *w, float dt) {
    for (int f = 0; f < MAX_FORCES; f++) {
        Force *fc = &w->forces[f];
        if (!fc->active || !fc->on) continue;
        Body *b = &w->bodies[fc->body];
        if (!b->active || b->frozen || b->held) continue;
        if (b->asleep) phys_wake(w, fc->body);
        b->powered = 1;
        vec3 r = q_rotate(b->rot, fc->lpos);
        if (fc->type == FORCE_THRUSTER) {
            apply_impulse(b, r, v3_scale(q_rotate(b->rot, fc->ldir), fc->strength * dt));
        } else {
            // Spring-damper on the attachment point's height, plus whatever cancels gravity
            float z = b->pos.z + r.z;
            float acc = GRAVITY - b->lift + HOVER_OMEGA * HOVER_OMEGA * (fc->strength - z) -
                        2.0f * HOVER_DAMPING * HOVER_OMEGA * b->vel.z;
            b->vel.z += clampf(acc, 0.0f, GRAVITY * 3.0f) * dt;
            b->angvel = v3_scale(b->angvel, 1.0f - 2.0f * dt); // and keep it from spinning forever
        }
    }
}

// Hydraulics move towards their extended or retracted length; winches reel in or let out until they
// reach the end. Either keeps its props awake while it moves; a running wheel motor does too.
static void update_joint_lengths(PhysWorld *w, float dt) {
    for (int j = 0; j < MAX_JOINTS; j++) {
        Joint *jt = &w->joints[j];
        if (!jt->active) continue;
        int moving = 0;
        if (jt->type == JOINT_ROD) {
            float target = jt->on ? jt->len_max : jt->len_min;
            float step = jt->rate * dt;
            if (fabsf(target - jt->length) > 0.01f) {
                jt->length += clampf(target - jt->length, -step, step);
                moving = 1;
            }
        } else if (jt->type == JOINT_ROPE && jt->rate > 0.0f && jt->on != WINCH_STOP) {
            jt->length += (jt->on == WINCH_IN ? -jt->rate : jt->rate) * dt;
            if (jt->length <= jt->len_min || jt->length >= jt->len_max) {
                jt->length = clampf(jt->length, jt->len_min, jt->len_max);
                jt->on = WINCH_STOP;
            }
            moving = 1;
        } else if (jt->type == JOINT_AXIS && jt->on) {
            moving = 1;
        }
        if (!moving) continue;
        Body *a = &w->bodies[jt->a];
        if (a->asleep) phys_wake(w, jt->a);
        a->powered = 1;
        if (jt->b >= 0) {
            Body *b = &w->bodies[jt->b];
            if (b->asleep) phys_wake(w, jt->b);
            b->powered = 1;
        }
    }
}

// Elastic joints: a damped spring that only pulls (like a rubber band) once stretched past its rest length
static void apply_springs(PhysWorld *w, float dt) {
    for (int j = 0; j < MAX_JOINTS; j++) {
        Joint *jt = &w->joints[j];
        if (!jt->active || jt->type != JOINT_ELASTIC) continue;
        Body *a = &w->bodies[jt->a];
        Body *b = jt->b >= 0 ? &w->bodies[jt->b] : NULL;
        if (!a->active || (b && !b->active)) {
            jt->active = 0;
            continue;
        }
        vec3 ra = q_rotate(a->rot, jt->la), rb = b ? q_rotate(b->rot, jt->lb) : v3(0, 0, 0);
        vec3 pa = v3_add(a->pos, ra), pb = b ? v3_add(b->pos, rb) : jt->lb;
        vec3 d = v3_sub(pb, pa);
        float len = v3_len(d);
        if (len <= jt->length || len < 1e-3f) continue;
        float im = body_inv_mass(a) + body_inv_mass(b);
        if (im <= 0.0f) continue;
        if (is_static(a) && a->asleep) phys_wake(w, jt->a);
        if (b && b->asleep) phys_wake(w, jt->b);
        vec3 n = v3_scale(d, 1.0f / len);
        float vn = v3_dot(v3_sub(point_velocity(b, rb), point_velocity(a, ra)), n);
        float m_eff = 1.0f / im;
        float force = m_eff * (ELASTIC_OMEGA * ELASTIC_OMEGA * (len - jt->length) + 2.0f * ELASTIC_DAMPING * ELASTIC_OMEGA * vn);
        if (force <= 0.0f) continue;
        vec3 p = v3_scale(n, force * dt);
        apply_impulse(a, ra, p);
        if (b) apply_impulse(b, rb, v3_scale(p, -1.0f));
    }
}

static void (*g_step_hook)(PhysWorld *w, Map *m, float dt);

void phys_set_step_hook(void (*hook)(PhysWorld *w, Map *m, float dt)) {
    g_step_hook = hook;
}

void phys_apply_impulse(Body *b, vec3 world_point, vec3 impulse) {
    apply_impulse(b, v3_sub(world_point, b->pos), impulse);
}

vec3 phys_point_velocity(const Body *b, vec3 world_point) {
    return point_velocity(b, v3_sub(world_point, b->pos));
}

static void step_once(PhysWorld *w, Map *m, float dt) {
    for (int i = 0; i < MAX_BODIES; i++) w->bodies[i].powered = 0;
    if (g_step_hook) g_step_hook(w, m, dt);
    apply_forces(w, dt);
    apply_springs(w, dt);
    update_joint_lengths(w, dt);
    for (int i = 0; i < MAX_BODIES; i++) {
        Body *b = &w->bodies[i];
        if (!b->active || is_static(b)) continue;
        if (!b->held) {
            b->vel.z -= (GRAVITY - b->lift) * dt;
            b->vel = v3_scale(b->vel, 1.0f - 0.2f * dt);
            b->angvel = v3_scale(b->angvel, 1.0f - (b->shape == SHAPE_BOX ? 0.6f : 0.3f) * dt);
        }
        float s = v3_len(b->vel);
        if (s > MAX_LINEAR) b->vel = v3_scale(b->vel, MAX_LINEAR / s);
        float a = v3_len(b->angvel);
        if (a > MAX_ANGULAR) b->angvel = v3_scale(b->angvel, MAX_ANGULAR / a);
    }

    generate_contacts(w, m);
    w->contacts_last_step = g_num_contacts;
    solve(w, dt);

    for (int i = 0; i < MAX_BODIES; i++) {
        Body *b = &w->bodies[i];
        if (!b->active || is_static(b)) continue;
        vec3 np = v3_mad(b->pos, b->vel, dt);
        // Contacts only see sample points once per step, so something fast (thrown, or dragged by the
        // physgun) could skip right through a thin wall. Sweep its core along the move to stop that.
        float r = b->half.x < b->half.y ? b->half.x : b->half.y;
        if (b->half.z < r) r = b->half.z;
        r = clampf(r * 0.5f, 0.5f, 8.0f);
        if (v3_dot(b->vel, b->vel) * dt * dt > r * r) {
            Trace tr;
            cm_trace(m, b->pos, np, v3(-r, -r, -r), v3(r, r, r), MASK_PROPSOLID, NULL, 0, &tr);
            if (!tr.startsolid && tr.fraction < 1.0f) {
                np = tr.endpos;
                float vn = v3_dot(b->vel, tr.normal);
                if (vn < 0.0f) {
                    if (-vn > b->impact) b->impact = -vn;
                    b->vel = v3_mad(b->vel, tr.normal, -vn * (1.0f + RESTITUTION));
                }
            }
        }
        b->pos = np;
        b->rot = q_integrate(b->rot, b->angvel, dt);
        // Balloons, water and dangling ropes keep things moving, so those never sleep.
        int can_sleep = !b->held && b->balloons == 0 && !b->in_water && !b->powered;
        if (can_sleep && v3_len(b->vel) < SLEEP_LINEAR && v3_len(b->angvel) < SLEEP_ANGULAR) {
            b->idle_time += dt;
            if (b->idle_time > SLEEP_TIME && !(joint_holds_up(w, i) && b->idle_time < SLEEP_TIME * 3.0f)) {
                b->asleep = 1;
                b->vel = v3(0, 0, 0);
                b->angvel = v3(0, 0, 0);
            }
        } else {
            b->idle_time = 0.0f;
        }
        if (b->pos.z < -32768.0f) phys_remove(w, i); // fell out of the world
    }
}

void phys_step(PhysWorld *w, Map *m, float dt) {
    w->accum += dt;
    int steps = 0, max_steps = g_max_steps + g_frame_steps - 1;
    while (w->accum >= STEP && steps < max_steps) {
        step_once(w, m, STEP);
        w->accum -= STEP;
        steps++;
    }
    if (steps == max_steps) w->accum = 0.0f; // too slow to catch up: drop time instead of spiralling
}

static int free_joint(PhysWorld *w) {
    for (int j = 0; j < MAX_JOINTS; j++) {
        if (!w->joints[j].active) return j;
    }
    return -1;
}

int phys_weld(PhysWorld *w, int a, int b, vec3 anchor) {
    int j = free_joint(w);
    if (j < 0 || a < 0 || a == b) return -1;
    Joint *jt = &w->joints[j];
    memset(jt, 0, sizeof(*jt));
    jt->active = 1;
    jt->type = JOINT_WELD;
    jt->a = a;
    jt->b = b;
    Body *ba = &w->bodies[a];
    jt->la = q_rotate_inv(ba->rot, v3_sub(anchor, ba->pos));
    if (b >= 0) {
        Body *bb = &w->bodies[b];
        jt->lb = q_rotate_inv(bb->rot, v3_sub(anchor, bb->pos));
        jt->rel = q_mul(q_conj(bb->rot), ba->rot);
    } else {
        jt->lb = anchor;
        jt->rel = ba->rot;
    }
    phys_wake(w, a);
    if (b >= 0) phys_wake(w, b);
    return j;
}

int phys_rope(PhysWorld *w, int a, vec3 world_a, int b, vec3 world_b, float length) {
    // Normalise so 'a' is always a body (a rope needs at least one)
    if (a < 0) {
        int ti = a;
        a = b;
        b = ti;
        vec3 tv = world_a;
        world_a = world_b;
        world_b = tv;
    }
    int j = free_joint(w);
    if (j < 0 || a < 0 || a == b) return -1;
    Joint *jt = &w->joints[j];
    memset(jt, 0, sizeof(*jt));
    jt->active = 1;
    jt->type = JOINT_ROPE;
    jt->a = a;
    jt->b = b;
    jt->length = length;
    Body *ba = &w->bodies[a];
    jt->la = q_rotate_inv(ba->rot, v3_sub(world_a, ba->pos));
    if (b >= 0) {
        Body *bb = &w->bodies[b];
        jt->lb = q_rotate_inv(bb->rot, v3_sub(world_b, bb->pos));
    } else {
        jt->lb = world_b;
    }
    phys_wake(w, a);
    if (b >= 0) phys_wake(w, b);
    return j;
}

int phys_ballsocket(PhysWorld *w, int a, int b, vec3 anchor) {
    int j = phys_weld(w, a, b, anchor);
    if (j >= 0) w->joints[j].type = JOINT_BALL;
    return j;
}

int phys_elastic(PhysWorld *w, int a, vec3 world_a, int b, vec3 world_b, float rest_length) {
    int j = phys_rope(w, a, world_a, b, world_b, rest_length);
    if (j >= 0) w->joints[j].type = JOINT_ELASTIC;
    return j;
}

int phys_slider(PhysWorld *w, int a, vec3 world_a, int b, vec3 world_b) {
    // Normalise so 'a' is always a body; the line is the same either way round
    if (a < 0) {
        int ti = a;
        a = b;
        b = ti;
        vec3 tv = world_a;
        world_a = world_b;
        world_b = tv;
    }
    vec3 d = v3_sub(world_a, world_b);
    float len = v3_len(d);
    vec3 axis = len > 1e-3f ? v3_scale(d, 1.0f / len) : v3(0, 0, 1);
    // A weld's anchors and fixed relative angle, then b's anchor moved to its own point
    int j = phys_weld(w, a, b, world_a);
    if (j < 0) return -1;
    Joint *jt = &w->joints[j];
    jt->type = JOINT_SLIDER;
    if (b >= 0) {
        const Body *bb = &w->bodies[b];
        jt->lb = q_rotate_inv(bb->rot, v3_sub(world_b, bb->pos));
        jt->axis = q_rotate_inv(bb->rot, axis);
    } else {
        jt->lb = world_b;
        jt->axis = axis;
    }
    return j;
}

int phys_axis(PhysWorld *w, int a, int b, vec3 world_anchor, vec3 world_axis, float motor) {
    int j = phys_weld(w, a, b, world_anchor);
    if (j < 0) return -1;
    Joint *jt = &w->joints[j];
    jt->type = JOINT_AXIS;
    world_axis = v3_norm(world_axis);
    jt->axis_a = q_rotate_inv(w->bodies[a].rot, world_axis);
    jt->axis = b >= 0 ? q_rotate_inv(w->bodies[b].rot, world_axis) : world_axis;
    jt->motor = motor;
    return j;
}

int phys_hydraulic(PhysWorld *w, int a, vec3 world_a, int b, vec3 world_b, float stretch, float rate) {
    float len = v3_len(v3_sub(world_a, world_b));
    int j = phys_rope(w, a, world_a, b, world_b, len);
    if (j < 0) return -1;
    Joint *jt = &w->joints[j];
    jt->type = JOINT_ROD;
    jt->len_min = len;
    jt->len_max = len * stretch;
    jt->rate = rate;
    return j;
}

int phys_winch(PhysWorld *w, int a, vec3 world_a, int b, vec3 world_b, float rate) {
    float len = v3_len(v3_sub(world_a, world_b));
    int j = phys_rope(w, a, world_a, b, world_b, len);
    if (j < 0) return -1;
    Joint *jt = &w->joints[j];
    jt->rate = rate;
    jt->len_min = 8.0f;
    jt->len_max = len * 3.0f > len + 512.0f ? len * 3.0f : len + 512.0f;
    jt->on = WINCH_STOP;
    return j;
}

int phys_toggle_joint(PhysWorld *w, int j) {
    Joint *jt = &w->joints[j];
    if (!jt->active) return 0;
    if (jt->type == JOINT_AXIS || jt->type == JOINT_ROD) {
        jt->on = !jt->on;
    } else if (jt->type == JOINT_ROPE && jt->rate > 0.0f) {
        jt->on = jt->on == WINCH_IN ? WINCH_OUT : WINCH_IN; // reel in, then let out, then in...
    } else {
        return 0;
    }
    phys_wake(w, jt->a);
    if (jt->b >= 0) phys_wake(w, jt->b);
    return 1;
}

int phys_toggle_joints(PhysWorld *w, int body) {
    int n = 0;
    for (int j = 0; j < MAX_JOINTS; j++) {
        const Joint *jt = &w->joints[j];
        if (jt->active && (jt->a == body || jt->b == body)) n += phys_toggle_joint(w, j);
    }
    return n;
}


int phys_add_force(PhysWorld *w, int type, int body, vec3 world_pos, vec3 world_dir, float strength) {

    if (body < 0 || body >= MAX_BODIES || !w->bodies[body].active) return -1;
    for (int f = 0; f < MAX_FORCES; f++) {
        Force *fc = &w->forces[f];
        if (fc->active) continue;
        const Body *b = &w->bodies[body];
        memset(fc, 0, sizeof(*fc));
        fc->active = 1;
        fc->on = 1;
        fc->type = type;
        fc->body = body;
        fc->lpos = q_rotate_inv(b->rot, v3_sub(world_pos, b->pos));
        fc->ldir = q_rotate_inv(b->rot, world_dir);
        fc->strength = strength;
        w->bodies[body].frozen = 0;
        phys_wake(w, body);
        return f;
    }
    return -1;
}

int phys_toggle_forces(PhysWorld *w, int body) {
    int n = 0, on = -1;
    for (int f = 0; f < MAX_FORCES; f++) {
        Force *fc = &w->forces[f];
        if (!fc->active || fc->body != body) continue;
        if (on < 0) on = !fc->on; // they all follow the first one
        fc->on = on;
        n++;
    }
    if (n) phys_wake(w, body);
    return n;
}

void phys_set_mass(PhysWorld *w, int i, float mass) {
    Body *b = &w->bodies[i];
    if (mass < 1.0f) mass = 1.0f;
    float k = b->mass / mass;
    b->inv_inertia = v3_scale(b->inv_inertia, k);
    b->buoyancy = clampf(b->buoyancy * k, 0.0f, 2.5f);
    b->mass = mass;
    b->inv_mass = 1.0f / mass;
    phys_wake(w, i);
}

void phys_blast(PhysWorld *w, vec3 centre, float radius, float speed) {
    for (int i = 0; i < MAX_BODIES; i++) {
        Body *b = &w->bodies[i];
        if (!b->active || b->held) continue;
        vec3 d = v3_sub(b->pos, centre);
        float dist = v3_len(d);
        if (dist >= radius) continue;
        float k = 1.0f - dist / radius;
        vec3 dir = dist > 1.0f ? v3_scale(d, 1.0f / dist) : v3(0, 0, 1);
        dir.z += 0.35f; // explosions throw things up as well as out
        b->frozen = 0;
        phys_wake(w, i);
        float heavy = clampf(50.0f / b->mass, 0.2f, 2.0f);
        b->vel = v3_mad(b->vel, v3_norm(dir), speed * k * heavy);
        b->angvel = v3_add(b->angvel, v3(dir.y * 6.0f * k, -dir.x * 6.0f * k, 0.0f));
    }
}

void phys_joint_anchors(const PhysWorld *w, const Joint *j, vec3 *pa, vec3 *pb) {
    const Body *a = &w->bodies[j->a];
    *pa = v3_add(a->pos, q_rotate(a->rot, j->la));
    if (j->b >= 0) {
        const Body *b = &w->bodies[j->b];
        *pb = v3_add(b->pos, q_rotate(b->rot, j->lb));
    } else {
        *pb = j->lb;
    }
}

int phys_raycast(const PhysWorld *w, vec3 origin, vec3 dir, float max_t, float *t_out) {
    int best = -1;
    float best_t = max_t;
    for (int i = 0; i < MAX_BODIES; i++) {
        const Body *b = &w->bodies[i];
        if (!b->active) continue;
        vec3 o = q_rotate_inv(b->rot, v3_sub(origin, b->pos));
        vec3 d = q_rotate_inv(b->rot, dir);
        float tmin = 0.0f, tmax = best_t;
        vec3 h = shape_extent(b);
        const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z}, hh[3] = {h.x, h.y, h.z};
        int hit = 1;
        for (int k = 0; k < 3 && hit; k++) {
            if (fabsf(dd[k]) < 1e-6f) {
                if (oo[k] < -hh[k] || oo[k] > hh[k]) hit = 0;
            } else {
                float t1 = (-hh[k] - oo[k]) / dd[k], t2 = (hh[k] - oo[k]) / dd[k];
                if (t1 > t2) {
                    float tmp = t1;
                    t1 = t2;
                    t2 = tmp;
                }
                if (t1 > tmin) tmin = t1;
                if (t2 < tmax) tmax = t2;
                if (tmin > tmax) hit = 0;
            }
        }
        if (hit && tmin < best_t) {
            best_t = tmin;
            best = i;
        }
    }
    if (best >= 0 && t_out) *t_out = best_t;
    return best;
}
