#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "panel_network.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_pages.h"
#include "ui_perf.h"
#include "ui_theme.h"
#include "ui_widgets.h"

typedef struct {
    const char *name;
    const char *glyph;
    const lv_font_t *glyph_font;
    uint32_t icon_bg;
    uint32_t icon_fg;
    ui_page_t page;
    bool custom_weather;
} home_app_t;

static const home_app_t s_apps[] = {
    {"设置", LV_SYMBOL_SETTINGS, &lv_font_montserrat_40, 0xEDEDEB, 0x2C2C2B, UI_PAGE_SETTINGS,
     false},
    {"文件", LV_SYMBOL_DIRECTORY, &lv_font_montserrat_40, 0xFCE8D2, 0xE07B24, UI_PAGE_FILES,
     false},
    {"音乐", LV_SYMBOL_AUDIO, &lv_font_montserrat_40, 0xFBE4E8, 0xDD5C70, UI_PAGE_CONTROL, false},
    {"天气", "", NULL, 0xE5F2FC, 0x2783DE, UI_PAGE_WEATHER, true},
    {"日历", "日", &ui_font_32, 0xFBE4E5, 0xD95A5F, UI_PAGE_CALENDAR, false},
    {"计算器", "=", &ui_font_32, 0xFBE8D2, 0xE07B24, UI_PAGE_CALCULATOR, false},
    {"时钟", "时", &ui_font_32, 0xEDEDEB, 0x2C2C2B, UI_PAGE_CLOCK, false},
    {"监视器", LV_SYMBOL_VIDEO, &lv_font_montserrat_40, 0xE3F3E9, 0x2E9E68, UI_PAGE_MONITOR,
     false},
    {"终端", ">_", &lv_font_montserrat_32, 0x2C2C2B, 0xFFFFFF, UI_PAGE_NONE, false},
};

typedef struct {
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_obj_t *clock_pill;
    lv_obj_t *heap_label;
    lv_obj_t *psram_label;
    lv_obj_t *heap_bar;
    lv_obj_t *psram_bar;
    lv_obj_t *page_uptime;
    lv_obj_t *page_heap;
    lv_obj_t *page_psram;
    lv_obj_t *net_label;
    lv_obj_t *weather_icon;
    ui_weather_kind_t weather_kind;
    lv_obj_t *weather_temp;
    lv_obj_t *weather_sub;
    lv_obj_t *weather_badge;
} home_ctx_t;

static home_ctx_t *s_ctx;
static lv_obj_t *s_home_screen;
static lv_obj_t *s_pager_viewport;
static lv_obj_t *s_page_container;
static lv_obj_t *s_pager_outgoing;
static lv_obj_t *s_dots[UI_HOME_PAGE_COUNT];
static int s_home_page;
static int s_pending_swipe;
static uint32_t s_pager_generation;
static bool s_pager_animating;

