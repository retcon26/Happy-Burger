#include "hud.h"

#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CANVAS_W 512 // pspDebugScreen assumes a 512-pixel stride
#define MAX_LINES 32
#define CANVAS_H (MAX_LINES * 8)
#define MAX_CHARS (CANVAS_W / 8)
#define MAX_RECTS 64

typedef struct {
    float u, v;
    unsigned int color;
    float x, y, z;
} SpriteVert;

typedef struct {
    unsigned int color;
    float x, y, z;
} RectVert;

typedef struct {
    int x, y, len;
    unsigned int color;
} Line;

typedef struct {
    int x, y, w, h;
    unsigned int color;
} Rect;

static unsigned int __attribute__((aligned(16))) g_canvas[CANVAS_W * CANVAS_H];
static Line g_lines[MAX_LINES];
static int g_num_lines;
static Rect g_rects[MAX_RECTS];
static int g_num_rects;

void hud_init(void) {
    // InitEx clears a whole 512x272 screen at its base, far bigger than g_canvas, so let it clear
    // VRAM (NULL = default base) and point it at the canvas only when printing.
    pspDebugScreenInitEx(NULL, PSP_DISPLAY_PIXEL_FORMAT_8888, 0);
    pspDebugScreenEnableBackColor(0);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
}

void hud_begin(void) {
    g_num_lines = 0;
    g_num_rects = 0;
}

static void add_line(int x, int y, unsigned int abgr, const char *fmt, va_list ap) {
    if (g_num_lines >= MAX_LINES) {
        return;
    }
    char buf[MAX_CHARS + 1];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n <= 0) {
        return;
    }
    if (n > MAX_CHARS) {
        n = MAX_CHARS;
    }
    int slot = g_num_lines++;
    memset(&g_canvas[slot * 8 * CANVAS_W], 0, CANVAS_W * 8 * sizeof(unsigned int));
    // The font is rendered white; colour comes from the sprite's vertex colour.
    pspDebugScreenSetBase((u32 *)g_canvas);
    pspDebugScreenSetXY(0, slot);
    pspDebugScreenPrintData(buf, n);
    g_lines[slot].x = x;
    g_lines[slot].y = y;
    g_lines[slot].len = n;
    g_lines[slot].color = abgr;
}

void hud_print(int col, int row, unsigned int abgr, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    add_line(col * 8, row * 8, abgr, fmt, ap);
    va_end(ap);
}

void hud_print_px(int x, int y, unsigned int abgr, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    add_line(x, y, abgr, fmt, ap);
    va_end(ap);
}

void hud_rect(int x, int y, int w, int h, unsigned int abgr) {
    if (g_num_rects >= MAX_RECTS) return;
    g_rects[g_num_rects++] = (Rect){x, y, w, h, abgr};
}

void hud_draw(void) {
    if (g_num_lines == 0 && g_num_rects == 0) {
        return;
    }
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

    if (g_num_rects) {
        sceGuDisable(GU_TEXTURE_2D);
        RectVert *rv = sceGuGetMemory(sizeof(RectVert) * 2 * g_num_rects);
        for (int i = 0; i < g_num_rects; i++) {
            const Rect *r = &g_rects[i];
            rv[i * 2] = (RectVert){r->color, (float)r->x, (float)r->y, 0.0f};
            rv[i * 2 + 1] = (RectVert){r->color, (float)(r->x + r->w), (float)(r->y + r->h), 0.0f};
        }
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, g_num_rects * 2, 0, rv);
    }

    if (g_num_lines) {
        sceKernelDcacheWritebackRange(g_canvas, CANVAS_W * g_num_lines * 8 * sizeof(unsigned int));
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, CANVAS_W, CANVAS_H, CANVAS_W, g_canvas);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexFlush();

        // Two sprites per line: a dark drop shadow one pixel down-right, then the text.
        SpriteVert *v = sceGuGetMemory(sizeof(SpriteVert) * 4 * g_num_lines);
        SpriteVert *p = v;
        for (int i = 0; i < g_num_lines; i++) {
            const Line *l = &g_lines[i];
            float x0 = (float)l->x, y0 = (float)l->y;
            float w = l->len * 8.0f;
            float v0 = i * 8.0f;
            for (int pass = 0; pass < 2; pass++) {
                float o = pass == 0 ? 1.0f : 0.0f;
                unsigned int c = pass == 0 ? 0xC0000000 : l->color;
                p[0] = (SpriteVert){0.0f, v0, c, x0 + o, y0 + o, 0.0f};
                p[1] = (SpriteVert){w, v0 + 8.0f, c, x0 + o + w, y0 + o + 8.0f, 0.0f};
                p += 2;
            }
        }
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       g_num_lines * 4, 0, v);
        sceGuDisable(GU_TEXTURE_2D);
    }

    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_DEPTH_TEST);
}
