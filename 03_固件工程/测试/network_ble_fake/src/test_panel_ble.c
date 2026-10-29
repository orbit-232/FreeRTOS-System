#include "panel_network.h"
#include "fake_ble.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "host/ble_hs.h"

static int failures;
static const fake_ble_stats_t *g_stats;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            printf("FAIL: %s\n", #cond);                                                  \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static void wait_ms(unsigned ms)
{
    Sleep(ms);
}

static bool wait_state(panel_ble_state_t want, unsigned timeout_ms)
{
    for (unsigned elapsed = 0; elapsed <= timeout_ms; elapsed += 5) {
        panel_ble_snapshot_t snapshot;
        panel_network_ble_get(&snapshot);
        if (snapshot.state == want) {
            return true;
        }
        Sleep(5);
    }
    return false;
}

static bool reason_contains(const char *needle)
{
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    return strstr(snapshot.reason, needle) != NULL;
}

static bool setup(void)
{
    fake_ble_reset();
    g_stats = fake_ble_stats();
    if (panel_network_ble_init() != ESP_OK) {
        printf("FAIL: panel_network_ble_init\n");
        return false;
    }
    wait_ms(30);
    return true;
}

static bool enable_and_sync(void)
{
    if (panel_network_ble_set_enabled(true) != ESP_OK) {
        return false;
    }
    if (!fake_ble_wait_flag(&g_stats->host_task_init_calls, 1, 2000)) {
        return false;
    }
    if (!fake_ble_fire_sync()) {
        return false;
    }
    return wait_state(PANEL_BLE_IDLE, 1000);
}

static bool begin_connect(void)
{
    uint8_t addr[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    return panel_network_ble_connect(addr, 1) == ESP_OK;
}

static int scenario_on_off_on(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.enabled && snapshot.state == PANEL_BLE_IDLE);
    CHECK(g_stats->hosted_connect_calls == 1);
    CHECK(g_stats->nimble_init_calls == 1);
    CHECK(g_stats->host_task_init_calls == 1);

    for (int i = 0; i < 3; i++) {
        CHECK(panel_network_ble_set_enabled(false) == ESP_OK);
        CHECK(wait_state(PANEL_BLE_OFF, 1000));
        panel_network_ble_get(&snapshot);
        CHECK(!snapshot.enabled);
        CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
        CHECK(wait_state(PANEL_BLE_IDLE, 1000));
        panel_network_ble_get(&snapshot);
        CHECK(snapshot.enabled && snapshot.state == PANEL_BLE_IDLE);
    }

    CHECK(g_stats->hosted_connect_calls == 1);
    CHECK(g_stats->nimble_init_calls == 1);
    CHECK(g_stats->host_task_init_calls == 1);
    CHECK(g_stats->nimble_stop_calls == 0);
    CHECK(g_stats->nimble_deinit_calls == 0);
    CHECK(g_stats->controller_disable_calls == 0);
    return 0;
}

static int scenario_scan_cancel(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(panel_network_ble_scan() == ESP_OK);
    CHECK(wait_state(PANEL_BLE_SCANNING, 1000));
    uint32_t old_session = (uint32_t)g_stats->last_scan_session;
    CHECK(old_session != 0);
    CHECK(fake_ble_scan_active());

    CHECK(panel_network_ble_set_enabled(false) == ESP_OK);
    CHECK(wait_state(PANEL_BLE_OFF, 1000));
    CHECK(g_stats->disc_cancel_calls == 1);
    CHECK(g_stats->nimble_init_calls == 1);

    /* A late event from the cancelled scan must not revive the OFF state. */
    (void)fake_ble_fire_scan_result(old_session);
    (void)fake_ble_fire_scan_complete(old_session);
    wait_ms(50);
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_OFF);
    CHECK(snapshot.device_count == 0);

    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    CHECK(wait_state(PANEL_BLE_IDLE, 1000));
    CHECK(panel_network_ble_scan() == ESP_OK);
    CHECK(wait_state(PANEL_BLE_SCANNING, 1000));
    uint32_t new_session = (uint32_t)g_stats->last_scan_session;
    CHECK(new_session != old_session);

    /* Old-session result/completion while a newer scan is active. */
    (void)fake_ble_fire_scan_result(old_session);
    (void)fake_ble_fire_scan_complete(old_session);
    wait_ms(50);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_SCANNING);
    CHECK(snapshot.device_count == 0);

    (void)fake_ble_fire_scan_result(new_session);
    wait_ms(30);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.device_count == 1);
    (void)fake_ble_fire_scan_complete(new_session);
    CHECK(wait_state(PANEL_BLE_IDLE, 1000));
    panel_network_ble_get(&snapshot);
    CHECK(!snapshot.scanning);
    CHECK(snapshot.device_count == 1);
    CHECK(g_stats->nimble_init_calls == 1);
    return 0;
}

