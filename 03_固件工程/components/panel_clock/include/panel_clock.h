#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_CLOCK_COUNTDOWN_MAX_SECONDS 359999U /* 99:59:59 */

typedef struct {
    bool valid;
    bool calibrated;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int weekday; /* Monday=0 */
    bool stopwatch_running;
    uint32_t stopwatch_ms;
    bool countdown_running;
    uint32_t countdown_remaining_ms;
    bool countdown_done;
    uint32_t countdown_done_generation;
    bool alarm_enabled;
    uint8_t alarm_hour;
    uint8_t alarm_minute;
    bool alarm_pending;
    uint32_t alarm_generation;
    bool alarm_persist_failed;
    uint32_t generation;
} panel_clock_snapshot_t;

esp_err_t panel_clock_init(void);
void panel_clock_get_snapshot(panel_clock_snapshot_t *out);

esp_err_t panel_clock_stopwatch_start(void);
esp_err_t panel_clock_stopwatch_pause(void);
esp_err_t panel_clock_stopwatch_reset(void);

esp_err_t panel_clock_countdown_start(uint32_t seconds);
esp_err_t panel_clock_countdown_pause(void);
esp_err_t panel_clock_countdown_resume(void);
esp_err_t panel_clock_countdown_reset(void);
esp_err_t panel_clock_countdown_ack(uint32_t generation);

esp_err_t panel_clock_alarm_set(bool enabled, uint8_t hour, uint8_t minute);
esp_err_t panel_clock_alarm_ack(uint32_t generation);

#ifdef __cplusplus
}
#endif
