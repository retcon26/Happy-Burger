#include "builds.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "osk.h"
#include "settings.h"
#include "ui.h"

#define SAVES_DIR "data/saves"
#define SAVES_EXT ".build"
#define MAGIC "PSPSANDBOX BUILD 1"
#define MAX_SAVES 64
#define ROW_H 17

static void build_path(char *out, int len, const char *name) {
    snprintf(out, len, SAVES_DIR "/%s" SAVES_EXT, name);
}

// --- The file: one line per thing, numbers first and names last (they can have spaces) ------------

int build_write(const char *name, const Build *b) {
    sceIoMkdir(SAVES_DIR, 0777);
    char path[96];
    build_path(path, sizeof(path), name);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, MAGIC "\n");
    fprintf(f, "map %s\n", b->map);
    fprintf(f, "origin %.2f %.2f %.2f %.2f\n", b->origin.x, b->origin.y, b->origin.z, b->yaw);
    for (int i = 0; i < b->num_props; i++) {
        const BuildProp *p = &b->props[i];
        fprintf(f, "prop %08X %.2f %d %d %.2f %.2f %.2f %.5f %.5f %.5f %.5f %s|%s\n", p->tint, p->mass, p->flags,
                p->balloons, p->pos.x, p->pos.y, p->pos.z, p->rot.x, p->rot.y, p->rot.z, p->rot.w, p->cat, p->name);
    }
    for (int i = 0; i < b->num_joints; i++) {
        const Joint *j = &b->joints[i];
        fprintf(f,
                "joint %d %d %d %.3f %.3f %.3f %.3f %.3f %.3f %.5f %.5f %.5f %.5f %.3f %.5f %.5f %.5f %.5f %.5f %.5f "
                "%d %.4f %.3f %.3f %.3f\n",
                j->type, j->a, j->b, j->la.x, j->la.y, j->la.z, j->lb.x, j->lb.y, j->lb.z, j->rel.x, j->rel.y,
                j->rel.z, j->rel.w, j->length, j->axis.x, j->axis.y, j->axis.z, j->axis_a.x, j->axis_a.y,
                j->axis_a.z, j->on, j->motor, j->rate, j->len_min, j->len_max);
    }
    for (int i = 0; i < b->num_forces; i++) {
        const Force *c = &b->forces[i];
        fprintf(f, "force %d %d %d %.3f %.3f %.3f %.5f %.5f %.5f %.3f\n", c->type, c->body, c->on, c->lpos.x,
                c->lpos.y, c->lpos.z, c->ldir.x, c->ldir.y, c->ldir.z, c->strength);
    }
    for (int i = 0; i < b->num_devices; i++) {
        const BuildDevice *d = &b->devices[i];
        fprintf(f, "device %d %d %d %.3f %.3f %.3f %.5f %.5f %.5f %d %d %d\n", d->type, d->body, d->on, d->lpos.x,
                d->lpos.y, d->lpos.z, d->ldir.x, d->ldir.y, d->ldir.z, d->colour, d->effect, d->target);
    }
    int ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

// Just the header and how many props: enough for the list
static int build_peek(const char *name, char *map, int map_len, int *props) {
    char path[96], line[160];
    build_path(path, sizeof(path), name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fgets(line, sizeof(line), f) && strncmp(line, MAGIC, strlen(MAGIC)) == 0;
    map[0] = 0;
    *props = 0;
    while (ok && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "map ", 4) == 0) {
            snprintf(map, map_len, "%.31s", line + 4);
            map[strcspn(map, "\r\n")] = 0;
        } else if (strncmp(line, "prop ", 5) == 0) {
            (*props)++;
        }
    }
    fclose(f);
    return ok ? 0 : -1;
}

