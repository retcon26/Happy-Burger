#include "script.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "game.h"
#include "nav.h"
#include "net.h"
#include "npc.h"
#include "ui.h"
#include "workshop.h"

// The game's own Lua (psp/lua, made into lua_builtin.h by tools/embed_lua.py when the EBOOT is built):
// init.lua, the library every script gets, and the gamemodes that come with the game. Built in, so an
// update of just the EBOOT (Options > Updates) brings them too.
#include "lua_builtin.h"

#define LUA_LOG "data/lua_errors.txt"
#define MAX_GAMEMODES 24

static lua_State *L;
static float g_time, g_frame;
static int g_server, g_client;
static char g_last_error[160];

// --- Errors: on the screen and in data/lua_errors.txt (the same one only once in a row) -------------

static void report(const char *msg) {
    if (!msg || strcmp(msg, g_last_error) == 0) return;
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
    game_message(msg);
    FILE *f = fopen(LUA_LOG, "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static void report_top(void) {
    report(lua_tostring(L, -1));
    lua_pop(L, 1);
}

// --- The game's own files and data/lua's -------------------------------------------------------------

static const char *builtin(const char *path) {
    for (int i = 0; i < (int)(sizeof(k_lua_builtin) / sizeof(k_lua_builtin[0])); i++) {
        if (strcmp(k_lua_builtin[i].path, path) == 0) return k_lua_builtin[i].src;
    }
    return NULL;
}

static int data_file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

// Loads data/lua/<path> if there's one (yours win), else the game's own; pushes the chunk. 0: neither.
static int load_lua(lua_State *s, const char *path) {
    char file[160];
    snprintf(file, sizeof(file), "data/lua/%s", path);
    int r;
    if (data_file_exists(file)) {
        r = luaL_loadfile(s, file);
    } else {
        const char *src = builtin(path);
        if (!src) return 0;
        char name[96];
        snprintf(name, sizeof(name), "@psp/lua/%s", path);
        r = luaL_loadbuffer(s, src, strlen(src), name);
    }
    if (r != LUA_OK) {
        report(lua_tostring(s, -1));
        lua_pop(s, 1);
        lua_pushnil(s); // a chunk that does nothing
        return -1;
    }
    return 1;
}

// Runs a file (load_lua's search); 0 if there's no such file
static int run_lua(const char *path) {
    int r = load_lua(L, path);
    if (r <= 0) {
        if (r < 0) lua_pop(L, 1);
        return r < 0;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) report_top();
    return 1;
}

static void run_file(const char *path) {
    if (luaL_dofile(L, path) != LUA_OK) report_top();
}

// The memory stick hands back names that fit 8.3 in capitals ("EXAMPLE.LUA"), as PPSSPP does: in lower
// case they match what's written in scripts and data/workshop.cfg
static void lower_name(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    }
}

// --- Basics: print, time, include --------------------------------------------------------------------

