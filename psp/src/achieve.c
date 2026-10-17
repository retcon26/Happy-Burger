#include "achieve.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "mem.h"
#include "settings.h"
#include "ui.h"

#define ACH_PATH "data/achievements.cfg"
#define ACH_TMP "data/achievements.tmp"
#define MAX_MAPS_SEEN 64
#define SAVE_EVERY 60.0f // seconds of play between saves of the playtime

typedef struct {
    const char *name, *desc;
    int goal; // for counted ones: playtime in minutes, props, mingebags, maps (0: done or not)
} AchDef;

static const AchDef k_ach[ACH_COUNT] = {
    {"Getting Started", "Play for 10 minutes", 10},
    {"Regular", "Play for an hour", 60},
    {"Addict", "Play for 10 hours", 600},
    {"Not Alone", "Play a game with another real person", 0},
    {"Full House", "Play a game with four players or more", 0},
    {"The Real Deal", "Play on a real PSP", 0},
    {"Old Faithful", "Play on a PSP-1000", 0},
    {"Slim and Bright", "Play on a PSP-2000 or PSP-3000", 0},
    {"On the Go", "Play on a PSP Go", 0},
    {"Creator", "Spawn 100 props", 100},
    {"Mingebag Hunter", "Kill 10 mingebags", 10},
    {"Explorer", "Play on 10 different maps", 10},
    {"Save Point", "Save a build", 0},
    {"Deja Vu", "Load a saved build on another map", 0},
    {"Online", "Join a dedicated server", 0},
    {"On the Air", "Host on Wi-Fi and have someone join", 0},
    {"Yes, I'm the real Retcon", "Play on a server as Retcon", 0},
};

enum { DEV_EMULATOR, DEV_PSP1000, DEV_PSPSLIM, DEV_PSPGO };
static const char *const k_dev_names[] = {"PPSSPP (emulator)", "PSP-1000", "PSP-2000 / 3000", "PSP Go"};

static struct {
    unsigned int unlocked; // bit per achievement
    float playtime;        // seconds, in games
    int props, minges;
    unsigned int maps[MAX_MAPS_SEEN];
    int num_maps;
    int device;
    float since_save;
    int queue[8], q_head, q_tail; // unlocked, waiting for the game's notification
    int sel, scroll;
    int loaded; // achieve_init ran: only then is there progress to save (the dedicated server never runs it)
} A;

static int exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

// What it's running on. The model can't be read in user mode, so it's worked out: PPSSPP keeps its
// settings on its memory stick (we only look at whether they're there); a PSP-1000 has 32MB; a
// PSP Go has internal storage (ef0:); anything else is a PSP-2000 or 3000.
static int detect_device(void) {
    if (exists("ms0:/PSP/SYSTEM/ppsspp.ini") || exists("ms0:/PSP/SYSTEM/controls.ini")) return DEV_EMULATOR;
    if (mem_tier() == MEM_TIER_LOW) return DEV_PSP1000;
    SceUID d = sceIoDopen("ef0:/");
    if (d >= 0) {
        sceIoDclose(d);
        return DEV_PSPGO;
    }
    return DEV_PSPSLIM;
}

static void unlock(int a) {
    if (a < 0 || a >= ACH_COUNT || (A.unlocked & (1u << a))) return;
    A.unlocked |= 1u << a;
    int next = (A.q_tail + 1) % 8;
    if (next != A.q_head) {
        A.queue[A.q_tail] = a;
        A.q_tail = next;
    }
    achieve_save();
}

static int progress(int a) {
    switch (a) {
    case ACH_PLAY_10M:
    case ACH_PLAY_1H:
    case ACH_PLAY_10H: return (int)(A.playtime / 60.0f);
    case ACH_CREATOR: return A.props;
    case ACH_MINGE_HUNTER: return A.minges;
    case ACH_EXPLORER: return A.num_maps;
    default: return 0;
    }
}

static void check_counts(void) {
    for (int a = 0; a < ACH_COUNT; a++) {
        if (k_ach[a].goal > 0 && progress(a) >= k_ach[a].goal) unlock(a);
    }
}

void achieve_init(void) {
    memset(&A, 0, sizeof(A));
    FILE *f = fopen(ACH_PATH, "r");
    if (!f) f = fopen(ACH_TMP, "r"); // switched off between achieve_save's remove and rename
    if (f) {
        char key[32];
        unsigned int v;
        while (fscanf(f, "%31s %u", key, &v) == 2) {
            if (strcmp(key, "unlocked") == 0) A.unlocked = v;
            if (strcmp(key, "playtime") == 0) A.playtime = (float)v;
            if (strcmp(key, "props") == 0) A.props = (int)v;
            if (strcmp(key, "mingebags") == 0) A.minges = (int)v;
            if (strcmp(key, "map") == 0 && A.num_maps < MAX_MAPS_SEEN) A.maps[A.num_maps++] = v;
        }
        fclose(f);
    }
    A.device = detect_device();
    A.loaded = 1;
}

// Written to a temporary file, then swapped in: the playtime saves every minute, and a PSP switched off (or
// PPSSPP closed) halfway through writing the file itself would leave it empty
void achieve_save(void) {
    if (!A.loaded) return; // never over someone's progress with nothing
    FILE *f = fopen(ACH_TMP, "w");
    if (!f) return;
    fprintf(f, "unlocked %u\nplaytime %u\nprops %d\nmingebags %d\n", A.unlocked, (unsigned int)A.playtime, A.props, A.minges);
    for (int i = 0; i < A.num_maps; i++) fprintf(f, "map %u\n", A.maps[i]);
    int ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (ok) {
        remove(ACH_PATH);
        rename(ACH_TMP, ACH_PATH);
    }
    A.since_save = 0.0f;
}

