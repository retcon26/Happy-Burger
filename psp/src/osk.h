#pragma once

// The PSP's own on-screen keyboard (for typing your multiplayer name). While it's open the system
// draws it over our frame: call osk_frame() every frame between gfx_end_frame() and gfx_present().

// Opens the keyboard with a title and starting text; max_len is the longest answer in characters.
int osk_start(const char *title, const char *initial, int max_len);
// Drives the keyboard. Returns 1 while it's open.
int osk_frame(void);
int osk_active(void);
// Once it has closed: 1 and the typed text (ASCII; anything else becomes '?') if it was confirmed,
// 0 if it was cancelled. Each result is handed out once.
int osk_result(char *out, int out_len);
