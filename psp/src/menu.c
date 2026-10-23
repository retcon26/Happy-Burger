#include "menu.h"

#include <math.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "achieve.h"
#include "audio.h"
#include "camera.h"
#include "game.h"
#include "workshop.h"

#include "gfx.h"
#include "map.h"
#include "mem.h"
#include "net.h"
#include "osk.h"
#include "script.h"
#include "settings.h"
#include "sky.h"
#include "ui.h"
#include "version.h"

#define MAX_MAPS 400
#define MAX_WORKSHOP_MAPS 64
#define WORKSHOP_LIST "data/maps/workshop.txt" // tools/bspconv.sh workshop writes it
#define BG_ABGR 0xFF101828

typedef enum { PAGE_MAIN, PAGE_NEWGAME, PAGE_OPTIONS, PAGE_MULTI, PAGE_JOIN, PAGE_PROFILE, PAGE_ACHIEVEMENTS, PAGE_WORKSHOP } Page;
#define MAX_HOSTS 32

// Map browser categories, by name prefix like GMod's New Game screen
static const struct {
    const char *name;
    const char *prefixes[12];
} k_cats[] = {
    {"Sandbox", {"gm_", NULL}},
    {"Counter-Strike", {"cs_", "de_", "as_", NULL}},
    {"Half-Life 2", {"d1_", "d2_", "d3_", NULL}},
    {"HL2 Deathmatch", {"dm_", NULL}},
    // Half-Life Deathmatch: Source's maps have no prefix of their own: they're named one by one
    {"HL Deathmatch", {"boot_camp", "bounce", "crossfire", "datacore", "frenzy", "lambda_bunker", "rapidcore",
                       "snarkpit", "stalkyard", "subtransit", "undertow", NULL}},

    {"Half-Life", {"hl1_", NULL}},
    {"Day of Defeat", {"dod_", NULL}},
    {"Team Fortress 2", {"ctf_", "cp_", "pl_", "plr_", "koth_", "arena_", "pd_", "sd_", "tc_", "mvm_", "pass_", NULL}},
    {"Workshop", {NULL}}, // maps from your GMod Workshop downloads (WORKSHOP_LIST), whatever their prefix
    {"Other", {NULL}},
};
#define CAT_WORKSHOP (NUM_CATS - 2)
#define NUM_CATS (int)(sizeof(k_cats) / sizeof(k_cats[0]))
#define GRID_COLS 3

static const char *k_main_items[] = {"Start Shift", "Play Together", "Options"};
// Start Shift and hosting go straight to the game: the burger joint, running Happy Burger
#define SHIFT_MAP "burgerjoint"
#define SHIFT_MODE "burger"
#define NUM_MAIN_ITEMS (int)(sizeof(k_main_items) / sizeof(k_main_items[0]))
// Host and Join are ad-hoc (PSP to PSP); Wi-Fi goes through an access point to games on the same
// network and to dedicated servers
static const char *k_multi_items[] = {"Host Game", "Join Game", "Host on Wi-Fi", "Online Servers", "Profile", "Back"};
#define NUM_MULTI_ITEMS 6
enum { MULTI_HOST, MULTI_JOIN, MULTI_HOST_WIFI, MULTI_ONLINE, MULTI_PROFILE, MULTI_BACK };
#define SERVERS_PATH "data/servers.cfg"
#define MAX_SAVED 12
#define QUERY_INTERVAL 1.0f
#define LIST_INTERVAL 15.0f // asking the server list again (servers come and go)
// The server list Online Servers asks when data/servers.cfg doesn't name one ("": none until you set one)
#define DEFAULT_SERVER_LIST ""

static struct {
    Page page;
    int sel;
    char maps[MAX_MAPS][32];
    int num_maps;
    int cat;
    int map_sel;
    int scroll;
    Map bg;
    int bg_loaded;
    Camera cam;
    float t;
    char error[96];
    int snd_move, snd_select;
    // Multiplayer
    int host;     // the map browser is picking a map to host
    int multi_sel;
    NetHost hosts[MAX_HOSTS];
    int num_hosts;
    int host_sel;
    int joining;
    int join_slot;
    uint8_t join_mac[6];
    // Online Servers: Join over Wi-Fi, with the servers you added (by address) as well as the LAN's games
    int online;
    char saved[MAX_SAVED][48];
    uint8_t saved_addr[MAX_SAVED][6];
    int saved_ok[MAX_SAVED]; // 1 resolved, -1 couldn't be, 0 not tried yet (DNS waits for Wi-Fi)
    int num_saved;
    float query_timer;
    int adding; // the keyboard is open: 1 a new server's address, 2 the server list's, 3 a password
    NetHost join_host; // the server being joined (asked for its password, or joined again after a map change)
    int sent_password; // that join sent a password
    // The server list (retcons-mod-server in list mode): every public server, without typing their addresses
    char list[48];
    uint8_t list_addr[6];
    int list_ok; // as saved_ok
    float list_timer;
    char autojoin[48]; // autoload.txt "join ADDRESS": join this server as soon as Wi-Fi is up
    char workshop[MAX_WORKSHOP_MAPS][32]; // the maps converted from the Workshop
    int num_workshop;
} M;

