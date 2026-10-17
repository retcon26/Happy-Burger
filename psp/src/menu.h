#pragma once

#include "input.h"

// The main menu: a live map orbiting in the background, the logo, GMod-style buttons,
// the New Game map browser and Options.

typedef enum {
    MENU_STAY,
    MENU_START_GAME,
    MENU_HOST_GAME, // start map_out and host it over ad-hoc
    MENU_JOIN_GAME, // a host accepted us: load map_out, then game_join(menu_join_slot, menu_join_mac)
    MENU_QUIT,
} MenuResult;

void menu_open(void);  // loads the background map
void menu_close(void); // frees it (before a game loads)
MenuResult menu_update(const InputState *in, float dt, char *map_out, int map_out_len);
void menu_render(float fps);
void menu_set_error(const char *msg);
void menu_show_achievements(void); // autoload.txt "menu achievements"
void menu_show_newgame(const char *category); // autoload.txt "menu newgame [Workshop]": Start New Game
void menu_show_updates(void);      // Options > Updates (autoload.txt "menu updates")
void menu_show_workshop(void);     // autoload.txt "menu workshop"
void menu_show_multiplayer(void); // autoload.txt "menu multiplayer"
void menu_show_online(void); // Multiplayer > Online Servers (autoload.txt "menu online")
void menu_autojoin(const char *address); // autoload.txt "join ADDRESS": Online Servers, joining it
void menu_show_profile(void); // opens Multiplayer > Profile (autoload.txt "menu profile")
void menu_join_info(int *slot, unsigned char mac[6]);

// Shared look for the main menu and the in-game pause menu
void menu_draw_backdrop(void);
void menu_draw_buttons(const char *const *labels, int count, int selected, float y, float row_h);
