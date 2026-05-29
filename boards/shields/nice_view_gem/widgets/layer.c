#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>

#include "layer.h"
#include "../assets/custom_fonts.h"
#include <zmk/keymap.h>
#include <zmk/matrix.h>

/* ── Toucan sprite dimensions (drawn with LVGL primitives) ─────────────── */
#define SPRITE_W   48
#define SPRITE_H   40

static void draw_wing(lv_obj_t *canvas, int16_t sx, int16_t sy,
                      int deflection) {
    int extension = deflection * 4;
    if (extension <= 0) {
        return; /* wing fully folded — nothing to draw */
    }

    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color    = LVGL_FOREGROUND;
    fill.bg_opa      = LV_OPA_COVER;
    fill.border_width = 0;
    fill.radius      = 0;

    int tip_y   = sy + 20 - extension;
    int root_y  = sy + 20;
    int root_rw = 14;               /* full root width (right side masked) */
    int span    = root_y - tip_y;  /* == extension                        */

    for (int row = tip_y; row < root_y; row++) {
        int d = row - tip_y;
        int w = (root_rw * d + span / 2) / span; /* grows 0 → root_rw    */
        if (w < 1) w = 1;
        lv_canvas_draw_rect(canvas, sx, row, w, 1, &fill);
    }
}

/* ── Toucan body drawing ───────────────────────────────────────────────── */

static void draw_toucan(lv_obj_t *canvas, int16_t sx, int16_t sy,
                        int deflection) {
    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color    = LVGL_FOREGROUND;
    fill.bg_opa      = LV_OPA_COVER;
    fill.border_width = 0;

    /* 1. Wing — drawn first so head/body covers the overlapping portion */
    draw_wing(canvas, sx, sy, deflection);

    /* 2. Body: 24×20 rounded oval, lower-left */
    fill.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(canvas, sx + 0, sy + 18, 24, 20, &fill);

    /* 3. Head: 22×22 filled circle centred at (sx+19, sy+11) */
    lv_canvas_draw_rect(canvas, sx + 8, sy + 0, 22, 22, &fill);

    /* 4. Beak: 25 column-slices, linearly tapered right-to-point
     *    half_h = round(4 × (24−c) / 24), centred on row sy+11  */
    fill.radius = 0;
    for (int c = 0; c < 25; c++) {
        int half_h = (4 * (24 - c) + 12) / 24;
        int h      = 2 * half_h + 1;
        lv_canvas_draw_rect(canvas, sx + 22 + c, sy + 11 - half_h, 1, h, &fill);
    }

    /* 5. Tail: 8 tapering horizontal rows, bottom-left */
    for (int r = 0; r < 8; r++) {
        int w = 8 - r;
        lv_canvas_draw_rect(canvas, sx + 0, sy + 32 + r, w, 1, &fill);
    }

    /* 6. Eye: 3×3 black square punched into the head (last — on top) */
    lv_draw_rect_dsc_t eye;
    lv_draw_rect_dsc_init(&eye);
    eye.bg_color     = LVGL_BACKGROUND;
    eye.bg_opa       = LV_OPA_COVER;
    eye.border_width = 0;
    eye.radius       = 0;
    lv_canvas_draw_rect(canvas, sx + 12, sy + 5, 3, 3, &eye);
}

/* ── Toucan pre-render cache ───────────────────────────────────────────── */

static lv_color_t   toucan_cache_buf[SPRITE_W * SPRITE_H];
static lv_img_dsc_t toucan_img_dsc;
static bool         toucan_cached = false;

static void ensure_toucan_cached(void) {
    if (toucan_cached) return;

    lv_obj_t *tmp = lv_canvas_create(lv_layer_top());
    lv_canvas_set_buffer(tmp, toucan_cache_buf, SPRITE_W, SPRITE_H,
                         LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(tmp, LVGL_BACKGROUND, LV_OPA_COVER);
    draw_toucan(tmp, 0, 0, 3);
    lv_obj_del(tmp);

    toucan_img_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    toucan_img_dsc.header.always_zero = 0;
    toucan_img_dsc.header.reserved    = 0;
    toucan_img_dsc.header.w           = SPRITE_W;
    toucan_img_dsc.header.h           = SPRITE_H;
    toucan_img_dsc.data_size          = sizeof(toucan_cache_buf);
    toucan_img_dsc.data               = (const uint8_t *)toucan_cache_buf;

    toucan_cached = true;
}

/* ── Helpers ───────────────────────────────────────────────────────────── */

static const char *get_layer_name(uint8_t layer_index,
                                   char *fallback, size_t sz) {
    const char *name =
        zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(layer_index));
    if (name == NULL || name[0] == '\0') {
        snprintf(fallback, sz, "L#%" PRIu8, layer_index);
        return fallback;
    }
    return name;
}

/* ── Draw ──────────────────────────────────────────────────────────────── */

void draw_layer_status(lv_obj_t *canvas, const struct status_state *state) {
    if (state->layer_index == 0) {
        ensure_toucan_cached();
        lv_draw_img_dsc_t img_dsc;
        lv_draw_img_dsc_init(&img_dsc);
        lv_canvas_draw_img(canvas,
                           (SCREEN_WIDTH - SPRITE_W) / 2,
                           ANIM_ZONE_Y + (ANIM_ZONE_H - SPRITE_H) / 2,
                           &toucan_img_dsc,
                           &img_dsc);
    } else {
        lv_draw_label_dsc_t label_dsc;
        init_label_dsc(&label_dsc, LVGL_FOREGROUND, &quinquefive_24,
                       LV_TEXT_ALIGN_CENTER);

        char fallback[16];
        const char *layer_name =
            get_layer_name(state->layer_index, fallback, sizeof(fallback));

        lv_canvas_draw_text(canvas,
                            0,
                            ANIM_ZONE_Y + (ANIM_ZONE_H - 24) / 2,
                            SCREEN_WIDTH,
                            &label_dsc,
                            layer_name);
    }
}
