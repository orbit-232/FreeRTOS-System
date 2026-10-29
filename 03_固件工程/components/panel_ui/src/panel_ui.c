#include "ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "panel_bsp.h"
#include "panel_music.h"
#include "panel_services.h"
#include "ui_diag.h"
#include "ui_gesture.h"
#include "ui_nav.h"
#include "ui_network_panel.h"
#include "ui_pages.h"
#include "ui_perf.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "panel_ui";

#define TRANSITION_LOG_LIMIT 8
#define DEMO_MAX_CYCLES 10
#define BENCH_MAX_CYCLES 3
#define DEMO_TIMEOUT_MS 60000
#define WORKLOAD_FINISH_TIMEOUT_MS 5000
#define DEMO_TIMER_MS 80
#define NOTICE_FADE_MS 140
#define SECTION_FADE_MS 140
#define OVERVIEW_FADE_MS 200
#define MRU_CAP 4

typedef enum {
    DEMO_STEP_RESTORE = 0,
    DEMO_STEP_SETTINGS,
    DEMO_STEP_BACK_SETTINGS,
    DEMO_STEP_WEATHER,
    DEMO_STEP_BACK_WEATHER,
    DEMO_STEP_MONITOR,
    DEMO_STEP_BACK_MONITOR,
    DEMO_STEP_CALCULATOR,
    DEMO_STEP_BACK_CALCULATOR,
    DEMO_STEP_CONTROL_OPEN,
    DEMO_STEP_CONTROL_CLOSE,
    DEMO_STEP_OVERVIEW_OPEN,
    DEMO_STEP_OVERVIEW_CLOSE,
    DEMO_STEP_HOME_NEXT,
    DEMO_STEP_HOME_PREV,
    DEMO_STEP_END_CYCLE,
    DEMO_STEP_COUNT,
} demo_step_t;

static lv_obj_t *s_root;
static lv_obj_t *s_active_page;
static lv_obj_t *s_outgoing_page;
static ui_nav_model_t s_nav;
static ui_page_t s_current = UI_PAGE_LOCK;
static ui_nav_stats_t s_stats;
static bool s_started;
static bool s_sleeping;
static lv_coord_t s_press_y;

static bool s_transition_active;
static ui_nav_motion_t s_transition_motion;
static uint32_t s_transition_generation;
static uint32_t s_transition_start_tick;
static ui_nav_pending_t s_pending;
static bool s_lock_pending;
static bool s_locking;
static bool s_pending_scheduled;

static lv_obj_t *s_control_overlay;
static uint32_t s_control_generation;
static bool s_control_animating;
static bool s_control_closing;
static bool s_control_after_home;
static ui_page_t s_control_after_page = UI_PAGE_NONE;

static lv_obj_t *s_notice_overlay;
static bool s_modal_open;
static bool s_modal_closing;
static uint32_t s_notice_generation;

static lv_obj_t *s_overview_overlay;
static lv_obj_t *s_overview_content;
static uint32_t s_overview_generation;
static bool s_overview_open;
static bool s_overview_animating;
static bool s_overview_closing;
static bool s_overview_after_home;
static bool s_overview_rebuild_pending;
static ui_page_t s_overview_after_page = UI_PAGE_NONE;
static ui_page_t s_overview_source_page = UI_PAGE_NONE;
static bool s_overview_source_dismissed;
static ui_page_t s_mru[MRU_CAP];
static uint8_t s_mru_count;

static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;

static lv_timer_t *s_demo_timer;
static unsigned s_demo_cycles;
static unsigned s_demo_target_cycles;
static unsigned s_demo_step;
static uint32_t s_demo_deadline;
static uint32_t s_demo_started_tick;
static bool s_bench_mode;
static bool s_workload_started_locked;
static bool s_expect_pending;
static bool s_workload_action_begun;
static lv_timer_t *s_workload_finish_timer;
static uint32_t s_workload_finish_deadline;
static char s_workload_reason[24];
static bool s_workload_complete;
static bool s_workload_verify_ok;
static uint32_t s_workload_elapsed_ms;
static bool s_user_state_saved;
static bool s_user_state_restored;
static ui_page_t s_user_mru[MRU_CAP];
static uint8_t s_user_mru_count;
static int s_user_settings_section;

typedef enum {
    EXPECT_NONE = 0,
    EXPECT_RESTORE,
    EXPECT_APP_ENTER,
    EXPECT_APP_BACK,
    EXPECT_CONTROL_OPEN,
    EXPECT_CONTROL_CLOSE,
    EXPECT_OVERVIEW_OPEN,
    EXPECT_OVERVIEW_CLOSE,
    EXPECT_HOME_NEXT,
    EXPECT_HOME_PREV,
} workload_expect_t;

static workload_expect_t s_expect_kind;
static ui_page_t s_expect_page = UI_PAGE_NONE;
static int s_expect_control = -1;
static int s_expect_home_page = -1;
static int s_expect_overview = -1;

static void schedule_pending(void);
static void close_notice(void);
static void notice_finish_close(void);
static void control_close_request(ui_page_t after_page, bool after_home);
static bool start_page_transition(ui_page_t target, ui_nav_motion_t motion);
static void workload_reset_verification(void);
static void workload_finish(const char *reason);
static void workload_finalize(void);
static void workload_save_user_state(void);
static void workload_restore_user_state(void);
static void mru_note_visit(ui_page_t page);
static void overview_build(void);
static void overview_rebuild_async_cb(void *arg);
static void overview_close_internal(bool after_home, ui_page_t after_page);
static void overview_finalize_close(void);
static void overview_finalize_async_cb(void *arg);
static void restore_awake(void);

static lv_obj_t *create_page(ui_page_t page)
{
    switch (page) {
    case UI_PAGE_LOCK:
        return ui_page_lock_create();
    case UI_PAGE_HOME:
        return ui_page_home_create();
    case UI_PAGE_CONTROL:
        return ui_page_control_create();
    case UI_PAGE_SETTINGS:
        return ui_page_settings_create();
    case UI_PAGE_WEATHER:
        return ui_page_weather_create();
    case UI_PAGE_MONITOR:
        return ui_page_monitor_create();
    case UI_PAGE_CALCULATOR:
        return ui_page_calculator_create();
    case UI_PAGE_CALENDAR:
        return ui_page_calendar_create();
    case UI_PAGE_CLOCK:
        return ui_page_clock_create();
    case UI_PAGE_FILES:
        return ui_page_files_create();
    default:
        return ui_page_home_create();
    }
}

static void page_delete_cb(lv_event_t *event)
{
    (void)event;
    s_stats.page_deletes++;
}

static lv_obj_t *page_add(ui_page_t page)
{
    lv_obj_t *obj = create_page(page);
    if (obj == NULL) {
        return NULL;
    }
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, UI_WIDTH, UI_HEIGHT);
    lv_obj_add_event_cb(obj, page_delete_cb, LV_EVENT_DELETE, NULL);
    s_stats.page_creates++;
    return obj;
}

static void update_depth_stats(void)
{
    if (s_nav.depth > s_stats.max_history_depth) {
        s_stats.max_history_depth = s_nav.depth;
    }
}

static bool route_busy(void)
{
    return s_transition_active;
}

static bool queue_page(ui_page_t page)
{
    if (ui_nav_pending_page(&s_pending, page)) {
        s_stats.coalesced++;
        ESP_LOGI(TAG, "coalesced request while busy: page=%d", (int)page);
        return true;
    }
    s_stats.rejected++;
    return false;
}

static bool queue_back(void)
{
    if (ui_nav_pending_back(&s_pending)) {
        s_stats.coalesced++;
        ESP_LOGI(TAG, "coalesced back request while busy");
        return true;
    }
    s_stats.rejected++;
    return false;
}

static bool queue_home(void)
{
    if (ui_nav_pending_home(&s_pending)) {
        s_stats.coalesced++;
        ESP_LOGI(TAG, "coalesced home request while busy");
        return true;
    }
    s_stats.rejected++;
    return false;
}

static void set_x_cb(void *obj, int32_t value)
{
    lv_obj_set_x((lv_obj_t *)obj, value);
}

static void set_y_cb(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void set_opa_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void finish_transition(void)
{
    if (!s_transition_active) {
        return;
    }

    s_transition_active = false;
    if (s_outgoing_page != NULL) {
        lv_obj_delete(s_outgoing_page);
        s_outgoing_page = NULL;
    }
    ui_perf_action_complete();

    uint32_t elapsed = lv_tick_get() - s_transition_start_tick;
    s_stats.completed++;
    s_stats.transition_count++;
    s_stats.last_transition_ms = elapsed;
    if (elapsed > s_stats.max_transition_ms) {
        s_stats.max_transition_ms = elapsed;
    }
    if (s_stats.transition_count <= TRANSITION_LOG_LIMIT) {
        ESP_LOGI(TAG, "transition complete: page=%d motion=%s elapsed=%ums",
                 (int)s_current, ui_nav_motion_name(s_transition_motion), (unsigned)elapsed);
    }
    ui_system_bar_refresh();
    if (s_current == UI_PAGE_LOCK) {
        s_locking = false;
    }
    schedule_pending();
}

static void transition_completed_cb(lv_anim_t *anim)
{
    if ((uint32_t)(uintptr_t)lv_anim_get_user_data(anim) != s_transition_generation) {
        return;
    }
    finish_transition();
}

static void start_position_animation(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb, int32_t from,
                                     int32_t to, uint32_t duration, uint32_t generation,
                                     bool finish)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    if (finish) {
        lv_anim_set_completed_cb(&anim, transition_completed_cb);
        lv_anim_set_user_data(&anim, (void *)(uintptr_t)generation);
    }
    lv_anim_start(&anim);
}

