#include "panel_network.h"

#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM) && defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_NIMBLE_ENABLED)

#include "esp_hosted.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "panel_ble";
#define BLE_SCAN_DURATION_MS 10000
#define BLE_CONNECT_TIMEOUT_MS 10000
#define BLE_HOST_SYNC_TIMEOUT_MS 3000
#define BLE_ASYNC_TIMEOUT_MS 5000
#define BLE_WORKER_POLL_MS 100
#define BLE_QUEUE_LEN 8

typedef enum {
    BLE_CMD_ENABLE,
    BLE_CMD_DISABLE,
    BLE_CMD_SCAN,
    BLE_CMD_STOP,
    BLE_CMD_CONNECT,
    BLE_CMD_DISCONNECT,
    BLE_CMD_START_PENDING,
} ble_cmd_type_t;

typedef struct {
    ble_cmd_type_t type;
    uint8_t addr[6];
    uint8_t addr_type;
} ble_cmd_t;

/*
 * Minimal lifecycle policy:
 * - The NimBLE host is created at most once per boot.  Normal disable only
 *   tears down active GAP operations and leaves the host running idle.
 * - A host reset, HCI sync timeout or host creation failure is terminal:
 *   s_host_inited is kept as-is and s_fault forbids every later init.
 * - Disable waits for the real connect-cancel / disconnect completion before
 *   reporting OFF.  Scanning is aborted synchronously by NimBLE's
 *   ble_gap_disc_cancel(), so no DISC_COMPLETE is required for that path.
 * - Session counters prevent late scan/connect/GATT callbacks from changing a
 *   newer state.
 */
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_sync_sem;
static QueueHandle_t s_queue;
static bool s_worker_started;
static bool s_teardown_deadline_active;
static TickType_t s_teardown_deadline;
static panel_ble_snapshot_t s_snapshot;
static bool s_enabled;
static bool s_host_inited;
static bool s_host_ready;
static bool s_fault;
static bool s_scanning;
static bool s_pending_connect;
static bool s_connect_inflight;
static bool s_connect_cancel_requested;
static bool s_disconnect_requested;
static bool s_disable_pending;
static uint8_t s_pending_addr[6];
static uint8_t s_pending_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint32_t s_gatt_session;
static uint32_t s_scan_session;
static uint32_t s_connect_session;
static uint8_t s_own_addr_type;

static void lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void host_sync(void);
static void host_reset(int reason);
static void host_task(void *arg);
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static bool apply_scan_cancel_result(int rc, bool user_stop);
static void handle_conn_cancel_result(int rc);
static void handle_terminate_result(int rc);
static void start_connect(void);

static bool scan_cancel_ok(int rc)
{
    return rc == 0 || rc == BLE_HS_EALREADY;
}

static void clear_teardown_deadline_locked(void)
{
    s_teardown_deadline_active = false;
    s_teardown_deadline = 0;
}

static void arm_teardown_deadline_locked(void)
{
    s_teardown_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(BLE_ASYNC_TIMEOUT_MS);
    s_teardown_deadline_active = true;
}

static bool teardown_deadline_expired_locked(void)
{
    return s_teardown_deadline_active &&
           (int32_t)(xTaskGetTickCount() - s_teardown_deadline) >= 0;
}

static void fault_locked(const char *reason)
{
    s_fault = true;
    s_host_ready = false;
    s_enabled = false;
    s_disable_pending = false;
    s_scanning = false;
    s_pending_connect = false;
    s_connect_inflight = false;
    s_connect_cancel_requested = false;
    s_disconnect_requested = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    clear_teardown_deadline_locked();
    s_gatt_session++;
    s_scan_session++;
    s_connect_session++;
    s_snapshot.enabled = false;
    s_snapshot.scanning = false;
    s_snapshot.device_count = 0;
    s_snapshot.service_count = 0;
    s_snapshot.generation++;
    s_snapshot.connected_name[0] = '\0';
    s_snapshot.state = PANEL_BLE_UNAVAILABLE;
    snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "%s",
             reason != NULL ? reason : "BLE 不可用，请重启");
}

