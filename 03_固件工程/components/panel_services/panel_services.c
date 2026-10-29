#include "panel_services.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "panel_bsp.h"
#include "panel_network.h"
#include "panel_wallpaper.h"
#include "panel_audio.h"
#include "panel_music.h"

static const char *TAG = "panel_services";

#define SETTINGS_NAMESPACE "panel"
#define SETTINGS_KEY "settings"
#define SETTINGS_MAGIC 0x57504E4CUL
#define SETTINGS_VERSION 2
#define SETTINGS_BLOB_MAX 64
#define SAVE_DELAY_US (1200ULL * 1000ULL)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    panel_settings_t settings;
    uint32_t flags;
} settings_blob_t;

/* Exact layout written by firmware 1.0.1 and earlier.  The v1 loader keeps
 * brightness/theme/sleep choices while applying the one-time animation policy. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    panel_settings_t settings;
} settings_blob_v1_t;

static SemaphoreHandle_t s_mutex;
static panel_settings_t s_settings;
static uint32_t s_settings_flags;
static bool s_migration_save_pending;
static esp_timer_handle_t s_save_timer;

static void lock_settings(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void unlock_settings(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

static void save_timer_cb(void *arg)
{
    (void)arg;
    panel_settings_t snapshot;
    lock_settings();
    snapshot = s_settings;
    unlock_settings();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    settings_blob_t blob = {
        .magic = SETTINGS_MAGIC,
        .version = SETTINGS_VERSION,
        .reserved = 0,
        .settings = snapshot,
        .flags = s_settings_flags,
    };
    err = nvs_set_blob(handle, SETTINGS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings commit failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG,
                 "settings saved to NVS (v%u brightness=%u theme=%u sleep=%us flags=0x%08x)",
                 SETTINGS_VERSION, snapshot.brightness, (unsigned)snapshot.theme,
                 snapshot.sleep_after_s, (unsigned)s_settings_flags);
    }
}

static void load_settings(void)
{
    panel_settings_set_defaults(&s_settings);
    s_settings_flags = 0;
    s_migration_save_pending = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return;
    }

    uint8_t raw[SETTINGS_BLOB_MAX];
    size_t len = sizeof(raw);
    err = nvs_get_blob(handle, SETTINGS_KEY, raw, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saved settings unreadable (err=%s), using defaults",
                 esp_err_to_name(err));
        return;
    }
    if (len < 8) {
        ESP_LOGW(TAG, "saved settings too short (%u bytes), using defaults", (unsigned)len);
        return;
    }

    uint32_t magic = 0;
    uint16_t version = 0;
    memcpy(&magic, raw, sizeof(magic));
    memcpy(&version, raw + sizeof(magic), sizeof(version));
    if (magic != SETTINGS_MAGIC) {
        ESP_LOGW(TAG, "saved settings magic mismatch, using defaults");
        return;
    }

    if (version == 1) {
        if (len != sizeof(settings_blob_v1_t)) {
            ESP_LOGW(TAG, "v1 settings size %u != %u, using defaults", (unsigned)len,
                     (unsigned)sizeof(settings_blob_v1_t));
            return;
        }
        settings_blob_v1_t old;
        memcpy(&old, raw, sizeof(old));
        if (!panel_settings_validate(&old.settings)) {
            ESP_LOGW(TAG, "v1 saved settings invalid, using defaults");
            return;
        }
        s_settings = old.settings;
        panel_settings_migration_t migration;
        if (!panel_settings_migrate(&s_settings, version, &s_settings_flags, &migration)) {
            ESP_LOGW(TAG, "v1 settings migration unsupported, using defaults");
            panel_settings_set_defaults(&s_settings);
            s_settings_flags = 0;
            return;
        }
        s_migration_save_pending = true;
        ESP_LOGI(TAG,
                 "settings migration v1->v2: animations_forced_on=%d flags=0x%08x; "
                 "persisting once",
                 migration.animations_forced_on ? 1 : 0, (unsigned)s_settings_flags);
        return;
    }

    if (version == SETTINGS_VERSION) {
        if (len != sizeof(settings_blob_t)) {
            ESP_LOGW(TAG, "v2 settings size %u != %u, using defaults", (unsigned)len,
                     (unsigned)sizeof(settings_blob_t));
            return;
        }
        settings_blob_t blob;
        memcpy(&blob, raw, sizeof(blob));
        if (!panel_settings_validate(&blob.settings)) {
            ESP_LOGW(TAG, "v2 saved settings invalid, using defaults");
            return;
        }
        s_settings = blob.settings;
        s_settings_flags = blob.flags;
        ESP_LOGI(TAG, "settings v2 loaded from NVS (animations=%d flags=0x%08x)",
                 s_settings.animations ? 1 : 0, (unsigned)s_settings_flags);
        return;
    }

    ESP_LOGW(TAG, "unsupported saved settings version %u, using defaults", version);
}

static bool parse_build_date(int *year, int *month, int *day)
{
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char month_text[4] = {0};
    if (sscanf(__DATE__, "%3s %d %d", month_text, day, year) != 3) {
        return false;
    }
    for (int i = 0; i < 12; i++) {
        if (strcmp(month_text, months[i]) == 0) {
            *month = i + 1;
            return true;
        }
    }
    return false;
}

static void seed_local_clock(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_build_date(&year, &month, &day) ||
        sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        ESP_LOGW(TAG, "could not seed local clock from build time");
        return;
    }

    struct tm build_tm = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second,
        .tm_isdst = -1,
    };
    time_t build_time = mktime(&build_tm);
    if (build_time <= 0) {
        ESP_LOGW(TAG, "build time could not be converted");
        return;
    }
    struct timeval tv = {.tv_sec = build_time, .tv_usec = 0};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "local clock seeded from build time; marked uncalibrated (no NTP)");
}

esp_err_t panel_services_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    load_settings();
    seed_local_clock();

    esp_err_t err = panel_bsp_set_brightness(s_settings.brightness);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to apply brightness %u: %s", s_settings.brightness,
                 esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = save_timer_cb,
        .name = "settings_save",
    };
    err = esp_timer_create(&timer_args, &s_save_timer);
    if (err != ESP_OK) {
        return err;
    }

    if (s_migration_save_pending) {
        s_migration_save_pending = false;
        save_timer_cb(NULL);
    }

    ESP_LOGI(TAG, "services ready: brightness=%u heap=%u psram=%u", s_settings.brightness,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    esp_err_t net_err = panel_network_init();
    if (net_err != ESP_OK) {
        ESP_LOGE(TAG, "network service init failed: %s", esp_err_to_name(net_err));
    }
    esp_err_t online_err = panel_online_init();
    if (online_err != ESP_OK) {
        ESP_LOGE(TAG, "online service init failed: %s", esp_err_to_name(online_err));
    }
    esp_err_t clock_err = panel_clock_init();
    if (clock_err != ESP_OK) {
        ESP_LOGE(TAG, "clock service init failed: %s", esp_err_to_name(clock_err));
    }
    esp_err_t files_err = panel_files_init();
    if (files_err != ESP_OK) {
        ESP_LOGE(TAG, "files service init failed: %s", esp_err_to_name(files_err));
    }
    esp_err_t assets_err = panel_assets_init();
    if (assets_err != ESP_OK) {
        ESP_LOGE(TAG, "asset service init failed: %s", esp_err_to_name(assets_err));
    }
    esp_err_t wallpaper_err = panel_wallpaper_init();
    if (wallpaper_err != ESP_OK) {
        ESP_LOGE(TAG, "wallpaper service init failed: %s", esp_err_to_name(wallpaper_err));
    }
    esp_err_t audio_err = panel_audio_init();
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "audio service init failed: %s", esp_err_to_name(audio_err));
    }
    esp_err_t music_err = panel_music_init();
    if (music_err != ESP_OK) {
        ESP_LOGE(TAG, "music service init failed: %s", esp_err_to_name(music_err));
    }
    return ESP_OK;
}

void panel_services_get_settings(panel_settings_t *out)
{
    if (out == NULL) {
        return;
    }
    lock_settings();
    *out = s_settings;
    unlock_settings();
}

bool panel_services_update_settings(const panel_settings_t *settings)
{
    if (settings == NULL || !panel_settings_validate(settings)) {
        return false;
    }
    lock_settings();
    s_settings = *settings;
    unlock_settings();

    esp_err_t err = panel_bsp_set_brightness(settings->brightness);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "brightness update failed: %s", esp_err_to_name(err));
        return false;
    }
    panel_services_schedule_save();
    return true;
}

esp_err_t panel_services_set_brightness(int percent)
{
    if (percent < PANEL_BRIGHTNESS_MIN) {
        percent = PANEL_BRIGHTNESS_MIN;
    }
    if (percent > PANEL_BRIGHTNESS_MAX) {
        percent = PANEL_BRIGHTNESS_MAX;
    }
    lock_settings();
    s_settings.brightness = (uint8_t)percent;
    unlock_settings();

    esp_err_t err = panel_bsp_set_brightness(percent);
    if (err == ESP_OK) {
        panel_services_schedule_save();
    }
    return err;
}

void panel_services_schedule_save(void)
{
    if (s_save_timer == NULL) {
        return;
    }
    esp_timer_stop(s_save_timer);
    esp_err_t err = esp_timer_start_once(s_save_timer, SAVE_DELAY_US);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not schedule settings save: %s", esp_err_to_name(err));
    }
}

void panel_services_get_runtime(panel_runtime_t *out)
{
    if (out == NULL) {
        return;
    }
    out->uptime_s = (uint64_t)(esp_timer_get_time() / 1000000ULL);
    out->heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    out->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    out->task_count = uxTaskGetNumberOfTasks();
}

bool panel_services_get_local_time(struct tm *out)
{
    if (out == NULL) {
        return false;
    }
    time_t now = time(NULL);
    if (now <= 0 || localtime_r(&now, out) == NULL) {
        return false;
    }
    return true;
}

bool panel_services_time_is_calibrated(void)
{
    return panel_online_time_is_calibrated();
}

bool panel_services_wifi_connected(void)
{
    return panel_network_wifi_connected();
}

bool panel_services_bt_connected(void)
{
    return panel_network_ble_connected();
}

void panel_services_get_weather(panel_weather_snapshot_t *out)
{
    panel_online_get_weather(out);
}

void panel_services_get_city_search(panel_city_search_snapshot_t *out)
{
    panel_online_get_city_search(out);
}

esp_err_t panel_services_weather_refresh(void)
{
    return panel_online_refresh();
}

esp_err_t panel_services_weather_search(const char *query)
{
    return panel_online_search_city(query);
}

esp_err_t panel_services_weather_choose_city(unsigned index, uint32_t search_generation)
{
    return panel_online_choose_city(index, search_generation);
}

void panel_services_get_clock(panel_clock_snapshot_t *out)
{
    panel_clock_get_snapshot(out);
}

esp_err_t panel_services_clock_stopwatch_start(void)
{
    return panel_clock_stopwatch_start();
}

esp_err_t panel_services_clock_stopwatch_pause(void)
{
    return panel_clock_stopwatch_pause();
}

esp_err_t panel_services_clock_stopwatch_reset(void)
{
    return panel_clock_stopwatch_reset();
}

esp_err_t panel_services_clock_countdown_start(uint32_t seconds)
{
    return panel_clock_countdown_start(seconds);
}

esp_err_t panel_services_clock_countdown_pause(void)
{
    return panel_clock_countdown_pause();
}

esp_err_t panel_services_clock_countdown_resume(void)
{
    return panel_clock_countdown_resume();
}

esp_err_t panel_services_clock_countdown_reset(void)
{
    return panel_clock_countdown_reset();
}

esp_err_t panel_services_clock_countdown_ack(uint32_t generation)
{
    return panel_clock_countdown_ack(generation);
}

esp_err_t panel_services_clock_alarm_set(bool enabled, uint8_t hour, uint8_t minute)
{
    return panel_clock_alarm_set(enabled, hour, minute);
}

esp_err_t panel_services_clock_alarm_ack(uint32_t generation)
{
    return panel_clock_alarm_ack(generation);
}

void panel_services_get_files(panel_files_snapshot_t *out)
{
    panel_files_get_snapshot(out);
}

esp_err_t panel_services_files_retry(void) { return panel_files_retry(); }
esp_err_t panel_services_files_refresh(uint32_t generation)
{
    return panel_files_refresh(generation);
}
esp_err_t panel_services_files_enter(unsigned index, uint32_t generation)
{
    return panel_files_enter(index, generation);
}
esp_err_t panel_services_files_up(uint32_t generation) { return panel_files_up(generation); }
esp_err_t panel_services_files_root(uint32_t generation) { return panel_files_root(generation); }
esp_err_t panel_services_files_page(int delta, uint32_t generation)
{
    return panel_files_page(delta, generation);
}
esp_err_t panel_services_files_preview(unsigned index, uint32_t generation)
{
    return panel_files_preview(index, generation);
}
esp_err_t panel_services_files_close_preview(void) { return panel_files_close_preview(); }

void panel_services_get_assets(panel_assets_status_t *out)
{
    panel_assets_get_status(out);
}

esp_err_t panel_services_assets_request_font(void) { return panel_assets_request_font(); }
void panel_services_assets_cancel_font(void) { panel_assets_cancel_font(); }
const lv_font_t *panel_services_assets_acquire_font(void) { return panel_assets_acquire_font(); }
void panel_services_assets_release_font(void) { panel_assets_release_font(); }
