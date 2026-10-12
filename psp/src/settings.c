#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "ui.h"

#define SETTINGS_PATH "data/settings.cfg"

Settings g_settings = {1, 3, 0, 1, 0, 1, 1, 1, 4, 3, 0, 0, 0, 1, 0, 0, 1};

// Values with names show the name; other multi-step options show dots; 0/1 options a checkbox
static const char *const k_dist_names[] = {"Near", "Medium", "Far", "Full"};
static const char *const k_quality_names[] = {"Low", "Medium", "High"};

typedef struct {
    const char *key;
    const char *label;
    int *value;
    int min, max;
    const char *const *names; // per value from min, or NULL (k_model_names: the player model list)
} Option;

static const char *const k_model_names[1] = {NULL}; // marks Player model, whose names come from the game

static Option k_options[] = {
    {"sound", "Sound", &g_settings.sound, 0, 1, NULL},
    {"look_speed", "Look speed", &g_settings.look_speed, 1, 5, NULL},
    {"invert_y", "Invert look up/down", &g_settings.invert_y, 0, 1, NULL},
    {"god_mode", "God mode (also games you host)", &g_settings.god_mode, 0, 1, NULL},
    {"player_model", "Player model", &g_settings.player_model, 0, 255, k_model_names},
    {"third_person", "Third-person view", &g_settings.third_person, 0, 1, NULL},
    {"npc_think", "NPC thinking", &g_settings.npc_think, 0, 1, NULL},
    {"npc_ignore_players", "NPCs ignore players", &g_settings.npc_ignore_players, 0, 1, NULL},
    {"unlimited_ammo", "Unlimited ammo", &g_settings.unlimited_ammo, 0, 1, NULL},
    {"hev_suit", "HEV suit", &g_settings.hev_suit, 0, 1, NULL},
    {"show_hud", "Show HUD", &g_settings.show_hud, 0, 1, NULL},
    {"crosshair", "Crosshair", &g_settings.crosshair, 0, 1, NULL},
    {"show_fps", "Show FPS", &g_settings.show_fps, 0, 1, NULL},
    {"draw_dist", "Draw distance", &g_settings.draw_dist, 1, 4, k_dist_names},
    {"physics", "Physics quality", &g_settings.physics, 1, 3, k_quality_names},
#ifdef PS2
    {"vu1", "VU1 renderer (faster)", &g_settings.vu1, 0, 1, NULL},
#endif
    {"debug", "Debug overlay", &g_settings.debug, 0, 1, NULL},
    {NULL, "Credits", NULL, 0, 0, NULL}, // a button: opens the credits
};
#define NUM_OPTIONS (int)(sizeof(k_options) / sizeof(k_options[0]))
#define ROW_H 22

static int g_sel, g_scroll;

static int g_pm_count;
static const char *(*g_pm_name)(int index);

void settings_set_player_models(int count, const char *(*name)(int index)) {
    g_pm_count = count;
    g_pm_name = name;
    if (g_settings.player_model >= count) g_settings.player_model = 0;
}

static const char *model_name(int v) {
    return g_pm_count > 0 && v >= 0 && v < g_pm_count ? g_pm_name(v) : "Citizen";
}

// --- Credits ----------------------------------------------------------------------------------
// They roll up the Options window like a film's, starting over at the end.

typedef struct {
    int font; // -1: a gap
    unsigned int colour;
    const char *text;
} CreditLine;

#define C_HEAD 0xFF6E5230
#define C_TEXT 0xFF303030
#define C_NAME 0xFFE6A03D
#define CREDITS_GAP 10.0f
#define CREDITS_FADE 12.0f  // lines fade in and out over this many pixels at the edges
#define CREDITS_SPEED 0.5f  // pixels per frame while rolling
#define CREDITS_WAIT 120    // frames before they start rolling

