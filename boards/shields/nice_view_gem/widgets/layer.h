#pragma once

#include <lvgl.h>
#include "util.h"

/* Animation zone bounds — vertical slice of the canvas reserved for layer display */
#define ANIM_ZONE_Y    28
#define ANIM_ZONE_H   110

struct layer_status_state {
    uint8_t index;
};

void draw_layer_status(lv_obj_t *canvas, const struct status_state *state);