#include "panel_music_import.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_files.h"
#include "panel_music.h"
#include "panel_music_import_policy.h"
#include "panel_music_tracks.h"
#include "panel_network.h"

#define TAG "music_import"
#define IMPORT_PORT 8080
#define IMPORT_MAX_BYTES (32U * 1024U * 1024U)
#define IMPORT_TOTAL_MS 360000U
#define IMPORT_CHUNK 2048U
#define IMPORT_TOKEN_MAX 64U

typedef enum {
    IMPORT_IDLE = 0,
    IMPORT_STARTING,
    IMPORT_ACTIVE,
    IMPORT_STOPPING,
} import_state_t;

static httpd_handle_t s_server;
static char s_token[IMPORT_TOKEN_MAX + 1U];
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_control;
static import_state_t s_state;
static uint32_t s_generation;
static bool s_transfer_active;
static bool s_stop_requested;

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return crc;
}

static bool token_ok(const char *token)
{
    if (token == NULL || s_mutex == NULL) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = false;
    if (s_token[0] != '\0' && strlen(token) == strlen(s_token)) {
        unsigned diff = 0;
        for (size_t i = 0; i < strlen(s_token); i++) diff |= (unsigned)(token[i] ^ s_token[i]);
        ok = diff == 0;
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

static bool import_active(void)
{
    if (s_mutex == NULL) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool active = s_state == IMPORT_ACTIVE && !s_stop_requested;
    xSemaphoreGive(s_mutex);
    return active;
}

static void request_stop(void)
{
    if (s_mutex == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state != IMPORT_IDLE) {
        s_stop_requested = true;
        s_state = IMPORT_STOPPING;
        memset(s_token, 0, sizeof(s_token));
    }
    TaskHandle_t control = s_control;
    xSemaphoreGive(s_mutex);
    if (control != NULL) xTaskNotifyGive(control);
}

static void control_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(120000));
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (notified == 0 && s_state == IMPORT_ACTIVE && !s_transfer_active) {
            s_stop_requested = true;
        }
        bool stop = s_stop_requested;
        httpd_handle_t server = stop ? s_server : NULL;
        if (stop) {
            s_server = NULL;
            s_state = IMPORT_STOPPING;
            memset(s_token, 0, sizeof(s_token));
        }
        xSemaphoreGive(s_mutex);
        if (!stop) continue;
        if (server != NULL) httpd_stop(server);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = IMPORT_IDLE;
        s_transfer_active = false;
        s_stop_requested = false;
        s_generation++;
        xSemaphoreGive(s_mutex);
    }
}

static void reply(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, body);
}

static bool path_pair(int track, char *target, size_t tcap, char *temp, size_t tmpcap)
{
    const panel_music_track_t *meta = panel_music_track_at((unsigned)track);
    const char *root = panel_files_root_path();
    if (meta == NULL || root == NULL || root[0] == '\0') return false;
    int n = snprintf(target, tcap, "%s/panel/music/%s", root, meta->file);
    int m = snprintf(temp, tmpcap, "%s/panel/music/.import-%s.tmp", root, meta->id);
    return n > 0 && (size_t)n < tcap && m > 0 && (size_t)m < tmpcap;
}

static int query_track(httpd_req_t *req)
{
    char query[320];
    char track_id[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return -1;
    if (httpd_query_key_value(query, "track", track_id, sizeof(track_id)) != ESP_OK) {
        snprintf(track_id, sizeof(track_id), "%s", "after_hours");
    }
    return panel_music_track_find(track_id);
}

static bool cleanup_temp(const char *temp)
{
    bool ok = false;
    if (panel_files_io_acquire()) {
        remove(temp);
        panel_files_io_release();
        ok = true;
    }
    return ok;
}

/* Called with the mount pin held. Uses short I/O leases and a bounded stack
 * buffer so an existing target can be checked without uploading it. */
static bool verify_target(const char *target, uint32_t *bytes, uint32_t *crc, bool *exists)
{
    *bytes = 0;
    *crc = 0;
    *exists = false;
    if (!panel_files_io_acquire()) return false;
    FILE *f = fopen(target, "rb");
    if (f == NULL) {
        panel_files_io_release();
        return true;
    }
    *exists = true;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        panel_files_io_release();
        return false;
    }
    long size = ftell(f);
    if (size < 0 || size > (long)(32U * 1024U * 1024U) || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        panel_files_io_release();
        return false;
    }
    *bytes = (uint32_t)size;
    panel_files_io_release();

    uint32_t c = 0xFFFFFFFFU;
    uint8_t buf[2048];
    bool ok = true;
    for (;;) {
        if (!panel_files_io_acquire()) {
            ok = false;
            break;
        }
        size_t n = fread(buf, 1, sizeof(buf), f);
        bool bad = ferror(f) != 0;
        panel_files_io_release();
        if (n > 0) c = crc32_update(c, buf, n);
        if (bad) {
            ok = false;
            break;
        }
        if (n == 0) break;
    }
    if (panel_files_io_acquire()) {
        fclose(f);
        panel_files_io_release();
    } else {
        fclose(f);
        ok = false;
    }
    if (!ok) return false;
    *crc = ~c;
    return true;
}

