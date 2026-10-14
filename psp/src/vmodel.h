#pragma once

#include <psptypes.h>

#include "proplib.h"

// Animated first-person weapons (data/vmodels.pvmd, from tools/bspconv/vmbuild.py): GMod's c_ weapon
// models with the player's arms, rigidly skinned, playing their draw / idle / fire / reload animations.
// The game falls back to the static c_ models in the prop pack when this pack is missing.

enum { VM_IDLE, VM_DRAW, VM_FIRE, VM_ALT, VM_RELOAD, VM_RELOAD_START, VM_RELOAD_END, VM_PUMP, VM_KINDS };

int vmodels_load(void);              // 0 on success
int vmodel_find(const char *name);   // by its prop pack label ("_vm_pistol"), or -1
int vmodel_has(int vm, int kind);
float vmodel_length(int vm, int kind); // seconds, 0 if it doesn't have it

// Plays an animation on the weapon in hand (a different vm also starts over). One-shot animations go
// back to idle when they end; loop keeps repeating it (the physics gun's hold).
void vmodel_play(int vm, int kind, int loop);
int vmodel_playing(void); // the kind playing now
void vmodel_update(float dt);

// Draws the weapon in hand, posed for this frame, with the GE's model matrix set to world.
void vmodel_draw(int vm, const ScePspFMatrix4 *world, const PropLight *light);
