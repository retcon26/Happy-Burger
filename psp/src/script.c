#include "script.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "ui.h"

static lua_State *L;
static float g_time;

// hook and timer live in Lua; the engine only calls hook.Call
static const char *k_bootstrap =
    "hook = { list = {} }\n"
    "function hook.Add(ev, name, fn) hook.list[ev] = hook.list[ev] or {}; hook.list[ev][name] = fn end\n"
    "function hook.Remove(ev, name) if hook.list[ev] then hook.list[ev][name] = nil end end\n"
    "function hook.Call(ev, ...)\n"
    "  local l = hook.list[ev]; if not l then return nil end\n"
    "  for name, fn in pairs(l) do\n"
    "    local ok, r = pcall(fn, ...)\n"
    "    if not ok then print('hook ' .. ev .. '/' .. tostring(name) .. ': ' .. tostring(r)); l[name] = nil\n"
    "    elseif r ~= nil then return r end\n"
    "  end\n"
    "end\n"
    "timer = { pending = {} }\n"
    "function timer.Simple(delay, fn) table.insert(timer.pending, { t = CurTime() + delay, fn = fn }) end\n"
    "hook.Add('Think', '_timers', function()\n"
    "  local now, keep = CurTime(), {}\n"
    "  for _, t in ipairs(timer.pending) do\n"
    "    if t.t <= now then local ok, e = pcall(t.fn); if not ok then print('timer: ' .. tostring(e)) end\n"
    "    else keep[#keep + 1] = t end\n"
    "  end\n"
    "  timer.pending = keep\n"
    "end)\n";

static int l_print(lua_State *s) {
    luaL_Buffer b;
    luaL_buffinit(s, &b);
    int n = lua_gettop(s);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, ' ');
        luaL_addstring(&b, luaL_tolstring(s, i, NULL));
        lua_pop(s, 1);
    }
    luaL_pushresult(&b);
    game_message(lua_tostring(s, -1));
    return 0;
}

static int l_curtime(lua_State *s) {
    lua_pushnumber(s, g_time);
    return 1;
}

static Body *check_body(lua_State *s, int arg) {
    int i = (int)luaL_checkinteger(s, arg);
    return game_body(i);
}

static int l_create_prop(lua_State *s) {
    const char *name = luaL_checkstring(s, 1);
    float x = (float)luaL_checknumber(s, 2), y = (float)luaL_checknumber(s, 3), z = (float)luaL_checknumber(s, 4);
    float yaw = (float)luaL_optnumber(s, 5, 0.0);
    int b = game_spawn_prop_by_name(name, x, y, z, yaw);
    if (b < 0) {
        lua_pushnil(s);
    } else {
        lua_pushinteger(s, b);
    }
    return 1;
}

static int l_remove(lua_State *s) {
    game_remove_body((int)luaL_checkinteger(s, 1));
    return 0;
}

static int l_isvalid(lua_State *s) {
    lua_pushboolean(s, !lua_isnoneornil(s, 1) && lua_isinteger(s, 1) && check_body(s, 1) != NULL);
    return 1;
}

static int l_getpos(lua_State *s) {
    Body *b = check_body(s, 1);
    if (!b) return 0;
    lua_pushnumber(s, b->pos.x);
    lua_pushnumber(s, b->pos.y);
    lua_pushnumber(s, b->pos.z);
    return 3;
}

static int l_setpos(lua_State *s) {
    Body *b = check_body(s, 1);
    if (!b) return 0;
    b->pos = v3((float)luaL_checknumber(s, 2), (float)luaL_checknumber(s, 3), (float)luaL_checknumber(s, 4));
    b->asleep = 0;
    return 0;
}

static int l_setvel(lua_State *s) {
    Body *b = check_body(s, 1);
    if (!b) return 0;
    b->vel = v3((float)luaL_checknumber(s, 2), (float)luaL_checknumber(s, 3), (float)luaL_checknumber(s, 4));
    b->frozen = 0;
    b->asleep = 0;
    b->idle_time = 0.0f;
    return 0;
}

static int l_setcolor(lua_State *s) {
    Body *b = check_body(s, 1);
    if (!b) return 0;
    int r = (int)luaL_checkinteger(s, 2), g = (int)luaL_checkinteger(s, 3), bl = (int)luaL_checkinteger(s, 4);
    b->tint_abgr = 0xFF000000u | ((unsigned)(bl & 255) << 16) | ((unsigned)(g & 255) << 8) | (unsigned)(r & 255);
    return 0;
}

