#include "ui.h"

#include <pspgu.h>
#include <pspkernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pak.h"
#include "pmap_format.h"

typedef struct {
    uint16_t atlas_w, atlas_h;
    uint8_t line, ascent;
    uint16_t pad;
    uint32_t atlas_ofs, glyph_ofs;
} FontRec;

typedef struct {
    uint16_t x, y;
    uint8_t w, h;
    int8_t xoff, yoff;
    uint8_t adv;
    uint8_t pad[3];
} Glyph;

typedef struct {
    char name[48];
    uint16_t tex_w, tex_h;
    uint16_t w, h;
    uint32_t clut_ofs, data_ofs;
} ImageRec;

typedef struct {
    float u, v;
    unsigned int color;
    float x, y, z;
} TVert;

typedef struct {
    unsigned int color;
    float x, y, z;
} CVert;

#define TVERT_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define CVERT_FMT (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define CIRCLE 32

static void *g_file;
static const FontRec *g_fonts;
static int g_num_fonts;
static const ImageRec *g_images;
static int g_num_images;
static unsigned int __attribute__((aligned(16))) g_ramp[256]; // white, alpha = index
static unsigned char __attribute__((aligned(16))) g_circle[CIRCLE * CIRCLE];

static void thumbs_load(void);

int ui_load(void) {
    unsigned int size;
    g_file = pak_read_file("data/ui.pak", &size);
    if (!g_file) return -1;
    const PmapHeader *h = g_file;
    if (memcmp(h->magic, "PUI ", 4) != 0) {
        free(g_file);
        g_file = NULL;
        return -2;
    }
    const PmapSection *f = pak_find(g_file, PMAP_SECTION_ID('F', 'N', 'T', 'S'));
    const PmapSection *im = pak_find(g_file, PMAP_SECTION_ID('I', 'M', 'G', 'S'));
    if (!f || !im) return -3;
    g_fonts = (const FontRec *)((const char *)g_file + f->offset);
    g_num_fonts = (int)f->count;
    g_images = (const ImageRec *)((const char *)g_file + im->offset);
    g_num_images = (int)im->count;

    for (int i = 0; i < 256; i++) g_ramp[i] = ((unsigned int)i << 24) | 0x00FFFFFFu;
    // Anti-aliased disc for rounded corners: 4x4 supersampled coverage
    for (int y = 0; y < CIRCLE; y++) {
        for (int x = 0; x < CIRCLE; x++) {
            int cover = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4.0f - CIRCLE / 2.0f, py = y + (sy + 0.5f) / 4.0f - CIRCLE / 2.0f;
                    if (px * px + py * py <= (CIRCLE / 2.0f) * (CIRCLE / 2.0f)) cover++;
                }
            }
            g_circle[y * CIRCLE + x] = (unsigned char)(cover * 255 / 16);
        }
    }
    thumbs_load();
    sceKernelDcacheWritebackAll();
    return 0;
}

int ui_ready(void) {
    return g_file != NULL;
}

// --- Map thumbnails ---------------------------------------------------------------------------------
// data/thumbs.pak: {"PTHB", u32 count, 0, 0}, count x {char map[32]; u32 offset}, then per map a
// 256-entry CLUT and a swizzled 128x128 T8 image. Hundreds of maps' worth would be megabytes, so
// only the index stays loaded and the pictures are read while they're on screen.

#define THUMB_SIZE 128
#define THUMB_BYTES (1024 + THUMB_SIZE * THUMB_SIZE)
#define THUMB_SLOTS 7 // the map browser shows 6, plus the loading screen
#define THUMB_LOADS_PER_FRAME 2

typedef struct {
    char map[32];
    uint32_t offset;
} ThumbRec;

static ThumbRec *g_thumbs;
static int g_num_thumbs;
static struct {
    int thumb; // -1 = free
    unsigned int last_used;
} g_thumb_slot[THUMB_SLOTS];
static unsigned char __attribute__((aligned(64))) g_thumb_data[THUMB_SLOTS][THUMB_BYTES];
static unsigned int g_ui_frame = 1, g_thumb_loads;

