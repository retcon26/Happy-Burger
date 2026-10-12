#pragma once

// Text overlay on an 8x8-pixel cell grid of 60x34.
// Text is rasterised into a small RAM texture with the debug font, then drawn by the GE as sprites.
// Drawing with the GE (not the CPU into VRAM) means it also shows up in emulators.
void hud_init(void);
// Start collecting lines for this frame.
void hud_begin(void);
void hud_print(int col, int row, unsigned int abgr, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
// Like hud_print but at a pixel position.
void hud_print_px(int x, int y, unsigned int abgr, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
// Filled rectangle in pixels, alpha-blended, drawn underneath the text.
void hud_rect(int x, int y, int w, int h, unsigned int abgr);
// Emit the collected text into the current display list. Call after the 3D scene, before gfx_end_frame.
void hud_draw(void);
