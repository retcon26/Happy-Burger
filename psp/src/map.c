#include "map.h"
#include "ents.h"
#include "gfx.h"
#include "mem.h"
#include "pak.h"

#include <math.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_DIR "data/maps"
#define VTYPE (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D)
#define DEG2RAD (3.14159265f / 180.0f)

// The GE fetches textures from VRAM much faster than from main RAM, but only ~1.2MB of VRAM is
// left after the frame buffers. Fill it with the textures covering the most triangles.
static void cache_textures_in_vram(Map *m) {
    int n = m->num_textures;
    unsigned int *usage = calloc(n > 0 ? n : 1, sizeof(unsigned int));
    int *order = calloc(n > 0 ? n : 1, sizeof(int));
    if (!usage || !order) {
        free(usage);
        free(order);
        return;
    }
    for (int i = 0; i < m->num_batches; i++) usage[m->batches[i].tex] += m->batches[i].num_indices;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++) { // insertion sort, busiest first
        int k = order[i], j = i - 1;
        while (j >= 0 && usage[order[j]] < usage[k]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }
    unsigned int ofs = gfx_vram_free_offset();
    unsigned char *vram_cpu = (unsigned char *)(0x40000000u | (unsigned int)sceGeEdramGetAddr()); // uncached
    unsigned char *vram_ge = (unsigned char *)sceGeEdramGetAddr();
    for (int oi = 0; oi < n; oi++) {
        int t = order[oi];
        if (!usage[t]) break;
        const PmapTexture *tx = &m->textures[t];
        unsigned int need = 1024;
        for (int l = 0; l < tx->levels; l++) need += (unsigned int)(tx->width >> l) * (unsigned int)(tx->height >> l);
        if (ofs + need > GFX_VRAM_SIZE) continue; // try smaller ones
        memcpy(vram_cpu + ofs, m->texref[t].clut, 1024);
        m->texref[t].clut = vram_ge + ofs;
        ofs += 1024;
        for (int l = 0; l < tx->levels; l++) {
            unsigned int sz = (unsigned int)(tx->width >> l) * (unsigned int)(tx->height >> l);
            memcpy(vram_cpu + ofs, m->texref[t].level[l], sz);
            m->texref[t].level[l] = vram_ge + ofs;
            ofs += (sz + 15) & ~15u;
        }
        m->vram_textures++;
        m->vram_bytes += need;
    }
    free(usage);
    free(order);
}

// Maps that reuse another map's files (the game changes them after loading): name -> files
static const char *const k_aliases[][2] = {
    {"gm_construct_13", "gm_construct"}, // the haunted night-time gm_construct (game.c)
};
#define NUM_ALIASES (int)(sizeof(k_aliases) / sizeof(k_aliases[0]))

const char *map_file_name(const char *name) {
    for (int i = 0; i < NUM_ALIASES; i++) {
        if (strcmp(name, k_aliases[i][0]) == 0) return k_aliases[i][1];
    }
    return name;
}

