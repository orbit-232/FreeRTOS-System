#include "ui_pages.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "panel_calendar.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static bool s_initialized;
static bool s_today_valid;
static int s_view_year;
static int s_view_month;
static panel_date_t s_today;
static panel_date_t s_selected;
static lv_obj_t *s_month_label;
static lv_obj_t *s_status_pill;
static lv_timer_t *s_timer;
static lv_obj_t *s_grid;
static lv_obj_t *s_selected_label;
static lv_obj_t *s_calendar_cells[PANEL_CALENDAR_CELLS];

static void calendar_refresh_grid(void);
static void calendar_clamp_to_today(void);

static void calendar_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    panel_date_t old_today = s_today;
    bool old_valid = s_today_valid;
    calendar_clamp_to_today();
    bool changed = old_valid != s_today_valid || old_today.year != s_today.year ||
                   old_today.month != s_today.month || old_today.day != s_today.day;
    if (changed) {
        if (old_valid && s_selected.year == old_today.year &&
            s_selected.month == old_today.month && s_selected.day == old_today.day) {
            s_selected = s_today;
        }
        calendar_refresh_grid();
    }
    if (s_status_pill != NULL) {
        lv_obj_t *label = lv_obj_get_child(s_status_pill, 0);
        if (label != NULL) {
            lv_label_set_text(label, panel_services_time_is_calibrated() ? "已校时" : "未校时");
        }
    }
}

static void calendar_delete_cb(lv_event_t *event)
{
    (void)event;
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_grid = NULL;
    s_month_label = NULL;
    s_status_pill = NULL;
    s_selected_label = NULL;
    memset(s_calendar_cells, 0, sizeof(s_calendar_cells));
}

static void calendar_clamp_to_today(void)
{
    struct tm tm_now;
    bool have = panel_services_get_local_time(&tm_now);
    if (have) {
        s_today.year = tm_now.tm_year + 1900;
        s_today.month = tm_now.tm_mon + 1;
        s_today.day = tm_now.tm_mday;
    } else {
        s_today.year = 1970;
        s_today.month = 1;
        s_today.day = 1;
    }
    if (!panel_calendar_date_valid(&s_today)) {
        s_today.year = 1970;
        s_today.month = 1;
        s_today.day = 1;
    }
    s_today_valid = panel_services_time_is_calibrated();
}

static void calendar_ensure_state(void)
{
    if (s_initialized) {
        return;
    }
    calendar_clamp_to_today();
    s_selected = s_today;
    s_view_year = s_selected.year;
    s_view_month = s_selected.month;
    s_initialized = true;
}

void ui_page_calendar_state_reset(void)
{
    s_initialized = false;
}

void ui_page_calendar_state_summary(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    calendar_ensure_state();
    snprintf(buf, len, "%d年%d月%d日", s_selected.year, s_selected.month, s_selected.day);
}

static bool calendar_cell_date(int index, panel_date_t *out)
{
    if (index < 0 || index >= PANEL_CALENDAR_CELLS) {
        return false;
    }
    panel_date_t first = {s_view_year, s_view_month, 1};
    int leading = panel_calendar_weekday(s_view_year, s_view_month, 1);
    if (leading < 0) {
        return false;
    }
    return panel_calendar_add_days(&first, index - leading, out);
}

static bool calendar_date_equal(const panel_date_t *a, const panel_date_t *b)
{
    return a != NULL && b != NULL && a->year == b->year && a->month == b->month &&
           a->day == b->day;
}

static void calendar_day_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    panel_date_t date;
    if (!calendar_cell_date(index, &date)) {
        return;
    }
    /* Adjacent-month cells are rendered for the 42-cell grid, but selection is
     * limited to the UI-supported 1970..2099 range. */
    if (!panel_calendar_date_valid(&date)) {
        return;
    }
    /* Only the current month is selectable; adjacent-month cells are display
     * context.  Month navigation uses the explicit header buttons. */
    if (date.year != s_view_year || date.month != s_view_month) {
        return;
    }
    if (date.year != s_view_year || date.month != s_view_month) {
        s_view_year = date.year;
        s_view_month = date.month;
    }
    s_selected = date;
    calendar_refresh_grid();
}

static void calendar_refresh_grid(void)
{
    if (s_month_label != NULL) {
        lv_label_set_text_fmt(s_month_label, "%d年%d月", s_view_year, s_view_month);
    }
    const ui_palette_t *p = ui_palette();
    panel_calendar_cell_t cells[PANEL_CALENDAR_CELLS];
    panel_calendar_build_month(s_view_year, s_view_month, s_today_valid ? &s_today : NULL, cells);
    for (int i = 0; i < PANEL_CALENDAR_CELLS; i++) {
        lv_obj_t *cell = s_calendar_cells[i];
        if (cell == NULL) {
            continue;
        }
        panel_date_t date;
        bool have_date = calendar_cell_date(i, &date);
        bool selected = have_date && calendar_date_equal(&date, &s_selected);
        lv_obj_t *label = lv_obj_get_child(cell, 0);
        if (label != NULL) {
            lv_label_set_text_fmt(label, "%d", cells[i].day);
            lv_obj_set_style_text_color(
                label, selected ? lv_color_white() : (cells[i].in_month ? p->text : p->muted), 0);
        }
        lv_obj_set_style_bg_color(cell, selected ? p->accent : p->surface, 0);
        lv_obj_set_style_bg_opa(cell, selected ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_obj_set_style_border_color(cell, cells[i].is_today ? p->accent : p->border, 0);
        lv_obj_set_style_border_width(cell, cells[i].is_today ? 2 : 1, 0);
    }
    if (s_selected_label != NULL) {
        if (s_today_valid) {
            lv_label_set_text_fmt(s_selected_label, "选中 %d年%d月%d日 · 今天 %d年%d月%d日",
                                  s_selected.year, s_selected.month, s_selected.day,
                                  s_today.year, s_today.month, s_today.day);
        } else {
            lv_label_set_text_fmt(s_selected_label, "选中 %d年%d月%d日 · 等待校时",
                                  s_selected.year, s_selected.month, s_selected.day);
        }
    }
}

static void calendar_month_cb(lv_event_t *event)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    panel_calendar_step_month(&s_view_year, &s_view_month, delta);
    calendar_refresh_grid();
}

