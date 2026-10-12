#include "propx.h"

#include <malloc.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "pak.h"

#define MAX_LOADED 96 // different extra props in memory at once (4 players x 40 props fit)
#define ICON_SLOTS 16 // the grid shows 12
#define ICON_LOADS_PER_FRAME 2

static const char *const k_pack[2] = {"data/props_x.low.ppak", "data/props_x.ppak"};
static const char *const k_icons = "data/props_x.icons";

static PropxEntry *g_entries;
static char (*g_cats)[24];
static int g_count, g_num_cats, g_num_icons, g_tier;
static int16_t *g_slot_of; // per model: its slot in g_loaded, -1 if not loaded, -2 if it failed
static struct {
    int model; // -1 = free
    PropLib lib;
} g_loaded[MAX_LOADED];

static struct {
    int model; // -1 = free
    unsigned int last_used;
} g_icon_slot[ICON_SLOTS];
static unsigned char __attribute__((aligned(64))) g_icon_data[ICON_SLOTS][PROPX_ICON_BYTES];
static unsigned int g_frame = 1, g_icon_frame, g_icon_loads;

int propx_init(void) {
    if (g_entries) return 0;
    uint32_t hdr[4];
    if (pak_read_range("data/props_x.idx", 0, hdr, sizeof(hdr)) < 0 || memcmp(hdr, "PXIX", 4) != 0 || hdr[1] != 1) return -1;
    int count = (int)hdr[2], cats = (int)hdr[3];
    size_t bytes = 24 * (size_t)cats + sizeof(PropxEntry) * (size_t)count;
    char *index = malloc(bytes);
    g_slot_of = malloc(sizeof(int16_t) * (size_t)count);
    if (!index || !g_slot_of || pak_read_range("data/props_x.idx", sizeof(hdr), index, (unsigned int)bytes) < 0) {
        free(index);
        free(g_slot_of);
        g_slot_of = NULL;
        return -1;
    }
    g_cats = (char(*)[24])index;
    g_entries = (PropxEntry *)(index + 24 * cats);
    g_count = count;
    g_num_cats = cats;
    for (int i = 0; i < count; i++) g_slot_of[i] = -1;
    for (int s = 0; s < MAX_LOADED; s++) g_loaded[s].model = -1;
    g_tier = mem_tier() == MEM_TIER_HIGH ? 1 : 0;
    if (!pak_file_exists(k_pack[g_tier])) g_tier ^= 1;

    uint32_t ih[4];
    if (pak_read_range(k_icons, 0, ih, sizeof(ih)) == 0 && memcmp(ih, "PXIC", 4) == 0) {
        g_num_icons = (int)ih[1] < count ? (int)ih[1] : count;
    }
    for (int s = 0; s < ICON_SLOTS; s++) g_icon_slot[s].model = -1;
    return 0;
}

int propx_count(void) {
    return g_count;
}

const char *propx_name(int i) {
    return i >= 0 && i < g_count ? g_entries[i].name : "";
}

int propx_category_of(int i) {
    return i >= 0 && i < g_count ? g_entries[i].category : 0;
}

void propx_info(int i, PpakModel *out) {
    memset(out, 0, sizeof(*out));
    if (i < 0 || i >= g_count) return;
    const PropxEntry *e = &g_entries[i];
    memcpy(out->name, e->name, sizeof(e->name));
    out->name[sizeof(e->name) - 1] = 0;
    for (int k = 0; k < 3; k++) {
        out->mins[k] = e->mins[k];
        out->maxs[k] = e->maxs[k];
    }
    out->mass = e->mass;
    out->flags = e->shape;
    out->surface = e->surface;
}

int propx_num_categories(void) {
    return g_num_cats;
}

const char *propx_category(int c) {
    return c >= 0 && c < g_num_cats ? g_cats[c] : "";
}

int propx_loaded(int i) {
    return i >= 0 && i < g_count && g_slot_of[i] >= 0;
}