static bool maybe_finish_disable_locked(void)
{
    if (!s_disable_pending) {
        return false;
    }
    if (s_scanning || s_connect_inflight || s_pending_connect ||
        s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }
    s_disable_pending = false;
    s_enabled = false;
    s_disconnect_requested = false;
    s_connect_cancel_requested = false;
    clear_teardown_deadline_locked();
    s_snapshot.enabled = false;
    s_snapshot.scanning = false;
    s_snapshot.service_count = 0;
    s_snapshot.state = PANEL_BLE_OFF;
    s_snapshot.reason[0] = '\0';
    return true;
}

static void check_teardown_deadline(void)
{
    bool expired = false;
    lock();
    if (!s_fault && teardown_deadline_expired_locked()) {
        fault_locked("BLE 断开超时，请重启");
        expired = true;
    }
    unlock();
    if (expired) {
        ESP_LOGE(TAG, "BLE teardown deadline expired; host faulted, restart required");
    }
}

static void signal_sync_waiter(void)
{
    if (s_sync_sem != NULL) {
        (void)xSemaphoreGive(s_sync_sem);
    }
}

static bool post(ble_cmd_t *cmd)
{
    if (s_queue == NULL) {
        return false;
    }
    return xQueueSend(s_queue, cmd, 0) == pdTRUE;
}

/*
 * ESP-Hosted-MCU before v2.5.2 enabled the co-processor BT controller by
 * default.  v2.5.2 introduced the controller init/enable RPCs.  The old
 * Waveshare C6 slave reports 0.0.0 (unreported) and does not implement the
 * FeatureControl RPC, so use the documented legacy order only for that
 * range.  This is a selection hint, not a claim that 0.0.0 is the real
 * firmware version.
 */
static bool fw_requires_legacy(bool version_ok, const esp_hosted_coprocessor_fwver_t *ver)
{
    if (!version_ok || ver == NULL) {
        return true;
    }
    if (ver->major1 < 2) {
        return true;
    }
    if (ver->major1 == 2 && ver->minor1 < 5) {
        return true;
    }
    return ver->major1 == 2 && ver->minor1 == 5 && ver->patch1 < 2;
}

static bool wait_hci_sync(uint32_t timeout_ms)
{
    if (s_sync_sem == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    lock();
    bool ok = s_host_ready && !s_fault;
    unlock();
    return ok;
}

static void start_scan(void)
{
    lock();
    if (s_fault || !s_enabled || s_disable_pending || !s_host_ready ||
        s_scanning || s_pending_connect || s_connect_inflight ||
        s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        unlock();
        return;
    }
    if (!panel_ble_scan_begin(&s_snapshot)) {
        unlock();
        return;
    }
    s_scanning = true;
    s_scan_session++;
    uint32_t session = s_scan_session;
    uint32_t generation = s_snapshot.scan_generation;
    unlock();

    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.itvl = 0x60;
    params.window = 0x30;
    params.filter_policy = 0;
    params.limited = 0;
    params.passive = 0;
    int rc = ble_gap_disc(s_own_addr_type, BLE_SCAN_DURATION_MS, &params,
                          ble_gap_event, (void *)(uintptr_t)session);
    if (rc != 0) {
        lock();
        if (!s_fault && session == s_scan_session) {
            s_scanning = false;
            s_scan_session++;
            panel_ble_scan_finish(&s_snapshot, generation);
            s_snapshot.state = PANEL_BLE_FAILED;
            snprintf(s_snapshot.reason, sizeof(s_snapshot.reason),
                     "扫描启动失败 %d", rc);
        }
        unlock();
    }
}

static bool apply_scan_cancel_result(int rc, bool user_stop)
{
    bool ok = scan_cancel_ok(rc);

    lock();
    if (s_fault) {
        unlock();
        return false;
    }
    if (ok) {
        /* ble_gap_disc_cancel() success means scanning is fully aborted. */
        if (s_scanning) {
            s_scanning = false;
            panel_ble_scan_finish(&s_snapshot, s_snapshot.scan_generation);
            s_snapshot.scanning = false;
        }
        s_scan_session++;
        if (s_disable_pending) {
            (void)maybe_finish_disable_locked();
        } else if (s_enabled && s_conn_handle == BLE_HS_CONN_HANDLE_NONE &&
                   !s_connect_inflight) {
            s_snapshot.state = PANEL_BLE_IDLE;
            if (user_stop) {
                snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "扫描已停止");
            }
        } else if (!s_enabled && s_conn_handle == BLE_HS_CONN_HANDLE_NONE &&
                   !s_connect_inflight && !s_disable_pending) {
            s_snapshot.state = PANEL_BLE_OFF;
        }
    } else {
        fault_locked("停止扫描失败，请重启");
    }
    unlock();
    return ok;
}