static int scenario_connect_cancel(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    uint32_t session = (uint32_t)g_stats->last_connect_session;
    CHECK(session != 0);
    CHECK(fake_ble_connect_active());

    CHECK(panel_network_ble_set_enabled(false) == ESP_OK);
    wait_ms(50);
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(!snapshot.enabled);
    CHECK(snapshot.state == PANEL_BLE_CONNECTING);
    CHECK(reason_contains("取消连接"));
    CHECK(g_stats->conn_cancel_calls == 1);

    CHECK(fake_ble_fire_connect_session(session, BLE_HS_EAPP,
                                        BLE_HS_CONN_HANDLE_NONE));
    CHECK(wait_state(PANEL_BLE_OFF, 1000));
    CHECK(g_stats->nimble_init_calls == 1);
    CHECK(g_stats->nimble_stop_calls == 0);
    return 0;
}

static int scenario_connected_then_disable(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    CHECK(fake_ble_fire_connect(0, 42));
    CHECK(wait_state(PANEL_BLE_CONNECTED, 1000));
    CHECK(g_stats->gatt_disc_calls == 1);

    CHECK(panel_network_ble_set_enabled(false) == ESP_OK);
    wait_ms(50);
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(!snapshot.enabled);
    CHECK(snapshot.state == PANEL_BLE_CONNECTED);
    CHECK(reason_contains("正在断开"));
    CHECK(g_stats->terminate_calls == 1);
    CHECK(g_stats->last_terminate_handle == 42);

    CHECK(fake_ble_fire_disconnect(42, 0x13));
    CHECK(wait_state(PANEL_BLE_OFF, 1000));
    /* Duplicate late disconnect must be ignored. */
    (void)fake_ble_fire_disconnect(42, 0x13);
    wait_ms(50);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_OFF);

    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    CHECK(wait_state(PANEL_BLE_IDLE, 1000));
    CHECK(g_stats->nimble_init_calls == 1);
    CHECK(g_stats->controller_disable_calls == 0);
    return 0;
}

static int scenario_sync_timeout(void)
{
    if (!setup()) return 1;
    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    CHECK(fake_ble_wait_flag(&g_stats->host_task_init_calls, 1, 2000));
    CHECK(wait_state(PANEL_BLE_UNAVAILABLE, 5000));
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(!snapshot.enabled);
    CHECK(strstr(snapshot.reason, "同步超时") != NULL);
    CHECK(g_stats->nimble_init_calls == 1);
    CHECK(g_stats->nimble_stop_calls == 0);
    CHECK(g_stats->nimble_deinit_calls == 0);

    /* Late sync after the timeout must not clear the fault or report ready. */
    CHECK(fake_ble_fire_sync());
    wait_ms(100);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(!snapshot.enabled);
    CHECK(strstr(snapshot.reason, "同步超时") != NULL);

    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    wait_ms(200);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(g_stats->hosted_connect_calls == 1);
    CHECK(g_stats->nimble_init_calls == 1);
    return 0;
}

static int scenario_host_reset(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(fake_ble_fire_host_reset(7));
    wait_ms(50);
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(!snapshot.enabled);
    CHECK(strstr(snapshot.reason, "重置") != NULL);
    CHECK(g_stats->nimble_init_calls == 1);
    CHECK(g_stats->nimble_stop_calls == 0);

    /* Late sync after reset must not resurrect the host. */
    CHECK(fake_ble_fire_sync());
    wait_ms(100);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(!snapshot.enabled);

    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    wait_ms(200);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(g_stats->hosted_connect_calls == 1);
    CHECK(g_stats->nimble_init_calls == 1);
    CHECK(g_stats->controller_disable_calls == 0);
    return 0;
}

static int scenario_hosted_failure(void)
{
    if (!setup()) return 1;
    fake_ble_set_hosted_connect_result(ESP_FAIL);
    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    CHECK(wait_state(PANEL_BLE_UNAVAILABLE, 1000));
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(strstr(snapshot.reason, "Hosted") != NULL);
    CHECK(g_stats->hosted_connect_calls == 1);
    CHECK(g_stats->nimble_init_calls == 0);

    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    wait_ms(200);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(g_stats->hosted_connect_calls == 1);
    CHECK(g_stats->nimble_init_calls == 0);
    return 0;
}

