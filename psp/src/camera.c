#include "camera.h"
#include "gfx.h"

#include <math.h>
#include <pspgu.h>
#include <pspgum.h>

#define DEG2RAD (3.14159265f / 180.0f)

void camera_init(Camera *c) {
    c->pos.x = 0.0f;
    c->pos.y = 0.0f;
    c->pos.z = 64.0f; // Source player eye height
    c->yaw = 0.0f;
    c->pitch = 0.0f;
    c->fov_y = 60.0f;
    c->znear = 4.0f;
    c->zfar = 6000.0f;
}

void camera_vectors(const Camera *c, ScePspFVector3 *forward, ScePspFVector3 *right) {
    float cy = cosf(c->yaw * DEG2RAD), sy = sinf(c->yaw * DEG2RAD);
    float cp = cosf(c->pitch * DEG2RAD), sp = sinf(c->pitch * DEG2RAD);
    if (forward) {
        forward->x = cp * cy;
        forward->y = cp * sy;
        forward->z = sp;
    }
    if (right) {
        right->x = sy;
        right->y = -cy;
        right->z = 0.0f;
    }
}

void camera_apply(const Camera *c) {
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(c->fov_y, (float)SCR_W / (float)SCR_H, c->znear, c->zfar);

    ScePspFVector3 fwd;
    camera_vectors(c, &fwd, 0);
    ScePspFVector3 eye = c->pos;
    ScePspFVector3 at = {eye.x + fwd.x, eye.y + fwd.y, eye.z + fwd.z};
    ScePspFVector3 up = {0.0f, 0.0f, 1.0f};

    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumLookAt(&eye, &at, &up);

    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
}
