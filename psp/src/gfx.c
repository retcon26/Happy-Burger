#include "gfx.h"

#include <pspdisplay.h>
#include <pspgu.h>

// One display list, rebuilt every frame. Per-frame vertex data from sceGuGetMemory also lives here.
static unsigned int __attribute__((aligned(64))) g_list[256 * 1024];

// Ordered 4x4 dither so 16-bit colour doesn't band on sky and lit walls.
static const ScePspIMatrix4 g_dither = {
    {-4, 0, -3, 1},
    {2, -2, 3, -1},
    {-3, 1, -4, 0},
    {3, -1, 2, -2},
};

static unsigned int g_vram_free; // byte offset of VRAM not used by the frame buffers

unsigned int gfx_vram_free_offset(void) {
    return g_vram_free;
}

void gfx_init(void) {
    void *fbp0 = guGetStaticVramBuffer(BUF_W, SCR_H, GU_PSM_5650);
    void *fbp1 = guGetStaticVramBuffer(BUF_W, SCR_H, GU_PSM_5650);
    void *zbp = guGetStaticVramBuffer(BUF_W, SCR_H, GU_PSM_4444);
    g_vram_free = (unsigned int)zbp + BUF_W * SCR_H * 2;

    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_5650, fbp0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, fbp1, BUF_W);
    sceGuDepthBuffer(zbp, BUF_W);
    sceGuOffset(2048 - (SCR_W / 2), 2048 - (SCR_H / 2));
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuShadeModel(GU_SMOOTH);
    sceGuFrontFace(GU_CW);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuSetDither(&g_dither);
    sceGuEnable(GU_DITHER);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

void gfx_shutdown(void) {
    sceGuTerm();
}

void gfx_begin_frame(unsigned int clear_abgr) {
    sceGuStart(GU_DIRECT, g_list);
    sceGuClearColor(clear_abgr);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT | GU_FAST_CLEAR_BIT);
}

void gfx_end_frame(void) {
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

void gfx_present(int vsync) {
    if (vsync) {
        sceDisplayWaitVblankStart();
    }
    sceGuSwapBuffers();
}

// --- Flashlight ----------------------------------------------------------------------------------
// GE light 1 as a spotlight from the eye. The world has no normals (its lighting is baked into the
// vertex colours), so it gets the light's ambient term instead, which the GE still attenuates and
// cones but doesn't angle: with the vertex colour as the ambient material, a lit spot gets up to
// twice as bright. Props and NPCs have normals and get proper diffuse light from it too.
#define FLASH_ABGR 0xFFD0F0FFu // warm white
static int g_flash_on;

// Lamps and lights (the Lamp and Light tools) on GE lights 2 and 3, the same way as the flashlight
static struct {
    int on;
    unsigned int abgr;
} g_extra[2];

void gfx_world_light(int k, int on, int spot, const float pos[3], const float dir[3], unsigned int abgr, float range) {
    if (k < 0 || k > 1) return;
    g_extra[k].on = on;
    if (!on) {
        sceGuDisable(GU_LIGHT2 + k);
        return;
    }
    g_extra[k].abgr = abgr;
    ScePspFVector3 p = {pos[0], pos[1], pos[2]};
    if (spot) {
        ScePspFVector3 d = {-dir[0], -dir[1], -dir[2]}; // pointing back at the light, as for the flashlight
        sceGuLight(2 + k, GU_SPOTLIGHT, GU_AMBIENT_AND_DIFFUSE, &p);
        sceGuLightSpot(2 + k, &d, 4.0f, 0.8f);
    } else {
        sceGuLight(2 + k, GU_POINTLIGHT, GU_AMBIENT_AND_DIFFUSE, &p);
    }
    sceGuLightAtt(2 + k, 1.0f, 0.0f, 6.0f / (range * range)); // about a seventh as bright at range
}

static int extras_on(void) {
    return g_extra[0].on || g_extra[1].on;
}

void gu_flashlight(int on, const float eye[3], const float fwd[3]) {
    g_flash_on = on;
    if (!on) {
        sceGuDisable(GU_LIGHT1);
        return;
    }
    // The GE compares the spot direction with the vertex-to-light vector, so it points back at the light
    ScePspFVector3 pos = {eye[0], eye[1], eye[2]}, dir = {-fwd[0], -fwd[1], -fwd[2]};
    sceGuLight(1, GU_SPOTLIGHT, GU_AMBIENT_AND_DIFFUSE, &pos);
    sceGuLightSpot(1, &dir, 6.0f, 0.82f); // cos of a ~35 degree cone
    sceGuLightAtt(1, 1.0f, 0.0015f, 0.0000015f);
}

void gfx_flash_world_begin(void) {
    if (!g_flash_on && !extras_on()) return;
    if (g_flash_on) {
        sceGuLightColor(1, GU_AMBIENT, FLASH_ABGR);
        sceGuLightColor(1, GU_DIFFUSE, 0xFF000000u); // no normals to angle it by
        sceGuEnable(GU_LIGHT1);
    }
    for (int k = 0; k < 2; k++) {
        if (!g_extra[k].on) continue;
        sceGuLightColor(2 + k, GU_AMBIENT, g_extra[k].abgr);
        sceGuLightColor(2 + k, GU_DIFFUSE, 0xFF000000u);
        sceGuEnable(GU_LIGHT2 + k);
    }
    sceGuColorMaterial(GU_AMBIENT);
    sceGuAmbient(0xFFFFFFFFu); // baked vertex colour passes through unchanged
    sceGuDisable(GU_LIGHT0);
    sceGuEnable(GU_LIGHTING);
}

void gfx_flash_world_end(void) {
    if (!g_flash_on && !extras_on()) return;
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_LIGHT1);
    sceGuDisable(GU_LIGHT2);
    sceGuDisable(GU_LIGHT3);
    sceGuColorMaterial(0);
}

void gfx_flash_lit_begin(void) {
    if (g_flash_on) {
        sceGuLightColor(1, GU_AMBIENT, 0xFF000000u);
        sceGuLightColor(1, GU_DIFFUSE, FLASH_ABGR);
        sceGuEnable(GU_LIGHT1);
    }
    for (int k = 0; k < 2; k++) {
        if (!g_extra[k].on) continue;
        sceGuLightColor(2 + k, GU_AMBIENT, 0xFF000000u);
        sceGuLightColor(2 + k, GU_DIFFUSE, g_extra[k].abgr);
        sceGuEnable(GU_LIGHT2 + k);
    }
}

void gfx_flash_lit_end(void) {
    sceGuDisable(GU_LIGHT1);
    sceGuDisable(GU_LIGHT2);
    sceGuDisable(GU_LIGHT3);
}


void gfx_set_fog(float near_dist, float far_dist, unsigned int abgr) {
    if (far_dist <= near_dist) {
        sceGuDisable(GU_FOG);
        return;
    }
    sceGuFog(near_dist, far_dist, abgr);
    sceGuEnable(GU_FOG);
}
