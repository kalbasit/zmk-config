#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <stdio.h>
#include <string.h>

#include "layer.h"
#include "../assets/custom_fonts.h"
#include <zmk/physical_layouts.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

/* ── Display zone ──────────────────────────────────────────────────────────
 * Center band between battery row (≈y=28) and output/profile row (≈y=138).
 */
#define ANIM_ZONE_Y    28
#define ANIM_ZONE_H   110

/* ── Toucan sprite dimensions (drawn with LVGL primitives) ─────────────── */
#define SPRITE_W   48
#define SPRITE_H   40

/* ── Wing flap ─────────────────────────────────────────────────────────────
 * 12-frame triangle wave → deflection 0-6-0, one full flap per 12 frames.
 * At 15 fps → ~1.25 flaps/second (natural slow glide).
 */
#define FLAP_PERIOD   12          /* frames per complete up-down cycle  */
#define FLAP_PEAK      6          /* max deflection (px × 4 = extension)*/

/* ── Wing drawing ──────────────────────────────────────────────────────────
 *
 * The wing is a LEFT-ALIGNED triangle drawn BEFORE the head & body.
 * Its left edge is pinned to sx; it fans rightward.  The head & body
 * (drawn after, in the same white colour) mask the right portion of the
 * wing — leaving a clearly visible white wedge to the upper-left of the
 * head against the black background.
 *
 *   Tip:      (sx,    sy+20 − extension)   ← 1-px point, above head
 *   Root-L:   (sx,    sy+20)               ← left edge of body (masked)
 *   Root-R:   (sx+14, sy+20)               ← inside body (masked)
 *
 * extension = deflection × 4   →   0 (folded) … 24 px (fully spread)
 *
 * At full spread the tip clears the top of the head (head starts at sy+0,
 * tip is at sy−4) and the visible wedge is ~10 px wide at mid-height.
 */
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

/* ── Animation state ───────────────────────────────────────────────────── */

static struct {
    int16_t    x, y;        /* sprite / text top-left                    */
    int8_t     dx, dy;      /* bounce velocity (px/frame)                */
    int16_t    max_x;       /* horizontal bounce limit                   */
    int16_t    max_y;       /* vertical bounce limit                     */
    lv_coord_t text_w;      /* measured width of current layer name      */
    uint8_t    flap_frame;  /* 0 … FLAP_PERIOD-1, for wing animation    */
    uint8_t    last_layer;  /* detect layer changes                      */
} anim = {
    .x = 8, .y = 8,
    .dx = 3, .dy = 2,
    .max_x  = SCREEN_WIDTH - SPRITE_W,
    .max_y  = ANIM_ZONE_H  - SPRITE_H,
    .text_w = 0,
    .flap_frame = 0,
    .last_layer = 0xFF,
};

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

/* ── Public animation API ──────────────────────────────────────────────── */

void layer_animation_reset(uint8_t new_layer) {
    if (new_layer == anim.last_layer) {
        return;
    }
    anim.last_layer = new_layer;

    if (new_layer == 0) {
        anim.max_x = SCREEN_WIDTH - SPRITE_W;
        anim.max_y = ANIM_ZONE_H  - SPRITE_H;
    } else {
        char fallback[16];
        const char *name = get_layer_name(new_layer, fallback, sizeof(fallback));
        anim.text_w = lv_txt_get_width(name, strlen(name),
                                        &quinquefive_24, 0, LV_TEXT_FLAG_NONE);
        anim.max_x  = SCREEN_WIDTH - anim.text_w;
        if (anim.max_x < 0) { anim.max_x = 0; }
        anim.max_y  = ANIM_ZONE_H - 24; /* quinquefive_24 cap height */
    }

    if (anim.x > anim.max_x) { anim.x = anim.max_x / 2; }
    if (anim.y > anim.max_y) { anim.y = anim.max_y / 2; }
}

bool layer_animation_tick(uint8_t layer_index) {
    /* Advance bounce position */
    anim.x += anim.dx;
    anim.y += anim.dy;

    if (anim.x <= 0)           { anim.x = 0;           anim.dx = -anim.dx; }
    if (anim.x >= anim.max_x)  { anim.x = anim.max_x;  anim.dx = -anim.dx; }
    if (anim.y <= 0)           { anim.y = 0;           anim.dy = -anim.dy; }
    if (anim.y >= anim.max_y)  { anim.y = anim.max_y;  anim.dy = -anim.dy; }

    /* Advance wing flap only on base layer */
    if (layer_index == 0) {
        anim.flap_frame = (anim.flap_frame + 1) % FLAP_PERIOD;
    }

    return true;
}

/* ── Draw ──────────────────────────────────────────────────────────────── */

void draw_layer_status(lv_obj_t *canvas, const struct status_state *state) {
    if (state->layer_index == 0) {
        /* Triangle-wave deflection: 0 → FLAP_PEAK → 0 over FLAP_PERIOD */
        int f          = anim.flap_frame;
        int deflection = (f < FLAP_PERIOD / 2) ? f : (FLAP_PERIOD - f);

        draw_toucan(canvas, anim.x, ANIM_ZONE_Y + anim.y, deflection);
    } else {
        lv_draw_label_dsc_t label_dsc;
        init_label_dsc(&label_dsc, LVGL_FOREGROUND, &quinquefive_24,
                       LV_TEXT_ALIGN_LEFT);

        char fallback[16];
        const char *layer_name =
            get_layer_name(state->layer_index, fallback, sizeof(fallback));

        lv_canvas_draw_text(canvas,
                            anim.x,
                            ANIM_ZONE_Y + anim.y,
                            anim.text_w + 1,
                            &label_dsc,
                            layer_name);
    }
}
