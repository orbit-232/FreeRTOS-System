#include "panel_bsp.h"

#include <stdlib.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lv_adapter_input.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "panel_boot_key.h"
#include "panel_touch_map.h"

#define BOOT_KEY_GPIO 35
#define BOOT_KEY_ARM_DELAY_MS 1500U

static const char *TAG = "panel_bsp";
static lv_display_t *s_display;
static panel_boot_key_t s_boot_key;
static bool s_boot_key_ready;

typedef struct {
    int raw_x;
    int raw_y;
    int logical_x;
    int logical_y;
    bool valid;
} touch_last_t;

static portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;
static touch_last_t s_touch_last;
static uint32_t s_bad_raw;

/*
 * Official adapter custom read hook.  The GT9271 driver flags are all zero, so
 * esp_lcd_touch_get_data() returns the controller's raw point.  The single
 * 270-degree transform is applied here, exactly once, before the adapter feeds
 * LVGL.  Out-of-range controller reports are rejected (and counted) rather than
 * wrapping to the opposite edge.
 */
static esp_err_t touch_read_exact(esp_lcd_touch_handle_t tp,
                                  esp_lcd_touch_point_data_t *points, uint8_t *count,
                                  uint8_t max_count, void *user_ctx)
{
    (void)user_ctx;
    *count = 0;
    if (tp == NULL || points == NULL || max_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_lcd_touch_read_data(tp);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_touch_point_data_t raw[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint8_t raw_count = 0;
    err = esp_lcd_touch_get_data(tp, raw, &raw_count, max_count);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t out = 0;
    for (uint8_t i = 0; i < raw_count && out < max_count; i++) {
        int lx = 0;
        int ly = 0;
        if (!panel_touch_transform_raw(raw[i].x, raw[i].y, &lx, &ly)) {
            taskENTER_CRITICAL(&s_touch_mux);
            s_bad_raw++;
            taskEXIT_CRITICAL(&s_touch_mux);
            if ((s_bad_raw % 100U) == 1U) {
                ESP_LOGW(TAG, "reject raw out-of-range point (%u,%u), native=%dx%d",
                         (unsigned)raw[i].x, (unsigned)raw[i].y, PANEL_NATIVE_WIDTH,
                         PANEL_NATIVE_HEIGHT);
            }
            continue;
        }

        points[out] = raw[i];
        points[out].x = (uint16_t)lx;
        points[out].y = (uint16_t)ly;
        if (out == 0) {
            taskENTER_CRITICAL(&s_touch_mux);
            s_touch_last.raw_x = raw[i].x;
            s_touch_last.raw_y = raw[i].y;
            s_touch_last.logical_x = lx;
            s_touch_last.logical_y = ly;
            s_touch_last.valid = true;
            taskEXIT_CRITICAL(&s_touch_mux);
        }
        out++;
    }
    *count = out;
    return ESP_OK;
}

static esp_err_t boot_key_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BOOT GPIO%d input config failed: %s", BOOT_KEY_GPIO,
                 esp_err_to_name(err));
        return err;
    }
    bool raw_low = gpio_get_level(BOOT_KEY_GPIO) == 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    panel_boot_key_init(&s_boot_key, raw_low, now_ms, BOOT_KEY_ARM_DELAY_MS);
    s_boot_key_ready = true;
    ESP_LOGI(TAG, "BOOT GPIO%d input, active-low, debounce 30ms, arm delay %ums, boot_level=%s",
             BOOT_KEY_GPIO, BOOT_KEY_ARM_DELAY_MS, raw_low ? "low" : "high");
    return ESP_OK;
}

