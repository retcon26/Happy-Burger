#pragma once

#include "input.h"
#include "map.h"
#include "player.h"

typedef enum {
    GAME_CONTINUE,
    GAME_EXIT_TO_MENU,
    GAME_QUIT,
} GameResult;

// Loads the packs every map uses (props, NPCs, sounds' names). Optional: game_start does it too, but
// doing it once at startup keeps them out of the way of the big per-map allocations.
void game_preload(void);
// Rebuilds Options > Player model (after Main menu > Workshop switches characters on or off)
void game_refresh_player_models(void);
int game_start(const char *map_name, char *err, int err_len);
// Multiplayer, right after game_start: host this game over ad-hoc, or join one (from the lobby)
void game_host(void);
void game_join(int slot, const unsigned char host_mac[6]);
const char *game_exit_message(void); // why a multiplayer game ended, or ""
void game_set_view(float x, float y, float z, float yaw, float pitch);
// Dev/test script command, e.g. "prop 3 100 200 0 45", "debug", "menu 0", "tool 2", "noclip".
void game_command(const char *line);

// For the Lua layer (script.c)
#include "physics.h"
int game_spawn_prop_by_name(const char *name, float x, float y, float z, float yaw);
Body *game_body(int i);
void game_remove_body(int i);
void game_player_pos(float *x, float *y, float *z);
float game_player_yaw(void);
void game_message(const char *msg);
int game_aim_body(void);
GameResult game_update(const InputState *in, float dt);
void game_render(float fps);
void game_end(void);
