#pragma once

#include "collision.h"
#include "ppak_format.h"
#include "vec.h"

// Small rigid-body engine for props. Shapes are boxes, spheres and cylinders, all handled as
// clouds of surface sample points. It does sequential-impulse contacts against the world brushes
// and each other, friction, welds and ropes, buoyancy in water, sleeping and freezing.
// It aims at GMod-ish fun on a 333MHz CPU, not accuracy.

#define MAX_BODIES 224 // map props plus up to PLAYER_PROP_LIMIT (game.c) spawned ones
#define MAX_JOINTS 48
#define MAX_SAMPLE_POINTS 32

enum { SHAPE_BOX, SHAPE_SPHERE, SHAPE_CYLINDER, SHAPE_HULL };

// A convex-hull collision shape (pointers into a prop pack, model space). Several pieces form a union.
typedef struct {
    const PpakHullPiece *pieces;
    int num_pieces;
    const PmapPlane *planes;
    const float (*points)[3];
    int num_points;
} HullShape;

typedef struct {
    int active;
    int lib;     // 0: spawnable props pack, 1: the map's own prop pack
    int model;   // index into that prop library
    int shape;   // SHAPE_*
    int surface; // SURF_* (impact sounds)
    vec3 pos;    // shape centre, world space
    quat rot;
    vec3 vel;    // units/s
    vec3 angvel; // rad/s, world space
    vec3 half;   // box half-extents; sphere uses half.x as radius; cylinder: half.x radius, half.z half-height
    vec3 offset; // shape centre in model space (the model is drawn at pos - rot * offset)
    HullShape hull; // SHAPE_HULL only
    float mass;
    float inv_mass;
    vec3 inv_inertia; // local-space diagonal
    float buoyancy;   // water density / body density, clamped
    int frozen;       // physgun-frozen: immovable until grabbed again
    int asleep;
    float idle_time;
    int held;         // driven by the physics gun this frame
    float lift;       // upward acceleration from balloons (units/s^2)
    int balloons;
    float impact;     // strongest impact speed since the game last read it
    int in_water;
    int nocollide;    // passes through other props (still hits the world)
    int powered;      // a thruster or hoverball is working on it this step (keeps it awake)
    float life;       // seconds until it's removed (broken pieces); 0 = it stays
    unsigned int tint_abgr;
    // Lighting cache
    unsigned int ambient_abgr;
    int sunlit;
    int light_age;
} Body;

// Weld: fixed together. Rope: can't move further apart. Ball socket: share a point but turn freely.
// Elastic: a spring that pulls back towards its rest length. Slider: a's anchor slides along a line
// through b's (and can't turn relative to b), like GMod's Slider.
// Axis (the Wheel tool): share a point and turn only about one axis, with a motor Use switches.
// Rod (the Hydraulic tool): a fixed distance apart, which Use moves between two lengths.
// A rope with a winch (rate > 0) reels in or lets out when Use switches it.
enum { JOINT_WELD, JOINT_ROPE, JOINT_BALL, JOINT_ELASTIC, JOINT_SLIDER, JOINT_AXIS, JOINT_ROD };
enum { WINCH_STOP, WINCH_IN, WINCH_OUT };

typedef struct {
    int active;
    int type;
    int a, b;      // body indices; b = -1 means the world
    vec3 la, lb;   // anchors in each body's local space (lb is a world point when b = -1)
    quat rel;      // weld, slider: a's orientation relative to b's, at creation
    float length;  // rope: maximum length; elastic: rest length; rod: its length now
    vec3 axis;     // slider: the line's direction, in b's local space (world when b = -1); axis: b's axle
    vec3 axis_a;   // axis: a's axle, in a's local space
    int on;        // axis: motor running; rod: extended; rope: WINCH_* (winches only)
    float motor;   // axis: the motor's speed (rad/s, + turns a forward about its axle)
    float rate;    // rod, winch: how fast the length changes (u/s); 0 for a plain rope
    float len_min, len_max; // rod, winch: the lengths it moves between
} Joint;

// Things attached to a prop that push it: a thruster pushes along its direction (in the prop's own frame),
// a hoverball holds the prop at a height. Use toggles them.
enum { FORCE_THRUSTER, FORCE_HOVER };
#define MAX_FORCES 32

typedef struct {
    int active, type, body, on;
    vec3 lpos, ldir; // in the body's local space
    float strength;  // thruster: force (kg u/s^2); hover: the height it holds
} Force;

