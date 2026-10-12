#pragma once

#include "camera.h"
#include "map.h"

// The sky: the map's six skybox textures when it has them (HL2, CS:S maps), otherwise a gradient
// dome with a sun built from env_skypaint colours (GMod's "painted" sky).
void sky_build(const Map *m);
// Draw first each frame. It doesn't touch the depth buffer.
void sky_draw(const Camera *cam);