static esp_err_t import_handler(httpd_req_t *req)
{
    if (!import_active()) {
        reply(req, "403 Forbidden", "import closed\n");
        return ESP_OK;
    }
    char query[320];
    char token[IMPORT_TOKEN_MAX + 1U] = {0};
    char name[160] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK ||
        !token_ok(token)) {
        reply(req, "403 Forbidden", "bad token\n");
        request_stop();
        return ESP_OK;
    }
    if (s_control != NULL) xTaskNotifyGive(s_control);
    (void)httpd_query_key_value(query, "name", name, sizeof(name));
    if (!panel_music_import_name_valid(name)) {
        reply(req, "400 Bad Request", "bad name\n");
        request_stop();
        return ESP_OK;
    }
    int track = query_track(req);
    if (track < 0) {
        reply(req, "400 Bad Request", "bad track\n");
        request_stop();
        return ESP_OK;
    }
    if (!panel_music_import_size_valid((uint32_t)req->content_len)) {
        reply(req, "413 Payload Too Large", "size limit\n");
        request_stop();
        return ESP_OK;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state != IMPORT_ACTIVE || s_stop_requested) {
        xSemaphoreGive(s_mutex);
        reply(req, "403 Forbidden", "import stopped\n");
        request_stop();
        return ESP_OK;
    }
    s_transfer_active = true;
    xSemaphoreGive(s_mutex);
    char crc_text[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-CRC32", crc_text, sizeof(crc_text)) != ESP_OK) {
        reply(req, "400 Bad Request", "missing crc\n");
        request_stop();
        return ESP_OK;
    }
    char *end = NULL;
    unsigned long expected_crc = strtoul(crc_text, &end, 16);
    if (end == crc_text || *end != '\0') {
        reply(req, "400 Bad Request", "bad crc\n");
        request_stop();
        return ESP_OK;
    }

    char target[224];
    char temp[224];
    if (!path_pair(track, target, sizeof(target), temp, sizeof(temp)) ||
        !panel_files_card_ready()) {
        reply(req, "503 Service Unavailable", "no card\n");
        request_stop();
        return ESP_OK;
    }
    if (!panel_files_mount_pin_acquire()) {
        reply(req, "503 Service Unavailable", "tf busy/retry\n");
        request_stop();
        return ESP_OK;
    }
    panel_music_stop();

    uint32_t existing_bytes = 0;
    uint32_t existing_crc = 0;
    bool existing = false;
    if (!verify_target(target, &existing_bytes, &existing_crc, &existing)) {
        panel_files_mount_pin_release();
        reply(req, "500 Internal Server Error", "verify failed\n");
        request_stop();
        return ESP_OK;
    }
    if (existing) {
        panel_files_mount_pin_release();
        char body[96];
        if (existing_bytes == (uint32_t)req->content_len &&
            existing_crc == (uint32_t)expected_crc) {
            snprintf(body, sizeof(body), "existing matched bytes=%u crc=%08x\n",
                     (unsigned)existing_bytes, (unsigned)existing_crc);
            reply(req, "200 OK", body);
        } else {
            snprintf(body, sizeof(body), "existing mismatch bytes=%u crc=%08x untouched\n",
                     (unsigned)existing_bytes, (unsigned)existing_crc);
            reply(req, "409 Conflict", body);
        }
        ESP_LOGI(TAG, "existing post bytes=%u crc=%08x", (unsigned)existing_bytes,
                 (unsigned)existing_crc);
        request_stop();
        return ESP_OK;
    }

    bool created = false;
    FILE *out = NULL;
    bool ok = false;
    uint32_t calc_crc = 0xFFFFFFFFU;
    uint32_t total = 0;
    int64_t start_us = esp_timer_get_time();

    if (!panel_files_io_acquire()) {
        panel_files_mount_pin_release();
        reply(req, "503 Service Unavailable", "tf busy\n");
        request_stop();
        return ESP_OK;
    }
    const char *root = panel_files_root_path();
    char panel_dir[192];
    char music_dir[208];
    snprintf(panel_dir, sizeof(panel_dir), "%s/panel", root);
    snprintf(music_dir, sizeof(music_dir), "%s/panel/music", root);
    if ((mkdir(panel_dir, 0777) != 0 && errno != EEXIST) ||
        (mkdir(music_dir, 0777) != 0 && errno != EEXIST)) {
        panel_files_io_release();
        panel_files_mount_pin_release();
        reply(req, "500 Internal Server Error", "mkdir failed\n");
        request_stop();
        return ESP_OK;
    }
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        panel_files_io_release();
        panel_files_mount_pin_release();
        reply(req, "409 Conflict", "temp exists; untouched\n");
        request_stop();
        return ESP_OK;
    }
    created = true;
    out = fdopen(fd, "wb");
    if (out == NULL) {
        close(fd);
        remove(temp);
        panel_files_io_release();
        panel_files_mount_pin_release();
        reply(req, "500 Internal Server Error", "temp open failed\n");
        request_stop();
        return ESP_OK;
    }
    panel_files_io_release();

    uint8_t buf[IMPORT_CHUNK];
    while (total < (uint32_t)req->content_len) {
        if ((esp_timer_get_time() - start_us) / 1000 > IMPORT_TOTAL_MS) break;
        size_t want = (uint32_t)req->content_len - total;
        if (want > sizeof(buf)) want = sizeof(buf);
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n <= 0) break;
        if (!import_active()) {
            ok = false;
            break;
        }
        if (!panel_files_io_acquire()) break;
        size_t written = fwrite(buf, 1, (size_t)n, out);
        bool flushed = fflush(out) == 0;
        panel_files_io_release();
        if (written != (size_t)n || !flushed) break;
        calc_crc = crc32_update(calc_crc, buf, (size_t)n);
        total += (uint32_t)n;
    }
    calc_crc = ~calc_crc;
    if (total != (uint32_t)req->content_len || calc_crc != (uint32_t)expected_crc) {
        ok = false;
    } else {
        ok = true;
    }

    if (panel_files_io_acquire()) {
        if (ok) {
            if (fflush(out) != 0) ok = false;
            if (ok && fsync(fileno(out)) != 0 && errno != EINVAL && errno != ENOSYS &&
                errno != ENOTSUP) {
                ok = false;
            }
        }
        if (fclose(out) != 0) ok = false;
        out = NULL;
        panel_files_io_release();
    } else {
        ok = false;
        if (out != NULL) fclose(out);
        out = NULL;
    }

    if (ok) {
        uint32_t read_crc = 0xFFFFFFFFU;
        uint32_t read_total = 0;
        FILE *verify = NULL;
        bool verify_locked = panel_files_io_acquire();
        if (verify_locked) verify = fopen(temp, "rb");
        if (verify != NULL) {
            uint8_t rb[IMPORT_CHUNK];
            size_t n;
            while ((n = fread(rb, 1, sizeof(rb), verify)) > 0) {
                read_crc = crc32_update(read_crc, rb, n);
                read_total += (uint32_t)n;
            }
            fclose(verify);
            panel_files_io_release();
            if (read_total != (uint32_t)req->content_len || ~read_crc != (uint32_t)expected_crc) {
                ok = false;
            }
        } else {
            if (verify_locked) panel_files_io_release();
            ok = false;
        }
    }

    if (ok) {
        if (panel_files_io_acquire()) {
            FILE *target_probe = fopen(target, "rb");
            if (target_probe != NULL) {
                fclose(target_probe);
                ok = false;
            } else if (rename(temp, target) != 0) {
                ok = false;
            }
            panel_files_io_release();
        } else {
            ok = false;
        }
    }

    if (!ok && created) cleanup_temp(temp);
    if (ok) {
        reply(req, "200 OK", "imported\n");
        ESP_LOGI(TAG, "import published path=%s bytes=%u crc=%08x", target,
                 (unsigned)total, (unsigned)expected_crc);
    } else {
        reply(req, "400 Bad Request", "import failed; temp removed\n");
        ESP_LOGW(TAG, "import failed bytes=%u", (unsigned)total);
    }
    panel_files_mount_pin_release();
    ESP_LOGI(TAG, "httpd stack_free=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    request_stop();
    return ESP_OK;
}