// The Workshop maps the converter listed ("name|addon title|item" lines)
static void load_workshop_maps(void) {
    M.num_workshop = 0;
    FILE *f = fopen(WORKSHOP_LIST, "r");
    if (!f) return;
    char line[160];
    while (M.num_workshop < MAX_WORKSHOP_MAPS && fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "|\r\n")] = 0;
        if (line[0] && strlen(line) < sizeof(M.workshop[0])) snprintf(M.workshop[M.num_workshop++], sizeof(M.workshop[0]), "%s", line);
    }
    fclose(f);
}

// New Game's gamemode: the one picked, unless it's gone or switched off in the Workshop (then Sandbox)
static int newgame_mode(void) {
    int k = script_gamemode_find(g_settings.gamemode);
    return k > 0 && workshop_gamemode_on(script_gamemode_id(k)) ? k : 0;
}

static int map_category(const char *name) {
    for (int i = 0; i < M.num_workshop; i++) {
        if (strcmp(M.workshop[i], name) == 0) return CAT_WORKSHOP;
    }
    for (int c = 0; c < NUM_CATS - 1; c++) {
        for (int p = 0; p < 12 && k_cats[c].prefixes[p]; p++) {
            if (strncmp(name, k_cats[c].prefixes[p], strlen(k_cats[c].prefixes[p])) == 0) return c;
        }
    }
    return NUM_CATS - 1;
}

// Half-Life's maps are converted as hl1_<name> (their names clash with Half-Life: Source's)
static const char *display_name(const char *map) {
    return strncmp(map, "hl1_", 4) == 0 ? map + 4 : map;
}

static int maps_in_cat(int cat, int *out) {
    int n = 0;
    for (int i = 0; i < M.num_maps; i++) {
        if (map_category(M.maps[i]) == cat && workshop_enabled(WS_MAP, M.maps[i])) out[n++] = i; // Workshop: off maps hidden
    }
    return n;
}

static void click(int id, float vol) {
    if (g_settings.sound) snd_play(id, vol, 0.0f);
}

// --- Online Servers: addresses you added, kept in data/servers.cfg (one per line) ---------------------

static void servers_load(void) {
    M.num_saved = 0;
    FILE *f = fopen(SERVERS_PATH, "r");
    if (!f) return;
    char line[64];
    while (M.num_saved < MAX_SAVED && fgets(line, sizeof(line), f)) {
        // "list ADDRESS" is the server list; any other line is a server
        int is_list = strncmp(line, "list ", 5) == 0;
        char *s = is_list ? line + 5 : line;
        s[strcspn(s, "\r\n ")] = 0;
        if (!s[0] || s[0] == '#') continue;
        if (is_list) {
            snprintf(M.list, sizeof(M.list), "%.47s", s);
            continue;
        }
        snprintf(M.saved[M.num_saved], sizeof(M.saved[0]), "%.47s", s);
        M.saved_ok[M.num_saved++] = 0;
    }
    fclose(f);
}

static void servers_save(void) {
    FILE *f = fopen(SERVERS_PATH, "w");
    if (!f) return;
    fprintf(f, "list %s\n", M.list[0] ? M.list : "-"); // "-": none (not even the default)
    for (int i = 0; i < M.num_saved; i++) fprintf(f, "%s\n", M.saved[i]);
    fclose(f);
}

static int list_set(void) {
    return M.list[0] && strcmp(M.list, "-") != 0;
}

// The rows of the list: the games that answered (hosts[]), then saved servers that haven't (-1 - index)
static int online_rows(int *rows) {
    int n = 0;
    for (int i = 0; i < M.num_hosts; i++) rows[n++] = i;
    for (int s = 0; s < M.num_saved && M.online; s++) {
        int found = 0;
        for (int i = 0; i < M.num_hosts && !found; i++) {
            found = M.saved_ok[s] == 1 && memcmp(M.hosts[i].mac, M.saved_addr[s], 6) == 0;
        }
        if (!found) rows[n++] = -1 - s;
    }
    return n;
}

// The saved server a row is (a game that answered counts if it's at a saved address), or -1
static int row_saved(int row) {
    if (row < 0) return -1 - row;
    for (int s = 0; s < M.num_saved; s++) {
        if (M.saved_ok[s] == 1 && memcmp(M.hosts[row].mac, M.saved_addr[s], 6) == 0) return s;
    }
    return -1;
}

// Wi-Fi: resolve what's new, ask the server list now and then, and every second everyone on the LAN,
// every saved server and every server the list gave about their games
static void online_query(float dt) {
    if (!M.online || net_poll() != NET_READY) return;
    if (list_set() && M.list_ok == 0) {
        // The list's own port unless the address says otherwise
        char text[56];
        snprintf(text, sizeof(text), strchr(M.list, ':') ? "%s" : "%s:18257", M.list);
        M.list_ok = net_resolve(text, M.list_addr) == 0 ? 1 : -1;
        M.list_timer = 0.0f;
        return;
    }
    for (int s = 0; s < M.num_saved; s++) {
        if (M.saved_ok[s] == 0) {
            M.saved_ok[s] = net_resolve(M.saved[s], M.saved_addr[s]) == 0 ? 1 : -1;
            return; // one DNS wait a frame at most
        }
    }
    M.list_timer -= dt;
    if (M.list_ok == 1 && M.list_timer <= 0.0f) {
        M.list_timer = LIST_INTERVAL;
        net_lobby_list_request(M.list_addr);
    }
    M.query_timer -= dt;
    if (M.query_timer > 0.0f) return;
    M.query_timer = QUERY_INTERVAL;
    net_lobby_query(NULL);
    for (int s = 0; s < M.num_saved; s++) {
        if (M.saved_ok[s] == 1) net_lobby_query(M.saved_addr[s]);
    }
}

