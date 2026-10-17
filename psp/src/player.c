#include "player.h"

#include <string.h>

// GMod defaults where they exist (sv_gravity 600, jump power 200, sv_friction 8, step 18).
#define GRAVITY 600.0f
#define JUMP_SPEED 200.0f
#define MAX_SPEED 250.0f
#define DUCK_SPEED 90.0f
#define ACCELERATE 10.0f
#define AIR_ACCELERATE 10.0f
#define AIR_WISH_CAP 30.0f
#define FRICTION 8.0f
#define STOP_SPEED 100.0f
#define STEP_SIZE 18.0f
#define MIN_WALK_NORMAL 0.7f
#define NOCLIP_SPEED 600.0f
#define MAX_VELOCITY 3500.0f

#define STAND_HEIGHT 72.0f
#define DUCK_HEIGHT 36.0f
#define STAND_EYE 64.0f
#define DUCK_EYE 28.0f
#define HULL_HALF 16.0f

#define DEG2RAD (3.14159265f / 180.0f)

typedef struct {
    Map *map;
    const CmBox *props;
    int num_props;
    vec3 mins, maxs;
} MoveCtx;

static void trace(const MoveCtx *c, vec3 a, vec3 b, Trace *tr) {
    cm_trace(c->map, a, b, c->mins, c->maxs, MASK_PLAYERSOLID, c->props, c->num_props, tr);
}

void player_hull(const Player *p, vec3 *mins, vec3 *maxs) {
    float half = p->hull_half > 0.0f ? p->hull_half : HULL_HALF;
    float h = p->ducked ? DUCK_HEIGHT : STAND_HEIGHT;
    if (p->hull_height > 0.0f && p->hull_height < h) h = p->hull_height;
    *mins = v3(-half, -half, 0.0f);
    *maxs = v3(half, half, h);
}

// Where the eye settles: a person's height, or just under the top of a smaller hull
static float eye_target(const Player *p, float eye) {
    if (p->hull_height > 0.0f && eye > p->hull_height - 4.0f) eye = p->hull_height - 4.0f;
    return eye < 4.0f ? 4.0f : eye;
}

void player_spawn(Player *p, vec3 feet) {
    memset(p, 0, sizeof(*p));
    p->origin = feet;
    p->view_height = STAND_EYE;
    p->ground_normal = v3(0, 0, 1);
}

vec3 player_eye(const Player *p) {
    return v3(p->origin.x, p->origin.y, p->origin.z + p->view_height);
}

static vec3 clip_velocity(vec3 v, vec3 n, float overbounce) {
    float back = v3_dot(v, n) * overbounce;
    vec3 out = v3_sub(v, v3_scale(n, back));
    // Kill tiny components that would keep us grinding into the plane
    if (out.x > -0.1f && out.x < 0.1f) out.x = 0.0f;
    if (out.y > -0.1f && out.y < 0.1f) out.y = 0.0f;
    if (out.z > -0.1f && out.z < 0.1f) out.z = 0.0f;
    return out;
}

// Quake's PM_SlideMove: move, and on each hit slide along the surfaces touched so far.
static void slide_move(Player *p, const MoveCtx *c, float dt) {
    vec3 planes[5];
    int numplanes = 0;
    vec3 primal = p->velocity;
    float time_left = dt;

    for (int bump = 0; bump < 4; bump++) {
        vec3 end = v3_mad(p->origin, p->velocity, time_left);
        Trace tr;
        trace(c, p->origin, end, &tr);
        if (tr.allsolid) {
            p->velocity.z = 0.0f; // stuck; let the unstick pass deal with it
            return;
        }
        if (tr.fraction > 0.0f) {
            p->origin = tr.endpos;
            numplanes = 0;
        }
        if (tr.fraction >= 1.0f) {
            return;
        }
        time_left -= time_left * tr.fraction;
        if (numplanes >= 5) {
            p->velocity = v3(0, 0, 0);
            return;
        }
        planes[numplanes++] = tr.normal;

        int i, j;
        for (i = 0; i < numplanes; i++) {
            p->velocity = clip_velocity(p->velocity, planes[i], 1.001f);
            for (j = 0; j < numplanes; j++) {
                if (j != i && v3_dot(p->velocity, planes[j]) < 0.0f) break;
            }
            if (j == numplanes) break;
        }
        if (i == numplanes) {
            if (numplanes != 2) {
                p->velocity = v3(0, 0, 0);
                return;
            }
            // Slide along the crease between two planes
            vec3 dir = v3_cross(planes[0], planes[1]);
            p->velocity = v3_scale(dir, v3_dot(dir, p->velocity));
        }
        if (v3_dot(p->velocity, primal) <= 0.0f) {
            p->velocity = v3(0, 0, 0);
            return;
        }
    }
}

