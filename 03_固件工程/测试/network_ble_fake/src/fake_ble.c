#include "fake_ble.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "esp_hosted.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

struct ble_hs_cfg_s ble_hs_cfg;

static fake_ble_stats_t g_stats;
static int g_hosted_connect_result;
static int g_fwversion_result;
static uint32_t g_fw_major;
static uint32_t g_fw_minor;
static uint32_t g_fw_patch;
static int g_nimble_init_result;
static int g_controller_init_result;
static int g_controller_enable_result;
static int g_disc_result;
static int g_connect_result;
static int g_disc_cancel_result;
static int g_conn_cancel_result;
static int g_terminate_result;
static int g_addr_result;

static ble_gap_event_fn *g_scan_cb;
static ble_gap_event_fn *g_connect_cb;
static ble_gattc_svc_fn *g_gatt_cb;
static uint16_t g_gatt_conn_handle;
static bool g_scan_active;
static bool g_connect_active;
static HANDLE g_host_stop_event;

const fake_ble_stats_t *fake_ble_stats(void)
{
    return &g_stats;
}

void fake_ble_reset(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    memset(&ble_hs_cfg, 0, sizeof(ble_hs_cfg));
    g_hosted_connect_result = 0;
    g_fwversion_result = 0;
    g_fw_major = 0;
    g_fw_minor = 0;
    g_fw_patch = 0;
    g_nimble_init_result = 0;
    g_controller_init_result = 0;
    g_controller_enable_result = 0;
    g_disc_result = 0;
    g_connect_result = 0;
    g_disc_cancel_result = 0;
    g_conn_cancel_result = 0;
    g_terminate_result = 0;
    g_addr_result = 0;
    g_scan_cb = NULL;
    g_connect_cb = NULL;
    g_gatt_cb = NULL;
    g_gatt_conn_handle = 0;
    g_scan_active = false;
    g_connect_active = false;
    if (g_host_stop_event != NULL) {
        CloseHandle(g_host_stop_event);
        g_host_stop_event = NULL;
    }
}

void fake_ble_set_hosted_connect_result(int rc) { g_hosted_connect_result = rc; }
void fake_ble_set_fwversion_result(int rc) { g_fwversion_result = rc; }
void fake_ble_set_fw_version(uint32_t major, uint32_t minor, uint32_t patch)
{
    g_fw_major = major;
    g_fw_minor = minor;
    g_fw_patch = patch;
}
void fake_ble_set_nimble_init_result(int rc) { g_nimble_init_result = rc; }
void fake_ble_set_controller_init_result(int rc) { g_controller_init_result = rc; }
void fake_ble_set_controller_enable_result(int rc) { g_controller_enable_result = rc; }
void fake_ble_set_disc_result(int rc) { g_disc_result = rc; }
void fake_ble_set_connect_result(int rc) { g_connect_result = rc; }
void fake_ble_set_disc_cancel_result(int rc) { g_disc_cancel_result = rc; }
void fake_ble_set_conn_cancel_result(int rc) { g_conn_cancel_result = rc; }
void fake_ble_set_terminate_result(int rc) { g_terminate_result = rc; }
void fake_ble_set_addr_result(int rc) { g_addr_result = rc; }

bool fake_ble_wait_flag(const volatile int *flag, int value, unsigned timeout_ms)
{
    for (unsigned elapsed = 0; elapsed <= timeout_ms; elapsed += 5) {
        if (*flag == value) {
            return true;
        }
        Sleep(5);
    }
    return *flag == value;
}

bool fake_ble_scan_active(void) { return g_scan_active; }
bool fake_ble_connect_active(void) { return g_connect_active; }

bool fake_ble_fire_sync(void)
{
    if (ble_hs_cfg.sync_cb == NULL) {
        return false;
    }
    ble_hs_cfg.sync_cb();
    return true;
}

bool fake_ble_fire_host_reset(int reason)
{
    if (ble_hs_cfg.reset_cb == NULL) {
        return false;
    }
    ble_hs_cfg.reset_cb(reason);
    return true;
}

