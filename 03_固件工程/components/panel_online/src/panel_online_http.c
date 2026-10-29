#include "panel_online_http.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "panel_online_http";

typedef struct {
    char content_type[64];
} http_headers_t;

/*
 * The SDK's esp_http_client_read()/fetch_headers() internally loop over
 * esp_transport_read() with the same client timeout for every packet.  Setting
 * timeout_ms before the call is not a total deadline.  A small persistent
 * watchdog holds only the public socket fd (not the client) and calls
 * shutdown() at the monotonic deadline.  The requester joins the watchdog
 * (idle handshake) before close/cleanup, so no timer/task touches a freed
 * client or a reused fd.
 */
static SemaphoreHandle_t s_wd_lock;
static SemaphoreHandle_t s_wd_wake;
static TaskHandle_t s_wd_task;
static bool s_wd_started;
static int s_wd_fd;
static int64_t s_wd_deadline_ms;
static volatile bool s_wd_active;
static bool s_wd_idle = true;
static bool s_wd_fired;

static void http_watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        TickType_t wait = s_wd_active ? pdMS_TO_TICKS(20) : portMAX_DELAY;
        (void)xSemaphoreTake(s_wd_wake, wait);
        if (s_wd_lock == NULL || xSemaphoreTake(s_wd_lock, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int64_t now = (int64_t)(esp_timer_get_time() / 1000);
        if (s_wd_active && now >= s_wd_deadline_ms) {
            int fd = s_wd_fd;
            s_wd_active = false;
            s_wd_fired = true;
            /* shutdown() is non-blocking; keep the lock so cleanup cannot
             * close/reuse the fd until this call has returned. */
            if (fd >= 0) {
                (void)shutdown(fd, SHUT_RDWR);
            }
            s_wd_idle = true;
        } else if (!s_wd_active) {
            s_wd_idle = true;
        }
        xSemaphoreGive(s_wd_lock);
    }
}