static void stop_scan(void)
{
    int rc;

    lock();
    if (s_fault) {
        unlock();
        return;
    }
    s_pending_connect = false;
    if (!s_scanning) {
        if (s_disable_pending) {
            (void)maybe_finish_disable_locked();
        } else if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            s_snapshot.state = PANEL_BLE_CONNECTED;
        } else if (s_connect_inflight) {
            s_snapshot.state = PANEL_BLE_CONNECTING;
        } else if (s_enabled) {
            s_snapshot.state = PANEL_BLE_IDLE;
            snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "扫描已停止");
        } else {
            s_snapshot.state = PANEL_BLE_OFF;
        }
        s_snapshot.scanning = false;
        unlock();
        return;
    }
    unlock();

    rc = ble_gap_disc_cancel();
    (void)apply_scan_cancel_result(rc, true);
}

static void handle_conn_cancel_result(int rc)
{
    lock();
    if (s_fault) {
        unlock();
        return;
    }
    if (rc == 0) {
        /* The CONNECT event with a cancelled status completes this path. */
    } else if (rc == BLE_HS_EALREADY) {
        s_connect_inflight = false;
        s_connect_cancel_requested = false;
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            /*
             * A CONNECT success raced the cancel.  Its own terminate path owns
             * the state and deadline; do not clear either here.
             */
            unlock();
            return;
        }
        clear_teardown_deadline_locked();
        s_gatt_session++;
        if (s_disable_pending) {
            (void)maybe_finish_disable_locked();
        } else if (s_enabled) {
            s_snapshot.state = PANEL_BLE_IDLE;
            snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "连接已取消");
        } else {
            s_snapshot.state = PANEL_BLE_OFF;
        }
    } else {
        fault_locked("取消连接失败，请重启");
    }
    unlock();
}

static void handle_terminate_result(int rc)
{
    lock();
    if (s_fault) {
        unlock();
        return;
    }
    if (rc == 0) {
        /* The DISCONNECT event completes this path. */
    } else if (rc == BLE_HS_ENOTCONN) {
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        clear_teardown_deadline_locked();
        s_gatt_session++;
        s_disconnect_requested = false;
        if (s_disable_pending) {
            (void)maybe_finish_disable_locked();
        } else if (s_enabled) {
            s_snapshot.state = PANEL_BLE_IDLE;
            snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "已断开");
        } else {
            s_snapshot.state = PANEL_BLE_OFF;
        }
    } else if (s_disable_pending) {
        fault_locked("断开连接失败，请重启");
    } else {
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            clear_teardown_deadline_locked();
            unlock();
            return;
        }
        s_snapshot.state = PANEL_BLE_CONNECTED;
        clear_teardown_deadline_locked();
        snprintf(s_snapshot.reason, sizeof(s_snapshot.reason),
                 "断开请求失败 %d", rc);
    }
    unlock();
}

static void start_connect(void)
{
    lock();
    if (s_fault || !s_enabled || s_disable_pending || !s_host_ready ||
        !s_pending_connect || s_connect_inflight || s_scanning ||
        s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        unlock();
        return;
    }
    uint8_t addr[6];
    uint8_t type = s_pending_type;
    memcpy(addr, s_pending_addr, sizeof(addr));
    clear_teardown_deadline_locked();
    s_pending_connect = false;
    s_connect_inflight = true;
    s_connect_cancel_requested = false;
    s_disconnect_requested = false;
    s_snapshot.state = PANEL_BLE_CONNECTING;
    s_snapshot.service_count = 0;
    s_snapshot.connected_name[0] = '\0';
    memcpy(s_snapshot.connected_addr, addr, sizeof(addr));
    s_snapshot.connected_addr_type = type;
    s_snapshot.reason[0] = '\0';
    s_gatt_session++;
    uint32_t session = ++s_connect_session;
    unlock();

    ble_addr_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.type = type;
    memcpy(peer.val, addr, sizeof(peer.val));
    int rc = ble_gap_connect(s_own_addr_type, &peer, BLE_CONNECT_TIMEOUT_MS, NULL,
                             ble_gap_event, (void *)(uintptr_t)session);
    if (rc != 0) {
        lock();
        if (!s_fault && session == s_connect_session) {
            s_connect_inflight = false;
            clear_teardown_deadline_locked();
            s_gatt_session++;
            if (s_disable_pending) {
                (void)maybe_finish_disable_locked();
            } else if (s_enabled) {
                s_snapshot.state = PANEL_BLE_FAILED;
                snprintf(s_snapshot.reason, sizeof(s_snapshot.reason),
                         "连接失败 %d", rc);
            } else {
                s_snapshot.state = PANEL_BLE_OFF;
            }
        }
        unlock();
    }
}