static int l_print(lua_State *s) {
    int n = lua_gettop(s); // before the buffer, which can take a stack slot of its own
    luaL_Buffer b;
    luaL_buffinit(s, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, ' ');
        luaL_tolstring(s, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    game_message(lua_tostring(s, -1));
    return 0;
}

static int l_curtime(lua_State *s) {
    lua_pushnumber(s, g_time);
    return 1;
}

static int l_frametime(lua_State *s) {
    lua_pushnumber(s, g_frame);
    return 1;
}

// include("gamemodes/base.lua"): runs that file (data/lua's, or the game's own) and returns what it returns
static int l_include(lua_State *s) {
    const char *path = luaL_checkstring(s, 1);
    int top = lua_gettop(s);
    int r = load_lua(s, path);
    if (r == 0) return luaL_error(s, "include: no file lua/%s", path);
    if (r < 0) return 0;
    lua_call(s, 0, LUA_MULTRET);
    return lua_gettop(s) - top;
}

static int l_error(lua_State *s) {
    report(luaL_checkstring(s, 1));
    return 0;
}

// --- Primitives for init.lua: _E.* ------------------------------------------------------------------
// Players by slot, props by body index, NPCs by index. init.lua wraps them in GMod-style objects.

static int slot_arg(lua_State *s, int i) {
    return (int)luaL_checkinteger(s, i);
}

static vec3 vec_args(lua_State *s, int i) {
    return v3((float)luaL_checknumber(s, i), (float)luaL_checknumber(s, i + 1), (float)luaL_checknumber(s, i + 2));
}

static int push_vec(lua_State *s, vec3 v) {
    lua_pushnumber(s, v.x);
    lua_pushnumber(s, v.y);
    lua_pushnumber(s, v.z);
    return 3;
}

static int e_pl_valid(lua_State *s) {
    lua_pushboolean(s, game_pl_valid(slot_arg(s, 1)));
    return 1;
}

static int e_pl_local(lua_State *s) {
    int slot = game_pl_local();
    if (slot < 0) {
        lua_pushnil(s);
    } else {
        lua_pushinteger(s, slot);
    }
    return 1;
}

static int e_pl_serial(lua_State *s) {
    lua_pushinteger(s, game_pl_serial(slot_arg(s, 1)));
    return 1;
}

static int e_pl_name(lua_State *s) {
    lua_pushstring(s, game_pl_name(slot_arg(s, 1)));
    return 1;
}

static int e_pl_health(lua_State *s) {
    lua_pushnumber(s, game_pl_health(slot_arg(s, 1)));
    return 1;
}

static int e_pl_set_health(lua_State *s) {
    game_pl_set_health(slot_arg(s, 1), (float)luaL_checknumber(s, 2));
    return 0;
}

static int e_pl_max_health(lua_State *s) {
    lua_pushnumber(s, game_pl_max_health(slot_arg(s, 1)));
    return 1;
}

static int e_pl_set_max_health(lua_State *s) {
    game_pl_set_max_health(slot_arg(s, 1), (float)luaL_checknumber(s, 2));
    return 0;
}

static int e_pl_pos(lua_State *s) {
    return push_vec(s, game_pl_pos(slot_arg(s, 1)));
}

// eye x y z, aim x y z, yaw, pitch
static int e_pl_view(lua_State *s) {
    vec3 eye, dir;
    float yaw, pitch;
    game_pl_view(slot_arg(s, 1), &eye, &dir, &yaw, &pitch);
    push_vec(s, eye);
    push_vec(s, dir);
    lua_pushnumber(s, yaw);
    lua_pushnumber(s, pitch);
    return 8;
}

static int e_pl_team(lua_State *s) {
    lua_pushinteger(s, game_pl_team(slot_arg(s, 1)));
    return 1;
}

static int e_pl_set_team(lua_State *s) {
    game_pl_set_team(slot_arg(s, 1), (int)luaL_checkinteger(s, 2));
    return 0;
}

static int e_pl_spawn(lua_State *s) {
    game_pl_spawn(slot_arg(s, 1));
    return 0;
}

static int e_pl_set_pos(lua_State *s) {
    game_pl_set_pos(slot_arg(s, 1), vec_args(s, 2), (float)luaL_optnumber(s, 5, 0.0));
    return 0;
}

static int e_pl_kill(lua_State *s) {
    game_pl_kill(slot_arg(s, 1), lua_toboolean(s, 2));
    return 0;
}

static int flag_arg(lua_State *s, int i) {
    static const char *const names[] = {"frozen", "blind", "notarget", NULL};
    static const int flags[] = {GPF_FROZEN, GPF_BLIND, GPF_NOTARGET};
    return flags[luaL_checkoption(s, i, NULL, names)];
}

static int e_pl_flag(lua_State *s) {
    lua_pushboolean(s, (game_pl_flags(slot_arg(s, 1)) & flag_arg(s, 2)) != 0);
    return 1;
}

static int e_pl_set_flag(lua_State *s) {
    game_pl_set_flag(slot_arg(s, 1), flag_arg(s, 2), lua_toboolean(s, 3));
    return 0;
}

static int e_pl_speed(lua_State *s) {
    lua_pushnumber(s, game_pl_speed(slot_arg(s, 1)));
    return 1;
}

static int e_pl_set_speed(lua_State *s) {
    game_pl_set_speed(slot_arg(s, 1), (float)luaL_checknumber(s, 2));
    return 0;
}

static int e_pl_model(lua_State *s) {
    int t = game_pl_model(slot_arg(s, 1));
    if (t < 0) {
        lua_pushnil(s);
    } else {
        lua_pushstring(s, npc_type_name(t));
    }
    return 1;
}

// A character by its NPC name ("Zombie"), or nil / "" for the player's own; false if there's no such one
static int e_pl_set_model(lua_State *s) {
    int slot = slot_arg(s, 1);
    const char *name = luaL_optstring(s, 2, "");
    int t = name[0] ? npc_type_find(name) : -1;
    if (name[0] && t < 0) {
        lua_pushboolean(s, 0);
        return 1;
    }
    game_pl_set_model(slot, t);
    lua_pushboolean(s, 1);
    return 1;
}

static int e_pl_disguise(lua_State *s) {
    lua_pushboolean(s, game_pl_disguise(slot_arg(s, 1), (int)luaL_checkinteger(s, 2)));
    return 1;
}

static int e_pl_undisguise(lua_State *s) {
    game_pl_undisguise(slot_arg(s, 1));
    return 0;
}

static int e_pl_disguise_name(lua_State *s) {
    const char *name = game_pl_disguise_name(slot_arg(s, 1));
    if (name) {
        lua_pushstring(s, name);
    } else {
        lua_pushnil(s);
    }
    return 1;
}

static int e_pl_weapon(lua_State *s) {
    lua_pushstring(s, game_weapon_id(game_pl_weapon(slot_arg(s, 1))));
    return 1;
}

static int e_pl_weapons(lua_State *s) {
    lua_pushinteger(s, (lua_Integer)game_pl_weapons(slot_arg(s, 1)));
    return 1;
}

static int e_pl_set_weapons(lua_State *s) {
    game_pl_set_weapons(slot_arg(s, 1), (unsigned int)luaL_checkinteger(s, 2));
    return 0;
}

static int e_pl_select(lua_State *s) {
    game_pl_select(slot_arg(s, 1), game_weapon_find(luaL_checkstring(s, 2)));
    return 0;
}

// (slot, weapon id or nil for a refill of everything, amount)
static int e_pl_give_ammo(lua_State *s) {
    int w = lua_isnoneornil(s, 2) ? -1 : game_weapon_find(luaL_checkstring(s, 2));
    if (!lua_isnoneornil(s, 2) && w < 0) return 0;
    game_pl_give_ammo(slot_arg(s, 1), w, (int)luaL_optinteger(s, 3, 0));
    return 0;
}

// A weapon's bit in the masks above, or nil if there's no such weapon
static int e_weapon_bit(lua_State *s) {
    int w = game_weapon_find(luaL_checkstring(s, 1));
    if (w < 0) {
        lua_pushnil(s);
    } else {
        lua_pushinteger(s, (lua_Integer)1 << w);
    }
    return 1;
}

static int e_weapon_ids(lua_State *s) {
    lua_newtable(s);
    for (int w = 0; w < game_weapon_count(); w++) {
        lua_pushstring(s, game_weapon_id(w));
        lua_rawseti(s, -2, w + 1);
    }
    return 1;
}

// (slot or -1 everyone, HUD_PRINTTALK 3 / HUD_PRINTCENTER 4, text)
static int e_message(lua_State *s) {
    int kind = (int)luaL_checkinteger(s, 2) == 4 ? GM_MSG_CENTER : GM_MSG_CHAT;
    game_gm_message(slot_arg(s, 1), kind, luaL_checkstring(s, 3));
    return 0;
}

// (slot or -1 everyone, name [, x, y, z])
static int e_sound(lua_State *s) {
    vec3 at;
    int positioned = !lua_isnoneornil(s, 3);
    if (positioned) at = vec_args(s, 3);
    game_gm_sound(slot_arg(s, 1), luaL_checkstring(s, 2), positioned ? &at : NULL);
    return 0;
}

static Body *body_arg(lua_State *s, int i) {
    return game_body((int)luaL_checkinteger(s, i));
}

static int e_ent_valid(lua_State *s) {
    lua_pushboolean(s, lua_isinteger(s, 1) && game_body((int)lua_tointeger(s, 1)) != NULL);
    return 1;
}

static int e_ent_serial(lua_State *s) {
    lua_pushinteger(s, game_body_serial((int)luaL_checkinteger(s, 1)));
    return 1;
}

static int e_ent_max(lua_State *s) {
    lua_pushinteger(s, MAX_BODIES);
    return 1;
}

static int e_ent_create(lua_State *s) {
    vec3 p = vec_args(s, 2);
    int b = game_spawn_prop_by_name(luaL_checkstring(s, 1), p.x, p.y, p.z, (float)luaL_optnumber(s, 5, 0.0));
    if (b < 0) {
        lua_pushnil(s);
    } else {
        lua_pushinteger(s, b);
    }
    return 1;
}

static int e_ent_remove(lua_State *s) {
    game_remove_body((int)luaL_checkinteger(s, 1));
    return 0;
}

static int e_ent_pos(lua_State *s) {
    Body *b = body_arg(s, 1);
    return b ? push_vec(s, b->pos) : 0;
}

static int e_ent_set_pos(lua_State *s) {
    Body *b = body_arg(s, 1);
    if (!b || !game_is_server()) return 0;
    b->pos = vec_args(s, 2);
    b->asleep = 0;
    return 0;
}

static int e_ent_vel(lua_State *s) {
    Body *b = body_arg(s, 1);
    return b ? push_vec(s, b->vel) : 0;
}

static int e_ent_set_vel(lua_State *s) {
    Body *b = body_arg(s, 1);
    if (!b || !game_is_server()) return 0;
    b->vel = vec_args(s, 2);
    b->frozen = 0;
    b->asleep = 0;
    b->idle_time = 0.0f;
    return 0;
}

static int e_ent_model(lua_State *s) {
    lua_pushstring(s, game_body_model_name((int)luaL_checkinteger(s, 1)));
    return 1;
}

static int e_ent_color(lua_State *s) {
    Body *b = body_arg(s, 1);
    if (!b || !game_is_server()) return 0;
    int r = (int)luaL_checkinteger(s, 2), g = (int)luaL_checkinteger(s, 3), bl = (int)luaL_checkinteger(s, 4);
    b->tint_abgr = 0xFF000000u | ((unsigned)(bl & 255) << 16) | ((unsigned)(g & 255) << 8) | (unsigned)(r & 255);
    return 0;
}

static int e_ent_freeze(lua_State *s) {
    Body *b = body_arg(s, 1);
    if (!b || !game_is_server()) return 0;
    b->frozen = lua_toboolean(s, 2);
    if (b->frozen) b->vel = b->angvel = v3(0, 0, 0);
    b->asleep = 0;
    return 0;
}

static int e_ent_frozen(lua_State *s) {
    Body *b = body_arg(s, 1);
    lua_pushboolean(s, b && b->frozen);
    return 1;
}

static int e_ent_mass(lua_State *s) {
    Body *b = body_arg(s, 1);
    lua_pushnumber(s, b ? b->mass : 0.0f);
    return 1;
}

static int e_ent_lib(lua_State *s) {
    Body *b = body_arg(s, 1);
    lua_pushinteger(s, b ? b->lib : 0);
    return 1;
}

// mins x y z, maxs x y z (about its centre)
static int e_ent_bounds(lua_State *s) {
    vec3 mn, mx;
    game_body_bounds((int)luaL_checkinteger(s, 1), &mn, &mx);
    push_vec(s, mn);
    return push_vec(s, mx) + 3;
}

static int e_aim_body(lua_State *s) {
    int b = game_aim_body();
    if (b < 0) {
        lua_pushnil(s);
    } else {
        lua_pushinteger(s, b);
    }
    return 1;
}

static int e_npc_spawn(lua_State *s) {
    int n = game_npc_spawn(luaL_checkstring(s, 1), vec_args(s, 2), (float)luaL_optnumber(s, 5, 0.0));
    if (n < 0) {
        lua_pushnil(s);
    } else {
        lua_pushinteger(s, n);
    }
    return 1;
}

// state (0 none, 1 dead, 2 alive), type name, x, y, z, yaw, health
static int e_npc_info(lua_State *s) {
    int type;
    vec3 pos;
    float yaw, hp;
    int st = npc_info((int)luaL_checkinteger(s, 1), &type, &pos, &yaw, &hp);
    lua_pushinteger(s, st);
    if (!st) return 1;
    lua_pushstring(s, npc_type_name(type));
    push_vec(s, pos);
    lua_pushnumber(s, yaw);
    lua_pushnumber(s, hp);
    return 7;
}

static int e_npc_serial(lua_State *s) {
    lua_pushinteger(s, npc_serial((int)luaL_checkinteger(s, 1)));
    return 1;
}

static int e_npc_set_health(lua_State *s) {
    if (game_is_server()) npc_set_health((int)luaL_checkinteger(s, 1), (float)luaL_checknumber(s, 2));
    return 0;
}

static int e_npc_set_pos(lua_State *s) {
    if (game_is_server()) npc_set_pos((int)luaL_checkinteger(s, 1), vec_args(s, 2));
    return 0;
}

static int e_npc_damage(lua_State *s) {
    game_npc_damage((int)luaL_checkinteger(s, 1), (float)luaL_checknumber(s, 2), (int)luaL_optinteger(s, 3, -1));
    return 0;
}

static int e_npc_remove(lua_State *s) {
    if (game_is_server()) npc_remove((int)luaL_checkinteger(s, 1));
    return 0;
}

static int e_npc_max(lua_State *s) {
    lua_pushinteger(s, MAX_NPCS);
    return 1;
}

static int e_npc_types(lua_State *s) {
    lua_newtable(s);
    int k = 0;
    for (int t = 0; t < npc_type_count(); t++) {
        if (npc_type_ragdoll_only(t)) continue;
        lua_pushstring(s, npc_type_name(t));
        lua_rawseti(s, -2, ++k);
    }
    return 1;
}

static int e_spawns(lua_State *s) {
    lua_pushinteger(s, game_spawn_count());
    return 1;
}

static int e_spawn(lua_State *s) {
    float yaw;
    push_vec(s, game_spawn_point((int)luaL_checkinteger(s, 1), &yaw));
    lua_pushnumber(s, yaw);
    return 4;
}

static int e_nav_count(lua_State *s) {
    lua_pushinteger(s, nav_available() ? nav_node_count() : 0);
    return 1;
}

static int e_nav_node(lua_State *s) {
    int i = (int)luaL_checkinteger(s, 1);
    if (!nav_available() || i < 0 || i >= nav_node_count()) return 0;
    return push_vec(s, nav_node_pos(i));
}

// (ax, ay, az, bx, by, bz [, skip slot]) -> fraction, x, y, z, body or nil, player slot or nil
static int e_trace(lua_State *s) {
    vec3 hit;
    int body, slot;
    float f = game_trace(vec_args(s, 1), vec_args(s, 4), (int)luaL_optinteger(s, 7, -1), &hit, &body, &slot);
    lua_pushnumber(s, f);
    push_vec(s, hit);
    if (body >= 0) {
        lua_pushinteger(s, body);
    } else {
        lua_pushnil(s);
    }
    if (slot >= 0) {
        lua_pushinteger(s, slot);
    } else {
        lua_pushnil(s);
    }
    return 6;
}

static int e_cleanup(lua_State *s) {
    game_cleanup_map();
    return 0;
}

static int e_button(lua_State *s) {
    lua_pushboolean(s, game_button_down(luaL_checkstring(s, 1)));
    return 1;
}

static int e_achieve(lua_State *s) {
    game_achievement(luaL_checkstring(s, 1));
    return 0;
}

static int e_dedicated(lua_State *s) {
    lua_pushboolean(s, game_is_dedicated());
    return 1;
}

// --- draw.*: inside HUDPaint ---------------------------------------------------------------------------

static int font_arg(lua_State *s, int i) {
    static const char *const names[] = {"small", "bold", "large", "title", NULL};
    return luaL_checkoption(s, i, "small", names);
}

static unsigned int colour_arg(lua_State *s, int i) {
    return (unsigned int)luaL_optinteger(s, i, (lua_Integer)0xFFFFFFFF);
}

// draw.Text(x, y, text [, colour [, align [, font]]]): align TEXT_ALIGN_LEFT / CENTER / RIGHT
static int l_draw_text(lua_State *s) {
    float x = (float)luaL_checknumber(s, 1), y = (float)luaL_checknumber(s, 2);
    const char *text = luaL_checkstring(s, 3);
    int align = (int)luaL_optinteger(s, 5, ALIGN_LEFT);
    if (align < ALIGN_LEFT || align > ALIGN_RIGHT) align = ALIGN_LEFT;
    ui_text_shadow(font_arg(s, 6), x, y, colour_arg(s, 4), align, text);
    return 0;
}

static int l_draw_rect(lua_State *s) {
    ui_rect((float)luaL_checknumber(s, 1), (float)luaL_checknumber(s, 2), (float)luaL_checknumber(s, 3),
            (float)luaL_checknumber(s, 4), colour_arg(s, 5));
    return 0;
}

// draw.RoundedBox(radius, x, y, w, h, colour), as GMod's
static int l_draw_rounded(lua_State *s) {
    ui_rrect((float)luaL_checknumber(s, 2), (float)luaL_checknumber(s, 3), (float)luaL_checknumber(s, 4),
             (float)luaL_checknumber(s, 5), (float)luaL_checknumber(s, 1), colour_arg(s, 6));
    return 0;
}

static int l_draw_text_width(lua_State *s) {
    lua_pushnumber(s, ui_text_width(font_arg(s, 2), luaL_checkstring(s, 1)));
    return 1;
}

// --- Starting and stopping ----------------------------------------------------------------------------

static void register_lib(const char *name, const luaL_Reg *fns) {
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, name);
}