static int scenario_disable_pending_reenable(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    uint32_t session = (uint32_t)g_stats->last_connect_session;

    CHECK(panel_network_ble_set_enabled(false) == ESP_OK);
    wait_ms(50);
    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    wait_ms(50);
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.enabled);
    CHECK(snapshot.state == PANEL_BLE_CONNECTING);
    CHECK(g_stats->nimble_init_calls == 1);

    CHECK(fake_ble_fire_connect_session(session, BLE_HS_EAPP,
                                        BLE_HS_CONN_HANDLE_NONE));
    CHECK(wait_state(PANEL_BLE_IDLE, 1000));
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.enabled);

    /* Re-enable while a real termination is still in flight. */
    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    CHECK(fake_ble_fire_connect(0, 9));
    CHECK(wait_state(PANEL_BLE_CONNECTED, 1000));
    CHECK(panel_network_ble_set_enabled(false) == ESP_OK);
    wait_ms(50);
    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    wait_ms(50);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.enabled);
    CHECK(snapshot.state == PANEL_BLE_CONNECTED);

    CHECK(fake_ble_fire_disconnect(9, 0x13));
    CHECK(wait_state(PANEL_BLE_IDLE, 1000));
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.enabled);
    CHECK(g_stats->nimble_init_calls == 1);
    return 0;
}

static int scenario_late_connect_success(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    uint32_t session = (uint32_t)g_stats->last_connect_session;

    CHECK(panel_network_ble_set_enabled(false) == ESP_OK);
    wait_ms(50);
    CHECK(fake_ble_fire_connect_session(session, 0, 77));
    wait_ms(50);
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(!snapshot.enabled);
    CHECK(snapshot.state == PANEL_BLE_CONNECTED);
    CHECK(g_stats->terminate_calls >= 1);
    CHECK(g_stats->last_terminate_handle == 77);

    CHECK(fake_ble_fire_disconnect(77, 0x13));
    CHECK(wait_state(PANEL_BLE_OFF, 1000));
    CHECK(g_stats->nimble_init_calls == 1);
    return 0;
}

static int scenario_gatt_sessions(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    CHECK(fake_ble_fire_connect(0, 5));
    CHECK(wait_state(PANEL_BLE_CONNECTED, 1000));
    uint32_t first_gatt = (uint32_t)g_stats->last_gatt_session;
    CHECK(first_gatt != 0);

    (void)fake_ble_fire_svc(first_gatt, false);
    wait_ms(30);
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.service_count == 1);
    (void)fake_ble_fire_svc(first_gatt - 1, false);
    wait_ms(30);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.service_count == 1);

    CHECK(fake_ble_fire_disconnect(5, 0x13));
    CHECK(wait_state(PANEL_BLE_IDLE, 1000));
    (void)fake_ble_fire_svc(first_gatt, false);
    wait_ms(30);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.service_count == 0);

    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    CHECK(fake_ble_fire_connect(0, 6));
    CHECK(wait_state(PANEL_BLE_CONNECTED, 1000));
    uint32_t second_gatt = (uint32_t)g_stats->last_gatt_session;
    CHECK(second_gatt != first_gatt);

    (void)fake_ble_fire_svc(first_gatt, false);
    wait_ms(30);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.service_count == 0);
    (void)fake_ble_fire_svc(second_gatt, false);
    wait_ms(30);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.service_count == 1);
    CHECK(g_stats->nimble_init_calls == 1);
    return 0;
}

static int scenario_addr_failure(void)
{
    if (!setup()) return 1;
    fake_ble_set_addr_result(-1);
    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    CHECK(fake_ble_wait_flag(&g_stats->host_task_init_calls, 1, 2000));
    CHECK(fake_ble_fire_sync());
    CHECK(wait_state(PANEL_BLE_UNAVAILABLE, 1000));
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(strstr(snapshot.reason, "地址初始化失败") != NULL);
    CHECK(g_stats->nimble_init_calls == 1);

    CHECK(fake_ble_fire_sync());
    wait_ms(100);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(g_stats->nimble_init_calls == 1);
    return 0;
}

