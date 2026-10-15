#include "workshop.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "npc.h"
#include "settings.h"
#include "ui.h"

#define WS_PATH "data/workshop.cfg"
#define MAX_OFF 512
#define MAX_ITEMS 320
#define ROW_H 15

static const char *const k_kind_keys[WS_KINDS] = {"map", "addon", "custom"};
static const char *const k_tab_names[WS_KINDS] = {"Maps", "Addons", "Custom"};

// What's switched off
static struct {
    int kind;
    char name[32];
} g_off[MAX_OFF];
static int g_num_off;

// The page: the items of each tab
static char g_items[WS_KINDS][MAX_ITEMS][32];
static int g_count[WS_KINDS];
static int g_tab, g_sel, g_scroll;

static int find_off(int kind, const char *name) {
    for (int i = 0; i < g_num_off; i++) {
        if (g_off[i].kind == kind && strcmp(g_off[i].name, name) == 0) return i;
    }
    return -1;
}

int workshop_enabled(int kind, const char *name) {
    return find_off(kind, name) < 0;
}

static void set_enabled(int kind, const char *name, int on) {
    int i = find_off(kind, name);
    if (on && i >= 0) {
        g_off[i] = g_off[--g_num_off];
    } else if (!on && i < 0 && g_num_off < MAX_OFF) {
        g_off[g_num_off].kind = kind;
        snprintf(g_off[g_num_off].name, sizeof(g_off[0].name), "%s", name);
        g_num_off++;
    }
}

void workshop_load(void) {
    g_num_off = 0;
    FILE *f = fopen(WS_PATH, "r");
    if (!f) return;
    char line[96], key[16], name[64];
    while (fgets(line, sizeof(line), f)) {
        // "off <kind> <name>" (names may have spaces: the rest of the line)
        int n = 0;
        if (sscanf(line, "off %15s %n", key, &n) != 1 || n <= 0) continue;
        snprintf(name, sizeof(name), "%s", line + n);
        name[strcspn(name, "\r\n")] = 0;
        for (int k = 0; k < WS_KINDS; k++) {
            if (strcmp(key, k_kind_keys[k]) == 0 && name[0]) set_enabled(k, name, 0);
        }
    }
    fclose(f);
}

static void save(void) {
    FILE *f = fopen(WS_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_num_off; i++) fprintf(f, "off %s %s\n", k_kind_keys[g_off[i].kind], g_off[i].name);
    fclose(f);
}

void workshop_open(char (*maps)[32], int num_maps) {
    g_count[WS_MAP] = 0;
    for (int i = 0; i < num_maps && i < MAX_ITEMS; i++) snprintf(g_items[WS_MAP][g_count[WS_MAP]++], 32, "%s", maps[i]);
    // Lua addons: the autorun scripts
    g_count[WS_ADDON] = 0;
    SceUID d = sceIoDopen("data/lua/autorun");
    if (d >= 0) {
        SceIoDirent ent;
        memset(&ent, 0, sizeof(ent));
        while (sceIoDread(d, &ent) > 0 && g_count[WS_ADDON] < MAX_ITEMS) {
            size_t n = strlen(ent.d_name);
            if (n > 4 && n < 32 && strcmp(ent.d_name + n - 4, ".lua") == 0) {
                snprintf(g_items[WS_ADDON][g_count[WS_ADDON]++], 32, "%s", ent.d_name);
            }
            memset(&ent, 0, sizeof(ent));
        }
        sceIoDclose(d);
    }
    // Custom: the Workshop's characters in the NPC pack
    g_count[WS_CUSTOM] = 0;
    for (int t = 0; t < npc_type_count() && g_count[WS_CUSTOM] < MAX_ITEMS; t++) {
        if (npc_type_workshop(t)) snprintf(g_items[WS_CUSTOM][g_count[WS_CUSTOM]++], 32, "%s", npc_type_name(t));
    }
    g_sel = g_scroll = 0;
}

