#include "panel_network.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static size_t bounded_len(const char *text, size_t max)
{
    if (text == NULL) return 0;
    for (size_t i = 0; i < max; i++) {
        if (text[i] == '\0') return i;
    }
    return max;
}

bool panel_wifi_ssid_valid(const char *ssid)
{
    if (ssid == NULL) {
        return false;
    }
    size_t len = bounded_len(ssid, PANEL_WIFI_SSID_MAX + 1);
    if (len == 0 || len > PANEL_WIFI_SSID_MAX) {
        return false;
    }
    return ssid[len] == '\0';
}

bool panel_wifi_password_valid(const char *password)
{
    if (password == NULL) {
        return false;
    }
    size_t len = bounded_len(password, PANEL_WIFI_PASS_MAX + 1);
    if (len > PANEL_WIFI_PASS_MAX) {
        return false;
    }
    return password[len] == '\0';
}

bool panel_wifi_credentials_valid(const char *ssid, const char *password, uint8_t authmode,
                                  char *reason, unsigned reason_len)
{
    if (reason != NULL && reason_len > 0) {
        reason[0] = '\0';
    }
    if (!panel_wifi_ssid_valid(ssid)) {
        if (reason != NULL && reason_len > 0) snprintf(reason, reason_len, "SSID 无效");
        return false;
    }
    if (password == NULL || !panel_wifi_password_valid(password)) {
        if (reason != NULL && reason_len > 0) snprintf(reason, reason_len, "密码过长");
        return false;
    }
    size_t len = strlen(password);
    if (authmode == 0) {
        if (len != 0) {
            if (reason != NULL && reason_len > 0) snprintf(reason, reason_len, "开放网络密码必须为空");
            return false;
        }
        return true;
    }
    if (len >= 8 && len <= 63) {
        return true;
    }
    if (len == 64) {
        for (size_t i = 0; i < 64; i++) {
            if (!isxdigit((unsigned char)password[i])) {
                if (reason != NULL && reason_len > 0) {
                    snprintf(reason, reason_len, "64 位密码必须是十六进制");
                }
                return false;
            }
        }
        return true;
    }
    if (reason != NULL && reason_len > 0) {
        snprintf(reason, reason_len, "密码需为 8-63 位或 64 位十六进制");
    }
    return false;
}

void panel_wifi_snapshot_reset(panel_wifi_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = PANEL_WIFI_OFF;
}

bool panel_wifi_scan_begin(panel_wifi_snapshot_t *snapshot)
{
    if (snapshot == NULL || !snapshot->enabled) {
        return false;
    }
    snapshot->generation++;
    snapshot->scan_generation = snapshot->generation;
    snapshot->scanning = true;
    snapshot->ap_count = 0;
    snapshot->state = PANEL_WIFI_SCANNING;
    snapshot->reason[0] = '\0';
    return true;
}

bool panel_wifi_scan_add(panel_wifi_snapshot_t *snapshot, uint32_t generation,
                         const panel_wifi_ap_t *ap)
{
    if (snapshot == NULL || ap == NULL || generation != snapshot->scan_generation ||
        !snapshot->scanning) {
        return false;
    }
    if (snapshot->ap_count >= PANEL_NET_MAX_APS) {
        return true; /* bounded: ignore extra, keep the scan successful */
    }
    if (!panel_wifi_ssid_valid(ap->ssid)) {
        return false;
    }
    snapshot->aps[snapshot->ap_count++] = *ap;
    return true;
}

bool panel_wifi_scan_finish(panel_wifi_snapshot_t *snapshot, uint32_t generation)
{
    if (snapshot == NULL || generation != snapshot->scan_generation) {
        return false;
    }
    snapshot->scanning = false;
    if (snapshot->connected) {
        snapshot->state = PANEL_WIFI_CONNECTED;
    } else if (snapshot->state == PANEL_WIFI_SCANNING) {
        snapshot->state = PANEL_WIFI_IDLE;
    }
    snapshot->generation++;
    return true;
}

void panel_ble_snapshot_reset(panel_ble_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = PANEL_BLE_OFF;
}

bool panel_ble_scan_begin(panel_ble_snapshot_t *snapshot)
{
    if (snapshot == NULL || !snapshot->enabled) {
        return false;
    }
    snapshot->generation++;
    snapshot->scan_generation = snapshot->generation;
    snapshot->scanning = true;
    snapshot->device_count = 0;
    snapshot->state = PANEL_BLE_SCANNING;
    snapshot->reason[0] = '\0';
    return true;
}

bool panel_ble_scan_add(panel_ble_snapshot_t *snapshot, uint32_t generation,
                        const panel_ble_device_t *device)
{
    if (snapshot == NULL || device == NULL || generation != snapshot->scan_generation ||
        !snapshot->scanning) {
        return false;
    }
    for (unsigned i = 0; i < snapshot->device_count; i++) {
        if (snapshot->devices[i].addr_type == device->addr_type &&
            memcmp(snapshot->devices[i].addr, device->addr, 6) == 0) {
            snapshot->devices[i] = *device;
            snapshot->generation++;
            return true;
        }
    }
    if (snapshot->device_count >= PANEL_NET_MAX_BLE) {
        return true;
    }
    snapshot->devices[snapshot->device_count++] = *device;
    snapshot->generation++;
    return true;
}

bool panel_ble_scan_finish(panel_ble_snapshot_t *snapshot, uint32_t generation)
{
    if (snapshot == NULL || generation != snapshot->scan_generation) {
        return false;
    }
    snapshot->scanning = false;
    if (snapshot->state == PANEL_BLE_SCANNING) {
        snapshot->state = PANEL_BLE_IDLE;
    }
    snapshot->generation++;
    return true;
}
