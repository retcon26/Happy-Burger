#include "menu.h"

#include <math.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#include "achieve.h"
#include "audio.h"
#include "camera.h"
#include "gfx.h"
#include "map.h"
#include "mem.h"
#include "net.h"
#include "settings.h"
#include "sky.h"
#include "ui.h"
#include "version.h"

#define MAX_MAPS 320
#define BG_ABGR 0xFF201810

typedef enum { PAGE_MAIN, PAGE_NEWGAME, PAGE_OPTIONS, PAGE_MULTI, PAGE_JOIN, PAGE_PROFILE, PAGE_ACHIEVEMENTS } Page;
#define MAX_HOSTS 8

// Map browser categories, by name prefix like GMod's New Game screen
static const struct {
    const char *name;
    const char *prefixes[12];
} k_cats[] = {
    {"Sandbox", {"gm_", NULL}},
    {"Counter-Strike", {"cs_", "de_", "as_", NULL}},
    {"Half-Life 2", {"d1_", "d2_", "d3_", NULL}},
    {"HL2 Deathmatch", {"dm_", NULL}},
    {"Half-Life", {"hl1_", NULL}},
    {"Day of Defeat", {"dod_", NULL}},
    {"Team Fortress 2", {"ctf_", "cp_", "pl_", "plr_", "koth_", "arena_", "pd_", "sd_", "tc_", "mvm_", "pass_", NULL}},
    {"Other", {NULL}},
};
#define NUM_CATS (int)(sizeof(k_cats) / sizeof(k_cats[0]))
#define GRID_COLS 3

static const char *k_main_items[] = {"Start New Game", "Multiplayer", "Achievements", "Options"};
#define NUM_MAIN_ITEMS (int)(sizeof(k_main_items) / sizeof(k_main_items[0]))
static const char *k_multi_items[] = {"Host Game", "Join Game", "Profile", "Back"};
#define NUM_MULTI_ITEMS 4

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
} M;

static int map_category(const char *name) {
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
        if (map_category(M.maps[i]) == cat) out[n++] = i;
    }
    return n;
}

static void click(int id, float vol) {
    if (g_settings.sound) snd_play(id, vol, 0.0f);
}