void menu_open(void) {
    M.page = PAGE_MAIN;
    M.sel = 0;
    M.num_maps = map_list(M.maps, MAX_MAPS);
    load_workshop_maps();
    M.snd_move = snd_find("menu_move");
    M.snd_select = snd_find("menu_select");
    // Open the browser on the first category that has maps
    int tmp[MAX_MAPS];
    for (M.cat = 0; M.cat < NUM_CATS - 1 && !maps_in_cat(M.cat, tmp); M.cat++) {
    }
    // Background: like GMod, a real map behind the menu. Flatgrass loads fastest.
    const char *bg = NULL;
    for (int i = 0; i < M.num_maps; i++) {
        if (strcmp(M.maps[i], "gm_flatgrass") == 0) bg = M.maps[i];
    }
    if (!bg && M.num_maps) bg = M.maps[0];
    char err[96];
    M.bg_loaded = bg && map_load(&M.bg, bg, err, sizeof(err)) == 0;
    if (M.bg_loaded) {
        sky_build(&M.bg);
        camera_init(&M.cam);
        M.cam.fov_y = 62.0f;
        M.cam.zfar = M.bg.info->fog_end + 512.0f;
    }
}

void menu_close(void) {
    if (M.bg_loaded) map_free(&M.bg);
    M.bg_loaded = 0;
}

void menu_show_newgame(const char *category) {
    M.page = PAGE_NEWGAME;
    M.host = 0;
    for (int c = 0; category && c < NUM_CATS; c++) {
        if (strcasecmp(k_cats[c].name, category) == 0) M.cat = c;
    }
    M.map_sel = M.scroll = 0;
}

void menu_show_achievements(void) {
    M.page = PAGE_ACHIEVEMENTS;
    M.sel = 3;
}

void menu_show_updates(void) {
    M.page = PAGE_OPTIONS;
    M.sel = 4;
    options_open_updates();
}

void menu_show_workshop(void) {
    // Maps listed by game, the same games as Start New Game's
    static const char *names[NUM_CATS];
    for (int c = 0; c < NUM_CATS; c++) names[c] = k_cats[c].name;
    workshop_open(M.maps, M.num_maps, map_category, names, NUM_CATS);

    M.page = PAGE_WORKSHOP;
    M.sel = 2;
}

void menu_show_online(void) {
    M.page = PAGE_JOIN;
    M.online = 1;
    M.num_hosts = 0;
    M.host_sel = 0;
    M.joining = 0;
    M.adding = 0;
    M.query_timer = 0.0f;
    snprintf(M.list, sizeof(M.list), "%s", DEFAULT_SERVER_LIST);
    M.list_ok = 0;
    servers_load();
    net_set_mode(NET_MODE_INET, 0);
    net_start();
}

void menu_autojoin(const char *address) {
    M.page = PAGE_JOIN;
    M.online = 1;
    M.num_hosts = 0;
    M.host_sel = 0;
    M.joining = 0;
    M.adding = 0;
    M.num_saved = 0;
    snprintf(M.autojoin, sizeof(M.autojoin), "%s", address);
    net_set_mode(NET_MODE_INET, 0);
    net_start();
}

void menu_show_multiplayer(void) {
    M.page = PAGE_MULTI;
    M.sel = 1;
    M.multi_sel = 0;
}

void menu_show_profile(void) {

    M.page = PAGE_PROFILE;
    M.multi_sel = MULTI_PROFILE;
}

void menu_set_error(const char *msg) {

    snprintf(M.error, sizeof(M.error), "%s", msg);
}

void menu_join_info(int *slot, unsigned char mac[6]) {
    *slot = M.join_slot;
    memcpy(mac, M.join_mac, 6);
}