static bool start_page_transition(ui_page_t target, ui_nav_motion_t motion)
{
    if (s_transition_active) {
        return false;
    }
    lv_obj_t *incoming = page_add(target);
    if (incoming == NULL) {
        ESP_LOGE(TAG, "failed to create page %d", (int)target);
        return false;
    }
    ui_perf_action_stage_start();
    if (s_active_page == NULL) {
        s_active_page = incoming;
        s_current = target;
        s_stats.accepted++;
        return true;
    }

    panel_settings_t settings;
    panel_services_get_settings(&settings);
    uint32_t duration = ui_nav_motion_duration(motion, settings.animations);
    lv_obj_t *outgoing = s_active_page;
    s_outgoing_page = outgoing;
    s_active_page = incoming;
    ui_page_t previous = s_current;
    s_current = target;
    if (previous != target && ui_nav_model_is_app(target)) {
        mru_note_visit(target);
    }
    s_transition_active = true;
    s_transition_motion = motion;
    s_transition_start_tick = lv_tick_get();
    s_stats.accepted++;

    ESP_LOGI(TAG, "transition start: page=%d motion=%s duration=%ums anim=%d",
             (int)target, ui_nav_motion_name(motion), (unsigned)duration,
             settings.animations ? 1 : 0);

    if (duration == 0) {
        lv_obj_set_pos(incoming, 0, 0);
        lv_obj_set_style_opa(incoming, LV_OPA_COVER, 0);
        finish_transition();
        return true;
    }

    s_transition_generation++;
    uint32_t generation = s_transition_generation;

    switch (motion) {
    case UI_NAV_MOTION_UNLOCK:
        lv_obj_set_pos(incoming, 0, 0);
        lv_obj_set_style_opa(incoming, LV_OPA_COVER, 0);
        lv_obj_set_pos(outgoing, 0, 0);
        lv_obj_move_foreground(outgoing);
        start_position_animation(outgoing, set_y_cb, 0, -UI_HEIGHT, duration, generation, false);
        /* Keep the completion on the incoming page so deleting the outgoing
         * lock screen can never delete the animation currently completing. */
        start_position_animation(incoming, set_x_cb, 0, 0, duration, generation, true);
        break;

    case UI_NAV_MOTION_FADE:
        lv_obj_set_pos(incoming, 0, 0);
        lv_obj_set_style_opa(incoming, LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(outgoing, LV_OPA_COVER, 0);
        start_position_animation(outgoing, set_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP, duration,
                                 generation, false);
        start_position_animation(incoming, set_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, duration,
                                 generation, true);
        break;

    case UI_NAV_MOTION_BACK:
        lv_obj_set_pos(incoming, -UI_WIDTH, 0);
        lv_obj_set_style_opa(incoming, LV_OPA_COVER, 0);
        lv_obj_set_pos(outgoing, 0, 0);
        start_position_animation(outgoing, set_x_cb, 0, UI_WIDTH, duration, generation, false);
        start_position_animation(incoming, set_x_cb, -UI_WIDTH, 0, duration, generation, true);
        break;

    case UI_NAV_MOTION_FORWARD:
    default:
        lv_obj_set_pos(incoming, UI_WIDTH, 0);
        lv_obj_set_style_opa(incoming, LV_OPA_COVER, 0);
        lv_obj_set_pos(outgoing, 0, 0);
        start_position_animation(outgoing, set_x_cb, 0, -UI_WIDTH, duration, generation, false);
        start_position_animation(incoming, set_x_cb, UI_WIDTH, 0, duration, generation, true);
        break;
    }
    return true;
}

static void pending_async_cb(void *arg)
{
    (void)arg;
    s_pending_scheduled = false;
    if (!s_started || s_transition_active || s_control_animating || s_modal_open ||
        s_modal_closing) {
        return;
    }

    ui_nav_pending_t pending;
    if (!ui_nav_pending_take(&s_pending, &pending)) {
        return;
    }
    s_stats.pending_dispatched++;
    switch (pending.kind) {
    case UI_NAV_PENDING_PAGE:
        ESP_LOGI(TAG, "dispatch pending page=%d", (int)pending.page);
        ui_navigate(pending.page);
        break;
    case UI_NAV_PENDING_BACK:
        ESP_LOGI(TAG, "dispatch pending back");
        ui_back();
        break;
    case UI_NAV_PENDING_HOME:
        ESP_LOGI(TAG, "dispatch pending home");
        ui_home();
        break;
    default:
        break;
    }
}

static void schedule_pending(void)
{
    if (!s_pending_scheduled && s_pending.kind != UI_NAV_PENDING_NONE) {
        s_pending_scheduled = true;
        lv_async_call(pending_async_cb, NULL);
    }
}

void ui_navigate(ui_page_t page)
{
    (void)ui_network_panel_back();
    if (page == UI_PAGE_CONTROL) {
        ui_open_control();
        return;
    }
    s_stats.requests++;
    if (!s_started) {
        s_stats.rejected++;
        return;
    }
    if (page == UI_PAGE_LOCK || page == UI_PAGE_NONE) {
        s_stats.rejected++;
        return;
    }
    if (s_modal_open || s_modal_closing) {
        s_stats.rejected++;
        return;
    }
    if (s_overview_overlay != NULL) {
        s_stats.rejected++;
        return;
    }
    if (s_control_overlay != NULL) {
        control_close_request(page, false);
        return;
    }
    if (route_busy()) {
        queue_page(page);
        return;
    }

    if (s_nav.locked) {
        if (page != UI_PAGE_HOME) {
            s_stats.rejected++;
            return;
        }
        ui_nav_model_t before = s_nav;
        ui_nav_motion_t motion = UI_NAV_MOTION_UNLOCK;
        if (!ui_nav_model_unlock(&s_nav) ||
            !start_page_transition(UI_PAGE_HOME, motion)) {
            s_nav = before;
            s_stats.rejected++;
            return;
        }
        update_depth_stats();
        return;
    }

    if (page == s_nav.current) {
        s_stats.rejected++;
        return;
    }

    ui_nav_model_t before = s_nav;
    ui_nav_motion_t motion = UI_NAV_MOTION_NONE;
    if (!ui_nav_model_enter(&s_nav, page, &motion)) {
        s_stats.rejected++;
        return;
    }
    if (!start_page_transition(page, motion)) {
        s_nav = before;
        s_stats.rejected++;
        return;
    }
    update_depth_stats();
}

void ui_navigate_force(ui_page_t page)
{
    s_stats.requests++;
    if (!s_started || page != s_nav.current || route_busy() || s_control_overlay != NULL ||
        s_modal_open || s_modal_closing || s_pending.kind != UI_NAV_PENDING_NONE ||
        s_pending_scheduled) {
        s_stats.rejected++;
        return;
    }
    if (!start_page_transition(page, UI_NAV_MOTION_FADE)) {
        s_stats.rejected++;
    }
}

void ui_back(void)
{
    s_stats.requests++;
    s_stats.back_requests++;
    if (!s_started) {
        s_stats.back_disabled++;
        return;
    }
    /* Back during the lock transition must not create a stale pending action
     * that survives into the next page. */
    if (s_locking) {
        s_stats.back_disabled++;
        return;
    }
    if (s_modal_open) {
        if (!s_modal_closing) {
            close_notice();
        }
        return;
    }
    if (s_overview_overlay != NULL) {
        if (!s_overview_closing) {
            overview_close_internal(false, UI_PAGE_NONE);
        }
        return;
    }
    if (s_control_overlay != NULL) {
        if (!s_control_closing) {
            control_close_request(UI_PAGE_NONE, false);
        }
        return;
    }
    if (ui_weather_panel_back()) {
        return;
    }
    if (ui_page_files_back()) {
        return;
    }
    if (route_busy()) {
        queue_back();
        return;
    }
    if (s_nav.current == UI_PAGE_SETTINGS && ui_page_settings_back()) {
        s_stats.settings_section_backs++;
        return;
    }

    ui_nav_model_t before = s_nav;
    ui_page_t target = UI_PAGE_NONE;
    ui_nav_motion_t motion = UI_NAV_MOTION_NONE;
    if (!ui_nav_model_back(&s_nav, &target, &motion)) {
        s_stats.back_disabled++;
        return;
    }
    if (!start_page_transition(target, motion)) {
        s_nav = before;
        s_stats.rejected++;
        return;
    }
    update_depth_stats();
}

void ui_home(void)
{
    (void)ui_network_panel_back();
    (void)ui_weather_panel_back();
    s_stats.requests++;
    s_stats.home_requests++;
    if (!s_started) {
        s_stats.rejected++;
        return;
    }
    if (s_modal_open) {
        notice_finish_close();
    }
    if (s_overview_overlay != NULL) {
        if (s_overview_closing) {
            s_overview_after_home = true;
            s_overview_after_page = UI_PAGE_NONE;
        } else {
            overview_close_internal(true, UI_PAGE_NONE);
        }
        return;
    }
    if (s_control_overlay != NULL) {
        s_control_after_home = true;
        s_control_after_page = UI_PAGE_NONE;
        if (!s_control_closing) {
            control_close_request(UI_PAGE_NONE, true);
        }
        return;
    }
    if (route_busy()) {
        queue_home();
        return;
    }

    ui_nav_model_t before = s_nav;
    ui_nav_motion_t motion = UI_NAV_MOTION_NONE;
    if (!ui_nav_model_home(&s_nav, &motion)) {
        s_stats.rejected++;
        return;
    }
    if (!start_page_transition(UI_PAGE_HOME, motion)) {
        s_nav = before;
        s_stats.rejected++;
        return;
    }
    update_depth_stats();
}

ui_page_t ui_current_page(void)
{
    if (s_control_overlay != NULL || s_nav.control_open) {
        return UI_PAGE_CONTROL;
    }
    return s_nav.current;
}

typedef struct {
    ui_page_t page;
    const char *name;
    const char *glyph;
    const lv_font_t *font;
    uint32_t icon_bg;
    uint32_t icon_fg;
} overview_app_t;

static const overview_app_t s_overview_apps[] = {
    {UI_PAGE_SETTINGS, "设置", LV_SYMBOL_SETTINGS, &lv_font_montserrat_40, 0xEDEDEB, 0x2C2C2B},
    {UI_PAGE_WEATHER, "天气", LV_SYMBOL_TINT, &lv_font_montserrat_40, 0xE5F2FC, 0x2783DE},
    {UI_PAGE_MONITOR, "监视器", LV_SYMBOL_VIDEO, &lv_font_montserrat_40, 0xE3F3E9, 0x2E9E68},
    {UI_PAGE_CALCULATOR, "计算器", "=", &ui_font_32, 0xFBE8D2, 0xE07B24},
    {UI_PAGE_CALENDAR, "日历", "日", &ui_font_32, 0xFBE4E5, 0xD95A5F},
    {UI_PAGE_CLOCK, "时钟", "时", &ui_font_32, 0xEDEDEB, 0x2C2C2B},
    {UI_PAGE_FILES, "文件", LV_SYMBOL_DIRECTORY, &lv_font_montserrat_40, 0xFCE8D2, 0xE07B24},
};

static const overview_app_t *overview_app_meta(ui_page_t page)
{
    for (size_t i = 0; i < sizeof(s_overview_apps) / sizeof(s_overview_apps[0]); i++) {
        if (s_overview_apps[i].page == page) {
            return &s_overview_apps[i];
        }
    }
    return NULL;
}

static void mru_note_visit(ui_page_t page)
{
    if (!ui_nav_model_is_app(page)) {
        return;
    }
    int found = -1;
    for (int i = 0; i < s_mru_count; i++) {
        if (s_mru[i] == page) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        if (s_mru_count < MRU_CAP) {
            s_mru_count++;
        }
        memmove(&s_mru[1], &s_mru[0], (s_mru_count - 1) * sizeof(s_mru[0]));
    } else if (found > 0) {
        memmove(&s_mru[1], &s_mru[0], found * sizeof(s_mru[0]));
    }
    s_mru[0] = page;
}

static void mru_remove(ui_page_t page)
{
    for (int i = 0; i < s_mru_count; i++) {
        if (s_mru[i] == page) {
            memmove(&s_mru[i], &s_mru[i + 1],
                    (s_mru_count - i - 1) * sizeof(s_mru[0]));
            s_mru_count--;
            return;
        }
    }
}

static void mru_clear(void)
{
    s_mru_count = 0;
}

int ui_overview_recent_count(void)
{
    return s_mru_count;
}

ui_page_t ui_overview_recent_at(int index)
{
    if (index < 0 || index >= s_mru_count) {
        return UI_PAGE_NONE;
    }
    return s_mru[index];
}

static void overview_app_summary(ui_page_t page, char *buf, size_t len)
{
    switch (page) {
    case UI_PAGE_SETTINGS:
        ui_page_settings_state_summary(buf, len);
        break;
    case UI_PAGE_CALCULATOR:
        ui_page_calculator_state_summary(buf, len);
        break;
    case UI_PAGE_CALENDAR:
        ui_page_calendar_state_summary(buf, len);
        break;
    case UI_PAGE_CLOCK:
        ui_page_clock_state_summary(buf, len);
        break;
    case UI_PAGE_FILES:
        ui_page_files_state_summary(buf, len);
        break;
    case UI_PAGE_WEATHER: {
        panel_weather_snapshot_t weather;
        panel_services_get_weather(&weather);
        if (weather.has_data) {
            snprintf(buf, len, "%s %d° · %s", weather.city, weather.current_temp_c,
                     weather.stale ? "缓存" : weather.wifi_connected ? "实时" : "离线");
        } else if (!weather.calibrated) {
            snprintf(buf, len, "天气 · 等待校时");
        } else if (!weather.wifi_connected) {
            snprintf(buf, len, "天气 · 等待网络");
        } else {
            snprintf(buf, len, "天气 · 暂无数据");
        }
        break;
    }
    case UI_PAGE_MONITOR: {
        panel_runtime_t runtime;
        panel_services_get_runtime(&runtime);
        snprintf(buf, len, "运行时实测 · 堆 %u KB", (unsigned)(runtime.heap_free / 1024U));
        break;
    }
    default:
        snprintf(buf, len, "未打开");
        break;
    }
}

static void overview_schedule_rebuild(void)
{
    if (s_overview_rebuild_pending || s_overview_closing) {
        return;
    }
    s_overview_rebuild_pending = true;
    lv_async_call(overview_rebuild_async_cb, NULL);
}

static void overview_card_open_cb(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) != lv_event_get_current_target_obj(event)) {
        return;
    }
    ui_page_t page = (ui_page_t)(uintptr_t)lv_event_get_user_data(event);
    if (page == UI_PAGE_NONE) {
        return;
    }
    if (s_control_overlay != NULL) {
        overview_close_internal(false, page);
    } else if (page == s_nav.current) {
        overview_close_internal(false, UI_PAGE_NONE);
    } else {
        overview_close_internal(false, page);
    }
}