int map_load(Map *m, const char *name, char *err, int err_len) {
    memset(m, 0, sizeof(*m));
    name = map_file_name(name);
    char hi[96], lo[96];
    snprintf(hi, sizeof(hi), MAP_DIR "/%s.pmap", name);
    snprintf(lo, sizeof(lo), MAP_DIR "/%s.low.pmap", name);
    // Full detail first whatever the memory tier; the fallback below takes the reduced map if it won't fit
    const char *path = hi;
    if (!pak_file_exists(path)) {
        path = path == hi ? lo : hi;
    }

    m->file = pak_read_file(path, &m->file_size);
    if (!m->file && path == hi && pak_file_exists(lo)) {
        path = lo; // not enough RAM left for the full-detail map: take the reduced one
        m->file = pak_read_file(path, &m->file_size);
    }
    if (!m->file) {
        // pak_read_file says why: a missing file, no memory, or a failed read
        snprintf(err, err_len, "can't load %s: %s", path, pak_last_error()[0] ? pak_last_error() : "not found");
        return -1;
    }
    const PmapHeader *h = m->file;
    if (memcmp(h->magic, PMAP_MAGIC, 4) != 0 || h->version != PMAP_VERSION) {
        snprintf(err, err_len, "%s: not a v%d pmap", path, PMAP_VERSION);
        map_free(m);
        return -2;
    }

    const unsigned char *base = m->file;
#define SECTION(var, type, a, b, c, d, countvar)                               \
    do {                                                                       \
        const PmapSection *s = pak_find(h, PMAP_SECTION_ID(a, b, c, d));       \
        if (!s) {                                                              \
            snprintf(err, err_len, "%s: missing %c%c%c%c", path, a, b, c, d);  \
            map_free(m);                                                       \
            return -3;                                                         \
        }                                                                      \
        var = (const type *)(base + s->offset);                                \
        countvar = (int)s->count;                                              \
    } while (0)

    int unused;
    SECTION(m->info, PmapInfo, 'I', 'N', 'F', 'O', unused);
    SECTION(m->spawns, PmapSpawn, 'S', 'P', 'W', 'N', m->num_spawns);
    SECTION(m->textures, PmapTexture, 'T', 'E', 'X', 'S', m->num_textures);
    SECTION(m->verts, PmapVertex, 'V', 'E', 'R', 'T', unused);
    SECTION(m->indices, uint16_t, 'I', 'N', 'D', 'X', unused);
    SECTION(m->batches, PmapBatch, 'B', 'T', 'C', 'H', m->num_batches);
    SECTION(m->planes, PmapPlane, 'P', 'L', 'A', 'N', unused);
    SECTION(m->nodes, PmapNode, 'N', 'O', 'D', 'E', m->num_nodes);
    SECTION(m->leafs, PmapLeaf, 'L', 'E', 'A', 'F', m->num_leafs);
    SECTION(m->pvs, uint32_t, 'P', 'V', 'S', ' ', m->num_clusters);
    SECTION(m->brushes, PmapBrush, 'B', 'R', 'S', 'H', m->num_brushes);
    SECTION(m->brush_sides, uint32_t, 'B', 'S', 'I', 'D', unused);
    SECTION(m->leaf_brushes, uint32_t, 'L', 'B', 'R', 'S', unused);
    SECTION(m->ents, PmapEnt, 'E', 'N', 'T', 'S', m->num_ents);
    SECTION(m->ent_batches, PmapBatch, 'E', 'B', 'A', 'T', unused);
    SECTION(m->ent_brushes, uint32_t, 'E', 'B', 'R', 'L', unused);
    SECTION(m->outputs, PmapOutput, 'E', 'O', 'U', 'T', unused);
    SECTION(m->sprops, PmapStaticProp, 'S', 'P', 'R', 'P', m->num_sprops);
    SECTION(m->spvs, uint32_t, 'S', 'P', 'V', 'S', unused);
    SECTION(m->phys_props, PmapPhysProp, 'P', 'H', 'Y', 'S', m->num_phys_props);
    SECTION(m->lstyles, PmapLightStyle, 'L', 'S', 'T', 'Y', m->num_lstyles);
    SECTION(m->lpatterns, PmapLightPattern, 'L', 'S', 'T', 'P', unused);
    const int32_t *skyt;
    SECTION(skyt, int32_t, 'S', 'K', 'Y', 'T', unused);
    m->sky_first_tex = *skyt;
#undef SECTION
    (void)unused;
    memset(m->style_switch, 1, sizeof(m->style_switch));
    memset(m->style_applied, 0, sizeof(m->style_applied)); // the file's colours have every extra style off

    m->brush_check = calloc(m->num_brushes > 0 ? m->num_brushes : 1, sizeof(uint32_t));
    m->texref = calloc(m->num_textures > 0 ? m->num_textures : 1, sizeof(TexRef));
    if (!m->brush_check || !m->texref) {
        snprintf(err, err_len, "out of memory");
        map_free(m);
        return -4;
    }
    for (int t = 0; t < m->num_textures; t++) {
        m->texref[t].clut = base + m->textures[t].clut_ofs;
        for (int l = 0; l < m->textures[t].levels; l++) m->texref[t].level[l] = base + m->textures[t].level_ofs[l];
    }
    cache_textures_in_vram(m);

    // The GE reads textures and vertices straight from RAM, so push everything out of the CPU cache.
    sceKernelDcacheWritebackAll();
    return 0;
}

