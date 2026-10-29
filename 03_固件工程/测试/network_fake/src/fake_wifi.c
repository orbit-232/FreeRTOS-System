#include "fake_idf.h"

#include <stdlib.h>

typedef struct {
    esp_event_base_t base;
    int32_t id;
    esp_event_handler_t handler;
    void *arg;
} handler_entry_t;

static handler_entry_t g_handlers[4];
static unsigned g_handler_count;
const char *fake_event_wifi_base = "WIFI_EVENT";
const char *fake_event_ip_base = "IP_EVENT";
static char g_config_ssid[33];
static char g_config_pass[65];
static char g_ap_ssid[33];
static uint32_t g_ip;
static bool g_connected;
static bool g_connecting;
static bool g_started;
static bool g_scanning;

void fake_event_emit(esp_event_base_t base, int32_t id, void *data)
{
    for (unsigned i = 0; i < g_handler_count; i++) {
        if (g_handlers[i].base == base &&
            (g_handlers[i].id == id || g_handlers[i].id == ESP_EVENT_ANY_ID)) {
            g_handlers[i].handler(g_handlers[i].arg, base, id, data);
        }
    }
}

void fake_wifi_set_ap(const char *ssid, uint32_t ip)
{
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "%s", ssid ? ssid : "");
    g_ip = ip;
    g_connected = true;
    g_connecting = false;
}

void fake_wifi_set_disconnected(void)
{
    g_connected = false;
    g_connecting = false;
    g_ap_ssid[0] = '\0';
    g_ip = 0;
}

void fake_wifi_add_scan_ap(const char *ssid, int8_t rssi, uint8_t authmode)
{
    (void)ssid; (void)rssi; (void)authmode;
}

void fake_wifi_clear_scan(void) {}

esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { static int x; return (esp_netif_t *)&x; }
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { (void)key; static int x; return (esp_netif_t *)&x; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
    (void)netif;
    if (!info) return ESP_ERR_INVALID_ARG;
    info->ip.addr = g_ip;
    return g_ip ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *arg,
                                              void **instance)
{
    (void)instance;
    if (g_handler_count >= 4) return ESP_ERR_NO_MEM;
    g_handlers[g_handler_count++] = (handler_entry_t){base, id, handler, arg};
    return ESP_OK;
}

esp_err_t esp_wifi_init(const wifi_init_config_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int storage) { (void)storage; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int mode) { (void)mode; return ESP_OK; }
esp_err_t esp_wifi_start(void) { g_started = true; return ESP_OK; }
esp_err_t esp_wifi_stop(void) { g_started = false; return ESP_OK; }
esp_err_t esp_wifi_connect(void)
{
    if (!g_started) return ESP_ERR_INVALID_STATE;
    g_connecting = true;
    return ESP_OK;
}
esp_err_t esp_wifi_disconnect(void)
{
    g_connecting = false;
    g_connected = false;
    return ESP_OK;
}
esp_err_t esp_wifi_set_config(int iface, wifi_config_t *cfg)
{
    (void)iface;
    if (!cfg) return ESP_ERR_INVALID_ARG;
    size_t n = strnlen((const char *)cfg->sta.ssid, sizeof(cfg->sta.ssid));
    memcpy(g_config_ssid, cfg->sta.ssid, n);
    g_config_ssid[n] = '\0';
    n = strnlen((const char *)cfg->sta.password, sizeof(cfg->sta.password));
    memcpy(g_config_pass, cfg->sta.password, n);
    g_config_pass[n] = '\0';
    return ESP_OK;
}
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *cfg, bool block)
{
    (void)cfg; (void)block;
    g_scanning = true;
    return ESP_OK;
}
esp_err_t esp_wifi_scan_stop(void) { g_scanning = false; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records)
{
    if (count) *count = 0;
    (void)records;
    return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{
    if (!g_connected || !ap) return ESP_FAIL;
    memset(ap, 0, sizeof(*ap));
    snprintf((char *)ap->ssid, sizeof(ap->ssid), "%s", g_ap_ssid);
    return ESP_OK;
}
const char *esp_err_to_name(esp_err_t err) { (void)err; return "fake"; }
