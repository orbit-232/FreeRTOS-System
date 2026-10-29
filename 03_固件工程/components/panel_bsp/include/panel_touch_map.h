#pragma once

/*
 * Single source of truth for the 10.1" JD9365 + GT9271 geometry and the exact
 * touch transform used by the firmware.
 *
 * Native panel : 800 x 1280 (portrait)
 * UI canvas    : 1280 x 800 (landscape, adapter rotation 270)
 *
 * The adapter rotates inside its flush path; it does NOT call
 * lv_display_set_rotation(), so LVGL itself stays at rotation 0 and does not
 * transform input points.
 *
 * The BSP/GT9271 mirror/swap flags are therefore all zero.  panel_bsp installs
 * an official esp_lv_adapter custom_touch_read callback which reads the raw
 * GT9271 point (esp_lcd_touch_read_data + esp_lcd_touch_get_data with identity
 * flags) and applies the one transform below.  This avoids the x_max off-by-one
 * of the flag path (mirror_y uses y_max instead of y_max-1) and guarantees every
 * logical point is inside 0..1279 / 0..799.
 *
 * Adapter rotation 270 logical->physical mapping:
 *   phy_x = lv_y
 *   phy_y = 1279 - lv_x
 * Its inverse (raw/physical -> logical) is:
 *   lv_x = 1279 - raw_y
 *   lv_y = raw_x
 */

#include <stdbool.h>
#include <stdint.h>

#define PANEL_NATIVE_WIDTH 800
#define PANEL_NATIVE_HEIGHT 1280
#define PANEL_LOGICAL_WIDTH 1280
#define PANEL_LOGICAL_HEIGHT 800

/* The GT9271 driver must not transform the raw point; panel_bsp does it once. */
#define PANEL_TOUCH_DRIVER_SWAP_XY 0
#define PANEL_TOUCH_DRIVER_MIRROR_X 0
#define PANEL_TOUCH_DRIVER_MIRROR_Y 0

/* Exact production transform.  raw_x/raw_y must be inside the native panel;
 * out-of-range controller reports are rejected instead of silently becoming an
 * off-screen or wrapped point.  Returns false for an invalid raw point. */
static inline bool panel_touch_transform_raw(int raw_x, int raw_y, int *logical_x,
                                             int *logical_y)
{
    if (raw_x < 0 || raw_x >= PANEL_NATIVE_WIDTH ||
            raw_y < 0 || raw_y >= PANEL_NATIVE_HEIGHT) {
        return false;
    }
    *logical_x = (PANEL_LOGICAL_WIDTH - 1) - raw_y;
    *logical_y = raw_x;
    return true;
}

/* Adapter's display_coord_to_phy() for ESP_LV_ADAPTER_ROTATE_270. */
static inline void panel_display_logical_to_physical(int logical_x, int logical_y,
                                                    int *physical_x, int *physical_y)
{
    *physical_x = logical_y;
    *physical_y = (PANEL_NATIVE_HEIGHT - 1) - logical_x;
}
