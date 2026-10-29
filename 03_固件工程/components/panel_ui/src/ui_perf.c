#include "ui_perf.h"

#include <stdio.h>
#include <string.h>

#include "../../../config/version.h"
#include "../fonts/ui_font_meta.h"
#include "panel_services.h"

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "panel_ppa_guard.h"
#endif
#if defined(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS) && CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
#include "esp_lv_adapter.h"
#endif

#if defined(CONFIG_LV_USE_FREERTOS_TASK_NOTIFY)
#define UI_LV_SYNC_TASK_NOTIFY (CONFIG_LV_USE_FREERTOS_TASK_NOTIFY ? 1 : 0)
#elif defined(LV_USE_FREERTOS_TASK_NOTIFY)
#define UI_LV_SYNC_TASK_NOTIFY (LV_USE_FREERTOS_TASK_NOTIFY ? 1 : 0)
#else
#define UI_LV_SYNC_TASK_NOTIFY 0
#endif
#define UI_LV_SYNC_NAME (UI_LV_SYNC_TASK_NOTIFY ? "task_notify" : "semaphore")

#if defined(ESP_PLATFORM)
#define PPA_GUARD_ACTIVE PANEL_PPA_GUARD_ENABLED
#define PPA_GUARD_HMAX PANEL_PPA_GUARD_MAX_BLOCK_H
#define PPA_GUARD_HITS panel_ppa_guard_fallback_count()
#else
#define PPA_GUARD_ACTIVE 0
#define PPA_GUARD_HMAX 0
#define PPA_GUARD_HITS 0
#endif

#if defined(CONFIG_LV_DRAW_SW_SHADOW_CACHE_SIZE)
#define UI_SHADOW_CACHE_SIZE CONFIG_LV_DRAW_SW_SHADOW_CACHE_SIZE
#elif defined(LV_DRAW_SW_SHADOW_CACHE_SIZE)
#define UI_SHADOW_CACHE_SIZE LV_DRAW_SW_SHADOW_CACHE_SIZE
#else
#define UI_SHADOW_CACHE_SIZE 0
#endif

static ui_perf_action_t s_actions[UI_PERF_MAX_ACTIONS];
static unsigned s_action_count;
static int s_current = -1;
static bool s_bench_ready;
static bool s_emitted;
static bool s_measurement_active;
static bool s_await_final;
static bool s_render_open;
static bool s_flush_open;
static bool s_wait_open;
static bool s_has_last_frame;
static bool s_final_render_open;
static uint64_t s_render_start_abs;
static uint64_t s_flush_start_abs;
static uint64_t s_wait_start_abs;
static uint64_t s_last_frame_abs;
static uint64_t s_final_render_start_abs;
static uint32_t s_cycle_flush_sum_us;
static uint32_t s_cycle_wait_sum_us;
static uint16_t s_cycle_flush_calls;
static uint16_t s_cycle_wait_calls;
static uint32_t s_action_overflow;
static unsigned s_cycles;
static ui_page_t s_start_page;
static panel_settings_t s_settings_before;
static uint32_t s_before_heap_free;
static uint32_t s_before_heap_min;
static uint32_t s_before_psram_free;
static uint32_t s_after_heap_free;
static uint32_t s_after_heap_min;
static uint32_t s_after_psram_free;
static uint32_t s_before_intdma_free;
static uint32_t s_before_intdma_min;
static uint32_t s_before_intdma_largest;
static uint32_t s_after_intdma_free;
static uint32_t s_after_intdma_min;
static uint32_t s_after_intdma_largest;
static bool s_have_after;
static uint32_t s_refresh_ms;
static uint32_t s_sort_buffer[UI_PERF_MAX_ACTIONS * UI_PERF_MAX_FRAMES];

static uint64_t perf_now_us(void)
{
#if defined(ESP_PLATFORM)
    return (uint64_t)esp_timer_get_time();
#else
    return (uint64_t)lv_tick_get() * 1000ULL;
#endif
}

static uint32_t elapsed_u32(uint64_t now, uint64_t start)
{
    uint64_t delta = now - start;
    return delta > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)delta;
}

static void read_intdma(uint32_t *free_bytes, uint32_t *min_bytes, uint32_t *largest)
{
#if defined(ESP_PLATFORM)
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    *free_bytes = (uint32_t)heap_caps_get_free_size(caps);
    *min_bytes = (uint32_t)heap_caps_get_minimum_free_size(caps);
    *largest = (uint32_t)heap_caps_get_largest_free_block(caps);
#else
    *free_bytes = 0;
    *min_bytes = 0;
    *largest = 0;
#endif
}