static void disable_ble(void)
{
    bool scanning;
    bool connecting;
    uint16_t conn;

    lock();
    if (s_fault) {
        s_snapshot.enabled = false;
        unlock();
        return;
    }
    if (!s_enabled && !s_disable_pending && !s_scanning &&
        !s_connect_inflight && s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        s_snapshot.enabled = false;
        s_snapshot.scanning = false;
        s_snapshot.service_count = 0;
        s_snapshot.state = PANEL_BLE_OFF;
        s_snapshot.reason[0] = '\0';
        clear_teardown_deadline_locked();
        unlock();
        return;
    }

    s_enabled = false;
    s_disable_pending = true;
    s_pending_connect = false;
    s_disconnect_requested = true;
    s_gatt_session++;
    s_snapshot.enabled = false;
    s_snapshot.scanning = false;
    s_snapshot.device_count = 0;
    s_snapshot.service_count = 0;
    s_snapshot.generation++;
    scanning = s_scanning;
    connecting = s_connect_inflight;
    conn = s_conn_handle;
    if (scanning) {
        s_snapshot.state = PANEL_BLE_IDLE;
        snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在停止扫描");
    } else if (connecting) {
        s_snapshot.state = PANEL_BLE_CONNECTING;
        snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在取消连接");
    } else if (conn != BLE_HS_CONN_HANDLE_NONE) {
        s_snapshot.state = PANEL_BLE_CONNECTED;
        snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在断开");
    } else {
        (void)maybe_finish_disable_locked();
    }
    unlock();

    if (scanning) {
        int rc = ble_gap_disc_cancel();
        (void)apply_scan_cancel_result(rc, false);
    }

    lock();
    if (s_fault) {
        unlock();
        return;
    }
    connecting = s_connect_inflight;
    if (connecting) {
        s_connect_cancel_requested = true;
        arm_teardown_deadline_locked();
        snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在取消连接");
        unlock();
        int rc = ble_gap_conn_cancel();
        handle_conn_cancel_result(rc);
    } else {
        unlock();
    }

    lock();
    if (s_fault) {
        unlock();
        return;
    }
    conn = s_conn_handle;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        arm_teardown_deadline_locked();
        snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在断开");
        unlock();
        int rc = ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        handle_terminate_result(rc);
    } else {
        (void)maybe_finish_disable_locked();
        unlock();
    }
}