const PropLib *propx_lib(int i) {
    if (i < 0 || i >= g_count || g_slot_of[i] == -2) return NULL;
    if (g_slot_of[i] >= 0) return &g_loaded[g_slot_of[i]].lib;
    int slot = -1;
    for (int s = 0; s < MAX_LOADED && slot < 0; s++) {
        if (g_loaded[s].model < 0) slot = s;
    }
    g_slot_of[i] = -2; // until it's in: a failure isn't retried every frame
    if (slot < 0) return NULL;
    const PropxEntry *e = &g_entries[i];
    void *buf = memalign(64, e->size[g_tier]);
    if (!buf) return NULL;
    if (pak_read_range(k_pack[g_tier], e->offset[g_tier], buf, e->size[g_tier]) < 0) {
        free(buf);
        return NULL;
    }
    char err[96];
    if (proplib_load_buffer(&g_loaded[slot].lib, buf, k_pack[g_tier], err, sizeof(err)) < 0) return NULL;
    sceKernelDcacheWritebackAll();
    g_loaded[slot].model = i;
    g_slot_of[i] = (int16_t)slot;
    return &g_loaded[slot].lib;
}

void propx_unload(int i) {
    if (i < 0 || i >= g_count) return;
    int s = g_slot_of[i];
    if (s >= 0) {
        proplib_free(&g_loaded[s].lib);
        g_loaded[s].model = -1;
    }
    g_slot_of[i] = -1;
}

void propx_unload_all(void) {
    for (int i = 0; i < g_count; i++) propx_unload(i);
}

void propx_retry_all(void) {
    for (int i = 0; i < g_count; i++) {
        if (g_slot_of[i] == -2) g_slot_of[i] = -1;
    }
}

void propx_new_frame(void) {
    g_frame++;
}

int propx_draw_icon(int i, float x, float y, float w, float h) {
    if (i < 0 || i >= g_num_icons) return 0;
    if (g_frame != g_icon_frame) { // a new frame: the load budget starts again
        g_icon_frame = g_frame;
        g_icon_loads = 0;
    }
    int slot = -1;
    for (int s = 0; s < ICON_SLOTS && slot < 0; s++) {
        if (g_icon_slot[s].model == i) slot = s;
    }
    if (slot < 0) {
        if (g_icon_loads >= ICON_LOADS_PER_FRAME) return 0;
        // A free slot, else the one shown longest ago, but never one drawn this frame: the GPU
        // hasn't read it yet
        int best = -1;
        for (int s = 0; s < ICON_SLOTS; s++) {
            if (g_icon_slot[s].model < 0) {
                best = s;
                break;
            }
            if (g_icon_slot[s].last_used != g_frame && (best < 0 || g_icon_slot[s].last_used < g_icon_slot[best].last_used)) best = s;
        }
        if (best < 0) return 0;
        g_icon_loads++;
        slot = best;
        g_icon_slot[slot].model = -1;
        if (pak_read_range(k_icons, 16 + (unsigned int)i * PROPX_ICON_BYTES, g_icon_data[slot], PROPX_ICON_BYTES) < 0) return 0;
        sceKernelDcacheWritebackAll();
        g_icon_slot[slot].model = i;
    }
    g_icon_slot[slot].last_used = g_frame;
    const unsigned char *d = g_icon_data[slot];
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(256 / 8, d);
    sceGuTexMode(GU_PSM_T8, 0, 0, 1);
    sceGuTexImage(0, PROPX_ICON_SIZE, PROPX_ICON_SIZE, PROPX_ICON_SIZE, d + 1024);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    typedef struct {
        float u, v;
        unsigned int color;
        float x, y, z;
    } TVert;
    TVert *v = sceGuGetMemory(2 * sizeof(TVert));
    v[0] = (TVert){0, 0, 0xFFFFFFFF, x, y, 0};
    v[1] = (TVert){PROPX_ICON_SIZE, PROPX_ICON_SIZE, 0xFFFFFFFF, x + w, y + h, 0};
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
    return 1;
}
