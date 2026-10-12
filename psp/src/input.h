#pragma once

#include <pspctrl.h>

typedef struct {
    float ax, ay;          // Analog stick, -1..1, deadzone removed. +ay is stick pushed down.
    unsigned int held;     // PSP_CTRL_* bits
    unsigned int pressed;  // Went down this frame
    unsigned int released; // Went up this frame
} InputState;

void input_init(void);
void input_update(InputState *in);
