#include "ents.h"

#include <pspgu.h>
#include <pspgum.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "collision.h"

#define USE_REACH 96.0f
#define MAX_PENDING 32
#define SPIN_ACCEL 60.0f // func_rotating spin-up, degrees/s^2
#define VTYPE (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D)

// Door flags (Source spawnflags)
#define SF_DOOR_START_OPEN 1
#define SF_DOOR_USE_OPENS 256
#define SF_DOOR_TOUCH_OPENS 1024
#define SF_DOOR_SILENT 4096
#define SF_BUTTON_TOUCH 256

typedef struct {
    float time;
    int target;
    int input;
    vec3 from; // who set it off (a door swings away from them)
} Pending;

static Pending g_pending[MAX_PENDING];
static int g_num_pending;
static float g_clock;
static int g_snd_move = -1, g_snd_stop = -1, g_snd_button = -1;

static void update_transform(const Map *m, int i) {
    const PmapEnt *e = &m->ents[i];
    EntState *s = &m->ent_state[i];
    vec3 origin = v3(e->origin[0], e->origin[1], e->origin[2]);
    vec3 axis = v3(e->axis[0], e->axis[1], e->axis[2]);
    s->pos = origin;
    s->rot = q_identity();
    s->rotated = 0;
    switch (e->type) {
    case PMAP_ENT_DOOR:
    case PMAP_ENT_BUTTON:
        s->pos = v3_mad(origin, axis, e->distance * s->progress);
        break;
    case PMAP_ENT_DOOR_ROTATING:
        s->rot = q_axis_angle(axis, e->distance * s->progress * s->open_sign * (3.14159265f / 180.0f));
        s->rotated = s->progress > 0.0f;
        break;
    case PMAP_ENT_ROTATING:
        s->rot = q_axis_angle(axis, s->angle * (3.14159265f / 180.0f));
        s->rotated = 1;
        break;
    default:
        break;
    }
    // World bounds from the 8 transformed corners
    vec3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
    for (int c = 0; c < 8; c++) {
        vec3 l = v3((c & 1) ? e->maxs[0] : e->mins[0], (c & 2) ? e->maxs[1] : e->mins[1], (c & 4) ? e->maxs[2] : e->mins[2]);
        vec3 w = v3_add(s->pos, s->rotated ? q_rotate(s->rot, l) : l);
        lo = v3(fminf(lo.x, w.x), fminf(lo.y, w.y), fminf(lo.z, w.z));
        hi = v3(fmaxf(hi.x, w.x), fmaxf(hi.y, w.y), fmaxf(hi.z, w.z));
    }
    s->wmin = lo;
    s->wmax = hi;
}

// A switchable light style is lit while any light entity using it is on.
static void update_light_switches(Map *m) {
    unsigned char seen[64] = {0}, on[64] = {0};
    for (int i = 0; i < m->num_ents; i++) {
        const PmapEnt *e = &m->ents[i];
        if (e->type != PMAP_ENT_LIGHT) continue;
        int s = (int)e->distance & 63;
        seen[s] = 1;
        on[s] |= m->ent_state[i].on;
    }
    for (int s = 0; s < 64; s++) {
        if (seen[s]) m->style_switch[s] = on[s];
    }
}

int ents_init(Map *m) {
    g_num_pending = 0;
    g_clock = 0.0f;
    g_snd_move = snd_find("door_move");
    g_snd_stop = snd_find("door_stop");
    g_snd_button = snd_find("button");
    if (m->num_ents == 0) return 0;
    m->ent_state = calloc(m->num_ents, sizeof(EntState));
    if (!m->ent_state) return -1;
    for (int i = 0; i < m->num_ents; i++) {
        const PmapEnt *e = &m->ents[i];
        EntState *s = &m->ent_state[i];
        s->open_sign = 1.0f;
        if ((e->type == PMAP_ENT_DOOR || e->type == PMAP_ENT_DOOR_ROTATING) && (e->flags & SF_DOOR_START_OPEN)) {
            s->state = ENT_OPEN;
            s->progress = 1.0f;
            s->timer = -1.0f;
        }
        if (e->type == PMAP_ENT_ROTATING && (e->flags & 1)) s->on = 1; // "start on"
        if (e->type == PMAP_ENT_LIGHT) s->on = !(e->flags & 1);        // "initially dark"
        update_transform(m, i);
    }
    update_light_switches(m);
    return 0;
}

