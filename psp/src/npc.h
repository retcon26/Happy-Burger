#pragma once

#include "camera.h"
#include "collision.h"
#include "physics.h"
#include "player.h"

// NPCs: skinned, animated characters from data/npcs.pnpc with simple AI.
//   zombie / antlion: chase the player (and rebels, Combine and citizens) and hit them
//   citizen: wander about, run from hostile NPCs
//   combine: armed; shoot the player, rebels, zombies and antlions
//   rebel: armed; follow the player and shoot Combine, zombies and antlions
// They're killed by guns and thrown props, or removed by the tool gun.

#define MAX_NPCS 16
#define NPC_ID_BASE 1000 // CmBox ids for NPCs: NPC_ID_BASE + index

enum { AI_CITIZEN, AI_ZOMBIE, AI_ANTLION, AI_COMBINE, AI_REBEL };

int npcs_load(void);
int npc_type_count(void);
const char *npc_type_name(int type);
// Some characters are only ragdolls (no animations, not in the NPC list); some can be player models
int npc_type_ragdoll_only(int type);
int npc_type_player_model(int type);

int npc_spawn(int type, vec3 pos, float yaw); // returns the NPC index or -1
void npc_remove(int i);
void npcs_clear(void);
int npc_active(int i);
int npc_alive_count(void);

// Players the NPCs care about (the local player, plus everyone else in multiplayer)
typedef struct {
    vec3 pos;  // feet
    int valid; // 0: absent, dead or noclipping, so ignored
} NpcTarget;

// Advances AI and animation. damage is called when an NPC hits target number `target`.
void npcs_update(Map *m, float dt, const NpcTarget *targets, int num_targets, PhysWorld *phys,
                 void (*damage)(int target, float amount));
void npcs_draw(Map *m, const Camera *cam);
// The weapon NPC i holds, as drawn this frame by npcs_draw: its prop pack name ("_smg1"...) and world
// matrix. 0 if it holds nothing or wasn't drawn. game.c draws it from the prop pack.
int npc_weapon(int i, const char **model_name, ScePspFMatrix4 *world);
// Called for every shot an NPC fires (weapon: its prop name), for the game's sound and tracer
void npc_set_shot_handler(void (*shot)(const char *weapon, vec3 from, vec3 to));

// For the player's movement and the guns (other players' puppets get ids PUPPET_ID_BASE + slot)
#define PUPPET_ID_BASE 2000
int npc_boxes(CmBox *out, int max);

// Multiplayer: the host sends NPC states, clients mirror them and only animate
typedef struct {
    uint8_t active, type, seq, dead;
    float pos[3];
    float yaw;
} NpcNet;
void npc_net_get(int i, NpcNet *out);
void npc_net_set(int i, const NpcNet *in);
void npcs_animate(float dt);
// Other players, drawn as their player model (type; -1 or anything without animations: a citizen)
void npc_puppet_set(int slot, int active, int type, vec3 pos, float yaw, float dt);
// The local player's own body, for the third-person view: drawn, but not solid and not hit by shots
void npc_self_set(int active, int type, vec3 pos, float yaw, float dt);
int npc_raycast(vec3 origin, vec3 dir, float max_t, float *t_out);
void npc_damage(int i, float amount);
void npc_damage_dir(int i, float amount, vec3 dv); // dv: velocity kick for the ragdoll if it dies
// Options: NPC thinking off (they stand still) / NPCs ignore players (no targeting or following them)
void npc_set_ai(int disabled, int ignore_players);
// The spawnable prop (props pack model) that cuts zombies in half when it flies into them: the Sawblade
void npc_set_blade_model(int model);
// game.c: a blood spray (a prop hit an NPC)
void game_blood(vec3 at, vec3 dir);

// Ragdolls: dead NPCs fall as ragdolls; they can also be spawned, shot, dragged and frozen.
// Each PSP simulates its own (they aren't sent over the network).
#define MAX_RAGDOLLS 6
#define RAGDOLL_ID_BASE 3000 // undo ids
int npc_ragdolls_available(void); // the NPC pack has ragdoll skeletons
int npc_ragdoll_spawn(int type, vec3 pos, float yaw);
int npc_ragdoll_active(int r);
void npc_ragdoll_remove(int r);
void npc_ragdolls_update(Map *m, float dt, PhysWorld *phys);
int npc_ragdoll_raycast(vec3 origin, vec3 dir, float max_t, float *t_out, int *bone_out);
vec3 npc_ragdoll_point(int r, int bone);
void npc_ragdoll_push(int r, int bone, vec3 dv); // velocity change at one bone (bullets, crowbar)
void npc_ragdoll_drag(int r, int bone, vec3 target, float dt); // physics gun
void npc_ragdoll_freeze(int r, int frozen);
// Inflator: multiplies one bone's size by factor (within limits); returns the new size
float npc_ragdoll_inflate(int r, int bone, float factor);
void npc_ragdoll_debug(int *startsolid, int *hits, int *asleep);