static const char *kind_name(uint8_t kind)
{
    static const char *names[UI_PERF_ACT_KIND_COUNT] = {
        "UNLOCK", "APP_ENTER", "APP_BACK", "CONTROL_OPEN", "CONTROL_CLOSE",
        "OVERVIEW_OPEN", "OVERVIEW_CLOSE", "HOME_NEXT", "HOME_PREV",
    };
    return kind < UI_PERF_ACT_KIND_COUNT ? names[kind] : "UNKNOWN";
}

static ui_perf_action_t *current_action(void)
{
    if (s_current < 0 || (unsigned)s_current >= s_action_count) {
        return NULL;
    }
    return &s_actions[s_current];
}

static void reset_cycle_sums(void)
{
    s_cycle_flush_sum_us = 0;
    s_cycle_wait_sum_us = 0;
    s_cycle_flush_calls = 0;
    s_cycle_wait_calls = 0;
}

static void reset_open_flags(void)
{
    s_render_open = false;
    s_flush_open = false;
    s_wait_open = false;
    s_final_render_open = false;
    s_render_start_abs = 0;
    s_flush_start_abs = 0;
    s_wait_start_abs = 0;
    s_final_render_start_abs = 0;
    reset_cycle_sums();
}

static void display_event_cb(lv_event_t *event)
{
    if (!s_bench_ready || s_current < 0) {
        return;
    }
    ui_perf_action_t *action = current_action();
    if (action == NULL) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(event);
    uint64_t now = perf_now_us();
    switch (code) {
    case LV_EVENT_RENDER_START:
        if (s_measurement_active) {
            s_render_start_abs = now;
            s_render_open = true;
        }
        if (s_await_final) {
            s_final_render_start_abs = now;
            s_final_render_open = true;
        }
        break;
    case LV_EVENT_RENDER_READY:
        if (s_measurement_active) {
            if (s_render_open) {
                uint32_t render_us = elapsed_u32(now, s_render_start_abs);
                unsigned index = action->render_samples;
                if (index < UI_PERF_MAX_FRAMES) {
                    action->render_cycle_us[index] = render_us;
                    action->flush_cycle_sum_us[index] = s_cycle_flush_sum_us;
                    action->flush_cycle_calls[index] = s_cycle_flush_calls;
                    action->wait_cycle_sum_us[index] = s_cycle_wait_sum_us;
                    action->wait_cycle_calls[index] = s_cycle_wait_calls;
                    action->render_samples++;
                    if (s_cycle_flush_calls > 0) {
                        action->flush_cycles++;
                    }
                    if (s_cycle_wait_calls > 0) {
                        action->wait_cycles++;
                    }
                } else {
                    action->render_overflow = true;
                    if (s_cycle_flush_calls > 0) {
                        action->flush_overflow = true;
                    }
                    if (s_cycle_wait_calls > 0) {
                        action->wait_overflow = true;
                    }
                }
                action->flush_calls += s_cycle_flush_calls;
                action->wait_calls += s_cycle_wait_calls;
                s_render_open = false;
                reset_cycle_sums();
            }
            if (!action->first_frame_valid) {
                action->first_frame_valid = true;
                action->first_frame_us = elapsed_u32(now, action->request_abs_us);
            }
            if (s_has_last_frame) {
                if (action->frame_samples < UI_PERF_MAX_FRAMES) {
                    action->frame_interval_us[action->frame_samples++] =
                        elapsed_u32(now, s_last_frame_abs);
                } else {
                    action->frame_overflow = true;
                }
            }
            action->frames++;
            s_last_frame_abs = now;
            s_has_last_frame = true;
        }
        if (s_await_final) {
            action->final_valid = true;
            action->final_missing = false;
            action->final_render_abs_us = now;
            action->final_after_complete_us =
                elapsed_u32(now, action->complete_abs_us);
            action->final_render_cycle_us =
                s_final_render_open
                    ? elapsed_u32(now, s_final_render_start_abs)
                    : 0;
            s_final_render_open = false;
            if (!action->first_frame_valid) {
                action->first_frame_valid = true;
                action->first_frame_us = elapsed_u32(now, action->request_abs_us);
            }
            s_await_final = false;
        }
        break;
    case LV_EVENT_FLUSH_START:
        if (s_measurement_active) {
            s_flush_start_abs = now;
            s_flush_open = true;
        }
        break;
    case LV_EVENT_FLUSH_FINISH:
        if (s_measurement_active && s_flush_open) {
            uint32_t value = elapsed_u32(now, s_flush_start_abs);
            if (s_cycle_flush_calls < UINT16_MAX) {
                s_cycle_flush_calls++;
            }
            if (UINT32_MAX - s_cycle_flush_sum_us < value) {
                s_cycle_flush_sum_us = UINT32_MAX;
            } else {
                s_cycle_flush_sum_us += value;
            }
            s_flush_open = false;
        }
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        if (s_measurement_active) {
            s_wait_start_abs = now;
            s_wait_open = true;
        }
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        if (s_measurement_active && s_wait_open) {
            uint32_t value = elapsed_u32(now, s_wait_start_abs);
            if (s_cycle_wait_calls < UINT16_MAX) {
                s_cycle_wait_calls++;
            }
            if (UINT32_MAX - s_cycle_wait_sum_us < value) {
                s_cycle_wait_sum_us = UINT32_MAX;
            } else {
                s_cycle_wait_sum_us += value;
            }
            s_wait_open = false;
        }
        break;
    default:
        break;
    }
}