static vec3 ent_center_world(const Map *m, int i, float progress, float sign) {
    const PmapEnt *e = &m->ents[i];
    vec3 c = v3((e->mins[0] + e->maxs[0]) * 0.5f, (e->mins[1] + e->maxs[1]) * 0.5f, (e->mins[2] + e->maxs[2]) * 0.5f);
    quat q = q_axis_angle(v3(e->axis[0], e->axis[1], e->axis[2]), e->distance * progress * sign * (3.14159265f / 180.0f));
    return v3_add(v3(e->origin[0], e->origin[1], e->origin[2]), q_rotate(q, c));
}

static void fire(Map *m, int i, unsigned int event, vec3 from) {
    const PmapEnt *e = &m->ents[i];
    for (uint32_t k = 0; k < e->num_outputs; k++) {
        const PmapOutput *o = &m->outputs[e->first_output + k];
        if (o->event != event || o->target < 0 || o->target >= m->num_ents) continue;
        if (g_num_pending < MAX_PENDING) g_pending[g_num_pending++] = (Pending){g_clock + o->delay, o->target, (int)o->input, from};
    }
}

static void play_at(const Map *m, int i, int snd) {
    if (m->ents[i].flags & SF_DOOR_SILENT) return;
    const EntState *s = &m->ent_state[i];
    snd_play_at(snd, v3_scale(v3_add(s->wmin, s->wmax), 0.5f), 0.8f);
}

static void door_open(Map *m, int i, vec3 from) {
    const PmapEnt *e = &m->ents[i];
    EntState *s = &m->ent_state[i];
    if (s->state == ENT_OPEN || s->state == ENT_OPENING) return;
    if (e->type == PMAP_ENT_DOOR_ROTATING && s->progress <= 0.0f) {
        // Swing away from whoever opened it
        float d1 = v3_len(v3_sub(ent_center_world(m, i, 0.5f, 1.0f), from));
        float d2 = v3_len(v3_sub(ent_center_world(m, i, 0.5f, -1.0f), from));
        s->open_sign = d1 >= d2 ? 1.0f : -1.0f;
    }
    s->state = ENT_OPENING;
    play_at(m, i, g_snd_move);
    fire(m, i, PMAP_EV_STARTOPEN, from);
}

static void door_close(Map *m, int i) {
    EntState *s = &m->ent_state[i];
    if (s->state == ENT_CLOSED || s->state == ENT_CLOSING) return;
    s->state = ENT_CLOSING;
    play_at(m, i, g_snd_move);
    fire(m, i, PMAP_EV_STARTCLOSE, s->pos);
}

static void activate(Map *m, int i, vec3 from) {
    const PmapEnt *e = &m->ents[i];
    EntState *s = &m->ent_state[i];
    switch (e->type) {
    case PMAP_ENT_DOOR:
    case PMAP_ENT_DOOR_ROTATING:
        if (s->state == ENT_OPEN && e->wait < 0.0f) {
            door_close(m, i); // toggle doors that stay open
        } else if (s->state == ENT_CLOSED || s->state == ENT_CLOSING) {
            door_open(m, i, from);
        }
        break;
    case PMAP_ENT_BUTTON:
        if (s->state != ENT_CLOSED) break;
        s->state = ENT_OPENING;
        snd_play_at(g_snd_button, v3_scale(v3_add(s->wmin, s->wmax), 0.5f), 0.9f);
        fire(m, i, PMAP_EV_ACTIVATE, from);
        break;
    case PMAP_ENT_ROTATING:
        s->on = !s->on;
        break;
    default:
        break;
    }
}

