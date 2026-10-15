#include "vehicle.h"

#include <math.h>
#include <string.h>

#define GRAV 600.0f // physics.c's gravity
#define MAX_SEAT_MODELS 8

// How a kind of vehicle rides. Points are in model space (the Jeep and Airboat face -Y, seats +X).
typedef struct {
    int num_wheels;
    vec3 wheel[4];      // wheel centres at rest (Airboat: the bottoms of its floats)
    float radius;       // tyre radius
    unsigned steer_mask; // wheels that steer
    unsigned drive_mask; // wheels the engine turns
    vec3 fwd;           // forward, model space
    vec3 eye;           // the driver's eyes
    float travel;       // how far a wheel can rise above where it rests
    float sag;          // how far the springs settle under the weight
    float grip;         // sideways friction, as a fraction of the load
    float engine;       // acceleration at full throttle from standstill (u/s^2)
    float top_speed, reverse_speed;
    float brake;        // deceleration when braking (u/s^2)
    float steer_max;    // radians, at a standstill
    int boat;
} VehSpec;

static const VehSpec k_jeep = {
    4,
    {{41.3f, -43.4f, 19.1f}, {-41.3f, -43.4f, 19.1f}, {42.3f, 92.4f, 20.6f}, {-42.3f, 92.4f, 20.6f}},
    19.0f, 0x3, 0xF, {0.0f, -1.0f, 0.0f}, {9.4f, 47.5f, 62.0f},
    10.0f, 6.0f, 1.3f, 520.0f, 1100.0f, 380.0f, 1300.0f, 0.6f, 0,
};
static const VehSpec k_airboat = {
    4,
    {{30.0f, -50.0f, 2.0f}, {-30.0f, -50.0f, 2.0f}, {30.0f, 70.0f, 2.0f}, {-30.0f, 70.0f, 2.0f}},
    0.0f, 0x0, 0x0, {0.0f, -1.0f, 0.0f}, {0.0f, 11.0f, 64.0f},
    12.0f, 5.0f, 0.12f, 480.0f, 1250.0f, 300.0f, 500.0f, 0.0f, 1,
};

static int g_jeep = -1, g_airboat = -1;
static int g_seats[MAX_SEAT_MODELS];
static float g_seat_eye[MAX_SEAT_MODELS];
static int g_num_seats;

typedef struct {
    float throttle, steer, steer_now;
    int handbrake, driven;
} Drive;
static Drive g_drive[MAX_BODIES];

void vehicles_set_models(int jeep, int airboat, const int *seats, const float *seat_eye, int num_seats) {
    g_jeep = jeep;
    g_airboat = airboat;
    g_num_seats = 0;
    for (int k = 0; k < num_seats && g_num_seats < MAX_SEAT_MODELS; k++) {
        if (seats[k] < 0) continue;
        g_seats[g_num_seats] = seats[k];
        g_seat_eye[g_num_seats++] = seat_eye[k];
    }
}

int vehicle_kind(const Body *b) {
    if (!b->active || b->lib != 0) return VEH_NONE;
    if (b->model == g_jeep) return VEH_JEEP;
    if (b->model == g_airboat) return VEH_AIRBOAT;
    for (int k = 0; k < g_num_seats; k++) {
        if (b->model == g_seats[k]) return VEH_SEAT;
    }
    return VEH_NONE;
}

static const VehSpec *spec_of(int kind) {
    return kind == VEH_JEEP ? &k_jeep : kind == VEH_AIRBOAT ? &k_airboat : NULL;
}

// A model-space point to world space
static vec3 to_world(const Body *b, vec3 p) {
    return v3_add(b->pos, q_rotate(b->rot, v3_sub(p, b->offset)));
}

void vehicle_seat(const Body *b, vec3 *eye, float *yaw) {
    int kind = vehicle_kind(b);
    const VehSpec *s = spec_of(kind);
    vec3 e = v3(0, 0, 40.0f), f = v3(1, 0, 0); // seats face +X
    if (s) {
        e = s->eye;
        f = s->fwd;
    } else {
        for (int k = 0; k < g_num_seats; k++) {
            if (b->model == g_seats[k]) e.z = g_seat_eye[k];
        }
    }
    *eye = to_world(b, e);
    vec3 wf = q_rotate(b->rot, f);
    *yaw = atan2f(wf.y, wf.x) * (180.0f / 3.14159265f);
}

