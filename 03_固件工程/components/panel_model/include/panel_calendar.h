#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_CALENDAR_MIN_YEAR 1970
#define PANEL_CALENDAR_MAX_YEAR 2099
#define PANEL_CALENDAR_CELLS 42

typedef struct {
    int year;
    int month; /* 1..12 */
    int day;   /* 1..31 */
} panel_date_t;

typedef struct {
    int day;
    int weekday; /* Monday=0 .. Sunday=6 */
    bool in_month;
    bool is_today;
} panel_calendar_cell_t;

bool panel_calendar_is_leap(int year);
int panel_calendar_days_in_month(int year, int month);
bool panel_calendar_month_valid(int year, int month);
bool panel_calendar_date_valid(const panel_date_t *date);
int panel_calendar_weekday(int year, int month, int day);
bool panel_calendar_add_days(const panel_date_t *date, int delta, panel_date_t *out);
bool panel_calendar_from_epoch_cst(int64_t epoch, panel_date_t *out);
void panel_calendar_step_month(int *year, int *month, int delta);
int panel_calendar_build_month(int year, int month, const panel_date_t *today,
                               panel_calendar_cell_t cells[PANEL_CALENDAR_CELLS]);

#ifdef __cplusplus
}
#endif