// Calls a global Lua function with nargs arguments already pushed; leaves nres results (nils on an error)
static int call_global(const char *fn, int nargs, int nres) {
    lua_getglobal(L, fn);
    lua_insert(L, -(nargs + 1));
    if (lua_pcall(L, nargs, nres, 0) != LUA_OK) {
        report_top();
        for (int k = 0; k < nres; k++) lua_pushnil(L);
        return 0;
    }
    return 1;
}

// hook.Run(name, ...) with nargs arguments already pushed; leaves one result
static int call_hook(const char *name, int nargs) {
    lua_getglobal(L, "hook");
    lua_getfield(L, -1, "Run");
    lua_remove(L, -2);
    lua_pushstring(L, name);
    // hook.Run and the name go below the arguments
    lua_rotate(L, -(nargs + 2), 2);
    if (lua_pcall(L, nargs + 1, 1, 0) != LUA_OK) {
        report_top();
        lua_pushnil(L);
        return 0;
    }
    return 1;
}

// A Player object for slot s (init.lua's player.GetBySlot), or nil
static void push_player(int s) {
    if (s < 0) {
        lua_pushnil(L);
        return;
    }
    lua_pushinteger(L, s);
    call_global("_player_obj", 1, 1);
}

static void push_entity(int body) {
    if (body < 0) {
        lua_pushnil(L);
        return;
    }
    lua_pushinteger(L, body);
    call_global("_ent_obj", 1, 1);
}