void achieve_update(float dt, int players) {
    A.playtime += dt;
    A.since_save += dt;
    // Running on real hardware counts once you're actually playing
    if (A.device != DEV_EMULATOR) {
        unlock(ACH_REAL_PSP);
        unlock(A.device == DEV_PSP1000 ? ACH_PSP1000 : A.device == DEV_PSPGO ? ACH_PSPGO : ACH_PSPSLIM);
    }
    if (players >= 2) unlock(ACH_NOT_ALONE);
    if (players >= 4) unlock(ACH_FULL_HOUSE);
    check_counts();
    if (A.since_save >= SAVE_EVERY) achieve_save();
}

void achieve_unlock(int a) {
    unlock(a);
}

void achieve_prop_spawned(void) {
    A.props++;
    check_counts();
}

void achieve_minge_killed(void) {
    A.minges++;
    check_counts();
}

void achieve_map(const char *name) {
    unsigned int h = 2166136261u; // FNV-1a of the map name
    for (const char *c = name; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    for (int i = 0; i < A.num_maps; i++) {
        if (A.maps[i] == h) return;
    }
    if (A.num_maps < MAX_MAPS_SEEN) A.maps[A.num_maps++] = h;
    check_counts();
    achieve_save();
}

int achieve_pop(void) {
    if (A.q_head == A.q_tail) return -1;
    int a = A.queue[A.q_head];
    A.q_head = (A.q_head + 1) % 8;
    return a;
}

const char *achieve_name(int a) {
    return a >= 0 && a < ACH_COUNT ? k_ach[a].name : "";
}

const char *achieve_device(void) {
    return k_dev_names[A.device];
}

// --- Main menu > Achievements ---------------------------------------------------------------------

#define ROW_H 30
#define C_GOLD 0xFF30B8F0

int achievements_update(const InputState *in) {
    int moved = 0;
    if (in->pressed & PSP_CTRL_UP) A.sel = (A.sel + ACH_COUNT - 1) % ACH_COUNT, moved = 1;
    if (in->pressed & PSP_CTRL_DOWN) A.sel = (A.sel + 1) % ACH_COUNT, moved = 1;
    if (moved && g_settings.sound) snd_play(snd_find("menu_move"), 0.4f, 0.0f);
    return (in->pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) != 0;
}

void achievements_draw(float x, float y, float w, float h) {
    ui_rrect(x, y, w, h, 4, 0xF0E8E8E8);
    ui_gradient(x, y, w, 22, 0xFF8C6A3D, 0xFF6E5230);
    int got = 0;
    for (int a = 0; a < ACH_COUNT; a++) got += (A.unlocked >> a) & 1;
    ui_text(FONT_BOLD, x + 8, y + 5, 0xFFFFFFFF, ALIGN_LEFT, "Achievements");
    ui_textf(FONT_SMALL, x + w - 8, y + 6, 0xFFE0E0E0, ALIGN_RIGHT, "%d / %d unlocked", got, ACH_COUNT);
    float top = y + 28;
    int rows = (int)((h - 50) / ROW_H);
    if (A.sel < A.scroll) A.scroll = A.sel;
    if (A.sel >= A.scroll + rows) A.scroll = A.sel - rows + 1;
    for (int r = 0; r < rows && A.scroll + r < ACH_COUNT; r++) {
        int a = A.scroll + r;
        const AchDef *d = &k_ach[a];
        int done = (A.unlocked >> a) & 1, sel = a == A.sel;
        float ry = top + r * ROW_H;
        if (sel) ui_rrect(x + 5, ry - 1, w - 16, ROW_H - 2, 3, 0xFFE6A03D);
        // A medal: gold when unlocked, grey when not
        ui_rrect(x + 10, ry + 3, 22, 22, 11, done ? C_GOLD : 0xFFA8A8A8);
        ui_rrect(x + 14, ry + 7, 14, 14, 7, done ? 0xFF60D8FF : 0xFFC8C8C8);
        unsigned int tc = sel ? 0xFFFFFFFF : done ? 0xFF303030 : 0xFF707070;
        ui_text(FONT_BOLD, x + 40, ry + 1, tc, ALIGN_LEFT, d->name);
        ui_text_clip(FONT_SMALL, x + 40, ry + 15, w - 140, sel ? 0xFFF0F0F0 : 0xFF707070, d->desc);
        if (done) {
            ui_text(FONT_SMALL, x + w - 18, ry + 8, sel ? 0xFFFFFFFF : 0xFF40A040, ALIGN_RIGHT, "Unlocked");
        } else if (d->goal > 0) {
            int p = progress(a);
            ui_textf(FONT_SMALL, x + w - 18, ry + 2, tc, ALIGN_RIGHT, a <= ACH_PLAY_10H ? "%d / %d min" : "%d / %d", p, d->goal);
            float bw = 70.0f, fill = bw * (float)(p < d->goal ? p : d->goal) / (float)d->goal;
            ui_rect(x + w - 18 - bw, ry + 18, bw, 4, 0xFFC0C0C0);
            ui_rect(x + w - 18 - bw, ry + 18, fill, 4, C_GOLD);
        }
    }
    if (rows < ACH_COUNT) {
        float track = rows * ROW_H - 4, bar = track * rows / ACH_COUNT;
        ui_rect(x + w - 8, top, 3, track, 0xFFD0D0D0);
        ui_rect(x + w - 8, top + (track - bar) * A.scroll / (ACH_COUNT - rows), 3, bar, 0xFF909090);
    }
    unsigned int mins = (unsigned int)(A.playtime / 60.0f);
    ui_textf(FONT_SMALL, x + 10, y + h - 18, 0xFF606060, ALIGN_LEFT, "Playing on: %s    Played: %uh %02um    O: back",
             achieve_device(), mins / 60, mins % 60);
}
