#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "panel_online.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_ONLINE_FORECAST_MAX 16384
#define PANEL_ONLINE_GEOCODE_MAX 8192
#define PANEL_ONLINE_CITY_QUERY_MAX 48

typedef struct {
    int64_t current_epoch;
    int current_temp_c;
    int apparent_temp_c;
    int humidity_pct;
    double wind_speed_kmh;
    int weather_code;
    panel_weather_kind_t kind;
    panel_online_hour_t hours[PANEL_ONLINE_HOURS];
    unsigned hour_count;
    panel_online_day_t days[PANEL_ONLINE_DAYS];
    unsigned day_count;
} panel_online_forecast_t;

panel_weather_kind_t panel_online_kind_from_code(int code);

esp_err_t panel_online_parse_forecast(const char *json, size_t len, int64_t now_epoch,
                                      panel_online_forecast_t *out,
                                      char *reason, size_t reason_len);
esp_err_t panel_online_parse_geocode(const char *json, size_t len,
                                     panel_city_result_t *results, unsigned max_results,
                                     unsigned *count, char *reason, size_t reason_len);

esp_err_t panel_online_build_forecast_url(double latitude, double longitude,
                                          char *out, size_t out_len);
esp_err_t panel_online_build_geocode_url(const char *query, char *out, size_t out_len);
bool panel_online_url_encode(const char *in, char *out, size_t out_len);
bool panel_online_city_valid(const panel_city_result_t *city);
bool panel_online_is_ascii_display(const char *text);

#ifdef __cplusplus
}
#endif
