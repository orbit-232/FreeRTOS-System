#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "fake_idf.h"

typedef struct {
    volatile int hosted_connect_calls;
    volatile int fwversion_calls;
    volatile int controller_init_calls;
    volatile int controller_enable_calls;
    volatile int controller_disable_calls;
    volatile int controller_deinit_calls;
    volatile int nimble_init_calls;
    volatile int nimble_stop_calls;
    volatile int nimble_deinit_calls;
    volatile int host_task_init_calls;
    volatile int gap_disc_calls;
    volatile int disc_cancel_calls;
    volatile int gap_connect_calls;
    volatile int conn_cancel_calls;
    volatile int terminate_calls;
    volatile int gatt_disc_calls;
    volatile uint32_t last_scan_session;
    volatile uint32_t last_connect_session;
    volatile uint32_t last_gatt_session;
    volatile uint16_t last_terminate_handle;
    volatile int last_terminate_reason;
} fake_ble_stats_t;

void fake_ble_reset(void);
const fake_ble_stats_t *fake_ble_stats(void);

void fake_ble_set_hosted_connect_result(int rc);
void fake_ble_set_fwversion_result(int rc);
void fake_ble_set_fw_version(uint32_t major, uint32_t minor, uint32_t patch);
void fake_ble_set_nimble_init_result(int rc);
void fake_ble_set_controller_init_result(int rc);
void fake_ble_set_controller_enable_result(int rc);
void fake_ble_set_disc_result(int rc);
void fake_ble_set_connect_result(int rc);
void fake_ble_set_disc_cancel_result(int rc);
void fake_ble_set_conn_cancel_result(int rc);
void fake_ble_set_terminate_result(int rc);
void fake_ble_set_addr_result(int rc);

bool fake_ble_wait_flag(const volatile int *flag, int value, unsigned timeout_ms);
bool fake_ble_scan_active(void);
bool fake_ble_connect_active(void);

bool fake_ble_fire_sync(void);
bool fake_ble_fire_host_reset(int reason);
bool fake_ble_fire_scan_result(uint32_t session);
bool fake_ble_fire_scan_complete(uint32_t session);
bool fake_ble_fire_connect(int status, uint16_t handle);
bool fake_ble_fire_connect_session(uint32_t session, int status, uint16_t handle);
bool fake_ble_fire_disconnect(uint16_t handle, int reason);
bool fake_ble_fire_disconnect_session(uint32_t session, uint16_t handle, int reason);
bool fake_ble_fire_svc(uint32_t session, bool last);
void fake_ble_stop_host_task(void);
