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

// --- Gamemodes (script.c runs their Lua; the engine keeps what they decide about each player) ---
// The gamemode the next game_start runs: a file in data/lua/gamemodes (without .lua) or one of the
// game's own; "" or "sandbox" for none. game_gamemode is the running one ("" sandbox).
void game_set_gamemode(const char *name);
const char *game_gamemode(void);
// What it allows (GM.Sandbox, GM.AllowPickup, GM.TeamBased) and how soon X respawns the dead
// (GM.RespawnTime; < 0: only when the gamemode says). script.c sets them once the gamemode has loaded.
enum { GMF_SANDBOX = 1, GMF_PICKUP = 2, GMF_TEAM_TAGS = 4 };
void game_gm_setup(int flags, float respawn_time);
int game_is_server(void);    // this PSP runs the rules: single player or the host
int game_is_dedicated(void); // ...with no player of its own
// Players by slot, 0 .. NET_MAX_PLAYERS - 1 (the setters only work where the rules run)
enum { GPF_FROZEN = 1, GPF_BLIND = 2, GPF_NOTARGET = 4 }; // can't move / sees nothing / NPCs leave them be
int game_pl_valid(int s);
int game_pl_local(void);   // this PSP's slot, -1 on a dedicated server
int game_pl_serial(int s); // new each time someone takes the slot
const char *game_pl_name(int s);
float game_pl_health(int s);
void game_pl_set_health(int s, float hp);
float game_pl_max_health(int s);
void game_pl_set_max_health(int s, float hp);
vec3 game_pl_pos(int s);
void game_pl_view(int s, vec3 *eye, vec3 *dir, float *yaw, float *pitch);
int game_pl_team(int s);
void game_pl_set_team(int s, int team);
void game_pl_spawn(int s);                           // alive again at a spawn point (GM:PlayerSpawn)
void game_pl_set_pos(int s, vec3 pos, float yaw);    // moved there as they are
void game_pl_kill(int s, int silent);                // silent: no PlayerDeath (spectating from the start)
int game_pl_flags(int s);
void game_pl_set_flag(int s, int flag, int on);
float game_pl_speed(int s);                          // walking speed, 1 = usual
void game_pl_set_speed(int s, float k);
int game_pl_model(int s);                            // forced player model (NPC type), -1: their own
void game_pl_set_model(int s, int type);
int game_pl_disguise(int s, int body);               // look like that prop (0 if it isn't one)
void game_pl_undisguise(int s);
const char *game_pl_disguise_name(int s);            // NULL when not disguised
int game_pl_weapon(int s);                           // the weapon in hand (Weapon index)
unsigned int game_pl_weapons(int s);                 // what they may carry: a bit per weapon
void game_pl_set_weapons(int s, unsigned int mask);  // hands always stay
void game_pl_select(int s, int weapon);
void game_pl_give_ammo(int s, int weapon, int amount); // weapon -1: a full refill of everything they carry
int game_weapon_count(void);
int game_weapon_find(const char *id);   // "smg", "crowbar"... or -1
const char *game_weapon_id(int w);
// To one player's PSP, or s = -1 everyone's: a chat line / text across the middle of the screen, and a
// sound (at a place, or at = NULL: heard everywhere)
enum { GM_MSG_CHAT, GM_MSG_CENTER };
void game_gm_message(int s, int kind, const char *text);
void game_gm_sound(int s, const char *name, const vec3 *at);
// Props: a number that's new each time the slot gets a prop, its spawn menu name, its box, mass
int game_body_serial(int i);
const char *game_body_model_name(int i);
void game_body_bounds(int i, vec3 *mins, vec3 *maxs);
// NPCs: spawned by type name, hurt by a player (attacker slot, -1 the world)
int game_npc_spawn(const char *type, vec3 pos, float yaw);
void game_npc_damage(int i, float amount, int attacker);
// The map: its spawn points, a trace (returns the fraction; *body the prop it hit or -1, *slot a player or
// -1; it goes through player skip), and putting everything back as it loaded (spawned props, NPCs and
// ragdolls gone, its own props back)
int game_spawn_count(void);
vec3 game_spawn_point(int i, float *yaw);
float game_trace(vec3 a, vec3 b, int skip, vec3 *hit, int *body, int *slot);
void game_cleanup_map(void);
int game_button_down(const char *name); // this PSP's buttons ("select", "l", "r"...), for HUDs
void game_achievement(const char *id);  // a gamemode's achievement for this PSP's player
GameResult game_update(const InputState *in, float dt);
void game_render(float fps);
void game_end(void);