void map_free(Map *m) {
    if (m->file) {
        free(m->file);
    }
    if (m->brush_check) {
        free(m->brush_check);
    }
    if (m->ent_state) {
        free(m->ent_state);
    }
    if (m->texref) {
        free(m->texref);
    }
    memset(m, 0, sizeof(*m));
}

void map_update_lightstyles(Map *m, float time_s) {
    if (!m->num_lstyles) return;
    unsigned char want[64];
    int changed = 0;
    int step = (int)(time_s * 10.0f);
    for (int s = 0; s < 64; s++) {
        const char *p = m->lpatterns[s];
        int len = (int)strnlen(p, 64);
        int v = 100;
        if (len) v = (p[step % len] - 'a') * 100 / ('m' - 'a'); // 'a' = 0, 'm' = 100, 'z' = 208
        if (!m->style_switch[s]) v = 0;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        want[s] = (unsigned char)v;
        changed |= want[s] != m->style_applied[s];
    }
    if (!changed) return;
    PmapVertex *verts = (PmapVertex *)m->verts;
    for (int i = 0; i < m->num_lstyles;) {
        const PmapLightStyle *first = &m->lstyles[i];
        uint32_t base = first->base;
        int r = (int)(base & 0xFF), g = (int)((base >> 8) & 0xFF), b = (int)((base >> 16) & 0xFF);
        while (i < m->num_lstyles && m->lstyles[i].vertex == first->vertex) {
            const PmapLightStyle *ls = &m->lstyles[i];
            int k = want[ls->style & 63];
            r += ls->dr * k / 100;
            g += ls->dg * k / 100;
            b += ls->db * k / 100;
            i++;
        }
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        verts[first->vertex].color = (base & 0xFF000000u) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    }
    memcpy(m->style_applied, want, sizeof(want));
    sceKernelDcacheWritebackAll(); // the GE reads vertex colours from RAM
}

int map_point_leaf(const Map *m, const ScePspFVector3 *p) {
    if (m->num_nodes == 0) {
        return 0;
    }
    int n = 0;
    while (n >= 0) {
        const PmapNode *node = &m->nodes[n];
        const PmapPlane *pl = &m->planes[node->plane];
        float d = pl->nx * p->x + pl->ny * p->y + pl->nz * p->z - pl->dist;
        n = node->children[d >= 0.0f ? 0 : 1];
    }
    return -1 - n;
}

typedef struct {
    float nx, ny, nz, d;
} FPlane;

