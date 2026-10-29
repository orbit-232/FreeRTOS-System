#pragma once

#include <stdint.h>

struct ble_gatt_error {
    int unused;
};
struct ble_gatt_svc {
    int unused;
};

typedef int ble_gattc_svc_fn(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *service,
                             void *arg);

int ble_gattc_disc_all_svcs(uint16_t conn_handle, ble_gattc_svc_fn *cb, void *arg);
