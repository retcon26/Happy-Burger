#include "shot.h"

#include <pspdisplay.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "gfx.h"

#define PHOTO_DIR "PSP/PHOTO/RetconsMod"
#define OUT_CHUNK 16384 // compressed bytes per IDAT chunk

#define STRIP_ROWS 16 // screen rows copied out at a time

static FILE *g_f;
static unsigned char g_out[OUT_CHUNK];
static unsigned int __attribute__((aligned(16))) g_list[64];
static uint32_t __attribute__((aligned(64))) g_strip[STRIP_ROWS * SCR_W]; // room for 32-bit pixels

// Rows y to y + rows of the screen into g_strip, by the GE: a block copy out of VRAM. The CPU could read VRAM
// itself on a PSP, but PPSSPP draws on the PC's graphics card and only fills the PSP's VRAM in for a copy
// like this one.
static const void *copy_strip(const void *top, int bufw, int psm, int y, int rows) {
    sceKernelDcacheWritebackInvalidateRange(g_strip, sizeof(g_strip));
    sceGuStart(GU_DIRECT, g_list);
    sceGuCopyImage(psm, 0, y, SCR_W, rows, bufw, (void *)top, 0, 0, SCR_W, g_strip);
    sceGuTexSync();
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    return (const void *)((uintptr_t)g_strip | 0x40000000u); // uncached: what the GE wrote, not the cache
}

static void put_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

// A PNG chunk: length, type, data, and the CRC of type and data
static int chunk(const char *type, const unsigned char *data, uint32_t len) {
    unsigned char head[8];
    put_be32(head, len);
    memcpy(head + 4, type, 4);
    uLong crc = crc32(0L, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, len);
    unsigned char tail[4];
    put_be32(tail, (uint32_t)crc);
    return fwrite(head, 1, 8, g_f) == 8 && (!len || fwrite(data, 1, len, g_f) == len) && fwrite(tail, 1, 4, g_f) == 4;
}

// The screen's pixels in RGB, whatever the display's format (the game draws in 5650)
static void row_rgb(const void *fb, int bufw, int fmt, int y, unsigned char *out) {
    for (int x = 0; x < SCR_W; x++) {
        unsigned int r, g, b;
        if (fmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
            uint32_t c = ((const uint32_t *)fb)[y * bufw + x];
            r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
        } else {
            uint16_t c = ((const uint16_t *)fb)[y * bufw + x];
            if (fmt == PSP_DISPLAY_PIXEL_FORMAT_565) {
                r = c & 31, g = (c >> 5) & 63, b = (c >> 11) & 31;
                r = (r << 3) | (r >> 2), g = (g << 2) | (g >> 4), b = (b << 3) | (b >> 2);
            } else if (fmt == PSP_DISPLAY_PIXEL_FORMAT_5551) {
                r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
                r = (r << 3) | (r >> 2), g = (g << 3) | (g >> 2), b = (b << 3) | (b >> 2);
            } else {
                r = (c & 15) * 17, g = ((c >> 4) & 15) * 17, b = ((c >> 8) & 15) * 17;
            }
        }
        out[x * 3] = (unsigned char)r;
        out[x * 3 + 1] = (unsigned char)g;
        out[x * 3 + 2] = (unsigned char)b;
    }
}

int shot_save(const char *name, char *out, int out_len) {
    void *top;
    int bufw, fmt;
    if (sceDisplayGetFrameBuf(&top, &bufw, &fmt, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0 || !top) {
        snprintf(out, (size_t)out_len, "Couldn't read the screen");
        return -1;
    }
    int psm = fmt == PSP_DISPLAY_PIXEL_FORMAT_8888   ? GU_PSM_8888
              : fmt == PSP_DISPLAY_PIXEL_FORMAT_5551 ? GU_PSM_5551
              : fmt == PSP_DISPLAY_PIXEL_FORMAT_4444 ? GU_PSM_4444
                                                     : GU_PSM_5650;

    // The memory stick's (or the PSP Go's storage's) PSP/PHOTO: the device the game runs from
    char cwd[128] = "", dev[8] = "ms0:";
    if (getcwd(cwd, sizeof(cwd)) && strncmp(cwd, "ef0:", 4) == 0) snprintf(dev, sizeof(dev), "ef0:");
    char path[128];
    snprintf(path, sizeof(path), "%s/PSP/PHOTO", dev);
    sceIoMkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/" PHOTO_DIR, dev);
    sceIoMkdir(path, 0777);
    SceIoStat st;
    int n;
    for (n = 1; n <= 9999; n++) {
        snprintf(path, sizeof(path), "%s/" PHOTO_DIR "/%.40s_%04d.png", dev, name, n);
        if (sceIoGetstat(path, &st) < 0) break;
    }
    g_f = fopen(path, "wb");
    if (!g_f) {
        snprintf(out, (size_t)out_len, "Couldn't save the screenshot (is the memory stick full?)");
        return -1;
    }

    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    unsigned char ihdr[13];
    put_be32(ihdr, SCR_W);
    put_be32(ihdr + 4, SCR_H);
    ihdr[8] = 8;  // bits per channel
    ihdr[9] = 2;  // RGB
    ihdr[10] = 0; // deflate
    ihdr[11] = 0; // adaptive filters
    ihdr[12] = 0; // not interlaced
    int ok = fwrite(sig, 1, 8, g_f) == 8 && chunk("IHDR", ihdr, 13);

    // Rows with the Sub filter (each byte less the one a pixel to its left), deflated a row at a time into
    // IDAT chunks. A small window keeps zlib to about 64 KB.
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (ok && deflateInit2(&z, 3, Z_DEFLATED, 13, 6, Z_DEFAULT_STRATEGY) != Z_OK) ok = 0;
    static unsigned char rgb[SCR_W * 3], row[1 + SCR_W * 3];
    const void *strip = NULL;
    for (int y = 0; ok && y < SCR_H; y++) {
        if (y % STRIP_ROWS == 0) {
            int rows = SCR_H - y < STRIP_ROWS ? SCR_H - y : STRIP_ROWS;
            strip = copy_strip(top, bufw, psm, y, rows);
        }
        row_rgb(strip, SCR_W, fmt, y % STRIP_ROWS, rgb);
        row[0] = 1;
        for (int i = 0; i < SCR_W * 3; i++) row[1 + i] = (unsigned char)(rgb[i] - (i >= 3 ? rgb[i - 3] : 0));
        z.next_in = row;
        z.avail_in = sizeof(row);
        int flush = y == SCR_H - 1 ? Z_FINISH : Z_NO_FLUSH;
        int r;
        do {
            z.next_out = g_out;
            z.avail_out = OUT_CHUNK;
            r = deflate(&z, flush);
            uint32_t have = OUT_CHUNK - z.avail_out;
            if (have && !chunk("IDAT", g_out, have)) ok = 0;
        } while (ok && (z.avail_out == 0 || (flush == Z_FINISH && r != Z_STREAM_END)));
    }
    deflateEnd(&z);
    if (ok) ok = chunk("IEND", NULL, 0);
    if (fclose(g_f) != 0) ok = 0;
    g_f = NULL;
    if (!ok) {
        remove(path);
        snprintf(out, (size_t)out_len, "Couldn't save the screenshot (is the memory stick full?)");
        return -1;
    }
    snprintf(out, (size_t)out_len, "%s", path);
    return 0;
}