MenuResult menu_update(const InputState *in, float dt, char *map_out, int map_out_len) {
    M.t += dt;
    if (M.page == PAGE_OPTIONS) {
        if (options_update(in)) M.page = PAGE_MAIN;
        return MENU_STAY;
    }
    if (M.page == PAGE_PROFILE) {
        if (profile_update(in)) M.page = PAGE_MULTI;
        return MENU_STAY;
    }
    if (M.page == PAGE_WORKSHOP) {
        if (workshop_update(in)) {
            M.page = PAGE_MAIN;
            M.map_sel = 0;
            M.scroll = 0;
            game_refresh_player_models(); // custom characters switched on or off
        }
        return MENU_STAY;
    }
    if (M.page == PAGE_ACHIEVEMENTS) {
        if (achievements_update(in)) M.page = PAGE_MAIN;
        return MENU_STAY;
    }
    if (M.page == PAGE_MAIN) {
        int n = NUM_MAIN_ITEMS;
        if (in->pressed & PSP_CTRL_UP) {
            M.sel = (M.sel + n - 1) % n;
            click(M.snd_move, 0.4f);
        }
        if (in->pressed & PSP_CTRL_DOWN) {
            M.sel = (M.sel + 1) % n;
            click(M.snd_move, 0.4f);
        }
        if (in->pressed & PSP_CTRL_CROSS) {
            click(M.snd_select, 0.6f);
            if (M.sel == 0) {
                snprintf(map_out, map_out_len, "%s", SHIFT_MAP);
                game_set_gamemode(SHIFT_MODE);
                M.error[0] = 0;
                return MENU_START_GAME;
            } else if (M.sel == 1) {
                M.page = PAGE_MULTI;
                M.multi_sel = 0;
            } else {
                M.page = PAGE_OPTIONS;
            }
        }
        return MENU_STAY;
    }
    if (M.page == PAGE_MULTI) {
        if (in->pressed & PSP_CTRL_UP) {
            M.multi_sel = (M.multi_sel + NUM_MULTI_ITEMS - 1) % NUM_MULTI_ITEMS;
            click(M.snd_move, 0.4f);
        }
        if (in->pressed & PSP_CTRL_DOWN) {
            M.multi_sel = (M.multi_sel + 1) % NUM_MULTI_ITEMS;
            click(M.snd_move, 0.4f);
        }
        if (in->pressed & PSP_CTRL_CIRCLE || ((in->pressed & PSP_CTRL_CROSS) && M.multi_sel == MULTI_BACK)) {
            M.page = PAGE_MAIN;
            click(M.snd_move, 0.4f);
        } else if (in->pressed & PSP_CTRL_CROSS) {
            click(M.snd_select, 0.6f);
            M.error[0] = 0;
            if (M.multi_sel == MULTI_HOST || M.multi_sel == MULTI_HOST_WIFI) {
                // A shift together: straight into the burger joint
                snprintf(map_out, map_out_len, "%s", SHIFT_MAP);
                game_set_gamemode(SHIFT_MODE);
                net_set_mode(M.multi_sel == MULTI_HOST_WIFI ? NET_MODE_INET : NET_MODE_ADHOC, 1);
                return MENU_HOST_GAME;
            } else if (M.multi_sel == MULTI_PROFILE) {
                M.page = PAGE_PROFILE;
            } else {
                if (M.multi_sel == MULTI_ONLINE) {
                    menu_show_online();
                } else {
                    M.page = PAGE_JOIN;
                    M.online = 0;
                    M.num_hosts = 0;
                    M.host_sel = 0;
                    M.joining = 0;
                    M.adding = 0;
                    net_set_mode(NET_MODE_ADHOC, 0);
                    net_start();
                }
            }
        }
        return MENU_STAY;
    }
    if (M.page == PAGE_JOIN) {
        int r = net_lobby_update(dt, M.hosts, MAX_HOSTS, &M.num_hosts);
        online_query(dt);
        if (M.autojoin[0] && net_poll() == NET_READY) {
            NetHost h;
            memset(&h, 0, sizeof(h));
            if (net_resolve(M.autojoin, h.mac) == 0) {
                M.joining = 1;
                M.join_host = h;
                M.sent_password = 1; // the one it joined with before, if any
                net_lobby_join(&h);
            } else {
                menu_set_error("Couldn't find that address");
            }
            M.autojoin[0] = 0;
        }
        int rows[MAX_HOSTS + MAX_SAVED];
        int nrows = online_rows(rows);
        if (M.host_sel >= nrows) M.host_sel = nrows > 0 ? nrows - 1 : 0;
        if (M.adding) {
            // The keyboard is up for a new server's address
            char typed[48];
            if (osk_active()) return MENU_STAY;
            int was = M.adding;
            M.adding = 0;
            if (was == 3) {
                // The server's password: join with it (Circle on the keyboard gives up)
                if (osk_result(typed, sizeof(typed)) && typed[0]) {
                    net_lobby_set_password(typed);
                    M.sent_password = 1;
                    M.joining = 1;
                    net_lobby_join(&M.join_host);
                }
                return MENU_STAY;
            }
            if (osk_result(typed, sizeof(typed))) {
                char *s = typed;
                while (*s == ' ') s++;
                s[strcspn(s, " ")] = 0;
                if (was == 2) {
                    // The server list: empty switches it off
                    snprintf(M.list, sizeof(M.list), "%s", *s ? s : "-");
                    M.list_ok = 0;
                    M.num_hosts = 0;
                    servers_save();
                } else if (*s && M.num_saved < MAX_SAVED) {
                    snprintf(M.saved[M.num_saved], sizeof(M.saved[0]), "%.47s", s);
                    M.saved_ok[M.num_saved++] = 0;
                    servers_save();
                    M.query_timer = 0.0f;
                } else if (*s) {
                    menu_set_error("That's enough servers: remove one with Square");
                }
            }
            return MENU_STAY;
        }
        if (r == 1) {
            net_lobby_result(map_out, map_out_len, &M.join_slot, M.join_mac);
            M.joining = 0;
            return MENU_JOIN_GAME; // the connection stays up for the game
        }
        if (r == -1) {
            M.joining = 0;
            menu_set_error("That game is full");
        }
        if (r == -2) {
            // A password: asked for (again, if the one sent was wrong)
            M.joining = 0;
            if (M.sent_password) menu_set_error("Wrong password");
            if (osk_start(M.sent_password ? "Wrong password: try again" : "This server needs a password", "", 15) == 0) M.adding = 3;
        }
        if ((in->pressed & PSP_CTRL_UP) && M.host_sel > 0) {
            M.host_sel--;
            click(M.snd_move, 0.4f);
        }
        if ((in->pressed & PSP_CTRL_DOWN) && M.host_sel + 1 < nrows) {
            M.host_sel++;
            click(M.snd_move, 0.4f);
        }
        if ((in->pressed & PSP_CTRL_CROSS) && nrows > 0 && !M.joining) {
            int row = rows[M.host_sel];
            if (row >= 0) {
                click(M.snd_select, 0.6f);
                M.error[0] = 0;
                M.join_host = M.hosts[row];
                M.sent_password = 0;
                net_lobby_set_password("");
                if (M.hosts[row].password) {
                    if (osk_start("This server needs a password", "", 15) == 0) M.adding = 3;
                } else {
                    M.joining = 1;
                    net_lobby_join(&M.hosts[row]);
                }
            } else {
                menu_set_error(M.saved_ok[-1 - row] < 0 ? "Couldn't find that address" : "That server isn't answering");
            }
        }
        if (M.online && (in->pressed & PSP_CTRL_RTRIGGER) && !M.joining) {
            click(M.snd_select, 0.6f);
            if (osk_start("Server list address (empty: none)", list_set() ? M.list : "", 47) == 0) M.adding = 2;
        }
        if (M.online && (in->pressed & PSP_CTRL_TRIANGLE) && !M.joining) {
            click(M.snd_select, 0.6f);
            if (osk_start("Server address (name or 1.2.3.4, :port optional)", "", 47) == 0) M.adding = 1;
        }
        if (M.online && (in->pressed & PSP_CTRL_SQUARE) && nrows > 0 && !M.joining) {
            int s = row_saved(rows[M.host_sel]);
            if (s >= 0) {
                click(M.snd_move, 0.4f);
                for (int k = s; k + 1 < M.num_saved; k++) {
                    memcpy(M.saved[k], M.saved[k + 1], sizeof(M.saved[0]));
                    memcpy(M.saved_addr[k], M.saved_addr[k + 1], 6);
                    M.saved_ok[k] = M.saved_ok[k + 1];
                }
                M.num_saved--;
                servers_save();
            }
        }
        if (in->pressed & PSP_CTRL_CIRCLE) {
            net_stop();
            M.page = PAGE_MULTI;
            click(M.snd_move, 0.4f);
        }
        return MENU_STAY;
    }

    // New Game browser
    int idx[MAX_MAPS];
    int n = maps_in_cat(M.cat, idx);
    if (in->pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
        int dir = (in->pressed & PSP_CTRL_RTRIGGER) ? 1 : NUM_CATS - 1;
        // Skip empty categories
        for (int k = 0; k < NUM_CATS; k++) {
            M.cat = (M.cat + dir) % NUM_CATS;
            if (maps_in_cat(M.cat, idx)) break;
        }
        n = maps_in_cat(M.cat, idx);
        M.map_sel = 0;
        M.scroll = 0;
        click(M.snd_move, 0.4f);
    }
    if (n > 0) {
        int before = M.map_sel;
        if ((in->pressed & PSP_CTRL_RIGHT) && M.map_sel + 1 < n) M.map_sel++;
        if ((in->pressed & PSP_CTRL_LEFT) && M.map_sel > 0) M.map_sel--;
        if ((in->pressed & PSP_CTRL_DOWN) && M.map_sel + GRID_COLS < n) M.map_sel += GRID_COLS;
        if ((in->pressed & PSP_CTRL_UP) && M.map_sel - GRID_COLS >= 0) M.map_sel -= GRID_COLS;
        if (M.map_sel != before) click(M.snd_move, 0.4f);
        int row = M.map_sel / GRID_COLS;
        if (row < M.scroll) M.scroll = row;
        if (row > M.scroll + 1) M.scroll = row - 1;
        if (in->pressed & PSP_CTRL_CROSS) {
            click(M.snd_select, 0.7f);
            snprintf(map_out, map_out_len, "%s", M.maps[idx[M.map_sel]]);
            game_set_gamemode(script_gamemode_id(newgame_mode()));
            M.error[0] = 0;
            if (M.host) net_set_mode(M.host == 2 ? NET_MODE_INET : NET_MODE_ADHOC, 1);
            return M.host ? MENU_HOST_GAME : MENU_START_GAME;
        }
    }
    if (in->pressed & PSP_CTRL_TRIANGLE) {
        // The next gamemode: Sandbox, Prop Hunt, Zombie Survival... (and yours in data/lua/gamemodes)
        // (skipping the ones switched off in the Workshop's Addons)
        int count = script_gamemode_count(), k = newgame_mode();
        for (int step = 0; step < count; step++) {
            k = (k + 1) % count;
            if (k == 0 || workshop_gamemode_on(script_gamemode_id(k))) break;
        }
        snprintf(g_settings.gamemode, sizeof(g_settings.gamemode), "%s", k ? script_gamemode_id(k) : "");
        settings_save();
        click(M.snd_move, 0.4f);
    }
    if (in->pressed & PSP_CTRL_CIRCLE) {
        M.page = M.host ? PAGE_MULTI : PAGE_MAIN;
        click(M.snd_move, 0.4f);
    }
    return MENU_STAY;
}