void ui_perf_attach(lv_display_t *disp)
{
    static bool attached;
    if (attached || disp == NULL) {
        return;
    }
    attached = true;
    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_FLUSH_FINISH, NULL);
    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_FLUSH_WAIT_START, NULL);
    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
}

void ui_perf_bench_reset(unsigned cycles, ui_page_t start_page,
                         const panel_settings_t *settings)
{
    memset(s_actions, 0, sizeof(s_actions));
    s_action_count = 0;
    s_current = -1;
    s_bench_ready = true;
    s_emitted = false;
    s_measurement_active = false;
    s_await_final = false;
    s_has_last_frame = false;
    s_last_frame_abs = 0;
    reset_open_flags();
    s_action_overflow = 0;
    s_cycles = cycles;
    s_start_page = start_page;
    s_have_after = false;
    if (settings != NULL) {
        s_settings_before = *settings;
    } else {
        panel_settings_set_defaults(&s_settings_before);
    }

    panel_runtime_t runtime;
    panel_services_get_runtime(&runtime);
    s_before_heap_free = runtime.heap_free;
    s_before_heap_min = runtime.heap_min;
    s_before_psram_free = runtime.psram_free;
    read_intdma(&s_before_intdma_free, &s_before_intdma_min, &s_before_intdma_largest);

#if defined(CONFIG_LV_DEF_REFR_PERIOD)
    s_refresh_ms = CONFIG_LV_DEF_REFR_PERIOD;
#elif defined(LV_DEF_REFR_PERIOD)
    s_refresh_ms = LV_DEF_REFR_PERIOD;
#else
    s_refresh_ms = 0;
#endif
}

void ui_perf_action_begin(ui_perf_action_kind_t kind)
{
    if (!s_bench_ready) {
        return;
    }
    if (s_current >= 0 && !s_actions[s_current].complete) {
        return;
    }
    if (s_action_count >= UI_PERF_MAX_ACTIONS) {
        s_action_overflow++;
        return;
    }
    unsigned index = s_action_count++;
    memset(&s_actions[index], 0, sizeof(s_actions[index]));
    ui_perf_action_t *action = &s_actions[index];
    action->kind = (uint8_t)kind;
    action->request_abs_us = perf_now_us();
    s_current = (int)index;
    s_measurement_active = true;
    s_await_final = false;
    s_has_last_frame = false;
    s_last_frame_abs = 0;
    reset_open_flags();
}

void ui_perf_action_stage_start(void)
{
    ui_perf_action_t *action = current_action();
    if (action == NULL || action->started || !s_bench_ready) {
        return;
    }
    action->started = true;
    action->prepare_us = elapsed_u32(perf_now_us(), action->request_abs_us);
}

void ui_perf_action_complete(void)
{
    ui_perf_action_t *action = current_action();
    if (action == NULL || action->complete || !s_bench_ready) {
        return;
    }
    if (!action->started) {
        action->started = true;
        action->prepare_us = 0;
    }
    action->complete = true;
    action->complete_abs_us = perf_now_us();
    action->complete_us = elapsed_u32(action->complete_abs_us, action->request_abs_us);
    s_measurement_active = false;
    s_await_final = true;
    s_render_open = false;
    s_flush_open = false;
    s_wait_open = false;
    s_final_render_open = false;
    s_final_render_start_abs = 0;
    reset_cycle_sums();
}