int build_read(const char *name, Build *b) {
    char path[96], line[320];
    build_path(path, sizeof(path), name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(b, 0, sizeof(*b));
    if (!fgets(line, sizeof(line), f) || strncmp(line, MAGIC, strlen(MAGIC)) != 0) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "map ", 4) == 0) {
            snprintf(b->map, sizeof(b->map), "%.31s", line + 4);
        } else if (strncmp(line, "origin ", 7) == 0) {
            sscanf(line + 7, "%f %f %f %f", &b->origin.x, &b->origin.y, &b->origin.z, &b->yaw);
        } else if (strncmp(line, "prop ", 5) == 0 && b->num_props < BUILD_MAX_PROPS) {
            BuildProp *p = &b->props[b->num_props];
            int n = 0;
            if (sscanf(line + 5, "%x %f %d %d %f %f %f %f %f %f %f %n", &p->tint, &p->mass, &p->flags, &p->balloons,
                       &p->pos.x, &p->pos.y, &p->pos.z, &p->rot.x, &p->rot.y, &p->rot.z, &p->rot.w, &n) < 11 ||
                n <= 0) {
                continue;
            }
            const char *rest = line + 5 + n, *bar = strchr(rest, '|');
            if (bar) {
                snprintf(p->cat, sizeof(p->cat), "%.*s", (int)(bar - rest), rest);
                snprintf(p->name, sizeof(p->name), "%s", bar + 1);
            } else {
                snprintf(p->name, sizeof(p->name), "%s", rest);
            }
            b->num_props++;
        } else if (strncmp(line, "joint ", 6) == 0 && b->num_joints < BUILD_MAX_JOINTS) {
            Joint *j = &b->joints[b->num_joints];
            memset(j, 0, sizeof(*j));
            if (sscanf(line + 6, "%d %d %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %d %f %f %f %f", &j->type,
                       &j->a, &j->b, &j->la.x, &j->la.y, &j->la.z, &j->lb.x, &j->lb.y, &j->lb.z, &j->rel.x, &j->rel.y,
                       &j->rel.z, &j->rel.w, &j->length, &j->axis.x, &j->axis.y, &j->axis.z, &j->axis_a.x,
                       &j->axis_a.y, &j->axis_a.z, &j->on, &j->motor, &j->rate, &j->len_min, &j->len_max) == 25) {
                j->active = 1;
                b->num_joints++;
            }
        } else if (strncmp(line, "force ", 6) == 0 && b->num_forces < BUILD_MAX_FORCES) {
            Force *c = &b->forces[b->num_forces];
            memset(c, 0, sizeof(*c));
            if (sscanf(line + 6, "%d %d %d %f %f %f %f %f %f %f", &c->type, &c->body, &c->on, &c->lpos.x, &c->lpos.y,
                       &c->lpos.z, &c->ldir.x, &c->ldir.y, &c->ldir.z, &c->strength) == 10) {
                c->active = 1;
                b->num_forces++;
            }
        } else if (strncmp(line, "device ", 7) == 0 && b->num_devices < BUILD_MAX_DEVICES) {
            BuildDevice *d = &b->devices[b->num_devices];
            if (sscanf(line + 7, "%d %d %d %f %f %f %f %f %f %d %d %d", &d->type, &d->body, &d->on, &d->lpos.x,
                       &d->lpos.y, &d->lpos.z, &d->ldir.x, &d->ldir.y, &d->ldir.z, &d->colour, &d->effect,
                       &d->target) == 12) {
                b->num_devices++;
            }
        }
    }
    fclose(f);
    // Anything pointing past the props is dropped (a hand-edited or cut-short file)
    int k = 0;
    for (int i = 0; i < b->num_joints; i++) {
        const Joint *j = &b->joints[i];
        if (j->a >= 0 && j->a < b->num_props && j->b >= -1 && j->b < b->num_props) b->joints[k++] = *j;
    }
    b->num_joints = k;
    k = 0;
    for (int i = 0; i < b->num_forces; i++) {
        if (b->forces[i].body >= 0 && b->forces[i].body < b->num_props) b->forces[k++] = b->forces[i];
    }
    b->num_forces = k;
    k = 0;
    for (int i = 0; i < b->num_devices; i++) {
        if (b->devices[i].body >= 0 && b->devices[i].body < b->num_props) b->devices[k++] = b->devices[i];
    }
    b->num_devices = k;
    return b->num_props > 0 ? 0 : -1;
}