static void overview_card_dismiss_cb(lv_event_t *event)
{
    ui_page_t page = (ui_page_t)(uintptr_t)lv_event_get_user_data(event);
    if (page == UI_PAGE_NONE) {
        return;
    }
    if (page == s_overview_source_page && ui_nav_model_is_app(page)) {
        s_overview_source_dismissed = true;
    }
    mru_remove(page);
    if (page == UI_PAGE_SETTINGS) {
        ui_page_settings_state_reset();
    } else if (page == UI_PAGE_CALCULATOR) {
        ui_page_calculator_state_reset();
    } else if (page == UI_PAGE_CALENDAR) {
        ui_page_calendar_state_reset();
    } else if (page == UI_PAGE_CLOCK) {
        ui_page_clock_ui_state_reset();
    } else if (page == UI_PAGE_FILES) {
        ui_page_files_ui_state_reset();
    }
    overview_schedule_rebuild();
}

static void overview_clear_all_cb(lv_event_t *event)
{
    (void)event;
    if (ui_nav_model_is_app(s_overview_source_page)) {
        s_overview_source_dismissed = true;
    }
    mru_clear();
    ui_page_settings_state_reset();
    ui_page_calculator_state_reset();
    ui_page_calendar_state_reset();
    ui_page_clock_ui_state_reset();
    ui_page_files_ui_state_reset();
    overview_schedule_rebuild();
}

static void overview_build(void)
{
    if (s_overview_overlay == NULL) {
        return;
    }
    const ui_palette_t *p = ui_palette();
    lv_obj_clean(s_overview_overlay);
    s_overview_content = NULL;

    ui_label_at(s_overview_overlay, 40, 48, "最近应用", &ui_font_32, p->text);
    ui_label_at(s_overview_overlay, 40, 92, "打开应用后会显示在这里", &ui_font_16, p->muted);

    lv_obj_t *clear = ui_button(s_overview_overlay, 1080, 52, 160, 48, "全部清除",
                                &ui_font_16, overview_clear_all_cb, NULL);
    lv_obj_set_style_bg_color(clear, p->surface_alt, 0);
    if (s_mru_count == 0) {
        lv_obj_add_state(clear, LV_STATE_DISABLED);
    }

    s_overview_content = lv_obj_create(s_overview_overlay);
    lv_obj_remove_flag(s_overview_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_overview_content, 40, 150);
    lv_obj_set_size(s_overview_content, 1200, 560);
    lv_obj_set_style_bg_opa(s_overview_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overview_content, 0, 0);
    lv_obj_set_style_pad_all(s_overview_content, 0, 0);

    if (s_mru_count == 0) {
        lv_obj_t *empty = ui_card(s_overview_content, 310, 100, 580, 280);
        lv_obj_t *title = ui_label(empty, "暂无最近应用", &ui_font_32, p->text);
        lv_obj_set_width(title, 500);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 76);
        lv_obj_t *body = ui_label(empty, "打开应用后会显示在这里", &ui_font_20, p->muted);
        lv_obj_set_width(body, 500);
        lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 132);
        return;
    }

    for (int i = 0; i < s_mru_count; i++) {
        const overview_app_t *app = overview_app_meta(s_mru[i]);
        if (app == NULL) {
            continue;
        }
        int x = (i % 2) * 620;
        int y = (i / 2) * 270;
        lv_obj_t *card = ui_card(s_overview_content, x, y, 580, 240);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, overview_card_open_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)app->page);
        ui_pressable(card);

        lv_obj_t *icon_box = lv_obj_create(card);
        lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(icon_box, 28, 28);
        lv_obj_set_size(icon_box, 76, 76);
        lv_obj_set_style_bg_color(icon_box, lv_color_hex(app->icon_bg), 0);
        lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(icon_box, 20, 0);
        lv_obj_set_style_border_width(icon_box, 0, 0);
        lv_obj_t *icon = ui_label(icon_box, app->glyph, app->font,
                                  lv_color_hex(app->icon_fg));
        lv_obj_center(icon);

        ui_label_at(card, 128, 28, app->name, &ui_font_32, p->text);
        char summary[96];
        overview_app_summary(app->page, summary, sizeof(summary));
        lv_obj_t *sub = ui_label_at(card, 128, 82, summary, &ui_font_16, p->muted);
        lv_obj_set_width(sub, 380);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_CLIP);
        ui_label_at(card, 128, 188, "打开", &ui_font_16, p->accent);

        lv_obj_t *close = lv_button_create(card);
        lv_obj_set_pos(close, 504, 20);
        lv_obj_set_size(close, 48, 48);
        lv_obj_set_style_radius(close, 24, 0);
        lv_obj_set_style_bg_color(close, p->surface_alt, 0);
        lv_obj_set_style_bg_opa(close, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(close, 0, 0);
        lv_obj_set_style_shadow_width(close, 0, 0);
        lv_obj_add_event_cb(close, overview_card_dismiss_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)app->page);
        lv_obj_set_user_data(close, (void *)(uintptr_t)app->page);
        ui_pressable(close);
        lv_obj_t *x_label = ui_label(close, LV_SYMBOL_CLOSE, &lv_font_montserrat_20, p->muted);
        lv_obj_center(x_label);
    }
}