int script_start(const char *map_name, const char *mode, int server, int client) {
    script_stop();
    g_time = g_frame = 0.0f;
    g_server = server;
    g_client = client;
    g_last_error[0] = 0;
    L = luaL_newstate();
    if (!L) return 1;
    luaL_openlibs(L);
    lua_register(L, "print", l_print);
    lua_register(L, "CurTime", l_curtime);
    lua_register(L, "FrameTime", l_frametime);
    lua_register(L, "include", l_include);
    static const luaL_Reg e[] = {
        {"pl_valid", e_pl_valid}, {"pl_local", e_pl_local}, {"pl_serial", e_pl_serial}, {"pl_name", e_pl_name},
        {"pl_health", e_pl_health}, {"pl_set_health", e_pl_set_health}, {"pl_max_health", e_pl_max_health},
        {"pl_set_max_health", e_pl_set_max_health}, {"pl_pos", e_pl_pos}, {"pl_view", e_pl_view},
        {"pl_team", e_pl_team}, {"pl_set_team", e_pl_set_team}, {"pl_spawn", e_pl_spawn}, {"pl_set_pos", e_pl_set_pos},
        {"pl_kill", e_pl_kill}, {"pl_flag", e_pl_flag}, {"pl_set_flag", e_pl_set_flag}, {"pl_speed", e_pl_speed},
        {"pl_set_speed", e_pl_set_speed}, {"pl_model", e_pl_model}, {"pl_set_model", e_pl_set_model},
        {"pl_disguise", e_pl_disguise}, {"pl_undisguise", e_pl_undisguise}, {"pl_disguise_name", e_pl_disguise_name},
        {"pl_weapon", e_pl_weapon}, {"pl_weapons", e_pl_weapons}, {"pl_set_weapons", e_pl_set_weapons},
        {"pl_select", e_pl_select}, {"pl_give_ammo", e_pl_give_ammo}, {"weapon_bit", e_weapon_bit},
        {"weapon_ids", e_weapon_ids}, {"message", e_message}, {"sound", e_sound}, {"ent_valid", e_ent_valid},
        {"ent_serial", e_ent_serial}, {"ent_max", e_ent_max}, {"ent_create", e_ent_create}, {"ent_remove", e_ent_remove},
        {"ent_pos", e_ent_pos}, {"ent_set_pos", e_ent_set_pos}, {"ent_vel", e_ent_vel}, {"ent_set_vel", e_ent_set_vel},
        {"ent_model", e_ent_model}, {"ent_color", e_ent_color}, {"ent_freeze", e_ent_freeze},
        {"ent_frozen", e_ent_frozen}, {"ent_mass", e_ent_mass}, {"ent_lib", e_ent_lib}, {"ent_bounds", e_ent_bounds},
        {"aim_body", e_aim_body}, {"npc_spawn", e_npc_spawn}, {"npc_info", e_npc_info}, {"npc_serial", e_npc_serial},
        {"npc_set_health", e_npc_set_health}, {"npc_set_pos", e_npc_set_pos}, {"npc_damage", e_npc_damage},
        {"npc_remove", e_npc_remove}, {"npc_max", e_npc_max}, {"npc_types", e_npc_types}, {"spawns", e_spawns},
        {"spawn", e_spawn}, {"nav_count", e_nav_count}, {"nav_node", e_nav_node}, {"trace", e_trace},
        {"cleanup", e_cleanup}, {"button", e_button}, {"achieve", e_achieve}, {"dedicated", e_dedicated},
        {"error", l_error}, {NULL, NULL}};
    static const luaL_Reg draw[] = {{"Text", l_draw_text}, {"SimpleText", l_draw_text}, {"Rect", l_draw_rect},
                                    {"RoundedBox", l_draw_rounded}, {"TextWidth", l_draw_text_width}, {NULL, NULL}};
    register_lib("_E", e);
    register_lib("draw", draw);
    lua_pushboolean(L, server);
    lua_setglobal(L, "SERVER");
    lua_pushboolean(L, client);
    lua_setglobal(L, "CLIENT");
    lua_pushstring(L, map_name);
    lua_setglobal(L, "MAP");
    lua_pushinteger(L, NET_MAX_PLAYERS);
    lua_setglobal(L, "MAX_PLAYERS");
    lua_pushstring(L, mode && mode[0] ? mode : "sandbox");
    lua_setglobal(L, "GAMEMODE_NAME");
    run_lua("init.lua");

    int found = 1;
    if (mode && mode[0]) {
        // GM is the gamemode's table while its file runs; then it's GAMEMODE, and its settings go to the engine
        lua_pushstring(L, mode);
        call_global("_gm_begin", 1, 0);
        char path[48];
        snprintf(path, sizeof(path), "gamemodes/%s.lua", mode);
        found = run_lua(path);
        if (!found) {
            char msg[64];
            snprintf(msg, sizeof(msg), "No gamemode %s on this PSP", mode);
            report(msg);
        }
        call_global("_gm_end", 0, 2);
        int flags = (int)lua_tointeger(L, -2);
        float respawn = (float)(lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1.0);
        lua_pop(L, 2);
        if (found) game_gm_setup(flags, respawn);
    }

    SceUID d = sceIoDopen("data/lua/autorun");
    if (d >= 0) {
        SceIoDirent ent;
        memset(&ent, 0, sizeof(ent));
        while (sceIoDread(d, &ent) > 0) {
            lower_name(ent.d_name);
            size_t n = strlen(ent.d_name);
            if (n > 4 && strcmp(ent.d_name + n - 4, ".lua") == 0 && workshop_enabled(WS_ADDON, ent.d_name)) {
                char path[128];
                snprintf(path, sizeof(path), "data/lua/autorun/%s", ent.d_name);
                run_file(path);
            }
            memset(&ent, 0, sizeof(ent));
        }
        sceIoDclose(d);
    }
    char path[96];
    snprintf(path, sizeof(path), "data/lua/maps/%s.lua", map_name);
    if (data_file_exists(path)) run_file(path);
    call_hook("Initialize", 0);
    lua_pop(L, 1);
    return found;
}