static const CreditLine k_credits[] = {
#ifdef PS2
    {FONT_LARGE, C_TEXT, "PS2GMOD"},
    {FONT_SMALL, C_TEXT, "A Garry's Mod-style sandbox for the PS2"},
#else
    {FONT_LARGE, C_TEXT, "PSP Sandbox"},
    {FONT_SMALL, C_TEXT, "A Garry's Mod-style sandbox for the PSP"},
#endif
    {-1, 0, NULL},
    {FONT_BOLD, C_HEAD, "Created by"},
    {FONT_LARGE, C_NAME, "Retcon"},
    {-1, 0, NULL},
    {FONT_BOLD, C_HEAD, "Garry's Mod"},
    {FONT_SMALL, C_TEXT, "Garry Newman"},
    {FONT_SMALL, C_TEXT, "Facepunch Studios"},
    {-1, 0, NULL},
    {FONT_BOLD, C_HEAD, "Source engine and Half-Life 2"},
    {FONT_SMALL, C_TEXT, "Valve"},
    {-1, 0, NULL},
    {FONT_BOLD, C_HEAD, "Maps, models, textures and sounds"},
    {FONT_SMALL, C_NAME, "Valve"},
    {FONT_SMALL, C_TEXT, "Half-Life"},
    {FONT_SMALL, C_TEXT, "Half-Life 2"},
    {FONT_SMALL, C_TEXT, "Half-Life 2: Episode One"},
    {FONT_SMALL, C_TEXT, "Half-Life 2: Episode Two"},
    {FONT_SMALL, C_TEXT, "Half-Life 2: Deathmatch"},
    {FONT_SMALL, C_TEXT, "Half-Life: Source"},
    {FONT_SMALL, C_TEXT, "Half-Life Deathmatch: Source"},
    {FONT_SMALL, C_TEXT, "Counter-Strike: Source"},
    {FONT_SMALL, C_TEXT, "Day of Defeat: Source"},
    {FONT_SMALL, C_TEXT, "Portal"},
    {FONT_SMALL, C_TEXT, "Portal 2"},
    {FONT_SMALL, C_TEXT, "Left 4 Dead"},
    {FONT_SMALL, C_TEXT, "Left 4 Dead 2"},
    {-1, 0, NULL},
    {FONT_SMALL, C_NAME, "Facepunch Studios"},
    {FONT_SMALL, C_TEXT, "Garry's Mod"},
    {-1, 0, NULL},
    {FONT_SMALL, C_NAME, "Crowbar Collective"},
    {FONT_SMALL, C_TEXT, "Black Mesa"},
    {-1, 0, NULL},
    {FONT_SMALL, C_NAME, "Fistful of Frags team"},
    {FONT_SMALL, C_TEXT, "Fistful of Frags"},
    {-1, 0, NULL},
    {FONT_BOLD, C_HEAD, "Built with"},
#ifdef PS2
    {FONT_SMALL, C_TEXT, "PS2SDK and gsKit - the ps2dev team"},
#else
    {FONT_SMALL, C_TEXT, "PSPSDK - the pspdev team"},
#endif
    {FONT_SMALL, C_TEXT, "Tahoma typeface - Matthew Carter"},
    {-1, 0, NULL},
    {FONT_SMALL, C_TEXT, "A fan project, not affiliated with or"},
    {FONT_SMALL, C_TEXT, "endorsed by Valve, Facepunch Studios,"},
    {FONT_SMALL, C_TEXT, "Crowbar Collective or the Fistful of"},
    {FONT_SMALL, C_TEXT, "Frags team. Their games are theirs."},
};
#define NUM_CREDITS (int)(sizeof(k_credits) / sizeof(k_credits[0]))

static int g_credits;      // showing the credits
static float g_credit_pos; // how far they've rolled, in pixels
static int g_credit_wait;  // frames left before they roll

static float credit_height(const CreditLine *c) {
    return c->font < 0 ? CREDITS_GAP : (float)ui_line_height(c->font) + 2.0f;
}

static void credits_update(const InputState *in) {
    if (in->pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS | PSP_CTRL_START)) {
        g_credits = 0;
        if (g_settings.sound) snd_play(snd_find("menu_move"), 0.4f, 0.0f);
        return;
    }
    // Down speeds them up, Up winds them back; otherwise they roll by themselves
    if (in->held & PSP_CTRL_DOWN) {
        g_credit_pos += 4.0f;
        g_credit_wait = 0;
    } else if (in->held & PSP_CTRL_UP) {
        g_credit_pos -= 4.0f;
        g_credit_wait = 0;
    } else if (g_credit_wait > 0) {
        g_credit_wait--;
    } else {
        g_credit_pos += CREDITS_SPEED;
    }
}

