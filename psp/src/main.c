#include <pspkernel.h>
#include <psppower.h>
#include <pspsdk.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "achieve.h"
#include "workshop.h"
#include "audio.h"
#include "game.h"
#include "gfx.h"
#include "hud.h"
#include "input.h"
#include "map.h"
#include "mem.h"
#include "menu.h"
#include "net.h"
#include "physics.h"
#include "settings.h"
#include "ui.h"
#include "update.h"

PSP_MODULE_INFO("RetconsMod", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
// Take all user memory except 1MB, which the kernel needs for thread stacks and file buffers.
PSP_HEAP_SIZE_KB(-1024);

static volatile int g_running = 1;
static int g_moved_saves; // move_saves_across brought saves over from PSP Sandbox's folder

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return 0;
    FILE *b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return 0;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
    fclose(a);
    fclose(b);
    return 1;
}

// The release puts the game in PSP/GAME/RetconsMod (it was PSP/GAME/PSPSandbox). The first time it runs
// from a new folder, it copies your saves over from a PSPSandbox folder beside it: settings, achievements,
// Workshop choices, servers and saved builds. Nothing happens once it has settings of its own, and the old
// folder stays as it was. eboot: argv[0], "ms0:/PSP/GAME/RetconsMod/EBOOT.PBP".
static void move_saves_across(const char *eboot) {
    if (!eboot) return;
    const char *end = strrchr(eboot, '/'); // the game's folder ends here
    const char *start = end;
    while (start > eboot && start[-1] != '/') start--;
    if (!end || start == eboot || strncasecmp(start, "PSPSandbox", (size_t)(end - start)) == 0) return;
    FILE *own = fopen("data/settings.cfg", "rb");
    if (own) {
        fclose(own);
        return;
    }
    char old[256], from[320], to[96];
    snprintf(old, sizeof(old), "%.*sPSPSandbox/data", (int)(start - eboot), eboot);
    static const char *const files[] = {"settings.cfg", "achievements.cfg", "workshop.cfg", "servers.cfg"};
    for (int i = 0; i < 4; i++) {
        snprintf(from, sizeof(from), "%s/%s", old, files[i]);
        snprintf(to, sizeof(to), "data/%s", files[i]);
        g_moved_saves |= copy_file(from, to);
    }
    snprintf(from, sizeof(from), "%s/saves", old);
    SceUID d = sceIoDopen(from);
    if (d < 0) return;
    sceIoMkdir("data/saves", 0777);
    SceIoDirent ent;
    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(d, &ent) > 0) {
        if (ent.d_name[0] != '.' && FIO_S_ISREG(ent.d_stat.st_mode)) {
            snprintf(from, sizeof(from), "%.200s/saves/%.100s", old, ent.d_name);
            snprintf(to, sizeof(to), "data/saves/%.80s", ent.d_name);
            g_moved_saves |= copy_file(from, to);
        }
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(d);
}
static int g_ignore_input; // set by "lockinput" in autoload.txt, for unattended test runs