// Tries the move both flat and stepped up by STEP_SIZE, and keeps whichever got further (stairs, kerbs).
static void step_slide_move(Player *p, const MoveCtx *c, float dt) {
    vec3 start_o = p->origin, start_v = p->velocity;
    slide_move(p, c, dt);
    vec3 down_o = p->origin, down_v = p->velocity;

    if (!p->on_ground) return;

    Trace tr;
    vec3 up = v3(start_o.x, start_o.y, start_o.z + STEP_SIZE);
    trace(c, start_o, up, &tr);
    if (tr.allsolid) return;
    p->origin = tr.endpos;
    p->velocity = start_v;
    slide_move(p, c, dt);

    vec3 down = v3(p->origin.x, p->origin.y, p->origin.z - STEP_SIZE - 1.0f);
    trace(c, p->origin, down, &tr);
    if (!tr.allsolid) p->origin = tr.endpos;
    // Stepping onto something too steep to stand on doesn't count.
    if (tr.fraction < 1.0f && tr.normal.z < MIN_WALK_NORMAL) {
        p->origin = down_o;
        p->velocity = down_v;
        return;
    }
    float dx = p->origin.x - start_o.x, dy = p->origin.y - start_o.y;
    float ddx = down_o.x - start_o.x, ddy = down_o.y - start_o.y;
    if (ddx * ddx + ddy * ddy > dx * dx + dy * dy) {
        p->origin = down_o;
        p->velocity = down_v;
    } else {
        p->velocity.z = down_v.z;
    }
}

static void categorize(Player *p, const MoveCtx *c) {
    if (p->velocity.z > 180.0f) { // moving up fast: jumping, not standing
        p->on_ground = 0;
        return;
    }
    Trace tr;
    vec3 below = v3(p->origin.x, p->origin.y, p->origin.z - 2.0f);
    trace(c, p->origin, below, &tr);
    if (tr.fraction < 1.0f && tr.normal.z >= MIN_WALK_NORMAL) {
        p->on_ground = 1;
        p->ground_normal = tr.normal;
        if (!tr.startsolid) p->origin = tr.endpos; // stick to slopes when walking down them
    } else {
        p->on_ground = 0;
    }
}

static void friction(Player *p, float dt) {
    vec3 v = p->velocity;
    float speed = sqrtf(v.x * v.x + v.y * v.y);
    if (speed < 0.1f) {
        p->velocity.x = p->velocity.y = 0.0f;
        return;
    }
    float control = speed < STOP_SPEED ? STOP_SPEED : speed;
    float newspeed = speed - control * FRICTION * dt;
    if (newspeed < 0.0f) newspeed = 0.0f;
    float k = newspeed / speed;
    p->velocity.x *= k;
    p->velocity.y *= k;
}

static void accelerate(Player *p, vec3 wishdir, float wishspeed, float accel, float cap, float dt) {
    float limited = wishspeed < cap ? wishspeed : cap;
    float add = limited - v3_dot(p->velocity, wishdir);
    if (add <= 0.0f) return;
    float a = accel * dt * wishspeed;
    if (a > add) a = add;
    p->velocity = v3_mad(p->velocity, wishdir, a);
}

// Gets the player out of geometry if they spawned or ducked into it.
static void unstick(Player *p, const MoveCtx *c) {
    Trace tr;
    trace(c, p->origin, p->origin, &tr);
    if (!tr.startsolid) return;
    for (int z = 1; z <= 64; z += 4) {
        vec3 o = v3(p->origin.x, p->origin.y, p->origin.z + (float)z);
        trace(c, o, o, &tr);
        if (!tr.startsolid) {
            p->origin = o;
            return;
        }
    }
}

