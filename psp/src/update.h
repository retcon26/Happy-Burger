#pragma once

#include "input.h"

// Options > Updates: checks GitHub for a newer engine (a release of retcon26/retcons-mod with an EBOOT.PBP),
// downloads it over Wi-Fi (HTTPS through mbedTLS) and puts it in place of the running EBOOT, keeping the old
// one as EBOOT.OLD. Only the engine updates: the game data is built from your own games on a PC, so a
// release that needs new data (its notes say <!-- data N --> with N above GAME_DATA_VERSION) is shown but
// not installed. Every step goes into data/update.log.

// eboot: argv[0], the EBOOT.PBP that's running
void update_init(const char *eboot);
// While a game is on (multiplayer may be using the network) the page only says to check from the main menu
void update_set_in_game(int in_game);

// The page, inside the Options window (settings_set_updates_page)
void update_page_open(void);
int update_page_update(const InputState *in); // 1 when the player backs out
void update_page_draw(float x, float y, float w, float h);

// autoload.txt test lines: "updaterepo OWNER/NAME", "updatever 1.2.3" (pretend to be that version),
// "updatetarget PATH" (install there instead), "updateget URL" (download to PATH.NEW, no install),
// "updateasset NAME" (that file of the release instead of EBOOT.PBP, not checked for being one),
// "updateauto" (download as soon as an update is found). 1 if it was one of them.
int update_command(const char *line);
