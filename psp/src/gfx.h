#pragma once

#define SCR_W 480
#define SCR_H 272
#define BUF_W 512

// Sets up the GE: two 16-bit (5650) colour buffers plus a 16-bit depth buffer in VRAM.
// That uses ~835KB of the 2MB of VRAM and leaves the rest for textures.
void gfx_init(void);
void gfx_shutdown(void);
// VRAM past the frame and depth buffers ([offset, 2MB) is free for textures).
unsigned int gfx_vram_free_offset(void);
#define GFX_VRAM_SIZE (2u * 1024u * 1024u)

// Opens the frame's display list and clears colour and depth.
void gfx_begin_frame(unsigned int clear_abgr);
// Closes the display list and waits for the GE to finish drawing.
void gfx_end_frame(void);
void gfx_present(int vsync);

// Flashlight: a spotlight from eye along fwd, applied to everything drawn while it's on.
void gu_flashlight(int on, const float eye[3], const float fwd[3]);
// Around the world's draw (baked vertex colours), and around lit models (props, NPCs).
void gfx_flash_world_begin(void);
void gfx_flash_world_end(void);
void gfx_flash_lit_begin(void);
void gfx_flash_lit_end(void);

// Distance fog in world units. far <= near disables it.
void gfx_set_fog(float near_dist, float far_dist, unsigned int abgr);