static int exit_callback(int arg1, int arg2, void *common) {
    g_running = 0;
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("ExitCallback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void) {
    int thid = sceKernelCreateThread("cb", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
}

// The blue loading screen, shown at boot and while a map (or the menu's background map) loads:
// the map's thumbnail and name, or the title when name is NULL.
#define LOADING_BLUE 0xFF8A3A12 // ABGR
static void draw_loading(const char *name) {
    gfx_begin_frame(LOADING_BLUE);
    if (ui_ready()) {
        ui_begin();
        ui_rect(0, 0, 480, 136, 0x30FFFFFF); // lighter top half, a cheap gradient
        ui_rect(0, 236, 480, 36, 0x40000000);
        if (name) {
            if (ui_map_thumb(name, 240 - 64, 56, 128, 128)) ui_outline(240 - 65, 55, 130, 130, 0x80FFFFFF);
            ui_text_shadow(FONT_LARGE, 240, 196, 0xFFFFFFFF, ALIGN_CENTER, name);
        } else {
            ui_text_shadow(FONT_TITLE, 240, 100, 0xFFFFFFFF, ALIGN_CENTER, "Retcon's Mod");
            ui_text_shadow(FONT_SMALL, 240, 150, 0xFFE0E0E0, ALIGN_CENTER, "by Retcon");
        }
        ui_text_shadow(FONT_SMALL, 240, 246, 0xFFFFFFFF, ALIGN_CENTER, "Loading...");
        ui_end();
    } else if (name) {
        hud_begin();
        hud_print(2, 16, 0xFFFFFFFF, "Loading %s...", name);
        hud_draw();
    }
    gfx_end_frame();
    gfx_present(1);
}

static void open_menu(void) {
    draw_loading(NULL); // menu_open loads the background map
    menu_open();
}

static int start_game(const char *name, char *status, int status_len) {
    menu_close(); // free the background map first, the 32MB model needs the room
    draw_loading(name);
    if (game_start(name, status, status_len) == 0) return 1;
    open_menu();
    return 0;
}

// Dev shortcut: data/autoload.txt. The first line is "mapname [x y z yaw pitch]"; later lines are
// game_command() lines (spawn props, open menus...). It lets a test run skip the menus.
static int run_autoload(char *status, int status_len) {
    FILE *f = fopen("data/autoload.txt", "r");
    if (!f) {
        return 0;
    }
    char line[128];
    int started = 0;
    if (fgets(line, sizeof(line), f)) {
        char nm[32];
        float c[5];
        int n = sscanf(line, "%31s %f %f %f %f %f", nm, &c[0], &c[1], &c[2], &c[3], &c[4]);
        char page[32];
        if (strcmp(nm, "menu") == 0 && sscanf(line, "%*s %31s", page) == 1 && strcmp(page, "profile") == 0) {
            menu_show_profile(); // "menu profile": Multiplayer > Profile
        }
        if (strcmp(nm, "join") == 0 && sscanf(line, "%*s %31s", page) == 1) {
            menu_autojoin(page); // "join ADDRESS": Online Servers, joining that server
        }
        if (strcmp(nm, "menu") == 0 && sscanf(line, "%*s %31s", page) == 1 && strcmp(page, "online") == 0) {
            menu_show_online(); // Multiplayer > Online Servers
        }
        if (strcmp(nm, "menu") == 0 && sscanf(line, "%*s %31s", page) == 1 && strcmp(page, "multiplayer") == 0) {
            menu_show_multiplayer();
        }
        if (strcmp(nm, "menu") == 0 && sscanf(line, "%*s %31s", page) == 1 && strcmp(page, "workshop") == 0) {
            menu_show_workshop();
        }
        if (strcmp(nm, "menu") == 0 && sscanf(line, "%*s %31s", page) == 1 && strcmp(page, "achievements") == 0) {
            menu_show_achievements();
        }
        if (strcmp(nm, "menu") == 0 && sscanf(line, "%*s %31s", page) == 1 && strcmp(page, "updates") == 0) {
            menu_show_updates();
        }

        if (n >= 1 && strcmp(nm, "menu") != 0 && strcmp(nm, "join") != 0 && start_game(nm, status, status_len)) {

            started = 1;
            if (n == 6) game_set_view(c[0], c[1], c[2], c[3], c[4]);
        }
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "lockinput", 9) == 0) {
                g_ignore_input = 1;
            } else if (started) {
                game_command(line);
            } else {
                update_command(line); // the updates page's test lines
            }
        }
    }
    fclose(f);
    return started;
}

