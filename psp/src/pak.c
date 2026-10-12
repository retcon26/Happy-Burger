#include "pak.h"
#include "mem.h"

#include <malloc.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef PS2
void gu_texture_cache_invalidate(void);
#endif

static char g_error[96];

const char *pak_last_error(void) {
    return g_error;
}

void *pak_read_file(const char *path, unsigned int *size_out) {
    g_error[0] = 0;
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        snprintf(g_error, sizeof(g_error), "open failed (0x%08X)", (unsigned int)fd);
        return NULL;
    }
    SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    if (size <= 0 || size > 256 * 1024 * 1024) {
        snprintf(g_error, sizeof(g_error), "bad size (%lld)", (long long)size);
        sceIoClose(fd);
        return NULL;
    }
    void *buf = memalign(64, (size_t)size);
    if (!buf) {
        // How short we are: the biggest block left, and the heap used / its size
        snprintf(g_error, sizeof(g_error), "no room for %uK (%s heap %u/%uK)", (unsigned int)(size / 1024),
                 mem_tier_name(), (unsigned int)(mem_heap_used() / 1024), (unsigned int)(mem_heap_total() / 1024));
        sceIoClose(fd);
        return NULL;
    }
    unsigned char *p = buf;
    SceOff left = size;
    while (left > 0) {
        int chunk = left > 1024 * 1024 ? 1024 * 1024 : (int)left;
        int got = sceIoRead(fd, p, chunk);
        if (got <= 0) {
            snprintf(g_error, sizeof(g_error), "read failed (0x%08X) at %u KB", (unsigned int)got,
                     (unsigned int)((size - left) / 1024));
            free(buf);
            sceIoClose(fd);
            return NULL;
        }
        p += got;
        left -= got;
    }
    sceIoClose(fd);
    *size_out = (unsigned int)size;
#ifdef PS2
    // A new file may reuse the address of a freed one, so GS-side texture copies keyed by
    // address are no longer trustworthy.
    gu_texture_cache_invalidate();
#endif
    return buf;
}

int pak_read_range(const char *path, unsigned int offset, void *dst, unsigned int size) {
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    int ok = sceIoLseek(fd, offset, PSP_SEEK_SET) == (SceOff)offset;
    unsigned char *p = dst;
    unsigned int left = size;
    while (ok && left > 0) {
        int chunk = left > 1024 * 1024 ? 1024 * 1024 : (int)left;
        int got = sceIoRead(fd, p, chunk);
        if (got <= 0) {
            ok = 0;
            break;
        }
        p += got;
        left -= (unsigned int)got;
    }
    sceIoClose(fd);
#ifdef PS2
    gu_texture_cache_invalidate(); // dst may hold a different texture than last time
#endif
    return ok ? 0 : -1;
}

int pak_file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

const PmapSection *pak_find(const void *file, uint32_t id) {
    const PmapHeader *h = file;
    const PmapSection *s = (const PmapSection *)(h + 1);
    for (uint32_t i = 0; i < h->num_sections; i++) {
        if (s[i].id == id) {
            return &s[i];
        }
    }
    return NULL;
}

void pak_bind_texture(const void *file, const PmapTexture *t) {
    const unsigned char *base = file;
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(256 / 8, base + t->clut_ofs);
    sceGuTexMode(GU_PSM_T8, t->levels - 1, 0, GU_TRUE);
    for (int l = 0; l < t->levels; l++) {
        int w = t->width >> l, h = t->height >> l;
        sceGuTexImage(l, w, h, w, base + t->level_ofs[l]);
    }
}