void menu_draw_backdrop(void) {
    // Darken the left side where the menu sits, like GMod's main menu
    ui_hgradient(0, 0, 300, 272, 0xC0000000, 0x00000000);
    ui_gradient(0, 212, 480, 60, 0x00000000, 0x90000000);
}

void menu_draw_buttons(const char *const *labels, int count, int selected, float y, float row_h) {
    for (int i = 0; i < count; i++) {
        float ry = y + i * row_h;
        int sel = i == selected;
        if (sel) ui_hgradient(16, ry - 3, 220, 24, 0x60FFFFFF, 0x00FFFFFF);
        ui_text_shadow(FONT_LARGE, sel ? 30 : 24, ry, sel ? 0xFFFFFFFF : 0xFFC8C8C8, ALIGN_LEFT, labels[i]);
    }
}

// ui.pak's logo (tools/burgerkit/art.py), or the name as text without one
static void draw_logo(void) {
    int logo = ui_image_find("logo");
    if (logo >= 0) {
        int w, h;
        ui_image_size(logo, &w, &h);
        ui_image(logo, 16, 14, (float)w, (float)h, 0xFFFFFFFF);
    } else {
        ui_text_shadow(FONT_TITLE, 20, 20, 0xFF30C8FF, ALIGN_LEFT, "Happy Burger");
    }
}