bool fake_ble_fire_scan_result(uint32_t session)
{
    if (g_scan_cb == NULL) {
        return false;
    }
    struct ble_gap_event event;
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_EVENT_DISC;
    event.disc.addr.type = 1;
    event.disc.addr.val[0] = 0x11;
    event.disc.addr.val[1] = 0x22;
    event.disc.addr.val[2] = 0x33;
    event.disc.addr.val[3] = 0x44;
    event.disc.addr.val[4] = 0x55;
    event.disc.addr.val[5] = 0x66;
    event.disc.rssi = -42;
    event.disc.data = NULL;
    event.disc.length_data = 0;
    return g_scan_cb(&event, (void *)(uintptr_t)session) == 0;
}

bool fake_ble_fire_scan_complete(uint32_t session)
{
    if (g_scan_cb == NULL) {
        return false;
    }
    struct ble_gap_event event;
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_EVENT_DISC_COMPLETE;
    g_scan_active = false;
    return g_scan_cb(&event, (void *)(uintptr_t)session) == 0;
}

bool fake_ble_fire_connect_session(uint32_t session, int status, uint16_t handle)
{
    if (g_connect_cb == NULL) {
        return false;
    }
    struct ble_gap_event event;
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_EVENT_CONNECT;
    event.connect.status = status;
    event.connect.conn_handle = handle;
    g_connect_active = false;
    return g_connect_cb(&event, (void *)(uintptr_t)session) == 0;
}

bool fake_ble_fire_connect(int status, uint16_t handle)
{
    return fake_ble_fire_connect_session((uint32_t)g_stats.last_connect_session,
                                         status, handle);
}

bool fake_ble_fire_disconnect(uint16_t handle, int reason)
{
    return fake_ble_fire_disconnect_session((uint32_t)g_stats.last_connect_session,
                                            handle, reason);
}

bool fake_ble_fire_disconnect_session(uint32_t session, uint16_t handle, int reason)
{
    if (g_connect_cb == NULL) {
        return false;
    }
    struct ble_gap_event event;
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_EVENT_DISCONNECT;
    event.disconnect.conn.conn_handle = handle;
    event.disconnect.reason = (uint8_t)reason;
    return g_connect_cb(&event, (void *)(uintptr_t)session) == 0;
}

bool fake_ble_fire_svc(uint32_t session, bool last)
{
    if (g_gatt_cb == NULL) {
        return false;
    }
    struct ble_gatt_svc dummy;
    memset(&dummy, 0, sizeof(dummy));
    return g_gatt_cb(g_gatt_conn_handle, NULL, last ? NULL : &dummy,
                     (void *)(uintptr_t)session) == 0;
}

int ble_hs_util_ensure_addr(int prefer_random)
{
    (void)prefer_random;
    return g_addr_result;
}

int ble_hs_id_infer_auto(int privacy, uint8_t *out_addr_type)
{
    (void)privacy;
    if (g_addr_result != 0) {
        return g_addr_result;
    }
    if (out_addr_type != NULL) {
        *out_addr_type = 0;
    }
    return 0;
}

int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *fields,
                            const uint8_t *data, uint8_t length)
{
    (void)data;
    (void)length;
    if (fields != NULL) {
        fields->name = NULL;
        fields->name_len = 0;
    }
    return -1;
}

int ble_gap_disc(uint8_t own_addr_type, int32_t duration_ms,
                 const struct ble_gap_disc_params *disc_params,
                 ble_gap_event_fn *cb, void *cb_arg)
{
    (void)own_addr_type;
    (void)duration_ms;
    (void)disc_params;
    g_stats.gap_disc_calls++;
    g_stats.last_scan_session = (uint32_t)(uintptr_t)cb_arg;
    g_scan_cb = cb;
    g_scan_active = g_disc_result == 0;
    return g_disc_result;
}