static int scenario_scan_cancel_failure(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(panel_network_ble_scan() == ESP_OK);
    CHECK(wait_state(PANEL_BLE_SCANNING, 1000));
    fake_ble_set_disc_cancel_result(ESP_FAIL);
    CHECK(panel_network_ble_set_enabled(false) == ESP_OK);
    CHECK(wait_state(PANEL_BLE_UNAVAILABLE, 1000));
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(strstr(snapshot.reason, "停止扫描失败") != NULL);
    CHECK(g_stats->nimble_init_calls == 1);

    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    wait_ms(200);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(g_stats->nimble_init_calls == 1);
    return 0;
}

static int scenario_conn_cancel_failure(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    fake_ble_set_conn_cancel_result(ESP_FAIL);
    CHECK(panel_network_ble_set_enabled(false) == ESP_OK);
    CHECK(wait_state(PANEL_BLE_UNAVAILABLE, 1000));
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(strstr(snapshot.reason, "取消连接失败") != NULL);
    CHECK(g_stats->nimble_init_calls == 1);

    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    wait_ms(200);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(g_stats->nimble_init_calls == 1);
    return 0;
}

static int scenario_init_retry_after_task_failure(void)
{
    fake_ble_reset();
    g_stats = fake_ble_stats();
    fake_set_task_create_fail(true);
    CHECK(panel_network_ble_init() == ESP_ERR_NO_MEM);
    fake_set_task_create_fail(false);
    CHECK(panel_network_ble_init() == ESP_OK);
    wait_ms(50);
    CHECK(enable_and_sync());
    CHECK(panel_network_ble_init() == ESP_OK);
    CHECK(g_stats->hosted_connect_calls == 1);
    CHECK(g_stats->nimble_init_calls == 1);
    return 0;
}

static int scenario_host_task_exit(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    wait_ms(100);
    fake_ble_stop_host_task();
    CHECK(wait_state(PANEL_BLE_UNAVAILABLE, 2000));
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(strstr(snapshot.reason, "host 停止") != NULL);
    CHECK(g_stats->nimble_init_calls == 1);
    CHECK(g_stats->nimble_stop_calls == 0);

    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    wait_ms(200);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(g_stats->nimble_init_calls == 1);
    return 0;
}

static int scenario_disconnect_session_guard(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    CHECK(fake_ble_fire_connect(0, 5));
    CHECK(wait_state(PANEL_BLE_CONNECTED, 1000));
    uint32_t first = (uint32_t)g_stats->last_connect_session;

    CHECK(fake_ble_fire_disconnect_session(first, 5, 0x13));
    CHECK(wait_state(PANEL_BLE_IDLE, 1000));

    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    CHECK(fake_ble_fire_connect(0, 5));
    CHECK(wait_state(PANEL_BLE_CONNECTED, 1000));
    uint32_t second = (uint32_t)g_stats->last_connect_session;
    CHECK(second != first);

    /* Reused handle with the old connect session must not clear the new link. */
    CHECK(fake_ble_fire_disconnect_session(first, 5, 0x13));
    wait_ms(150);
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.enabled);
    CHECK(snapshot.state == PANEL_BLE_CONNECTED);

    CHECK(fake_ble_fire_disconnect_session(second, 5, 0x13));
    CHECK(wait_state(PANEL_BLE_IDLE, 1000));
    CHECK(g_stats->nimble_init_calls == 1);
    return 0;
}

static int scenario_teardown_timeout_connect(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    uint32_t session = (uint32_t)g_stats->last_connect_session;

    CHECK(panel_network_ble_set_enabled(false) == ESP_OK);
    wait_ms(50);
    /* Re-enable must not cancel the in-flight teardown deadline. */
    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    wait_ms(50);
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.enabled);
    CHECK(snapshot.state == PANEL_BLE_CONNECTING);

    CHECK(wait_state(PANEL_BLE_UNAVAILABLE, 7000));
    panel_network_ble_get(&snapshot);
    CHECK(!snapshot.enabled);
    CHECK(strstr(snapshot.reason, "断开超时") != NULL);
    CHECK(g_stats->nimble_init_calls == 1);
    CHECK(g_stats->nimble_stop_calls == 0);
    CHECK(g_stats->nimble_deinit_calls == 0);
    CHECK(g_stats->controller_disable_calls == 0);

    /* The worker must keep consuming commands after the timeout fault. */
    for (int i = 0; i < 4; i++) {
        (void)panel_network_ble_set_enabled(false);
        (void)panel_network_ble_set_enabled(true);
    }
    wait_ms(300);
    CHECK(panel_network_ble_set_enabled(false) == ESP_OK);

    /* A late CONNECT completion must not revive the faulted host. */
    (void)fake_ble_fire_connect_session(session, BLE_HS_EAPP,
                                        BLE_HS_CONN_HANDLE_NONE);
    wait_ms(100);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(!snapshot.enabled);
    return 0;
}