static void apply_input(Map *m, int i, int input, vec3 from) {
    const PmapEnt *e = &m->ents[i];
    EntState *s = &m->ent_state[i];
    if (e->type == PMAP_ENT_LIGHT) {
        if (input == PMAP_IN_START || input == PMAP_IN_OPEN) s->on = 1;
        if (input == PMAP_IN_STOP || input == PMAP_IN_CLOSE) s->on = 0;
        if (input == PMAP_IN_TOGGLE) s->on = !s->on;
        update_light_switches(m);
        return;
    }
    switch (input) {
    case PMAP_IN_OPEN: door_open(m, i, from); break;
    case PMAP_IN_CLOSE: door_close(m, i); break;
    case PMAP_IN_TOGGLE:
        if (e->type == PMAP_ENT_ROTATING) {
            s->on = !s->on;
        } else if (s->state == ENT_OPEN || s->state == ENT_OPENING) {
            door_close(m, i);
        } else {
            door_open(m, i, from);
        }
        break;
    case PMAP_IN_START: s->on = 1; break;
    case PMAP_IN_STOP: s->on = 0; break;
    case PMAP_IN_PRESS: activate(m, i, from); break;
    case PMAP_IN_REVERSE: s->spin = -s->spin; break;
    default: break;
    }
}

static int boxes_touch(vec3 amin, vec3 amax, vec3 bmin, vec3 bmax) {
    return amin.x <= bmax.x && amax.x >= bmin.x && amin.y <= bmax.y && amax.y >= bmin.y && amin.z <= bmax.z &&
           amax.z >= bmin.z;
}

void ents_update(Map *m, float dt, vec3 pmin, vec3 pmax) {
    g_clock += dt;
    map_update_lightstyles(m, g_clock);
    vec3 pcenter = v3_scale(v3_add(pmin, pmax), 0.5f);
    for (int i = 0; i < g_num_pending;) {
        if (g_pending[i].time <= g_clock) {
            Pending p = g_pending[i];
            g_pending[i] = g_pending[--g_num_pending];
            apply_input(m, p.target, p.input, p.from);
        } else {
            i++;
        }
    }

    for (int i = 0; i < m->num_ents; i++) {
        const PmapEnt *e = &m->ents[i];
        EntState *s = &m->ent_state[i];
        if (e->type == PMAP_ENT_LIGHT) continue;
        if (e->type == PMAP_ENT_TRIGGER) {
            vec3 tmin = v3(e->origin[0] + e->mins[0], e->origin[1] + e->mins[1], e->origin[2] + e->mins[2]);
            vec3 tmax = v3(e->origin[0] + e->maxs[0], e->origin[1] + e->maxs[1], e->origin[2] + e->maxs[2]);
            int in = boxes_touch(pmin, pmax, tmin, tmax);
            if (in && !s->inside && !s->done) {
                fire(m, i, PMAP_EV_ACTIVATE, pcenter);
                if (e->flags & PMAP_ENT_TRIGGER_ONCE) s->done = 1;
            } else if (!in && s->inside) {
                fire(m, i, PMAP_EV_UNTOUCH, pcenter); // e.g. TF2's spawn doors close behind you
            }
            s->inside = in;
            continue;
        }
        if (e->type == PMAP_ENT_ROTATING) {
            float target = s->on ? e->speed : 0.0f;
            float step = SPIN_ACCEL * dt;
            s->spin += clampf(target - s->spin, -step, step);
            if (s->spin != 0.0f) {
                s->angle = fmodf(s->angle + s->spin * dt, 360.0f);
                update_transform(m, i);
            }
            continue;
        }
        // Doors and buttons
        if ((e->type == PMAP_ENT_DOOR || e->type == PMAP_ENT_DOOR_ROTATING) && (e->flags & SF_DOOR_TOUCH_OPENS) &&
            s->state == ENT_CLOSED) {
            vec3 grow = v3(4, 4, 4);
            if (boxes_touch(v3_sub(pmin, grow), v3_add(pmax, grow), s->wmin, s->wmax)) door_open(m, i, pcenter);
        }
        float travel = fabsf(e->distance);
        float rate = travel > 0.01f ? e->speed / travel : 10.0f;
        if (s->state == ENT_OPENING) {
            s->progress += rate * dt;
            if (s->progress >= 1.0f) {
                s->progress = 1.0f;
                s->state = ENT_OPEN;
                s->timer = e->wait;
                if (e->type != PMAP_ENT_BUTTON) {
                    play_at(m, i, g_snd_stop);
                    fire(m, i, PMAP_EV_OPEN, pcenter);
                }
            }
            update_transform(m, i);
        } else if (s->state == ENT_CLOSING) {
            float before = s->progress;
            s->progress -= rate * dt;
            if (s->progress < 0.0f) s->progress = 0.0f;
            update_transform(m, i);
            // Don't crush the player: bounce back open
            if (e->type != PMAP_ENT_BUTTON && boxes_touch(pmin, pmax, s->wmin, s->wmax) && e->solid) {
                s->progress = before;
                s->state = ENT_OPENING;
                update_transform(m, i);
            } else if (s->progress <= 0.0f) {
                s->state = ENT_CLOSED;
                if (e->type != PMAP_ENT_BUTTON) {
                    play_at(m, i, g_snd_stop);
                    fire(m, i, PMAP_EV_CLOSE, pcenter);
                }
            }
        } else if (s->state == ENT_OPEN && s->timer >= 0.0f) {
            s->timer -= dt;
            if (s->timer <= 0.0f) {
                if (e->type == PMAP_ENT_BUTTON) {
                    s->state = ENT_CLOSING;
                } else {
                    door_close(m, i);
                }
            }
        }
    }
}