static void draw_newgame(void) {
    float x = 40, y = 20, w = 400, h = 232;
    ui_rrect(x - 2, y - 2, w + 4, h + 4, 6, 0x60000000);
    ui_rrect(x, y, w, h, 5, 0xF5E8E8E8);
    ui_gradient(x, y, w, 24, 0xFF2A30C0, 0xFF1E2290);
    ui_text(FONT_BOLD, x + 10, y + 6, 0xFFFFFFFF, ALIGN_LEFT,
            M.host == 2 ? "Host Multiplayer Game (Wi-Fi)" : M.host ? "Host Multiplayer Game (Ad-hoc)" : "Start New Game");

    // Categories down the left
    float cx = x + 6, cy = y + 30;
    int idx[MAX_MAPS];
    for (int c = 0; c < NUM_CATS; c++) {
        int n = maps_in_cat(c, idx);
        int sel = c == M.cat;
        if (sel) ui_rrect(cx, cy + c * 20 - 2, 108, 18, 3, 0xFF2AA8F0);
        unsigned int tc = sel ? 0xFFFFFFFF : (n ? 0xFF303030 : 0xFFA0A0A0);
        ui_text(FONT_SMALL, cx + 6, cy + c * 20 + 1, tc, ALIGN_LEFT, k_cats[c].name);
        ui_textf(FONT_SMALL, cx + 102, cy + c * 20 + 1, sel ? 0xFFFFFFFF : 0xFF808080, ALIGN_RIGHT, "%d", n);
    }

    // Map grid with thumbnails
    int n = maps_in_cat(M.cat, idx);
    float gx = x + 124, gy = y + 30;
    const float tile = 84, gap = 6;
    if (n == 0) ui_text(FONT_SMALL, gx, gy + 10, 0xFF606060, ALIGN_LEFT, "No maps here. Add some in tools/maps.txt");
    for (int k = M.scroll * GRID_COLS; k < n && k < (M.scroll + 2) * GRID_COLS; k++) {
        int r = k / GRID_COLS - M.scroll, c = k % GRID_COLS;
        float tx = gx + c * (tile + gap), ty = gy + r * (tile + gap); // the name is on the tile: two rows fit above the buttons
        int sel = k == M.map_sel;
        ui_rrect(tx - 2, ty - 2, tile + 4, tile + 4, 3, sel ? 0xFF2AA8F0 : 0xFFC8C8C8);
        if (!ui_map_thumb(M.maps[idx[k]], tx, ty, tile, tile)) {
            ui_gradient(tx, ty, tile, tile, 0xFF606060, 0xFF303030);
            ui_text(FONT_BOLD, tx + tile / 2, ty + tile / 2 - 6, 0xFFE0E0E0, ALIGN_CENTER, "?");
        }
        ui_rect(tx, ty + tile - 14, tile, 14, 0xA0000000);
        ui_text_clip(FONT_SMALL, tx + 3, ty + tile - 13, tile - 6, 0xFFFFFFFF, display_name(M.maps[idx[k]]));
    }
    int rows = (n + GRID_COLS - 1) / GRID_COLS;
    if (rows > 2) {
        float bar_h = 174.0f * 2 / rows;
        ui_rect(x + w - 8, gy, 3, 174, 0xFFC0C0C0);
        ui_rect(x + w - 8, gy + M.scroll * (174.0f - bar_h) / (rows - 2), 3, bar_h, 0xFF808080);
    }

    // Start button, mustard, and the gamemode it starts with
    ui_rrect(x + w - 112, y + h - 26, 104, 20, 3, 0xFF2AA8F0);
    ui_text(FONT_BOLD, x + w - 60, y + h - 22, 0xFFFFFFFF, ALIGN_CENTER, M.host ? "X  Host Game" : "X  Start Game");
    ui_text(FONT_SMALL, x + 10, y + h - 21, 0xFF606060, ALIGN_LEFT, "L/R: category    Triangle: gamemode    O: back");
    int gm = newgame_mode();
    char line[64];
    snprintf(line, sizeof(line), "Gamemode: %s", script_gamemode_title(gm));
    float gw = ui_text_width(FONT_BOLD, line) + 14;
    ui_rrect(x + w - 6 - gw, y + 3, gw, 18, 3, gm > 0 ? 0xFF3D8CE6 : 0x40FFFFFF);
    ui_text(FONT_BOLD, x + w - 6 - gw / 2, y + 6, 0xFFFFFFFF, ALIGN_CENTER, line);
}

