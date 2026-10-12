#pragma once

#include "collision.h"
#include "vec.h"

// Source-like player movement: walk, jump, crouch, step up stairs, slide along walls, and noclip.
typedef struct {
    vec3 origin;   // feet
    vec3 velocity; // units/s
    int on_ground;
    vec3 ground_normal;
    int ducked;
    int noclip;
    float view_height; // smoothed eye height above origin
    int water_level;   // 0 dry, 1 feet, 2 waist (swimming), 3 head under
} Player;

typedef struct {
    float forward, side; // -1..1
    float yaw, pitch;    // degrees, view direction
    int jump;            // held
    int duck;            // held
    int fast;            // noclip speed boost
    float walk_speed;    // NPCs: on the ground, move at exactly this (units/s) along yaw, with no
                         // acceleration or friction (which hold anything under ~80 u/s to a crawl)
} MoveCmd;

void player_spawn(Player *p, vec3 feet);
// Moves the player one step. Props are passed as boxes so the player bumps into them.
void player_move(Player *p, Map *m, const MoveCmd *cmd, const CmBox *props, int num_props, float dt);
vec3 player_eye(const Player *p);
void player_hull(const Player *p, vec3 *mins, vec3 *maxs);