static void calendar_today_cb(lv_event_t *event)
{
    (void)event;
    calendar_clamp_to_today();
    s_selected = s_today;
    s_view_year = s_today.year;
    s_view_month = s_today.month;
    calendar_refresh_grid();
}

lv_obj_t *ui_page_calendar_create(void)
{
    const ui_palette_t *p = ui_palette();
    calendar_ensure_state();
    calendar_clamp_to_today();
    lv_obj_t *screen = ui_screen_create();
    if (ui_theme_effective() == PANEL_THEME_DARK) {
        ui_screen_gradient(screen, p->accent_soft, p->bg);
    } else {
        ui_screen_gradient(screen, lv_color_hex(0xF8ECEC), lv_color_hex(0xFBF8F4));
    }
    ui_status_bar(screen, false);
    lv_obj_t *header = ui_header(screen, "日历", NULL);

    lv_obj_t *year_prev = ui_button(header, 560, 8, 110, 40, "上一年", &ui_font_16,
                                    calendar_month_cb, (void *)(intptr_t)-12);
    lv_obj_t *month_prev = ui_button(header, 680, 8, 110, 40, "上个月", &ui_font_16,
                                     calendar_month_cb, (void *)(intptr_t)-1);
    ui_button(header, 800, 8, 110, 40, "今天", &ui_font_16, calendar_today_cb, NULL);
    lv_obj_t *month_next = ui_button(header, 920, 8, 110, 40, "下个月", &ui_font_16,
                                     calendar_month_cb, (void *)(intptr_t)1);
    lv_obj_t *year_next = ui_button(header, 1040, 8, 110, 40, "下一年", &ui_font_16,
                                    calendar_month_cb, (void *)(intptr_t)12);
    (void)year_prev;
    (void)month_prev;
    (void)month_next;
    (void)year_next;

    s_month_label = ui_label_at(screen, 40, 132, "", &ui_font_32, p->text);
    s_status_pill = ui_pill(screen, panel_services_time_is_calibrated() ? "已校时" : "未校时",
                            p->accent, p->accent_soft, &ui_font_16);
    lv_obj_set_pos(s_status_pill, 300, 140);

    static const char *weekdays[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    for (int i = 0; i < 7; i++) {
        lv_obj_t *label = ui_label_at(screen, 40 + i * 165, 180, weekdays[i], &ui_font_16,
                                      p->muted);
        lv_obj_set_width(label, 150);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }

    s_grid = lv_obj_create(screen);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_grid, 40, 218);
    lv_obj_set_size(s_grid, 1160, 420);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    for (int i = 0; i < PANEL_CALENDAR_CELLS; i++) {
        int row = i / 7;
        int col = i % 7;
        lv_obj_t *cell = ui_button(s_grid, col * 165, row * 70, 150, 62, "", &ui_font_20,
                                   calendar_day_cb, (void *)(intptr_t)i);
        lv_obj_set_style_radius(cell, 14, 0);
        lv_obj_set_style_bg_color(cell, p->surface, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_60, 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, p->border, 0);
        lv_obj_t *label = lv_obj_get_child(cell, 0);
        if (label != NULL) {
            lv_obj_set_style_text_font(label, &ui_font_20, 0);
        }
        s_calendar_cells[i] = cell;
    }
    s_selected_label = ui_label_at(screen, 40, 658, "", &ui_font_16, p->muted);
    lv_obj_set_width(s_selected_label, 1160);
    ui_nav_bar(screen, UI_PAGE_CALENDAR);
    lv_obj_add_event_cb(screen, calendar_delete_cb, LV_EVENT_DELETE, NULL);
    s_timer = lv_timer_create(calendar_timer_cb, 1000, NULL);
    calendar_refresh_grid();
    return screen;
}

#ifdef WAVESHARE_HOST_SIM
void ui_page_calendar_test_set_month(int year, int month)
{
    if (!panel_calendar_month_valid(year, month)) {
        return;
    }
    s_view_year = year;
    s_view_month = month;
    calendar_refresh_grid();
}

void ui_page_calendar_test_click_outside(void)
{
    panel_calendar_cell_t cells[PANEL_CALENDAR_CELLS];
    if (panel_calendar_build_month(s_view_year, s_view_month, &s_today, cells) !=
        PANEL_CALENDAR_CELLS) {
        return;
    }
    for (int i = 0; i < PANEL_CALENDAR_CELLS; i++) {
        if (!cells[i].in_month && s_calendar_cells[i] != NULL) {
            lv_obj_send_event(s_calendar_cells[i], LV_EVENT_CLICKED, NULL);
            return;
        }
    }
}
#endif