typedef struct {
    Body bodies[MAX_BODIES];
    Joint joints[MAX_JOINTS];
    Force forces[MAX_FORCES];
    float accum; // fixed-step time accumulator
    int contacts_last_step;
} PhysWorld;

void phys_init(PhysWorld *w);
// Returns the body index, or -1 if the world is full.
int phys_spawn(PhysWorld *w, int model, int shape, vec3 model_mins, vec3 model_maxs, float mass, vec3 pos, quat rot);
// Same, into a particular free slot (multiplayer clients mirror the host's body indices); -1 if taken
int phys_spawn_at(PhysWorld *w, int slot, int model, int shape, vec3 model_mins, vec3 model_maxs, float mass, vec3 pos,
                  quat rot);
void phys_remove(PhysWorld *w, int i);
void phys_wake(PhysWorld *w, int i);
void phys_step(PhysWorld *w, Map *m, float dt);
// 1 low, 2 medium, 3 high (default): fewer solver passes and catch-up steps are cheaper but softer
void phys_set_quality(int q);
// Options > Target FPS: under 60 a frame needs more than one fixed step, so it may catch up that many more
void phys_set_frame_rate(int fps);
// Called at the start of every fixed step, before gravity and contacts (vehicles: suspension and engines)
void phys_set_step_hook(void (*hook)(PhysWorld *w, Map *m, float dt));
// An impulse (kg u/s) at a world point, and how fast that point of the body is moving
void phys_apply_impulse(Body *b, vec3 world_point, vec3 impulse);
vec3 phys_point_velocity(const Body *b, vec3 world_point);

// Joints. b = -1 attaches to the world. Return the joint index or -1.
int phys_weld(PhysWorld *w, int a, int b, vec3 world_anchor);
int phys_rope(PhysWorld *w, int a, vec3 world_a, int b, vec3 world_b, float length);
int phys_ballsocket(PhysWorld *w, int a, int b, vec3 world_anchor);
int phys_elastic(PhysWorld *w, int a, vec3 world_a, int b, vec3 world_b, float rest_length);
// The prop(s) can only slide along the line through the two points, keeping their relative angle
int phys_slider(PhysWorld *w, int a, vec3 world_a, int b, vec3 world_b);

// Wheel: a turns about world_axis through world_anchor, relative to b; its motor (off at first) spins at
// motor rad/s. Hydraulic: a rod between the points, Use moves it between their distance and
// stretch times it. Winch: a rope that reels in and lets out at rate u/s.
int phys_axis(PhysWorld *w, int a, int b, vec3 world_anchor, vec3 world_axis, float motor);
int phys_hydraulic(PhysWorld *w, int a, vec3 world_a, int b, vec3 world_b, float stretch, float rate);
int phys_winch(PhysWorld *w, int a, vec3 world_a, int b, vec3 world_b, float rate);
// Switches the wheel motors, hydraulics and winches on a body (Use); returns how many there are
int phys_toggle_joints(PhysWorld *w, int body);
int phys_toggle_joint(PhysWorld *w, int j); // one joint; 1 if it was one Use switches
// Thrusters and hoverballs. Returns the force index or -1 when full.

int phys_add_force(PhysWorld *w, int type, int body, vec3 world_pos, vec3 world_dir, float strength);
// Switches every thruster and hoverball on a body; returns how many it has (0: none)
int phys_toggle_forces(PhysWorld *w, int body);
// A new mass for a prop (the Weight tool), keeping its shape's inertia in proportion
void phys_set_mass(PhysWorld *w, int i, float mass);
// Shoves everything within radius away from centre, strongest at the middle (explosions)
void phys_blast(PhysWorld *w, vec3 centre, float radius, float speed);
void phys_joint_anchors(const PhysWorld *w, const Joint *j, vec3 *pa, vec3 *pb);

// Ray against all bodies. Returns the nearest body hit within max_t.
int phys_raycast(const PhysWorld *w, vec3 origin, vec3 dir, float max_t, float *t_out);
// World-space AABB of a body (for the player's collision and culling).
void phys_body_aabb(const Body *b, vec3 *mins, vec3 *maxs);
void phys_body_matrix(const Body *b, ScePspFMatrix4 *m);
// The body's real (rotated) collision shape, for player, NPC and ragdoll traces.
void phys_body_cmbox(const Body *b, CmBox *out, int id);
