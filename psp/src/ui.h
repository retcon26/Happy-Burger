#pragma once

// Immediate-mode 2D drawing for menus and the HUD: anti-aliased fonts, rounded panels, gradients
// and images (spawn icons, map thumbnails, logo) from data/ui.pak.
// Call between ui_begin() and ui_end(), after the 3D scene, inside the frame's display list.

enum { FONT_SMALL, FONT_BOLD, FONT_LARGE, FONT_TITLE, FONT_COUNT };
enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

int ui_load(void);
int ui_ready(void);

void ui_begin(void);
void ui_end(void);

void ui_rect(float x, float y, float w, float h, unsigned int abgr);
void ui_rrect(float x, float y, float w, float h, float radius, unsigned int abgr);
void ui_gradient(float x, float y, float w, float h, unsigned int top, unsigned int bottom);
void ui_hgradient(float x, float y, float w, float h, unsigned int left, unsigned int right);
void ui_outline(float x, float y, float w, float h, unsigned int abgr);

int ui_image_find(const char *name);
// Image size in pixels (its content area), 0 if missing.
void ui_image_size(int id, int *w, int *h);
void ui_image(int id, float x, float y, float w, float h, unsigned int tint);
// A map's thumbnail (data/thumbs.pak), read from disc while it's shown: 0 if the map has none or
// it isn't in yet (a couple are read per frame)
int ui_map_thumb(const char *map, float x, float y, float w, float h);

float ui_text_width(int font, const char *s);
int ui_line_height(int font);
// Draws text with its top at y; returns the width.
float ui_text(int font, float x, float y, unsigned int abgr, int align, const char *s);
float ui_textf(int font, float x, float y, unsigned int abgr, int align, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
// Text with a 1px dark shadow, for HUD text over the 3D view.
float ui_text_shadow(int font, float x, float y, unsigned int abgr, int align, const char *s);
// Fit text into a width, cutting with "..." when needed.
void ui_text_clip(int font, float x, float y, float max_w, unsigned int abgr, const char *s);
