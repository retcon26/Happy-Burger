#pragma once

// Screenshots (Pause > Screenshot): what's on the screen now, as a PNG in PSP/PHOTO/HappyBurger/ on the memory
// stick (or the PSP Go's internal storage), where the XMB's Photo app shows it. Named <name>_NNNN.png with
// the first number that's free. Returns 0 and the file's path in out, or -1 (out says why).
int shot_save(const char *name, char *out, int out_len);
