#pragma once

// Lua 5.4 scripting with a small GMod-flavoured API (hook.Add, timer.Simple, ents.*, player.*).
// It runs data/lua/autorun/*.lua and data/lua/maps/<map>.lua when a map starts.

void script_start(const char *map_name);
void script_stop(void);
void script_think(float dt);
// "PlayerUse" hook; returns 1 if a script handled the use.
int script_use(void);
// "HUDPaint" hook; scripts draw text with draw.Text.
void script_hud(void);
