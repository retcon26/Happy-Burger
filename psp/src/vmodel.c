#include "vmodel.h"

#include <pspgu.h>
#include <pspgum.h>
#include <math.h>
#include <string.h>

#include "gfx.h"
#include "mem.h"
#include "pak.h"
#include "pmap_format.h"

#define MAX_PALETTE 96

typedef struct {
    char name[24];
    uint32_t first_mesh, num_meshes;
    uint32_t num_bones;
    uint32_t seq[VM_KINDS];
    float mins[3], maxs[3];
} VmModel;

typedef struct {
    uint32_t first_vertex, num_vertices, first_index, num_indices;
    uint16_t tex, flags;
} VmMesh;

typedef struct {
    float u, v, nx, ny, nz, x, y, z;
    uint32_t bone;
} VmVertex;

typedef struct {
    uint32_t num_frames;
    float fps;
    uint32_t loop;
    uint32_t first_matrix;
} VmSeq;

typedef struct {
    float u, v, nx, ny, nz, x, y, z;
} DrawVert;

static void *g_file;
static const VmModel *g_models;
static int g_num_models;
static const VmMesh *g_meshes;
static const VmVertex *g_verts;
static const uint16_t *g_indices;
static const VmSeq *g_seqs;
static const int16_t *g_mats; // 12 per bone per frame: rotation / 16384, position / 64
static const PmapTexture *g_textures;

// What's playing on the weapon in hand
static struct {
    int vm, kind, loop;
    float time;
} P = {-1, VM_IDLE, 0, 0.0f};

int vmodels_load(void) {
#ifdef PS2
    const char *path = "data/vmodels.low.pvmd"; // the PS2's 32 MB goes further on maps
#else
    const char *path = mem_tier() == MEM_TIER_HIGH ? "data/vmodels.pvmd" : "data/vmodels.low.pvmd";
#endif
    if (!pak_file_exists(path)) path = "data/vmodels.pvmd";
    if (!pak_file_exists(path)) return -1;
    unsigned int size;
    g_file = pak_read_file(path, &size);
    if (!g_file) return -1;
    const unsigned char *base = g_file;
#define SEC(var, type, a, b, c, d)                                                  \
    do {                                                                            \
        const PmapSection *s = pak_find(g_file, PMAP_SECTION_ID(a, b, c, d));       \
        if (!s) {                                                                   \
            g_num_models = 0;                                                       \
            return -2;                                                              \
        }                                                                           \
        var = (const type *)(base + s->offset);                                     \
    } while (0)
    SEC(g_models, VmModel, 'V', 'M', 'D', 'L');
    SEC(g_meshes, VmMesh, 'M', 'E', 'S', 'H');
    SEC(g_verts, VmVertex, 'V', 'E', 'R', 'T');
    SEC(g_indices, uint16_t, 'I', 'N', 'D', 'X');
    SEC(g_seqs, VmSeq, 'S', 'E', 'Q', 'S');
    SEC(g_mats, int16_t, 'M', 'A', 'T', 'Q');
    SEC(g_textures, PmapTexture, 'T', 'E', 'X', 'S');
#undef SEC
    g_num_models = (int)pak_find(g_file, PMAP_SECTION_ID('V', 'M', 'D', 'L'))->count;
    return 0;
}

int vmodel_find(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < g_num_models; i++) {
        if (strncmp(g_models[i].name, name, sizeof(g_models[i].name)) == 0) return i;
    }
    return -1;
}

static const VmSeq *seq_of(int vm, int kind) {
    if (vm < 0 || vm >= g_num_models || kind < 0 || kind >= VM_KINDS) return NULL;
    uint32_t s = g_models[vm].seq[kind];
    return s == 0xFFFFFFFFu ? NULL : &g_seqs[s];
}

int vmodel_has(int vm, int kind) {
    return seq_of(vm, kind) != NULL;
}

float vmodel_length(int vm, int kind) {
    const VmSeq *s = seq_of(vm, kind);
    if (!s || s->fps <= 0.0f) return 0.0f;
    return (float)(s->loop ? s->num_frames : s->num_frames - 1) / s->fps;
}

void vmodel_play(int vm, int kind, int loop) {
    if (!seq_of(vm, kind)) kind = VM_IDLE;
    P.vm = vm;
    P.kind = kind;
    P.loop = loop;
    P.time = 0.0f;
}

int vmodel_playing(void) {
    return P.kind;
}

void vmodel_update(float dt) {
    P.time += dt;
    if (P.kind != VM_IDLE && !P.loop && P.time >= vmodel_length(P.vm, P.kind)) {
        P.kind = VM_IDLE; // a one-shot finished: back to breathing
        P.time = 0.0f;
    }
}