static const char *weekday_cn(int wday)
{
    static const char *days[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    return (wday >= 0 && wday <= 6) ? days[wday] : "";
}

static void format_bytes(uint32_t bytes, char *buf, size_t len)
{
    if (bytes >= 1024U * 1024U) {
        snprintf(buf, len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, len, "%u KB", (unsigned)(bytes / 1024U));
    }
}

static void format_uptime(uint64_t seconds, char *buf, size_t len)
{
    uint64_t days = seconds / 86400ULL;
    uint64_t hours = (seconds % 86400ULL) / 3600ULL;
    uint64_t mins = (seconds % 3600ULL) / 60ULL;
    if (days > 0) {
        snprintf(buf, len, "%" PRIu64 "天 %02" PRIu64 ":%02" PRIu64, days, hours, mins);
    } else {
        snprintf(buf, len, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, hours, mins,
                 seconds % 60ULL);
    }
}

static void update_network_label(home_ctx_t *ctx);

static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    const char *old = lv_label_get_text(label);
    if (old == NULL || strcmp(old, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static ui_weather_kind_t ui_weather_kind(panel_weather_kind_t kind)
{
    switch (kind) {
    case PANEL_WEATHER_CLEAR: return UI_WEATHER_SUN;
    case PANEL_WEATHER_PARTLY: return UI_WEATHER_PARTLY;
    case PANEL_WEATHER_DRIZZLE:
    case PANEL_WEATHER_RAIN:
    case PANEL_WEATHER_THUNDER: return UI_WEATHER_RAIN;
    default: return UI_WEATHER_CLOUD;
    }
}

static const char *weather_badge_text(const panel_weather_snapshot_t *weather)
{
    if (!weather->calibrated) return "等待校时";
    if (!weather->wifi_connected) return "离线";
    if (weather->refreshing) return "刷新中";
    if (weather->stale) return "缓存";
    if (weather->has_data) return "实时";
    return "等待数据";
}

static const char *weather_condition_text(int code)
{
    if (code == 0) return "晴";
    if (code == 1 || code == 2) return "局部多云";
    if (code == 3) return "多云";
    if (code == 45 || code == 48) return "雾";
    if (code >= 51 && code <= 57) return "毛毛雨";
    if (code >= 61 && code <= 67) return "雨";
    if (code >= 71 && code <= 77) return "雪";
    if (code >= 80 && code <= 82) return "阵雨";
    if (code == 85 || code == 86) return "阵雪";
    if (code >= 95 && code <= 99) return "雷暴";
    return "未知";
}

static void update_weather_card(home_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    panel_weather_snapshot_t weather;
    panel_services_get_weather(&weather);
    if (ctx->clock_pill != NULL) {
        set_text_if_changed(ctx->clock_pill,
                            panel_services_time_is_calibrated() ? "已校时 · CST-8" :
                                                                  "未校时 · 本地时钟");
    }
    if (ctx->weather_temp != NULL) {
        char temp[16];
        if (weather.has_data) {
            snprintf(temp, sizeof(temp), "%d°", weather.current_temp_c);
        } else {
            snprintf(temp, sizeof(temp), "--");
        }
        set_text_if_changed(ctx->weather_temp, temp);
    }
    if (ctx->weather_sub != NULL) {
        char text[96];
        if (weather.has_data) {
            snprintf(text, sizeof(text), "%s · %s",
                     weather_condition_text(weather.weather_code),
                     weather.city[0] ? weather.city : "—");
        } else {
            snprintf(text, sizeof(text), "%s", weather.reason[0] ? weather.reason :
                                                                    "等待天气数据");
        }
        set_text_if_changed(ctx->weather_sub, text);
    }
    if (ctx->weather_badge != NULL) {
        set_text_if_changed(ctx->weather_badge, weather_badge_text(&weather));
    }
    ui_weather_kind_t kind = ui_weather_kind(weather.kind);
    if (ctx->weather_icon != NULL && ctx->weather_kind != kind) {
        lv_obj_t *parent = lv_obj_get_parent(ctx->weather_icon);
        lv_obj_t *old = ctx->weather_icon;
        ctx->weather_icon = ui_weather_icon(parent, kind, 78);
        lv_obj_set_pos(ctx->weather_icon, 22, 30);
        lv_obj_delete(old);
        ctx->weather_kind = kind;
    }
}

static void home_timer_cb(lv_timer_t *timer)
{
    home_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (ctx == NULL) {
        return;
    }
    struct tm now;
    if (panel_services_get_local_time(&now)) {
        lv_label_set_text_fmt(ctx->time_label, "%02d:%02d", now.tm_hour, now.tm_min);
        lv_label_set_text_fmt(ctx->date_label, "%d月%d日 %s", now.tm_mon + 1, now.tm_mday,
                              weekday_cn(now.tm_wday));
    }

    panel_runtime_t runtime;
    panel_services_get_runtime(&runtime);
    char heap_text[24];
    char psram_text[24];
    format_bytes(runtime.heap_free, heap_text, sizeof(heap_text));
    format_bytes(runtime.psram_free, psram_text, sizeof(psram_text));
    lv_label_set_text(ctx->heap_label, heap_text);
    lv_label_set_text(ctx->psram_label, psram_text);
    update_network_label(ctx);
    update_weather_card(ctx);
    if (runtime.heap_total > 0) {
        ui_bar_set(ctx->heap_bar, (int)((runtime.heap_free * 100ULL) / runtime.heap_total));
    }
    if (runtime.psram_total > 0) {
        ui_bar_set(ctx->psram_bar, (int)((runtime.psram_free * 100ULL) / runtime.psram_total));
    }

    if (ctx->page_uptime != NULL) {
        char uptime[32];
        format_uptime(runtime.uptime_s, uptime, sizeof(uptime));
        lv_label_set_text(ctx->page_uptime, uptime);
    }
    if (ctx->page_heap != NULL) {
        lv_label_set_text(ctx->page_heap, heap_text);
    }
    if (ctx->page_psram != NULL) {
        lv_label_set_text(ctx->page_psram, psram_text);
    }
}

static void home_delete_cb(lv_event_t *event)
{
    lv_timer_t *timer = lv_event_get_user_data(event);
    if (timer != NULL) {
        home_ctx_t *ctx = lv_timer_get_user_data(timer);
        lv_timer_delete(timer);
        lv_free(ctx);
    }
    if (lv_event_get_target_obj(event) == s_home_screen) {
        s_ctx = NULL;
        s_home_screen = NULL;
        s_pager_viewport = NULL;
        s_page_container = NULL;
        s_pager_outgoing = NULL;
        s_pending_swipe = 0;
        s_pager_animating = false;
        s_dots[0] = s_dots[1] = NULL;
    }
}

static void app_clicked_cb(lv_event_t *event)
{
    const home_app_t *app = lv_event_get_user_data(event);
    if (app == NULL) {
        return;
    }
    if (app->page != UI_PAGE_NONE) {
        ui_navigate(app->page);
        return;
    }
    char message[64];
    snprintf(message, sizeof(message), "“%s”暂未实现", app->name);
    ui_toast(message);
}

static lv_obj_t *stat_card(lv_obj_t *parent, int x, int y, const char *key, const char *value,
                           lv_obj_t **value_label)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *card = ui_card(parent, x, y, 238, 136);
    ui_label_at(card, 22, 20, key, &ui_font_16, p->muted);
    lv_obj_t *v = ui_label_at(card, 22, 54, value, &ui_font_32, p->text);
    lv_obj_set_width(v, 196);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_CLIP);
    if (value_label != NULL) {
        *value_label = v;
    }
    return card;
}

static void build_page_content(void)
{
    if (s_page_container == NULL || s_ctx == NULL) {
        return;
    }
    const ui_palette_t *p = ui_palette();
    s_ctx->page_uptime = NULL;
    s_ctx->page_heap = NULL;
    s_ctx->page_psram = NULL;
    lv_obj_clean(s_page_container);

    if (s_home_page == 0) {
        int idx = 0;
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++, idx++) {
                if (idx >= (int)(sizeof(s_apps) / sizeof(s_apps[0]))) {
                    break;
                }
                const home_app_t *app = &s_apps[idx];
                lv_obj_t *button = lv_button_create(s_page_container);
                lv_obj_set_pos(button, 28 + col * 256, row * 190);
                lv_obj_set_size(button, 216, 182);
                lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
                lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_STATE_PRESSED);
                lv_obj_set_style_bg_color(button, p->surface_alt, LV_STATE_PRESSED);
                lv_obj_set_style_border_width(button, 0, 0);
                lv_obj_set_style_shadow_width(button, 0, 0);
                lv_obj_set_style_radius(button, 28, 0);
                lv_obj_add_event_cb(button, app_clicked_cb, LV_EVENT_CLICKED, (void *)app);

                lv_obj_t *icon_box = lv_obj_create(button);
                lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_size(icon_box, 108, 108);
                lv_obj_align(icon_box, LV_ALIGN_TOP_MID, 0, 6);
                lv_obj_set_style_bg_color(icon_box, lv_color_hex(app->icon_bg), 0);
                lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(icon_box, 0, 0);
                lv_obj_set_style_radius(icon_box, 28, 0);

                if (app->custom_weather) {
                    lv_obj_t *icon = ui_weather_icon(icon_box, UI_WEATHER_CLOUD, 64);
                    lv_obj_center(icon);
                } else {
                    lv_obj_t *icon = ui_label(icon_box, app->glyph, app->glyph_font,
                                              lv_color_hex(app->icon_fg));
                    lv_obj_center(icon);
                }

                lv_obj_t *name = ui_label(button, app->name, &ui_font_20, p->text);
                lv_obj_set_width(name, 168);
                lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 130);
            }
        }
    } else {
        ui_label_at(s_page_container, 0, 0, "系统概览", &ui_font_32, p->text);
        ui_label_at(s_page_container, 0, 45, "以下数值来自本机运行时接口。", &ui_font_16,
                    p->muted);
        panel_runtime_t runtime;
        panel_services_get_runtime(&runtime);
        char uptime[32];
        char heap_text[24];
        char psram_text[24];
        format_uptime(runtime.uptime_s, uptime, sizeof(uptime));
        format_bytes(runtime.heap_free, heap_text, sizeof(heap_text));
        format_bytes(runtime.psram_free, psram_text, sizeof(psram_text));
        stat_card(s_page_container, 0, 82, "运行时间 · 实测", uptime, &s_ctx->page_uptime);
        stat_card(s_page_container, 264, 82, "内部 RAM 可用 · 实测", heap_text,
                  &s_ctx->page_heap);
        stat_card(s_page_container, 528, 82, "PSRAM 可用 · 实测", psram_text,
                  &s_ctx->page_psram);

        lv_obj_t *note = ui_card(s_page_container, 0, 246, 766, 244);
        ui_label_at(note, 24, 22, "关于本页", &ui_font_24, p->text);
        lv_obj_t *body = ui_label_at(note, 24, 62,
                                     "CPU 占用、芯片温度和帧率需要可靠的运行时接口；当前固件不展示伪造数值，详情见“监视器”。\n\n文件（只读 TF）、音乐（TF 卡）、日历、时钟、天气、计算器和监视器已接入；终端暂未接入。",
                                     &ui_font_20, p->muted);
        lv_obj_set_width(body, 710);
        lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    }

    for (int i = 0; i < UI_HOME_PAGE_COUNT; i++) {
        if (s_dots[i] == NULL) {
            continue;
        }
        bool on = i == s_home_page;
        lv_obj_set_width(s_dots[i], on ? 24 : 8);
        lv_obj_set_style_bg_color(s_dots[i], on ? p->accent : p->border, 0);
    }
}

