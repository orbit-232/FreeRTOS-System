#include "fake_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct esp_http_client {
    esp_http_client_config_t config;
    bool opened;
    bool complete;
    bool chunked;
    size_t bytes_read;
    int last_timeout_ms;
};

static int g_status;
static char g_content_type[64];
static int64_t g_content_length;
static char g_body[32768];
static size_t g_body_len;
static size_t g_chunk_size = 1024;
static unsigned g_chunk_delay_ms;
static bool g_endless;
static esp_err_t g_open_error;
static int64_t g_fetch_error;
static int g_read_error_after;
static bool g_read_eagain;
static bool g_read_block;
static bool g_fetch_block;
static HANDLE g_shutdown_event;
static volatile LONG g_shutdown_calls;
static volatile LONG g_internal_packets;
static volatile LONG g_shutdown_in_progress;
static bool g_shutdown_completed;
static bool g_close_before_shutdown_complete;
static unsigned g_shutdown_delay_ms;
static int g_socket_fd;
static bool g_cleanup_after_shutdown;
static int g_open_calls;
static int g_close_calls;
static int g_cleanup_calls;
static int g_read_calls;
static int g_last_timeout_ms;

void fake_http_reset(void)
{
    g_status = 200;
    snprintf(g_content_type, sizeof(g_content_type), "application/json");
    g_content_length = -1;
    g_body[0] = '\0';
    g_body_len = 0;
    g_chunk_size = 1024;
    g_chunk_delay_ms = 0;
    g_endless = false;
    g_open_error = ESP_OK;
    g_fetch_error = 0;
    g_read_error_after = -1;
    g_read_eagain = false;
    g_read_block = false;
    g_fetch_block = false;
    g_shutdown_calls = 0;
    g_internal_packets = 0;
    g_shutdown_in_progress = 0;
    g_shutdown_completed = false;
    g_close_before_shutdown_complete = false;
    g_shutdown_delay_ms = 0;
    g_socket_fd = 4242;
    g_cleanup_after_shutdown = false;
    if (g_shutdown_event == NULL) {
        g_shutdown_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    } else {
        ResetEvent(g_shutdown_event);
    }
    g_open_calls = 0;
    g_close_calls = 0;
    g_cleanup_calls = 0;
    g_read_calls = 0;
    g_last_timeout_ms = 0;
}

void fake_http_set_status(int status) { g_status = status; }
void fake_http_set_content_type(const char *content_type)
{
    snprintf(g_content_type, sizeof(g_content_type), "%s",
             content_type != NULL ? content_type : "");
}
void fake_http_set_content_length(int64_t length) { g_content_length = length; }
void fake_http_set_body(const char *body, size_t len)
{
    if (body == NULL || len >= sizeof(g_body)) {
        g_body_len = 0;
        return;
    }
    memcpy(g_body, body, len);
    g_body[len] = '\0';
    g_body_len = len;
    if (g_content_length < 0) {
        g_content_length = (int64_t)len;
    }
}
void fake_http_set_chunk_size(size_t chunk_size)
{
    g_chunk_size = chunk_size > 0 ? chunk_size : 1;
}
void fake_http_set_chunk_delay_ms(unsigned delay_ms) { g_chunk_delay_ms = delay_ms; }
void fake_http_set_endless(bool endless) { g_endless = endless; }
void fake_http_set_open_error(esp_err_t error) { g_open_error = error; }
void fake_http_set_fetch_error(int64_t error) { g_fetch_error = error; }
void fake_http_set_read_error_after(int bytes) { g_read_error_after = bytes; }
void fake_http_set_read_eagain(bool eagain) { g_read_eagain = eagain; }
void fake_http_set_read_block(bool block)
{
    g_read_block = block;
    if (g_shutdown_event == NULL) {
        g_shutdown_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    }
    ResetEvent(g_shutdown_event);
}

void fake_http_set_fetch_block(bool block)
{
    g_fetch_block = block;
    if (g_shutdown_event == NULL) {
        g_shutdown_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    }
    ResetEvent(g_shutdown_event);
}

int fake_http_open_calls(void) { return g_open_calls; }
int fake_http_close_calls(void) { return g_close_calls; }
int fake_http_cleanup_calls(void) { return g_cleanup_calls; }
int fake_http_read_calls(void) { return g_read_calls; }
int fake_http_shutdown_calls(void) { return (int)g_shutdown_calls; }
bool fake_http_cleanup_after_shutdown(void) { return g_cleanup_after_shutdown; }
bool fake_http_close_before_shutdown_complete(void)
{
    return g_close_before_shutdown_complete;
}
int fake_http_internal_packets(void) { return (int)g_internal_packets; }
void fake_http_set_shutdown_delay_ms(unsigned delay_ms) { g_shutdown_delay_ms = delay_ms; }
void fake_http_set_socket(int socket_fd) { g_socket_fd = socket_fd; }
int fake_http_last_timeout_ms(void) { return g_last_timeout_ms; }