static void credits_draw(float x, float y, float w, float h) {
    ui_rrect(x, y, w, h, 4, 0xF0E8E8E8);
    ui_gradient(x, y, w, 22, 0xFF8C6A3D, 0xFF6E5230);
    ui_text(FONT_BOLD, x + 8, y + 5, 0xFFFFFFFF, ALIGN_LEFT, "Credits");
    float top = y + 28, bottom = y + h - 24, view = bottom - top;
    float total = 0.0f;
    for (int i = 0; i < NUM_CREDITS; i++) total += credit_height(&k_credits[i]);
    // Past the end they come back up from the bottom
    if (g_credit_pos > total) g_credit_pos = -view;
    if (g_credit_pos < -view) g_credit_pos = -view;
    // There's no clipping: a line is only drawn while it's wholly inside, fading near the edges
    float ly = top - g_credit_pos;
    for (int i = 0; i < NUM_CREDITS; i++) {
        const CreditLine *c = &k_credits[i];
        float lh = credit_height(c);
        if (c->font >= 0 && ly >= top && ly + lh <= bottom) {
            float edge = fminf(ly - top, bottom - (ly + lh));
            unsigned int a = edge >= CREDITS_FADE ? 255u : (unsigned int)(255.0f * edge / CREDITS_FADE);
            ui_text(c->font, x + w * 0.5f, ly, (a << 24) | (c->colour & 0xFFFFFFu), ALIGN_CENTER, c->text);
        }
        ly += lh;
    }
    ui_text(FONT_SMALL, x + 10, y + h - 18, 0xFF606060, ALIGN_LEFT, "Up / Down: scroll      O: back");
}

void settings_load(void) {
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char key[32];
    int v;
    while (fscanf(f, "%31s %d", key, &v) == 2) {
        for (int i = 0; i < NUM_OPTIONS; i++) {
            if (k_options[i].value && strcmp(key, k_options[i].key) == 0 && v >= k_options[i].min && v <= k_options[i].max) {
                *k_options[i].value = v;
            }
        }
    }
    fclose(f);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    for (int i = 0; i < NUM_OPTIONS; i++) {
        if (k_options[i].value) fprintf(f, "%s %d\n", k_options[i].key, *k_options[i].value);
    }
    fclose(f);
}

int options_update(const InputState *in) {
    if (g_credits) {
        credits_update(in);
        return 0;
    }
    int click = snd_find("menu_move");
    if (in->pressed & PSP_CTRL_UP) g_sel = (g_sel + NUM_OPTIONS - 1) % NUM_OPTIONS;
    if (in->pressed & PSP_CTRL_DOWN) g_sel = (g_sel + 1) % NUM_OPTIONS;
    Option *o = &k_options[g_sel];
    int changed = (in->pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) != 0;
    if (!o->value) {
        // The Credits row
        if (in->pressed & PSP_CTRL_CROSS) {
            g_credits = 1;
            g_credit_pos = -CREDITS_FADE;
            g_credit_wait = CREDITS_WAIT;
            changed = 1;
        }
    } else if (o->names == k_model_names) {
        // Player model: steps round the list
        int before = *o->value;
        if (g_pm_count > 0 && (in->pressed & (PSP_CTRL_RIGHT | PSP_CTRL_CROSS))) *o->value = (*o->value + 1) % g_pm_count;
        if (g_pm_count > 0 && (in->pressed & PSP_CTRL_LEFT)) *o->value = (*o->value + g_pm_count - 1) % g_pm_count;
        if (*o->value != before) changed = 1;
    } else {
        int before = *o->value;
        if (in->pressed & (PSP_CTRL_RIGHT | PSP_CTRL_CROSS)) {
            *o->value = *o->value >= o->max ? (o->max == 1 || o->names ? o->min : o->max) : *o->value + 1;
        }
        if (in->pressed & PSP_CTRL_LEFT) {
            *o->value = *o->value <= o->min ? (o->max == 1 || o->names ? o->max : o->min) : *o->value - 1;
        }
        if (*o->value != before) changed = 1;
    }
    if (changed && g_settings.sound) snd_play(click, 0.4f, 0.0f);
    if (in->pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) {
        settings_save();
        return 1;
    }
    return 0;
}