// Four side planes plus a far plane at the fog distance. Normals point into the view volume.
static void build_frustum(const Camera *cam, float far_dist, FPlane out[5]) {
    ScePspFVector3 f, r;
    camera_vectors(cam, &f, &r);
    // up = r x f
    ScePspFVector3 u = {r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
    float hy = cam->fov_y * 0.5f * DEG2RAD;
    float hx = atanf(tanf(hy) * (480.0f / 272.0f));
    // Widen a little so batches at the screen edge don't pop.
    hx += 0.05f;
    hy += 0.05f;
    float sx = sinf(hx), cx = cosf(hx), sy = sinf(hy), cy = cosf(hy);
    ScePspFVector3 n[5] = {
        {f.x * sx + r.x * cx, f.y * sx + r.y * cx, f.z * sx + r.z * cx},
        {f.x * sx - r.x * cx, f.y * sx - r.y * cx, f.z * sx - r.z * cx},
        {f.x * sy - u.x * cy, f.y * sy - u.y * cy, f.z * sy - u.z * cy},
        {f.x * sy + u.x * cy, f.y * sy + u.y * cy, f.z * sy + u.z * cy},
        {-f.x, -f.y, -f.z},
    };
    const ScePspFVector3 *e = &cam->pos;
    for (int i = 0; i < 5; i++) {
        out[i].nx = n[i].x;
        out[i].ny = n[i].y;
        out[i].nz = n[i].z;
        out[i].d = n[i].x * e->x + n[i].y * e->y + n[i].z * e->z;
    }
    out[4].d -= far_dist;
}

static int box_visible(const FPlane pl[5], const float mins[3], const float maxs[3]) {
    for (int i = 0; i < 5; i++) {
        float x = pl[i].nx >= 0.0f ? maxs[0] : mins[0];
        float y = pl[i].ny >= 0.0f ? maxs[1] : mins[1];
        float z = pl[i].nz >= 0.0f ? maxs[2] : mins[2];
        if (pl[i].nx * x + pl[i].ny * y + pl[i].nz * z < pl[i].d) {
            return 0;
        }
    }
    return 1;
}

// --- Near-camera clipping -------------------------------------------------------------------
// The GE has no real clipper: a triangle with a vertex behind the camera, or one that projects
// more than ~2048 pixels off-screen, is dropped whole, so floors and walls next to the player
// flicker out. Batches close to the camera are clipped on the CPU against a near plane and a
// widened "guard band" frustum, and only the clipped pieces are sent.

#define NEAR_ZONE 640.0f   // batches whose box is within this distance get CPU clipping
#define GUARD_SCALE 5.0f   // guard frustum is this many screen-widths wide (GE limit is ~8.5)
#define CLIP_NEAR_DIST 6.0f
#define CLIP_SCRATCH 6144  // max clipped vertices per frame
#define VTYPE_CLIP (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

// Full-precision vertex for the CPU clipper; packed vertices are expanded into these.
typedef struct {
    float u, v;
    uint32_t color;
    float x, y, z;
} ClipVertex;

static ClipVertex g_clip_scratch[CLIP_SCRATCH];

static ClipVertex unpack_vertex(const PmapBatch *b, const PmapVertex *v) {
    float step = (float)b->step_fp * (1.0f / 65536.0f);
    ClipVertex o;
    o.u = b->uv_off[0] + (float)v->u * b->uv_scale[0] * (1.0f / 32768.0f);
    o.v = b->uv_off[1] + (float)v->v * b->uv_scale[1] * (1.0f / 32768.0f);
    o.color = v->color;
    o.x = b->origin[0] + (float)v->x * step;
    o.y = b->origin[1] + (float)v->y * step;
    o.z = b->origin[2] + (float)v->z * step;
    return o;
}

void map_batch_matrix(const PmapBatch *b, const ScePspFMatrix4 *parent, ScePspFMatrix4 *out) {
    // The GE reads 16-bit positions as value/32768, so scale by step * 32768 and move to origin.
    float s = (float)b->step_fp * (1.0f / 65536.0f) * 32768.0f;
    if (!parent) {
        out->x = (ScePspFVector4){s, 0.0f, 0.0f, 0.0f};
        out->y = (ScePspFVector4){0.0f, s, 0.0f, 0.0f};
        out->z = (ScePspFVector4){0.0f, 0.0f, s, 0.0f};
        out->w = (ScePspFVector4){b->origin[0], b->origin[1], b->origin[2], 1.0f};
        return;
    }
    const ScePspFMatrix4 *p = parent;
    out->x = (ScePspFVector4){p->x.x * s, p->x.y * s, p->x.z * s, 0.0f};
    out->y = (ScePspFVector4){p->y.x * s, p->y.y * s, p->y.z * s, 0.0f};
    out->z = (ScePspFVector4){p->z.x * s, p->z.y * s, p->z.z * s, 0.0f};
    const float *o = b->origin;
    out->w = (ScePspFVector4){p->x.x * o[0] + p->y.x * o[1] + p->z.x * o[2] + p->w.x,
                              p->x.y * o[0] + p->y.y * o[1] + p->z.y * o[2] + p->w.y,
                              p->x.z * o[0] + p->y.z * o[1] + p->z.z * o[2] + p->w.z, 1.0f};
}

void map_batch_begin(const PmapBatch *b, const ScePspFMatrix4 *parent) {
    ScePspFMatrix4 mat;
    map_batch_matrix(b, parent, &mat);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadMatrix(&mat);
    sceGumUpdateMatrix();
    sceGuTexScale(b->uv_scale[0], b->uv_scale[1]);
    sceGuTexOffset(b->uv_off[0], b->uv_off[1]);
}

void map_batch_end(void) {
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumUpdateMatrix();
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
}

void map_bind_texture(const Map *m, int ti) {
    const PmapTexture *t = &m->textures[ti];
    const TexRef *r = &m->texref[ti];
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(256 / 8, r->clut);
    sceGuTexMode(GU_PSM_T8, t->levels - 1, 0, GU_TRUE);
    for (int l = 0; l < t->levels; l++) {
        int w = t->width >> l, h = t->height >> l;
        sceGuTexImage(l, w, h, w, r->level[l]);
    }
}

static void build_guard(const Camera *cam, FPlane out[5]) {
    ScePspFVector3 f, r;
    camera_vectors(cam, &f, &r);
    ScePspFVector3 u = {r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
    float ty = tanf(cam->fov_y * 0.5f * DEG2RAD);
    float hy = atanf(ty * GUARD_SCALE);
    float hx = atanf(ty * (480.0f / 272.0f) * GUARD_SCALE);
    float sx = sinf(hx), cx = cosf(hx), sy = sinf(hy), cy = cosf(hy);
    ScePspFVector3 n[5] = {
        {f.x * sx + r.x * cx, f.y * sx + r.y * cx, f.z * sx + r.z * cx},
        {f.x * sx - r.x * cx, f.y * sx - r.y * cx, f.z * sx - r.z * cx},
        {f.x * sy - u.x * cy, f.y * sy - u.y * cy, f.z * sy - u.z * cy},
        {f.x * sy + u.x * cy, f.y * sy + u.y * cy, f.z * sy + u.z * cy},
        {f.x, f.y, f.z},
    };
    const ScePspFVector3 *e = &cam->pos;
    for (int i = 0; i < 5; i++) {
        out[i].nx = n[i].x;
        out[i].ny = n[i].y;
        out[i].nz = n[i].z;
        out[i].d = n[i].x * e->x + n[i].y * e->y + n[i].z * e->z;
    }
    out[4].d += CLIP_NEAR_DIST;
}

static int box_near(const Camera *cam, const float mins[3], const float maxs[3]) {
    const float p[3] = {cam->pos.x, cam->pos.y, cam->pos.z};
    float d2 = 0.0f;
    for (int k = 0; k < 3; k++) {
        float d = p[k] < mins[k] ? mins[k] - p[k] : (p[k] > maxs[k] ? p[k] - maxs[k] : 0.0f);
        d2 += d * d;
    }
    return d2 < NEAR_ZONE * NEAR_ZONE;
}

static inline float plane_dist(const FPlane *p, const ClipVertex *v) {
    return p->nx * v->x + p->ny * v->y + p->nz * v->z - p->d;
}

static ClipVertex lerp_vertex(const ClipVertex *a, const ClipVertex *b, float t) {
    ClipVertex o;
    o.u = a->u + (b->u - a->u) * t;
    o.v = a->v + (b->v - a->v) * t;
    o.x = a->x + (b->x - a->x) * t;
    o.y = a->y + (b->y - a->y) * t;
    o.z = a->z + (b->z - a->z) * t;
    uint32_t c = 0;
    for (int s = 0; s < 32; s += 8) {
        float ca = (float)((a->color >> s) & 0xFF), cb = (float)((b->color >> s) & 0xFF);
        c |= ((uint32_t)(ca + (cb - ca) * t) & 0xFF) << s;
    }
    o.color = c;
    return o;
}

// Sutherland-Hodgman against one plane; keeps the side where plane_dist >= 0.
static int clip_poly(const ClipVertex *in, int n, ClipVertex *out, const FPlane *pl) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const ClipVertex *a = &in[i], *b = &in[(i + 1) % n];
        float da = plane_dist(pl, a), db = plane_dist(pl, b);
        if (da >= 0.0f) out[m++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) out[m++] = lerp_vertex(a, b, da / (da - db));
    }
    return m;
}

