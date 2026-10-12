#include "proplib.h"

#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "mem.h"
#include "pak.h"

#define VTYPE (GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D)

int proplib_load(PropLib *lib, const char *path_base, char *err, int err_len) {
    memset(lib, 0, sizeof(*lib));
    char hi[96], lo[96];
    snprintf(hi, sizeof(hi), "%s.ppak", path_base);
    snprintf(lo, sizeof(lo), "%s.low.ppak", path_base);
    const char *path = mem_tier() == MEM_TIER_HIGH ? hi : lo;
    if (!pak_file_exists(path)) {
        path = path == hi ? lo : hi;
    }
    unsigned int size;
    lib->file = pak_read_file(path, &size);
    if (!lib->file && path == hi && pak_file_exists(lo)) {
        path = lo; // PS2: fall back to the low-detail pack when RAM is short
        lib->file = pak_read_file(path, &size);
    }
    if (!lib->file) {
        snprintf(err, err_len, "can't read %s", path);
        return -1;
    }
    void *file = lib->file;
    lib->file = NULL;
    return proplib_load_buffer(lib, file, path, err, err_len);
}

int proplib_load_buffer(PropLib *lib, void *file, const char *path, char *err, int err_len) {
    memset(lib, 0, sizeof(*lib));
    lib->file = file;
    const PmapHeader *h = lib->file;
    if (memcmp(h->magic, PPAK_MAGIC, 4) != 0 || h->version != PPAK_VERSION) {
        snprintf(err, err_len, "%s: not a v%d prop pack", path, PPAK_VERSION);
        proplib_free(lib);
        return -2;
    }
    const unsigned char *base = lib->file;
    const PmapSection *s;
#define GET(var, type, a, b, c, d)                          \
    s = pak_find(h, PMAP_SECTION_ID(a, b, c, d));           \
    if (!s) {                                               \
        snprintf(err, err_len, "%s: missing section", path); \
        proplib_free(lib);                                  \
        return -3;                                          \
    }                                                       \
    var = (const type *)(base + s->offset);
    GET(lib->models, PpakModel, 'M', 'D', 'L', 'S');
    lib->num_models = (int)s->count;
    GET(lib->meshes, PpakMesh, 'M', 'E', 'S', 'H');
    GET(lib->verts, PpakVertex, 'V', 'E', 'R', 'T');
    GET(lib->indices, uint16_t, 'I', 'N', 'D', 'X');
    GET(lib->textures, PmapTexture, 'T', 'E', 'X', 'S');
#undef GET
    s = pak_find(h, PMAP_SECTION_ID('C', 'A', 'T', 'S'));
    if (s) {
        lib->categories = (const char(*)[24])(base + s->offset);
        lib->num_categories = (int)s->count;
    }
    const PmapSection *hs = pak_find(h, PMAP_SECTION_ID('H', 'U', 'L', 'L'));
    const PmapSection *hp = pak_find(h, PMAP_SECTION_ID('H', 'P', 'C', 'E'));
    const PmapSection *hl = pak_find(h, PMAP_SECTION_ID('H', 'P', 'L', 'N'));
    const PmapSection *hn = pak_find(h, PMAP_SECTION_ID('H', 'P', 'N', 'T'));
    if (hs && hp && hl && hn && (int)hs->count == lib->num_models) {
        lib->hulls = (const PpakHull *)(base + hs->offset);
        lib->hull_pieces = (const PpakHullPiece *)(base + hp->offset);
        lib->hull_planes = (const PmapPlane *)(base + hl->offset);
        lib->hull_points = (const float(*)[3])(base + hn->offset);
    }
    sceKernelDcacheWritebackAll();
    return 0;
}

void proplib_free(PropLib *lib) {
    if (lib->file) {
        free(lib->file);
    }
    memset(lib, 0, sizeof(*lib));
}

void proplib_begin(void) {
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_LIGHTING);
    sceGuEnable(GU_LIGHT0);
    gfx_flash_lit_begin();
    sceGuEnable(GU_CULL_FACE);
    sceGuFrontFace(GU_CCW);
    sceGuTexFilter(GU_LINEAR_MIPMAP_LINEAR, GU_LINEAR);
    sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuAlphaFunc(GU_GREATER, 0x80, 0xFF);
}

void proplib_end(void) {
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_LIGHT0);
    gfx_flash_lit_end();
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
}

void proplib_draw(const PropLib *lib, int model, const ScePspFMatrix4 *world, const PropLight *light) {
    if (model < 0 || model >= lib->num_models) return;
    const PpakModel *md = &lib->models[model];

    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &light->sun_dir);
    sceGuLightColor(0, GU_DIFFUSE, light->sun_abgr);
    sceGuAmbient(light->ambient_abgr);
    sceGuColor(light->tint_abgr);

    sceGumMatrixMode(GU_MODEL);
    sceGumLoadMatrix(world);

    for (uint32_t i = 0; i < md->num_meshes; i++) {
        const PpakMesh *mesh = &lib->meshes[md->first_mesh + i];
        pak_bind_texture(lib->file, &lib->textures[mesh->tex]);
        if (mesh->flags & (PMAP_BATCH_ALPHATEST | PMAP_BATCH_TRANSLUCENT)) {
            sceGuEnable(GU_ALPHA_TEST);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        } else {
            sceGuDisable(GU_ALPHA_TEST);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
        }
        sceGumDrawArray(GU_TRIANGLES, VTYPE, mesh->num_indices, lib->indices + mesh->first_index,
                        lib->verts + mesh->first_vertex);
    }
}
