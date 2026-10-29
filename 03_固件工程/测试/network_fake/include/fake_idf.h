#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110a

typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base, int32_t id, void *data);

typedef struct esp_netif_t esp_netif_t;
typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;
typedef struct {
    esp_ip4_addr_t ip;
} esp_netif_ip_info_t;

#define IPSTR "%u.%u.%u.%u"
#define IP2STR(a) ((unsigned)(((a)->addr) & 0xff)), ((unsigned)(((a)->addr >> 8) & 0xff)), \
                  ((unsigned)(((a)->addr >> 16) & 0xff)), ((unsigned)(((a)->addr >> 24) & 0xff))

typedef struct {
    uint8_t ssid[33];
    int8_t rssi;
    uint8_t authmode;
    uint8_t primary;
    uint8_t bssid[6];
} wifi_ap_record_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    struct {
        uint8_t authmode;
    } threshold;
} wifi_sta_config_t;
typedef union {
    wifi_sta_config_t sta;
} wifi_config_t;
typedef struct {
    int unused;
} wifi_init_config_t;
typedef struct {
    int unused;
} wifi_scan_config_t;

#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define WIFI_AUTH_OPEN 0
#define WIFI_MODE_STA 1
#define WIFI_IF_STA 0
#define WIFI_STORAGE_RAM 1

typedef struct {
    uint8_t reason;
} wifi_event_sta_disconnected_t;
typedef struct {
    esp_netif_ip_info_t ip_info;
} ip_event_got_ip_t;

extern const char *fake_event_wifi_base;
extern const char *fake_event_ip_base;
#define WIFI_EVENT fake_event_wifi_base
#define IP_EVENT fake_event_ip_base
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define WIFI_EVENT_SCAN_DONE 3
#define IP_EVENT_STA_GOT_IP 4
#define ESP_EVENT_ANY_ID -1

typedef struct fake_queue *QueueHandle_t;
typedef struct fake_sem *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1u
#define pdMS_TO_TICKS(x) ((TickType_t)(x))

typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1

/* fake control surface used only by the host test */
void fake_event_emit(esp_event_base_t base, int32_t id, void *data);
void fake_wifi_set_ap(const char *ssid, uint32_t ip);
void fake_wifi_set_disconnected(void);
void fake_wifi_add_scan_ap(const char *ssid, int8_t rssi, uint8_t authmode);
void fake_wifi_clear_scan(void);
void fake_advance_ms(uint32_t ms);
void fake_sleep_ms(unsigned ms);
void vTaskDelay(TickType_t ticks);
void fake_set_task_create_fail(bool fail);
void fake_nvs_reset(void);
bool fake_nvs_has_creds(void);

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                       unsigned prio, TaskHandle_t *out);
TickType_t xTaskGetTickCount(void);

esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *arg,
                                              void **instance);
esp_err_t esp_wifi_init(const wifi_init_config_t *cfg);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_set_config(int iface, wifi_config_t *cfg);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *cfg, bool block);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap);

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t len);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
const char *esp_err_to_name(esp_err_t err);

#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr, tag, msg) do { esp_err_t e_ = (expr); if (e_ != ESP_OK) { \
    ESP_LOGE(tag, "%s: %s", msg, esp_err_to_name(e_)); return e_; } } while (0)
