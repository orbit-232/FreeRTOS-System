#include "panel_network.h"

#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "panel_network";

#define WIFI_NVS_NAMESPACE "network"
#define WIFI_NVS_KEY "wifi"
#define WIFI_CRED_MAGIC 0x57494649UL
#define WIFI_CRED_VERSION 2
#define WIFI_MAX_RETRY 3
#define WIFI_QUEUE_LEN 32
#define WIFI_CONNECT_TIMEOUT_MS 15000U
#define WIFI_DISCONNECT_TIMEOUT_MS 2000U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t auto_connect;
    uint8_t authmode;
    uint8_t ssid_len;
    uint8_t pass_len;
    char ssid[PANEL_WIFI_SSID_MAX + 1];
    char password[PANEL_WIFI_PASS_MAX + 1];
} wifi_creds_t;

/* Version 1 written by the previous phase-A candidate; migrate without ever
 * trusting an unbounded string. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t auto_connect;
    char ssid[PANEL_WIFI_SSID_MAX + 1];
    char password[PANEL_WIFI_PASS_MAX + 1];
} wifi_creds_v1_t;

typedef enum {
    WIFI_CMD_ENABLE,
    WIFI_CMD_DISABLE,
    WIFI_CMD_SCAN,
    WIFI_CMD_CONNECT,
    WIFI_CMD_DISCONNECT,
    WIFI_CMD_FORGET,
    WIFI_EVT_STARTED,
    WIFI_EVT_SCAN_DONE,
    WIFI_EVT_DISCONNECTED,
    WIFI_EVT_GOT_IP,
} wifi_evt_type_t;

typedef struct {
    wifi_evt_type_t type;
    int reason;
    char ip[16];
    bool remember;
    uint8_t authmode;
    char ssid[PANEL_WIFI_SSID_MAX + 1];
    char password[PANEL_WIFI_PASS_MAX + 1];
} wifi_evt_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static panel_wifi_snapshot_t s_published;
static wifi_creds_t s_confirmed;
static bool s_has_confirmed;

/* Worker-owned state. No other task reads or writes these fields. */
static panel_wifi_snapshot_t s_wifi;
static bool s_inited;
static bool s_started;
static bool s_connecting;
static bool s_wait_disconnect;
static bool s_sta_connected;
static bool s_manual_stop;
static bool s_has_pending;
static bool s_user_attempt;
static volatile bool s_event_lost;
static uint32_t s_retry;
static uint32_t s_connect_deadline_ms;
static uint32_t s_disconnect_deadline_ms;
static uint32_t s_scan_token;
static wifi_creds_t s_pending;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void publish(void)
{
    lock();
    s_published = s_wifi;
    unlock();
}

static void set_reason(const char *text)
{
    snprintf(s_wifi.reason, sizeof(s_wifi.reason), "%s", text ? text : "");
}

static esp_err_t queue_send(const wifi_evt_t *evt)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_queue, evt, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    wifi_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        evt.type = WIFI_EVT_STARTED;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        evt.type = WIFI_EVT_SCAN_DONE;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        evt.type = WIFI_EVT_DISCONNECTED;
        evt.reason = data ? ((wifi_event_sta_disconnected_t *)data)->reason : 0;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        evt.type = WIFI_EVT_GOT_IP;
        ip_event_got_ip_t *ip = data;
        if (ip != NULL) {
            snprintf(evt.ip, sizeof(evt.ip), IPSTR, IP2STR(&ip->ip_info.ip));
        }
    } else {
        return;
    }
    if (queue_send(&evt) != ESP_OK) {
        s_event_lost = true;
        ESP_LOGE(TAG, "wifi event queue full, dropped type=%d", (int)evt.type);
    }
}

static bool creds_valid(const wifi_creds_t *creds)
{
    if (creds == NULL || creds->magic != WIFI_CRED_MAGIC ||
        creds->version != WIFI_CRED_VERSION ||
        creds->ssid_len == 0 || creds->ssid_len > PANEL_WIFI_SSID_MAX ||
        creds->pass_len > PANEL_WIFI_PASS_MAX) {
        return false;
    }
    if (creds->ssid[creds->ssid_len] != '\0' ||
        creds->password[creds->pass_len] != '\0') {
        return false;
    }
    return panel_wifi_credentials_valid(creds->ssid, creds->password, creds->authmode,
                                        NULL, 0);
}