void script_stop(void) {
    if (L) lua_close(L);
    L = NULL;
}

void script_think(float dt) {
    if (!L) return;
    g_time += dt;
    g_frame = dt;
    lua_pushnumber(L, dt);
    call_global("_think", 1, 0);
}

int script_use(int slot, int body) {
    if (!L || !g_server) return 0;
    push_player(slot);
    push_entity(body);
    call_hook("PlayerUse", 2);
    int r = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return r;
}

void script_hud(void) {
    if (!L || !g_client) return;
    call_global("_hud", 0, 0);
}

// --- The gamemode's hooks ------------------------------------------------------------------------------

static void player_hook(const char *fn, int s) {
    if (!L || !g_server) return;
    lua_pushinteger(L, s);
    call_global(fn, 1, 0);
}

void script_player_join(int s) {
    player_hook("_player_join", s);
}

void script_player_spawn(int s) {
    player_hook("_player_spawn", s);
}

void script_player_leave(int s) {
    player_hook("_player_leave", s);
}

void script_player_death(int s, int attacker) {
    if (!L || !g_server) return;
    lua_pushinteger(L, s);
    if (attacker >= 0) {
        lua_pushinteger(L, attacker);
    } else {
        lua_pushnil(L);
    }
    call_global("_player_death", 2, 0);
}