int vehicle_exits(const Body *b, vec3 *out, int max) {
    int kind = vehicle_kind(b);
    int n = 0;
    // Left, right, behind, in front, on top (the HL2 Jeep's exit attachments, roughly)
    static const vec3 jeep[] = {{80, 30, 10}, {-80, 30, 10}, {0, 150, 10}, {0, -110, 10}, {0, 30, 95}};
    static const vec3 boat[] = {{75, 10, 10}, {-75, 10, 10}, {0, 140, 10}, {0, -110, 10}, {0, 10, 100}};
    static const vec3 seat[] = {{0, 40, 0}, {0, -40, 0}, {40, 0, 0}, {-40, 0, 0}, {0, 0, 60}};
    const vec3 *list = kind == VEH_JEEP ? jeep : kind == VEH_AIRBOAT ? boat : seat;
    for (int k = 0; k < 5 && n < max; k++) out[n++] = to_world(b, list[k]);
    return n;
}

void vehicle_drive(int i, float throttle, float steer, int handbrake) {
    if (i < 0 || i >= MAX_BODIES) return;
    Drive *d = &g_drive[i];
    d->throttle = clampf(throttle, -1.0f, 1.0f);
    d->steer = clampf(steer, -1.0f, 1.0f);
    d->handbrake = handbrake;
    d->driven = 1;
}

void vehicles_end_frame(void) {
    for (int i = 0; i < MAX_BODIES; i++) g_drive[i].driven = 0;
}

