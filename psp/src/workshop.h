#pragma once

#include "input.h"

// Main menu > Workshop: turning maps, Lua addons and custom (Workshop) characters on and off, like GMod's
// addon list. What's off is saved to data/workshop.cfg.
//   Maps: off ones aren't offered in Start New Game or Host Game.
//   Addons (data/lua/autorun/*.lua): off ones don't run.
//   Custom: characters from your GMod Workshop downloads; off ones leave the spawn menu and player models.

enum { WS_MAP, WS_ADDON, WS_CUSTOM, WS_KINDS };

void workshop_load(void);
int workshop_enabled(int kind, const char *name);

// The page. The menu passes the maps it found, which game each is from (cat_of: an index into
// cat_names) and how many games there are. Returns 1 when the player backs out (saved then).
void workshop_open(char (*maps)[32], int num_maps, int (*cat_of)(const char *map), const char *const *cat_names,
                   int num_cats);

int workshop_update(const InputState *in);
void workshop_draw(float x, float y, float w, float h);