static bool load_creds(void)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    wifi_creds_t creds;
    memset(&creds, 0, sizeof(creds));
    size_t len = sizeof(creds);
    esp_err_t err = nvs_get_blob(handle, WIFI_NVS_KEY, &creds, &len);
    nvs_close(handle);
    if (err == ESP_OK && len == sizeof(creds)) {
        if (!creds_valid(&creds)) {
            ESP_LOGW(TAG, "ignored invalid wifi credentials");
            return false;
        }
        s_confirmed = creds;
        s_has_confirmed = true;
        return true;
    }
    if (err == ESP_OK && len == sizeof(wifi_creds_v1_t)) {
        wifi_creds_v1_t old;
        memcpy(&old, &creds, sizeof(old));
        const char *ssid_end = memchr(old.ssid, '\0', sizeof(old.ssid));
        const char *pass_end = memchr(old.password, '\0', sizeof(old.password));
        if (old.magic != WIFI_CRED_MAGIC || old.version != 1 || ssid_end == NULL ||
            pass_end == NULL || ssid_end == old.ssid ||
            (size_t)(pass_end - old.password) > PANEL_WIFI_PASS_MAX) {
            ESP_LOGW(TAG, "ignored invalid v1 wifi credentials");
            return false;
        }
        size_t ssid_len = (size_t)(ssid_end - old.ssid);
        size_t pass_len = (size_t)(pass_end - old.password);
        uint8_t authmode = pass_len ? 3 : 0;
        if (ssid_len > PANEL_WIFI_SSID_MAX ||
            !panel_wifi_credentials_valid(old.ssid, old.password, authmode, NULL, 0)) {
            ESP_LOGW(TAG, "ignored invalid v1 wifi credentials values");
            return false;
        }
        memset(&s_confirmed, 0, sizeof(s_confirmed));
        s_confirmed.magic = WIFI_CRED_MAGIC;
        s_confirmed.version = WIFI_CRED_VERSION;
        s_confirmed.auto_connect = old.auto_connect;
        s_confirmed.authmode = authmode;
        s_confirmed.ssid_len = (uint8_t)ssid_len;
        s_confirmed.pass_len = (uint8_t)pass_len;
        memcpy(s_confirmed.ssid, old.ssid, ssid_len);
        memcpy(s_confirmed.password, old.password, pass_len);
        s_has_confirmed = true;
        return true;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "ignored unreadable wifi credentials: %s", esp_err_to_name(err));
    }
    return false;
}

static bool save_creds(const wifi_creds_t *creds)
{
    if (creds == NULL || !creds_valid(creds)) {
        return false;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi creds nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, WIFI_NVS_KEY, creds, sizeof(*creds));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi creds save failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void erase_creds(void)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_erase_key(handle, WIFI_NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

static esp_err_t start_stack(void)
{
    if (s_inited) {
        if (s_started) {
            return ESP_OK;
        }
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            return err;
        }
        s_started = true;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        return loop;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi ram storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL),
                        TAG, "wifi event");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL),
                        TAG, "ip event");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    s_inited = true;
    s_started = true;
    return ESP_OK;
}

static void fail_state(const char *reason)
{
    s_connecting = false;
    s_wait_disconnect = false;
    s_sta_connected = false;
    s_user_attempt = false;
    s_connect_deadline_ms = 0;
    s_disconnect_deadline_ms = 0;
    s_wifi.state = PANEL_WIFI_FAILED;
    s_wifi.connected = false;
    s_wifi.ip[0] = '\0';
    set_reason(reason);
}