void options_draw(float x, float y, float w, float h) {
    if (g_credits) {
        credits_draw(x, y, w, h);
        return;
    }
    // A light Derma-style window
    ui_rrect(x, y, w, h, 4, 0xF0E8E8E8);
    ui_gradient(x, y, w, 22, 0xFF8C6A3D, 0xFF6E5230);
    ui_text(FONT_BOLD, x + 8, y + 5, 0xFFFFFFFF, ALIGN_LEFT, "Options");
    // More options than fit: the list scrolls to keep the selected one in view
    int rows = (int)((h - 50) / ROW_H);
    if (rows > NUM_OPTIONS) rows = NUM_OPTIONS;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + rows) g_scroll = g_sel - rows + 1;
    for (int r = 0; r < rows; r++) {
        int i = g_scroll + r;
        float ry = y + 30 + r * ROW_H;
        int selected = i == g_sel;
        if (selected) ui_rrect(x + 6, ry - 2, w - 18, 20, 3, 0xFFE6A03D);
        unsigned int tc = selected ? 0xFFFFFFFF : 0xFF303030;
        ui_text(FONT_SMALL, x + 14, ry + 2, tc, ALIGN_LEFT, k_options[i].label);
        const Option *o = &k_options[i];
        if (!o->value) {
            // A button
            ui_text(FONT_SMALL, x + w - 22, ry + 2, tc, ALIGN_RIGHT, ">");
        } else if (o->names == k_model_names) {
            // "< Dr. Kleiner >"
            const char *name = model_name(*o->value);
            ui_text(FONT_SMALL, x + w - 22, ry + 2, tc, ALIGN_RIGHT, name);
            if (selected) {
                ui_text(FONT_SMALL, x + w - 32 - ui_text_width(FONT_SMALL, name), ry + 2, tc, ALIGN_LEFT, "<");
                ui_text(FONT_SMALL, x + w - 16, ry + 2, tc, ALIGN_LEFT, ">");
            }
        } else if (o->names) {
            // Named values: "< Medium >"
            ui_text(FONT_SMALL, x + w - 22, ry + 2, tc, ALIGN_RIGHT, o->names[*o->value - o->min]);
            if (selected) {
                ui_text(FONT_SMALL, x + w - 98, ry + 2, tc, ALIGN_LEFT, "<");
                ui_text(FONT_SMALL, x + w - 16, ry + 2, tc, ALIGN_LEFT, ">");
            }
        } else if (o->max == 1) {
            // Checkbox
            float bx = x + w - 36, by = ry + 1;
            ui_rect(bx, by, 14, 14, selected ? 0xFFFFFFFF : 0xFF909090);
            ui_rect(bx + 1, by + 1, 12, 12, 0xFFFFFFFF);
            if (*o->value) ui_rect(bx + 3, by + 3, 8, 8, 0xFFE6A03D);
        } else {
            // Slider dots
            for (int k = o->min; k <= o->max; k++) {
                float dx = x + w - 36 - (o->max - k) * 12;
                ui_rrect(dx, ry + 4, 9, 9, 4.5f, k <= *o->value ? (selected ? 0xFFFFFFFF : 0xFFE6A03D) : 0xFFB0B0B0);
            }
        }
    }
    if (rows < NUM_OPTIONS) {
        float track = rows * ROW_H - 4, bar = track * rows / NUM_OPTIONS;
        ui_rect(x + w - 9, y + 28, 3, track, 0xFFD0D0D0);
        ui_rect(x + w - 9, y + 28 + (track - bar) * g_scroll / (NUM_OPTIONS - rows), 3, bar, 0xFF909090);
    }
    ui_text(FONT_SMALL, x + 10, y + h - 18, 0xFF606060, ALIGN_LEFT, "X / Left / Right: change      O: back");
}