int fake_http_shutdown(int socket, int how)
{
    (void)socket;
    (void)how;
    InterlockedIncrement(&g_shutdown_calls);
    InterlockedExchange(&g_shutdown_in_progress, 1);
    /* Wake the blocked read first, then keep shutdown "in progress" longer
     * than the old 200 ms disarm poll.  A correct join must wait for this. */
    if (g_shutdown_event != NULL) {
        SetEvent(g_shutdown_event);
    }
    if (g_shutdown_delay_ms > 0) {
        Sleep(g_shutdown_delay_ms);
    }
    g_shutdown_completed = true;
    InterlockedExchange(&g_shutdown_in_progress, 0);
    return 0;
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    if (config == NULL) {
        return NULL;
    }
    struct esp_http_client *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return NULL;
    }
    client->config = *config;
    client->last_timeout_ms = config->timeout_ms;
    return client;
}

esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms)
{
    if (client == NULL) return ESP_ERR_INVALID_ARG;
    client->last_timeout_ms = timeout_ms;
    g_last_timeout_ms = timeout_ms;
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{
    (void)write_len;
    if (client == NULL) return ESP_ERR_INVALID_ARG;
    g_open_calls++;
    if (g_open_error != ESP_OK) return g_open_error;
    client->opened = true;
    client->complete = false;
    client->chunked = g_content_length < 0;
    client->bytes_read = 0;
    return ESP_OK;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    if (client == NULL) return ESP_FAIL;
    if (g_fetch_block) {
        for (;;) {
            if (g_shutdown_event != NULL &&
                WaitForSingleObject(g_shutdown_event, 10) == WAIT_OBJECT_0) {
                return ESP_FAIL;
            }
            InterlockedIncrement(&g_internal_packets);
        }
    }
    if (g_fetch_error != 0) return g_fetch_error;
    if (client->config.event_handler != NULL) {
        esp_http_client_event_t event;
        memset(&event, 0, sizeof(event));
        event.event_id = HTTP_EVENT_ON_HEADER;
        event.client = client;
        event.user_data = client->config.user_data;
        event.header_key = "Content-Type";
        event.header_value = g_content_type;
        (void)client->config.event_handler(&event);
    }
    return client->chunked ? 0 : g_content_length;
}

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len)
{
    if (client == NULL || buffer == NULL || len <= 0) return ESP_ERR_INVALID_ARG;
    g_read_calls++;
    if (g_read_block) {
        /* Emulate the SDK's internal read loop: many packet/timeout iterations
         * inside one esp_http_client_read call, none returning to the caller. */
        for (;;) {
            if (g_shutdown_event != NULL &&
                WaitForSingleObject(g_shutdown_event, 10) == WAIT_OBJECT_0) {
                return ESP_FAIL;
            }
            InterlockedIncrement(&g_internal_packets);
        }
    }
    if (g_read_eagain) {
        Sleep(10);
        return -ESP_ERR_HTTP_EAGAIN;
    }
    if (g_read_error_after >= 0 && (int)client->bytes_read >= g_read_error_after) {
        return ESP_FAIL;
    }
    if (!g_endless && client->bytes_read >= g_body_len) {
        client->complete = true;
        return 0;
    }
    if (g_chunk_delay_ms > 0) {
        Sleep(g_chunk_delay_ms);
    }
    size_t want = (size_t)len;
    if (want > g_chunk_size) want = g_chunk_size;
    size_t available = g_endless ? want : g_body_len - client->bytes_read;
    if (want > available) want = available;
    if (want == 0) {
        client->complete = true;
        return 0;
    }
    memcpy(buffer, g_body + client->bytes_read, want);
    client->bytes_read += want;
    if (!g_endless && client->bytes_read >= g_body_len) {
        client->complete = true;
    }
    return (int)want;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    (void)client;
    return g_status;
}

int64_t esp_http_client_get_content_length(esp_http_client_handle_t client)
{
    (void)client;
    return g_content_length;
}

bool esp_http_client_is_chunked_response(esp_http_client_handle_t client)
{
    return client != NULL && client->chunked;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{
    return client != NULL && client->complete;
}

int esp_http_client_get_socket(esp_http_client_handle_t client)
{
    return client != NULL && client->opened ? g_socket_fd : -1;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
    if (client == NULL) return ESP_ERR_INVALID_ARG;
    if (g_shutdown_in_progress || (g_shutdown_calls > 0 && !g_shutdown_completed)) {
        g_close_before_shutdown_complete = true;
    }
    g_close_calls++;
    client->opened = false;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    if (client == NULL) return ESP_ERR_INVALID_ARG;
    if (g_shutdown_in_progress || (g_shutdown_calls > 0 && !g_shutdown_completed)) {
        g_close_before_shutdown_complete = true;
    }
    g_cleanup_calls++;
    g_cleanup_after_shutdown = g_shutdown_completed;
    free(client);
    return ESP_OK;
}