// The bone matrices for this frame, blended between the two nearest baked frames
static void pose(int vm, float (*out)[12]) {
    const VmModel *md = &g_models[vm];
    int nb = (int)md->num_bones < MAX_PALETTE ? (int)md->num_bones : MAX_PALETTE;
    int kind = P.vm == vm ? P.kind : VM_IDLE;
    float time = P.vm == vm ? P.time : 0.0f;
    const VmSeq *s = seq_of(vm, kind);
    if (!s) s = seq_of(vm, VM_IDLE);
    if (!s) {
        for (int b = 0; b < nb; b++) {
            static const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            memcpy(out[b], id, sizeof(id));
        }
        return;
    }
    int nf = (int)s->num_frames;
    float f = time * s->fps;
    int f0, f1;
    float t;
    if (s->loop || P.loop) {
        float w = fmodf(f, (float)nf);
        f0 = (int)w;
        f1 = (f0 + 1) % nf;
        t = w - (float)f0;
    } else {
        if (f > (float)(nf - 1)) f = (float)(nf - 1);
        f0 = (int)f;
        f1 = f0 + 1 < nf ? f0 + 1 : f0;
        t = f - (float)f0;
    }
    const int16_t *a = g_mats + ((size_t)s->first_matrix + (size_t)f0 * md->num_bones) * 12;
    const int16_t *b = g_mats + ((size_t)s->first_matrix + (size_t)f1 * md->num_bones) * 12;
    for (int k = 0; k < nb; k++) {
        for (int j = 0; j < 12; j++) {
            float sc = (j & 3) == 3 ? 1.0f / 64.0f : 1.0f / 16384.0f;
            float va = (float)a[k * 12 + j], vb = (float)b[k * 12 + j];
            out[k][j] = (va + (vb - va) * t) * sc;
        }
    }
}

void vmodel_draw(int vm, const ScePspFMatrix4 *world, const PropLight *light) {
    if (vm < 0 || vm >= g_num_models) return;
    const VmModel *md = &g_models[vm];
    static float palette[MAX_PALETTE][12];
    pose(vm, palette);
    int nb = (int)md->num_bones < MAX_PALETTE ? (int)md->num_bones : MAX_PALETTE;

    proplib_begin();
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &light->sun_dir);
    sceGuLightColor(0, GU_DIFFUSE, light->sun_abgr);
    sceGuAmbient(light->ambient_abgr);
    sceGuColor(light->tint_abgr);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadMatrix(world);
    for (uint32_t i = 0; i < md->num_meshes; i++) {
        const VmMesh *mesh = &g_meshes[md->first_mesh + i];
        DrawVert *out = sceGuGetMemory(mesh->num_vertices * sizeof(DrawVert));
        const VmVertex *src = g_verts + mesh->first_vertex;
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        // CPU skinning: each vertex follows its bone's matrix for this frame
        for (uint32_t v = 0; v < mesh->num_vertices; v++) {
            const VmVertex *sv = &src[v];
            const float *p = palette[(int)sv->bone < nb ? sv->bone : 0];
            DrawVert *o = &out[v];
            o->u = sv->u;
            o->v = sv->v;
            o->nx = p[0] * sv->nx + p[1] * sv->ny + p[2] * sv->nz;
            o->ny = p[4] * sv->nx + p[5] * sv->ny + p[6] * sv->nz;
            o->nz = p[8] * sv->nx + p[9] * sv->ny + p[10] * sv->nz;
            o->x = p[0] * sv->x + p[1] * sv->y + p[2] * sv->z + p[3];
            o->y = p[4] * sv->x + p[5] * sv->y + p[6] * sv->z + p[7];
            o->z = p[8] * sv->x + p[9] * sv->y + p[10] * sv->z + p[11];
            if (o->x < lo[0]) lo[0] = o->x;
            if (o->x > hi[0]) hi[0] = o->x;
            if (o->y < lo[1]) lo[1] = o->y;
            if (o->y > hi[1]) hi[1] = o->y;
            if (o->z < lo[2]) lo[2] = o->z;
            if (o->z > hi[2]) hi[2] = o->z;
        }
        pak_bind_texture(g_file, &g_textures[mesh->tex]);
        if (mesh->flags & (PMAP_BATCH_ALPHATEST | PMAP_BATCH_TRANSLUCENT)) {
            sceGuEnable(GU_ALPHA_TEST);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        } else {
            sceGuDisable(GU_ALPHA_TEST);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
        }
#ifdef PS2
        gu_hint_bounds_model(lo, hi); // VU1 draws it when it needs no clipping (ps2/gu.c)
#else
        (void)lo;
        (void)hi;
#endif
        sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
                        mesh->num_indices, g_indices + mesh->first_index, out);
    }
    proplib_end();
}