static esp_err_t apply_connect(void)
{
    if (!s_has_pending) {
        fail_state("没有待连接网络");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = start_stack();
    if (err != ESP_OK) {
        fail_state("Wi-Fi 启动失败");
        return err;
    }
    wifi_config_t config;
    memset(&config, 0, sizeof(config));
    size_t ssid_len = strlen(s_pending.ssid);
    size_t pass_len = strlen(s_pending.password);
    if (ssid_len > sizeof(config.sta.ssid) || pass_len > sizeof(config.sta.password)) {
        fail_state("凭据长度无效");
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(config.sta.ssid, s_pending.ssid, ssid_len);
    memcpy(config.sta.password, s_pending.password, pass_len);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        fail_state("Wi-Fi 配置失败");
        return err;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        fail_state("Wi-Fi 连接启动失败");
        return err;
    }
    s_connecting = true;
    s_wait_disconnect = false;
    s_manual_stop = false;
    if (s_connect_deadline_ms == 0) {
        s_connect_deadline_ms = now_ms() + WIFI_CONNECT_TIMEOUT_MS;
    }
    s_wifi.state = PANEL_WIFI_CONNECTING;
    s_wifi.connected = false;
    s_wifi.ip[0] = '\0';
    snprintf(s_wifi.ssid, sizeof(s_wifi.ssid), "%s", s_pending.ssid);
    set_reason("");
    return ESP_OK;
}

static esp_err_t begin_connect(const wifi_creds_t *creds, bool user_attempt)
{
    if (creds == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pending = *creds;
    s_has_pending = true;
    s_user_attempt = user_attempt;
    if (user_attempt) {
        s_retry = 0;
        s_connect_deadline_ms = now_ms() + WIFI_CONNECT_TIMEOUT_MS;
    } else if (s_connect_deadline_ms == 0) {
        s_connect_deadline_ms = now_ms() + WIFI_CONNECT_TIMEOUT_MS;
    }
    s_manual_stop = false;
    s_wifi.state = PANEL_WIFI_CONNECTING;
    s_wifi.connected = false;
    s_wifi.ip[0] = '\0';
    if (s_wifi.scanning) {
        esp_wifi_scan_stop();
        s_wifi.scanning = false;
        s_scan_token++;
    }
    if (s_sta_connected || s_connecting) {
        s_connecting = false;
        s_wait_disconnect = true;
        s_disconnect_deadline_ms = now_ms() + WIFI_DISCONNECT_TIMEOUT_MS;
        if (s_started) {
            esp_wifi_disconnect();
        }
        return ESP_OK;
    }
    return apply_connect();
}

static void scan_done(void)
{
    if (!s_wifi.scanning) {
        return;
    }
    uint16_t count = PANEL_NET_MAX_APS;
    wifi_ap_record_t records[PANEL_NET_MAX_APS];
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < count && i < PANEL_NET_MAX_APS; i++) {
            panel_wifi_ap_t ap;
            memset(&ap, 0, sizeof(ap));
            snprintf(ap.ssid, sizeof(ap.ssid), "%s", (const char *)records[i].ssid);
            ap.rssi = records[i].rssi;
            ap.authmode = records[i].authmode;
            ap.channel = records[i].primary;
            memcpy(ap.bssid, records[i].bssid, 6);
            panel_wifi_scan_add(&s_wifi, s_scan_token, &ap);
        }
        panel_wifi_scan_finish(&s_wifi, s_scan_token);
        s_wifi.state = s_wifi.connected ? PANEL_WIFI_CONNECTED : PANEL_WIFI_IDLE;
    } else {
        s_wifi.scanning = false;
        s_wifi.state = s_wifi.connected ? PANEL_WIFI_CONNECTED : PANEL_WIFI_FAILED;
        set_reason("扫描结果读取失败");
    }
    publish();
}

static void handle_disconnected(int reason)
{
    s_sta_connected = false;
    s_wifi.connected = false;
    s_wifi.ip[0] = '\0';
    if (s_wifi.state == PANEL_WIFI_OFF || s_wifi.state == PANEL_WIFI_UNAVAILABLE) {
        publish();
        return;
    }
    if (s_wait_disconnect) {
        s_wait_disconnect = false;
        s_disconnect_deadline_ms = 0;
        if (s_has_pending) {
            apply_connect();
        }
        publish();
        return;
    }
    s_connecting = false;
    s_connect_deadline_ms = 0;
    if (s_manual_stop) {
        s_wifi.state = PANEL_WIFI_IDLE;
        set_reason("已断开");
        publish();
        return;
    }
    if (!s_user_attempt && s_has_confirmed && s_confirmed.auto_connect &&
        s_retry < WIFI_MAX_RETRY) {
        uint32_t retry = s_retry + 1;
        s_retry = retry;
        begin_connect(&s_confirmed, false);
        s_retry = retry;
        publish();
        return;
    }
    s_user_attempt = false;
    s_wifi.state = PANEL_WIFI_FAILED;
    char text[64];
    snprintf(text, sizeof(text), "断开原因 %d", reason);
    set_reason(text);
    publish();
}

