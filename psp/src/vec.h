#pragma once

#include <math.h>
#include <psptypes.h>

typedef ScePspFVector3 vec3;

static inline vec3 v3(float x, float y, float z) {
    vec3 r = {x, y, z};
    return r;
}
static inline vec3 v3_add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3_sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3_scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline vec3 v3_mad(vec3 a, vec3 b, float s) { return v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
static inline float v3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 v3_cross(vec3 a, vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float v3_len(vec3 a) { return sqrtf(v3_dot(a, a)); }
static inline vec3 v3_norm(vec3 a) {
    float l = v3_len(a);
    return l > 1e-6f ? v3_scale(a, 1.0f / l) : v3(0.0f, 0.0f, 0.0f);
}
static inline vec3 v3_lerp(vec3 a, vec3 b, float t) { return v3_mad(a, v3_sub(b, a), t); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Unit quaternions for rigid-body orientation
typedef struct {
    float x, y, z, w;
} quat;

static inline quat q_identity(void) {
    quat q = {0.0f, 0.0f, 0.0f, 1.0f};
    return q;
}
static inline quat q_mul(quat a, quat b) {
    quat r = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    return r;
}
static inline quat q_conj(quat q) {
    quat r = {-q.x, -q.y, -q.z, q.w};
    return r;
}
static inline quat q_norm(quat q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-6f) return q_identity();
    float k = 1.0f / l;
    quat r = {q.x * k, q.y * k, q.z * k, q.w * k};
    return r;
}
static inline quat q_axis_angle(vec3 axis, float rad) {
    float s = sinf(rad * 0.5f);
    quat r = {axis.x * s, axis.y * s, axis.z * s, cosf(rad * 0.5f)};
    return r;
}
static inline vec3 q_rotate(quat q, vec3 v) {
    vec3 u = v3(q.x, q.y, q.z);
    vec3 t = v3_scale(v3_cross(u, v), 2.0f);
    return v3_add(v3_add(v, v3_scale(t, q.w)), v3_cross(u, t));
}
static inline vec3 q_rotate_inv(quat q, vec3 v) { return q_rotate(q_conj(q), v); }
// Integrates angular velocity w (rad/s, world space) over dt.
static inline quat q_integrate(quat q, vec3 w, float dt) {
    quat wq = {w.x, w.y, w.z, 0.0f};
    quat d = q_mul(wq, q);
    float h = 0.5f * dt;
    quat r = {q.x + d.x * h, q.y + d.y * h, q.z + d.z * h, q.w + d.w * h};
    return q_norm(r);
}
