#pragma once

#include "esp_err.h"

typedef void *sd_pwr_ctrl_handle_t;

typedef struct {
    int ldo_chan_id;
} sd_pwr_ctrl_ldo_config_t;

esp_err_t sd_pwr_ctrl_new_on_chip_ldo(const sd_pwr_ctrl_ldo_config_t *configs,
                                      sd_pwr_ctrl_handle_t *ret_drv);
esp_err_t sd_pwr_ctrl_del_on_chip_ldo(sd_pwr_ctrl_handle_t ctrl_handle);
