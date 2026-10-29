#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_theme.h"
#include "ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t requests;
    uint32_t accepted;
    uint32_t coalesced;
    uint32_t rejected;
    uint32_t completed;
    uint32_t back_requests;
    uint32_t back_disabled;
    uint32_t home_requests;
    uint32_t control_opens;
    uint32_t control_closes;
    uint32_t settings_section_backs;
    uint32_t page_creates;
    uint32_t page_deletes;
    uint32_t pending_dispatched;
    uint32_t max_history_depth;
    uint32_t transition_count;
    uint32_t last_transition_ms;
    uint32_t max_transition_ms;
    uint32_t demo_runs;
    uint32_t demo_steps;
    uint32_t bench_runs;
    uint32_t bench_state_restored;
    uint32_t verified_unlock;
    uint32_t verified_app_enter;
    uint32_t verified_app_back;
    uint32_t verified_control_open;
    uint32_t verified_control_close;
    uint32_t verified_overview_open;
    uint32_t verified_overview_close;
    uint32_t verified_home_next;
    uint32_t verified_home_prev;
    uint32_t verify_failures;
} ui_nav_stats_t;

void ui_start(void);
void ui_navigate(ui_page_t page);
void ui_navigate_force(ui_page_t page);
void ui_rebuild_current_async(void);
ui_page_t ui_current_page(void);
void ui_back(void);
void ui_home(void);
void ui_open_control(void);
void ui_close_control(void);
void ui_open_overview(void);
void ui_close_overview(void);
bool ui_overview_is_open(void);
bool ui_overview_is_animating(void);
int ui_overview_recent_count(void);
ui_page_t ui_overview_recent_at(int index);
void ui_system_bar_refresh(void);
void ui_toast(const char *text);
bool ui_show_notice(const char *title, const char *badge, const char *body);

/* Low-overhead aggregates for the later hardware baseline.  Reading is safe
 * from the UART console; no per-frame logging is performed here. */
void ui_nav_get_stats(ui_nav_stats_t *out);
void ui_nav_log_stats(void);

/* Bounded serial demo entry: cycles are clamped to 1..10 and the demo always
 * restores Home before releasing the LVGL path.  Measurement is phase B. */
bool ui_nav_demo_start(unsigned cycles);
void ui_nav_demo_stop(void);
bool ui_nav_demo_active(void);

/* Bounded benchmark: cycles clamped to 1..3; starts from boot lock or Home
 * page 1, resets deterministic Settings landing, requires animations ON, emits
 * aggregate/raw CSV after restoring Home. */
bool ui_nav_bench_start(unsigned cycles);
void ui_nav_bench_stop(void);
bool ui_nav_bench_active(void);

bool ui_nav_transition_active(void);
bool ui_nav_control_open(void);
bool ui_nav_idle(void);

#ifdef WAVESHARE_HOST_SIM
bool ui_test_is_sleeping(void);
#endif

#ifdef __cplusplus
}
#endif