static void draw_near_batch(const Map *m, const PmapBatch *b, const FPlane guard[5], int *scratch_used,
                            MapDrawStats *stats) {
    const uint16_t *idx = m->indices + b->first_index;
    const PmapVertex *vb = m->verts + b->first_vertex;
    int ntri = (int)b->num_indices / 3;
    uint16_t *keep = sceGuGetMemory(((ntri * 3 * sizeof(uint16_t)) + 3) & ~3);
    int nkeep = 0;
    int first_clipped = *scratch_used;

    for (int t = 0; t < ntri; t++) {
        ClipVertex tv[3] = {unpack_vertex(b, &vb[idx[t * 3]]), unpack_vertex(b, &vb[idx[t * 3 + 1]]),
                            unpack_vertex(b, &vb[idx[t * 3 + 2]])};
        unsigned int crossing = 0;
        int culled = 0;
        for (int p = 0; p < 5; p++) {
            int out = (plane_dist(&guard[p], &tv[0]) < 0.0f) + (plane_dist(&guard[p], &tv[1]) < 0.0f) +
                      (plane_dist(&guard[p], &tv[2]) < 0.0f);
            if (out == 3) {
                culled = 1;
                break;
            }
            if (out) crossing |= 1u << p;
        }
        if (culled) continue;
        if (!crossing) {
            keep[nkeep++] = idx[t * 3];
            keep[nkeep++] = idx[t * 3 + 1];
            keep[nkeep++] = idx[t * 3 + 2];
            continue;
        }
        ClipVertex buf_a[12], buf_b[12];
        ClipVertex *cur = buf_a, *nxt = buf_b;
        cur[0] = tv[0];
        cur[1] = tv[1];
        cur[2] = tv[2];
        int n = 3;
        for (int p = 0; p < 5 && n >= 3; p++) {
            if (crossing & (1u << p)) {
                n = clip_poly(cur, n, nxt, &guard[p]);
                ClipVertex *tmp = cur;
                cur = nxt;
                nxt = tmp;
            }
        }
        if (n < 3 || *scratch_used + (n - 2) * 3 > CLIP_SCRATCH) continue;
        for (int k = 1; k < n - 1; k++) {
            g_clip_scratch[(*scratch_used)++] = cur[0];
            g_clip_scratch[(*scratch_used)++] = cur[k];
            g_clip_scratch[(*scratch_used)++] = cur[k + 1];
        }
    }

    if (nkeep) {
        map_batch_begin(b, NULL);
        sceGuDrawArray(GU_TRIANGLES, VTYPE, nkeep, keep, vb);
    }
    int nclip = *scratch_used - first_clipped;
    if (nclip) {
        // Clipped pieces are already in world space at full precision
        map_batch_end();
        ClipVertex *dst = sceGuGetMemory(nclip * sizeof(ClipVertex));
        memcpy(dst, &g_clip_scratch[first_clipped], nclip * sizeof(ClipVertex));
        sceGuDrawArray(GU_TRIANGLES, VTYPE_CLIP, nclip, 0, dst);
    }
    stats->tris_drawn += (nkeep + nclip) / 3;
    stats->clipped_tris += nclip / 3;
}