bool ui_perf_action_started(void)
{
    ui_perf_action_t *action = current_action();
    return action != NULL && action->started;
}

bool ui_perf_action_finalized(void)
{
    ui_perf_action_t *action = current_action();
    if (action == NULL) {
        return true;
    }
    if (!action->complete) {
        return false;
    }
    if (action->final_valid) {
        return true;
    }
    if (!s_await_final) {
        return true;
    }
    if (perf_now_us() - action->complete_abs_us >= UI_PERF_FINAL_RENDER_WAIT_US) {
        action->final_missing = true;
        s_await_final = false;
        s_final_render_open = false;
        return true;
    }
    return false;
}

void ui_perf_action_cancel(void)
{
    if (s_current < 0) {
        return;
    }
    ui_perf_action_t *action = current_action();
    if (action != NULL && !action->started && !action->complete) {
        s_action_count--;
    }
    s_current = -1;
    s_measurement_active = false;
    s_await_final = false;
    reset_open_flags();
}

void ui_perf_bench_stop_measurement(void)
{
    s_measurement_active = false;
    s_render_open = false;
    s_flush_open = false;
    s_wait_open = false;
    s_flush_start_abs = 0;
    s_wait_start_abs = 0;
    reset_cycle_sums();
}

static void insertion_sort(uint32_t *values, unsigned count)
{
    for (unsigned i = 1; i < count; i++) {
        uint32_t value = values[i];
        unsigned j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = value;
    }
}

static uint32_t percentile(const uint32_t *values, unsigned count, unsigned pct)
{
    if (count == 0) {
        return 0;
    }
    memcpy(s_sort_buffer, values, count * sizeof(uint32_t));
    insertion_sort(s_sort_buffer, count);
    unsigned index = (count - 1) * pct / 100;
    return s_sort_buffer[index];
}

static uint32_t action_percentile(const uint32_t *values, unsigned count, unsigned pct)
{
    if (count == 0) {
        return 0;
    }
    uint32_t local[UI_PERF_MAX_FRAMES];
    memcpy(local, values, count * sizeof(uint32_t));
    insertion_sort(local, count);
    return local[(count - 1) * pct / 100];
}

static uint32_t action_cycle_percentile(const ui_perf_action_t *action, bool flush,
                                        unsigned pct)
{
    uint32_t local[UI_PERF_MAX_FRAMES];
    unsigned count = 0;
    for (unsigned j = 0; j < action->render_samples; j++) {
        const uint16_t calls = flush ? action->flush_cycle_calls[j]
                                     : action->wait_cycle_calls[j];
        if (calls == 0) {
            continue;
        }
        local[count++] = flush ? action->flush_cycle_sum_us[j]
                               : action->wait_cycle_sum_us[j];
    }
    if (count == 0) {
        return 0;
    }
    insertion_sort(local, count);
    return local[(count - 1) * pct / 100];
}

static void gather_metric(ui_perf_snapshot_t *snapshot, unsigned metric)
{
    unsigned count = 0;
    for (unsigned i = 0; i < s_action_count; i++) {
        const ui_perf_action_t *action = &s_actions[i];
        const uint32_t *src = NULL;
        unsigned n = 0;
        switch (metric) {
        case 0:
            src = action->frame_interval_us;
            n = action->frame_samples;
            break;
        case 1:
            src = action->render_cycle_us;
            n = action->render_samples;
            break;
        case 2:
            src = action->flush_cycle_sum_us;
            n = action->flush_cycles;
            break;
        case 3:
            src = action->wait_cycle_sum_us;
            n = action->wait_cycles;
            break;
        default:
            break;
        }
        /* flush/wait sums are aligned to render indexes; if a cycle had no
         * callbacks, it is not a flush/wait sample.  Collect only the stored
         * prefix so a sparse cycle cannot be mistaken for another sample. */
        if ((metric == 2 || metric == 3) && action->render_samples > 0) {
            for (unsigned j = 0; j < action->render_samples; j++) {
                const uint16_t calls = metric == 2 ? action->flush_cycle_calls[j]
                                                   : action->wait_cycle_calls[j];
                if (calls == 0) {
                    continue;
                }
                if (count < UI_PERF_MAX_ACTIONS * UI_PERF_MAX_FRAMES) {
                    s_sort_buffer[count++] =
                        metric == 2 ? action->flush_cycle_sum_us[j]
                                    : action->wait_cycle_sum_us[j];
                }
            }
            continue;
        }
        for (unsigned j = 0; j < n && count < UI_PERF_MAX_ACTIONS * UI_PERF_MAX_FRAMES; j++) {
            s_sort_buffer[count++] = src[j];
        }
    }
    snapshot->frame_samples = (metric == 0) ? count : snapshot->frame_samples;
    snapshot->render_samples = (metric == 1) ? count : snapshot->render_samples;
    snapshot->flush_cycles = (metric == 2) ? count : snapshot->flush_cycles;
    snapshot->wait_cycles = (metric == 3) ? count : snapshot->wait_cycles;
    insertion_sort(s_sort_buffer, count);
    uint32_t p50 = count > 0 ? s_sort_buffer[(count - 1) * 50 / 100] : 0;
    uint32_t p95 = count > 0 ? s_sort_buffer[(count - 1) * 95 / 100] : 0;
    switch (metric) {
    case 0:
        snapshot->frame_p50_us = p50;
        snapshot->frame_p95_us = p95;
        break;
    case 1:
        snapshot->render_p50_us = p50;
        snapshot->render_p95_us = p95;
        break;
    case 2:
        snapshot->flush_cycle_p50_us = p50;
        snapshot->flush_cycle_p95_us = p95;
        break;
    case 3:
        snapshot->wait_cycle_p50_us = p50;
        snapshot->wait_cycle_p95_us = p95;
        break;
    default:
        break;
    }
}

