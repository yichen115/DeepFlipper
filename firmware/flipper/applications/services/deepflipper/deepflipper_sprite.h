#pragma once
#include "deepflipper.h"
#include <gui/canvas.h>

void deepflipper_sprite_init(void);
void deepflipper_sprite_update(const DeepFlipperSnapshot* snapshot);
void deepflipper_draw(Canvas* canvas);
void deepflipper_render_reset(void);
bool deepflipper_rendered(void);
// Desktop level-up: the whale girl cheers for a few seconds (no flash-heavy film).
void deepflipper_celebrate(void);
