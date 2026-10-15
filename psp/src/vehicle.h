#pragma once

#include "physics.h"

// Drivable vehicles and seats, like GMod's: HL2's Jeep and Airboat, and seats to sit on your own
// contraptions. They're ordinary props (bodies) whose model is one of these; the Jeep rides on four
// raycast wheels with springs, the Airboat on four floats that also sit on water, pushed by its fan.

enum { VEH_NONE, VEH_JEEP, VEH_AIRBOAT, VEH_SEAT };

// Which spawnable props (lib 0 models) are which, by the game when the prop pack is loaded;
// seat_eye: per seat model, how high the sitter's eyes are above the model's origin
void vehicles_set_models(int jeep, int airboat, const int *seats, const float *seat_eye, int num_seats);
// Installs the physics step hook
void vehicles_init(void);

int vehicle_kind(const Body *b);
// The driver's eye point (world) and the way the vehicle faces (degrees, like the player's yaw)
void vehicle_seat(const Body *b, vec3 *eye, float *yaw);
// Exit points to try, in the body's frame (model space), best first; returns how many
int vehicle_exits(const Body *b, vec3 *out, int max);

// Someone drives body i this frame: throttle and steer -1..1 (steer > 0: right), handbrake 0/1.
// Without it a vehicle just sits on its wheels or floats.
void vehicle_drive(int i, float throttle, float steer, int handbrake);
// After the physics step: forget this frame's inputs
void vehicles_end_frame(void);
