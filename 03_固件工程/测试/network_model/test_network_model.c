#include <stdio.h>
#include <string.h>

#include "panel_network.h"

static int failures;
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void)
{
    char ssid32[33];
    char ssid33[34];
    char pass64[65];
    char pass65[66];
    char hex64[65];
    char shortpass[8];
    memset(ssid32, 'a', 32); ssid32[32] = 0;
    memset(ssid33, 'a', 33); ssid33[33] = 0;
    memset(pass64, 'p', 64); pass64[64] = 0;
    memset(pass65, 'p', 65); pass65[65] = 0;
    memset(hex64, 'a', 64); hex64[64] = 0;
    memset(shortpass, 'a', 7); shortpass[7] = 0;
    check(panel_wifi_ssid_valid(ssid32), "32-byte SSID valid");
    check(!panel_wifi_ssid_valid(ssid33), "33-byte SSID rejected");
    check(!panel_wifi_ssid_valid(""), "empty SSID rejected");
    check(panel_wifi_password_valid(pass64), "64-byte password valid");
    check(!panel_wifi_password_valid(pass65), "65-byte password rejected");
    check(panel_wifi_password_valid(""), "empty password valid (open network)");
    char reason[64];
    check(panel_wifi_credentials_valid("Open", "", 0, reason, sizeof(reason)),
          "open AP empty password");
    check(!panel_wifi_credentials_valid("Open", "x", 0, reason, sizeof(reason)),
          "open AP rejects password");
    check(!panel_wifi_credentials_valid("Wpa", shortpass, 3, reason, sizeof(reason)),
          "WPA rejects 7 bytes");
    check(panel_wifi_credentials_valid("Wpa", "12345678", 3, reason, sizeof(reason)),
          "WPA accepts 8 bytes");
    check(panel_wifi_credentials_valid("Wpa", hex64, 3, reason, sizeof(reason)),
          "WPA accepts 64 hex");
    check(!panel_wifi_credentials_valid("Wpa", pass64, 3, reason, sizeof(reason)),
          "WPA rejects 64 non-hex");

    panel_wifi_snapshot_t wifi;
    panel_wifi_snapshot_reset(&wifi);
    wifi.enabled = true;
    check(panel_wifi_scan_begin(&wifi), "scan begin");
    uint32_t gen = wifi.scan_generation;
    for (int i = 0; i < 20; i++) {
        panel_wifi_ap_t ap;
        memset(&ap, 0, sizeof(ap));
        snprintf(ap.ssid, sizeof(ap.ssid), "ap-%d", i);
        ap.rssi = (int8_t)-i;
        check(panel_wifi_scan_add(&wifi, gen, &ap), "scan add");
    }
    check(wifi.ap_count == PANEL_NET_MAX_APS, "scan list capped at 16");
    panel_wifi_ap_t stale;
    memset(&stale, 0, sizeof(stale));
    snprintf(stale.ssid, sizeof(stale.ssid), "stale");
    check(!panel_wifi_scan_add(&wifi, gen + 1, &stale), "stale scan generation rejected");
    check(panel_wifi_scan_finish(&wifi, gen), "scan finish");
    check(!wifi.scanning && wifi.state == PANEL_WIFI_IDLE, "scan state idle");

    panel_ble_snapshot_t ble;
    panel_ble_snapshot_reset(&ble);
    ble.enabled = true;
    check(panel_ble_scan_begin(&ble), "ble scan begin");
    uint32_t bgen = ble.scan_generation;
    for (int i = 0; i < 20; i++) {
        panel_ble_device_t dev;
        memset(&dev, 0, sizeof(dev));
        snprintf(dev.name, sizeof(dev.name), "dev-%d", i);
        dev.addr[0] = (uint8_t)i;
        dev.rssi = (int8_t)-i;
        check(panel_ble_scan_add(&ble, bgen, &dev), "ble scan add");
    }
    check(ble.device_count == PANEL_NET_MAX_BLE, "ble list capped at 16");
    panel_ble_device_t dup;
    memset(&dup, 0, sizeof(dup));
    snprintf(dup.name, sizeof(dup.name), "renamed");
    dup.addr[0] = 3;
    check(panel_ble_scan_add(&ble, bgen, &dup), "ble duplicate accepted");
    check(ble.device_count == PANEL_NET_MAX_BLE, "ble duplicate does not grow list");
    check(strcmp(ble.devices[3].name, "renamed") == 0, "ble duplicate updated");
    check(panel_ble_scan_finish(&ble, bgen), "ble scan finish");

    panel_ble_snapshot_t typed;
    panel_ble_snapshot_reset(&typed);
    typed.enabled = true;
    check(panel_ble_scan_begin(&typed), "typed ble scan");
    panel_ble_device_t t0;
    memset(&t0, 0, sizeof(t0));
    snprintf(t0.name, sizeof(t0.name), "same-address");
    t0.addr[0] = 0x33;
    t0.addr_type = 0;
    check(panel_ble_scan_add(&typed, typed.scan_generation, &t0), "typed addr add");
    t0.addr_type = 1;
    check(panel_ble_scan_add(&typed, typed.scan_generation, &t0), "typed addr/type add");
    check(typed.device_count == 2, "address+type is duplicate key");

    printf("NETWORK_MODEL_TEST failures=%d\n", failures);
    return failures ? 1 : 0;
}
