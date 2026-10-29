#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "panel_assets.h"
#include "panel_clock.h"
#include "panel_files.h"
#include "panel_online.h"
#include "panel_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t uptime_s;
    uint32_t heap_free;
    uint32_t heap_min;
    uint32_t heap_total;
    uint32_t psram_free;
    uint32_t psram_total;
    uint32_t task_count;
} panel_runtime_t;

/** Initialize NVS-backed settings, apply backlight and seed the local clock. */
esp_err_t panel_services_init(void);

/** Music-only startup: backlight + TF + audio + music worker.
 * Does not start Wi-Fi/BLE, weather, clock, wallpaper or external fonts. */
esp_err_t panel_services_init_music_only(void);

/** Copy the current settings snapshot (thread-safe). */
void panel_services_get_settings(panel_settings_t *out);

/** Validate, store and schedule a delayed NVS commit. */
bool panel_services_update_settings(const panel_settings_t *settings);

/** Change the real backlight level and schedule a delayed settings commit. */
esp_err_t panel_services_set_brightness(int percent);

/** Schedule a settings write at least 1.2 s after the last change. */
void panel_services_schedule_save(void);

/** Fill measured runtime values. Never synthesizes CPU/FPS/temperature. */
void panel_services_get_runtime(panel_runtime_t *out);

/** Local clock seeded from the firmware build time; always reports uncalibrated. */
bool panel_services_get_local_time(struct tm *out);
bool panel_services_time_is_calibrated(void);

/** True when the Wi-Fi/Bluetooth service is actually connected. */
bool panel_services_wifi_connected(void);
bool panel_services_bt_connected(void);

/** Weather/time facade; UI only copies snapshots or queues commands. */
void panel_services_get_weather(panel_weather_snapshot_t *out);
void panel_services_get_city_search(panel_city_search_snapshot_t *out);
esp_err_t panel_services_weather_refresh(void);
esp_err_t panel_services_weather_search(const char *query);
esp_err_t panel_services_weather_choose_city(unsigned index, uint32_t search_generation);

/** Clock facade: UI copies snapshots or queues commands; no NVS/time waits. */
void panel_services_get_clock(panel_clock_snapshot_t *out);
esp_err_t panel_services_clock_stopwatch_start(void);
esp_err_t panel_services_clock_stopwatch_pause(void);
esp_err_t panel_services_clock_stopwatch_reset(void);
esp_err_t panel_services_clock_countdown_start(uint32_t seconds);
esp_err_t panel_services_clock_countdown_pause(void);
esp_err_t panel_services_clock_countdown_resume(void);
esp_err_t panel_services_clock_countdown_reset(void);
esp_err_t panel_services_clock_countdown_ack(uint32_t generation);
esp_err_t panel_services_clock_alarm_set(bool enabled, uint8_t hour, uint8_t minute);
esp_err_t panel_services_clock_alarm_ack(uint32_t generation);

void panel_services_get_files(panel_files_snapshot_t *out);
esp_err_t panel_services_files_retry(void);
esp_err_t panel_services_files_refresh(uint32_t generation);
esp_err_t panel_services_files_enter(unsigned index, uint32_t generation);
esp_err_t panel_services_files_up(uint32_t generation);
esp_err_t panel_services_files_root(uint32_t generation);
esp_err_t panel_services_files_page(int delta, uint32_t generation);
esp_err_t panel_services_files_preview(unsigned index, uint32_t generation);
esp_err_t panel_services_files_close_preview(void);

void panel_services_get_assets(panel_assets_status_t *out);
esp_err_t panel_services_assets_request_font(void);
void panel_services_assets_cancel_font(void);
const lv_font_t *panel_services_assets_acquire_font(void);
void panel_services_assets_release_font(void);

#ifdef __cplusplus
}
#endif
