#pragma once

#include "vec.h"

// Lua 5.4 scripting with a GMod-flavoured API (hook, timer, player, ents, npc, team, draw...; docs/LUA.md).
// When a map starts it runs the game's own library (psp/lua/init.lua, built into the EBOOT), the gamemode
// if there is one, then data/lua/autorun/*.lua and data/lua/maps/<map>.lua.
// A gamemode is data/lua/gamemodes/<name>.lua, or one of the game's own (psp/lua/gamemodes). Its rules
// run where server is set (single player, the host, a dedicated server); its HUD where client is (every PSP
// with a player).

// Returns 0 if mode names a gamemode that isn't there (the rest still runs)
int script_start(const char *map_name, const char *mode, int server, int client);
void script_stop(void);
void script_think(float dt);
// Use: "PlayerUse"(ply, ent) where the rules run (body -1: nothing in reach). Returns 1 if a script handled it.
int script_use(int slot, int body);
// "HUDPaint": scripts draw with draw.Text and the rest
void script_hud(void);

// The gamemode's hooks, called where the rules run
void script_player_join(int s);                    // PlayerInitialSpawn
void script_player_spawn(int s);                   // PlayerSpawn, then PlayerLoadout
void script_player_death(int s, int attacker);     // PlayerDeath (attacker: a slot, or -1)
void script_player_leave(int s);                   // PlayerDisconnected
int script_should_damage(int victim, int attacker); // PlayerShouldTakeDamage: 0 stops it
void script_entity_damage(int body, int attacker, float amount); // EntityTakeDamage: a player shot a prop
void script_npc_killed(int npc, int type, int attacker);         // OnNPCKilled
int script_select_spawn(int s, vec3 *pos, float *yaw);           // PlayerSelectSpawn: 0 for the map's own
int script_quick_chat(int s, const char *text);                  // PlayerQuickChat: 1 keeps it out of the chat
// The gamemode's networked variables (SetGlobalInt, ply:SetNWInt...): the host packs them for PK_GMSTATE,
// clients unpack them
int script_nw_pack(char *out, int max);
void script_nw_unpack(const char *in, int len);

// The gamemodes there are, for the menus: index 0 is Sandbox (id "sandbox"), then the game's own, then
// data/lua/gamemodes' (script_gamemodes_scan looks again)
void script_gamemodes_scan(void);
int script_gamemode_count(void);
const char *script_gamemode_id(int i);
const char *script_gamemode_title(int i);
int script_gamemode_find(const char *id); // its index, or -1