static int scenario_teardown_timeout_disconnect(void)
{
    if (!setup()) return 1;
    CHECK(enable_and_sync());
    CHECK(begin_connect());
    CHECK(wait_state(PANEL_BLE_CONNECTING, 1000));
    CHECK(fake_ble_fire_connect(0, 9));
    CHECK(wait_state(PANEL_BLE_CONNECTED, 1000));
    uint32_t session = (uint32_t)g_stats->last_connect_session;

    CHECK(panel_network_ble_set_enabled(false) == ESP_OK);
    wait_ms(50);
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_CONNECTED);

    CHECK(wait_state(PANEL_BLE_UNAVAILABLE, 7000));
    panel_network_ble_get(&snapshot);
    CHECK(strstr(snapshot.reason, "断开超时") != NULL);
    CHECK(g_stats->nimble_init_calls == 1);
    CHECK(g_stats->terminate_calls == 1);

    /* The late DISCONNECT must not turn the fault into OFF/IDLE. */
    (void)fake_ble_fire_disconnect_session(session, 9, 0x13);
    wait_ms(100);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(!snapshot.enabled);
    return 0;
}

static int scenario_modern_init_failure(void)
{
    if (!setup()) return 1;
    fake_ble_set_fw_version(2, 5, 2);
    fake_ble_set_controller_init_result(ESP_FAIL);
    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    CHECK(wait_state(PANEL_BLE_UNAVAILABLE, 1000));
    panel_ble_snapshot_t snapshot;
    panel_network_ble_get(&snapshot);
    CHECK(strstr(snapshot.reason, "BT 初始化失败") != NULL);
    CHECK(g_stats->controller_init_calls == 1);
    CHECK(g_stats->controller_enable_calls == 0);
    CHECK(g_stats->nimble_init_calls == 0);
    CHECK(g_stats->controller_disable_calls == 0);

    CHECK(panel_network_ble_set_enabled(true) == ESP_OK);
    wait_ms(200);
    panel_network_ble_get(&snapshot);
    CHECK(snapshot.state == PANEL_BLE_UNAVAILABLE);
    CHECK(g_stats->controller_init_calls == 1);
    CHECK(g_stats->nimble_init_calls == 0);
    return 0;
}

typedef struct {
    const char *name;
    int (*run)(void);
} scenario_t;

int main(int argc, char **argv)
{
    static const scenario_t scenarios[] = {
        {"init_retry_after_task_failure", scenario_init_retry_after_task_failure},
        {"host_task_exit", scenario_host_task_exit},
        {"on_off_on", scenario_on_off_on},
        {"scan_cancel", scenario_scan_cancel},
        {"scan_cancel_failure", scenario_scan_cancel_failure},
        {"connect_cancel", scenario_connect_cancel},
        {"connect_cancel_failure", scenario_conn_cancel_failure},
        {"connected_then_disable", scenario_connected_then_disable},
        {"disconnect_session_guard", scenario_disconnect_session_guard},
        {"teardown_timeout_connect", scenario_teardown_timeout_connect},
        {"teardown_timeout_disconnect", scenario_teardown_timeout_disconnect},
        {"sync_timeout", scenario_sync_timeout},
        {"host_reset", scenario_host_reset},
        {"hosted_failure", scenario_hosted_failure},
        {"disable_pending_reenable", scenario_disable_pending_reenable},
        {"late_connect_success", scenario_late_connect_success},
        {"gatt_sessions", scenario_gatt_sessions},
        {"addr_failure", scenario_addr_failure},
        {"modern_init_failure", scenario_modern_init_failure},
    };

    if (argc < 2) {
        printf("usage: %s <scenario>\n", argv[0]);
        for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
            printf("  %s\n", scenarios[i].name);
        }
        return 2;
    }
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (strcmp(argv[1], scenarios[i].name) == 0) {
            int rc = scenarios[i].run();
            printf("BLE_FAKE_TEST scenario=%s failures=%d\n", argv[1], failures);
            return rc != 0 || failures != 0;
        }
    }
    printf("unknown scenario: %s\n", argv[1]);
    return 2;
}