esp_err_t panel_bsp_init(void)
{
    if (s_display != NULL) {
        return ESP_OK;
    }

    esp_err_t boot_err = boot_key_init();
    if (boot_err != ESP_OK) {
        /* The display remains usable; only the optional lock shortcut is lost. */
        ESP_LOGW(TAG, "BOOT key unavailable, continuing without it");
    }

    /*
     * Native panel is 800x1280 portrait.  The panel is mounted such that the
     * previous 90-degree setting produced an upside-down landscape image; the
     * correct landscape orientation is 270 degrees (180 degrees from the old
     * setting).  ESP_LV_ADAPTER_ROTATE_270 maps logical (0,0) to native
     * (0,1279) and logical (1279,799) to native (799,0).
     *
     * The adapter rotates inside its flush path and leaves LVGL's own display
     * rotation at 0.  Touch is mapped once in touch_read_exact() below, installed
     * through the official esp_lv_adapter custom_touch_read hook; the GT9271
     * mirror/swap flags stay zero to avoid a second transform.  See
     * panel_touch_map.h for the full derivation and the host test.
     */
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_270,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = PANEL_TOUCH_DRIVER_SWAP_XY,
            .mirror_x = PANEL_TOUCH_DRIVER_MIRROR_X,
            .mirror_y = PANEL_TOUCH_DRIVER_MIRROR_Y,
        },
    };

    s_display = bsp_display_start_with_config(&cfg);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "bsp_display_start_with_config returned NULL");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "display: %ux%u rotation=270 tear_avoid=triple_partial",
             (unsigned)lv_display_get_horizontal_resolution(s_display),
             (unsigned)lv_display_get_vertical_resolution(s_display));
    ESP_LOGI(TAG, "touch: driver flags swap=%d mirror_x=%d mirror_y=%d; exact map "
                  "lv_x=1279-raw_y lv_y=raw_x",
             PANEL_TOUCH_DRIVER_SWAP_XY, PANEL_TOUCH_DRIVER_MIRROR_X,
             PANEL_TOUCH_DRIVER_MIRROR_Y);

    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight on");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(70), TAG, "set initial brightness");

    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev == NULL) {
        ESP_LOGE(TAG, "BSP reported no touch input device");
        return ESP_FAIL;
    }

    esp_lv_adapter_touch_callbacks_t cbs = {
        .custom_touch_read = touch_read_exact,
    };
    /* The adapter task is already running; update its callback under the same
     * display lock used by the input loop. */
    if (bsp_display_lock(1000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = esp_lv_adapter_set_touch_callbacks(indev, &cbs);
    bsp_display_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not install exact touch transform: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "touch exact transform installed via esp_lv_adapter custom_touch_read");

    return ESP_OK;
}

lv_display_t *panel_bsp_display(void)
{
    return s_display;
}

lv_indev_t *panel_bsp_input(void)
{
    return bsp_display_get_input_dev();
}

bool panel_bsp_boot_key_take_press(void)
{
    if (!s_boot_key_ready) return false;
    bool raw_low = gpio_get_level(BOOT_KEY_GPIO) == 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return panel_boot_key_update(&s_boot_key, raw_low, now_ms);
}

bool panel_bsp_lock(uint32_t timeout_ms)
{
    return bsp_display_lock(timeout_ms) == ESP_OK;
}

void panel_bsp_unlock(void)
{
    bsp_display_unlock();
}

esp_err_t panel_bsp_set_brightness(int percent)
{
    if (percent < PANEL_BSP_BRIGHTNESS_MIN) {
        percent = PANEL_BSP_BRIGHTNESS_MIN;
    }
    if (percent > 100) {
        percent = 100;
    }
    return bsp_display_brightness_set(percent);
}

int panel_bsp_get_brightness(void)
{
    return bsp_display_brightness_get();
}

esp_err_t panel_bsp_backlight_on(void)
{
    return bsp_display_backlight_on();
}

esp_err_t panel_bsp_backlight_off(void)
{
    return bsp_display_backlight_off();
}

void panel_bsp_touch_get_last(int *raw_x, int *raw_y, int *logical_x, int *logical_y,
                              bool *valid)
{
    taskENTER_CRITICAL(&s_touch_mux);
    touch_last_t last = s_touch_last;
    taskEXIT_CRITICAL(&s_touch_mux);
    if (raw_x != NULL) {
        *raw_x = last.raw_x;
    }
    if (raw_y != NULL) {
        *raw_y = last.raw_y;
    }
    if (logical_x != NULL) {
        *logical_x = last.logical_x;
    }
    if (logical_y != NULL) {
        *logical_y = last.logical_y;
    }
    if (valid != NULL) {
        *valid = last.valid;
    }
}

uint32_t panel_bsp_touch_bad_raw_count(void)
{
    taskENTER_CRITICAL(&s_touch_mux);
    uint32_t count = s_bad_raw;
    taskEXIT_CRITICAL(&s_touch_mux);
    return count;
}
