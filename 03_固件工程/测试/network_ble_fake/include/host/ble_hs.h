#pragma once

#include <stdint.h>

#include "fake_idf.h"

#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_EALREADY 2
#define BLE_HS_ENOTCONN 7
#define BLE_HS_EAPP 9
#define BLE_HS_ETIMEOUT 13

struct ble_hs_cfg_s {
    void (*sync_cb)(void);
    void (*reset_cb)(int reason);
};

extern struct ble_hs_cfg_s ble_hs_cfg;

struct ble_hs_adv_fields {
    const uint8_t *name;
    uint8_t name_len;
};

int ble_hs_util_ensure_addr(int prefer_random);
int ble_hs_id_infer_auto(int privacy, uint8_t *out_addr_type);
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *fields,
                            const uint8_t *data, uint8_t length);