// Options > Draw distance: nothing further than this is drawn (the game pulls the fog in to match)
static float g_view_limit = 1e9f;

void map_set_view_limit(float dist) {
    g_view_limit = dist;
}

void map_draw(const Map *m, const Camera *cam, MapDrawStats *stats) {
    memset(stats, 0, sizeof(*stats));
    int leaf = map_point_leaf(m, &cam->pos);
    int cluster = (leaf >= 0 && leaf < m->num_leafs) ? m->leafs[leaf].cluster : -1;
    stats->cluster = cluster;
    // Outside the map or inside a wall there's no PVS row, so everything is a candidate.
    const uint32_t *row = (cluster >= 0 && cluster < m->num_clusters)
                              ? m->pvs + (unsigned int)cluster * m->info->pvs_row_words
                              : NULL;

    FPlane frustum[5], guard[5];
    build_frustum(cam, m->info->fog_end < g_view_limit ? m->info->fog_end : g_view_limit, frustum);
    build_guard(cam, guard);
    int scratch_used = 0;

    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumUpdateMatrix();

    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_FRAGMENT_2X); // vertex lighting is stored at half brightness (see geometry.py)
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR_MIPMAP_LINEAR, GU_LINEAR);
    sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuFrontFace(GU_CCW);
    sceGuEnable(GU_CULL_FACE);
    sceGuAlphaFunc(GU_GREATER, 0x80, 0xFF);
    gfx_flash_world_begin();

    int cur_tex = -1;
    int cur_flags = 0;
    for (int i = 0; i < m->num_batches; i++) {
        if (row && !(row[i >> 5] & (1u << (i & 31)))) {
            continue;
        }
        stats->batches_visible++;
        const PmapBatch *b = &m->batches[i];
        if (!box_visible(frustum, b->mins, b->maxs)) {
            continue;
        }
        if (b->tex != cur_tex) {
            map_bind_texture(m, b->tex);
            cur_tex = b->tex;
            stats->tex_binds++;
        }
        if (b->flags != cur_flags) {
            int changed = b->flags ^ cur_flags;
            if (changed & PMAP_BATCH_ALPHATEST) {
                if (b->flags & PMAP_BATCH_ALPHATEST) {
                    sceGuEnable(GU_ALPHA_TEST);
                } else {
                    sceGuDisable(GU_ALPHA_TEST);
                }
            }
            if (changed & PMAP_BATCH_TRANSLUCENT) {
                if (b->flags & PMAP_BATCH_TRANSLUCENT) {
                    sceGuEnable(GU_BLEND);
                    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
                    sceGuDepthMask(GU_TRUE);
                } else {
                    sceGuDisable(GU_BLEND);
                    sceGuDepthMask(GU_FALSE);
                }
            }
            if (changed & PMAP_BATCH_NOCULL) {
                if (b->flags & PMAP_BATCH_NOCULL) {
                    sceGuDisable(GU_CULL_FACE);
                } else {
                    sceGuEnable(GU_CULL_FACE);
                }
            }
            sceGuTexFunc(GU_TFX_MODULATE,
                         (b->flags & (PMAP_BATCH_ALPHATEST | PMAP_BATCH_TRANSLUCENT)) ? GU_TCC_RGBA : GU_TCC_RGB);
            cur_flags = b->flags;
        }
        stats->batches_drawn++;
        if (box_near(cam, b->mins, b->maxs)) {
            draw_near_batch(m, b, guard, &scratch_used, stats);
        } else {
            map_batch_begin(b, NULL);
            sceGuDrawArray(GU_TRIANGLES, VTYPE, b->num_indices, m->indices + b->first_index,
                           m->verts + b->first_vertex);
            stats->tris_drawn += b->num_indices / 3;
        }
    }
    map_batch_end();

    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_CULL_FACE);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    if (m->ent_state) {
        ents_draw(m, cam, &stats->tris_drawn);
    }
    gfx_flash_world_end();

    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_FRAGMENT_2X);
    sceGuDisable(GU_TEXTURE_2D);
}