static esp_err_t enable_ble(void)
{
    lock();
    if (s_fault) {
        unlock();
        ESP_LOGW(TAG, "BLE enable rejected: host is faulted; restart required");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_enabled && !s_disable_pending) {
        unlock();
        return ESP_OK;
    }
    if (s_host_inited) {
        if (!s_host_ready) {
            unlock();
            return ESP_ERR_INVALID_STATE;
        }
        s_enabled = true;
        s_disable_pending = false;
        s_snapshot.enabled = true;
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            s_snapshot.state = PANEL_BLE_CONNECTED;
            snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在断开");
        } else if (s_connect_inflight) {
            s_snapshot.state = PANEL_BLE_CONNECTING;
            snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在取消连接");
        } else if (s_scanning) {
            s_snapshot.state = PANEL_BLE_SCANNING;
            s_snapshot.reason[0] = '\0';
        } else {
            s_disconnect_requested = false;
            s_connect_cancel_requested = false;
            s_snapshot.state = PANEL_BLE_IDLE;
            s_snapshot.reason[0] = '\0';
        }
        unlock();
        return ESP_OK;
    }
    unlock();

    int hosted_rc = esp_hosted_connect_to_slave();
    if (hosted_rc != 0) {
        ESP_LOGE(TAG, "esp_hosted_connect_to_slave failed: %d", hosted_rc);
        lock();
        fault_locked("C6 Hosted 连接失败，请重启");
        unlock();
        return ESP_FAIL;
    }

    esp_hosted_coprocessor_fwver_t fw;
    memset(&fw, 0, sizeof(fw));
    bool version_ok = esp_hosted_get_coprocessor_fwversion(&fw) == 0;
    bool legacy = fw_requires_legacy(version_ok, &fw);

    if (!legacy) {
        esp_err_t err = esp_hosted_bt_controller_init();
        if (err == ESP_OK) {
            err = esp_hosted_bt_controller_enable();
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "C6 Hosted BT init/enable failed: %s", esp_err_to_name(err));
            lock();
            fault_locked("C6 Hosted BT 初始化失败，请重启");
            unlock();
            return err;
        }
    } else {
        ESP_LOGI(TAG, "legacy Hosted BT slave path (fw=%s%lu.%lu.%lu)",
                 version_ok ? "" : "unreported/",
                 (unsigned long)fw.major1, (unsigned long)fw.minor1,
                 (unsigned long)fw.patch1);
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        lock();
        fault_locked("NimBLE 初始化失败，请重启");
        unlock();
        return err;
    }
    /* Official VHCI example sets callbacks after nimble_port_init(). */
    ble_hs_cfg.sync_cb = host_sync;
    ble_hs_cfg.reset_cb = host_reset;

    lock();
    s_host_inited = true;
    s_host_ready = false;
    s_enabled = true;
    s_disable_pending = false;
    s_disconnect_requested = false;
    s_connect_cancel_requested = false;
    s_snapshot.enabled = true;
    s_snapshot.state = PANEL_BLE_CONNECTING;
    snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在初始化 BLE");
    unlock();

    (void)xSemaphoreTake(s_sync_sem, 0);
    nimble_port_freertos_init(host_task);
    if (!wait_hci_sync(BLE_HOST_SYNC_TIMEOUT_MS)) {
        lock();
        bool already_faulted = s_fault;
        if (!already_faulted) {
            fault_locked("BLE HCI 同步超时，请重启");
        }
        unlock();
        if (already_faulted) {
            ESP_LOGE(TAG, "BLE host faulted before HCI sync completed (legacy=%d)",
                     legacy);
        } else {
            ESP_LOGE(TAG, "BLE HCI sync timeout (legacy=%d)", legacy);
        }
        return already_faulted ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }

    lock();
    if (s_fault) {
        unlock();
        return ESP_FAIL;
    }
    if (!s_host_ready) {
        fault_locked("BLE HCI 同步失败，请重启");
        unlock();
        return ESP_FAIL;
    }
    s_snapshot.state = PANEL_BLE_IDLE;
    s_snapshot.reason[0] = '\0';
    unlock();
    return ESP_OK;
}

static void cmd_connect(const ble_cmd_t *cmd)
{
    if (cmd == NULL) {
        return;
    }

    lock();
    if (s_fault || !s_enabled || s_disable_pending || !s_host_ready) {
        unlock();
        return;
    }
    if (s_connect_inflight) {
        snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在连接");
        unlock();
        return;
    }
    memcpy(s_pending_addr, cmd->addr, sizeof(s_pending_addr));
    s_pending_type = cmd->addr_type;
    s_pending_connect = true;
    bool scanning = s_scanning;
    uint16_t conn = s_conn_handle;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        s_snapshot.state = PANEL_BLE_CONNECTED;
        snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在断开旧连接");
        arm_teardown_deadline_locked();
    }
    unlock();

    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        int rc = ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        if (rc == BLE_HS_ENOTCONN) {
            lock();
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            clear_teardown_deadline_locked();
            s_gatt_session++;
            s_disconnect_requested = false;
            unlock();
            start_connect();
        } else if (rc != 0) {
            lock();
            s_pending_connect = false;
            clear_teardown_deadline_locked();
            s_snapshot.state = PANEL_BLE_FAILED;
            snprintf(s_snapshot.reason, sizeof(s_snapshot.reason),
                     "断开旧连接失败 %d", rc);
            unlock();
        }
        return;
    }

    if (scanning) {
        int rc = ble_gap_disc_cancel();
        if (!apply_scan_cancel_result(rc, false)) {
            lock();
            s_pending_connect = false;
            unlock();
            return;
        }
    }
    start_connect();
}

