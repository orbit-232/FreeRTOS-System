#include "panel_online.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "panel_network.h"
#include "panel_online_model.h"

#include "panel_online_http.h"

static const char *TAG = "panel_online";

#define ONLINE_QUEUE_LEN 4
#define ONLINE_WORKER_STACK 12288
#define ONLINE_WORKER_PRIO 4
#define ONLINE_REFRESH_MS (20 * 60 * 1000)
#define ONLINE_RETRY_MS (60 * 1000)
#define ONLINE_MANUAL_THROTTLE_MS (30 * 1000)
#define ONLINE_SNTP_RETRY_MS (30 * 1000)
#define ONLINE_CITY_NAMESPACE "panel_online"
#define ONLINE_CITY_KEY "city"
#define ONLINE_CITY_MAGIC 0x504F4E4CUL
#define ONLINE_CITY_VERSION 1

typedef enum {
    ONLINE_CMD_REFRESH,
    ONLINE_CMD_SEARCH,
    ONLINE_CMD_SELECT_CITY,
} online_cmd_type_t;

typedef struct {
    online_cmd_type_t type;
    uint32_t generation;
    char query[PANEL_ONLINE_CITY_QUERY_MAX + 1];
    panel_city_result_t city;
} online_cmd_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    panel_city_result_t city;
} online_city_blob_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static bool s_started;
static bool s_calibrated;
static volatile bool s_calibrated_pending;
static bool s_sntp_started;
static int64_t s_sntp_retry_ms;
static int64_t s_manual_last_ms;
static int64_t s_last_success_ms;
static int64_t s_next_retry_ms;
static panel_city_result_t s_city;
static panel_weather_snapshot_t s_weather;
static panel_city_search_snapshot_t s_search;
static uint32_t s_city_generation;
static uint32_t s_search_generation;
static unsigned s_ntp_server_index;

/* CONFIG_LWIP_SNTP_MAX_SERVERS is 1 in this project.  Use one slot and
 * alternate a domestic primary/backup on retry instead of setting an
 * out-of-range index. */
static const char *const NTP_SERVERS[] = {
    "ntp.aliyun.com",
    "cn.pool.ntp.org",
};

static const panel_city_result_t DEFAULT_CITY = {
    .name = "广州",
    .region = "广东",
    .country = "中国",
    .latitude = 23.11667,
    .longitude = 113.25,
};

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000);
}

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void set_weather_reason_locked(const char *text)
{
    const char *value = text != NULL ? text : "";
    if (strcmp(s_weather.reason, value) != 0) {
        snprintf(s_weather.reason, sizeof(s_weather.reason), "%s", value);
        s_weather.generation++;
    }
}

static void set_search_reason_locked(const char *text)
{
    const char *value = text != NULL ? text : "";
    if (strcmp(s_search.error, value) != 0) {
        snprintf(s_search.error, sizeof(s_search.error), "%s", value);
        s_search.generation++;
    }
}

static void copy_city_locked(const panel_city_result_t *city)
{
    s_city = *city;
    snprintf(s_weather.city, sizeof(s_weather.city), "%s", city->name);
    snprintf(s_weather.region, sizeof(s_weather.region), "%s", city->region);
    snprintf(s_weather.country, sizeof(s_weather.country), "%s", city->country);
    s_weather.latitude = city->latitude;
    s_weather.longitude = city->longitude;
}

static void clear_weather_data_locked(void)
{
    s_weather.has_data = false;
    s_weather.stale = false;
    s_weather.refreshing = false;
    s_weather.updated_epoch = 0;
    s_weather.data_epoch = 0;
    s_weather.current_temp_c = 0;
    s_weather.apparent_temp_c = 0;
    s_weather.humidity_pct = 0;
    s_weather.wind_speed_kmh = 0;
    s_weather.weather_code = 0;
    s_weather.kind = PANEL_WEATHER_UNKNOWN;
    s_weather.hour_count = 0;
    s_weather.day_count = 0;
    memset(s_weather.hours, 0, sizeof(s_weather.hours));
    memset(s_weather.days, 0, sizeof(s_weather.days));
}

