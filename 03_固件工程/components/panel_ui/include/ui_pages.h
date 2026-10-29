#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Page factories. Each factory returns a new screen object (owned by LVGL).
 * In firmware the screen object is a full-size child of the persistent root. */
lv_obj_t *ui_page_lock_create(void);
lv_obj_t *ui_page_home_create(void);
lv_obj_t *ui_page_control_create(void);
lv_obj_t *ui_page_control_create_on(lv_obj_t *parent);
lv_obj_t *ui_page_settings_create(void);
lv_obj_t *ui_page_weather_create(void);
lv_obj_t *ui_page_monitor_create(void);
lv_obj_t *ui_page_calculator_create(void);
lv_obj_t *ui_page_calendar_create(void);
lv_obj_t *ui_page_clock_create(void);
lv_obj_t *ui_page_files_create(void);

/* Home pagination is driven by the global indev gesture handler. */
#define UI_HOME_PAGE_COUNT 2
void ui_page_home_swipe(int delta);
bool ui_page_home_animating(void);
int ui_page_home_current_page(void);
void ui_page_home_reset_for_benchmark(void);

/* Deterministic benchmark content setup; no effect on normal navigation. */
void ui_page_settings_reset_for_benchmark(void);

/* Bounded in-RAM state used by the Overview; dismissal resets transient state
 * only, never persistent settings or NVS. */
void ui_page_settings_state_reset(void);
void ui_page_settings_state_summary(char *buf, size_t len);
int ui_page_settings_current_section(void);
void ui_page_settings_restore_section(int section);
void ui_page_calculator_state_reset(void);
void ui_page_calculator_state_summary(char *buf, size_t len);
void ui_page_calculator_benchmark_snapshot(void);
void ui_page_calculator_benchmark_restore(void);
void ui_page_calendar_state_reset(void);
void ui_page_calendar_state_summary(char *buf, size_t len);
void ui_page_clock_ui_state_reset(void);
void ui_page_clock_state_summary(char *buf, size_t len);
void ui_page_files_ui_state_reset(void);
void ui_page_files_state_summary(char *buf, size_t len);
bool ui_page_files_back(void);

/* Settings subsection back: consumes the request only when a subsection other
 * than the landing section is open. */
bool ui_page_settings_back(void);

/* Weather city-search modal. */
bool ui_weather_panel_back(void);

#ifdef WAVESHARE_HOST_SIM
void ui_weather_panel_test_open(void);
bool ui_weather_panel_test_modal_open(void);
void ui_weather_panel_test_set_query(const char *query);
bool ui_weather_panel_test_search(void);
void ui_page_calendar_test_set_month(int year, int month);
void ui_page_calendar_test_click_outside(void);
#endif

#ifdef __cplusplus
}
#endif
