#pragma once

#include <stdint.h>

#include "host/ble_hs.h"

typedef struct {
    uint8_t type;
    uint8_t val[6];
} ble_addr_t;

struct ble_gap_disc_params {
    uint16_t itvl;
    uint16_t window;
    uint8_t filter_policy;
    uint8_t limited;
    uint8_t passive;
};

struct ble_gap_conn_params {
    int unused;
};

struct ble_gap_disc_desc {
    uint8_t event_type;
    uint8_t length_data;
    ble_addr_t addr;
    int8_t rssi;
    const uint8_t *data;
};

struct ble_gap_event {
    uint8_t type;
    union {
        struct {
            int status;
            uint16_t conn_handle;
        } connect;
        struct ble_gap_disc_desc disc;
        struct {
            struct {
                uint16_t conn_handle;
            } conn;
            uint8_t reason;
        } disconnect;
    };
};

typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);

#define BLE_GAP_EVENT_CONNECT 0
#define BLE_GAP_EVENT_DISCONNECT 1
#define BLE_GAP_EVENT_DISC 4
#define BLE_GAP_EVENT_DISC_COMPLETE 7

#define BLE_ERR_REM_USER_CONN_TERM 0x13

int ble_gap_disc(uint8_t own_addr_type, int32_t duration_ms,
                 const struct ble_gap_disc_params *disc_params,
                 ble_gap_event_fn *cb, void *cb_arg);
int ble_gap_disc_cancel(void);
int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer_addr,
                    int32_t duration_ms,
                    const struct ble_gap_conn_params *params,
                    ble_gap_event_fn *cb, void *cb_arg);
int ble_gap_conn_cancel(void);
int ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason);
