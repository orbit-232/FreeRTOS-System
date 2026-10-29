#include "fake_online.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "panel_online_http.h"

static bool g_wifi;
static int g_sntp_starts;
static int g_sntp_stops;
static int g_sntp_restarts;
static char g_ntp_server[64];
static sntp_sync_time_cb_t g_sync_cb;

static char g_forecast[16384];
static size_t g_forecast_len;
static int g_forecast_status;
static char g_forecast_type[64];
static char g_geocode[8192];
static size_t g_geocode_len;
static int g_geocode_status;
static char g_geocode_type[64];
static esp_err_t g_http_error;
static unsigned g_http_delay_ms;
static bool g_block_next;
static HANDLE g_block_event;
static volatile LONG g_http_calls;
static char g_last_url[512];

static unsigned char g_city_blob[512];
static size_t g_city_blob_len;
static bool g_city_saved;

void fake_online_reset(void)
{
    g_wifi = false;
    g_sntp_starts = 0;
    g_sntp_stops = 0;
    g_sntp_restarts = 0;
    g_ntp_server[0] = '\0';
    g_sync_cb = NULL;
    g_forecast[0] = '\0';
    g_forecast_len = 0;
    g_forecast_status = 0;
    g_forecast_type[0] = '\0';
    g_geocode[0] = '\0';
    g_geocode_len = 0;
    g_geocode_status = 0;
    g_geocode_type[0] = '\0';
    g_http_error = ESP_OK;
    g_http_delay_ms = 0;
    g_block_next = false;
    g_http_calls = 0;
    g_last_url[0] = '\0';
    g_city_blob_len = 0;
    g_city_saved = false;
    if (g_block_event != NULL) {
        CloseHandle(g_block_event);
        g_block_event = NULL;
    }
}

void fake_online_set_wifi(bool connected) { g_wifi = connected; }
bool panel_network_wifi_connected(void) { return g_wifi; }

void fake_online_fire_sntp_sync(void)
{
    if (g_sync_cb != NULL) {
        struct timeval tv = {.tv_sec = (time_t)time(NULL), .tv_usec = 0};
        g_sync_cb(&tv);
    }
}
int fake_online_sntp_starts(void) { return g_sntp_starts; }
int fake_online_sntp_stops(void) { return g_sntp_stops; }
int fake_online_sntp_restarts(void) { return g_sntp_restarts; }
const char *fake_online_last_ntp_server(void) { return g_ntp_server; }

void fake_online_http_set_forecast(int status, const char *content_type,
                                   const char *body, size_t len)
{
    g_forecast_status = status;
    snprintf(g_forecast_type, sizeof(g_forecast_type), "%s",
             content_type != NULL ? content_type : "");
    if (body != NULL && len < sizeof(g_forecast)) {
        memcpy(g_forecast, body, len);
        g_forecast[len] = '\0';
        g_forecast_len = len;
    } else {
        g_forecast_len = 0;
    }
}

void fake_online_http_set_geocode(int status, const char *content_type,
                                  const char *body, size_t len)
{
    g_geocode_status = status;
    snprintf(g_geocode_type, sizeof(g_geocode_type), "%s",
             content_type != NULL ? content_type : "");
    if (body != NULL && len < sizeof(g_geocode)) {
        memcpy(g_geocode, body, len);
        g_geocode[len] = '\0';
        g_geocode_len = len;
    } else {
        g_geocode_len = 0;
    }
}

void fake_online_http_set_error(esp_err_t error) { g_http_error = error; }
void fake_online_http_set_delay_ms(unsigned delay_ms) { g_http_delay_ms = delay_ms; }

void fake_online_http_block_next(void)
{
    if (g_block_event == NULL) {
        g_block_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    }
    ResetEvent(g_block_event);
    g_block_next = true;
}

void fake_online_http_release(void)
{
    if (g_block_event != NULL) {
        SetEvent(g_block_event);
    }
}

int fake_online_http_calls(void) { return (int)g_http_calls; }
const char *fake_online_last_url(void) { return g_last_url; }
bool fake_online_nvs_has_city(void) { return g_city_saved; }

esp_err_t panel_online_http_get(const char *url, char *body, size_t cap, size_t *len,
                                int *status, char *content_type, size_t content_type_cap)
{
    InterlockedIncrement(&g_http_calls);
    snprintf(g_last_url, sizeof(g_last_url), "%s", url != NULL ? url : "");
    bool geocode = url != NULL && strstr(url, "geocoding-api") != NULL;
    char local_body[16384];
    size_t local_len = geocode ? g_geocode_len : g_forecast_len;
    int local_status = geocode ? g_geocode_status : g_forecast_status;
    char local_type[64];
    snprintf(local_type, sizeof(local_type), "%s", geocode ? g_geocode_type : g_forecast_type);
    if (local_len >= sizeof(local_body)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(local_body, geocode ? g_geocode : g_forecast, local_len);
    local_body[local_len] = '\0';
    if (g_block_next) {
        g_block_next = false;
        if (g_block_event != NULL) {
            WaitForSingleObject(g_block_event, 10000);
        }
    }
    if (g_http_delay_ms > 0) {
        Sleep(g_http_delay_ms);
    }
    if (g_http_error != ESP_OK) {
        return g_http_error;
    }
    if (len == NULL || status == NULL || body == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (local_len >= cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(body, local_body, local_len);
    body[local_len] = '\0';
    *len = local_len;
    *status = local_status;
    if (content_type != NULL && content_type_cap > 0) {
        snprintf(content_type, content_type_cap, "%s", local_type);
    }
    return ESP_OK;
}

void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t mode) { (void)mode; }
void esp_sntp_set_sync_mode(sntp_sync_mode_t mode) { (void)mode; }
void esp_sntp_setservername(uint8_t idx, const char *server)
{
    (void)idx;
    snprintf(g_ntp_server, sizeof(g_ntp_server), "%s", server != NULL ? server : "");
}
void esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t cb) { g_sync_cb = cb; }
void esp_sntp_set_sync_interval(uint32_t interval_ms) { (void)interval_ms; }
void esp_sntp_init(void) { g_sntp_starts++; }
void esp_sntp_stop(void) { g_sntp_stops++; }
bool esp_sntp_restart(void)
{
    g_sntp_restarts++;
    return true;
}

esp_err_t nvs_open(const char *namespace_name, int open_mode, nvs_handle_t *out_handle)
{
    (void)open_mode;
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (namespace_name == NULL || strcmp(namespace_name, "panel_online") != 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    (void)handle;
    if (key == NULL || strcmp(key, "city") != 0 || !g_city_saved) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (length == NULL || out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*length < g_city_blob_len) {
        *length = g_city_blob_len;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, g_city_blob, g_city_blob_len);
    *length = g_city_blob_len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    if (key == NULL || strcmp(key, "city") != 0 || value == NULL ||
        length > sizeof(g_city_blob)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(g_city_blob, value, length);
    g_city_blob_len = length;
    g_city_saved = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