static void thumbs_load(void) {
    uint32_t h[4];
    for (int s = 0; s < THUMB_SLOTS; s++) g_thumb_slot[s].thumb = -1;
    if (pak_read_range("data/thumbs.pak", 0, h, sizeof(h)) < 0 || memcmp(h, "PTHB", 4) != 0) return;
    g_thumbs = malloc(sizeof(ThumbRec) * h[1]);
    if (g_thumbs && pak_read_range("data/thumbs.pak", sizeof(h), g_thumbs, (unsigned int)(sizeof(ThumbRec) * h[1])) == 0) {
        g_num_thumbs = (int)h[1];
    }
}

int ui_map_thumb(const char *map, float x, float y, float w, float h) {
    int t = -1;
    for (int i = 0; i < g_num_thumbs && t < 0; i++) {
        if (strncmp(g_thumbs[i].map, map, sizeof(g_thumbs[i].map)) == 0) t = i;
    }
    if (t < 0) return 0;
    int slot = -1;
    for (int s = 0; s < THUMB_SLOTS && slot < 0; s++) {
        if (g_thumb_slot[s].thumb == t) slot = s;
    }
    if (slot < 0) {
        if (g_thumb_loads >= THUMB_LOADS_PER_FRAME) return 0;
        // A free slot, else the one shown longest ago (never one drawn this frame: the GPU hasn't read it)
        for (int s = 0; s < THUMB_SLOTS; s++) {
            if (g_thumb_slot[s].thumb < 0) {
                slot = s;
                break;
            }
            if (g_thumb_slot[s].last_used != g_ui_frame && (slot < 0 || g_thumb_slot[s].last_used < g_thumb_slot[slot].last_used)) {
                slot = s;
            }
        }
        if (slot < 0) return 0;
        g_thumb_loads++;
        g_thumb_slot[slot].thumb = -1;
        if (pak_read_range("data/thumbs.pak", g_thumbs[t].offset, g_thumb_data[slot], THUMB_BYTES) < 0) return 0;
        sceKernelDcacheWritebackAll();
        g_thumb_slot[slot].thumb = t;
    }
    g_thumb_slot[slot].last_used = g_ui_frame;
    const unsigned char *d = g_thumb_data[slot];
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(256 / 8, d);
    sceGuTexMode(GU_PSM_T8, 0, 0, 1);
    sceGuTexImage(0, THUMB_SIZE, THUMB_SIZE, THUMB_SIZE, d + 1024);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    TVert *v = sceGuGetMemory(2 * sizeof(TVert));
    v[0] = (TVert){0, 0, 0xFFFFFFFF, x, y, 0};
    v[1] = (TVert){THUMB_SIZE, THUMB_SIZE, 0xFFFFFFFF, x + w, y + h, 0};
    sceGuDrawArray(GU_SPRITES, TVERT_FMT, 2, 0, v);
    return 1;
}

void ui_begin(void) {
    g_ui_frame++; // a new frame: the thumbnail loading budget starts again
    g_thumb_loads = 0;
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuShadeModel(GU_SMOOTH);
}

void ui_end(void) {
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_DEPTH_TEST);
}

static void bind_alpha(const void *data, int w, int h, int swizzled, int linear) {
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(256 / 8, g_ramp);
    sceGuTexMode(GU_PSM_T8, 0, 0, swizzled);
    sceGuTexImage(0, w, h, w, data);
    sceGuTexFilter(linear ? GU_LINEAR : GU_NEAREST, linear ? GU_LINEAR : GU_NEAREST);
}