static bool http_watchdog_arm(int fd, int64_t deadline_ms)
{
    if (!s_wd_started) {
        if (s_wd_lock == NULL) {
            s_wd_lock = xSemaphoreCreateMutex();
            s_wd_wake = xSemaphoreCreateBinary();
        }
        if (s_wd_lock == NULL || s_wd_wake == NULL) {
            return false;
        }
        if (xTaskCreate(http_watchdog_task, "online_http_wd", 3072, NULL, 5,
                        &s_wd_task) != pdPASS) {
            return false;
        }
        s_wd_started = true;
    }
    if (xSemaphoreTake(s_wd_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    s_wd_fd = fd;
    s_wd_deadline_ms = deadline_ms;
    s_wd_fired = false;
    s_wd_idle = false;
    s_wd_active = true;
    xSemaphoreGive(s_wd_lock);
    (void)xSemaphoreGive(s_wd_wake);
    return true;
}

static bool http_watchdog_disarm(void)
{
    if (!s_wd_started || s_wd_lock == NULL) {
        return true;
    }
    /* The watchdog holds this same mutex through shutdown().  Acquiring it is
     * a real join: on return no shutdown(fd) can still be in flight. */
    if (xSemaphoreTake(s_wd_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    s_wd_active = false;
    s_wd_fd = -1;
    s_wd_idle = true;
    xSemaphoreGive(s_wd_lock);
    (void)xSemaphoreGive(s_wd_wake);
    return true;
}

static bool http_watchdog_fired(void)
{
    bool fired = false;
    if (s_wd_started && s_wd_lock != NULL &&
        xSemaphoreTake(s_wd_lock, portMAX_DELAY) == pdTRUE) {
        fired = s_wd_fired;
        xSemaphoreGive(s_wd_lock);
    }
    return fired;
}

static void copy_header_value(char *dst, size_t cap, const char *value)
{
    if (dst == NULL || cap == 0 || value == NULL) {
        return;
    }
    size_t n = strlen(value);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, value, n);
    dst[n] = '\0';
}

/* The IDF HTTP parser ignores event-handler return codes, so this callback is
 * used only to capture headers.  Body cancellation is done by explicit
 * open/fetch/read below, never by returning ESP_FAIL here. */
static esp_err_t http_event_cb(esp_http_client_event_t *event)
{
    http_headers_t *headers = (http_headers_t *)event->user_data;
    if (headers != NULL && event->event_id == HTTP_EVENT_ON_HEADER &&
        event->header_key != NULL && event->header_value != NULL &&
        strcasecmp(event->header_key, "Content-Type") == 0) {
        copy_header_value(headers->content_type, sizeof(headers->content_type),
                          event->header_value);
    }
    return ESP_OK;
}

static int64_t remaining_ms(int64_t deadline_ms)
{
    int64_t now = (int64_t)(esp_timer_get_time() / 1000);
    return deadline_ms - now;
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

esp_err_t panel_online_http_get(const char *url, char *body, size_t cap, size_t *len,
                                int *status, char *content_type, size_t content_type_cap)
{
    if (url == NULL || body == NULL || cap < 2 || len == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *len = 0;
    *status = 0;
    body[0] = '\0';
    if (content_type != NULL && content_type_cap > 0) {
        content_type[0] = '\0';
    }

    http_headers_t headers = {0};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_cb,
        .user_data = &headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = PANEL_ONLINE_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
        .skip_cert_common_name_check = false,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    int64_t deadline_ms = (int64_t)(esp_timer_get_time() / 1000) +
                          PANEL_ONLINE_HTTP_TIMEOUT_MS;
    esp_err_t err = ESP_OK;
    bool opened = false;
    bool watchdog_armed = false;
    bool closed = false;
    if (esp_http_client_set_timeout_ms(client, PANEL_ONLINE_HTTP_TIMEOUT_MS) != ESP_OK) {
        err = ESP_FAIL;
        goto cleanup;
    }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    opened = true;

    int fd = esp_http_client_get_socket(client);
    if (fd < 0 || !http_watchdog_arm(fd, deadline_ms)) {
        err = ESP_FAIL;
        goto cleanup;
    }
    watchdog_armed = true;

    int64_t remaining = remaining_ms(deadline_ms);
    if (remaining <= 0) {
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
    if (esp_http_client_set_timeout_ms(client, (int)(remaining > INT_MAX ? INT_MAX : remaining)) !=
        ESP_OK) {
        err = ESP_FAIL;
        goto cleanup;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);
    *status = http_status;
    if (http_watchdog_fired()) {
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
    if (content_length == -ESP_ERR_HTTP_EAGAIN) {
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
    if (content_length < 0 && !esp_http_client_is_chunked_response(client)) {
        err = ESP_FAIL;
        goto cleanup;
    }
    if (http_status != 200) {
        /* Nothing in the body is consumed for an invalid status. */
        goto cleanup;
    }
    if (content_length >= 0 && (uint64_t)content_length >= (uint64_t)cap) {
        ESP_LOGW(TAG, "Content-Length %d exceeds cap %u",
                 (int)content_length, (unsigned)cap);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    if (content_type != NULL && content_type_cap > 0) {
        copy_header_value(content_type, content_type_cap, headers.content_type);
    }
    if (!json_content_type(headers.content_type)) {
        ESP_LOGW(TAG, "unexpected Content-Type: %s",
                 headers.content_type[0] ? headers.content_type : "(missing)");
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    size_t used = 0;
    for (;;) {
        remaining = remaining_ms(deadline_ms);
        if (remaining <= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (esp_http_client_set_timeout_ms(
                client, (int)(remaining > INT_MAX ? INT_MAX : remaining)) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        size_t space = cap - used;
        if (space <= 1) {
            /* No room for another byte plus the terminator: read one byte to
             * distinguish completion from overflow. */
            char probe = 0;
            int got = esp_http_client_read(client, &probe, 1);
            if (http_watchdog_fired()) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            if (got > 0) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (got < 0) {
                err = got == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
                break;
            }
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            continue;
        }
        int want = space - 1 > 2048 ? 2048 : (int)(space - 1);
        int got = esp_http_client_read(client, body + used, want);
        if (http_watchdog_fired()) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (got > 0) {
            used += (size_t)got;
            continue;
        }
        if (got < 0) {
            err = got == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
            break;
        }
        if (esp_http_client_is_complete_data_received(client)) {
            break;
        }
        /* A zero-length read that is not EOF should not busy-loop; the next
         * iteration checks the monotonic deadline again. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err == ESP_OK) {
        body[used] = '\0';
        *len = used;
    }

cleanup:
    if (watchdog_armed) {
        if (!http_watchdog_disarm()) {
            /* Safety over leak: do not close/cleanup while a captured fd may
             * still be inside shutdown(). */
            ESP_LOGE(TAG, "watchdog join failed; leaking client to avoid fd reuse");
            *len = 0;
            body[0] = '\0';
            return ESP_FAIL;
        }
        if (http_watchdog_fired()) {
            err = ESP_ERR_TIMEOUT;
        }
    }
    if (opened && !closed) {
        (void)esp_http_client_close(client);
        closed = true;
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        *len = 0;
        body[0] = '\0';
    }
    return err;
}