int script_should_damage(int victim, int attacker) {
    if (!L || !g_server) return 1;
    push_player(victim);
    push_player(attacker);
    call_hook("PlayerShouldTakeDamage", 2);
    int r = !lua_isboolean(L, -1) || lua_toboolean(L, -1); // only false stops it
    lua_pop(L, 1);
    return r;
}

void script_entity_damage(int body, int attacker, float amount) {
    if (!L || !g_server) return;
    lua_pushinteger(L, body);
    lua_pushinteger(L, attacker);
    lua_pushnumber(L, amount);
    call_global("_entity_damage", 3, 0);
}

void script_npc_killed(int npc, int type, int attacker) {
    if (!L || !g_server) return;
    lua_pushinteger(L, npc);
    lua_pushinteger(L, attacker);
    call_global("_npc_killed", 2, 0);
}

int script_select_spawn(int s, vec3 *pos, float *yaw) {
    if (!L || !g_server) return 0;
    lua_pushinteger(L, s);
    call_global("_select_spawn", 1, 4);
    int ok = lua_isnumber(L, -4);
    if (ok) {
        *pos = v3((float)lua_tonumber(L, -4), (float)lua_tonumber(L, -3), (float)lua_tonumber(L, -2));
        *yaw = (float)(lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0);
    }
    lua_pop(L, 4);
    return ok;
}