static void gather_scalar(ui_perf_snapshot_t *snapshot)
{
    uint32_t first[UI_PERF_MAX_ACTIONS];
    uint32_t complete[UI_PERF_MAX_ACTIONS];
    uint32_t final[UI_PERF_MAX_ACTIONS];
    unsigned first_n = 0;
    unsigned complete_n = 0;
    unsigned final_n = 0;

    snapshot->actions = s_action_count;
    snapshot->frames = 0;
    snapshot->final_missing = 0;
    snapshot->action_overflow = s_action_overflow;
    snapshot->frame_overflow = 0;
    snapshot->render_overflow = 0;
    snapshot->flush_overflow = 0;
    snapshot->wait_overflow = 0;
    snapshot->frame_samples = 0;
    snapshot->render_samples = 0;
    snapshot->flush_cycles = 0;
    snapshot->wait_cycles = 0;
    snapshot->flush_calls = 0;
    snapshot->wait_calls = 0;

    for (unsigned i = 0; i < s_action_count; i++) {
        const ui_perf_action_t *action = &s_actions[i];
        snapshot->frames += action->frames;
        snapshot->flush_calls += action->flush_calls;
        snapshot->wait_calls += action->wait_calls;
        if (action->final_missing) {
            snapshot->final_missing++;
        }
        if (action->frame_overflow) {
            snapshot->frame_overflow++;
        }
        if (action->render_overflow) {
            snapshot->render_overflow++;
        }
        if (action->flush_overflow) {
            snapshot->flush_overflow++;
        }
        if (action->wait_overflow) {
            snapshot->wait_overflow++;
        }
        if (action->first_frame_valid) {
            first[first_n++] = action->first_frame_us;
        }
        if (action->complete) {
            complete[complete_n++] = action->complete_us;
        }
        if (action->final_valid) {
            final[final_n++] = (uint32_t)(action->final_render_abs_us -
                                          action->request_abs_us);
        }
    }

    snapshot->request_first_p50_us = percentile(first, first_n, 50);
    snapshot->request_first_p95_us = percentile(first, first_n, 95);
    snapshot->request_complete_p50_us = percentile(complete, complete_n, 50);
    snapshot->request_complete_p95_us = percentile(complete, complete_n, 95);
    snapshot->request_final_p50_us = percentile(final, final_n, 50);
    snapshot->request_final_p95_us = percentile(final, final_n, 95);
}

void ui_perf_get_snapshot(ui_perf_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    gather_scalar(out);
    gather_metric(out, 0);
    gather_metric(out, 1);
    gather_metric(out, 2);
    gather_metric(out, 3);
}

static bool settings_equal(const panel_settings_t *a, const panel_settings_t *b)
{
    return a->brightness == b->brightness &&
           a->auto_brightness == b->auto_brightness &&
           a->theme == b->theme &&
           a->animations == b->animations &&
           a->sleep_after_s == b->sleep_after_s &&
           a->do_not_disturb == b->do_not_disturb &&
           a->rotation_locked == b->rotation_locked &&
           a->notification_preview == b->notification_preview;
}

