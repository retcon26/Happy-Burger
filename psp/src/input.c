#include "input.h"

#include <math.h>

// PSP nubs drift a lot when worn, so the deadzone is generous.
#define DEADZONE 0.25f

static unsigned int g_prev;

void input_init(void) {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    g_prev = 0;
}

static float axis(unsigned char raw) {
    float v = ((float)raw - 128.0f) / 127.0f;
    float a = fabsf(v);
    if (a < DEADZONE) {
        return 0.0f;
    }
    // Rescale so output starts at 0 right after the deadzone instead of jumping to DEADZONE.
    float s = (a - DEADZONE) / (1.0f - DEADZONE);
    if (s > 1.0f) {
        s = 1.0f;
    }
    return v < 0.0f ? -s : s;
}

void input_update(InputState *in) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);

    in->ax = axis(pad.Lx);
    in->ay = axis(pad.Ly);
    in->held = pad.Buttons;
    in->pressed = pad.Buttons & ~g_prev;
    in->released = ~pad.Buttons & g_prev;
    g_prev = pad.Buttons;
}