// Join Game (ad-hoc): the games nearby, from their hosts' beacons. Online Servers (Wi-Fi): the games on
// the same network and the servers you added.
static void draw_join(void) {
    float x = 40, y = 20, w = 400, h = 232;
    ui_rrect(x - 2, y - 2, w + 4, h + 4, 6, 0x60000000);
    ui_rrect(x, y, w, h, 5, 0xF5E8E8E8);
    ui_gradient(x, y, w, 24, 0xFF2A30C0, 0xFF1E2290);
    ui_text(FONT_BOLD, x + 10, y + 6, 0xFFFFFFFF, ALIGN_LEFT, M.online ? "Online Servers (Wi-Fi)" : "Join Game (Ad-hoc)");
    ui_textf(FONT_SMALL, x + w - 10, y + 7, 0xFFE0E0E0, ALIGN_RIGHT, "You: %s", net_nickname());

    NetState st = net_poll();
    int rows[MAX_HOSTS + MAX_SAVED];
    int nrows = online_rows(rows);
    float ly = y + 32;
    if (st != NET_READY) {
        ui_text(FONT_SMALL, x + 14, ly + 4, st == NET_FAILED ? 0xFF2020C0 : 0xFF404040, ALIGN_LEFT,
                st == NET_FAILED ? net_status() : M.online ? "Connecting to Wi-Fi..." : "Starting ad-hoc wireless...");
        if (st == NET_FAILED && M.online) {
            ui_text(FONT_SMALL, x + 14, ly + 22, 0xFF606060, ALIGN_LEFT, "Check the WLAN switch and System Settings > Network");
            ui_text(FONT_SMALL, x + 14, ly + 38, 0xFF606060, ALIGN_LEFT, "Settings. In PPSSPP, enable networking (WLAN).");
        } else if (st == NET_FAILED) {
            ui_text(FONT_SMALL, x + 14, ly + 22, 0xFF606060, ALIGN_LEFT, "Check the WLAN switch. In PPSSPP, enable networking and");
            ui_text(FONT_SMALL, x + 14, ly + 38, 0xFF606060, ALIGN_LEFT, "the built-in ad-hoc server in Settings > Networking.");
        }
    } else if (nrows == 0) {
        // A little spinner while we listen for beacons
        int dots = (int)(M.t * 3.0f) % 4;
        ui_textf(FONT_SMALL, x + 14, ly + 4, 0xFF404040, ALIGN_LEFT, "Looking for games%.*s", dots, "...");
        ui_text(FONT_SMALL, x + 14, ly + 22, 0xFF606060, ALIGN_LEFT,
                M.online ? "Triangle adds a server's address; games on this Wi-Fi show up here."
                         : "Someone nearby has to pick Multiplayer > Host Game.");
    }
    // Online: five rows, then the server list's line
    int shown = M.online ? 5 : 6;
    int first = M.host_sel >= shown ? M.host_sel - shown + 1 : 0;
    for (int r = first; r < nrows && r < first + shown; r++) {
        float ry = ly + (r - first) * 30;
        int sel = r == M.host_sel, row = rows[r];
        unsigned int tc = sel ? 0xFFFFFFFF : 0xFF303030;
        if (row < 0) {
            // A saved server that hasn't answered
            int s = -1 - row;
            ui_rrect(x + 8, ry, w - 16, 26, 3, sel ? 0xFF7090C0 : 0xFFE0E0E0);
            ui_text_clip(FONT_BOLD, x + 40, ry + 6, 220, sel ? 0xFFFFFFFF : 0xFF808080, M.saved[s]);
            ui_text(FONT_SMALL, x + w - 16, ry + 7, sel ? 0xFFFFFFFF : 0xFF909090, ALIGN_RIGHT,
                    st != NET_READY ? "" : M.saved_ok[s] < 0 ? "address not found" : "no answer");
            continue;
        }
        const NetHost *hs = &M.hosts[row];
        ui_rrect(x + 8, ry, w - 16, 26, 3, sel ? 0xFF2AA8F0 : 0xFFF8F8F8);
        ui_map_thumb(hs->map, x + 11, ry + 2, 22, 22);
        ui_text_clip(FONT_BOLD, x + 40, ry + 6, 132, tc, hs->host);
        char where[56];
        int gm = hs->mode[0] ? script_gamemode_find(hs->mode) : -1;
        if (hs->mode[0]) {
            snprintf(where, sizeof(where), "%s - %s", hs->map, gm > 0 ? script_gamemode_title(gm) : hs->mode);
        } else {
            snprintf(where, sizeof(where), "%s", hs->map);
        }
        ui_text_clip(FONT_SMALL, x + 180, ry + 7, 130, tc, where);
        ui_textf(FONT_SMALL, x + w - 16, ry + 7, tc, ALIGN_RIGHT, "%s%d / %d",
                 hs->password ? "locked  " : hs->dedicated ? "server  " : "", hs->players,
                 hs->max);
    }
    if (M.joining) {
        ui_rrect(x + w / 2 - 90, y + h / 2 - 14, 180, 28, 4, 0xE0303030);
        ui_text(FONT_BOLD, x + w / 2, y + h / 2 - 6, 0xFFFFFFFF, ALIGN_CENTER, "Joining...");
    }
    if (M.online) {
        char line[96];
        int listed = net_lobby_listed();
        if (!list_set()) {
            snprintf(line, sizeof(line), "Server list: none (R sets one)");
        } else if (M.list_ok < 0) {
            snprintf(line, sizeof(line), "Server list %s: address not found", M.list);
        } else if (st != NET_READY || M.list_ok == 0 || listed < 0) {
            snprintf(line, sizeof(line), "Server list %s: asking...", M.list);
        } else {
            snprintf(line, sizeof(line), "Server list %s: %d server%s", M.list, listed, listed == 1 ? "" : "s");
        }
        ui_text_clip(FONT_SMALL, x + 12, y + h - 46, w - 24, 0xFF707070, line);
    }
    int can_join = nrows > 0 && rows[M.host_sel] >= 0;
    ui_rrect(x + w - 112, y + h - 26, 104, 20, 3, can_join ? 0xFF2AA8F0 : 0xFFB0B0B0);
    ui_text(FONT_BOLD, x + w - 60, y + h - 22, 0xFFFFFFFF, ALIGN_CENTER, "X  Join");
    ui_text(FONT_SMALL, x + 10, y + h - 21, 0xFF606060, ALIGN_LEFT,
            M.online ? "Triangle: add   Square: remove   R: list   O: back" : "Up/Down: pick a game    O: back");
}