void player_move(Player *p, Map *m, const MoveCmd *cmd, const CmBox *props, int num_props, float dt) {
    if (dt <= 0.0f) return;
    float cy = cosf(cmd->yaw * DEG2RAD), sy = sinf(cmd->yaw * DEG2RAD);

    if (p->noclip) {
        float cp = cosf(cmd->pitch * DEG2RAD), sp = sinf(cmd->pitch * DEG2RAD);
        vec3 fwd = v3(cp * cy, cp * sy, sp);
        vec3 right = v3(sy, -cy, 0.0f);
        float speed = NOCLIP_SPEED * (cmd->fast ? 3.0f : 1.0f);
        vec3 wish = v3_add(v3_scale(fwd, cmd->forward), v3_scale(right, cmd->side));
        if (cmd->jump) wish.z += 1.0f;
        if (cmd->duck) wish.z -= 1.0f;
        p->velocity = v3_scale(wish, speed);
        p->origin = v3_mad(p->origin, p->velocity, dt);
        p->on_ground = 0;
        p->ducked = 0;
        p->view_height = STAND_EYE;
        return;
    }

    MoveCtx c = {m, props, num_props, {0, 0, 0}, {0, 0, 0}};

    // Water level: feet, waist, eyes
    p->water_level = 0;
    if (cm_point_contents(m, v3(p->origin.x, p->origin.y, p->origin.z + 2.0f)) & PMAP_CONTENTS_WATER) {
        p->water_level = 1;
        if (cm_point_contents(m, v3(p->origin.x, p->origin.y, p->origin.z + 36.0f)) & PMAP_CONTENTS_WATER) {
            p->water_level = 2;
            if (cm_point_contents(m, player_eye(p)) & PMAP_CONTENTS_WATER) p->water_level = 3;
        }
    }
    if (p->water_level >= 2) {
        // Swim toward where we look; jump swims up. Water slows everything down.
        p->ducked = 0;
        player_hull(p, &c.mins, &c.maxs);
        unstick(p, &c);
        float cp = cosf(cmd->pitch * DEG2RAD), sp = sinf(cmd->pitch * DEG2RAD);
        vec3 fwd = v3(cp * cy, cp * sy, sp);
        vec3 right = v3(sy, -cy, 0.0f);
        vec3 wish = v3_add(v3_scale(fwd, cmd->forward), v3_scale(right, cmd->side));
        if (cmd->jump) wish.z += 1.0f;
        float ws = v3_len(wish);
        vec3 wishdir = ws > 0.0f ? v3_scale(wish, 1.0f / ws) : wish;
        if (ws > 1.0f) ws = 1.0f;
        float wishspeed = ws * MAX_SPEED * 0.8f * (cmd->speed > 0.0f ? cmd->speed : 1.0f);
        if (ws == 0.0f) { // drift down slowly
            wishdir = v3(0, 0, -1);
            wishspeed = 40.0f;
        }
        // Water friction
        float speed = v3_len(p->velocity);
        if (speed > 0.1f) {
            float ns = speed - speed * 4.0f * dt;
            p->velocity = v3_scale(p->velocity, (ns < 0.0f ? 0.0f : ns) / speed);
        }
        accelerate(p, wishdir, wishspeed, ACCELERATE, wishspeed, dt);
        // Hop out at the surface
        if (cmd->jump && p->water_level == 2 && p->velocity.z < JUMP_SPEED * 0.6f) p->velocity.z = JUMP_SPEED * 0.6f;
        p->on_ground = 0;
        step_slide_move(p, &c, dt);
        p->view_height += (eye_target(p, STAND_EYE) - p->view_height) * clampf(dt * 12.0f, 0.0f, 1.0f);
        return;
    }

    // Crouch instantly; stand back up only if there's room.
    if (cmd->duck) {
        p->ducked = 1;
    } else if (p->ducked) {
        p->ducked = 0;
        player_hull(p, &c.mins, &c.maxs);
        Trace tr;
        trace(&c, p->origin, p->origin, &tr);
        if (tr.startsolid) p->ducked = 1;
    }
    player_hull(p, &c.mins, &c.maxs);
    unstick(p, &c);

    categorize(p, &c);

    vec3 fwd = v3(cy, sy, 0.0f);
    vec3 right = v3(sy, -cy, 0.0f);
    vec3 wish = v3_add(v3_scale(fwd, cmd->forward), v3_scale(right, cmd->side));
    float wishspeed = v3_len(wish);
    vec3 wishdir = wishspeed > 0.0f ? v3_scale(wish, 1.0f / wishspeed) : wish;
    if (wishspeed > 1.0f) wishspeed = 1.0f;
    wishspeed *= (p->ducked ? DUCK_SPEED : MAX_SPEED) * (cmd->speed > 0.0f ? cmd->speed : 1.0f);

    if (p->on_ground) {
        p->velocity.z = 0.0f;
        if (cmd->jump) {
            p->velocity.z = JUMP_SPEED;
            p->on_ground = 0;
        } else {
            friction(p, dt);
        }
    }

    if (p->on_ground && cmd->walk_speed > 0.0f && !cmd->jump) {
        p->velocity.x = fwd.x * cmd->walk_speed;
        p->velocity.y = fwd.y * cmd->walk_speed;
    } else if (p->on_ground) {
        accelerate(p, wishdir, wishspeed, ACCELERATE, wishspeed, dt);
    } else {
        accelerate(p, wishdir, wishspeed, AIR_ACCELERATE, AIR_WISH_CAP, dt);
        p->velocity.z -= GRAVITY * dt;
    }
    if (p->velocity.z < -MAX_VELOCITY) p->velocity.z = -MAX_VELOCITY;

    step_slide_move(p, &c, dt);
    categorize(p, &c);

    float target = eye_target(p, p->ducked ? DUCK_EYE : STAND_EYE);
    p->view_height += (target - p->view_height) * clampf(dt * 12.0f, 0.0f, 1.0f);
}
