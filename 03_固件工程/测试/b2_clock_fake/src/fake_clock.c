#include "fake_clock.h"

#include <string.h>

#include "nvs.h"

static int64_t g_epoch;
static int64_t g_mono_us;
static bool g_calibrated;
static uint8_t g_nvs_blob[64];
static size_t g_nvs_len;
static bool g_nvs_saved;
static bool g_nvs_fail;
static unsigned g_nvs_writes;

int64_t panel_clock_platform_epoch(void) { return g_epoch; }
int64_t panel_clock_platform_monotonic_us(void) { return g_mono_us; }
bool panel_online_time_is_calibrated(void) { return g_calibrated; }

void fake_clock_set_epoch(int64_t epoch) { g_epoch = epoch; }
void fake_clock_advance_monotonic_us(int64_t delta_us) { g_mono_us += delta_us; }
int64_t fake_clock_epoch(void) { return g_epoch; }
int64_t fake_clock_monotonic_us(void) { return g_mono_us; }
void fake_clock_set_calibrated(bool calibrated) { g_calibrated = calibrated; }
bool fake_clock_is_calibrated(void) { return g_calibrated; }

void fake_clock_reset(void)
{
    g_epoch = 0;
    g_mono_us = 0;
    g_calibrated = false;
    memset(g_nvs_blob, 0, sizeof(g_nvs_blob));
    g_nvs_len = 0;
    g_nvs_saved = false;
    g_nvs_fail = false;
    g_nvs_writes = 0;
}

void fake_clock_nvs_reset(void)
{
    memset(g_nvs_blob, 0, sizeof(g_nvs_blob));
    g_nvs_len = 0;
    g_nvs_saved = false;
    g_nvs_fail = false;
    g_nvs_writes = 0;
}

void fake_clock_nvs_set_raw(const void *data, size_t len)
{
    if (data == NULL || len > sizeof(g_nvs_blob)) {
        return;
    }
    memcpy(g_nvs_blob, data, len);
    g_nvs_len = len;
    g_nvs_saved = true;
}

bool fake_clock_nvs_saved(void) { return g_nvs_saved; }
size_t fake_clock_nvs_len(void) { return g_nvs_len; }
unsigned fake_clock_nvs_writes(void) { return g_nvs_writes; }
void fake_clock_nvs_set_fail(bool fail) { g_nvs_fail = fail; }

esp_err_t nvs_open(const char *namespace_name, int open_mode, nvs_handle_t *out_handle)
{
    (void)open_mode;
    if (out_handle == NULL) return ESP_ERR_INVALID_ARG;
    if (namespace_name == NULL || strcmp(namespace_name, "panel_clock") != 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    (void)handle;
    if (key == NULL || strcmp(key, "alarm") != 0 || !g_nvs_saved) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (length == NULL || out_value == NULL) return ESP_ERR_INVALID_ARG;
    if (*length < g_nvs_len) {
        *length = g_nvs_len;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, g_nvs_blob, g_nvs_len);
    *length = g_nvs_len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    if (key == NULL || strcmp(key, "alarm") != 0 || value == NULL ||
        length > sizeof(g_nvs_blob)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_nvs_fail) {
        return ESP_FAIL;
    }
    memcpy(g_nvs_blob, value, length);
    g_nvs_len = length;
    g_nvs_saved = true;
    g_nvs_writes++;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}