static void pager_set_x_cb(void *obj, int32_t value)
{
    lv_obj_set_x((lv_obj_t *)obj, value);
}

static void pager_finished_cb(lv_anim_t *anim)
{
    if ((uint32_t)(uintptr_t)lv_anim_get_user_data(anim) != s_pager_generation) {
        return;
    }
    if (s_pager_outgoing != NULL) {
        lv_obj_delete(s_pager_outgoing);
        s_pager_outgoing = NULL;
    }
    s_pager_animating = false;
    ui_perf_action_complete();
    if (s_pending_swipe != 0) {
        int delta = s_pending_swipe;
        s_pending_swipe = 0;
        ui_page_home_swipe(delta);
    }
}

bool ui_page_home_animating(void)
{
    return s_pager_animating;
}

int ui_page_home_current_page(void)
{
    return s_home_page;
}

void ui_page_home_reset_for_benchmark(void)
{
    if (s_page_container == NULL && !s_pager_animating) {
        s_home_page = 0;
    }
}

void ui_page_home_swipe(int delta)
{
    if (s_page_container == NULL || s_pager_viewport == NULL || delta == 0) {
        return;
    }
    int step = delta > 0 ? 1 : -1;
    if (s_pager_animating) {
        s_pending_swipe = step;
        return;
    }

    int old_page = s_home_page;
    int new_page = (old_page + step + UI_HOME_PAGE_COUNT) % UI_HOME_PAGE_COUNT;
    lv_obj_t *outgoing = s_page_container;
    lv_obj_t *incoming = lv_obj_create(s_pager_viewport);
    lv_obj_remove_flag(incoming, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(incoming, step * 768, 0);
    lv_obj_set_size(incoming, 768, 636);
    lv_obj_set_style_bg_opa(incoming, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(incoming, 0, 0);
    lv_obj_set_style_pad_all(incoming, 0, 0);

    s_page_container = incoming;
    s_pager_outgoing = outgoing;
    s_home_page = new_page;
    build_page_content();
    ui_perf_action_stage_start();

    uint32_t duration = ui_anim_time(200);
    if (duration == 0) {
        lv_obj_delete(outgoing);
        s_pager_outgoing = NULL;
        lv_obj_set_x(incoming, 0);
        ui_perf_action_complete();
        return;
    }

    s_pager_animating = true;
    s_pager_generation++;
    uint32_t generation = s_pager_generation;

    lv_anim_t old_anim;
    lv_anim_init(&old_anim);
    lv_anim_set_var(&old_anim, outgoing);
    lv_anim_set_exec_cb(&old_anim, pager_set_x_cb);
    lv_anim_set_values(&old_anim, 0, -step * 768);
    lv_anim_set_duration(&old_anim, duration);
    lv_anim_set_path_cb(&old_anim, lv_anim_path_ease_out);
    lv_anim_start(&old_anim);

    lv_anim_t new_anim;
    lv_anim_init(&new_anim);
    lv_anim_set_var(&new_anim, incoming);
    lv_anim_set_exec_cb(&new_anim, pager_set_x_cb);
    lv_anim_set_values(&new_anim, step * 768, 0);
    lv_anim_set_duration(&new_anim, duration);
    lv_anim_set_path_cb(&new_anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&new_anim, pager_finished_cb);
    lv_anim_set_user_data(&new_anim, (void *)(uintptr_t)generation);
    lv_anim_start(&new_anim);
}

static lv_obj_t *clock_card(lv_obj_t *parent, home_ctx_t *ctx)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *card = ui_card(parent, 40, 82, 400, 220);
    ctx->time_label = ui_label_at(card, 24, 18, "--:--", &ui_font_num_80, p->text);
    ctx->date_label = ui_label_at(card, 28, 118, "", &ui_font_20, p->muted);
    lv_obj_t *pill = ui_pill(card, "未校时 · 本地时钟", p->accent, p->accent_soft, &ui_font_16);
    lv_obj_set_pos(pill, 28, 164);
    ctx->clock_pill = pill;
    return card;
}

static lv_obj_t *weather_card(lv_obj_t *parent, home_ctx_t *ctx)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *card = ui_card(parent, 40, 318, 400, 150);
    lv_obj_t *icon = ui_weather_icon(card, UI_WEATHER_CLOUD, 78);
    lv_obj_set_pos(icon, 22, 30);
    ctx->weather_icon = icon;
    ctx->weather_kind = UI_WEATHER_CLOUD;
    ctx->weather_temp = ui_label_at(card, 116, 24, "--", &ui_font_num_48, p->text);
    ctx->weather_sub = ui_label_at(card, 118, 84, "等待天气数据", &ui_font_20, p->text);
    lv_obj_set_width(ctx->weather_sub, 260);
    lv_label_set_long_mode(ctx->weather_sub, LV_LABEL_LONG_MODE_DOTS);
    ctx->weather_badge = ui_pill(card, "等待校时", p->accent, p->accent_soft, &ui_font_16);
    lv_obj_set_pos(ctx->weather_badge, 286, 30);
    return card;
}

