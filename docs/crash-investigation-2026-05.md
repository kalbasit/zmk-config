# Right-Half Lock-Up: Crash Investigation (May 2026)

## Symptom

The right half (peripheral) of the Toucan split keyboard would lock up
intermittently, requiring a hard power cycle. Once locked it would not respond
to reset, bootloader button, or BLE reconnect.

Crash first appeared after merging:
> `4d60ca1 feat(display): bounce toucan + layer names on nice!view (#209)`

## Root Cause

Two cooperating problems:

### 1. Display: ~50 LVGL draw calls per refresh on the base layer

`boards/shields/nice_view_gem/widgets/layer.c` — `draw_toucan()` executes
roughly 50 `lv_canvas_draw_rect` calls every time `draw_layer_status` is
invoked with `layer_index == 0`.

`draw_top()` in `screen.c` is called on **every** ZMK event subscription
including `zmk_peripheral_battery_state_changed` — so every battery-level
update from the right half triggers a full `draw_toucan()` redraw.

### 2. BLE: battery notify races with connection parameter renegotiation

`CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y` subscribes to the
peripheral's BAS GATT characteristic (CCCD = notify). When ZMK's idle timer
fires (30 s default), the central sends a connection parameter update request.
If `bt_gatt_notify` on the peripheral fires during that window, the nRF52840
SoftDevice asserts and the MCU hangs permanently.

The display drawing in (1) shifts CPU scheduling enough that this race fires
more frequently — but the race exists independently of the display.

## Evidence

**Git bisect** confirmed `4d60ca1` as the first bad commit.

**Control test**: flashing the original `zmk-keyboard-toucan` firmware (no
custom display code) with `CENTRAL_BATTERY_LEVEL_FETCHING=y` active ran
3 h 15 min+ with zero crashes. This proves the crash is specific to the
display load added in zmk-config.

**Log evidence**: `split_central_battery_level_notify_func` appeared in USB
logs — the ZMK fork fix (which removes that function) was not yet deployed
because `zephyrDepsHash` in `flake.nix` was stale, so Nix used cached old
ZMK source.

## Branches / Workarounds Tried

### This branch (`fix-ble-enable-experimental`)

Changes on this branch:

| File | Change |
|------|--------|
| `config/west.yml` | Points to `kalbasit/zmk` fork, branch `fix/peripheral-battery-bas-notify-crash-v0.3` |
| `config/toucan.conf` | Enables `CONFIG_ZMK_BLE_EXPERIMENTAL_FEATURES=y`, `CONFIG_BT_CTLR_TX_PWR_PLUS_8=y`, `CONFIG_ZMK_BLE_PASSKEY_ENTRY=y` |
| `config/toucan_left.conf` | USB logging enabled (`CONFIG_ZMK_USB_LOGGING=y`) |
| `config/toucan_right.conf` | USB logging enabled (new file, right-side specific) |
| `boards/shields/toucan/toucan_{left,right}.overlay` | CDC ACM UART node for USB console |
| `flake.nix` | `zephyrDepsHash = ""` to force re-fetch of ZMK fork (hash needs updating) |

**Note**: `config/toucan_left.conf` does NOT have
`CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y` — this is intentionally
disabled as a workaround to avoid the BAS notify race while the root-cause fix
is validated.

### ZMK fork fix (`fix/peripheral-battery-bas-notify-crash-v0.3`)

Commit `47bd42b3` in `https://github.com/kalbasit/zmk` replaces
subscribe-and-notify with periodic read-only polling for peripheral battery
level. This eliminates the race entirely.

Status: implemented, not yet deployed (flake.nix zephyrDepsHash stale), not
yet submitted upstream (need to understand it well enough to write a good PR
description).

## Quick Fix: Pre-Render Toucan to Static Buffer

Instead of calling ~50 `lv_canvas_draw_rect` calls every display refresh,
pre-render the toucan once into a static pixel buffer and blit it as a single
image on subsequent redraws.

See branch `fix/display-toucan-prerender` for the implementation.

**How it works in `layer.c`**:

```c
static lv_color_t toucan_cache_buf[SPRITE_W * SPRITE_H];
static bool toucan_cached = false;

static void ensure_toucan_cached(void) {
    if (toucan_cached) return;
    // allocate a temporary canvas backed by toucan_cache_buf
    // draw_toucan() once into it
    // mark cached = true
}
```

Then `draw_layer_status()` calls `ensure_toucan_cached()` and uses
`lv_canvas_draw_img()` — one blit instead of ~50 rect primitives.

## Files of Interest

- `boards/shields/nice_view_gem/widgets/layer.c` — display crash trigger
- `boards/shields/nice_view_gem/widgets/screen.c` — `draw_top()` subscription loop
- `config/toucan_left.conf` — `FETCHING` config workaround
- `config/west.yml` — ZMK fork pin
- `flake.nix` — zephyrDepsHash (needs update after west.yml change)
