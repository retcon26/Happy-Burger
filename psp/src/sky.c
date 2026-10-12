#include "sky.h"

#include <math.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>

#define SEGMENTS 16 // 22.5 degrees each, small enough that no on-screen triangle hits the GE's guard band
#define RINGS 7     // from 30 degrees below the horizon up to the zenith
#define RADIUS 800.0f
#define SUN_SIZE 60.0f
#define BOX_DIV 8   // each skybox face is cut into 8x8 quads, for the same guard-band reason

typedef struct {
    unsigned int color;
    float x, y, z;
} SkyVert;

typedef struct {
    float u, v;
    float x, y, z;
} BoxVert;

#define SKY_VTYPE (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define BOX_VTYPE (GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

static SkyVert __attribute__((aligned(16))) g_dome[SEGMENTS * RINGS * 6];
static int g_dome_count;
static SkyVert __attribute__((aligned(16))) g_sun[6];
static BoxVert __attribute__((aligned(16))) g_box[6][BOX_DIV * BOX_DIV * 6];
static const Map *g_map;
static int g_use_box;

static unsigned int lerp_abgr(unsigned int a, unsigned int b, float t) {
    unsigned int out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        float ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
        out |= ((unsigned int)(ca + (cb - ca) * t) & 0xFF) << s;
    }
    return out;
}

static SkyVert dome_vert(int seg, int ring, unsigned int top, unsigned int bottom) {
    // Ring 0 is below the horizon; the last ring is the zenith.
    float elev = (-30.0f + 120.0f * (float)ring / (float)RINGS) * (3.14159265f / 180.0f);
    if (elev > 1.5707963f) elev = 1.5707963f;
    float az = (float)seg * (2.0f * 3.14159265f / (float)SEGMENTS);
    float ce = cosf(elev);
    SkyVert v;
    v.x = RADIUS * ce * cosf(az);
    v.y = RADIUS * ce * sinf(az);
    v.z = RADIUS * sinf(elev);
    // GMod's skypaint fades quickly from the horizon colour into the top colour.
    float t = elev <= 0.0f ? 0.0f : sqrtf(sinf(elev));
    v.color = lerp_abgr(bottom, top, t);
    return v;
}