static void update_network_label(home_ctx_t *ctx)
{
    if (ctx == NULL || ctx->net_label == NULL) {
        return;
    }
    panel_wifi_snapshot_t wifi;
    panel_ble_snapshot_t ble;
    panel_network_wifi_get(&wifi);
    panel_network_ble_get(&ble);
    char text[96];
    snprintf(text, sizeof(text), "Wi-Fi %s · 蓝牙 %s",
             wifi.connected ? "已连接" : "未连接",
             ble.state == PANEL_BLE_CONNECTED ? "已连接" : "未连接");
    lv_label_set_text(ctx->net_label, text);
}

static lv_obj_t *system_card(lv_obj_t *parent, home_ctx_t *ctx)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *card = ui_card(parent, 40, 484, 400, 218);
    ui_label_at(card, 24, 20, "可用堆内存", &ui_font_16, p->muted);
    ctx->heap_label = ui_label_at(card, 210, 14, "--", &ui_font_24, p->text);
    lv_obj_set_width(ctx->heap_label, 166);
    lv_obj_set_style_text_align(ctx->heap_label, LV_TEXT_ALIGN_RIGHT, 0);
    ctx->heap_bar = ui_bar_create(card, 24, 58, 352, 8);

    ui_label_at(card, 24, 90, "PSRAM 可用", &ui_font_16, p->muted);
    ctx->psram_label = ui_label_at(card, 210, 84, "--", &ui_font_24, p->text);
    lv_obj_set_width(ctx->psram_label, 166);
    lv_obj_set_style_text_align(ctx->psram_label, LV_TEXT_ALIGN_RIGHT, 0);
    ctx->psram_bar = ui_bar_create(card, 24, 128, 352, 8);

    ctx->net_label = ui_label_at(card, 24, 158, "", &ui_font_16, p->muted);
    update_network_label(ctx);
    lv_obj_t *pill = ui_pill(card, "运行时实测", p->success, p->surface_alt, &ui_font_16);
    lv_obj_set_pos(pill, 264, 152);
    return card;
}