int map_list(char names[][32], int max) {
    SceUID d = sceIoDopen(MAP_DIR);
    if (d < 0) {
        return 0;
    }
    int n = 0;
    SceIoDirent ent;
    memset(&ent, 0, sizeof(ent));
    while (n < max && sceIoDread(d, &ent) > 0) {
        const char *nm = ent.d_name;
        size_t len = strlen(nm);
        if (len > 5 && strcmp(nm + len - 5, ".pmap") == 0) {
            size_t stem = len - 5;
            if (stem > 4 && strncmp(nm + stem - 4, ".low", 4) == 0) {
                stem -= 4;
            }
            if (stem < 32) {
                char buf[32];
                memcpy(buf, nm, stem);
                buf[stem] = 0;
                int dup = 0;
                for (int i = 0; i < n; i++) {
                    if (strcmp(names[i], buf) == 0) {
                        dup = 1;
                    }
                }
                if (!dup) {
                    strcpy(names[n++], buf);
                }
            }
        }
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(d);
    // Aliases show up when the map they borrow is there
    for (int a = 0; a < NUM_ALIASES && n < max; a++) {
        for (int i = 0; i < n; i++) {
            if (strcmp(names[i], k_aliases[a][1]) == 0) {
                snprintf(names[n++], 32, "%s", k_aliases[a][0]);
                break;
            }
        }
    }
    return n;
}