static void overview_rebuild_async_cb(void *arg)
{
    (void)arg;
    s_overview_rebuild_pending = false;
    if (s_overview_overlay != NULL && !s_overview_closing) {
        overview_build();
    }
}

static void overview_finalize_async_cb(void *arg)
{
    (void)arg;
    overview_finalize_close();
}

static void overview_open_completed_cb(lv_anim_t *anim)
{
    if ((uint32_t)(uintptr_t)lv_anim_get_user_data(anim) != s_overview_generation) {
        return;
    }
    s_overview_animating = false;
    ui_perf_action_complete();
}

static void overview_close_completed_cb(lv_anim_t *anim)
{
    if ((uint32_t)(uintptr_t)lv_anim_get_user_data(anim) != s_overview_generation) {
        return;
    }
    overview_finalize_close();
}

static void overview_close_internal(bool after_home, ui_page_t after_page)
{
    if (s_overview_overlay == NULL) {
        return;
    }
    if (s_overview_closing) {
        if (after_home) {
            s_overview_after_home = true;
            s_overview_after_page = UI_PAGE_NONE;
        } else if (after_page != UI_PAGE_NONE) {
            s_overview_after_page = after_page;
            s_overview_after_home = false;
        }
        return;
    }

    s_overview_open = false;
    s_overview_closing = true;
    s_overview_animating = true;
    s_overview_after_home = after_home;
    s_overview_after_page = after_page;
    s_overview_generation++;
    ui_perf_action_stage_start();

    panel_settings_t settings;
    panel_services_get_settings(&settings);
    if (!settings.animations) {
        lv_async_call(overview_finalize_async_cb, NULL);
        return;
    }
    lv_anim_t y_anim;
    lv_anim_init(&y_anim);
    lv_anim_set_var(&y_anim, s_overview_overlay);
    lv_anim_set_exec_cb(&y_anim, set_y_cb);
    lv_anim_set_values(&y_anim, lv_obj_get_y(s_overview_overlay), 40);
    lv_anim_set_duration(&y_anim, OVERVIEW_FADE_MS);
    lv_anim_set_path_cb(&y_anim, lv_anim_path_ease_out);
    lv_anim_start(&y_anim);

    lv_anim_t opa_anim;
    lv_anim_init(&opa_anim);
    lv_anim_set_var(&opa_anim, s_overview_overlay);
    lv_anim_set_exec_cb(&opa_anim, set_opa_cb);
    lv_anim_set_values(&opa_anim, lv_obj_get_style_opa(s_overview_overlay, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&opa_anim, OVERVIEW_FADE_MS);
    lv_anim_set_path_cb(&opa_anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&opa_anim, overview_close_completed_cb);
    lv_anim_set_user_data(&opa_anim, (void *)(uintptr_t)s_overview_generation);
    lv_anim_start(&opa_anim);
}

void ui_close_overview(void)
{
    overview_close_internal(false, UI_PAGE_NONE);
}

bool ui_overview_is_open(void)
{
    return s_overview_overlay != NULL && s_overview_open;
}

void ui_open_overview(void)
{
    (void)ui_network_panel_back();
    (void)ui_weather_panel_back();
    s_stats.requests++;
    if (!s_started || s_nav.locked || ui_diag_is_active()) {
        s_stats.rejected++;
        return;
    }
    if (s_overview_overlay != NULL) {
        if (s_overview_animating) {
            s_stats.rejected++;
            return;
        }
        if (s_overview_open) {
            overview_close_internal(false, UI_PAGE_NONE);
        }
        return;
    }
    if (s_transition_active || s_control_animating || s_modal_closing ||
        s_pending.kind != UI_NAV_PENDING_NONE || s_pending_scheduled) {
        s_stats.rejected++;
        return;
    }
    if (s_modal_open) {
        notice_finish_close();
    }
    if (s_modal_open) {
        s_stats.rejected++;
        return;
    }

    s_overview_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(s_overview_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_overview_overlay, 0, 0);
    lv_obj_set_size(s_overview_overlay, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_style_bg_color(s_overview_overlay, ui_palette()->bg, 0);
    lv_obj_set_style_bg_opa(s_overview_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overview_overlay, 0, 0);
    lv_obj_set_style_radius(s_overview_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overview_overlay, 0, 0);

    s_overview_open = true;
    s_overview_closing = false;
    s_overview_animating = true;
    s_overview_after_home = false;
    s_overview_after_page = UI_PAGE_NONE;
    s_overview_source_page = s_nav.current;
    s_overview_source_dismissed = false;
    s_overview_generation++;
    overview_build();
    ui_perf_action_stage_start();

    panel_settings_t settings;
    panel_services_get_settings(&settings);
    if (!settings.animations) {
        lv_obj_set_y(s_overview_overlay, 0);
        lv_obj_set_style_opa(s_overview_overlay, LV_OPA_COVER, 0);
        s_overview_animating = false;
        ui_perf_action_complete();
        ui_system_bar_refresh();
        return;
    }

    lv_obj_set_y(s_overview_overlay, 40);
    lv_obj_set_style_opa(s_overview_overlay, LV_OPA_TRANSP, 0);

    lv_anim_t y_anim;
    lv_anim_init(&y_anim);
    lv_anim_set_var(&y_anim, s_overview_overlay);
    lv_anim_set_exec_cb(&y_anim, set_y_cb);
    lv_anim_set_values(&y_anim, 40, 0);
    lv_anim_set_duration(&y_anim, OVERVIEW_FADE_MS);
    lv_anim_set_path_cb(&y_anim, lv_anim_path_ease_out);
    lv_anim_start(&y_anim);

    lv_anim_t opa_anim;
    lv_anim_init(&opa_anim);
    lv_anim_set_var(&opa_anim, s_overview_overlay);
    lv_anim_set_exec_cb(&opa_anim, set_opa_cb);
    lv_anim_set_values(&opa_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&opa_anim, OVERVIEW_FADE_MS);
    lv_anim_set_path_cb(&opa_anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&opa_anim, overview_open_completed_cb);
    lv_anim_set_user_data(&opa_anim, (void *)(uintptr_t)s_overview_generation);
    lv_anim_start(&opa_anim);
    ui_system_bar_refresh();
}

static void overview_finalize_close(void)
{
    bool after_home = s_overview_after_home;
    ui_page_t after_page = s_overview_after_page;
    bool source_dismissed = s_overview_source_dismissed;
    s_overview_after_home = false;
    s_overview_after_page = UI_PAGE_NONE;
    s_overview_source_page = UI_PAGE_NONE;
    s_overview_source_dismissed = false;

    if (s_overview_overlay != NULL) {
        lv_obj_delete(s_overview_overlay);
        s_overview_overlay = NULL;
    }
    s_overview_content = NULL;
    s_overview_open = false;
    s_overview_closing = false;
    s_overview_animating = false;
    s_overview_rebuild_pending = false;

    if (after_home) {
        ui_home();
    } else if (after_page != UI_PAGE_NONE) {
        if (source_dismissed) {
            /* The dismissed source is gone from MRU and must not be captured
             * in history just because the user picked another recent app. */
            s_nav.current = UI_PAGE_HOME;
            s_nav.depth = 0;
        }
        if (s_control_overlay != NULL) {
            control_close_request(after_page, false);
        } else if (after_page != s_nav.current) {
            ui_navigate(after_page);
        }
    } else if (source_dismissed) {
        if (s_control_overlay != NULL) {
            control_close_request(UI_PAGE_NONE, true);
        } else {
            ui_home();
        }
    }
    ui_perf_action_complete();
    ui_system_bar_refresh();
}

bool ui_overview_is_animating(void)
{
    return s_overview_animating;
}

void ui_system_bar_refresh(void)
{
    bool visible = s_started && !s_nav.locked && !ui_diag_is_active();
    ui_system_bar_set_visible(visible);
}

static void control_anim_completed_cb(lv_anim_t *anim)
{
    if ((uint32_t)(uintptr_t)lv_anim_get_user_data(anim) != s_control_generation) {
        return;
    }
    if (!s_control_closing) {
        s_control_animating = false;
        s_stats.control_opens++;
        ui_perf_action_complete();
        ESP_LOGI(TAG, "control centre opened over source page %d", (int)s_nav.current);
        schedule_pending();
    }
}

static void control_finalize_close(void)
{
    ui_page_t after_page = s_control_after_page;
    bool after_home = s_control_after_home;
    s_control_after_page = UI_PAGE_NONE;
    s_control_after_home = false;

    if (s_control_overlay != NULL) {
        lv_obj_delete(s_control_overlay);
        s_control_overlay = NULL;
    }
    s_control_animating = false;
    s_control_closing = false;
    ui_page_t source = UI_PAGE_NONE;
    ui_nav_model_control_close(&s_nav, &source);
    s_stats.control_closes++;
    ui_perf_action_complete();
    ESP_LOGI(TAG, "control centre closed; source page restored: %d", (int)source);

    if (after_home) {
        ui_home();
    } else if (after_page != UI_PAGE_NONE) {
        ui_navigate(after_page);
    } else {
        schedule_pending();
    }
}

static void control_close_completed_cb(lv_anim_t *anim)
{
    if ((uint32_t)(uintptr_t)lv_anim_get_user_data(anim) != s_control_generation) {
        return;
    }
    control_finalize_close();
}

static void control_close_request(ui_page_t after_page, bool after_home)
{
    if (s_control_overlay == NULL) {
        return;
    }
    if (s_control_closing) {
        if (after_home) {
            s_control_after_home = true;
            s_control_after_page = UI_PAGE_NONE;
        } else if (after_page != UI_PAGE_NONE) {
            s_control_after_page = after_page;
            s_control_after_home = false;
        }
        return;
    }

    s_control_closing = true;
    s_control_animating = true;
    s_control_after_home = after_home;
    s_control_after_page = after_page;
    lv_anim_delete(s_control_overlay, set_y_cb);
    s_control_generation++;
    ui_perf_action_stage_start();

    panel_settings_t settings;
    panel_services_get_settings(&settings);
    uint32_t duration = ui_nav_motion_duration(UI_NAV_MOTION_CONTROL_CLOSE, settings.animations);
    if (duration == 0) {
        control_finalize_close();
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_control_overlay);
    lv_anim_set_exec_cb(&anim, set_y_cb);
    lv_anim_set_values(&anim, lv_obj_get_y(s_control_overlay), -UI_HEIGHT);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, control_close_completed_cb);
    lv_anim_set_user_data(&anim, (void *)(uintptr_t)s_control_generation);
    lv_anim_start(&anim);
}

void ui_open_control(void)
{
    (void)ui_network_panel_back();
    (void)ui_weather_panel_back();
    s_stats.requests++;
    if (!s_started || s_nav.locked || s_control_overlay != NULL || route_busy() ||
        s_modal_open || s_modal_closing || s_overview_overlay != NULL ||
        ui_diag_is_active() ||
        s_pending.kind != UI_NAV_PENDING_NONE || s_pending_scheduled ||
        ui_page_home_animating()) {
        s_stats.rejected++;
        return;
    }
    if (!ui_nav_model_control_open(&s_nav)) {
        s_stats.rejected++;
        return;
    }

    if (s_toast_timer != NULL) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (s_toast != NULL) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }

    s_control_overlay = ui_page_control_create_on(lv_layer_top());
    if (s_control_overlay == NULL) {
        ui_page_t source = UI_PAGE_NONE;
        ui_nav_model_control_close(&s_nav, &source);
        s_stats.rejected++;
        return;
    }
    lv_obj_set_pos(s_control_overlay, 0, -UI_HEIGHT);
    s_control_closing = false;
    s_control_animating = true;
    s_control_generation++;
    ui_perf_action_stage_start();

    panel_settings_t settings;
    panel_services_get_settings(&settings);
    uint32_t duration = ui_nav_motion_duration(UI_NAV_MOTION_CONTROL_OPEN, settings.animations);
    if (duration == 0) {
        lv_obj_set_y(s_control_overlay, 0);
        s_control_animating = false;
        s_stats.control_opens++;
        ui_perf_action_complete();
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_control_overlay);
    lv_anim_set_exec_cb(&anim, set_y_cb);
    lv_anim_set_values(&anim, -UI_HEIGHT, 0);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, control_anim_completed_cb);
    lv_anim_set_user_data(&anim, (void *)(uintptr_t)s_control_generation);
    lv_anim_start(&anim);
}