lv_obj_t *ui_page_home_create(void)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *screen = ui_screen_create();
    s_pager_outgoing = NULL;
    s_pending_swipe = 0;
    s_pager_animating = false;
    s_pager_generation++;
    s_home_screen = screen;
    ui_status_bar(screen, false);

    home_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return screen;
    }
    memset(ctx, 0, sizeof(*ctx));
    s_ctx = ctx;

    clock_card(screen, ctx);
    weather_card(screen, ctx);
    system_card(screen, ctx);

    ui_label_at(screen, 472, 58, "应用", &ui_font_20, p->muted);

    s_pager_viewport = lv_obj_create(screen);
    lv_obj_remove_flag(s_pager_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_pager_viewport, 472, 90);
    lv_obj_set_size(s_pager_viewport, 768, 636);
    lv_obj_set_style_bg_opa(s_pager_viewport, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pager_viewport, 0, 0);
    lv_obj_set_style_pad_all(s_pager_viewport, 0, 0);
    lv_obj_set_style_radius(s_pager_viewport, 0, 0);

    s_page_container = lv_obj_create(s_pager_viewport);
    lv_obj_remove_flag(s_page_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_page_container, 0, 0);
    lv_obj_set_size(s_page_container, 768, 636);
    lv_obj_set_style_bg_opa(s_page_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page_container, 0, 0);
    lv_obj_set_style_pad_all(s_page_container, 0, 0);

    lv_obj_t *dots = lv_obj_create(screen);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(dots, 472, 700);
    lv_obj_set_size(dots, 768, 24);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_style_pad_all(dots, 0, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 10, 0);
    for (int i = 0; i < UI_HOME_PAGE_COUNT; i++) {
        s_dots[i] = lv_obj_create(dots);
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_dots[i], 8, 8);
        lv_obj_set_style_radius(s_dots[i], 4, 0);
        lv_obj_set_style_bg_color(s_dots[i], p->border, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_dots[i], 0, 0);
    }

    build_page_content();
    ui_nav_bar(screen, UI_PAGE_HOME);
    lv_timer_t *timer = lv_timer_create(home_timer_cb, 1000, ctx);
    home_timer_cb(timer);
    lv_obj_add_event_cb(screen, home_delete_cb, LV_EVENT_DELETE, timer);
    return screen;
}