static void build_dome(const PmapInfo *info) {
    int n = 0;
    for (int r = 0; r < RINGS; r++) {
        for (int s = 0; s < SEGMENTS; s++) {
            SkyVert a = dome_vert(s, r, info->sky_top_abgr, info->sky_bottom_abgr);
            SkyVert b = dome_vert(s + 1, r, info->sky_top_abgr, info->sky_bottom_abgr);
            SkyVert c = dome_vert(s + 1, r + 1, info->sky_top_abgr, info->sky_bottom_abgr);
            SkyVert d = dome_vert(s, r + 1, info->sky_top_abgr, info->sky_bottom_abgr);
            g_dome[n++] = a;
            g_dome[n++] = b;
            g_dome[n++] = c;
            g_dome[n++] = a;
            g_dome[n++] = c;
            g_dome[n++] = d;
        }
    }
    g_dome_count = n;

    // Sun: a quad facing the camera, centred along sun_dir
    float sx = info->sun_dir[0], sy = info->sun_dir[1], sz = info->sun_dir[2];
    float ax = -sy, ay = sx, az = 0.0f; // horizontal tangent
    float al = sqrtf(ax * ax + ay * ay);
    if (al < 1e-3f) {
        ax = 1.0f;
        ay = 0.0f;
        al = 1.0f;
    }
    ax /= al;
    ay /= al;
    // bitangent = sun x tangent
    float bx = sy * az - sz * ay, by = sz * ax - sx * az, bz = sx * ay - sy * ax;
    float cx = sx * (RADIUS * 0.9f), cy = sy * (RADIUS * 0.9f), cz = sz * (RADIUS * 0.9f);
    unsigned int sun = 0xFFD0F8FF; // warm white
    float k[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    SkyVert q[4];
    for (int i = 0; i < 4; i++) {
        q[i].color = sun;
        q[i].x = cx + (ax * k[i][0] + bx * k[i][1]) * SUN_SIZE;
        q[i].y = cy + (ay * k[i][0] + by * k[i][1]) * SUN_SIZE;
        q[i].z = cz + (az * k[i][0] + bz * k[i][1]) * SUN_SIZE;
    }
    g_sun[0] = q[0];
    g_sun[1] = q[1];
    g_sun[2] = q[2];
    g_sun[3] = q[0];
    g_sun[4] = q[2];
    g_sun[5] = q[3];
}

// Quake/Source skybox face layout: for each face (rt bk lf ft up dn), which of (s, t, 1) feeds x, y, z
// (1-based index, negative = flipped). The same table Quake 2's sky code uses.
static const int k_st_to_vec[6][3] = {
    {3, -1, 2}, {-3, 1, 2}, {1, 3, 2}, {-1, -3, 2}, {-2, -1, 3}, {2, -1, -3},
};

static BoxVert box_vert(int face, float s, float t) {
    float b[3] = {s * RADIUS, t * RADIUS, RADIUS};
    float v[3];
    for (int j = 0; j < 3; j++) {
        int k = k_st_to_vec[face][j];
        v[j] = k < 0 ? -b[-k - 1] : b[k - 1];
    }
    BoxVert o;
    // Texture s,t run 0..1 with t flipped (image rows go down); stay half a texel off the edges
    o.u = 0.002f + (s + 1.0f) * 0.5f * 0.996f;
    o.v = 0.002f + (1.0f - (t + 1.0f) * 0.5f) * 0.996f;
    o.x = v[0];
    o.y = v[1];
    o.z = v[2];
    return o;
}

static void build_box(void) {
    for (int f = 0; f < 6; f++) {
        int n = 0;
        for (int j = 0; j < BOX_DIV; j++) {
            for (int i = 0; i < BOX_DIV; i++) {
                float s0 = -1.0f + 2.0f * i / BOX_DIV, s1 = -1.0f + 2.0f * (i + 1) / BOX_DIV;
                float t0 = -1.0f + 2.0f * j / BOX_DIV, t1 = -1.0f + 2.0f * (j + 1) / BOX_DIV;
                BoxVert a = box_vert(f, s0, t0), b = box_vert(f, s1, t0), c = box_vert(f, s1, t1), d = box_vert(f, s0, t1);
                g_box[f][n++] = a;
                g_box[f][n++] = b;
                g_box[f][n++] = c;
                g_box[f][n++] = a;
                g_box[f][n++] = c;
                g_box[f][n++] = d;
            }
        }
    }
}

void sky_build(const Map *m) {
    g_map = m;
    g_use_box = m->sky_first_tex >= 0 && m->sky_first_tex + 6 <= m->num_textures;
    if (g_use_box) {
        build_box();
    } else {
        build_dome(m->info);
    }
    sceKernelDcacheWritebackAll();
}

void sky_draw(const Camera *cam) {
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_CULL_FACE);

    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumTranslate(&cam->pos);
    if (g_use_box) {
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        for (int f = 0; f < 6; f++) {
            map_bind_texture(g_map, g_map->sky_first_tex + f);
            sceGumDrawArray(GU_TRIANGLES, BOX_VTYPE, BOX_DIV * BOX_DIV * 6, 0, g_box[f]);
        }
        sceGuDisable(GU_TEXTURE_2D);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    } else {
        sceGuDisable(GU_TEXTURE_2D);
        sceGumDrawArray(GU_TRIANGLES, SKY_VTYPE, g_dome_count, 0, g_dome);
        sceGumDrawArray(GU_TRIANGLES, SKY_VTYPE, 6, 0, g_sun);
    }
    sceGumLoadIdentity();

    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_DEPTH_TEST);
}