static int l_freeze(lua_State *s) {
    Body *b = check_body(s, 1);
    if (!b) return 0;
    b->frozen = lua_toboolean(s, 2);
    if (b->frozen) b->vel = b->angvel = v3(0, 0, 0);
    b->asleep = 0;
    return 0;
}

static int l_player_pos(lua_State *s) {
    float x, y, z;
    game_player_pos(&x, &y, &z);
    lua_pushnumber(s, x);
    lua_pushnumber(s, y);
    lua_pushnumber(s, z);
    return 3;
}

static int l_player_yaw(lua_State *s) {
    lua_pushnumber(s, game_player_yaw());
    return 1;
}

static int l_player_aim(lua_State *s) {
    int b = game_aim_body();
    if (b < 0) {
        lua_pushnil(s);
    } else {
        lua_pushinteger(s, b);
    }
    return 1;
}

static int l_draw_text(lua_State *s) {
    int x = (int)luaL_checkinteger(s, 1), y = (int)luaL_checkinteger(s, 2);
    const char *text = luaL_checkstring(s, 3);
    unsigned int color = (unsigned int)luaL_optinteger(s, 4, 0xFFFFFFFF);
    ui_text_shadow(FONT_SMALL, (float)x, (float)y, color, ALIGN_LEFT, text);
    return 0;
}

static void register_lib(const char *name, const luaL_Reg *fns) {
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, name);
}

static void run_file(const char *path) {
    if (luaL_dofile(L, path) != LUA_OK) {
        game_message(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void script_start(const char *map_name) {
    script_stop();
    g_time = 0.0f;
    L = luaL_newstate();
    if (!L) return;
    luaL_openlibs(L);
    lua_register(L, "print", l_print);
    lua_register(L, "CurTime", l_curtime);
    static const luaL_Reg ents[] = {{"CreateProp", l_create_prop}, {"Remove", l_remove}, {"IsValid", l_isvalid},
                                    {"GetPos", l_getpos}, {"SetPos", l_setpos}, {"SetVelocity", l_setvel},
                                    {"SetColor", l_setcolor}, {"Freeze", l_freeze}, {NULL, NULL}};
    static const luaL_Reg player[] = {{"GetPos", l_player_pos}, {"GetYaw", l_player_yaw},
                                      {"GetAimEntity", l_player_aim}, {NULL, NULL}};
    static const luaL_Reg draw[] = {{"Text", l_draw_text}, {NULL, NULL}};
    register_lib("ents", ents);
    register_lib("player", player);
    register_lib("draw", draw);
    if (luaL_dostring(L, k_bootstrap) != LUA_OK) {
        game_message(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pushstring(L, map_name);
    lua_setglobal(L, "MAP");

    SceUID d = sceIoDopen("data/lua/autorun");
    if (d >= 0) {
        SceIoDirent ent;
        memset(&ent, 0, sizeof(ent));
        while (sceIoDread(d, &ent) > 0) {
            size_t n = strlen(ent.d_name);
            if (n > 4 && strcmp(ent.d_name + n - 4, ".lua") == 0) {
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
    SceIoStat st;
    if (sceIoGetstat(path, &st) >= 0) run_file(path);
}

void script_stop(void) {
    if (L) lua_close(L);
    L = NULL;
}

// hook.Call(name, ...) with nargs arguments already pushed; leaves one result on the stack.
static int call_hook(const char *name, int nargs) {
    lua_getglobal(L, "hook");
    lua_getfield(L, -1, "Call");
    lua_remove(L, -2);
    lua_pushstring(L, name);
    // Move hook.Call and the name below the arguments
    lua_rotate(L, -(nargs + 2), 2);
    if (lua_pcall(L, nargs + 1, 1, 0) != LUA_OK) {
        game_message(lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_pushnil(L);
        return 0;
    }
    return 1;
}

void script_think(float dt) {
    if (!L) return;
    g_time += dt;
    lua_pushnumber(L, dt);
    call_hook("Think", 1);
    lua_pop(L, 1);
}

int script_use(void) {
    if (!L) return 0;
    call_hook("PlayerUse", 0);
    int r = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return r;
}

void script_hud(void) {
    if (!L) return;
    call_hook("HUDPaint", 0);
    lua_pop(L, 1);
}