// --- Multiplayer ----------------------------------------------------------------------------------

int ents_net_pack(const Map *m, int first, int count, unsigned char *out) {
    for (int k = 0; k < count; k++) {
        const EntState *s = &m->ent_state[first + k];
        unsigned int v = (unsigned int)(clampf(s->progress, 0.0f, 1.0f) * 65535.0f);
        out[k * 4 + 0] = (unsigned char)((s->state & 3) | (s->on ? 4 : 0) | (s->open_sign < 0.0f ? 8 : 0) | (s->done ? 16 : 0));
        out[k * 4 + 1] = 0;
        out[k * 4 + 2] = (unsigned char)(v & 0xFF);
        out[k * 4 + 3] = (unsigned char)(v >> 8);
    }
    return count * 4;
}

void ents_net_unpack(Map *m, int first, int count, const unsigned char *in) {
    int lights = 0;
    for (int k = 0; k < count && first + k < m->num_ents; k++) {
        int i = first + k;
        const PmapEnt *e = &m->ents[i];
        EntState *s = &m->ent_state[i];
        int state = in[k * 4] & 3, on = (in[k * 4] & 4) != 0;
        float sign = (in[k * 4] & 8) ? -1.0f : 1.0f;
        float progress = (float)(in[k * 4 + 2] | (in[k * 4 + 3] << 8)) / 65535.0f;
        if (e->type == PMAP_ENT_LIGHT || e->type == PMAP_ENT_ROTATING) {
            lights |= e->type == PMAP_ENT_LIGHT && s->on != on;
            s->on = on;
            continue;
        }
        if (e->type != PMAP_ENT_DOOR && e->type != PMAP_ENT_DOOR_ROTATING && e->type != PMAP_ENT_BUTTON) continue;
        // Sounds for what the host's doors started doing
        if (state != s->state) {
            if (e->type == PMAP_ENT_BUTTON) {
                if (state == ENT_OPENING) play_at(m, i, g_snd_button);
            } else if (state == ENT_OPENING || state == ENT_CLOSING) {
                if (!(e->flags & SF_DOOR_SILENT)) play_at(m, i, g_snd_move);
            } else if (!(e->flags & SF_DOOR_SILENT)) {
                play_at(m, i, g_snd_stop);
            }
        }
        s->state = state;
        s->open_sign = sign;
        // Small differences are just latency: let ents_client_update keep the motion smooth
        if (fabsf(progress - s->progress) > 0.15f || state == ENT_OPEN || state == ENT_CLOSED) s->progress = progress;
        update_transform(m, i);
    }
    if (lights) update_light_switches(m);
}

