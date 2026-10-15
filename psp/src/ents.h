#pragma once

#include "camera.h"
#include "map.h"
#include "vec.h"

// Runtime side of brush entities: doors (sliding and swinging), buttons, spinning fans and
// triggers, linked by Source's I/O outputs ("button OnPressed -> door Open").

enum { ENT_CLOSED, ENT_OPENING, ENT_OPEN, ENT_CLOSING };

typedef struct EntState {
    int state;
    float progress;  // 0 closed .. 1 open
    float open_sign; // swinging doors: +1/-1, picked so the door swings away from the player
    float timer;     // seconds until an open door/button returns
    float angle;     // func_rotating: current angle (degrees)
    float spin;      // func_rotating: current speed (degrees/s)
    int on;          // func_rotating: motor on
    int inside;      // triggers: the player was inside last frame
    int done;        // trigger_once has fired
    vec3 pos;        // world position of the entity's local origin
    quat rot;
    int rotated;     // rot isn't identity
    vec3 wmin, wmax; // world bounds
} EntState;

int ents_init(Map *m);
// Moves doors, runs timers and outputs, fires triggers touched by the players' boxes (n of them:
// everyone in the game, or none on a dedicated server with nobody on it).
void ents_update(Map *m, float dt, const vec3 *player_mins, const vec3 *player_maxs, int n);
// "Use" along a ray from the eye: presses buttons and opens doors within reach. Returns 1 if something reacted.
int ents_use(Map *m, vec3 eye, vec3 dir);
// Multiplayer: the host packs door/button/fan/light states (4 bytes each), clients apply them and
// run ents_client_update instead of ents_update (motion and light animation, no game logic).
int ents_net_pack(const Map *m, int first, int count, unsigned char *out);
void ents_net_unpack(Map *m, int first, int count, const unsigned char *in);
void ents_client_update(Map *m, float dt);
// Draws moving entities. Call from map_draw with world render state set.
void ents_draw(const Map *m, const Camera *cam, int *tris);