int script_quick_chat(int s, const char *text) {
    if (!L || !g_server) return 0;
    push_player(s);
    lua_pushstring(L, text);
    call_hook("PlayerQuickChat", 2);
    int r = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return r;
}

int script_nw_pack(char *out, int max) {
    if (!L) return 0;
    call_global("_nw_pack", 0, 1);
    size_t n = 0;
    const char *s = lua_tolstring(L, -1, &n);
    if (!s) n = 0;
    if ((int)n > max) n = (size_t)max; // too much: the end is cut off (keep the variables few and short)
    if (n) memcpy(out, s, n);
    lua_pop(L, 1);
    return (int)n;
}

void script_nw_unpack(const char *in, int len) {
    if (!L) return;
    lua_pushlstring(L, in, (size_t)len);
    call_global("_nw_unpack", 1, 0);
}

// --- The gamemodes there are ---------------------------------------------------------------------------

static struct {
    char id[NET_MODE_LEN];
    char title[32];
} g_modes[MAX_GAMEMODES];
static int g_num_modes;

// GM.Name = "Prop Hunt" in a gamemode's source, or its id
static void mode_title(const char *src, const char *id, char *out, int len) {
    snprintf(out, len, "%s", id);
    const char *p = src ? strstr(src, "GM.Name") : NULL;
    if (!p) return;
    p = strchr(p, '"');
    if (!p) return;
    const char *e = strchr(p + 1, '"');
    if (!e || e - p - 1 >= len) return;
    snprintf(out, len, "%.*s", (int)(e - p - 1), p + 1);
}

