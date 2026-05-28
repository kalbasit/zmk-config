#pragma once

#include <lvgl.h>
#include "util.h"

struct layer_status_state {
    uint8_t index;
};

void draw_layer_status(lv_obj_t *canvas, const struct status_state *state);

/* Animation API — called from screen.c */
void layer_animation_reset(uint8_t new_layer);
bool layer_animation_tick(uint8_t layer_index);