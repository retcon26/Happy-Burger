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
    int target_fps; // 0..3: 25, 30, 60, 120 frames a second (settings_target_fps)
    int lod;        // 1..5: how far models are drawn, 0.5x to 2x (settings_lod_scale)
    int god_mode;   // 0/1: no damage in single player; when hosting, for everyone in the game
    int player_model; // which of the player models (settings_set_player_models) you look like
    int third_person; // 0/1: the camera follows behind you, so you see your player model
    int npc_think;    // 0/1: NPCs think and move (off: they stand still, like GMod's ai_disabled)
    int npc_ignore_players; // 0/1: NPCs don't target or follow players (ai_ignoreplayers)
    int unlimited_ammo; // 0/1: guns never run out or need reloading
    int hev_suit;       // 0/1: you wear the HEV suit: 100 suit power at spawn soaks up most damage
    int spawn_weapons;  // 0/1: spawn with every weapon (off: the toolbox; guns from the spawn menu's Weapons)
    // Multiplayer > Profile: how other players see you
    int player_colour;  // profile_colour index: tints your player model and name (0: none)
    int physgun_colour; // profile_colour index for your physics gun's beam (0: GMod's blue)
    char name[16];      // your name in games ("": the PSP's nickname)
    char gamemode[16];  // New Game's gamemode ("": Sandbox; Happy Burger by default)
    char difficulty[16]; // Start Shift's last pick: "tutorial", "easy", "medium", "hard" or "extreme"
} Settings;

#define PROFILE_COLOURS 11
// ABGR; index 0 is "none" (white) for a player colour, GMod's blue for a physics gun
unsigned int profile_colour(int i, int physgun);
const char *profile_colour_name(int i, int physgun);

// Multiplayer > Profile: name, player model and colours. Returns 1 when the player backs out (saved
// then). profile_frame() goes between gfx_end_frame() and gfx_present() (the on-screen keyboard).
int profile_update(const InputState *in);
void profile_draw(float x, float y, float w, float h);
void profile_frame(void);

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
// Options > Target FPS in frames a second
int settings_target_fps(void);
// Options > LOD distance: how far props, static props and NPCs are drawn, as a multiple of the usual
// distance (0.5 to 2; the draw distance still caps it)
float settings_lod_scale(void);
// Options > Player model offers these, named by name(0..count-1); the game registers them once the
// NPC pack has loaded
void settings_set_player_models(int count, const char *(*name)(int index));

// The Options page, shared by the main menu and the in-game menu.
// Returns 1 when the player backs out (settings are saved then).
int options_update(const InputState *in);
void options_draw(float x, float y, float w, float h);

// Options > Updates: the platform's update page, drawn in the Options window (the PSP's update.c registers
// it; without one the row does nothing). update returns 1 when the player backs out of it.
void settings_set_updates_page(void (*open)(void), int (*update)(const InputState *in),
                               void (*draw)(float x, float y, float w, float h));
void options_open_updates(void); // straight to that page (autoload.txt "menu updates")