static void cmd_disconnect(void)
{
    lock();
    if (s_fault) {
        unlock();
        return;
    }
    s_pending_connect = false;
    s_disconnect_requested = true;
    s_gatt_session++;
    bool connecting = s_connect_inflight;
    uint16_t conn = s_conn_handle;
    if (connecting) {
        s_connect_cancel_requested = true;
        s_snapshot.state = PANEL_BLE_CONNECTING;
        snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在取消连接");
        arm_teardown_deadline_locked();
    } else if (conn != BLE_HS_CONN_HANDLE_NONE) {
        s_snapshot.state = PANEL_BLE_CONNECTED;
        snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在断开");
        arm_teardown_deadline_locked();
    } else if (s_scanning) {
        s_snapshot.state = PANEL_BLE_SCANNING;
    } else if (s_disable_pending) {
        (void)maybe_finish_disable_locked();
    } else {
        s_snapshot.state = s_enabled ? PANEL_BLE_IDLE : PANEL_BLE_OFF;
        s_snapshot.service_count = 0;
        s_snapshot.reason[0] = '\0';
    }
    unlock();

    if (connecting) {
        int rc = ble_gap_conn_cancel();
        handle_conn_cancel_result(rc);
    } else if (conn != BLE_HS_CONN_HANDLE_NONE) {
        int rc = ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        handle_terminate_result(rc);
    }
}

static void host_sync(void)
{
    uint8_t own_addr_type = 0;
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &own_addr_type);
    }

    lock();
    if (s_fault || !s_host_inited) {
        unlock();
        signal_sync_waiter();
        return;
    }
    if (rc != 0) {
        fault_locked("BLE 地址初始化失败，请重启");
        unlock();
        signal_sync_waiter();
        return;
    }
    s_own_addr_type = own_addr_type;
    s_host_ready = true;
    if (s_enabled &&
        (s_snapshot.state == PANEL_BLE_IDLE ||
         s_snapshot.state == PANEL_BLE_CONNECTING)) {
        s_snapshot.state = PANEL_BLE_IDLE;
        s_snapshot.reason[0] = '\0';
    }
    unlock();
    signal_sync_waiter();
}

static void host_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset: %d", reason);
    lock();
    char text[64];
    snprintf(text, sizeof(text), "BLE host 重置(%d)，请重启", reason);
    fault_locked(text);
    unlock();
    signal_sync_waiter();
}

