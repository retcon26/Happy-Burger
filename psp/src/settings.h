#pragma once

#include "input.h"

// Player options, saved to data/settings.cfg.
typedef struct {
    int sound;      // 0/1
    int look_speed; // 1..5
    int invert_y;   // 0/1
    int show_fps;   // 0/1
    int debug;      // 0/1: debug overlay
    int crosshair;  // 0/1
    int vu1;        // 0/1: PS2 map geometry on the VU1 coprocessor
    int show_hud;   // 0/1: health, ammo, notes and hints
    int draw_dist;  // 1..4: near, medium, far, the map's full distance
    int physics;    // 1..3: low, medium, high
    int god_mode;   // 0/1: no damage in single player; when hosting, for everyone in the game
    int player_model; // which of the player models (settings_set_player_models) you look like
    int third_person; // 0/1: the camera follows behind you, so you see your player model
    int npc_think;    // 0/1: NPCs think and move (off: they stand still, like GMod's ai_disabled)
    int npc_ignore_players; // 0/1: NPCs don't target or follow players (ai_ignoreplayers)
    int unlimited_ammo; // 0/1: guns never run out or need reloading
    int hev_suit;       // 0/1: you wear the HEV suit: 100 suit power at spawn soaks up most damage
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
// Options > Player model offers these, named by name(0..count-1); the game registers them once the
// NPC pack has loaded
void settings_set_player_models(int count, const char *(*name)(int index));

// The Options page, shared by the main menu and the in-game menu.
// Returns 1 when the player backs out (settings are saved then).
int options_update(const InputState *in);
void options_draw(float x, float y, float w, float h);