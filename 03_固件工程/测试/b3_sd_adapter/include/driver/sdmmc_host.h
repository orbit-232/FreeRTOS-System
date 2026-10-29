#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct sdmmc_card sdmmc_card_t;

typedef struct {
    bool host_initialized;
    int num_of_init_slots;
} sdmmc_host_state_t;

typedef struct {
    int slot;
    esp_err_t (*init)(void);
    esp_err_t (*deinit_p)(int slot);
    void *pwr_ctrl_handle;
} sdmmc_host_t;

typedef struct {
    int width;
    unsigned flags;
    int clk;
    int cmd;
    int d0;
    int d1;
    int d2;
    int d3;
} sdmmc_slot_config_t;

#define SDMMC_HOST_DEFAULT() ((sdmmc_host_t){0})
#define SDMMC_SLOT_CONFIG_DEFAULT() ((sdmmc_slot_config_t){0})

esp_err_t sdmmc_host_get_state(sdmmc_host_state_t *state);
esp_err_t sdmmc_host_deinit_slot(int slot);