static int ble_gatt_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg)
{
    uint32_t session = (uint32_t)(uintptr_t)arg;
    lock();
    if (s_fault || !s_enabled || conn_handle != s_conn_handle ||
        session != s_gatt_session ||
        s_snapshot.state != PANEL_BLE_CONNECTED) {
        unlock();
        return 0;
    }
    if (service != NULL) {
        s_snapshot.service_count++;
    }
    unlock();
    (void)error;
    return 0;
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    uint32_t session_arg = (uint32_t)(uintptr_t)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *disc = &event->disc;
        panel_ble_device_t dev;
        memset(&dev, 0, sizeof(dev));
        dev.rssi = disc->rssi;
        dev.addr_type = disc->addr.type;
        memcpy(dev.addr, disc->addr.val, 6);
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0 &&
            fields.name != NULL && fields.name_len > 0) {
            size_t n = fields.name_len;
            if (n > PANEL_BLE_NAME_MAX) n = PANEL_BLE_NAME_MAX;
            memcpy(dev.name, fields.name, n);
        } else {
            snprintf(dev.name, sizeof(dev.name), "(匿名)");
        }
        lock();
        if (!s_fault && s_scanning && session_arg == s_scan_session) {
            panel_ble_scan_add(&s_snapshot, s_snapshot.scan_generation, &dev);
        }
        unlock();
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        lock();
        if (session_arg != s_scan_session || !s_scanning) {
            unlock();
            return 0;
        }
        s_scanning = false;
        s_scan_session++;
        panel_ble_scan_finish(&s_snapshot, s_snapshot.scan_generation);
        s_snapshot.scanning = false;
        if (s_disable_pending) {
            (void)maybe_finish_disable_locked();
        } else if (s_enabled) {
            s_snapshot.state = PANEL_BLE_IDLE;
            s_snapshot.reason[0] = '\0';
        } else {
            s_snapshot.state = PANEL_BLE_OFF;
        }
        bool pending = s_pending_connect && s_enabled && !s_disable_pending;
        unlock();
        if (pending) {
            ble_cmd_t cmd = {.type = BLE_CMD_START_PENDING};
            if (!post(&cmd)) {
                lock();
                s_pending_connect = false;
                snprintf(s_snapshot.reason, sizeof(s_snapshot.reason),
                         "连接命令队列忙");
                unlock();
            }
        }
        return 0;
    case BLE_GAP_EVENT_CONNECT: {
        uint16_t conn_handle = event->connect.conn_handle;
        int status = event->connect.status;
        lock();
        if (session_arg != s_connect_session || !s_connect_inflight) {
            unlock();
            if (status == 0 && conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                if (rc != 0) {
                    ESP_LOGW(TAG, "terminate stale connect failed: %d", rc);
                }
            }
            return 0;
        }
        bool cancelled = s_connect_cancel_requested;
        s_connect_inflight = false;
        s_connect_cancel_requested = false;
        clear_teardown_deadline_locked();
        s_gatt_session++;
        if (status != 0) {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_disconnect_requested = false;
            if (s_disable_pending) {
                (void)maybe_finish_disable_locked();
            } else if (!s_enabled) {
                s_snapshot.state = PANEL_BLE_OFF;
            } else if (cancelled) {
                s_snapshot.state = PANEL_BLE_IDLE;
                snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "已取消连接");
            } else {
                s_snapshot.state = PANEL_BLE_FAILED;
                snprintf(s_snapshot.reason, sizeof(s_snapshot.reason),
                         "连接失败 %d", status);
            }
            unlock();
            return 0;
        }

        s_conn_handle = conn_handle;
        s_snapshot.service_count = 0;
        if (s_disable_pending || !s_enabled || cancelled ||
            s_disconnect_requested) {
            s_snapshot.state = PANEL_BLE_CONNECTED;
            snprintf(s_snapshot.reason, sizeof(s_snapshot.reason), "正在断开");
            arm_teardown_deadline_locked();
            unlock();
            int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            if (rc != 0) {
                handle_terminate_result(rc);
            }
            return 0;
        }

        s_snapshot.state = PANEL_BLE_CONNECTED;
        s_snapshot.reason[0] = '\0';
        uint32_t session = s_gatt_session;
        unlock();
        int rc = ble_gattc_disc_all_svcs(conn_handle, ble_gatt_svc_cb,
                                         (void *)(uintptr_t)session);
        if (rc != 0) {
            ESP_LOGW(TAG, "GATT service discovery failed: %d", rc);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t conn_handle = event->disconnect.conn.conn_handle;
        lock();
        if (conn_handle != s_conn_handle || session_arg != s_connect_session) {
            unlock();
            return 0;
        }
        bool was_enabled = s_enabled;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        clear_teardown_deadline_locked();
        s_gatt_session++;
        s_snapshot.service_count = 0;
        s_snapshot.connected_name[0] = '\0';
        s_disconnect_requested = false;
        if (s_disable_pending) {
            (void)maybe_finish_disable_locked();
        } else if (was_enabled) {
            s_snapshot.state = PANEL_BLE_IDLE;
            snprintf(s_snapshot.reason, sizeof(s_snapshot.reason),
                     "已断开 %d", event->disconnect.reason);
        } else {
            s_snapshot.state = PANEL_BLE_OFF;
        }
        bool pending = s_pending_connect && s_enabled && !s_disable_pending;
        unlock();
        if (pending) {
            ble_cmd_t cmd = {.type = BLE_CMD_START_PENDING};
            if (!post(&cmd)) {
                lock();
                s_pending_connect = false;
                snprintf(s_snapshot.reason, sizeof(s_snapshot.reason),
                         "连接命令队列忙");
                unlock();
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

static void worker(void *arg)
{
    (void)arg;
    ble_cmd_t cmd;
    for (;;) {
        check_teardown_deadline();
        if (xQueueReceive(s_queue, &cmd, pdMS_TO_TICKS(BLE_WORKER_POLL_MS)) != pdTRUE) {
            continue;
        }
        check_teardown_deadline();
        switch (cmd.type) {
        case BLE_CMD_ENABLE: {
            esp_err_t err = enable_ble();
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "BLE enable failed: %s", esp_err_to_name(err));
            }
            break;
        }
        case BLE_CMD_DISABLE: disable_ble(); break;
        case BLE_CMD_SCAN: start_scan(); break;
        case BLE_CMD_STOP: stop_scan(); break;
        case BLE_CMD_CONNECT: cmd_connect(&cmd); break;
        case BLE_CMD_DISCONNECT: cmd_disconnect(); break;
        case BLE_CMD_START_PENDING: start_connect(); break;
        default: break;
        }
        check_teardown_deadline();
        memset(&cmd, 0, sizeof(cmd));
    }
}

esp_err_t panel_network_ble_init(void)
{
    if (s_worker_started) {
        return ESP_OK;
    }
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutex();
    if (s_sync_sem == NULL) s_sync_sem = xSemaphoreCreateBinary();
    if (s_queue == NULL) s_queue = xQueueCreate(BLE_QUEUE_LEN, sizeof(ble_cmd_t));
    if (s_lock == NULL || s_sync_sem == NULL || s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lock();
    panel_ble_snapshot_reset(&s_snapshot);
    s_snapshot.state = PANEL_BLE_OFF;
    unlock();
    if (xTaskCreate(worker, "panel_ble", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_worker_started = true;
    return ESP_OK;
}

esp_err_t panel_network_ble_set_enabled(bool enabled)
{
    ble_cmd_t cmd = {.type = enabled ? BLE_CMD_ENABLE : BLE_CMD_DISABLE};
    return post(&cmd) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t panel_network_ble_scan(void)
{
    lock();
    bool can_scan = !s_fault && s_enabled && s_host_ready &&
                    !s_disable_pending && !s_scanning && !s_pending_connect &&
                    !s_connect_inflight &&
                    s_conn_handle == BLE_HS_CONN_HANDLE_NONE;
    unlock();
    if (!can_scan) {
        return ESP_ERR_INVALID_STATE;
    }
    ble_cmd_t cmd = {.type = BLE_CMD_SCAN};
    return post(&cmd) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t panel_network_ble_stop(void)
{
    ble_cmd_t cmd = {.type = BLE_CMD_STOP};
    return post(&cmd) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t panel_network_ble_connect(const uint8_t addr[6], uint8_t addr_type)
{
    if (addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    bool can_connect = !s_fault && s_enabled && s_host_ready &&
                       !s_disable_pending && !s_connect_inflight;
    unlock();
    if (!can_connect) {
        return ESP_ERR_INVALID_STATE;
    }
    ble_cmd_t cmd = {.type = BLE_CMD_CONNECT, .addr_type = addr_type};
    memcpy(cmd.addr, addr, 6);
    return post(&cmd) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t panel_network_ble_disconnect(void)
{
    ble_cmd_t cmd = {.type = BLE_CMD_DISCONNECT};
    return post(&cmd) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void panel_network_ble_get(panel_ble_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    lock();
    *out = s_snapshot;
    unlock();
}

bool panel_network_ble_connected(void)
{
    lock();
    bool connected = s_snapshot.state == PANEL_BLE_CONNECTED;
    unlock();
    return connected;
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    lock();
    if (!s_fault) {
        fault_locked("BLE host 停止，请重启");
    }
    unlock();
    signal_sync_waiter();
    nimble_port_freertos_deinit();
}

#else

esp_err_t panel_network_ble_init(void) { return ESP_OK; }

esp_err_t panel_network_ble_set_enabled(bool enabled) { (void)enabled; return ESP_ERR_INVALID_STATE; }
esp_err_t panel_network_ble_scan(void) { return ESP_ERR_INVALID_STATE; }
esp_err_t panel_network_ble_stop(void) { return ESP_ERR_INVALID_STATE; }
esp_err_t panel_network_ble_connect(const uint8_t addr[6], uint8_t addr_type)
{
    (void)addr; (void)addr_type;
    return ESP_ERR_INVALID_STATE;
}
esp_err_t panel_network_ble_disconnect(void) { return ESP_ERR_INVALID_STATE; }
void panel_network_ble_get(panel_ble_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    panel_ble_snapshot_reset(out);
    out->state = PANEL_BLE_UNAVAILABLE;
    snprintf(out->reason, sizeof(out->reason), "BT host is not enabled in this build");
}
bool panel_network_ble_connected(void) { return false; }

#endif
