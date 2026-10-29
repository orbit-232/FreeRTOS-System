#include "panel_calendar.h"

#include <limits.h>
#include <string.h>

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int64_t era = floor_div(year, 400);
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned mp = month > 2 ? month - 3 : month + 9;
    const unsigned doy = (153U * mp + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *year, int *month, int *day)
{
    z += 719468;
    const int64_t era = floor_div(z, 146097);
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    int y = (int)yoe + (int)(era * 400);
    const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    const unsigned mp = (5U * doy + 2U) / 153U;
    const unsigned d = doy - (153U * mp + 2U) / 5U + 1U;
    const unsigned m = mp < 10U ? mp + 3U : mp - 9U;
    y += m <= 2U;
    *year = y;
    *month = (int)m;
    *day = (int)d;
}

bool panel_calendar_is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int panel_calendar_days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && panel_calendar_is_leap(year)) {
        return 29;
    }
    return days[month - 1];
}

bool panel_calendar_month_valid(int year, int month)
{
    return year >= PANEL_CALENDAR_MIN_YEAR && year <= PANEL_CALENDAR_MAX_YEAR &&
           month >= 1 && month <= 12;
}

bool panel_calendar_date_valid(const panel_date_t *date)
{
    if (date == NULL || !panel_calendar_month_valid(date->year, date->month)) {
        return false;
    }
    int days = panel_calendar_days_in_month(date->year, date->month);
    return date->day >= 1 && date->day <= days;
}

static bool gregorian_valid(int year, int month, int day)
{
    if (year < 1 || year > 9999 || month < 1 || month > 12) {
        return false;
    }
    int days = panel_calendar_days_in_month(year, month);
    return day >= 1 && day <= days;
}

int panel_calendar_weekday(int year, int month, int day)
{
    if (!gregorian_valid(year, month, day)) {
        return -1;
    }
    int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day);
    /* 1970-01-01 was Thursday; Monday=0. */
    int wd = (int)((days + 3) % 7);
    return wd < 0 ? wd + 7 : wd;
}

bool panel_calendar_add_days(const panel_date_t *date, int delta, panel_date_t *out)
{
    if (date == NULL || out == NULL || !gregorian_valid(date->year, date->month, date->day)) {
        return false;
    }
    int64_t days = days_from_civil(date->year, (unsigned)date->month, (unsigned)date->day);
    int y;
    int m;
    int d;
    civil_from_days(days + delta, &y, &m, &d);
    out->year = y;
    out->month = m;
    out->day = d;
    return true;
}

bool panel_calendar_from_epoch_cst(int64_t epoch, panel_date_t *out)
{
    if (out == NULL || epoch < 0 || epoch > INT64_MAX - 8 * 3600) {
        return false;
    }
    int64_t days = floor_div(epoch + 8 * 3600, 86400);
    int y;
    int m;
    int d;
    civil_from_days(days, &y, &m, &d);
    out->year = y;
    out->month = m;
    out->day = d;
    return panel_calendar_date_valid(out);
}

void panel_calendar_step_month(int *year, int *month, int delta)
{
    if (year == NULL || month == NULL || *month < 1 || *month > 12) {
        return;
    }
    int64_t total = (int64_t)*year * 12 + (*month - 1) + (int64_t)delta;
    int64_t y = total / 12;
    int64_t m = total % 12;
    if (m < 0) {
        m += 12;
        y--;
    }
    m++;
    if (y < PANEL_CALENDAR_MIN_YEAR) {
        y = PANEL_CALENDAR_MIN_YEAR;
        m = 1;
    } else if (y > PANEL_CALENDAR_MAX_YEAR) {
        y = PANEL_CALENDAR_MAX_YEAR;
        m = 12;
    }
    *year = (int)y;
    *month = (int)m;
}

int panel_calendar_build_month(int year, int month, const panel_date_t *today,
                               panel_calendar_cell_t cells[PANEL_CALENDAR_CELLS])
{
    if (cells == NULL || !panel_calendar_month_valid(year, month)) {
        return 0;
    }
    memset(cells, 0, sizeof(panel_calendar_cell_t) * PANEL_CALENDAR_CELLS);
    panel_date_t first = {year, month, 1};
    int leading = panel_calendar_weekday(year, month, 1);
    panel_date_t start;
    if (!panel_calendar_add_days(&first, -leading, &start)) {
        return 0;
    }
    for (int i = 0; i < PANEL_CALENDAR_CELLS; i++) {
        panel_date_t d;
        if (!panel_calendar_add_days(&start, i, &d)) {
            return 0;
        }
        cells[i].day = d.day;
        cells[i].weekday = panel_calendar_weekday(d.year, d.month, d.day);
        cells[i].in_month = d.year == year && d.month == month;
        cells[i].is_today = today != NULL && panel_calendar_date_valid(today) &&
                            d.year == today->year && d.month == today->month &&
                            d.day == today->day;
    }
    return PANEL_CALENDAR_CELLS;
}