static esp_err_t info_handler(httpd_req_t *req)
{
    if (!import_active()) {
        reply(req, "403 Forbidden", "import closed\n");
        return ESP_OK;
    }
    char query[320];
    char token[IMPORT_TOKEN_MAX + 1U] = {0};
    char name[160] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK ||
        !token_ok(token)) {
        reply(req, "403 Forbidden", "bad token\n");
        return ESP_OK;
    }
    if (s_control != NULL) xTaskNotifyGive(s_control);
    (void)httpd_query_key_value(query, "name", name, sizeof(name));
    if (!panel_music_import_name_valid(name)) {
        reply(req, "400 Bad Request", "bad name\n");
        return ESP_OK;
    }
    int track = query_track(req);
    if (track < 0) {
        reply(req, "400 Bad Request", "bad track\n");
        return ESP_OK;
    }
    char target[224];
    char temp[224];
    if (!path_pair(track, target, sizeof(target), temp, sizeof(temp)) ||
        !panel_files_card_ready()) {
        reply(req, "503 Service Unavailable", "no card\n");
        return ESP_OK;
    }
    if (!panel_files_mount_pin_acquire()) {
        reply(req, "503 Service Unavailable", "tf busy/retry\n");
        return ESP_OK;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_transfer_active = true;
    xSemaphoreGive(s_mutex);
    uint32_t bytes = 0;
    uint32_t crc = 0;
    bool exists = false;
    bool ok = verify_target(target, &bytes, &crc, &exists);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_transfer_active = false;
    xSemaphoreGive(s_mutex);
    panel_files_mount_pin_release();
    if (!ok) {
        reply(req, "500 Internal Server Error", "verify failed\n");
        return ESP_OK;
    }
    if (!exists) {
        reply(req, "404 Not Found", "absent\n");
        return ESP_OK;
    }
    char body[96];
    snprintf(body, sizeof(body), "existing bytes=%u crc=%08x\n",
             (unsigned)bytes, (unsigned)crc);
    reply(req, "200 OK", body);
    ESP_LOGI(TAG, "existing verify bytes=%u crc=%08x", (unsigned)bytes, (unsigned)crc);
    ESP_LOGI(TAG, "httpd stack_free=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    request_stop();
    return ESP_OK;
}

esp_err_t panel_music_import_start(const char *token)
{
    if (!panel_music_import_token_valid(token)) return ESP_ERR_INVALID_ARG;
    panel_wifi_snapshot_t wifi;
    panel_network_wifi_get(&wifi);
    if (!wifi.connected || wifi.ip[0] == '\0') return ESP_ERR_INVALID_STATE;

    panel_music_stop();
    for (int i = 0; i < 50 && panel_music_is_active(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (panel_music_is_active()) return ESP_ERR_INVALID_STATE;

    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state != IMPORT_IDLE) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = IMPORT_STARTING;
    s_transfer_active = false;
    s_stop_requested = false;
    s_generation++;
    memset(s_token, 0, sizeof(s_token));
    memcpy(s_token, token, strlen(token));
    xSemaphoreGive(s_mutex);

    if (s_control == NULL) {
        if (xTaskCreate(control_task, "music_import_ctl", 4096, NULL, 3, &s_control) != pdPASS) {
            s_control = NULL;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_state = IMPORT_IDLE;
            memset(s_token, 0, sizeof(s_token));
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = IMPORT_PORT;
    config.max_uri_handlers = 2;
    config.stack_size = 20480;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    httpd_uri_t uri = {
        .uri = "/music/import",
        .method = HTTP_POST,
        .handler = import_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t info_uri = {
        .uri = "/music/info",
        .method = HTTP_GET,
        .handler = info_handler,
        .user_ctx = NULL,
    };
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = IMPORT_IDLE;
        memset(s_token, 0, sizeof(s_token));
        xSemaphoreGive(s_mutex);
        return err;
    }
    err = httpd_register_uri_handler(server, &uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &info_uri);
    if (err != ESP_OK) {
        httpd_stop(server);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = IMPORT_IDLE;
        memset(s_token, 0, sizeof(s_token));
        xSemaphoreGive(s_mutex);
        return err;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == IMPORT_STOPPING) {
        xSemaphoreGive(s_mutex);
        httpd_stop(server);
        return ESP_ERR_INVALID_STATE;
    }
    s_server = server;
    s_state = IMPORT_ACTIVE;
    TaskHandle_t control = s_control;
    xSemaphoreGive(s_mutex);
    if (control != NULL) xTaskNotifyGive(control);
    ESP_LOGI(TAG, "LAN import armed at http://%s:%d/music/import (one shot)", wifi.ip,
             IMPORT_PORT);
    return ESP_OK;
}

void panel_music_import_stop(void)
{
    request_stop();
}

bool panel_music_import_is_active(void)
{
    return import_active();
}