void ui_close_control(void)
{
    s_stats.requests++;
    control_close_request(UI_PAGE_NONE, false);
}

static void notice_finish_close(void)
{
    if (s_notice_overlay != NULL) {
        lv_obj_delete(s_notice_overlay);
        s_notice_overlay = NULL;
    }
    s_modal_open = false;
    s_modal_closing = false;
    schedule_pending();
}

static void notice_close_completed_cb(lv_anim_t *anim)
{
    if ((uint32_t)(uintptr_t)lv_anim_get_user_data(anim) != s_notice_generation) {
        return;
    }
    notice_finish_close();
}

static void close_notice(void)
{
    if (!s_modal_open || s_modal_closing) {
        return;
    }
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    if (!settings.animations || s_notice_overlay == NULL) {
        notice_finish_close();
        return;
    }
    s_modal_closing = true;
    s_notice_generation++;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_notice_overlay);
    lv_anim_set_exec_cb(&anim, set_opa_cb);
    lv_anim_set_values(&anim, lv_obj_get_style_opa(s_notice_overlay, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&anim, NOTICE_FADE_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&anim, notice_close_completed_cb);
    lv_anim_set_user_data(&anim, (void *)(uintptr_t)s_notice_generation);
    lv_anim_start(&anim);
}

static void notice_close_cb(lv_event_t *event)
{
    (void)event;
    close_notice();
}

static void notice_overlay_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target_obj(event);
    lv_obj_t *overlay = lv_event_get_current_target_obj(event);
    if (target == overlay) {
        close_notice();
    }
}

bool ui_show_notice(const char *title, const char *badge, const char *body)
{
    if (s_modal_open || s_notice_overlay != NULL || s_control_overlay != NULL ||
        s_overview_overlay != NULL || s_transition_active) {
        return false;
    }
    /* A reminder must wake the panel and count as activity; otherwise the
     * 500 ms sleep timer can immediately blank it again after a long idle. */
    restore_awake();
    lv_display_trigger_activity(NULL);
    s_modal_open = true;
    s_modal_closing = false;
    const ui_palette_t *p = ui_palette();

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(overlay, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_add_event_cb(overlay, notice_overlay_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = ui_card(overlay, 0, 0, 660, 360);
    lv_obj_center(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    if (badge != NULL) {
        lv_obj_t *pill = ui_pill(card, badge, p->accent, p->accent_soft, &ui_font_16);
        lv_obj_set_pos(pill, 32, 28);
    }
    ui_label_at(card, 32, 82, title, &ui_font_32, p->text);
    lv_obj_t *body_label = ui_label_at(card, 32, 142, body, &ui_font_20, p->muted);
    lv_obj_set_width(body_label, 596);
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *close = ui_button(card, 32, 284, 596, 52, "知道了", &ui_font_20,
                                notice_close_cb, NULL);
    lv_obj_set_style_bg_color(close, p->accent, 0);
    lv_obj_set_style_text_color(close, lv_color_white(), 0);
    lv_obj_t *close_label = lv_obj_get_child(close, 0);
    if (close_label != NULL) {
        lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
    }

    s_notice_overlay = overlay;
    s_notice_generation++;
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    if (settings.animations) {
        lv_obj_set_style_opa(overlay, LV_OPA_TRANSP, 0);
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, overlay);
        lv_anim_set_exec_cb(&anim, set_opa_cb);
        lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&anim, NOTICE_FADE_MS);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_start(&anim);
    } else {
        lv_obj_set_style_opa(overlay, LV_OPA_COVER, 0);
    }
    return true;
}

static void toast_timer_cb(lv_timer_t *timer)
{
    if (s_toast != NULL) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    s_toast_timer = NULL;
    lv_timer_delete(timer);
}

void ui_toast(const char *text)
{
    if (s_modal_open || s_control_overlay != NULL || text == NULL) {
        return;
    }
    if (s_toast_timer != NULL) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (s_toast != NULL) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }

    const ui_palette_t *p = ui_palette();
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(box, p->text, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
    lv_obj_set_style_radius(box, 18, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_hor(box, 24, 0);
    lv_obj_set_style_pad_ver(box, 14, 0);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_shadow_width(box, 24, 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(box, p->shadow, 0);
    lv_obj_t *label = ui_label(box, text, &ui_font_20, p->bg);
    lv_obj_center(label);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -(UI_NAV_HEIGHT + 20));
    s_toast = box;
    s_toast_timer = lv_timer_create(toast_timer_cb, 1900, NULL);
}

static void restore_awake(void)
{
    if (!s_sleeping) {
        return;
    }
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    panel_bsp_backlight_on();
    panel_bsp_set_brightness(settings.brightness);
    lv_display_trigger_activity(NULL);
    s_sleeping = false;
    ESP_LOGI(TAG, "display wake on touch");
}

static void sleep_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_sleeping || ui_diag_is_active()) {
        return;
    }
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    if (settings.sleep_after_s == 0) {
        return;
    }
    uint32_t inactive_ms = lv_display_get_inactive_time(NULL);
    if (inactive_ms >= (uint32_t)settings.sleep_after_s * 1000U) {
        panel_bsp_backlight_off();
        s_sleeping = true;
        ESP_LOGI(TAG, "display sleep after %us idle", settings.sleep_after_s);
    }
}

static void indev_pressed_cb(lv_event_t *event)
{
    (void)event;
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL) {
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        s_press_y = point.y;
        ui_diag_note_touch(point.x, point.y, "press");
    }
    restore_awake();
}

static ui_swipe_dir_t swipe_dir_from_lv(lv_dir_t dir)
{
    switch (dir) {
    case LV_DIR_LEFT:
        return UI_SWIPE_LEFT;
    case LV_DIR_RIGHT:
        return UI_SWIPE_RIGHT;
    case LV_DIR_TOP:
        return UI_SWIPE_TOP;
    case LV_DIR_BOTTOM:
        return UI_SWIPE_BOTTOM;
    default:
        return UI_SWIPE_NONE;
    }
}

static void indev_gesture_cb(lv_event_t *event)
{
    (void)event;
    if (s_modal_open || s_overview_overlay != NULL || ui_diag_is_active()) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    ui_gesture_action_t action = ui_gesture_resolve(
        ui_current_page(), swipe_dir_from_lv(lv_indev_get_gesture_dir(indev)), s_press_y,
        UI_HEIGHT, s_modal_open);
    switch (action) {
    case UI_GESTURE_ACTION_UNLOCK:
        ui_navigate(UI_PAGE_HOME);
        break;
    case UI_GESTURE_ACTION_CLOSE_CONTROL:
        ui_close_control();
        break;
    case UI_GESTURE_ACTION_OPEN_CONTROL:
        ui_open_control();
        break;
    case UI_GESTURE_ACTION_HOME_NEXT:
        ui_page_home_swipe(1);
        break;
    case UI_GESTURE_ACTION_HOME_PREV:
        ui_page_home_swipe(-1);
        break;
    default:
        break;
    }
}