static void ride(PhysWorld *w, Map *m, int i, const VehSpec *s, float dt) {
    Body *b = &w->bodies[i];
    Drive *d = &g_drive[i];
    if (!d->driven) d->throttle = d->steer = 0.0f, d->handbrake = 0;
    if (b->asleep) {
        if (!d->driven) return; // parked and settled
        phys_wake(w, i);
    }
    if (d->driven) b->powered = 1;

    vec3 up = q_rotate(b->rot, v3(0, 0, 1));
    vec3 fwd = q_rotate(b->rot, s->fwd);
    vec3 right = v3_cross(fwd, up);
    float per = b->mass / (float)s->num_wheels; // each wheel's share
    float k = per * GRAV / s->sag;              // spring rate: settles by sag under the weight
    float c = 2.0f * 0.7f * sqrtf(k * per);     // damping
    float natural = s->travel + s->sag;         // the spring's free length (from its mount)
    float reach = natural + s->radius + 6.0f;   // how far down a wheel looks for the ground
    float vf = v3_dot(b->vel, fwd);

    // Steering eases in, and turns less at speed
    float lim = s->steer_max * (1.0f - 0.8f * clampf(fabsf(vf) / s->top_speed, 0.0f, 1.0f));
    d->steer_now += (d->steer * lim - d->steer_now) * clampf(dt * 8.0f, 0.0f, 1.0f);
    float sc = cosf(d->steer_now), ss = sinf(d->steer_now);

    unsigned int mask = MASK_PROPSOLID | (s->boat ? PMAP_CONTENTS_WATER : 0);
    int touching = 0, on_water = 0;
    for (int wi = 0; wi < s->num_wheels; wi++) {
        vec3 mount = to_world(b, v3_add(s->wheel[wi], v3(0, 0, s->travel)));
        Trace tr;
        cm_trace_line(m, mount, v3_mad(mount, up, -reach), mask, NULL, 0, &tr);
        if (tr.fraction >= 1.0f || tr.startsolid) continue;
        touching++;
        vec3 p = tr.endpos;
        vec3 n = tr.normal;
        if (s->boat && (cm_point_contents(m, v3_mad(p, n, -2.0f)) & PMAP_CONTENTS_WATER)) on_water++;
        float compress = natural + s->radius - tr.fraction * reach;
        vec3 v = phys_point_velocity(b, p);
        float force = k * compress - c * v3_dot(v, up);
        force = clampf(force, 0.0f, per * GRAV * 8.0f);
        phys_apply_impulse(b, p, v3_scale(up, force * dt));

        // The tyre's own frame on the ground: front wheels turned by the steering
        vec3 wf = (s->steer_mask >> wi) & 1 ? v3_add(v3_scale(fwd, sc), v3_scale(right, ss)) : fwd;
        wf = v3_mad(wf, n, -v3_dot(wf, n));
        float wl = v3_len(wf);
        if (wl < 1e-3f) continue;
        wf = v3_scale(wf, 1.0f / wl);
        vec3 side = v3_cross(wf, n);
        float vlat = v3_dot(v, side), vlong = v3_dot(v, wf);

        int rear = !((s->steer_mask >> wi) & 1);
        float mu = s->grip;
        if (s->boat && !on_water) mu = 0.9f; // dragging on land: it grips, but only crawls along
        if (!s->boat && d->handbrake && rear) mu *= 0.35f; // the back slides out
        float jl = -vlat * per;
        float jd = 0.0f;
        if (!s->boat) {
            float t = d->throttle;
            if (d->handbrake && rear) {
                jd = -copysignf(fminf(fabsf(vlong) * per, s->brake * per * dt), vlong);
            } else if (t * vlong < -0.01f && fabsf(vlong) > 40.0f) {
                jd = -copysignf(fminf(fabsf(vlong) * per, s->brake * per * dt * fabsf(t)), vlong); // braking
            } else if ((s->drive_mask >> wi) & 1) {
                float top = t > 0.0f ? s->top_speed : s->reverse_speed;
                jd = s->engine * t * clampf(1.0f - fabsf(vlong) / top, 0.0f, 1.0f) * per * dt;
                if (fabsf(t) < 0.05f) jd = -vlong * per * 0.5f * dt; // coasting slows down
            }
        }
        // The tyre can only take so much before it slides (the friction circle)
        float lim_j = mu * force * dt;
        float tot = sqrtf(jl * jl + jd * jd);
        if (tot > lim_j && tot > 0.0f) {
            jl *= lim_j / tot;
            jd *= lim_j / tot;
        }
        // Pushed level with the centre of mass rather than at the contact patch: cornering doesn't roll it
        // over (HL2's Jeep is just as forgiving)
        vec3 at = v3_mad(p, up, v3_dot(v3_sub(b->pos, p), up));
        phys_apply_impulse(b, at, v3_add(v3_scale(side, jl), v3_scale(wf, jd)));
    }

    if (!s->boat && touching >= 2) {
        // Body roll dies away quickly on the ground, so it leans in corners but rarely tips
        float roll = v3_dot(b->angvel, fwd);
        b->angvel = v3_mad(b->angvel, fwd, -roll * clampf(dt * 5.0f, 0.0f, 1.0f));
    } else if (!s->boat && d->driven && !touching) {
        // In the air: a little help to land on the wheels (it turns towards level, gently)
        vec3 axis = v3_cross(up, v3(0, 0, 1));
        b->angvel = v3_mad(b->angvel, axis, 1.5f * dt);
        b->angvel = v3_scale(b->angvel, 1.0f - clampf(dt * 0.8f, 0.0f, 1.0f));
    }

    if (s->boat && touching) {

        // The fan pushes along the hull, flat; the rudders turn it, better the faster it goes
        vec3 flat = v3(fwd.x, fwd.y, 0.0f);
        float fl = v3_len(flat);
        if (fl > 1e-3f) {
            flat = v3_scale(flat, 1.0f / fl);
            float t = d->throttle;
            float top = t > 0.0f ? s->top_speed : s->reverse_speed;
            float acc = s->engine * t * clampf(1.0f - fabsf(vf) / top, 0.0f, 1.0f) * (on_water ? 1.0f : 0.3f);
            b->vel = v3_mad(b->vel, flat, acc * dt);
        }
        float yaw_rate = v3_dot(b->angvel, up);
        float want = -d->steer * (0.9f + 0.8f * clampf(fabsf(vf) / 600.0f, 0.0f, 1.0f));
        if (d->driven) b->angvel = v3_mad(b->angvel, up, (want - yaw_rate) * clampf(dt * 4.0f, 0.0f, 1.0f));
        if (on_water) b->vel = v3_scale(b->vel, 1.0f - 0.25f * dt);
    }

    // Upside down and nearly stopped: roll it back onto its wheels, like HL2 does for you
    if (d->driven && up.z < 0.3f && v3_len(b->vel) < 120.0f) {
        vec3 axis = v3_cross(up, v3(0, 0, 1));
        b->angvel = v3_mad(b->angvel, axis, 6.0f * dt);
        b->vel.z += GRAV * 0.6f * dt;
    }
}

static void step_hook(PhysWorld *w, Map *m, float dt) {
    for (int i = 0; i < MAX_BODIES; i++) {
        Body *b = &w->bodies[i];
        if (!b->active || b->frozen || b->held) continue;
        const VehSpec *s = spec_of(vehicle_kind(b));
        if (s) ride(w, m, i, s, dt);
    }
}

void vehicles_init(void) {
    memset(g_drive, 0, sizeof(g_drive));
    phys_set_step_hook(step_hook);
}
