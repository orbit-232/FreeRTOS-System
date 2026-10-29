#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Keep a visible floor so a zero-brightness setting cannot lock the user out. */
#define PANEL_BSP_BRIGHTNESS_MIN 12

/**
 * Initialize the official Waveshare BSP for the 10.1" 800x1280 JD9365 panel,
 * rotated 270 degrees to a 1280x800 landscape canvas, with the matching GT9271
 * touch transform.  This is the only component that owns rotation, touch
 * flags, backlight GPIO/PWM and the LVGL adapter task.  See
 * panel_touch_map.h for the coordinate derivation and the host test.
 */
esp_err_t panel_bsp_init(void);

lv_display_t *panel_bsp_display(void);
lv_indev_t *panel_bsp_input(void);

/* GPIO35 BOOT key: active-low input only, debounced in the UI timer context.
 * Returns true once per press; a key low at boot is ignored until released. */
bool panel_bsp_boot_key_take_press(void);

bool panel_bsp_lock(uint32_t timeout_ms);
void panel_bsp_unlock(void);

esp_err_t panel_bsp_set_brightness(int percent);
int panel_bsp_get_brightness(void);
esp_err_t panel_bsp_backlight_on(void);
esp_err_t panel_bsp_backlight_off(void);

/* Last raw GT9271 point and the logical point produced by the exact transform.
 * raw_* are measured controller coordinates; logical_* are what LVGL sees.
 * Both are copied under a short critical section. */
void panel_bsp_touch_get_last(int *raw_x, int *raw_y, int *logical_x, int *logical_y,
                              bool *valid);
uint32_t panel_bsp_touch_bad_raw_count(void);

#ifdef __cplusplus
}
#endif
