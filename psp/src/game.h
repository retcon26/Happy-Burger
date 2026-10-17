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
// A dedicated server (retcons-mod-server): hosts with no player of its own; log gets the notes
void game_host_dedicated(void (*log)(const char *msg));
// Its rules: up to max_players (1 to NET_MAX_PLAYERS - 1) and a password to join ("" or NULL: none)
void game_set_server(int max_players, const char *password);
// Its console: who's in each slot (NULL: nobody), sending a player away, a chat line for everyone
enum { GAME_LEAVE_ENDED, GAME_LEAVE_KICKED, GAME_LEAVE_MAP }; // why, as the player is told
const char *game_peer_name(int slot);
void game_kick(int slot, int reason);
void game_chat(const char *text);
void game_join(int slot, const unsigned char host_mac[6]);
const char *game_exit_message(void); // why a multiplayer game ended, or ""
// After a game ended because its server changed map: the server's address ("1.2.3.4:18256") to join
// again, once. 0 if it ended any other way.
int game_rejoin_address(char *out, int len);
void game_set_view(float x, float y, float z, float yaw, float pitch);
// Pause > Screenshot saves the screen with this (the PSP's shot_save; without one the item just resumes)
void game_set_screenshot(int (*save)(const char *name, char *out, int out_len));
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