void ui_rect(float x, float y, float w, float h, unsigned int c) {
    if (w <= 0 || h <= 0) return;
    sceGuDisable(GU_TEXTURE_2D);
    CVert *v = sceGuGetMemory(2 * sizeof(CVert));
    v[0] = (CVert){c, x, y, 0};
    v[1] = (CVert){c, x + w, y + h, 0};
    sceGuDrawArray(GU_SPRITES, CVERT_FMT, 2, 0, v);
}

static void quad_colors(float x, float y, float w, float h, unsigned int c00, unsigned int c10, unsigned int c01,
                        unsigned int c11) {
    sceGuDisable(GU_TEXTURE_2D);
    CVert *v = sceGuGetMemory(4 * sizeof(CVert));
    v[0] = (CVert){c00, x, y, 0};
    v[1] = (CVert){c10, x + w, y, 0};
    v[2] = (CVert){c01, x, y + h, 0};
    v[3] = (CVert){c11, x + w, y + h, 0};
    sceGuDrawArray(GU_TRIANGLE_STRIP, CVERT_FMT, 4, 0, v);
}

void ui_gradient(float x, float y, float w, float h, unsigned int top, unsigned int bottom) {
    quad_colors(x, y, w, h, top, top, bottom, bottom);
}

void ui_hgradient(float x, float y, float w, float h, unsigned int left, unsigned int right) {
    quad_colors(x, y, w, h, left, right, left, right);
}

void ui_outline(float x, float y, float w, float h, unsigned int c) {
    ui_rect(x, y, w, 1, c);
    ui_rect(x, y + h - 1, w, 1, c);
    ui_rect(x, y + 1, 1, h - 2, c);
    ui_rect(x + w - 1, y + 1, 1, h - 2, c);
}

void ui_rrect(float x, float y, float w, float h, float r, unsigned int c) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 1.0f) {
        ui_rect(x, y, w, h, c);
        return;
    }
    ui_rect(x + r, y, w - 2 * r, h, c);
    ui_rect(x, y + r, r, h - 2 * r, c);
    ui_rect(x + w - r, y + r, r, h - 2 * r, c);
    // Corners from the disc texture, a quarter each
    bind_alpha(g_circle, CIRCLE, CIRCLE, 0, 1);
    TVert *v = sceGuGetMemory(8 * sizeof(TVert));
    const float hc = CIRCLE / 2.0f;
    v[0] = (TVert){0, 0, c, x, y, 0};
    v[1] = (TVert){hc, hc, c, x + r, y + r, 0};
    v[2] = (TVert){hc, 0, c, x + w - r, y, 0};
    v[3] = (TVert){CIRCLE, hc, c, x + w, y + r, 0};
    v[4] = (TVert){0, hc, c, x, y + h - r, 0};
    v[5] = (TVert){hc, CIRCLE, c, x + r, y + h, 0};
    v[6] = (TVert){hc, hc, c, x + w - r, y + h - r, 0};
    v[7] = (TVert){CIRCLE, CIRCLE, c, x + w, y + h, 0};
    sceGuDrawArray(GU_SPRITES, TVERT_FMT, 8, 0, v);
}

int ui_image_find(const char *name) {
    for (int i = 0; i < g_num_images; i++) {
        if (strncmp(g_images[i].name, name, sizeof(g_images[i].name)) == 0) return i;
    }
    return -1;
}

void ui_image_size(int id, int *w, int *h) {
    if (id < 0 || id >= g_num_images) {
        *w = *h = 0;
        return;
    }
    *w = g_images[id].w;
    *h = g_images[id].h;
}

void ui_image(int id, float x, float y, float w, float h, unsigned int tint) {
    if (id < 0 || id >= g_num_images) return;
    const ImageRec *im = &g_images[id];
    const char *base = g_file;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(256 / 8, base + im->clut_ofs);
    sceGuTexMode(GU_PSM_T8, 0, 0, 1);
    sceGuTexImage(0, im->tex_w, im->tex_h, im->tex_w, base + im->data_ofs);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    TVert *v = sceGuGetMemory(2 * sizeof(TVert));
    v[0] = (TVert){0, 0, tint, x, y, 0};
    v[1] = (TVert){(float)im->w, (float)im->h, tint, x + w, y + h, 0};
    sceGuDrawArray(GU_SPRITES, TVERT_FMT, 2, 0, v);
}