static void rebuild_async_cb(void *arg)
{
    (void)arg;
    ui_system_bar_rebuild();
    ui_navigate_force(s_nav.current);
}

void ui_rebuild_current_async(void)
{
    lv_async_call(rebuild_async_cb, NULL);
}void ui_nav_get_stats(ui_nav_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_stats;
}

void ui_nav_log_stats(void)
{
    if (!panel_bsp_lock(1000)) {
        ESP_LOGW(TAG, "could not lock LVGL for navigation stats");
        return;
    }
    ui_nav_stats_t stats = s_stats;
    panel_runtime_t runtime;
    panel_services_get_runtime(&runtime);
    panel_bsp_unlock();

    ESP_LOGI(TAG,
             "NAVSTATS requests=%u accepted=%u coalesced=%u rejected=%u completed=%u "
             "back=%u back_disabled=%u home=%u control_open=%u control_close=%u "
             "settings_back=%u creates=%u deletes=%u pending=%u depth_max=%u "
             "last_ms=%u max_ms=%u demo_runs=%u bench_runs=%u bench_state_restored=%u "
             "verify_fail=%u verified_enter=%u verified_back=%u verified_ctl=%u/%u "
             "verified_overview=%u/%u verified_page=%u/%u heap=%u heap_min=%u psram=%u",
             (unsigned)stats.requests, (unsigned)stats.accepted, (unsigned)stats.coalesced,
             (unsigned)stats.rejected, (unsigned)stats.completed, (unsigned)stats.back_requests,
             (unsigned)stats.back_disabled, (unsigned)stats.home_requests,
             (unsigned)stats.control_opens, (unsigned)stats.control_closes,
             (unsigned)stats.settings_section_backs, (unsigned)stats.page_creates,
             (unsigned)stats.page_deletes, (unsigned)stats.pending_dispatched,
             (unsigned)stats.max_history_depth, (unsigned)stats.last_transition_ms,
             (unsigned)stats.max_transition_ms, (unsigned)stats.demo_runs,
             (unsigned)stats.bench_runs, (unsigned)stats.bench_state_restored,
             (unsigned)stats.verify_failures,
             (unsigned)stats.verified_app_enter, (unsigned)stats.verified_app_back,
             (unsigned)stats.verified_control_open, (unsigned)stats.verified_control_close,
             (unsigned)stats.verified_overview_open, (unsigned)stats.verified_overview_close,
             (unsigned)stats.verified_home_next, (unsigned)stats.verified_home_prev,
             (unsigned)runtime.heap_free, (unsigned)runtime.heap_min,
             (unsigned)runtime.psram_free);
}

static void workload_reset_verification(void)
{
    s_stats.verified_unlock = 0;
    s_stats.verified_app_enter = 0;
    s_stats.verified_app_back = 0;
    s_stats.verified_control_open = 0;
    s_stats.verified_control_close = 0;
    s_stats.verified_overview_open = 0;
    s_stats.verified_overview_close = 0;
    s_stats.verified_home_next = 0;
    s_stats.verified_home_prev = 0;
    s_stats.verify_failures = 0;
    s_expect_pending = false;
    s_expect_kind = EXPECT_NONE;
    s_expect_page = UI_PAGE_NONE;
    s_expect_control = -1;
    s_expect_home_page = -1;
    s_expect_overview = -1;
    s_workload_action_begun = false;
}

static void workload_set_expect(workload_expect_t kind, ui_page_t page, int control,
                                int home_page)
{
    s_expect_pending = true;
    s_expect_kind = kind;
    s_expect_page = page;
    s_expect_control = control;
    s_expect_home_page = home_page;
    s_expect_overview = -1;
}

static void workload_set_expect_overview(workload_expect_t kind, bool open)
{
    s_expect_pending = true;
    s_expect_kind = kind;
    s_expect_page = UI_PAGE_NONE;
    s_expect_control = -1;
    s_expect_home_page = -1;
    s_expect_overview = open ? 1 : 0;
}

static bool workload_verify_expected(void)
{
    if (!s_expect_pending) {
        return true;
    }
    if (s_expect_page != UI_PAGE_NONE && ui_current_page() != s_expect_page) {
        return false;
    }
    if (s_expect_control >= 0 &&
        (ui_nav_control_open() ? 1 : 0) != s_expect_control) {
        return false;
    }
    if (s_expect_home_page >= 0 && ui_page_home_current_page() != s_expect_home_page) {
        return false;
    }
    if (s_expect_overview >= 0) {
        if ((ui_overview_is_open() ? 1 : 0) != s_expect_overview ||
            ui_overview_is_animating()) {
            return false;
        }
    }

    switch (s_expect_kind) {
    case EXPECT_RESTORE:
        if (s_workload_started_locked) {
            s_stats.verified_unlock++;
        }
        break;
    case EXPECT_APP_ENTER:
        s_stats.verified_app_enter++;
        break;
    case EXPECT_APP_BACK:
        s_stats.verified_app_back++;
        break;
    case EXPECT_CONTROL_OPEN:
        s_stats.verified_control_open++;
        break;
    case EXPECT_CONTROL_CLOSE:
        s_stats.verified_control_close++;
        break;
    case EXPECT_OVERVIEW_OPEN:
        s_stats.verified_overview_open++;
        break;
    case EXPECT_OVERVIEW_CLOSE:
        s_stats.verified_overview_close++;
        break;
    case EXPECT_HOME_NEXT:
        s_stats.verified_home_next++;
        break;
    case EXPECT_HOME_PREV:
        s_stats.verified_home_prev++;
        break;
    default:
        break;
    }
    s_expect_pending = false;
    s_expect_kind = EXPECT_NONE;
    s_expect_page = UI_PAGE_NONE;
    s_expect_control = -1;
    s_expect_home_page = -1;
    s_expect_overview = -1;
    return true;
}

static bool workload_counts_ok(void)
{
    unsigned cycles = s_demo_target_cycles;
    if (s_stats.verify_failures != 0) {
        return false;
    }
    if (s_stats.verified_app_enter < 4 * cycles ||
        s_stats.verified_app_back < 4 * cycles ||
        s_stats.verified_control_open < cycles ||
        s_stats.verified_control_close < cycles ||
        s_stats.verified_overview_open < cycles ||
        s_stats.verified_overview_close < cycles ||
        s_stats.verified_home_next < cycles ||
        s_stats.verified_home_prev < cycles) {
        return false;
    }
    if (s_workload_started_locked && s_stats.verified_unlock < 1) {
        return false;
    }
    return true;
}

static void workload_begin_action(ui_perf_action_kind_t kind)
{
    s_workload_action_begun = false;
    if (!s_bench_mode) {
        return;
    }
    ui_perf_action_begin(kind);
    s_workload_action_begun = true;
}

static bool workload_require_started(void)
{
    if (!s_bench_mode || !s_workload_action_begun) {
        return true;
    }
    if (ui_perf_action_started()) {
        return true;
    }
    if (!ui_nav_idle()) {
        return true;
    }
    s_stats.verify_failures++;
    workload_finish("action-not-started");
    return false;
}

static void workload_restore_home(void)
{
    if (ui_current_page() == UI_PAGE_LOCK) {
        /* Normal user Home is intentionally non-destructive on the lock screen.
         * The workload bootstrap uses the normal unlock route instead. */
        ui_navigate(UI_PAGE_HOME);
    } else {
        ui_home();
    }
}

static void workload_save_user_state(void)
{
    memcpy(s_user_mru, s_mru, sizeof(s_user_mru));
    s_user_mru_count = s_mru_count;
    s_user_settings_section = ui_page_settings_current_section();
    ui_page_calculator_benchmark_snapshot();
    s_user_state_saved = true;
    s_user_state_restored = false;
}

static void workload_restore_user_state(void)
{
    if (!s_user_state_saved) {
        return;
    }
    memcpy(s_mru, s_user_mru, sizeof(s_mru));
    s_mru_count = s_user_mru_count;
    ui_page_settings_restore_section(s_user_settings_section);
    ui_page_calculator_benchmark_restore();
    s_user_state_saved = false;
    s_user_state_restored = true;
}

