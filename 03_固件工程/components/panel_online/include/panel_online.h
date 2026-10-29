#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_ONLINE_CITY_MAX 63
#define PANEL_ONLINE_REGION_MAX 47
#define PANEL_ONLINE_COUNTRY_MAX 31
#define PANEL_ONLINE_REASON_MAX 95
#define PANEL_ONLINE_HOURS 6
#define PANEL_ONLINE_DAYS 5
#define PANEL_ONLINE_SEARCH_RESULTS 6

typedef enum {
    PANEL_WEATHER_UNKNOWN = 0,
    PANEL_WEATHER_CLEAR,
    PANEL_WEATHER_PARTLY,
    PANEL_WEATHER_CLOUDY,
    PANEL_WEATHER_FOG,
    PANEL_WEATHER_DRIZZLE,
    PANEL_WEATHER_RAIN,
    PANEL_WEATHER_SNOW,
    PANEL_WEATHER_THUNDER,
} panel_weather_kind_t;

typedef struct {
    int hour;          /* local hour 0..23 */
    int day_offset;    /* 0 today, 1 tomorrow, ... */
    int temp_c;
    panel_weather_kind_t kind;
} panel_online_hour_t;

typedef struct {
    int day_offset;
    int weekday;       /* 0 Sunday .. 6 Saturday */
    int temp_max_c;
    int temp_min_c;
    panel_weather_kind_t kind;
} panel_online_day_t;

typedef struct {
    bool has_data;
    bool calibrated;
    bool wifi_connected;
    bool refreshing;
    bool stale;
    int64_t updated_epoch;       /* fetch time, real UTC epoch; 0 if none */
    int64_t data_epoch;          /* current.time from the response; 0 if none */
    char city[PANEL_ONLINE_CITY_MAX + 1];
    char region[PANEL_ONLINE_REGION_MAX + 1];
    char country[PANEL_ONLINE_COUNTRY_MAX + 1];
    double latitude;
    double longitude;
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
    char reason[PANEL_ONLINE_REASON_MAX + 1];
    uint32_t generation;
} panel_weather_snapshot_t;

typedef struct {
    char name[PANEL_ONLINE_CITY_MAX + 1];
    char region[PANEL_ONLINE_REGION_MAX + 1];
    char country[PANEL_ONLINE_COUNTRY_MAX + 1];
    double latitude;
    double longitude;
} panel_city_result_t;

typedef struct {
    bool searching;
    unsigned count;
    panel_city_result_t results[PANEL_ONLINE_SEARCH_RESULTS];
    char error[PANEL_ONLINE_REASON_MAX + 1];
    uint32_t generation;
} panel_city_search_snapshot_t;

/* Start the single online worker.  Idempotent after success. */
esp_err_t panel_online_init(void);

/* Thread-safe copies; UI never blocks on network/JSON/NVS. */
void panel_online_get_weather(panel_weather_snapshot_t *out);
void panel_online_get_city_search(panel_city_search_snapshot_t *out);
bool panel_online_time_is_calibrated(void);

/* Queue a manual refresh.  Returns ESP_ERR_NOT_FINISHED when throttled and
 * ESP_ERR_TIMEOUT when the command queue is full. */
esp_err_t panel_online_refresh(void);

/* Queue city search (English/pinyin, 1..48 bytes) and select a result by
 * index.  Selection carries the search generation so a stale UI row cannot
 * select a different city after results changed. */
esp_err_t panel_online_search_city(const char *query);
esp_err_t panel_online_choose_city(unsigned index, uint32_t search_generation);

#ifdef __cplusplus
}
#endif