void ents_client_update(Map *m, float dt) {
    g_clock += dt;
    map_update_lightstyles(m, g_clock);
    for (int i = 0; i < m->num_ents; i++) {
        const PmapEnt *e = &m->ents[i];
        EntState *s = &m->ent_state[i];
        if (e->type == PMAP_ENT_ROTATING) {
            float target = s->on ? e->speed : 0.0f;
            float step = SPIN_ACCEL * dt;
            s->spin += clampf(target - s->spin, -step, step);
            if (s->spin != 0.0f) {
                s->angle = fmodf(s->angle + s->spin * dt, 360.0f);
                update_transform(m, i);
            }
        } else if (e->type == PMAP_ENT_DOOR || e->type == PMAP_ENT_DOOR_ROTATING || e->type == PMAP_ENT_BUTTON) {
            float travel = fabsf(e->distance);
            float rate = travel > 0.01f ? e->speed / travel : 10.0f;
            if (s->state == ENT_OPENING && s->progress < 1.0f) {
                s->progress = fminf(1.0f, s->progress + rate * dt);
                update_transform(m, i);
            } else if (s->state == ENT_CLOSING && s->progress > 0.0f) {
                s->progress = fmaxf(0.0f, s->progress - rate * dt);
                update_transform(m, i);
            }
        }
    }
}

int ents_use(Map *m, vec3 eye, vec3 dir) {
    // What's the nearest usable entity box along the ray, before any wall?
    Trace tr;
    cm_trace_line(m, eye, v3_mad(eye, dir, USE_REACH), MASK_PLAYERSOLID, NULL, 0, &tr);
    int best = -1;
    float best_t = USE_REACH + 8.0f;
    for (int i = 0; i < m->num_ents; i++) {
        const PmapEnt *e = &m->ents[i];
        if (e->type == PMAP_ENT_TRIGGER || e->type == PMAP_ENT_LIGHT) continue;
        const EntState *s = &m->ent_state[i];
        // Slab test against the world bounds, grown a little so thin doors are easy to hit
        float tmin = 0.0f, tmax = best_t;
        const float o[3] = {eye.x, eye.y, eye.z}, d[3] = {dir.x, dir.y, dir.z};
        const float lo[3] = {s->wmin.x - 4, s->wmin.y - 4, s->wmin.z - 4}, hi[3] = {s->wmax.x + 4, s->wmax.y + 4, s->wmax.z + 4};
        int hit = 1;
        for (int k = 0; k < 3 && hit; k++) {
            if (fabsf(d[k]) < 1e-6f) {
                if (o[k] < lo[k] || o[k] > hi[k]) hit = 0;
            } else {
                float t1 = (lo[k] - o[k]) / d[k], t2 = (hi[k] - o[k]) / d[k];
                if (t1 > t2) {
                    float t = t1;
                    t1 = t2;
                    t2 = t;
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
    if (best < 0) return 0;
    // A wall in between blocks the use (entities themselves are part of the trace, so allow a margin)
    if (tr.fraction * USE_REACH + 12.0f < best_t) return 0;
    activate(m, best, eye);
    return 1;
}

void ents_draw(const Map *m, const Camera *cam, int *tris) {
    for (int i = 0; i < m->num_ents; i++) {
        const PmapEnt *e = &m->ents[i];
        if (!e->num_batches) continue;
        const EntState *s = &m->ent_state[i];
        // Rough distance cull
        vec3 c = v3_scale(v3_add(s->wmin, s->wmax), 0.5f);
        if (v3_len(v3_sub(c, cam->pos)) > m->info->fog_end + 256.0f) continue;
        ScePspFMatrix4 mat;
        vec3 ax = q_rotate(s->rot, v3(1, 0, 0)), ay = q_rotate(s->rot, v3(0, 1, 0)), az = q_rotate(s->rot, v3(0, 0, 1));
        mat.x = (ScePspFVector4){ax.x, ax.y, ax.z, 0.0f};
        mat.y = (ScePspFVector4){ay.x, ay.y, ay.z, 0.0f};
        mat.z = (ScePspFVector4){az.x, az.y, az.z, 0.0f};
        mat.w = (ScePspFVector4){s->pos.x, s->pos.y, s->pos.z, 1.0f};
        for (uint32_t k = 0; k < e->num_batches; k++) {
            const PmapBatch *b = &m->ent_batches[e->first_batch + k];
            map_bind_texture(m, b->tex);
            map_batch_begin(b, &mat);
            sceGuDrawArray(GU_TRIANGLES, VTYPE, b->num_indices, m->indices + b->first_index, m->verts + b->first_vertex);
            *tris += b->num_indices / 3;
        }
    }
    map_batch_end();
}