static void workload_finalize(void)
{
    if (s_demo_timer != NULL) {
        lv_timer_delete(s_demo_timer);
        s_demo_timer = NULL;
    }
    if (s_workload_finish_timer != NULL) {
        lv_timer_delete(s_workload_finish_timer);
        s_workload_finish_timer = NULL;
    }
    workload_restore_user_state();
    bool restored_home = ui_current_page() == UI_PAGE_HOME && ui_nav_idle() &&
                         !ui_nav_control_open();
    if (s_bench_mode) {
        ui_perf_result_meta_t meta = {
            .reason = s_workload_reason,
            .complete = s_workload_complete,
            .verify_ok = s_workload_verify_ok,
            .restored_home = restored_home,
            .state_restored = s_user_state_restored,
            .elapsed_ms = s_workload_elapsed_ms,
            .verified_unlock = s_stats.verified_unlock,
            .verified_app_enter = s_stats.verified_app_enter,
            .verified_app_back = s_stats.verified_app_back,
            .verified_control_open = s_stats.verified_control_open,
            .verified_control_close = s_stats.verified_control_close,
            .verified_overview_open = s_stats.verified_overview_open,
            .verified_overview_close = s_stats.verified_overview_close,
            .verified_home_next = s_stats.verified_home_next,
            .verified_home_prev = s_stats.verified_home_prev,
            .verify_failures = s_stats.verify_failures,
        };
        ui_perf_bench_emit(&meta);
    } else {
        ESP_LOGI(TAG,
                 "NAVDEMO_DONE reason=%s cycles=%u/%u steps=%u elapsed=%ums "
                 "restored_home=%d state_restored=%d verified_unlock=%u app_enter=%u app_back=%u "
                 "control_open=%u control_close=%u overview=%u/%u next=%u prev=%u failures=%u",
                 s_workload_reason, s_demo_cycles, s_demo_target_cycles, s_demo_step,
                 (unsigned)s_workload_elapsed_ms, restored_home ? 1 : 0,
                 s_user_state_restored ? 1 : 0,
                 (unsigned)s_stats.verified_unlock, (unsigned)s_stats.verified_app_enter,
                 (unsigned)s_stats.verified_app_back,
                 (unsigned)s_stats.verified_control_open,
                 (unsigned)s_stats.verified_control_close,
                 (unsigned)s_stats.verified_overview_open,
                 (unsigned)s_stats.verified_overview_close,
                 (unsigned)s_stats.verified_home_next,
                 (unsigned)s_stats.verified_home_prev, (unsigned)s_stats.verify_failures);
    }
    if (s_bench_mode && s_user_state_restored) {
        s_stats.bench_state_restored++;
    }
    s_bench_mode = false;
}

static void workload_finish_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (ui_current_page() == UI_PAGE_HOME && ui_nav_idle()) {
        workload_finalize();
        return;
    }
    if ((int32_t)(lv_tick_get() - s_workload_finish_deadline) >= 0) {
        workload_finalize();
    }
}

static void workload_finish(const char *reason)
{
    if (s_demo_timer == NULL && s_workload_finish_timer == NULL) {
        return;
    }
    if (s_workload_finish_timer != NULL) {
        return;
    }
    if (s_demo_timer != NULL) {
        lv_timer_delete(s_demo_timer);
        s_demo_timer = NULL;
    }

    bool counts_ok = workload_counts_ok();
    const char *final_reason = reason != NULL ? reason : "unknown";
    bool requested_complete = strcmp(final_reason, "complete") == 0;
    if (requested_complete && !counts_ok) {
        final_reason = "verify-failed";
    }
    snprintf(s_workload_reason, sizeof(s_workload_reason), "%s", final_reason);
    s_workload_verify_ok = counts_ok;
    s_workload_complete = requested_complete && counts_ok;
    s_workload_elapsed_ms = lv_tick_get() - s_demo_started_tick;
    ui_perf_bench_stop_measurement();

    workload_restore_home();
    if (ui_current_page() == UI_PAGE_HOME && ui_nav_idle()) {
        workload_finalize();
        return;
    }
    s_workload_finish_deadline = lv_tick_get() + WORKLOAD_FINISH_TIMEOUT_MS;
    s_workload_finish_timer =
        lv_timer_create(workload_finish_timer_cb, DEMO_TIMER_MS, NULL);
}

static void workload_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if ((int32_t)(lv_tick_get() - s_demo_deadline) >= 0) {
        workload_finish("deadline");
        return;
    }
    if (s_expect_pending) {
        if (!ui_nav_idle()) {
            return;
        }
        if (s_expect_overview >= 0 && ui_overview_is_animating()) {
            return;
        }
        if (s_bench_mode && !ui_perf_action_finalized()) {
            return;
        }
        if (!workload_verify_expected()) {
            s_stats.verify_failures++;
            workload_finish("verify-failed");
        }
        return;
    }
    if (!ui_nav_idle()) {
        return;
    }

    if (s_demo_step >= DEMO_STEP_END_CYCLE) {
        s_demo_cycles++;
        if (s_demo_cycles >= s_demo_target_cycles) {
            workload_finish("complete");
            return;
        }
        s_demo_step = DEMO_STEP_SETTINGS;
    }

    s_workload_action_begun = false;
    switch ((demo_step_t)s_demo_step) {
    case DEMO_STEP_RESTORE: {
        bool was_locked = ui_current_page() == UI_PAGE_LOCK;
        if (s_bench_mode && was_locked) {
            workload_begin_action(UI_PERF_ACT_UNLOCK);
        }
        workload_set_expect(EXPECT_RESTORE, UI_PAGE_HOME, -1, -1);
        ui_navigate(UI_PAGE_HOME);
        if (!workload_require_started()) {
            return;
        }
        break;
    }
    case DEMO_STEP_SETTINGS:
        workload_begin_action(UI_PERF_ACT_APP_ENTER);
        workload_set_expect(EXPECT_APP_ENTER, UI_PAGE_SETTINGS, -1, -1);
        ui_navigate(UI_PAGE_SETTINGS);
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_BACK_SETTINGS:
        workload_begin_action(UI_PERF_ACT_APP_BACK);
        workload_set_expect(EXPECT_APP_BACK, UI_PAGE_HOME, -1, -1);
        ui_back();
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_WEATHER:
        workload_begin_action(UI_PERF_ACT_APP_ENTER);
        workload_set_expect(EXPECT_APP_ENTER, UI_PAGE_WEATHER, -1, -1);
        ui_navigate(UI_PAGE_WEATHER);
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_BACK_WEATHER:
        workload_begin_action(UI_PERF_ACT_APP_BACK);
        workload_set_expect(EXPECT_APP_BACK, UI_PAGE_HOME, -1, -1);
        ui_back();
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_MONITOR:
        workload_begin_action(UI_PERF_ACT_APP_ENTER);
        workload_set_expect(EXPECT_APP_ENTER, UI_PAGE_MONITOR, -1, -1);
        ui_navigate(UI_PAGE_MONITOR);
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_BACK_MONITOR:
        workload_begin_action(UI_PERF_ACT_APP_BACK);
        workload_set_expect(EXPECT_APP_BACK, UI_PAGE_HOME, -1, -1);
        ui_back();
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_CALCULATOR:
        workload_begin_action(UI_PERF_ACT_APP_ENTER);
        workload_set_expect(EXPECT_APP_ENTER, UI_PAGE_CALCULATOR, -1, -1);
        ui_navigate(UI_PAGE_CALCULATOR);
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_BACK_CALCULATOR:
        workload_begin_action(UI_PERF_ACT_APP_BACK);
        workload_set_expect(EXPECT_APP_BACK, UI_PAGE_HOME, -1, -1);
        ui_back();
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_CONTROL_OPEN:
        workload_begin_action(UI_PERF_ACT_CONTROL_OPEN);
        workload_set_expect(EXPECT_CONTROL_OPEN, UI_PAGE_CONTROL, 1, -1);
        ui_open_control();
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_CONTROL_CLOSE:
        workload_begin_action(UI_PERF_ACT_CONTROL_CLOSE);
        workload_set_expect(EXPECT_CONTROL_CLOSE, UI_PAGE_HOME, 0, -1);
        ui_close_control();
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_OVERVIEW_OPEN:
        workload_begin_action(UI_PERF_ACT_OVERVIEW_OPEN);
        workload_set_expect_overview(EXPECT_OVERVIEW_OPEN, true);
        ui_open_overview();
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_OVERVIEW_CLOSE:
        workload_begin_action(UI_PERF_ACT_OVERVIEW_CLOSE);
        workload_set_expect_overview(EXPECT_OVERVIEW_CLOSE, false);
        ui_close_overview();
        if (!workload_require_started()) {
            return;
        }
        break;
    case DEMO_STEP_HOME_NEXT: {
        int expected = (ui_page_home_current_page() + 1) % UI_HOME_PAGE_COUNT;
        workload_begin_action(UI_PERF_ACT_HOME_NEXT);
        workload_set_expect(EXPECT_HOME_NEXT, UI_PAGE_HOME, -1, expected);
        ui_page_home_swipe(1);
        if (!workload_require_started()) {
            return;
        }
        break;
    }
    case DEMO_STEP_HOME_PREV: {
        int expected = (ui_page_home_current_page() + UI_HOME_PAGE_COUNT - 1) %
                       UI_HOME_PAGE_COUNT;
        workload_begin_action(UI_PERF_ACT_HOME_PREV);
        workload_set_expect(EXPECT_HOME_PREV, UI_PAGE_HOME, -1, expected);
        ui_page_home_swipe(-1);
        if (!workload_require_started()) {
            return;
        }
        break;
    }
    default:
        break;
    }
    s_demo_step++;
    s_stats.demo_steps++;
}

static void workload_stop_async(void *arg)
{
    (void)arg;
    if (s_demo_timer != NULL || s_workload_finish_timer != NULL) {
        workload_finish("stop");
    }
}