static void handle_got_ip(const char *ip)
{
    if (!s_connecting || s_wifi.state != PANEL_WIFI_CONNECTING) {
        return;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
        strcmp((const char *)ap.ssid, s_pending.ssid) != 0) {
        fail_state("连接 AP 与请求不一致");
        if (s_started) esp_wifi_disconnect();
        publish();
        return;
    }
    s_connecting = false;
    s_wait_disconnect = false;
    s_sta_connected = true;
    s_user_attempt = false;
    s_retry = 0;
    s_connect_deadline_ms = 0;
    s_wifi.scanning = false;
    s_wifi.connected = true;
    s_wifi.state = PANEL_WIFI_CONNECTED;
    snprintf(s_wifi.ip, sizeof(s_wifi.ip), "%s", ip ? ip : "");
    snprintf(s_wifi.ssid, sizeof(s_wifi.ssid), "%s", s_pending.ssid);
    set_reason("");
    if (s_has_pending && s_pending.auto_connect) {
        if (!save_creds(&s_pending)) {
            set_reason("已连接；凭据保存失败");
        } else {
            s_confirmed = s_pending;
            s_has_confirmed = true;
        }
    }
    if (s_has_pending) {
        s_confirmed = s_pending;
        s_has_confirmed = true;
        s_wifi.auto_reconnect = s_pending.auto_connect;
        memset(&s_pending, 0, sizeof(s_pending));
        s_has_pending = false;
    }
    publish();
}

static void handle_disable(void)
{
    s_wifi.enabled = false;
    s_wifi.state = PANEL_WIFI_OFF;
    s_wifi.scanning = false;
    s_wifi.connected = false;
    s_wifi.ap_count = 0;
    s_wifi.ip[0] = '\0';
    set_reason("");
    s_connecting = false;
    s_wait_disconnect = false;
    s_sta_connected = false;
    s_manual_stop = false;
    s_user_attempt = false;
    s_has_pending = false;
    s_retry = 0;
    s_connect_deadline_ms = 0;
    s_disconnect_deadline_ms = 0;
    memset(&s_pending, 0, sizeof(s_pending));
    s_scan_token++;
    if (s_started) {
        esp_wifi_stop();
        s_started = false;
    }
    publish();
}

static void handle_enable(void)
{
    if (s_wifi.enabled && s_wifi.state != PANEL_WIFI_OFF) {
        return;
    }
    esp_err_t err = start_stack();
    if (err != ESP_OK) {
        s_wifi.enabled = false;
        s_wifi.state = PANEL_WIFI_UNAVAILABLE;
        set_reason("Wi-Fi Hosted 初始化失败");
        publish();
        return;
    }
    s_wifi.enabled = true;
    s_wifi.state = PANEL_WIFI_IDLE;
    s_manual_stop = false;
    s_user_attempt = false;
    s_retry = 0;
    set_reason("");
    if (s_has_confirmed && s_confirmed.auto_connect) {
        begin_connect(&s_confirmed, false);
    }
    publish();
}

static void handle_scan(void)
{
    if (!s_wifi.enabled) {
        set_reason("Wi-Fi 未开启");
        publish();
        return;
    }
    if (s_wifi.scanning) {
        return;
    }
    if (s_connecting || s_wait_disconnect) {
        set_reason("连接中，稍后再扫描");
        publish();
        return;
    }
    if (start_stack() != ESP_OK) {
        s_wifi.state = PANEL_WIFI_UNAVAILABLE;
        set_reason("Wi-Fi 启动失败");
        publish();
        return;
    }
    if (!panel_wifi_scan_begin(&s_wifi)) {
        return;
    }
    s_scan_token = s_wifi.scan_generation;
    wifi_scan_config_t scan;
    memset(&scan, 0, sizeof(scan));
    esp_err_t err = esp_wifi_scan_start(&scan, false);
    if (err != ESP_OK) {
        s_wifi.scanning = false;
        s_wifi.state = s_wifi.connected ? PANEL_WIFI_CONNECTED : PANEL_WIFI_FAILED;
        set_reason("扫描启动失败");
    }
    publish();
}