int workshop_update(const InputState *in) {
    int click = 0, n = g_count[g_tab];
    if (in->pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
        g_tab = (g_tab + ((in->pressed & PSP_CTRL_RTRIGGER) ? 1 : WS_KINDS - 1)) % WS_KINDS;
        g_sel = g_scroll = 0;
        click = 1;
    } else if (n > 0) {
        if (in->pressed & PSP_CTRL_UP) g_sel = (g_sel + n - 1) % n, click = 1;
        if (in->pressed & PSP_CTRL_DOWN) g_sel = (g_sel + 1) % n, click = 1;
        // Left / Right: a page at a time through a long list
        if (in->pressed & PSP_CTRL_LEFT) g_sel = g_sel >= 10 ? g_sel - 10 : 0, click = 1;
        if (in->pressed & PSP_CTRL_RIGHT) g_sel = g_sel + 10 < n ? g_sel + 10 : n - 1, click = 1;
        if (in->pressed & PSP_CTRL_CROSS) {
            const char *name = g_items[g_tab][g_sel];
            set_enabled(g_tab, name, !workshop_enabled(g_tab, name));
            click = 1;
        }
        if (in->pressed & PSP_CTRL_TRIANGLE) {
            // Everything in this tab on, or (if it all is) off
            int all_on = 1;
            for (int i = 0; i < n; i++) all_on &= workshop_enabled(g_tab, g_items[g_tab][i]);
            for (int i = 0; i < n; i++) set_enabled(g_tab, g_items[g_tab][i], !all_on);
            click = 1;
        }
    }
    if (click && g_settings.sound) snd_play(snd_find("menu_move"), 0.4f, 0.0f);
    if (in->pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) {
        save();
        return 1;
    }
    return 0;
}

void workshop_draw(float x, float y, float w, float h) {
    ui_rrect(x, y, w, h, 4, 0xF0E8E8E8);
    ui_gradient(x, y, w, 22, 0xFF8C6A3D, 0xFF6E5230);
    ui_text(FONT_BOLD, x + 8, y + 5, 0xFFFFFFFF, ALIGN_LEFT, "Workshop");
    // Tabs
    float tx = x + 100;
    for (int k = 0; k < WS_KINDS; k++) {
        float tw = ui_text_width(FONT_SMALL, k_tab_names[k]) + 16;
        if (k == g_tab) ui_rrect(tx, y + 3, tw, 16, 3, 0xFFE6A03D);
        ui_text(FONT_SMALL, tx + 8, y + 5, k == g_tab ? 0xFFFFFFFF : 0xFFE0E0E0, ALIGN_LEFT, k_tab_names[k]);
        tx += tw + 4;
    }
    int n = g_count[g_tab], on = 0;
    for (int i = 0; i < n; i++) on += workshop_enabled(g_tab, g_items[g_tab][i]);
    ui_textf(FONT_SMALL, x + w - 8, y + 6, 0xFFE0E0E0, ALIGN_RIGHT, "%d / %d on", on, n);

    float top = y + 28;
    int rows = (int)((h - 52) / ROW_H);
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + rows) g_scroll = g_sel - rows + 1;
    if (n == 0) {
        ui_text(FONT_SMALL, x + 14, top + 4, 0xFF606060, ALIGN_LEFT,
                g_tab == WS_ADDON    ? "No addons: put .lua files in data/lua/autorun"
                : g_tab == WS_CUSTOM ? "Nothing from the Workshop: see tools/npcs.txt (game=workshop)"
                                     : "No maps");
    }
    for (int r = 0; r < rows && g_scroll + r < n; r++) {
        int i = g_scroll + r;
        float ry = top + r * ROW_H;
        int sel = i == g_sel, enabled = workshop_enabled(g_tab, g_items[g_tab][i]);
        if (sel) ui_rrect(x + 6, ry - 1, w - 18, ROW_H - 1, 3, 0xFFE6A03D);
        // A checkbox, then the name
        ui_rect(x + 12, ry + 1, 11, 11, sel ? 0xFFFFFFFF : 0xFF909090);
        ui_rect(x + 13, ry + 2, 9, 9, 0xFFFFFFFF);
        if (enabled) ui_rect(x + 15, ry + 4, 5, 5, 0xFF40A040);
        ui_text_clip(FONT_SMALL, x + 30, ry, w - 50, sel ? 0xFFFFFFFF : enabled ? 0xFF303030 : 0xFF909090,
                     g_items[g_tab][i]);
    }
    if (n > rows) {
        float track = rows * ROW_H - 2, bar = track * rows / n;
        ui_rect(x + w - 9, top, 3, track, 0xFFD0D0D0);
        ui_rect(x + w - 9, top + (track - bar) * g_scroll / (n - rows), 3, bar, 0xFF909090);
    }
    ui_text(FONT_SMALL, x + 10, y + h - 18, 0xFF606060, ALIGN_LEFT,
            "X: on/off   Triangle: all   L/R: tab   O: back");
}
