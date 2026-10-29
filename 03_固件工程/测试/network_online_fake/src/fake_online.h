#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void fake_online_reset(void);
void fake_online_set_wifi(bool connected);
void fake_online_fire_sntp_sync(void);
int fake_online_sntp_starts(void);
int fake_online_sntp_stops(void);
int fake_online_sntp_restarts(void);
const char *fake_online_last_ntp_server(void);

void fake_online_http_set_forecast(int status, const char *content_type,
                                   const char *body, size_t len);
void fake_online_http_set_geocode(int status, const char *content_type,
                                  const char *body, size_t len);
void fake_online_http_set_error(esp_err_t error);
void fake_online_http_set_delay_ms(unsigned delay_ms);
void fake_online_http_block_next(void);
void fake_online_http_release(void);
int fake_online_http_calls(void);
const char *fake_online_last_url(void);
bool fake_online_nvs_has_city(void);

#ifdef __cplusplus
}
#endif