int ble_gap_disc_cancel(void)
{
    g_stats.disc_cancel_calls++;
    if (g_disc_cancel_result == 0 || g_disc_cancel_result == BLE_HS_EALREADY) {
        g_scan_active = false;
    }
    return g_disc_cancel_result;
}

int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer_addr,
                    int32_t duration_ms,
                    const struct ble_gap_conn_params *params,
                    ble_gap_event_fn *cb, void *cb_arg)
{
    (void)own_addr_type;
    (void)peer_addr;
    (void)duration_ms;
    (void)params;
    g_stats.gap_connect_calls++;
    g_stats.last_connect_session = (uint32_t)(uintptr_t)cb_arg;
    g_connect_cb = cb;
    g_connect_active = g_connect_result == 0;
    return g_connect_result;
}

int ble_gap_conn_cancel(void)
{
    g_stats.conn_cancel_calls++;
    if (g_conn_cancel_result == BLE_HS_EALREADY) {
        g_connect_active = false;
    }
    return g_conn_cancel_result;
}

int ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason)
{
    g_stats.terminate_calls++;
    g_stats.last_terminate_handle = conn_handle;
    g_stats.last_terminate_reason = hci_reason;
    if (g_terminate_result == BLE_HS_ENOTCONN) {
        g_connect_active = false;
    }
    return g_terminate_result;
}

int ble_gattc_disc_all_svcs(uint16_t conn_handle, ble_gattc_svc_fn *cb, void *arg)
{
    g_stats.gatt_disc_calls++;
    g_stats.last_gatt_session = (uint32_t)(uintptr_t)arg;
    g_gatt_cb = cb;
    g_gatt_conn_handle = conn_handle;
    return 0;
}

int esp_hosted_connect_to_slave(void)
{
    g_stats.hosted_connect_calls++;
    return g_hosted_connect_result;
}

int esp_hosted_get_coprocessor_fwversion(esp_hosted_coprocessor_fwver_t *ver_info)
{
    g_stats.fwversion_calls++;
    if (ver_info != NULL) {
        ver_info->major1 = g_fw_major;
        ver_info->minor1 = g_fw_minor;
        ver_info->patch1 = g_fw_patch;
    }
    return g_fwversion_result;
}

esp_err_t esp_hosted_bt_controller_init(void)
{
    g_stats.controller_init_calls++;
    return g_controller_init_result;
}

esp_err_t esp_hosted_bt_controller_enable(void)
{
    g_stats.controller_enable_calls++;
    return g_controller_enable_result;
}

esp_err_t esp_hosted_bt_controller_disable(void)
{
    g_stats.controller_disable_calls++;
    return ESP_OK;
}

esp_err_t esp_hosted_bt_controller_deinit(bool mem_release)
{
    (void)mem_release;
    g_stats.controller_deinit_calls++;
    return ESP_OK;
}

esp_err_t nimble_port_init(void)
{
    g_stats.nimble_init_calls++;
    return g_nimble_init_result;
}

int nimble_port_stop(void)
{
    g_stats.nimble_stop_calls++;
    return 0;
}

esp_err_t nimble_port_deinit(void)
{
    g_stats.nimble_deinit_calls++;
    return ESP_OK;
}

void nimble_port_freertos_init(void (*host_task_fn)(void *))
{
    g_stats.host_task_init_calls++;
    if (g_host_stop_event == NULL) {
        g_host_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    }
    if (g_host_stop_event != NULL) {
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)host_task_fn, NULL, 0, NULL);
    }
}

void nimble_port_run(void)
{
    if (g_host_stop_event != NULL) {
        WaitForSingleObject(g_host_stop_event, INFINITE);
    }
}

void nimble_port_freertos_deinit(void)
{
    if (g_host_stop_event != NULL) {
        SetEvent(g_host_stop_event);
    }
}

void fake_ble_stop_host_task(void)
{
    if (g_host_stop_event != NULL) {
        SetEvent(g_host_stop_event);
    }
}

const char *esp_err_to_name(esp_err_t err)
{
    static char text[24];
    snprintf(text, sizeof(text), "err %d", (int)err);
    return text;
}
