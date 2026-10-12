#pragma once

#include "proplib.h"

// Extra props: thousands of spawnable models from the Source games that stay on disc until
// something spawns them, so they cost next to no memory until used. Only a compact index (name,
// bounds, mass, shape, category) is kept loaded; each model is a small standalone prop pack inside
// data/props_x.ppak (or props_x.low.ppak), read in the first time it's needed. Spawn icons are in
// data/props_x.icons, read a few at a time while the spawn menu shows them.
//
// data/props_x.idx, written by tools/bspconv/extras.py:
//   {char magic[4] "PXIX"; u32 version; u32 count; u32 num_categories}
//   num_categories x char[24]
//   count x PropxEntry
// The packs are the models' prop packs back to back, each 64-byte aligned.
// Icons: {char magic[4] "PXIC"; u32 count; u32 0, 0}, then per model a 256-entry CLUT and a
// swizzled 64x64 T8 image (PROPX_ICON_BYTES each).

#define PROPX_ICON_SIZE 64
#define PROPX_ICON_BYTES (1024 + PROPX_ICON_SIZE * PROPX_ICON_SIZE)

typedef struct {
    char name[24];
    int16_t mins[3], maxs[3]; // whole units
    float mass;
    uint16_t category; // into the index's categories
    uint8_t shape;     // PPAK_SHAPE_*
    uint8_t surface;   // SURF_*
    uint32_t offset[2], size[2]; // in props_x.low.ppak [0] and props_x.ppak [1]
} PropxEntry;

int propx_init(void); // reads the index; 0 when there is one
int propx_count(void);
const char *propx_name(int i);
int propx_category_of(int i);
// The model's record (bounds, mass, shape) without loading it
void propx_info(int i, PpakModel *out);
int propx_num_categories(void);
const char *propx_category(int c);
// The model's prop pack (its model is index 0), read from disc the first time. NULL if it can't
// be (out of memory, or too many different extra props in use).
const PropLib *propx_lib(int i);
int propx_loaded(int i);
void propx_unload(int i);
// Frees every loaded model: the game calls it between maps, when no props can be using them
void propx_unload_all(void);
// Lets models that failed to load be tried again
void propx_retry_all(void);
// Draws the model's spawn icon, reading it from disc if needed (a couple per frame, so a page
// fills in quickly without a stall). Returns 0 while it isn't loaded yet.
int propx_draw_icon(int i, float x, float y, float w, float h);
// Call once per frame the spawn menu is drawn (paces the icon loading)
void propx_new_frame(void);
