#include "panel_network.h"
#include "fake_idf.h"

#include <stdio.h>
#include <string.h>

static int failures;
static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

esp_err_t panel_network_ble_init(void) { return ESP_OK; }
esp_err_t panel_network_ble_set_enabled(bool enabled) { (void)enabled; return ESP_OK; }
esp_err_t panel_network_ble_scan(void) { return ESP_OK; }
esp_err_t panel_network_ble_stop(void) { return ESP_OK; }
esp_err_t panel_network_ble_connect(const uint8_t addr[6], uint8_t addr_type)
{
    (void)addr; (void)addr_type;
    return ESP_OK;
}
esp_err_t panel_network_ble_disconnect(void) { return ESP_OK; }
void panel_network_ble_get(panel_ble_snapshot_t *out)
{
    if (out) panel_ble_snapshot_reset(out);
}
bool panel_network_ble_connected(void) { return false; }

static void wait_ms(int ms) { fake_sleep_ms((unsigned)ms); }
static void emit_ip(uint32_t addr)
{
    ip_event_got_ip_t data;
    memset(&data, 0, sizeof(data));
    data.ip_info.ip.addr = addr;
    fake_event_emit(IP_EVENT, IP_EVENT_STA_GOT_IP, &data);
}
static void emit_disconnected(int reason)
{
    wifi_event_sta_disconnected_t data = {.reason = (uint8_t)reason};
    fake_event_emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &data);
}

static bool wait_state(panel_wifi_state_t want, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        panel_wifi_snapshot_t s;
        panel_network_wifi_get(&s);
        if (s.state == want) return true;
        wait_ms(10);
    }
    return false;
}

int main(void)
{
    fake_nvs_reset();
    check(panel_network_init() == ESP_OK, "init");
    wait_ms(30);
    check(panel_network_wifi_scan() == ESP_ERR_INVALID_STATE, "scan rejected while off");
    check(panel_network_wifi_connect("x", "bad", 3, true) == ESP_ERR_INVALID_ARG,
          "invalid credential rejected");
    check(panel_network_wifi_set_enabled(true) == ESP_OK, "enable queue");
    wait_ms(80);
    fake_event_emit(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    wait_ms(80);

    panel_wifi_snapshot_t s;
    panel_network_wifi_get(&s);
    check(s.enabled && !s.connected, "enabled idle");

    /* Open AP, remember=false: must not write NVS, but can connect. */
    check(panel_network_wifi_connect("Open", "", 0, false) == ESP_OK, "open queue");
    wait_ms(100);
    fake_wifi_set_ap("Open", 0x0101A8C0u);
    emit_ip(0x0101A8C0u);
    wait_ms(100);
    panel_network_wifi_get(&s);
    check(s.connected && strcmp(s.ssid, "Open") == 0, "open connected");
    check(!fake_nvs_has_creds(), "remember=false not persisted");

    /* Late IP/disconnect after disable must not revive OFF. */
    check(panel_network_wifi_set_enabled(false) == ESP_OK, "disable queue");
    wait_ms(100);
    emit_ip(0x0101A8C0u);
    emit_disconnected(8);
    wait_ms(80);
    panel_network_wifi_get(&s);
    check(s.state == PANEL_WIFI_OFF && !s.connected && s.ip[0] == '\0', "late events ignored");

    /* Remembered AP: save on confirmed IP. */
    check(panel_network_wifi_set_enabled(true) == ESP_OK, "re-enable");
    wait_ms(80);
    check(panel_network_wifi_connect("Saved", "12345678", 3, true) == ESP_OK, "saved queue");
    wait_ms(100);
    fake_wifi_set_ap("Saved", 0x0201A8C0u);
    emit_ip(0x0201A8C0u);
    wait_ms(100);
    panel_network_wifi_get(&s);
    check(s.connected, "saved connected");
    check(fake_nvs_has_creds(), "remember=true persisted");

    /* New AP failure must not silently reconnect the old confirmed AP. */
    check(panel_network_wifi_connect("Wrong", "bad-password", 3, true) == ESP_OK, "wrong queue");
    check(wait_state(PANEL_WIFI_CONNECTING, 500), "wrong wait old disconnect");
    fake_wifi_set_disconnected();
    emit_disconnected(15);
    wait_ms(100);
    emit_disconnected(15);
    check(wait_state(PANEL_WIFI_FAILED, 500), "new AP failure stays failed");
    panel_network_wifi_get(&s);
    check(!s.connected, "new AP failure not connected");

    /* AP mismatch on GOT_IP is rejected. */
    check(panel_network_wifi_connect("Expected", "12345678", 3, true) == ESP_OK, "mismatch queue");
    wait_ms(120);
    fake_wifi_set_disconnected();
    emit_disconnected(8);
    wait_ms(120);
    fake_wifi_set_ap("Other", 0x0301A8C0u);
    emit_ip(0x0301A8C0u);
    check(wait_state(PANEL_WIFI_FAILED, 500), "AP mismatch rejected");

    /* Timeout must fire even if the worker is not idle. */
    check(panel_network_wifi_connect("Timeout", "12345678", 3, false) == ESP_OK, "timeout queue");
    wait_ms(120);
    fake_wifi_set_disconnected();
    emit_disconnected(8);
    check(wait_state(PANEL_WIFI_CONNECTING, 500), "timeout wait connect");
    fake_advance_ms(16000);
    check(wait_state(PANEL_WIFI_FAILED, 800), "connect timeout");

    /* Scan while connected is allowed; late scan result after disable ignored. */
    check(panel_network_wifi_connect("ScanAP", "12345678", 3, false) == ESP_OK, "scan connect");
    check(wait_state(PANEL_WIFI_CONNECTING, 500), "scan wait connect");
    fake_wifi_set_ap("ScanAP", 0x0401A8C0u);
    emit_ip(0x0401A8C0u);
    check(wait_state(PANEL_WIFI_CONNECTED, 500), "scan AP connected");
    check(panel_network_wifi_scan() == ESP_OK, "scan queue");
    wait_ms(80);
    panel_network_wifi_get(&s);
    check(s.scanning, "scanning");
    fake_event_emit(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, NULL);
    wait_ms(100);
    panel_network_wifi_get(&s);
    check(!s.scanning && s.state == PANEL_WIFI_CONNECTED, "scan keeps connected");
    check(panel_network_wifi_set_enabled(false) == ESP_OK, "disable after scan");
    wait_ms(80);
    fake_event_emit(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, NULL);
    wait_ms(50);
    panel_network_wifi_get(&s);
    check(s.state == PANEL_WIFI_OFF, "late scan after off ignored");

    printf("NETWORK_FAKE_TEST failures=%d\n", failures);
    return failures ? 1 : 0;
}