// --- The Saves page -------------------------------------------------------------------------------

static struct {
    char name[BUILD_NAME_LEN];
    char map[32];
    int props;
} g_saves[MAX_SAVES];
static int g_num_saves;
static int g_sel, g_scroll; // row 0 is "Save the current build", then the saves
static int g_confirm = -1;  // the save Triangle was pressed on once (press again to delete)
static char g_map[32];
static char g_msg[48];
static int g_naming;

static int cmp_saves(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

void saves_refresh(void) {
    g_num_saves = 0;
    SceUID d = sceIoDopen(SAVES_DIR);
    if (d >= 0) {
        SceIoDirent ent;
        memset(&ent, 0, sizeof(ent));
        int ext = (int)strlen(SAVES_EXT);
        while (sceIoDread(d, &ent) > 0 && g_num_saves < MAX_SAVES) {
            int n = (int)strlen(ent.d_name);
            if (n > ext && n - ext < BUILD_NAME_LEN && strcmp(ent.d_name + n - ext, SAVES_EXT) == 0) {
                snprintf(g_saves[g_num_saves].name, BUILD_NAME_LEN, "%.*s", n - ext, ent.d_name);
                if (build_peek(g_saves[g_num_saves].name, g_saves[g_num_saves].map, 32, &g_saves[g_num_saves].props) == 0) {
                    g_num_saves++;
                }
            }
            memset(&ent, 0, sizeof(ent));
        }
        sceIoDclose(d);
    }
    qsort(g_saves, g_num_saves, sizeof(g_saves[0]), cmp_saves);
    if (g_sel > g_num_saves) g_sel = g_num_saves;
}

void saves_open(const char *map) {
    snprintf(g_map, sizeof(g_map), "%s", map);
    g_sel = g_scroll = 0;
    g_confirm = -1;
    g_msg[0] = 0;
    g_naming = 0;
    saves_refresh();
}

static int find_save(const char *name) {
    for (int i = 0; i < g_num_saves; i++) {
        if (strcmp(g_saves[i].name, name) == 0) return i;
    }
    return -1;
}

// A file name from what was typed: letters, digits, spaces, - and _
static void clean_name(char *s) {
    char *w = s;
    for (const char *r = s; *r; r++) {
        char c = *r;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                 c == '_';
        if (ok && !(c == ' ' && w == s)) *w++ = c;
    }
    *w = 0;
    while (w > s && w[-1] == ' ') *--w = 0;
}

// A name nothing has yet: "Build 1", "Build 2"...
static void next_free_name(char *out, int len) {
    for (int n = 1; n < 1000; n++) {
        snprintf(out, len, "Build %d", n);
        if (find_save(out) < 0) return;
    }
}

int saves_update(const InputState *in, char *name, int name_len) {
    int click = 0;
    if (g_naming) {
        if (osk_active()) return SAVES_NONE;
        char typed[BUILD_NAME_LEN];
        g_naming = 0;
        if (!osk_result(typed, sizeof(typed))) return SAVES_NONE; // cancelled
        clean_name(typed);
        if (!typed[0]) {
            snprintf(g_msg, sizeof(g_msg), "That name can't be used");
            return SAVES_NONE;
        }
        snprintf(name, name_len, "%s", typed);
        return SAVES_SAVE;
    }
    int rows = g_num_saves + 1;
    if (in->pressed & PSP_CTRL_UP) {
        g_sel = (g_sel + rows - 1) % rows;
        click = 1;
    }
    if (in->pressed & PSP_CTRL_DOWN) {
        g_sel = (g_sel + 1) % rows;
        click = 1;
    }
    if (click) g_confirm = -1;
    int result = SAVES_NONE;
    const int s = g_sel - 1; // the save the cursor's on, or -1 on "Save"
    if (in->pressed & PSP_CTRL_CROSS) {
        click = 1;
        if (s < 0) {
            char def[BUILD_NAME_LEN];
            next_free_name(def, sizeof(def));
            if (osk_start("Name this build", def, BUILD_NAME_LEN - 1) == 0) g_naming = 1;
        } else {
            snprintf(name, name_len, "%s", g_saves[s].name);
            result = SAVES_LOAD_AIM;
        }
    } else if ((in->pressed & PSP_CTRL_SQUARE) && s >= 0) {
        click = 1;
        if (strcmp(g_saves[s].map, g_map) == 0) {
            snprintf(name, name_len, "%s", g_saves[s].name);
            result = SAVES_LOAD_IN_PLACE;
        } else {
            snprintf(g_msg, sizeof(g_msg), "Built on %s: X loads it here", g_saves[s].map);
        }
    } else if ((in->pressed & PSP_CTRL_TRIANGLE) && s >= 0) {
        click = 1;
        if (g_confirm != s) {
            g_confirm = s;
        } else {
            char path[96];
            build_path(path, sizeof(path), g_saves[s].name);
            snprintf(g_msg, sizeof(g_msg), sceIoRemove(path) >= 0 ? "Deleted %s" : "Couldn't delete %s", g_saves[s].name);
            g_confirm = -1;
            saves_refresh();
        }
    }
    if (click && g_settings.sound) snd_play(snd_find("menu_move"), 0.4f, 0.0f);
    if (in->pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) result = SAVES_BACK;
    return result;
}

void saves_frame(void) {
    osk_frame();
}

void saves_draw(float x, float y, float w, float h) {
    ui_rrect(x, y, w, h, 4, 0xF0E8E8E8);
    ui_gradient(x, y, w, 22, 0xFF2A30C0, 0xFF1E2290);
    ui_text(FONT_BOLD, x + 8, y + 5, 0xFFFFFFFF, ALIGN_LEFT, "Saves");
    ui_textf(FONT_SMALL, x + w - 8, y + 6, 0xFFE0E0E0, ALIGN_RIGHT, "%d saved", g_num_saves);
    float top = y + 28;
    int rows = (int)((h - 64) / ROW_H), total = g_num_saves + 1;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + rows) g_scroll = g_sel - rows + 1;
    for (int r = 0; r < rows && g_scroll + r < total; r++) {
        int row = g_scroll + r, sel = row == g_sel;
        float ry = top + r * ROW_H;
        if (sel) ui_rrect(x + 6, ry - 1, w - 12, ROW_H - 1, 3, 0xFF2AA8F0);
        unsigned int tc = sel ? 0xFFFFFFFF : 0xFF303030, sub = sel ? 0xFFFFFFFF : 0xFF707070;
        if (row == 0) {
            ui_text(FONT_BOLD, x + 12, ry, tc, ALIGN_LEFT, "+  Save the current build");
            continue;
        }
        int s = row - 1;
        ui_text_clip(FONT_SMALL, x + 14, ry + 1, w * 0.45f, tc, g_saves[s].name);
        if (g_confirm == s) {
            ui_text(FONT_SMALL, x + w - 10, ry + 1, sel ? 0xFFFFFFFF : 0xFF2020C0, ALIGN_RIGHT,
                    "Triangle again to delete");
        } else {
            ui_textf(FONT_SMALL, x + w - 10, ry + 1, sub, ALIGN_RIGHT, "%d prop%s  -  %s", g_saves[s].props,
                     g_saves[s].props == 1 ? "" : "s", g_saves[s].map);
        }
    }
    if (g_num_saves == 0) {
        ui_text(FONT_SMALL, x + 14, top + ROW_H + 4, 0xFF606060, ALIGN_LEFT, "Nothing saved yet");
    }
    if (g_msg[0]) ui_text(FONT_SMALL, x + 10, y + h - 34, 0xFF2060A0, ALIGN_LEFT, g_msg);
    ui_text(FONT_SMALL, x + 10, y + h - 18, 0xFF606060, ALIGN_LEFT,
            g_sel == 0 ? "X: save (name it)   O: back"
                       : "X: load where you look   Square: where it was built   Triangle: delete");
}
