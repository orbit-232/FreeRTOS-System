#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NO_MEM 0x101
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_WIFI_SSID_MAX 32
#define PANEL_WIFI_PASS_MAX 64
#define PANEL_NET_MAX_APS 16
#define PANEL_BLE_NAME_MAX 32
#define PANEL_NET_MAX_BLE 16

typedef enum {
    PANEL_WIFI_OFF = 0,
    PANEL_WIFI_IDLE,
    PANEL_WIFI_SCANNING,
    PANEL_WIFI_CONNECTING,
    PANEL_WIFI_CONNECTED,
    PANEL_WIFI_FAILED,
    PANEL_WIFI_UNAVAILABLE,
} panel_wifi_state_t;

typedef struct {
    char ssid[PANEL_WIFI_SSID_MAX + 1];
    int8_t rssi;
    uint8_t authmode;
    uint8_t channel;
    uint8_t bssid[6];
} panel_wifi_ap_t;

typedef struct {
    panel_wifi_state_t state;
    bool enabled;
    bool scanning;
    bool connected;
    bool auto_reconnect;
    uint32_t generation;
    uint32_t scan_generation;
    unsigned ap_count;
    panel_wifi_ap_t aps[PANEL_NET_MAX_APS];
    char ssid[PANEL_WIFI_SSID_MAX + 1];
    char ip[16];
    char reason[64];
} panel_wifi_snapshot_t;

typedef enum {
    PANEL_BLE_OFF = 0,
    PANEL_BLE_IDLE,
    PANEL_BLE_SCANNING,
    PANEL_BLE_CONNECTING,
    PANEL_BLE_CONNECTED,
    PANEL_BLE_FAILED,
    PANEL_BLE_UNAVAILABLE,
} panel_ble_state_t;

typedef struct {
    char name[PANEL_BLE_NAME_MAX + 1];
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
} panel_ble_device_t;

typedef struct {
    panel_ble_state_t state;
    bool enabled;
    bool scanning;
    uint32_t generation;
    uint32_t scan_generation;
    unsigned device_count;
    panel_ble_device_t devices[PANEL_NET_MAX_BLE];
    char connected_name[PANEL_BLE_NAME_MAX + 1];
    uint8_t connected_addr[6];
    uint8_t connected_addr_type;
    unsigned service_count;
    char reason[64];
} panel_ble_snapshot_t;

/* Pure helpers, safe to exercise on the host. */
bool panel_wifi_ssid_valid(const char *ssid);
bool panel_wifi_password_valid(const char *password);
/* authmode 0 is open; WPA passphrase 8..63 bytes or exactly 64 hex bytes. */
bool panel_wifi_credentials_valid(const char *ssid, const char *password, uint8_t authmode,
                                  char *reason, unsigned reason_len);
void panel_wifi_snapshot_reset(panel_wifi_snapshot_t *snapshot);
bool panel_wifi_scan_begin(panel_wifi_snapshot_t *snapshot);
bool panel_wifi_scan_add(panel_wifi_snapshot_t *snapshot, uint32_t generation,
                         const panel_wifi_ap_t *ap);
bool panel_wifi_scan_finish(panel_wifi_snapshot_t *snapshot, uint32_t generation);
void panel_ble_snapshot_reset(panel_ble_snapshot_t *snapshot);
bool panel_ble_scan_begin(panel_ble_snapshot_t *snapshot);
bool panel_ble_scan_add(panel_ble_snapshot_t *snapshot, uint32_t generation,
                        const panel_ble_device_t *device);
bool panel_ble_scan_finish(panel_ble_snapshot_t *snapshot, uint32_t generation);

esp_err_t panel_network_init(void);
esp_err_t panel_network_ble_init(void);
esp_err_t panel_network_wifi_set_enabled(bool enabled);
esp_err_t panel_network_wifi_scan(void);
esp_err_t panel_network_wifi_connect(const char *ssid, const char *password, uint8_t authmode,
                                     bool remember);
esp_err_t panel_network_wifi_disconnect(bool forget);
void panel_network_wifi_get(panel_wifi_snapshot_t *out);
bool panel_network_wifi_connected(void);

esp_err_t panel_network_ble_set_enabled(bool enabled);
esp_err_t panel_network_ble_scan(void);
esp_err_t panel_network_ble_stop(void);
esp_err_t panel_network_ble_connect(const uint8_t addr[6], uint8_t addr_type);
esp_err_t panel_network_ble_disconnect(void);
void panel_network_ble_get(panel_ble_snapshot_t *out);
bool panel_network_ble_connected(void);

#ifdef __cplusplus
}
#endif