void ui_perf_bench_emit(const ui_perf_result_meta_t *meta)
{
    if (meta == NULL || s_emitted) {
        return;
    }
    s_emitted = true;
    s_measurement_active = false;

    panel_runtime_t after;
    panel_services_get_runtime(&after);
    s_after_heap_free = after.heap_free;
    s_after_heap_min = after.heap_min;
    s_after_psram_free = after.psram_free;
    read_intdma(&s_after_intdma_free, &s_after_intdma_min, &s_after_intdma_largest);
    s_have_after = true;

    panel_settings_t current_settings;
    panel_services_get_settings(&current_settings);
    bool settings_unchanged = settings_equal(&s_settings_before, &current_settings);

    ui_perf_snapshot_t snapshot;
    ui_perf_get_snapshot(&snapshot);

    int adapter_fps = -1;
    int adapter_stats = 0;
    int font_decode_supported = 0;
#if defined(CONFIG_LV_USE_FONT_COMPRESSED)
    font_decode_supported = CONFIG_LV_USE_FONT_COMPRESSED ? 1 : 0;
#elif defined(LV_USE_FONT_COMPRESSED)
    font_decode_supported = LV_USE_FONT_COMPRESSED ? 1 : 0;
#endif
#if defined(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS) && CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    adapter_stats = 1;
    uint32_t fps = 0;
    if (esp_lv_adapter_get_fps(NULL, &fps) == ESP_OK) {
        adapter_fps = (int)fps;
    }
#endif

    printf("NAVBENCH_META,schema=2,fw=%s,cycles=%u,start_page=%d,actions=%u,"
           "max_actions=%u,max_frames=%u,refresh_ms=%u,draw_units=%d,"
           "compressed=%d,font_decode_supported=%d,font_encoding=%s,"
           "font_bitmap_format=%d,lv_sync=%s,lv_sync_task_notify=%d,"
           "ppa_guard=%d,ppa_guard_hmax=%u,ppa_guard_hits=%u,"
           "shadow_cache_size=%d,resolution=1280x800,theme=%d,brightness=%u,"
           "animations=%d,sleep_s=%u,heap_free_before=%u,heap_min_before=%u,"
           "psram_free_before=%u,adapter_stats=%d,adapter_fps=%d,"
           "intdma_free_before=%u,intdma_min_before=%u,intdma_largest_before=%u\n",
           PANEL_FW_VERSION, s_cycles, (int)s_start_page, (unsigned)snapshot.actions,
           (unsigned)UI_PERF_MAX_ACTIONS, (unsigned)UI_PERF_MAX_FRAMES,
           (unsigned)s_refresh_ms,
#if defined(CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT)
           CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT,
#elif defined(LV_DRAW_SW_DRAW_UNIT_CNT)
           LV_DRAW_SW_DRAW_UNIT_CNT,
#else
           0,
#endif
           UI_FONT_ACTIVE_COMPRESSED, font_decode_supported, UI_FONT_ENCODING_NAME,
           UI_FONT_BITMAP_FORMAT, UI_LV_SYNC_NAME, UI_LV_SYNC_TASK_NOTIFY,
           PPA_GUARD_ACTIVE, (unsigned)PPA_GUARD_HMAX, (unsigned)PPA_GUARD_HITS,
           UI_SHADOW_CACHE_SIZE, (int)s_settings_before.theme,
           s_settings_before.brightness, s_settings_before.animations ? 1 : 0,
           s_settings_before.sleep_after_s, (unsigned)s_before_heap_free,
           (unsigned)s_before_heap_min, (unsigned)s_before_psram_free,
           adapter_stats, adapter_fps, (unsigned)s_before_intdma_free,
           (unsigned)s_before_intdma_min, (unsigned)s_before_intdma_largest);

    s_after_heap_free = s_have_after ? s_after_heap_free : s_before_heap_free;
    s_after_heap_min = s_have_after ? s_after_heap_min : s_before_heap_min;
    s_after_psram_free = s_have_after ? s_after_psram_free : s_before_psram_free;
    s_after_intdma_free = s_have_after ? s_after_intdma_free : s_before_intdma_free;
    s_after_intdma_min = s_have_after ? s_after_intdma_min : s_before_intdma_min;
    s_after_intdma_largest = s_have_after ? s_after_intdma_largest : s_before_intdma_largest;

    printf("NAVBENCH_AGG,schema=2,elapsed_ms=%u,frames=%u,frame_samples=%u,"
           "render_samples=%u,flush_cycles=%u,wait_cycles=%u,flush_calls=%u,"
           "wait_calls=%u,frame_p50_us=%u,frame_p95_us=%u,render_p50_us=%u,"
           "render_p95_us=%u,flush_cycle_p50_us=%u,flush_cycle_p95_us=%u,"
           "wait_cycle_p50_us=%u,wait_cycle_p95_us=%u,"
           "req_first_p50_us=%u,req_first_p95_us=%u,"
           "req_complete_p50_us=%u,req_complete_p95_us=%u,"
           "req_final_p50_us=%u,req_final_p95_us=%u,"
           "final_missing=%u,action_overflow=%u,frame_overflow=%u,"
           "render_overflow=%u,flush_overflow=%u,wait_overflow=%u,"
           "heap_free_after=%u,heap_min_after=%u,psram_free_after=%u,"
           "intdma_free_after=%u,intdma_min_after=%u,intdma_largest_after=%u,"
           "settings_unchanged=%d,state_restored=%d,verify_ok=%d,"
           "verified_unlock=%u,verified_app_enter=%u,verified_app_back=%u,"
           "verified_control_open=%u,verified_control_close=%u,"
           "verified_overview_open=%u,verified_overview_close=%u,"
           "verified_home_next=%u,verified_home_prev=%u,verify_failures=%u,"
           "restored_home=%d,complete=%d,reason=%s\n",
           (unsigned)meta->elapsed_ms, (unsigned)snapshot.frames,
           (unsigned)snapshot.frame_samples, (unsigned)snapshot.render_samples,
           (unsigned)snapshot.flush_cycles, (unsigned)snapshot.wait_cycles,
           (unsigned)snapshot.flush_calls, (unsigned)snapshot.wait_calls,
           (unsigned)snapshot.frame_p50_us, (unsigned)snapshot.frame_p95_us,
           (unsigned)snapshot.render_p50_us, (unsigned)snapshot.render_p95_us,
           (unsigned)snapshot.flush_cycle_p50_us,
           (unsigned)snapshot.flush_cycle_p95_us,
           (unsigned)snapshot.wait_cycle_p50_us,
           (unsigned)snapshot.wait_cycle_p95_us,
           (unsigned)snapshot.request_first_p50_us,
           (unsigned)snapshot.request_first_p95_us,
           (unsigned)snapshot.request_complete_p50_us,
           (unsigned)snapshot.request_complete_p95_us,
           (unsigned)snapshot.request_final_p50_us,
           (unsigned)snapshot.request_final_p95_us,
           (unsigned)snapshot.final_missing, (unsigned)snapshot.action_overflow,
           (unsigned)snapshot.frame_overflow, (unsigned)snapshot.render_overflow,
           (unsigned)snapshot.flush_overflow, (unsigned)snapshot.wait_overflow,
           (unsigned)s_after_heap_free, (unsigned)s_after_heap_min,
           (unsigned)s_after_psram_free, (unsigned)s_after_intdma_free,
           (unsigned)s_after_intdma_min, (unsigned)s_after_intdma_largest,
           settings_unchanged ? 1 : 0,
           meta->state_restored ? 1 : 0, meta->verify_ok ? 1 : 0,
           (unsigned)meta->verified_unlock, (unsigned)meta->verified_app_enter,
           (unsigned)meta->verified_app_back, (unsigned)meta->verified_control_open,
           (unsigned)meta->verified_control_close,
           (unsigned)meta->verified_overview_open,
           (unsigned)meta->verified_overview_close,
           (unsigned)meta->verified_home_next, (unsigned)meta->verified_home_prev,
           (unsigned)meta->verify_failures, meta->restored_home ? 1 : 0,
           (meta->complete && meta->verify_ok && meta->state_restored &&
            settings_unchanged && meta->restored_home) ? 1 : 0,
           meta->reason != NULL ? meta->reason : "unknown");

    for (unsigned i = 0; i < s_action_count; i++) {
        const ui_perf_action_t *action = &s_actions[i];
        uint32_t frame_p50 = action_percentile(action->frame_interval_us,
                                                action->frame_samples, 50);
        uint32_t frame_p95 = action_percentile(action->frame_interval_us,
                                                action->frame_samples, 95);
        uint32_t render_p50 = action_percentile(action->render_cycle_us,
                                                 action->render_samples, 50);
        uint32_t render_p95 = action_percentile(action->render_cycle_us,
                                                 action->render_samples, 95);
        uint32_t flush_p50 = action_cycle_percentile(action, true, 50);
        uint32_t flush_p95 = action_cycle_percentile(action, true, 95);
        uint32_t wait_p50 = action_cycle_percentile(action, false, 50);
        uint32_t wait_p95 = action_cycle_percentile(action, false, 95);
        uint32_t req_final = action->final_valid
                                 ? (uint32_t)(action->final_render_abs_us -
                                              action->request_abs_us)
                                 : 0;
        printf("NAVBENCH_ACTION,idx=%u,kind=%s,frames=%u,frame_samples=%u,"
               "render_samples=%u,flush_cycles=%u,wait_cycles=%u,flush_calls=%u,"
               "wait_calls=%u,frame_p50_us=%u,frame_p95_us=%u,"
               "render_p50_us=%u,render_p95_us=%u,flush_cycle_p50_us=%u,"
               "flush_cycle_p95_us=%u,wait_cycle_p50_us=%u,wait_cycle_p95_us=%u,"
               "req_first_us=%u,prepare_us=%u,req_complete_us=%u,"
               "final_after_us=%u,final_render_cycle_us=%u,req_final_us=%u,"
               "final_valid=%d,final_missing=%d,frame_overflow=%d,"
               "render_overflow=%d,flush_overflow=%d,wait_overflow=%d\n",
               i, kind_name(action->kind), (unsigned)action->frames,
               (unsigned)action->frame_samples, (unsigned)action->render_samples,
               (unsigned)action->flush_cycles, (unsigned)action->wait_cycles,
               (unsigned)action->flush_calls, (unsigned)action->wait_calls,
               (unsigned)frame_p50, (unsigned)frame_p95,
               (unsigned)render_p50, (unsigned)render_p95,
               (unsigned)flush_p50, (unsigned)flush_p95,
               (unsigned)wait_p50, (unsigned)wait_p95,
               action->first_frame_valid ? (unsigned)action->first_frame_us : 0,
               (unsigned)action->prepare_us, (unsigned)action->complete_us,
               (unsigned)action->final_after_complete_us,
               (unsigned)action->final_render_cycle_us, (unsigned)req_final,
               action->final_valid ? 1 : 0, action->final_missing ? 1 : 0,
               action->frame_overflow ? 1 : 0, action->render_overflow ? 1 : 0,
               action->flush_overflow ? 1 : 0, action->wait_overflow ? 1 : 0);

        for (unsigned j = 0; j < action->frame_samples; j++) {
            printf("NAVBENCH_FRAME,action=%u,kind=%s,idx=%u,interval_us=%u\n",
                   i, kind_name(action->kind), j,
                   (unsigned)action->frame_interval_us[j]);
        }
        for (unsigned j = 0; j < action->render_samples; j++) {
            printf("NAVBENCH_RENDER,action=%u,kind=%s,idx=%u,render_us=%u\n",
                   i, kind_name(action->kind), j,
                   (unsigned)action->render_cycle_us[j]);
        }
        for (unsigned j = 0; j < action->render_samples; j++) {
            if (action->flush_cycle_calls[j] == 0) {
                continue;
            }
            printf("NAVBENCH_FLUSH_CYCLE,action=%u,kind=%s,idx=%u,sum_us=%u,"
                   "calls=%u\n",
                   i, kind_name(action->kind), j,
                   (unsigned)action->flush_cycle_sum_us[j],
                   (unsigned)action->flush_cycle_calls[j]);
        }
        for (unsigned j = 0; j < action->render_samples; j++) {
            if (action->wait_cycle_calls[j] == 0) {
                continue;
            }
            printf("NAVBENCH_WAIT_CYCLE,action=%u,kind=%s,idx=%u,sum_us=%u,"
                   "calls=%u\n",
                   i, kind_name(action->kind), j,
                   (unsigned)action->wait_cycle_sum_us[j],
                   (unsigned)action->wait_cycle_calls[j]);
        }
    }

    printf("NAVBENCH_END,schema=2,reason=%s,actions=%u,restored_home=%d,"
           "state_restored=%d,settings_unchanged=%d,complete=%d\n",
           meta->reason != NULL ? meta->reason : "unknown", s_action_count,
           meta->restored_home ? 1 : 0, meta->state_restored ? 1 : 0,
           settings_unchanged ? 1 : 0,
           (meta->complete && meta->verify_ok && meta->state_restored &&
            settings_unchanged && meta->restored_home) ? 1 : 0);
    fflush(stdout);
    s_bench_ready = false;
    s_current = -1;
}