int ui_image_bind(int id, float *u, float *v) {
    if (id < 0 || id >= g_num_images) return 0;
    const ImageRec *im = &g_images[id];
    const char *base = g_file;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(256 / 8, base + im->clut_ofs);
    sceGuTexMode(GU_PSM_T8, 0, 0, 1);
    sceGuTexImage(0, im->tex_w, im->tex_h, im->tex_w, base + im->data_ofs);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    *u = (float)im->w / (float)im->tex_w;
    *v = (float)im->h / (float)im->tex_h;
    return 1;
}

static const Glyph *glyph(int font, int ch) {
    if (ch < 32 || ch > 126) ch = '?';
    const Glyph *g = (const Glyph *)((const char *)g_file + g_fonts[font].glyph_ofs);
    return &g[ch - 32];
}

float ui_text_width(int font, const char *s) {
    if (!g_file || font >= g_num_fonts) return 0.0f;
    float w = 0.0f;
    for (; *s; s++) w += glyph(font, (unsigned char)*s)->adv;
    return w;
}

int ui_line_height(int font) {
    return (g_file && font < g_num_fonts) ? g_fonts[font].line : 12;
}

float ui_text(int font, float x, float y, unsigned int c, int align, const char *s) {
    if (!g_file || font >= g_num_fonts || !*s) return 0.0f;
    float w = ui_text_width(font, s);
    if (align == ALIGN_CENTER) x -= (float)(int)(w * 0.5f);
    if (align == ALIGN_RIGHT) x -= w;
    x = (float)(int)x;
    y = (float)(int)y;
    int n = (int)strlen(s);
    const FontRec *f = &g_fonts[font];
    bind_alpha((const char *)g_file + f->atlas_ofs, f->atlas_w, f->atlas_h, 1, 0);
    TVert *v = sceGuGetMemory((size_t)n * 2 * sizeof(TVert));
    int k = 0;
    float pen = x;
    for (int i = 0; i < n; i++) {
        const Glyph *g = glyph(font, (unsigned char)s[i]);
        if (g->w) {
            float gx = pen + g->xoff, gy = y + g->yoff;
            v[k++] = (TVert){g->x, g->y, c, gx, gy, 0};
            v[k++] = (TVert){(float)(g->x + g->w), (float)(g->y + g->h), c, gx + g->w, gy + g->h, 0};
        }
        pen += g->adv;
    }
    if (k) sceGuDrawArray(GU_SPRITES, TVERT_FMT, k, 0, v);
    return w;
}

float ui_textf(int font, float x, float y, unsigned int c, int align, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return ui_text(font, x, y, c, align, buf);
}

float ui_text_shadow(int font, float x, float y, unsigned int c, int align, const char *s) {
    unsigned int shadow = (c & 0xFF000000u) >= 0x80000000u ? 0xA0000000u : 0x50000000u;
    ui_text(font, x + 1, y + 1, shadow, align, s);
    return ui_text(font, x, y, c, align, s);
}

void ui_text_clip(int font, float x, float y, float max_w, unsigned int c, const char *s) {
    if (ui_text_width(font, s) <= max_w) {
        ui_text(font, x, y, c, ALIGN_LEFT, s);
        return;
    }
    char buf[96];
    int n = (int)strlen(s);
    if (n > 90) n = 90;
    float dots = ui_text_width(font, "...");
    while (n > 0) {
        memcpy(buf, s, (size_t)n);
        buf[n] = 0;
        if (ui_text_width(font, buf) + dots <= max_w) break;
        n--;
    }
    strcpy(buf + n, "...");
    ui_text(font, x, y, c, ALIGN_LEFT, buf);
}
