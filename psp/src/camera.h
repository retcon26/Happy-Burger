#pragma once

#include <psptypes.h>

// World space matches Source maps: Z is up, units are Source units (inches).
// Yaw 0 looks down +X and increases toward +Y, as in Source.
typedef struct {
    ScePspFVector3 pos;
    float yaw;   // degrees
    float pitch; // degrees, + looks up
    float fov_y; // degrees
    float znear, zfar;
} Camera;

void camera_init(Camera *c);
void camera_vectors(const Camera *c, ScePspFVector3 *forward, ScePspFVector3 *right);
// Loads the projection and view matrices and resets the model matrix to identity.
void camera_apply(const Camera *c);