static void workload_start_async(void *arg)
{
    uintptr_t payload = (uintptr_t)arg;
    bool bench = (payload & 0x100U) != 0;
    unsigned cycles = (unsigned)(payload & 0xFFU);
    const char *mode_name = bench ? "NAVBENCH" : "NAVDEMO";

    if (!s_started || s_demo_timer != NULL || s_workload_finish_timer != NULL) {
        ESP_LOGW(TAG, "%s rejected: already running or UI not started", mode_name);
        return;
    }
    if (!ui_nav_idle() || s_modal_open || s_control_overlay != NULL ||
        s_overview_overlay != NULL) {
        ESP_LOGW(TAG, "%s rejected: UI is busy or overlay open", mode_name);
        return;
    }
    ui_page_t start = ui_current_page();
    if (start != UI_PAGE_LOCK && start != UI_PAGE_HOME) {
        ESP_LOGW(TAG, "%s rejected: start from boot lock or Home", mode_name);
        return;
    }
    unsigned max_cycles = bench ? BENCH_MAX_CYCLES : DEMO_MAX_CYCLES;
    if (cycles < 1) {
        cycles = 1;
    }
    if (cycles > max_cycles) {
        cycles = max_cycles;
    }

    panel_settings_t settings;
    panel_services_get_settings(&settings);
    if (bench) {
        if (!settings.animations) {
            ESP_LOGW(TAG, "NAVBENCH rejected: animations are disabled");
            return;
        }
        if (start == UI_PAGE_HOME && ui_page_home_current_page() != 0) {
            ESP_LOGW(TAG, "NAVBENCH rejected: return Home to page 1 first");
            return;
        }
    }
    workload_save_user_state();
    ui_page_settings_reset_for_benchmark();
    if (bench) {
        ui_perf_bench_reset(cycles, start, &settings);
    }

    workload_reset_verification();
    s_bench_mode = bench;
    s_workload_started_locked = start == UI_PAGE_LOCK;
    s_demo_cycles = 0;
    s_demo_target_cycles = cycles;
    s_demo_step = DEMO_STEP_RESTORE;
    s_demo_deadline = lv_tick_get() + DEMO_TIMEOUT_MS;
    s_demo_started_tick = lv_tick_get();
    s_stats.demo_steps = 0;
    if (bench) {
        s_stats.bench_runs++;
    } else {
        s_stats.demo_runs++;
    }
    s_demo_timer = lv_timer_create(workload_timer_cb, DEMO_TIMER_MS, NULL);
    if (bench) {
        ESP_LOGI(TAG, "NAVBENCH_START cycles=%u start_page=%d timeout=%ums",
                 cycles, (int)start, DEMO_TIMEOUT_MS);
    } else {
        ESP_LOGI(TAG, "NAVDEMO_START cycles=%u timeout=%ums steps_per_cycle=%d",
                 cycles, DEMO_TIMEOUT_MS, (int)DEMO_STEP_COUNT);
    }
}

static bool workload_start(unsigned cycles, bool bench)
{
    if (!panel_bsp_lock(1000)) {
        ESP_LOGW(TAG, "could not lock LVGL to start %s",
                 bench ? "navigation benchmark" : "navigation demo");
        return false;
    }
    uintptr_t payload = (uintptr_t)(cycles & 0xFFU) | (bench ? 0x100U : 0U);
    lv_async_call(workload_start_async, (void *)payload);
    panel_bsp_unlock();
    return true;
}

bool ui_nav_demo_start(unsigned cycles)
{
    return workload_start(cycles, false);
}

bool ui_nav_bench_start(unsigned cycles)
{
    return workload_start(cycles, true);
}

void ui_nav_demo_stop(void)
{
    if (!panel_bsp_lock(1000)) {
        ESP_LOGW(TAG, "could not lock LVGL to stop navigation workload");
        return;
    }
    lv_async_call(workload_stop_async, NULL);
    panel_bsp_unlock();
}

void ui_nav_bench_stop(void)
{
    ui_nav_demo_stop();
}

bool ui_nav_demo_active(void)
{
    return s_demo_timer != NULL || s_workload_finish_timer != NULL;
}

bool ui_nav_bench_active(void)
{
    return s_bench_mode && ui_nav_demo_active();
}

bool ui_nav_transition_active(void)
{
    return s_transition_active;
}

bool ui_nav_control_open(void)
{
    return s_control_overlay != NULL || s_nav.control_open;
}

bool ui_nav_idle(void)
{
    return !s_transition_active && !s_control_animating && !s_modal_open && !s_modal_closing &&
           s_pending.kind == UI_NAV_PENDING_NONE && !s_pending_scheduled &&
           !ui_page_home_animating();
}

#ifdef WAVESHARE_HOST_SIM
bool ui_test_is_sleeping(void)
{
    return s_sleeping;
}
#endif

static uint32_t s_seen_alarm_generation;
static uint32_t s_seen_countdown_generation;
static uint32_t s_shown_alarm_generation;
static uint32_t s_shown_countdown_generation;

static void clock_reminder_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_started) {
        return;
    }
    panel_clock_snapshot_t snap;
    panel_services_get_clock(&snap);

    bool alarm_new = snap.alarm_pending &&
                     snap.alarm_generation != s_seen_alarm_generation &&
                     snap.alarm_generation != s_shown_alarm_generation;
    bool countdown_new = snap.countdown_done &&
                         snap.countdown_done_generation != s_seen_countdown_generation &&
                         snap.countdown_done_generation != s_shown_countdown_generation;

    if (alarm_new || countdown_new) {
        char body[224];
        if (alarm_new && countdown_new) {
            snprintf(body, sizeof(body),
                     "每日 %02u:%02u 闹钟时间到。\n倒计时也结束。\n按返回键关闭屏幕提醒。",
                     snap.alarm_hour, snap.alarm_minute);
        } else if (alarm_new) {
            snprintf(body, sizeof(body),
                     "每日 %02u:%02u 闹钟时间到。\n按返回键关闭屏幕提醒。",
                     snap.alarm_hour, snap.alarm_minute);
        } else {
            snprintf(body, sizeof(body), "倒计时结束。\n按返回键关闭屏幕提醒。");
        }
        const char *badge = alarm_new && countdown_new ? "闹钟 / 倒计时" :
                            (alarm_new ? "闹钟" : "倒计时");
        /* Only mark shown after the real overlay was created.  If a city
         * keyboard, notice, control centre, overview or transition is active,
         * ui_show_notice returns false and the pending event is retried. */
        if (ui_show_notice("屏幕提醒", badge, body)) {
            if (alarm_new) s_shown_alarm_generation = snap.alarm_generation;
            if (countdown_new) s_shown_countdown_generation = snap.countdown_done_generation;
        }
    }

    /* Acknowledgement is separate from display: a full command queue must not
     * swallow the event.  Retry until the service confirms the same generation. */
    if (snap.alarm_pending && s_shown_alarm_generation == snap.alarm_generation &&
        panel_services_clock_alarm_ack(snap.alarm_generation) == ESP_OK) {
        s_seen_alarm_generation = snap.alarm_generation;
        s_shown_alarm_generation = 0;
    }
    if (snap.countdown_done &&
        s_shown_countdown_generation == snap.countdown_done_generation &&
        panel_services_clock_countdown_ack(snap.countdown_done_generation) == ESP_OK) {
        s_seen_countdown_generation = snap.countdown_done_generation;
        s_shown_countdown_generation = 0;
    }
}

static void clear_pending_nav(void)
{
    s_pending.kind = UI_NAV_PENDING_NONE;
    s_pending.page = UI_PAGE_NONE;
    s_pending_scheduled = false;
}

static void lock_process_pending(void)
{
    if (!s_lock_pending || !s_started) return;
    panel_music_stop();
    if (s_locking) return;
    if (s_current == UI_PAGE_LOCK) {
        s_lock_pending = false;
        clear_pending_nav();
        return;
    }
    if (s_transition_active || s_modal_closing || ui_overview_is_animating()) return;
    if (s_modal_open) {
        ui_back();
        return;
    }
    if (s_overview_overlay != NULL) {
        ui_close_overview();
        return;
    }
    if (s_control_overlay != NULL) {
        ui_close_control();
        return;
    }

    ui_nav_model_t before = s_nav;
    ui_nav_model_init(&s_nav, UI_PAGE_LOCK);
    s_locking = true;
    if (!start_page_transition(UI_PAGE_LOCK, UI_NAV_MOTION_FADE)) {
        s_nav = before;
        s_locking = false;
        return;
    }
    /* A stale back/home from the cover or a previous transition must not be
     * replayed after the lock page is installed. */
    clear_pending_nav();
    s_lock_pending = false;
}

static void boot_key_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (panel_bsp_boot_key_take_press()) {
        s_lock_pending = true;
    }
    lock_process_pending();
}

void ui_start(void)
{
    if (s_started) {
        return;
    }
    if (!panel_bsp_lock(1000)) {
        ESP_LOGE(TAG, "could not lock LVGL during UI start");
        return;
    }

    ui_perf_attach(lv_display_get_default());
    memset(&s_stats, 0, sizeof(s_stats));
    ui_page_nav_suppress(true);
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    ui_theme_init(settings.theme);
    ui_system_bar_create();

    ui_page_parent_set(NULL);
    s_root = ui_screen_create();
    if (s_root == NULL) {
        panel_bsp_unlock();
        return;
    }
    lv_obj_t *old = lv_screen_active();
    lv_screen_load_anim(s_root, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, false);
    if (old != NULL && old != s_root) {
        lv_obj_delete(old);
    }
    ui_page_parent_set(s_root);

    s_nav.control_open = false;
    ui_nav_model_init(&s_nav, UI_PAGE_LOCK);
    s_current = UI_PAGE_LOCK;
    s_active_page = page_add(UI_PAGE_LOCK);
    if (s_active_page == NULL) {
        panel_bsp_unlock();
        return;
    }
    lv_obj_set_pos(s_active_page, 0, 0);

    lv_indev_t *indev = panel_bsp_input();
    if (indev != NULL) {
        lv_indev_add_event_cb(indev, indev_pressed_cb, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(indev, indev_gesture_cb, LV_EVENT_GESTURE, NULL);
    } else {
        ESP_LOGW(TAG, "no indev available; swipe navigation disabled");
    }

    lv_timer_create(sleep_timer_cb, 500, NULL);
    lv_timer_create(clock_reminder_timer_cb, 250, NULL);
    lv_timer_create(boot_key_timer_cb, 20, NULL);
    s_started = true;
    ui_system_bar_refresh();
    panel_bsp_unlock();

    ESP_LOGI(TAG, "UI started: page lock, 1280x800, theme=%s, brightness=%u",
             ui_theme_effective() == PANEL_THEME_DARK ? "dark" : "light",
             settings.brightness);
}