void menu_render(float fps) {
    gfx_begin_frame(BG_ABGR);
    if (M.bg_loaded) {
        // Slow orbit around the map's spawn point
        const PmapSpawn *sp = &M.bg.spawns[0];
        float a = M.t * 0.08f;
        if (M.bg.num_spawns >= 4) {
            // The burger joint's fourth spawn is the camera's: a slow look round the dining room from there
            const PmapSpawn *c = &M.bg.spawns[3];
            M.cam.pos.x = c->x;
            M.cam.pos.y = c->y;
            M.cam.pos.z = c->z;
            M.cam.yaw = c->yaw + sinf(M.t * 0.15f) * 22.0f;
            M.cam.pitch = -8.0f;
        } else {
            M.cam.pos.x = sp->x + cosf(a) * 900.0f;
            M.cam.pos.y = sp->y + sinf(a) * 900.0f;
            M.cam.pos.z = sp->z + 260.0f;
            M.cam.yaw = a * (180.0f / 3.14159265f) + 180.0f;
            M.cam.pitch = -12.0f;
        }
        camera_apply(&M.cam);
        sky_draw(&M.cam);
        gfx_set_fog(M.bg.info->fog_start, M.bg.info->fog_end, M.bg.info->fog_abgr);
        MapDrawStats ds;
        map_draw(&M.bg, &M.cam, &ds);
        gfx_set_fog(0, 0, 0);
    }

    ui_begin();
    menu_draw_backdrop();
    if (M.page == PAGE_MAIN) {
        draw_logo();
        menu_draw_buttons(k_main_items, NUM_MAIN_ITEMS, M.sel, 112, 26);
    } else if (M.page == PAGE_MULTI) {
        draw_logo();
        ui_text_shadow(FONT_BOLD, 24, 94, 0xFFA0A0A0, ALIGN_LEFT, "MULTIPLAYER (UP TO 4)");
        menu_draw_buttons(k_multi_items, NUM_MULTI_ITEMS, M.multi_sel, 114, 22); // six items: a bit closer than the main menu
    } else if (M.page == PAGE_PROFILE) {
        draw_logo();
        profile_draw(150, 60, 300, 186);
    } else if (M.page == PAGE_WORKSHOP) {
        workshop_draw(40, 20, 400, 228);
    } else if (M.page == PAGE_ACHIEVEMENTS) {
        achievements_draw(40, 20, 400, 228);
    } else if (M.page == PAGE_JOIN) {
        draw_join();
    } else if (M.page == PAGE_NEWGAME) {
        draw_newgame();
    } else {
        draw_logo();
        options_draw(150, 60, 300, 186);
    }
    if (M.error[0]) {
        ui_rrect(16, 232, 448, 18, 3, 0xD02020A0);
        ui_text_clip(FONT_SMALL, 22, 235, 436, 0xFFFFFFFF, M.error);
    }
    ui_text(FONT_SMALL, 474, 256, 0xA0FFFFFF, ALIGN_RIGHT,
            mem_tier() == MEM_TIER_HIGH ? "Full detail (64MB)" : "Low detail (32MB)");
    ui_text(FONT_SMALL, 6, 256, 0xA0FFFFFF, ALIGN_LEFT, "v" GAME_VERSION);
    if (g_settings.show_fps) ui_textf(FONT_SMALL, 474, 4, 0xA0FFFFFF, ALIGN_RIGHT, "%.0f fps", fps);
    ui_end();
    gfx_end_frame();
    profile_frame(); // the on-screen keyboard draws itself over the frame
    gfx_present(1);
}

