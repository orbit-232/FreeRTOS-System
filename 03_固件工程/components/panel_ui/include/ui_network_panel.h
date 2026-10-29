#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the real Wi-Fi/BLE sub-panel inside the Settings network section. */
lv_obj_t *ui_network_panel_create(lv_obj_t *parent);
/* Closes the password modal first. Returns true when a modal was closed. */
bool ui_network_panel_back(void);

#ifdef WAVESHARE_HOST_SIM
void ui_network_panel_test_open(const char *ssid, uint8_t authmode);
bool ui_network_panel_test_modal_open(void);
#endif

#ifdef __cplusplus
}
#endif
