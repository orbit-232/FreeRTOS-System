#include "panel_wallpaper.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel_files.h"

#define TAG "panel_wallpaper"
#define WALLPAPER_W 1280U
#define WALLPAPER_H 800U
#define WALLPAPER_BYTES (WALLPAPER_W * WALLPAPER_H * 2U)
#define WALLPAPER_REL "/panel/assets/lock/lock_wallpaper.rgb565"
#define WALLPAPER_STACK 8192U
#define WALLPAPER_RETRY_MS 2000U

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t width;
    uint16_t height;
    uint16_t format;
    uint16_t reserved;
    uint32_t data_bytes;
    uint32_t crc32;
    uint32_t reserved2;
} wallpaper_header_t;

static panel_wallpaper_t s_info;
static bool s_ready;
static bool s_wanted;
static bool s_started;
static bool s_failed_logged;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t wallpaper_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static bool read_exact(FILE *file, void *buf, size_t len)
{
    return fread(buf, 1, len, file) == len;
}

esp_err_t panel_wallpaper_load_file(const char *path)
{
    if (path == NULL || path[0] == '\0') return ESP_ERR_INVALID_ARG;

    taskENTER_CRITICAL(&s_state_mux);
    bool already_ready = s_ready;
    taskEXIT_CRITICAL(&s_state_mux);
    if (already_ready) return ESP_OK;

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (!s_failed_logged) {
            ESP_LOGW(TAG, "wallpaper missing: %s", path);
            s_failed_logged = true;
        }
        return ESP_ERR_NOT_FOUND;
    }

    wallpaper_header_t header;
    if (!read_exact(file, &header, sizeof(header)) ||
        memcmp(header.magic, "PWL1", 4) != 0 ||
        header.width != WALLPAPER_W || header.height != WALLPAPER_H ||
        header.format != 1U || header.data_bytes != WALLPAPER_BYTES) {
        fclose(file);
        if (!s_failed_logged) {
            ESP_LOGW(TAG, "wallpaper header invalid: %s", path);
            s_failed_logged = true;
        }
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *pixels = heap_caps_malloc(WALLPAPER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        fclose(file);
        ESP_LOGE(TAG, "wallpaper PSRAM allocation failed");
        return ESP_ERR_NO_MEM;
    }
    if (!read_exact(file, pixels, WALLPAPER_BYTES) ||
        wallpaper_crc32(pixels, WALLPAPER_BYTES) != header.crc32) {
        fclose(file);
        heap_caps_free(pixels);
        if (!s_failed_logged) {
            ESP_LOGW(TAG, "wallpaper data/CRC invalid: %s", path);
            s_failed_logged = true;
        }
        return ESP_ERR_INVALID_SIZE;
    }
    fclose(file);

    taskENTER_CRITICAL(&s_state_mux);
    if (s_ready || !s_wanted) {
        taskEXIT_CRITICAL(&s_state_mux);
        heap_caps_free(pixels);
        return s_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    s_info.width = WALLPAPER_W;
    s_info.height = WALLPAPER_H;
    s_info.pixels = pixels;
    s_ready = true;
    taskEXIT_CRITICAL(&s_state_mux);
    s_failed_logged = false;
    ESP_LOGI(TAG, "wallpaper ready: %u bytes from %s", (unsigned)WALLPAPER_BYTES, path);
    return ESP_OK;
}

const panel_wallpaper_t *panel_wallpaper_get(void)
{
    const panel_wallpaper_t *result = NULL;
    taskENTER_CRITICAL(&s_state_mux);
    if (s_ready) result = &s_info;
    taskEXIT_CRITICAL(&s_state_mux);
    return result;
}

void panel_wallpaper_request(void)
{
    taskENTER_CRITICAL(&s_state_mux);
    s_wanted = true;
    taskEXIT_CRITICAL(&s_state_mux);
}

void panel_wallpaper_release(void)
{
    uint8_t *pixels = NULL;
    taskENTER_CRITICAL(&s_state_mux);
    s_wanted = false;
    if (s_ready) {
        pixels = (uint8_t *)s_info.pixels;
        s_info.pixels = NULL;
        s_info.width = 0;
        s_info.height = 0;
        s_ready = false;
    }
    taskEXIT_CRITICAL(&s_state_mux);
    if (pixels != NULL) heap_caps_free(pixels);
}

static bool wallpaper_is_ready(void)
{
    taskENTER_CRITICAL(&s_state_mux);
    bool ready = s_ready;
    taskEXIT_CRITICAL(&s_state_mux);
    return ready;
}

static bool wallpaper_is_wanted(void)
{
    taskENTER_CRITICAL(&s_state_mux);
    bool wanted = s_wanted;
    taskEXIT_CRITICAL(&s_state_mux);
    return wanted;
}

static void wallpaper_worker(void *arg)
{
    (void)arg;
    char path[192];
    for (;;) {
        if (!wallpaper_is_ready() && wallpaper_is_wanted() && panel_files_card_ready()) {
            const char *root = panel_files_root_path();
            if (root != NULL && root[0] != '\0') {
                int n = snprintf(path, sizeof(path), "%s%s", root, WALLPAPER_REL);
                if (n > 0 && (size_t)n < sizeof(path) && panel_files_io_acquire()) {
                    (void)panel_wallpaper_load_file(path);
                    panel_files_io_release();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(WALLPAPER_RETRY_MS));
    }
}

esp_err_t panel_wallpaper_init(void)
{
    if (s_started) return ESP_OK;
    if (xTaskCreate(wallpaper_worker, "panel_wallpaper", WALLPAPER_STACK, NULL, 3, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}
