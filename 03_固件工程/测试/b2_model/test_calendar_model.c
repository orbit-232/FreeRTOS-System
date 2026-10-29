#include "panel_calendar.h"

#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

int main(void)
{
    CHECK(panel_calendar_is_leap(1900) == false);
    CHECK(panel_calendar_is_leap(2000) == true);
    CHECK(panel_calendar_is_leap(2100) == false);
    CHECK(panel_calendar_days_in_month(2000, 2) == 29);
    CHECK(panel_calendar_days_in_month(2100, 2) == 28);
    CHECK(panel_calendar_days_in_month(1900, 2) == 28);

    panel_date_t d = {2026, 10, 24};
    CHECK(panel_calendar_date_valid(&d));
    CHECK(panel_calendar_weekday(2026, 10, 24) == 5); /* Saturday, Monday=0 */
    CHECK(panel_calendar_weekday(2000, 1, 1) == 5);
    CHECK(panel_calendar_weekday(1970, 1, 1) == 3);
    CHECK(!panel_calendar_date_valid(&(panel_date_t){2026, 2, 29}));
    CHECK(!panel_calendar_date_valid(&(panel_date_t){2100, 1, 1}));
    CHECK(!panel_calendar_date_valid(&(panel_date_t){1969, 12, 31}));

    panel_date_t out;
    CHECK(panel_calendar_add_days(&(panel_date_t){2026, 12, 31}, 1, &out));
    CHECK(out.year == 2027 && out.month == 1 && out.day == 1);
    CHECK(panel_calendar_add_days(&(panel_date_t){2026, 1, 1}, -1, &out));
    CHECK(out.year == 2025 && out.month == 12 && out.day == 31);

    int y = 2026;
    int m = 1;
    panel_calendar_step_month(&y, &m, -1);
    CHECK(y == 2025 && m == 12);
    panel_calendar_step_month(&y, &m, 12);
    CHECK(y == 2026 && m == 12);
    panel_calendar_step_month(&y, &m, 1);
    CHECK(y == 2027 && m == 1);
    y = 2099;
    m = 12;
    panel_calendar_step_month(&y, &m, 1);
    CHECK(y == 2099 && m == 12);
    y = 1970;
    m = 1;
    panel_calendar_step_month(&y, &m, -1);
    CHECK(y == 1970 && m == 1);

    panel_calendar_cell_t cells[PANEL_CALENDAR_CELLS];
    CHECK(panel_calendar_build_month(2025, 12, &(panel_date_t){2025, 12, 25}, cells) == 42);
    CHECK(cells[0].weekday == 0); /* 2025-12-01 is Monday */
    CHECK(cells[0].day == 1 && cells[0].in_month);
    CHECK(cells[30].day == 31 && cells[30].in_month);
    CHECK(cells[31].day == 1 && !cells[31].in_month); /* 2026-01-01 */
    CHECK(cells[24].is_today); /* 2025-12-25 = index 24 */
    CHECK(!cells[23].is_today);

    CHECK(panel_calendar_build_month(1970, 1, NULL, cells) == 42);
    CHECK(cells[0].day == 29 && !cells[0].in_month); /* 1969-12-29 */
    CHECK(cells[0].weekday == 0);
    CHECK(cells[3].day == 1 && cells[3].in_month);
    for (int i = 0; i < PANEL_CALENDAR_CELLS; i++) {
        CHECK(cells[i].weekday >= 0 && cells[i].weekday <= 6);
    }

    CHECK(panel_calendar_build_month(2099, 12, NULL, cells) == 42);
    int dec31 = -1;
    for (int i = 0; i < PANEL_CALENDAR_CELLS; i++) {
        CHECK(cells[i].weekday >= 0 && cells[i].weekday <= 6);
        if (cells[i].in_month && cells[i].day == 31) dec31 = i;
    }
    CHECK(dec31 >= 0);
    CHECK(cells[dec31 + 1].day == 1 && !cells[dec31 + 1].in_month); /* 2100-01-01 */
    CHECK(panel_calendar_weekday(2100, 1, 1) == 4); /* Friday */

    CHECK(panel_calendar_build_month(2026, 2, NULL, cells) == 42);
    int in_month = 0;
    int first_in_month = -1;
    for (int i = 0; i < PANEL_CALENDAR_CELLS; i++) {
        if (cells[i].in_month) {
            in_month++;
            if (first_in_month < 0) first_in_month = i;
        }
    }
    CHECK(in_month == 28);
    CHECK(first_in_month == 6);
    CHECK(cells[first_in_month].weekday == 6); /* 2026-02-01 Sunday */

    int64_t epoch = 1792837200; /* 2026-10-24 18:20 CST */
    panel_date_t cst;
    CHECK(panel_calendar_from_epoch_cst(epoch, &cst));
    CHECK(cst.year == 2026 && cst.month == 10 && cst.day == 24);
    CHECK(!panel_calendar_from_epoch_cst(INT64_MAX, &cst));
    CHECK(!panel_calendar_from_epoch_cst(INT64_MAX - 100, &cst));

    y = 2026;
    m = 6;
    panel_calendar_step_month(&y, &m, 0x7FFFFFFF);
    CHECK(y == 2099 && m == 12);
    panel_calendar_step_month(&y, &m, -0x7FFFFFFF);
    CHECK(y == 1970 && m == 1);

    printf("B2_CALENDAR_TEST failures=%d\n", failures);
    return failures ? 1 : 0;
}
