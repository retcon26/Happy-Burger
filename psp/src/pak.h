#pragma once

// Shared helpers for the sectioned asset files (.pmap maps, .ppak prop packs).

#include "pmap_format.h"

// Reads a whole file into a 64-byte-aligned buffer (free() it). NULL on failure.
void *pak_read_file(const char *path, unsigned int *size_out);
// Why the last pak_read_file returned NULL ("" if it didn't)
const char *pak_last_error(void);
// Reads size bytes at offset of a file into dst. 0 on success.
int pak_read_range(const char *path, unsigned int offset, void *dst, unsigned int size);
int pak_file_exists(const char *path);
const PmapSection *pak_find(const void *file, uint32_t id);
// Binds a T8 texture with its CLUT and mip levels; file is the buffer the offsets are relative to.
void pak_bind_texture(const void *file, const PmapTexture *t);