static int mode_index(const char *id) {
    for (int i = 0; i < g_num_modes; i++) {
        if (strcasecmp(g_modes[i].id, id) == 0) return i;
    }
    return -1;
}

static void add_mode(const char *id, const char *src) {
    if (g_num_modes >= MAX_GAMEMODES || strlen(id) >= NET_MODE_LEN || mode_index(id) >= 0) return;
    snprintf(g_modes[g_num_modes].id, sizeof(g_modes[0].id), "%s", id);
    mode_title(src, id, g_modes[g_num_modes].title, sizeof(g_modes[0].title));
    g_num_modes++;
}

void script_gamemodes_scan(void) {
    g_num_modes = 0;
    add_mode("sandbox", "GM.Name = \"Sandbox\"");
    // Yours first: one named like the game's own replaces it
    SceUID d = sceIoDopen("data/lua/gamemodes");
    if (d >= 0) {
        SceIoDirent ent;
        memset(&ent, 0, sizeof(ent));
        while (sceIoDread(d, &ent) > 0) {
            lower_name(ent.d_name);
            size_t n = strlen(ent.d_name);
            if (n > 4 && n - 4 < NET_MODE_LEN && strcmp(ent.d_name + n - 4, ".lua") == 0) {
                char id[NET_MODE_LEN], path[128];
                snprintf(id, sizeof(id), "%.*s", (int)(n - 4), ent.d_name);
                snprintf(path, sizeof(path), "data/lua/gamemodes/%s", ent.d_name);
                char head[2048] = {0};
                FILE *f = fopen(path, "r");
                if (f) {
                    if (!fread(head, 1, sizeof(head) - 1, f)) head[0] = 0;
                    fclose(f);
                }
                add_mode(id, head);
            }
            memset(&ent, 0, sizeof(ent));
        }
        sceIoDclose(d);
    }
    for (int i = 0; i < (int)(sizeof(k_lua_builtin) / sizeof(k_lua_builtin[0])); i++) {
        const char *p = k_lua_builtin[i].path;
        size_t n = strlen(p);
        if (strncmp(p, "gamemodes/", 10) != 0 || n < 14 || strcmp(p + n - 4, ".lua") != 0) continue;
        char id[NET_MODE_LEN];
        snprintf(id, sizeof(id), "%.*s", (int)(n - 14), p + 10);
        add_mode(id, k_lua_builtin[i].src);
    }
}

int script_gamemode_count(void) {
    if (!g_num_modes) script_gamemodes_scan();
    return g_num_modes;
}

const char *script_gamemode_id(int i) {
    return i >= 0 && i < script_gamemode_count() ? g_modes[i].id : "sandbox";
}

const char *script_gamemode_title(int i) {
    return i >= 0 && i < script_gamemode_count() ? g_modes[i].title : "Sandbox";
}

int script_gamemode_find(const char *id) {
    script_gamemode_count();
    return mode_index(id && id[0] ? id : "sandbox");
}