static bool load_city(panel_city_result_t *out)
{
    if (out == NULL) {
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(ONLINE_CITY_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    online_city_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(handle, ONLINE_CITY_KEY, &blob, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != sizeof(blob) || blob.magic != ONLINE_CITY_MAGIC ||
        blob.version != ONLINE_CITY_VERSION || !panel_online_city_valid(&blob.city)) {
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "saved city invalid, using Guangzhou default");
        }
        return false;
    }
    *out = blob.city;
    return true;
}

static bool save_city(const panel_city_result_t *city)
{
    if (city == NULL || !panel_online_city_valid(city)) {
        return false;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ONLINE_CITY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "city nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    online_city_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = ONLINE_CITY_MAGIC;
    blob.version = ONLINE_CITY_VERSION;
    blob.city = *city;
    err = nvs_set_blob(handle, ONLINE_CITY_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "city save failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool json_content_type(const char *content_type)
{
    if (content_type == NULL) {
        return false;
    }
    const char *p = content_type;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return strncasecmp(p, "application/json", 16) == 0;
}

static void apply_forecast_locked(const panel_online_forecast_t *forecast)
{
    s_weather.has_data = true;
    s_weather.stale = false;
    s_weather.refreshing = false;
    s_weather.current_temp_c = forecast->current_temp_c;
    s_weather.apparent_temp_c = forecast->apparent_temp_c;
    s_weather.humidity_pct = forecast->humidity_pct;
    s_weather.wind_speed_kmh = forecast->wind_speed_kmh;
    s_weather.weather_code = forecast->weather_code;
    s_weather.kind = forecast->kind;
    memcpy(s_weather.hours, forecast->hours, sizeof(forecast->hours));
    s_weather.hour_count = forecast->hour_count;
    memcpy(s_weather.days, forecast->days, sizeof(forecast->days));
    s_weather.day_count = forecast->day_count;
    s_weather.data_epoch = forecast->current_epoch;
    s_weather.updated_epoch = time(NULL);
    if (s_weather.updated_epoch < 0) {
        s_weather.updated_epoch = 0;
    }
    s_weather.reason[0] = '\0';
    s_weather.generation++;
    s_last_success_ms = now_ms();
    s_next_retry_ms = 0;
}

static void do_refresh(void)
{
    bool wifi = panel_network_wifi_connected();
    bool calibrated;
    lock();
    calibrated = s_calibrated;
    unlock();
    if (!wifi) {
        lock();
        set_weather_reason_locked("等待网络");
        unlock();
        return;
    }
    if (!calibrated) {
        lock();
        set_weather_reason_locked("等待校时");
        unlock();
        return;
    }

    lock();
    if (s_weather.refreshing) {
        unlock();
        return;
    }
    s_weather.refreshing = true;
    set_weather_reason_locked("刷新中");
    uint32_t city_generation = s_city_generation;
    panel_city_result_t city = s_city;
    unlock();

    char url[512];
    esp_err_t err = panel_online_build_forecast_url(city.latitude, city.longitude,
                                                     url, sizeof(url));
    char *body = (char *)malloc(PANEL_ONLINE_FORECAST_MAX + 1);
    if (body == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    size_t len = 0;
    int status = 0;
    char content_type[64] = {0};
    if (err == ESP_OK) {
        err = panel_online_http_get(url, body, PANEL_ONLINE_FORECAST_MAX + 1, &len,
                                    &status, content_type, sizeof(content_type));
    }

    panel_online_forecast_t forecast;
    memset(&forecast, 0, sizeof(forecast));
    char parse_reason[PANEL_ONLINE_REASON_MAX + 1] = {0};
    bool parsed = false;
    const char *error_text = "网络请求失败";
    if (err == ESP_ERR_INVALID_SIZE) {
        error_text = "响应超长";
    } else if (err == ESP_ERR_TIMEOUT) {
        error_text = "网络请求超时";
    } else if (err == ESP_ERR_INVALID_RESPONSE) {
        error_text = "内容类型错误";
    } else if (err != ESP_OK) {
        error_text = "网络请求失败";
    } else if (status != 200) {
        error_text = "HTTP 状态错误";
    } else if (!json_content_type(content_type)) {
        error_text = "内容类型错误";
    } else {
        esp_err_t parse_err = panel_online_parse_forecast(body, len, time(NULL), &forecast,
                                                          parse_reason,
                                                          sizeof(parse_reason));
        if (parse_err == ESP_OK) {
            parsed = true;
        } else {
            error_text = parse_reason[0] ? parse_reason : "JSON 无效";
        }
    }
    free(body);

    int64_t now = now_ms();
    lock();
    if (city_generation != s_city_generation) {
        /* A newer city selection owns the weather snapshot now. */
        unlock();
        return;
    }
    if (parsed) {
        apply_forecast_locked(&forecast);
        ESP_LOGI(TAG, "weather updated: city=%s temp=%d humidity=%d wind=%.1f",
                 s_city.name, s_weather.current_temp_c, s_weather.humidity_pct,
                 s_weather.wind_speed_kmh);
    } else {
        s_weather.refreshing = false;
        s_weather.stale = s_weather.has_data;
        set_weather_reason_locked(error_text);
        s_next_retry_ms = now + ONLINE_RETRY_MS;
        ESP_LOGW(TAG, "weather refresh failed: %s", error_text);
    }
    unlock();
}

static void do_search(const online_cmd_t *cmd)
{
    if (cmd == NULL) {
        return;
    }
    lock();
    if (cmd->generation != s_search_generation) {
        /* A newer search owns the search snapshot; do not clear its results. */
        unlock();
        return;
    }
    bool wifi = panel_network_wifi_connected();
    bool calibrated = s_calibrated;
    if (!wifi) {
        s_search.searching = false;
        set_search_reason_locked("等待网络");
        unlock();
        return;
    }
    if (!calibrated) {
        s_search.searching = false;
        set_search_reason_locked("等待校时");
        unlock();
        return;
    }
    s_search.searching = true;
    set_search_reason_locked("");
    uint32_t generation = cmd->generation;
    unlock();

    char url[512];
    esp_err_t err = panel_online_build_geocode_url(cmd->query, url, sizeof(url));
    char *body = (char *)malloc(PANEL_ONLINE_GEOCODE_MAX + 1);
    if (body == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    size_t len = 0;
    int status = 0;
    char content_type[64] = {0};
    if (err == ESP_OK) {
        err = panel_online_http_get(url, body, PANEL_ONLINE_GEOCODE_MAX + 1, &len,
                                    &status, content_type, sizeof(content_type));
    }

    panel_city_result_t results[PANEL_ONLINE_SEARCH_RESULTS];
    memset(results, 0, sizeof(results));
    unsigned count = 0;
    char parse_reason[PANEL_ONLINE_REASON_MAX + 1] = {0};
    bool parsed = false;
    const char *error_text = "城市搜索失败";
    if (err == ESP_ERR_INVALID_SIZE) {
        error_text = "响应超长";
    } else if (err == ESP_ERR_TIMEOUT) {
        error_text = "网络请求超时";
    } else if (err == ESP_ERR_INVALID_RESPONSE) {
        error_text = "内容类型错误";
    } else if (err != ESP_OK) {
        error_text = "网络请求失败";
    } else if (status != 200) {
        error_text = "HTTP 状态错误";
    } else if (!json_content_type(content_type)) {
        error_text = "内容类型错误";
    } else {
        esp_err_t parse_err = panel_online_parse_geocode(body, len, results,
                                                         PANEL_ONLINE_SEARCH_RESULTS, &count,
                                                         parse_reason,
                                                         sizeof(parse_reason));
        if (parse_err == ESP_OK) {
            parsed = true;
        } else {
            error_text = parse_reason[0] ? parse_reason : "城市未找到";
        }
    }
    free(body);

    lock();
    if (generation != s_search_generation) {
        unlock();
        return;
    }
    s_search.searching = false;
    if (parsed) {
        memcpy(s_search.results, results, sizeof(results));
        s_search.count = count;
        s_search.error[0] = '\0';
        s_search.generation++;
    } else {
        s_search.count = 0;
        set_search_reason_locked(error_text);
    }
    unlock();
}

static void do_select_city(const online_cmd_t *cmd)
{
    if (cmd == NULL) {
        return;
    }
    lock();
    if (cmd->generation != s_city_generation) {
        unlock();
        return;
    }
    panel_city_result_t city = s_city;
    unlock();

    if (!save_city(&city)) {
        ESP_LOGW(TAG, "city choice not persisted; using it for this boot");
    }
    do_refresh();
}

static void handle_cmd(const online_cmd_t *cmd)
{
    switch (cmd->type) {
    case ONLINE_CMD_REFRESH:
        do_refresh();
        break;
    case ONLINE_CMD_SEARCH:
        do_search(cmd);
        break;
    case ONLINE_CMD_SELECT_CITY:
        do_select_city(cmd);
        break;
    default:
        break;
    }
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    if (s_lock != NULL && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        s_calibrated = true;
        s_weather.calibrated = true;
        s_weather.generation++;
        s_next_retry_ms = 0;
        xSemaphoreGive(s_lock);
    } else {
        s_calibrated_pending = true;
    }
}

static void manage_sntp(bool wifi, bool calibrated)
{
    int64_t now = now_ms();
    if (wifi && !s_sntp_started) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
        esp_sntp_setservername(0, NTP_SERVERS[s_ntp_server_index]);
        esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
        esp_sntp_set_sync_interval(3600000);
        esp_sntp_init();
        s_sntp_started = true;
        s_sntp_retry_ms = now + ONLINE_SNTP_RETRY_MS;
        ESP_LOGI(TAG, "SNTP started (CST-8, server=%s)",
                 NTP_SERVERS[s_ntp_server_index]);
        return;
    }
    if (!wifi && s_sntp_started) {
        esp_sntp_stop();
        s_sntp_started = false;
        ESP_LOGI(TAG, "SNTP stopped with Wi-Fi down");
        return;
    }
    if (wifi && s_sntp_started && !calibrated && now >= s_sntp_retry_ms) {
        s_ntp_server_index = (s_ntp_server_index + 1) % (sizeof(NTP_SERVERS) / sizeof(NTP_SERVERS[0]));
        esp_sntp_setservername(0, NTP_SERVERS[s_ntp_server_index]);
        if (!esp_sntp_restart()) {
            ESP_LOGW(TAG, "SNTP restart failed; re-initializing");
            esp_sntp_stop();
            s_sntp_started = false;
        }
        s_sntp_retry_ms = now + ONLINE_SNTP_RETRY_MS;
        ESP_LOGW(TAG, "SNTP retry with server=%s", NTP_SERVERS[s_ntp_server_index]);
    }
}

static void maybe_auto_refresh(void)
{
    lock();
    if (!s_weather.wifi_connected || !s_calibrated || s_weather.refreshing) {
        unlock();
        return;
    }
    int64_t now = now_ms();
    bool due = now >= s_next_retry_ms;
    bool need = !s_weather.has_data || s_weather.stale ||
                (s_last_success_ms > 0 && now - s_last_success_ms >= ONLINE_REFRESH_MS);
    unlock();
    if (need && due) {
        do_refresh();
    }
}

static void service_tick(void)
{
    bool wifi = panel_network_wifi_connected();
    bool calibrated;
    lock();
    if (s_calibrated_pending) {
        s_calibrated = true;
        s_calibrated_pending = false;
        s_next_retry_ms = 0;
    }
    calibrated = s_calibrated;
    bool wifi_changed = s_weather.wifi_connected != wifi;
    bool cal_changed = s_weather.calibrated != calibrated;
    s_weather.wifi_connected = wifi;
    s_weather.calibrated = calibrated;
    if (wifi_changed) {
        s_weather.generation++;
        if (wifi) {
            s_next_retry_ms = 0;
        }
    }
    if (cal_changed) {
        s_weather.generation++;
        if (calibrated) {
            s_next_retry_ms = 0;
        }
    }
    if (!wifi) {
        if (s_weather.has_data) {
            if (!s_weather.stale) {
                s_weather.stale = true;
                s_weather.generation++;
            }
            set_weather_reason_locked("离线，显示缓存");
        } else {
            set_weather_reason_locked("等待网络");
        }
    } else if (!calibrated) {
        set_weather_reason_locked("等待校时");
    } else if (!s_weather.has_data && !s_weather.refreshing) {
        if (s_weather.reason[0] == '\0' || strncmp(s_weather.reason, "等待", 6) == 0) {
            set_weather_reason_locked("等待刷新");
        }
    }
    unlock();

    manage_sntp(wifi, calibrated);
    if (wifi && calibrated) {
        maybe_auto_refresh();
    }
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        service_tick();
        online_cmd_t cmd;
        if (xQueueReceive(s_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            handle_cmd(&cmd);
        }
    }
}

esp_err_t panel_online_init(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_queue == NULL) {
        s_queue = xQueueCreate(ONLINE_QUEUE_LEN, sizeof(online_cmd_t));
    }
    if (s_lock == NULL || s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    panel_city_result_t city = DEFAULT_CITY;
    (void)load_city(&city);

    lock();
    memset(&s_weather, 0, sizeof(s_weather));
    memset(&s_search, 0, sizeof(s_search));
    s_calibrated = false;
    s_calibrated_pending = false;
    s_sntp_started = false;
    s_sntp_retry_ms = 0;
    s_manual_last_ms = 0;
    s_last_success_ms = 0;
    s_next_retry_ms = 0;
    s_city_generation = 1;
    s_search_generation = 0;
    s_city = city;
    copy_city_locked(&city);
    s_weather.wifi_connected = panel_network_wifi_connected();
    s_weather.calibrated = false;
    s_weather.reason[0] = '\0';
    snprintf(s_weather.reason, sizeof(s_weather.reason), "等待网络");
    s_weather.generation = 1;
    s_search.searching = false;
    s_search.count = 0;
    s_search.generation = 1;
    unlock();

    if (xTaskCreate(worker, "panel_online", ONLINE_WORKER_STACK, NULL,
                    ONLINE_WORKER_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "online worker started; city=%s lat=%.5f lon=%.5f",
             s_city.name, s_city.latitude, s_city.longitude);
    return ESP_OK;
}

void panel_online_get_weather(panel_weather_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_weather;
    unlock();
}

void panel_online_get_city_search(panel_city_search_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_search;
    unlock();
}

bool panel_online_time_is_calibrated(void)
{
    if (s_lock == NULL) {
        return false;
    }
    lock();
    bool calibrated = s_calibrated;
    unlock();
    return calibrated;
}

esp_err_t panel_online_refresh(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t now = now_ms();
    lock();
    if (s_manual_last_ms != 0 && now - s_manual_last_ms < ONLINE_MANUAL_THROTTLE_MS) {
        unlock();
        return ESP_ERR_NOT_FINISHED;
    }
    unlock();

    online_cmd_t cmd = {.type = ONLINE_CMD_REFRESH};
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    lock();
    s_manual_last_ms = now;
    unlock();
    return ESP_OK;
}

esp_err_t panel_online_search_city(const char *query)
{
    if (!s_started || query == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = strlen(query);
    if (len == 0 || len > PANEL_ONLINE_CITY_QUERY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const unsigned char *p = (const unsigned char *)query; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    lock();
    if (!s_weather.wifi_connected) {
        s_search.searching = false;
        set_search_reason_locked("等待网络");
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_calibrated) {
        s_search.searching = false;
        set_search_reason_locked("等待校时");
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    online_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ONLINE_CMD_SEARCH;
    snprintf(cmd.query, sizeof(cmd.query), "%s", query);
    cmd.generation = s_search_generation + 1;
    /* Queue first, while holding the state lock.  A failed send must leave the
     * previous results/searching state untouched, and the worker cannot start
     * the command before the generation is committed below. */
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        unlock();
        return ESP_ERR_TIMEOUT;
    }
    s_search_generation = cmd.generation;
    s_search.searching = true;
    s_search.count = 0;
    s_search.error[0] = '\0';
    s_search.generation++;
    unlock();
    return ESP_OK;
}

esp_err_t panel_online_choose_city(unsigned index, uint32_t search_generation)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    online_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ONLINE_CMD_SELECT_CITY;
    lock();
    if (search_generation != s_search.generation || index >= s_search.count) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    panel_city_result_t city = s_search.results[index];
    cmd.city = city;
    cmd.generation = s_city_generation + 1;
    /* Transactional: no city/cache/generation change unless the command is
     * actually accepted.  The worker blocks on the same lock until commit. */
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        unlock();
        return ESP_ERR_TIMEOUT;
    }
    s_city_generation = cmd.generation;
    s_city = city;
    copy_city_locked(&city);
    clear_weather_data_locked();
    set_weather_reason_locked("等待刷新");
    s_weather.generation++;
    s_next_retry_ms = 0;
    unlock();
    return ESP_OK;
}