void menu_open(void) {
    M.page = PAGE_MAIN;
    M.sel = 0;
    M.num_maps = map_list(M.maps, MAX_MAPS);
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

void menu_show_achievements(void) {
    M.page = PAGE_ACHIEVEMENTS;
    M.sel = 2;
}

void menu_show_profile(void) {

    M.page = PAGE_PROFILE;
    M.multi_sel = 2;
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
                M.page = PAGE_NEWGAME;
                M.host = 0;
                M.map_sel = 0;
                M.scroll = 0;
            } else if (M.sel == 1) {
                M.page = PAGE_MULTI;
                M.multi_sel = 0;
            } else if (M.sel == 2) {
                M.page = PAGE_ACHIEVEMENTS;
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
        if (in->pressed & PSP_CTRL_CIRCLE || ((in->pressed & PSP_CTRL_CROSS) && M.multi_sel == 3)) {
            M.page = PAGE_MAIN;
            click(M.snd_move, 0.4f);
        } else if (in->pressed & PSP_CTRL_CROSS) {
            click(M.snd_select, 0.6f);
            M.error[0] = 0;
            if (M.multi_sel == 0) {
                M.page = PAGE_NEWGAME;
                M.host = 1;
                M.map_sel = 0;
                M.scroll = 0;
            } else if (M.multi_sel == 2) {
                M.page = PAGE_PROFILE;
            } else {
                M.page = PAGE_JOIN;
                M.num_hosts = 0;
                M.host_sel = 0;
                M.joining = 0;
                net_start();
            }
        }
        return MENU_STAY;
    }
    if (M.page == PAGE_JOIN) {
        int r = net_lobby_update(dt, M.hosts, MAX_HOSTS, &M.num_hosts);
        if (M.host_sel >= M.num_hosts) M.host_sel = M.num_hosts > 0 ? M.num_hosts - 1 : 0;
        if (r == 1) {
            net_lobby_result(map_out, map_out_len, &M.join_slot, M.join_mac);
            M.joining = 0;
            return MENU_JOIN_GAME; // the connection stays up for the game
        }
        if (r == -1) {
            M.joining = 0;
            menu_set_error("That game is full");
        }
        if ((in->pressed & PSP_CTRL_UP) && M.host_sel > 0) {
            M.host_sel--;
            click(M.snd_move, 0.4f);
        }
        if ((in->pressed & PSP_CTRL_DOWN) && M.host_sel + 1 < M.num_hosts) {
            M.host_sel++;
            click(M.snd_move, 0.4f);
        }
        if ((in->pressed & PSP_CTRL_CROSS) && M.num_hosts > 0 && !M.joining) {
            click(M.snd_select, 0.6f);
            M.error[0] = 0;
            M.joining = 1;
            net_lobby_join(&M.hosts[M.host_sel]);
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
            M.error[0] = 0;
            return M.host ? MENU_HOST_GAME : MENU_START_GAME;
        }
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

static void draw_logo(void) {
    int logo = ui_image_find("logo");
    if (logo >= 0) {
        int w, h;
        ui_image_size(logo, &w, &h);
        float s = 220.0f / (float)w;
        if (h * s > 56.0f) s = 56.0f / (float)h;
        ui_image(logo, 20, 18, w * s, h * s, 0xFFFFFFFF);
        ui_text_shadow(FONT_BOLD, 22, 20 + h * s + 2, 0xFFFFFFFF, ALIGN_LEFT, "for PSP");
    } else {
        ui_text_shadow(FONT_TITLE, 20, 20, 0xFFFFFFFF, ALIGN_LEFT, "PSP Sandbox");
    }
}

static void draw_newgame(void) {
    float x = 40, y = 20, w = 400, h = 232;
    ui_rrect(x - 2, y - 2, w + 4, h + 4, 6, 0x60000000);
    ui_rrect(x, y, w, h, 5, 0xF5E8E8E8);
    ui_gradient(x, y, w, 24, 0xFF8C6A3D, 0xFF6E5230);
    ui_text(FONT_BOLD, x + 10, y + 6, 0xFFFFFFFF, ALIGN_LEFT, M.host ? "Host Multiplayer Game (Ad-hoc)" : "Start New Game");

    // Categories down the left
    float cx = x + 6, cy = y + 30;
    int idx[MAX_MAPS];
    for (int c = 0; c < NUM_CATS; c++) {
        int n = maps_in_cat(c, idx);
        int sel = c == M.cat;
        if (sel) ui_rrect(cx, cy + c * 20 - 2, 108, 18, 3, 0xFFE6A03D);
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
        float tx = gx + c * (tile + gap), ty = gy + r * (tile + 12 + gap);
        int sel = k == M.map_sel;
        ui_rrect(tx - 2, ty - 2, tile + 4, tile + 4, 3, sel ? 0xFFE6A03D : 0xFFC8C8C8);
        if (!ui_map_thumb(M.maps[idx[k]], tx, ty, tile, tile)) {
            ui_gradient(tx, ty, tile, tile, 0xFF606060, 0xFF303030);
            ui_text(FONT_BOLD, tx + tile / 2, ty + tile / 2 - 6, 0xFFE0E0E0, ALIGN_CENTER, "?");
        }
        ui_rect(tx, ty + tile - 14, tile, 14, 0xA0000000);
        ui_text_clip(FONT_SMALL, tx + 3, ty + tile - 13, tile - 6, 0xFFFFFFFF, display_name(M.maps[idx[k]]));
    }
    int rows = (n + GRID_COLS - 1) / GRID_COLS;
    if (rows > 2) {
        float bar_h = 196.0f * 2 / rows;
        ui_rect(x + w - 8, gy, 3, 196, 0xFFC0C0C0);
        ui_rect(x + w - 8, gy + M.scroll * (196.0f - bar_h) / (rows - 2), 3, bar_h, 0xFF808080);
    }

    // Start button, GMod blue
    ui_rrect(x + w - 112, y + h - 26, 104, 20, 3, 0xFFE6A03D);
    ui_text(FONT_BOLD, x + w - 60, y + h - 22, 0xFFFFFFFF, ALIGN_CENTER, M.host ? "X  Host Game" : "X  Start Game");
    ui_text(FONT_SMALL, x + 10, y + h - 21, 0xFF606060, ALIGN_LEFT, "L/R: category    O: back");
}

// Join Game: the games nearby, from their hosts' beacons
static void draw_join(void) {
    float x = 40, y = 20, w = 400, h = 232;
    ui_rrect(x - 2, y - 2, w + 4, h + 4, 6, 0x60000000);
    ui_rrect(x, y, w, h, 5, 0xF5E8E8E8);
    ui_gradient(x, y, w, 24, 0xFF8C6A3D, 0xFF6E5230);
    ui_text(FONT_BOLD, x + 10, y + 6, 0xFFFFFFFF, ALIGN_LEFT, "Join Game (Ad-hoc)");
    ui_textf(FONT_SMALL, x + w - 10, y + 7, 0xFFE0E0E0, ALIGN_RIGHT, "You: %s", net_nickname());

    NetState st = net_poll();
    float ly = y + 32;
    if (st != NET_READY) {
        ui_text(FONT_SMALL, x + 14, ly + 4, st == NET_FAILED ? 0xFF2020C0 : 0xFF404040, ALIGN_LEFT,
                st == NET_FAILED ? net_status() : "Starting ad-hoc wireless...");
        if (st == NET_FAILED) {
            ui_text(FONT_SMALL, x + 14, ly + 22, 0xFF606060, ALIGN_LEFT, "Check the WLAN switch. In PPSSPP, enable networking and");
            ui_text(FONT_SMALL, x + 14, ly + 38, 0xFF606060, ALIGN_LEFT, "the built-in ad-hoc server in Settings > Networking.");
        }
    } else if (M.num_hosts == 0) {
        // A little spinner while we listen for beacons
        int dots = (int)(M.t * 3.0f) % 4;
        ui_textf(FONT_SMALL, x + 14, ly + 4, 0xFF404040, ALIGN_LEFT, "Looking for games%.*s", dots, "...");
        ui_text(FONT_SMALL, x + 14, ly + 22, 0xFF606060, ALIGN_LEFT, "Someone nearby has to pick Multiplayer > Host Game.");
    }
    for (int i = 0; i < M.num_hosts; i++) {
        const NetHost *hs = &M.hosts[i];
        float ry = ly + i * 30;
        int sel = i == M.host_sel;
        ui_rrect(x + 8, ry, w - 16, 26, 3, sel ? 0xFFE6A03D : 0xFFF8F8F8);
        ui_map_thumb(hs->map, x + 11, ry + 2, 22, 22);
        unsigned int tc = sel ? 0xFFFFFFFF : 0xFF303030;
        ui_text(FONT_BOLD, x + 40, ry + 6, tc, ALIGN_LEFT, hs->host);
        ui_text(FONT_SMALL, x + 180, ry + 7, tc, ALIGN_LEFT, hs->map);
        ui_textf(FONT_SMALL, x + w - 16, ry + 7, tc, ALIGN_RIGHT, "%d / %d", hs->players, hs->max);
    }
    if (M.joining) {
        ui_rrect(x + w / 2 - 90, y + h / 2 - 14, 180, 28, 4, 0xE0303030);
        ui_text(FONT_BOLD, x + w / 2, y + h / 2 - 6, 0xFFFFFFFF, ALIGN_CENTER, "Joining...");
    }
    ui_rrect(x + w - 112, y + h - 26, 104, 20, 3, M.num_hosts ? 0xFFE6A03D : 0xFFB0B0B0);
    ui_text(FONT_BOLD, x + w - 60, y + h - 22, 0xFFFFFFFF, ALIGN_CENTER, "X  Join");
    ui_text(FONT_SMALL, x + 10, y + h - 21, 0xFF606060, ALIGN_LEFT, "Up/Down: pick a game    O: back");
}

void menu_render(float fps) {
    gfx_begin_frame(BG_ABGR);
    if (M.bg_loaded) {
        // Slow orbit around the map's spawn point
        const PmapSpawn *sp = &M.bg.spawns[0];
        float a = M.t * 0.08f;
        M.cam.pos.x = sp->x + cosf(a) * 900.0f;
        M.cam.pos.y = sp->y + sinf(a) * 900.0f;
        M.cam.pos.z = sp->z + 260.0f;
        M.cam.yaw = a * (180.0f / 3.14159265f) + 180.0f;
        M.cam.pitch = -12.0f;
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
        ui_text_shadow(FONT_BOLD, 24, 94, 0xFFA0A0A0, ALIGN_LEFT, "MULTIPLAYER (AD-HOC, UP TO 4)");
        menu_draw_buttons(k_multi_items, NUM_MULTI_ITEMS, M.multi_sel, 112, 26);
    } else if (M.page == PAGE_PROFILE) {
        draw_logo();
        profile_draw(150, 60, 300, 186);
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