static void handle_connect(const wifi_evt_t *evt)
{
    wifi_creds_t creds;
    memset(&creds, 0, sizeof(creds));
    creds.magic = WIFI_CRED_MAGIC;
    creds.version = WIFI_CRED_VERSION;
    creds.auto_connect = evt->remember ? 1 : 0;
    creds.authmode = evt->authmode;
    creds.ssid_len = (uint8_t)strlen(evt->ssid);
    creds.pass_len = (uint8_t)strlen(evt->password);
    snprintf(creds.ssid, sizeof(creds.ssid), "%s", evt->ssid);
    snprintf(creds.password, sizeof(creds.password), "%s", evt->password);
    begin_connect(&creds, true);
    memset(&creds, 0, sizeof(creds));
    publish();
}

static void handle_disconnect(bool forget)
{
    s_wifi.connected = false;
    s_wifi.ip[0] = '\0';
    s_wifi.state = PANEL_WIFI_IDLE;
    s_connecting = false;
    s_wait_disconnect = false;
    s_sta_connected = false;
    s_manual_stop = true;
    s_user_attempt = false;
    s_has_pending = false;
    s_retry = 0;
    s_connect_deadline_ms = 0;
    s_disconnect_deadline_ms = 0;
    memset(&s_pending, 0, sizeof(s_pending));
    if (s_started) {
        esp_wifi_disconnect();
    }
    if (forget) {
        erase_creds();
        memset(&s_confirmed, 0, sizeof(s_confirmed));
        s_has_confirmed = false;
        s_wifi.auto_reconnect = false;
        set_reason("已忘记网络");
    } else {
        set_reason("已断开");
    }
    publish();
}

static void handle_started(void)
{
    if (!s_wifi.enabled || s_connecting || s_wait_disconnect || s_sta_connected ||
        s_wifi.connected || s_manual_stop) {
        return;
    }
    if (s_has_confirmed && s_confirmed.auto_connect) {
        begin_connect(&s_confirmed, false);
        publish();
    }
}

static void check_deadlines(void)
{
    uint32_t now = now_ms();
    if (s_disconnect_deadline_ms != 0 &&
        (int32_t)(now - s_disconnect_deadline_ms) >= 0) {
        s_disconnect_deadline_ms = 0;
        if (s_has_pending) {
            apply_connect();
            publish();
        }
    }
    if (s_connect_deadline_ms != 0 &&
        (int32_t)(now - s_connect_deadline_ms) >= 0) {
        fail_state("连接超时，可重试");
        if (s_started) {
            esp_wifi_disconnect();
        }
        publish();
    }
}

static void reconcile_lost_event(void)
{
    if (!s_event_lost) {
        return;
    }
    s_event_lost = false;
    if (!s_started || !s_wifi.enabled) {
        return;
    }
    wifi_ap_record_t ap;
    bool linked = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (linked && !s_wait_disconnect && !s_wifi.connected) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info;
        if (netif != NULL && esp_netif_get_ip_info(netif, &info) == ESP_OK &&
            info.ip.addr != 0) {
            char ip[16];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
            handle_got_ip(ip);
        }
    } else if (!linked && (s_wifi.connected || s_wifi.state == PANEL_WIFI_CONNECTED)) {
        handle_disconnected(0);
    }
}

static void worker(void *arg)
{
    (void)arg;
    wifi_evt_t evt;
    for (;;) {
        check_deadlines();
        reconcile_lost_event();
        if (xQueueReceive(s_queue, &evt, pdMS_TO_TICKS(250)) != pdTRUE) {
            continue;
        }
        switch (evt.type) {
        case WIFI_CMD_ENABLE: handle_enable(); break;        case WIFI_CMD_DISABLE: handle_disable(); break;
        case WIFI_CMD_SCAN: handle_scan(); break;
        case WIFI_CMD_CONNECT: handle_connect(&evt); break;
        case WIFI_CMD_DISCONNECT: handle_disconnect(false); break;
        case WIFI_CMD_FORGET: handle_disconnect(true); break;
        case WIFI_EVT_STARTED: handle_started(); break;
        case WIFI_EVT_SCAN_DONE: scan_done(); break;
        case WIFI_EVT_DISCONNECTED: handle_disconnected(evt.reason); break;
        case WIFI_EVT_GOT_IP: handle_got_ip(evt.ip); break;
        default: break;
        }
        memset(&evt, 0, sizeof(evt));
        check_deadlines();
    }
}

