#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef int nvs_handle_t;

#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110a

esp_err_t nvs_open(const char *namespace_name, int open_mode, nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_commit(nvs_handle_t handle);

void fake_clock_nvs_reset(void);
void fake_clock_nvs_set_raw(const void *data, size_t len);
bool fake_clock_nvs_saved(void);
size_t fake_clock_nvs_len(void);
unsigned fake_clock_nvs_writes(void);
void fake_clock_nvs_set_fail(bool fail);
