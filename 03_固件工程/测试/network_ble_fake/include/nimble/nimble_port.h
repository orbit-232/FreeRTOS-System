#pragma once

#include "esp_err.h"

esp_err_t nimble_port_init(void);
int nimble_port_stop(void);
esp_err_t nimble_port_deinit(void);
void nimble_port_run(void);
