#pragma once

#include "input.h"
#include "physics.h"

// Saved builds (Pause menu > Saves), like GMod's saves and dupes: the props you spawned, their welds,
// ropes and the rest, thrusters, hoverballs, balloons, lamps, lights, buttons and emitters. They're
// text files in data/saves/ (on the memory stick, or the PSP Go's internal storage), and load on any
// map: where you're looking, turned the way you face, or where they were built when it's the same map.

#define BUILD_MAX_PROPS MAX_BODIES // every prop there can be (a full server's autosave)
#define BUILD_MAX_JOINTS MAX_JOINTS
#define BUILD_MAX_FORCES MAX_FORCES
#define BUILD_MAX_DEVICES 24
#define BUILD_NAME_LEN 24

enum { BUILD_FROZEN = 1, BUILD_NOCOLLIDE = 2 };

// Positions are in the build's own frame: from its origin (the middle of its floor), turned so the
// way the builder faced is +X. Joints and forces keep the physics engine's fields, with a and b (and
// body) as prop numbers in the build; world anchors (b = -1) are in the build's frame too.
typedef struct {
    char name[32];     // the prop pack's name for it
    char cat[24];      // its spawn menu category (names can repeat between games)
    vec3 pos;
    quat rot;
    unsigned int tint;
    float mass;
    int flags;         // BUILD_*
    int balloons;
} BuildProp;

typedef struct {
    int type, body, on;
    vec3 lpos, ldir;
    int colour, effect, target; // target: a button's prop, or -1
} BuildDevice;

typedef struct {
    char map[32];      // where it was built
    vec3 origin;       // there, in that map's coordinates
    float yaw;         // the way the builder faced
    int num_props, num_joints, num_forces, num_devices;
    BuildProp props[BUILD_MAX_PROPS];
    Joint joints[BUILD_MAX_JOINTS];
    Force forces[BUILD_MAX_FORCES];
    BuildDevice devices[BUILD_MAX_DEVICES];
} Build;

int build_write(const char *name, const Build *b); // 0 on success
int build_read(const char *name, Build *b);        // 0 on success

// --- The Saves page -----------------------------------------------------------------------------

enum { SAVES_NONE, SAVES_BACK, SAVES_SAVE, SAVES_LOAD_AIM, SAVES_LOAD_IN_PLACE };

// map: the one being played (Square loads a build in place only on its own map)
void saves_open(const char *map);
// Returns SAVES_*; for a save or a load, name gets the build's name. The page stays open after a save.
int saves_update(const InputState *in, char *name, int name_len);
void saves_frame(void); // between gfx_end_frame() and gfx_present(): the on-screen keyboard
void saves_draw(float x, float y, float w, float h);
void saves_refresh(void); // re-reads the list (after a save)
