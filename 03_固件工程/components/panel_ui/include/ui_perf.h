#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "panel_settings.h"
#include "ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 3 cycles x (4 app enter/4 back + control open/close + overview open/close +
 * home next/prev) + unlock = 43 actions.  48 leaves bounded headroom. */
#define UI_PERF_MAX_ACTIONS 48
#define UI_PERF_MAX_FRAMES 20
#define UI_PERF_FINAL_RENDER_WAIT_US 120000U

typedef enum {
    UI_PERF_ACT_UNLOCK = 0,
    UI_PERF_ACT_APP_ENTER,
    UI_PERF_ACT_APP_BACK,
    UI_PERF_ACT_CONTROL_OPEN,
    UI_PERF_ACT_CONTROL_CLOSE,
    UI_PERF_ACT_OVERVIEW_OPEN,
    UI_PERF_ACT_OVERVIEW_CLOSE,
    UI_PERF_ACT_HOME_NEXT,
    UI_PERF_ACT_HOME_PREV,
    UI_PERF_ACT_KIND_COUNT,
} ui_perf_action_kind_t;

typedef struct {
    uint8_t kind;
    bool started;
    bool complete;
    bool final_valid;
    bool final_missing;
    bool frame_overflow;
    bool render_overflow;
    bool flush_overflow;
    bool wait_overflow;
    bool first_frame_valid;
    uint16_t frames;
    uint16_t frame_samples;
    uint16_t render_samples;
    uint16_t flush_cycles;
    uint16_t wait_cycles;
    uint32_t flush_calls;
    uint32_t wait_calls;
    uint64_t request_abs_us;
    uint64_t complete_abs_us;
    uint64_t final_render_abs_us;
    uint32_t prepare_us;
    uint32_t first_frame_us;
    uint32_t complete_us;
    uint32_t final_after_complete_us;
    uint32_t final_render_cycle_us;
    uint32_t frame_interval_us[UI_PERF_MAX_FRAMES];
    uint32_t render_cycle_us[UI_PERF_MAX_FRAMES];
    /* Per-render-cycle sum, not per partial-flush chunk.  The 50-line partial
     * buffer may issue many flush/wait callbacks per refresh. */
    uint32_t flush_cycle_sum_us[UI_PERF_MAX_FRAMES];
    uint16_t flush_cycle_calls[UI_PERF_MAX_FRAMES];
    uint32_t wait_cycle_sum_us[UI_PERF_MAX_FRAMES];
    uint16_t wait_cycle_calls[UI_PERF_MAX_FRAMES];
} ui_perf_action_t;

typedef struct {
    uint32_t actions;
    uint32_t frames; /* active interval only; final redraw is separate */
    uint32_t frame_samples;
    uint32_t render_samples;
    uint32_t flush_cycles;
    uint32_t wait_cycles;
    uint32_t flush_calls;
    uint32_t wait_calls;
    uint32_t final_missing;
    uint32_t action_overflow;
    uint32_t frame_overflow;
    uint32_t render_overflow;
    uint32_t flush_overflow;
    uint32_t wait_overflow;
    uint32_t request_first_p50_us;
    uint32_t request_first_p95_us;
    uint32_t request_complete_p50_us;
    uint32_t request_complete_p95_us;
    uint32_t request_final_p50_us;
    uint32_t request_final_p95_us;
    uint32_t frame_p50_us;
    uint32_t frame_p95_us;
    uint32_t render_p50_us;
    uint32_t render_p95_us;
    uint32_t flush_cycle_p50_us;
    uint32_t flush_cycle_p95_us;
    uint32_t wait_cycle_p50_us;
    uint32_t wait_cycle_p95_us;
} ui_perf_snapshot_t;

typedef struct {
    const char *reason;
    bool complete;
    bool verify_ok;
    bool restored_home;
    bool state_restored;
    uint32_t elapsed_ms;
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
} ui_perf_result_meta_t;

/* Attach passive display hooks.  Call from the LVGL task after display init. */
void ui_perf_attach(lv_display_t *disp);

void ui_perf_bench_reset(unsigned cycles, ui_page_t start_page,
                         const panel_settings_t *settings);

void ui_perf_action_begin(ui_perf_action_kind_t kind);
void ui_perf_action_stage_start(void);
void ui_perf_action_complete(void);
bool ui_perf_action_started(void);
bool ui_perf_action_finalized(void);
void ui_perf_action_cancel(void);

void ui_perf_bench_stop_measurement(void);
void ui_perf_bench_emit(const ui_perf_result_meta_t *meta);
void ui_perf_get_snapshot(ui_perf_snapshot_t *out);

#ifdef __cplusplus
}
#endif