esp_err_t panel_network_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_queue == NULL) {
        s_queue = xQueueCreate(WIFI_QUEUE_LEN, sizeof(wifi_evt_t));
        if (s_queue == NULL) return ESP_ERR_NO_MEM;
    }
    load_creds();
    panel_wifi_snapshot_reset(&s_wifi);
    s_wifi.auto_reconnect = s_has_confirmed && s_confirmed.auto_connect;
    s_wifi.enabled = false;
    publish();
    if (xTaskCreate(worker, "panel_wifi", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (s_has_confirmed && s_confirmed.auto_connect) {
        wifi_evt_t evt = {.type = WIFI_CMD_ENABLE};
        if (queue_send(&evt) != ESP_OK) {
            ESP_LOGW(TAG, "wifi boot enable queue full");
        }
    }
    return panel_network_ble_init();
}

esp_err_t panel_network_wifi_set_enabled(bool enabled)
{
    wifi_evt_t evt = {.type = enabled ? WIFI_CMD_ENABLE : WIFI_CMD_DISABLE};
    return queue_send(&evt);
}

esp_err_t panel_network_wifi_scan(void)
{
    lock();
    bool enabled = s_published.enabled;
    unlock();
    if (!enabled) return ESP_ERR_INVALID_STATE;
    wifi_evt_t evt = {.type = WIFI_CMD_SCAN};
    return queue_send(&evt);
}

esp_err_t panel_network_wifi_connect(const char *ssid, const char *password, uint8_t authmode,
                                     bool remember)
{
    char reason[64];
    if (!panel_wifi_credentials_valid(ssid, password, authmode, reason, sizeof(reason))) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = WIFI_CMD_CONNECT;
    evt.authmode = authmode;
    evt.remember = remember;
    snprintf(evt.ssid, sizeof(evt.ssid), "%s", ssid);
    snprintf(evt.password, sizeof(evt.password), "%s", password);
    esp_err_t err = queue_send(&evt);
    memset(&evt, 0, sizeof(evt));
    return err;
}

esp_err_t panel_network_wifi_disconnect(bool forget)
{
    wifi_evt_t evt = {.type = forget ? WIFI_CMD_FORGET : WIFI_CMD_DISCONNECT};
    return queue_send(&evt);
}

void panel_network_wifi_get(panel_wifi_snapshot_t *out)
{
    if (out == NULL) return;
    lock();
    *out = s_published;
    unlock();
}

bool panel_network_wifi_connected(void)
{
    lock();
    bool connected = s_published.connected;
    unlock();
    return connected;
}

#else

esp_err_t panel_network_init(void) { return ESP_OK; }

esp_err_t panel_network_wifi_set_enabled(bool enabled) { (void)enabled; return ESP_ERR_INVALID_STATE; }
esp_err_t panel_network_wifi_scan(void) { return ESP_ERR_INVALID_STATE; }
esp_err_t panel_network_wifi_connect(const char *ssid, const char *password, uint8_t authmode,
                                     bool remember)
{
    (void)ssid; (void)password; (void)authmode; (void)remember;
    return ESP_ERR_INVALID_STATE;
}
esp_err_t panel_network_wifi_disconnect(bool forget) { (void)forget; return ESP_ERR_INVALID_STATE; }
void panel_network_wifi_get(panel_wifi_snapshot_t *out)
{
    if (out == NULL) return;
    panel_wifi_snapshot_reset(out);
    out->state = PANEL_WIFI_UNAVAILABLE;
    snprintf(out->reason, sizeof(out->reason), "host build has no radio");
}
bool panel_network_wifi_connected(void) { return false; }

#endif
