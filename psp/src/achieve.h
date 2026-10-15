#pragma once

#include "input.h"

// Achievements, saved to data/achievements.cfg: playtime, playing with other people, playing on a
// real PSP (by model), and a few things done in game.

enum {
    ACH_PLAY_10M,     // Getting Started: play for 10 minutes
    ACH_PLAY_1H,      // Regular: an hour
    ACH_PLAY_10H,     // Addict: ten hours
    ACH_NOT_ALONE,    // play a game with at least one other real person
    ACH_FULL_HOUSE,   // a game with all four players
    ACH_REAL_PSP,     // play on a real PSP (not an emulator)
    ACH_PSP1000,      // ...a PSP-1000
    ACH_PSPSLIM,      // ...a PSP-2000 or 3000
    ACH_PSPGO,        // ...a PSP Go
    ACH_CREATOR,      // spawn 100 props
    ACH_MINGE_HUNTER, // kill 10 mingebags
    ACH_EXPLORER,     // play on 10 different maps
    // 1.7.0 (new ones go on the end: progress files keep a bit per achievement)
    ACH_SAVE_POINT,   // save a build
    ACH_DEJA_VU,      // load a build on another map than it was built on
    ACH_ONLINE,       // join a dedicated server
    ACH_ON_THE_AIR,   // host a game over Wi-Fi that someone joins
    ACH_COUNT
};

void achieve_init(void); // loads progress and works out what it's running on
void achieve_save(void);
// In game, every frame: playtime, and how many players are in the game (1 when alone)
void achieve_update(float dt, int players);
void achieve_prop_spawned(void);
void achieve_minge_killed(void);
void achieve_map(const char *name);
void achieve_unlock(int a); // the ones that are done or not (ACH_SAVE_POINT...)
// The next achievement unlocked since the last call (for the game's notification), or -1
int achieve_pop(void);
const char *achieve_name(int a);
const char *achieve_device(void); // "PSP-2000 / 3000", "PPSSPP (emulator)"...

// Main menu > Achievements. Returns 1 when the player backs out.
int achievements_update(const InputState *in);
void achievements_draw(float x, float y, float w, float h);