int main(int argc, char *argv[]) {
    // A real PSP traps on float exceptions (0/0, x/0, overflow) and kills the game, where PPSSPP
    // and the PS2 carry on with NaN/Inf. Ragdoll and collision maths can hit them in rare poses.
    pspSdkDisableFPUExceptions();
    setup_callbacks();
    scePowerSetClockFrequency(333, 333, 166);

    mem_init();
    input_init();
    gfx_init();
    draw_loading(NULL); // plain blue until ui.pak is in
    hud_init();
    move_saves_across(argc > 0 ? argv[0] : NULL);
    settings_load();
    update_init(argc > 0 ? argv[0] : NULL);
    settings_set_updates_page(update_page_open, update_page_update, update_page_draw);
    achieve_init();
    workshop_load();
    ui_load();
    draw_loading(NULL);
    audio_init();
    // Props and NPCs load now, before the menu's background map, so they sit at the bottom of the
    // heap for good and every map gets one unbroken block above them
    game_preload();

    char status[96] = "";
    open_menu();
    int in_game = run_autoload(status, sizeof(status));
    if (status[0]) {
        menu_set_error(status);
    } else if (g_moved_saves) {
        menu_set_error("Your saves came over from PSP/GAME/PSPSandbox");
    }

    InputState in;
    SceInt64 t_prev = sceKernelGetSystemTimeWide();
    // FPS: a smoothed frame time, so the number settles quickly and isn't dragged down by map loads
    SceInt64 t_shown = t_prev;
    float frame_avg = 1.0f / 60.0f;
    float fps = 60.0f;

    while (g_running) {
        SceInt64 now = sceKernelGetSystemTimeWide();
        float dt = (float)(now - t_prev) / 1000000.0f;
        if (dt > 0.0f && dt < 0.5f) frame_avg += (dt - frame_avg) * 0.1f; // a loading stall isn't a frame
        if (now - t_shown >= 250000) {
            fps = 1.0f / frame_avg;
            t_shown = now;
        }
        if (dt > 0.1f) {
            dt = 0.1f; // Avoid a huge step after a stall (HOME menu, loading).
        }
        t_prev = now;
        update_set_in_game(in_game);
        int target = settings_target_fps(); // Options > Target FPS
        gfx_set_frame_rate(target);
        phys_set_frame_rate(target);
        input_update(&in);
        if (g_ignore_input) {
            memset(&in, 0, sizeof(in));
        }

        if (in_game) {
            GameResult r = game_update(&in, dt);
            if (r == GAME_QUIT) {
                break;
            }
            if (r == GAME_EXIT_TO_MENU) {
                char why[64];
                snprintf(why, sizeof(why), "%s", game_exit_message());
                game_end();
                in_game = 0;
                open_menu();
                if (why[0]) menu_set_error(why);
                char rejoin[48];
                if (game_rejoin_address(rejoin, sizeof(rejoin))) menu_autojoin(rejoin); // the server changed map
            } else {
                game_render(fps);
            }
        } else {
            char map[32];
            MenuResult r = menu_update(&in, dt, map, sizeof(map));
            if (r == MENU_QUIT) {
                break;
            }
            if (r == MENU_START_GAME || r == MENU_HOST_GAME || r == MENU_JOIN_GAME) {
                int slot = 0;
                unsigned char host_mac[6];
                if (r == MENU_JOIN_GAME) menu_join_info(&slot, host_mac);
                if (start_game(map, status, sizeof(status))) {
                    in_game = 1;
                    if (r == MENU_HOST_GAME) game_host();
                    if (r == MENU_JOIN_GAME) game_join(slot, host_mac);
                } else {
                    if (r == MENU_JOIN_GAME) {
                        net_stop(); // we don't have the host's map
                        snprintf(status, sizeof(status), "Can't join: you don't have the map %s", map);
                    }
                    menu_set_error(status);
                }
                t_prev = sceKernelGetSystemTimeWide();
                continue;
            }
            menu_render(fps);
        }
    }

    if (in_game) {
        game_end();
    }
    menu_close();
    audio_shutdown();
    gfx_shutdown();
    sceKernelExitGame();
    return 0;
}
