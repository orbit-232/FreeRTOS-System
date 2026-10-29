#include "ui_pages.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#define WEATHER_HOURS 6
#define WEATHER_DAYS 5

typedef struct {
    unsigned index;
    uint32_t search_generation;
} weather_city_row_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *city_label;
    lv_obj_t *state_pill;
    lv_obj_t *hero_icon;
    ui_weather_kind_t hero_kind;
    lv_obj_t *temp_label;
    lv_obj_t *condition_label;
    lv_obj_t *highlow_label;
    lv_obj_t *feels_value;
    lv_obj_t *humidity_value;
    lv_obj_t *wind_value;
    lv_obj_t *hour_label[WEATHER_HOURS];
    lv_obj_t *hour_temp[WEATHER_HOURS];
    lv_obj_t *hour_icon[WEATHER_HOURS];
    ui_weather_kind_t hour_kind[WEATHER_HOURS];
    lv_obj_t *hour_badge;
    lv_obj_t *day_label[WEATHER_DAYS];
    lv_obj_t *day_low[WEATHER_DAYS];
    lv_obj_t *day_high[WEATHER_DAYS];
    lv_obj_t *day_icon[WEATHER_DAYS];
    ui_weather_kind_t day_kind[WEATHER_DAYS];
    lv_obj_t *day_track[WEATHER_DAYS];
    lv_obj_t *day_fill[WEATHER_DAYS];
    lv_obj_t *note;
    lv_obj_t *refresh_feedback;
    uint8_t refresh_feedback_ttl;
    lv_timer_t *timer;
    lv_obj_t *modal;
    lv_obj_t *modal_ta;
    lv_obj_t *modal_error;
    lv_obj_t *modal_list;
    lv_obj_t *modal_keyboard;
    uint32_t seen_weather;
    uint32_t seen_search;
    panel_weather_snapshot_t weather;
    panel_city_search_snapshot_t search;
} weather_ctx_t;

static weather_ctx_t *s_ctx;

static void modal_city_cb(lv_event_t *event);
static void modal_row_delete_cb(lv_event_t *event);

static const char *weekday_cn(int weekday)
{
    static const char *days[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    return (weekday >= 0 && weekday <= 6) ? days[weekday] : "";
}

static const char *weather_text(int code)
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

static ui_weather_kind_t ui_kind(panel_weather_kind_t kind)
{
    switch (kind) {
    case PANEL_WEATHER_CLEAR: return UI_WEATHER_SUN;
    case PANEL_WEATHER_PARTLY: return UI_WEATHER_PARTLY;
    case PANEL_WEATHER_DRIZZLE:
    case PANEL_WEATHER_RAIN:
    case PANEL_WEATHER_THUNDER: return UI_WEATHER_RAIN;
    case PANEL_WEATHER_CLOUDY:
    case PANEL_WEATHER_FOG:
    case PANEL_WEATHER_SNOW:
    default: return UI_WEATHER_CLOUD;
    }
}

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

static void set_icon(lv_obj_t **slot, lv_obj_t *parent, int x, int y, int size,
                     ui_weather_kind_t kind, ui_weather_kind_t *current)
{
    if (*slot != NULL && *current == kind) {
        return;
    }
    if (*slot != NULL) {
        lv_obj_delete(*slot);
    }
    *slot = ui_weather_icon(parent, kind, size);
    lv_obj_set_pos(*slot, x, y);
    *current = kind;
}

static void format_clock(int64_t epoch, char *buf, size_t len)
{
    if (epoch <= 0) {
        snprintf(buf, len, "--:--");
        return;
    }
    time_t value = (time_t)epoch;
    struct tm *tmp = localtime(&value);
    if (tmp == NULL) {
        snprintf(buf, len, "--:--");
        return;
    }
    struct tm tm_value = *tmp;
    snprintf(buf, len, "%02d:%02d", tm_value.tm_hour, tm_value.tm_min);
}

static const char *state_text(const panel_weather_snapshot_t *weather)
{
    if (!weather->calibrated) return "未校时";
    if (!weather->wifi_connected) return "离线";
    if (weather->refreshing) return "刷新中";
    if (weather->stale) return "缓存";
    if (weather->has_data) return "实时";
    return "等待数据";
}

static void update_state_pill(weather_ctx_t *ctx, const panel_weather_snapshot_t *weather)
{
    const ui_palette_t *p = ui_palette();
    lv_color_t fg = p->muted;
    lv_color_t bg = p->surface_alt;
    if (weather->has_data && weather->wifi_connected && weather->calibrated &&
        !weather->stale && !weather->refreshing) {
        fg = p->success;
        bg = p->surface_alt;
    } else if (weather->stale || !weather->wifi_connected) {
        fg = p->warning;
        bg = p->surface_alt;
    } else if (!weather->calibrated) {
        fg = p->accent;
        bg = p->accent_soft;
    }
    set_text_if_changed(ctx->state_pill, state_text(weather));
    lv_obj_set_style_text_color(ctx->state_pill, fg, 0);
    lv_obj_set_style_bg_color(ctx->state_pill, bg, 0);
}

static void update_note(weather_ctx_t *ctx, const panel_weather_snapshot_t *weather)
{
    char updated[8];
    char data[8];
    format_clock(weather->updated_epoch, updated, sizeof(updated));
    format_clock(weather->data_epoch, data, sizeof(data));
    char text[192];
    if (!weather->has_data) {
        if (!weather->wifi_connected) {
            snprintf(text, sizeof(text), "等待网络连接后自动获取天气");
        } else if (!weather->calibrated) {
            snprintf(text, sizeof(text), "已联网，等待自动校时后获取天气");
        } else if (weather->refreshing) {
            snprintf(text, sizeof(text), "正在获取天气…");
        } else if (weather->reason[0]) {
            snprintf(text, sizeof(text), "天气不可用：%s", weather->reason);
        } else {
            snprintf(text, sizeof(text), "等待天气数据");
        }
    } else if (!weather->wifi_connected) {
        snprintf(text, sizeof(text), "离线 · 显示上次缓存 %s · 数据 %s", updated, data);
    } else if (weather->stale) {
        snprintf(text, sizeof(text), "缓存 · 上次成功 %s · 数据 %s", updated, data);
    } else if (weather->refreshing) {
        snprintf(text, sizeof(text), "正在刷新 · 上次更新 %s · 数据 %s", updated, data);
    } else {
        snprintf(text, sizeof(text), "更新 %s · 数据 %s · Open-Meteo", updated, data);
    }
    set_text_if_changed(ctx->note, text);
}

static void update_weather(weather_ctx_t *ctx, const panel_weather_snapshot_t *weather)
{
    const ui_palette_t *p = ui_palette();
    set_text_if_changed(ctx->city_label, weather->city[0] ? weather->city : "—");
    update_state_pill(ctx, weather);
    update_note(ctx, weather);

    set_icon(&ctx->hero_icon, ctx->screen, 40, 174, 116,
             ui_kind(weather->kind), &ctx->hero_kind);

    char temp[24];
    if (weather->has_data) {
        snprintf(temp, sizeof(temp), "%d°", weather->current_temp_c);
    } else {
        snprintf(temp, sizeof(temp), "--");
    }
    set_text_if_changed(ctx->temp_label, temp);

    const char *condition = weather->has_data ? weather_text(weather->weather_code) : "--";
    set_text_if_changed(ctx->condition_label, condition);

    char highlow[64];
    if (weather->has_data && weather->day_count > 0) {
        snprintf(highlow, sizeof(highlow), "最高 %d° · 最低 %d°",
                 weather->days[0].temp_max_c, weather->days[0].temp_min_c);
    } else {
        snprintf(highlow, sizeof(highlow), "--");
    }
    set_text_if_changed(ctx->highlow_label, highlow);

    if (weather->has_data) {
        snprintf(temp, sizeof(temp), "%d°", weather->apparent_temp_c);
        set_text_if_changed(ctx->feels_value, temp);
        snprintf(temp, sizeof(temp), "%d%%", weather->humidity_pct);
        set_text_if_changed(ctx->humidity_value, temp);
        snprintf(temp, sizeof(temp), "%.1f km/h", weather->wind_speed_kmh);
        set_text_if_changed(ctx->wind_value, temp);
    } else {
        set_text_if_changed(ctx->feels_value, "--");
        set_text_if_changed(ctx->humidity_value, "--");
        set_text_if_changed(ctx->wind_value, "--");
    }

    set_text_if_changed(ctx->hour_badge,
                        !weather->calibrated ? "等待校时" :
                        (!weather->wifi_connected ? "离线" :
                         (weather->stale ? "缓存" : "逐小时")));
    for (int i = 0; i < WEATHER_HOURS; i++) {
        char label[24];
        char value[12];
        if (i < (int)weather->hour_count) {
            if (weather->hours[i].day_offset == 0) {
                snprintf(label, sizeof(label), "%02d时", weather->hours[i].hour);
            } else {
                snprintf(label, sizeof(label), "明天%02d时", weather->hours[i].hour);
            }
            snprintf(value, sizeof(value), "%d°", weather->hours[i].temp_c);
            set_icon(&ctx->hour_icon[i], lv_obj_get_parent(ctx->hour_label[i]),
                     22 + i * 119 + 36, 78, 42, ui_kind(weather->hours[i].kind),
                     &ctx->hour_kind[i]);
        } else {
            snprintf(label, sizeof(label), "--");
            snprintf(value, sizeof(value), "--");
        }
        set_text_if_changed(ctx->hour_label[i], label);
        set_text_if_changed(ctx->hour_temp[i], value);
    }

    for (int i = 0; i < WEATHER_DAYS; i++) {
        char label[24];
        char low[12];
        char high[12];
        if (i < (int)weather->day_count) {
            const panel_online_day_t *day = &weather->days[i];
            if (day->day_offset == 0) {
                snprintf(label, sizeof(label), "今天");
            } else if (day->day_offset == 1) {
                snprintf(label, sizeof(label), "明天");
            } else {
                snprintf(label, sizeof(label), "%s", weekday_cn(day->weekday));
            }
            snprintf(low, sizeof(low), "%d°", day->temp_min_c);
            snprintf(high, sizeof(high), "%d°", day->temp_max_c);
            set_icon(&ctx->day_icon[i], lv_obj_get_parent(ctx->day_label[i]),
                     104, 62 + i * 56 - 2, 32, ui_kind(day->kind), &ctx->day_kind[i]);
            int low_c = day->temp_min_c;
            int high_c = day->temp_max_c;
            if (low_c < -10) low_c = -10;
            if (high_c > 40) high_c = 40;
            if (high_c < low_c) high_c = low_c;
            int left = (low_c + 10) * 8;
            int width = (high_c - low_c) * 8;
            if (width < 6) width = 6;
            if (left + width > 400) left = 400 - width;
            if (left < 0) left = 0;
            lv_obj_set_pos(ctx->day_fill[i], left, 0);
            lv_obj_set_size(ctx->day_fill[i], width, 6);
        } else {
            snprintf(label, sizeof(label), "--");
            snprintf(low, sizeof(low), "--");
            snprintf(high, sizeof(high), "--");
            lv_obj_set_size(ctx->day_fill[i], 0, 6);
        }
        set_text_if_changed(ctx->day_label[i], label);
        set_text_if_changed(ctx->day_low[i], low);
        set_text_if_changed(ctx->day_high[i], high);
    }
    (void)p;
}

static void weather_timer_cb(lv_timer_t *timer)
{
    weather_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (ctx == NULL) {
        return;
    }
    if (ctx->refresh_feedback_ttl > 0) {
        ctx->refresh_feedback_ttl--;
        if (ctx->refresh_feedback_ttl == 0) {
            set_text_if_changed(ctx->refresh_feedback, "");
        }
    }
    panel_weather_snapshot_t weather;
    panel_services_get_weather(&weather);
    ctx->weather = weather;
    update_weather(ctx, &weather);

    if (ctx->modal != NULL) {
        panel_city_search_snapshot_t search;
        panel_services_get_city_search(&search);
        if (search.generation != ctx->seen_search) {
            ctx->seen_search = search.generation;
            ctx->search = search;
            lv_obj_clean(ctx->modal_list);
            for (unsigned i = 0; i < search.count; i++) {
                const panel_city_result_t *city = &search.results[i];
                char text[160];
                snprintf(text, sizeof(text), "%s · %s · %s", city->name,
                         city->region[0] ? city->region : "-",
                         city->country[0] ? city->country : "-");
                lv_obj_t *row = lv_button_create(ctx->modal_list);
                lv_obj_set_size(row, 1024, 32);
                lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(row, 0, 0);
                lv_obj_set_style_pad_all(row, 0, 0);
                weather_city_row_t *row_ref = lv_malloc(sizeof(*row_ref));
                if (row_ref == NULL) {
                    lv_obj_delete(row);
                    continue;
                }
                memset(row_ref, 0, sizeof(*row_ref));
                row_ref->index = i;
                row_ref->search_generation = search.generation;
                lv_obj_set_user_data(row, row_ref);
                lv_obj_add_event_cb(row, modal_city_cb, LV_EVENT_CLICKED, ctx);
                lv_obj_add_event_cb(row, modal_row_delete_cb, LV_EVENT_DELETE, row_ref);
                ui_label_at(row, 8, 5, text, &ui_font_16, ui_palette()->text);
            }
            if (search.count == 0 && ctx->modal_error != NULL &&
                search.error[0] != '\0') {
                set_text_if_changed(ctx->modal_error, search.error);
            }
        }
        if (ctx->modal_error != NULL) {
            if (search.searching) {
                set_text_if_changed(ctx->modal_error, "搜索中…");
            } else if (search.error[0] != '\0') {
                set_text_if_changed(ctx->modal_error, search.error);
            } else if (search.count > 0) {
                set_text_if_changed(ctx->modal_error, "选择城市后自动刷新天气");
            }
        }
    }
}

static void modal_close(weather_ctx_t *ctx)
{
    if (ctx == NULL || ctx->modal == NULL) {
        return;
    }
    lv_obj_t *modal = ctx->modal;
    ctx->modal = NULL;
    ctx->modal_ta = NULL;
    ctx->modal_error = NULL;
    ctx->modal_list = NULL;
    ctx->modal_keyboard = NULL;
    lv_obj_delete(modal);
}

static void modal_cancel_cb(lv_event_t *event)
{
    weather_ctx_t *ctx = lv_event_get_user_data(event);
    modal_close(ctx);
}

static void modal_search_now(weather_ctx_t *ctx)
{
    if (ctx == NULL || ctx->modal_ta == NULL) {
        return;
    }
    const char *query = lv_textarea_get_text(ctx->modal_ta);
    if (query == NULL || query[0] == '\0') {
        if (ctx->modal_error != NULL) lv_label_set_text(ctx->modal_error, "请输入城市名");
        return;
    }
    esp_err_t err = panel_services_weather_search(query);
    if (err != ESP_OK && ctx->modal_error != NULL) {
        lv_label_set_text(ctx->modal_error,
                          err == ESP_ERR_TIMEOUT ? "命令队列忙，请重试" : "城市查询无效");
    }
}

static void modal_search_cb(lv_event_t *event)
{
    modal_search_now(lv_event_get_user_data(event));
}

static void modal_keyboard_cb(lv_event_t *event)
{
    weather_ctx_t *ctx = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CANCEL) {
        modal_close(ctx);
    } else if (code == LV_EVENT_READY) {
        modal_search_now(ctx);
    }
}

static void modal_row_delete_cb(lv_event_t *event)
{
    lv_free(lv_event_get_user_data(event));
}

static void modal_city_cb(lv_event_t *event)
{
    weather_ctx_t *ctx = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_current_target_obj(event);
    weather_city_row_t *ref = lv_obj_get_user_data(row);
    if (ctx == NULL || ref == NULL) {
        return;
    }
    esp_err_t err = panel_services_weather_choose_city(ref->index, ref->search_generation);
    if (err == ESP_OK) {
        modal_close(ctx);
    } else if (ctx->modal_error != NULL) {
        lv_label_set_text(ctx->modal_error,
                          err == ESP_ERR_TIMEOUT ? "命令队列忙，请重试" :
                          err == ESP_ERR_INVALID_STATE ? "结果已更新，请重新搜索" :
                          "切换城市失败");
    }
}

static void modal_open(weather_ctx_t *ctx)
{
    if (ctx == NULL || ctx->modal != NULL) {
        return;
    }
    modal_close(ctx);
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);

    lv_obj_t *panel = ui_card(overlay, 80, 40, 1120, 720);
    ui_label_at(panel, 28, 18, "选择城市", &ui_font_32, ui_palette()->text);
    ui_label_at(panel, 28, 58, "输入英文或拼音，例如 Guangzhou / Shenzhen",
                &ui_font_16, ui_palette()->muted);
    lv_obj_t *ta = lv_textarea_create(panel);
    lv_obj_set_pos(ta, 28, 94);
    lv_obj_set_size(ta, 700, 48);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 48);
    lv_textarea_set_placeholder_text(ta, "城市名");
    lv_obj_set_style_text_font(ta, &ui_font_20, 0);

    ui_button(panel, 744, 94, 120, 48, "搜索", &ui_font_16, modal_search_cb, ctx);
    ui_button(panel, 880, 94, 120, 48, "取消", &ui_font_16, modal_cancel_cb, ctx);
    lv_obj_t *error = ui_label_at(panel, 28, 152, "", &ui_font_16, ui_palette()->danger);
    lv_obj_set_width(error, 1040);
    lv_obj_set_style_text_color(error, lv_color_hex(0xC0392B), 0);

    lv_obj_t *list = lv_obj_create(panel);
    lv_obj_set_pos(list, 28, 184);
    lv_obj_set_size(list, 1040, 208);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *keyboard = lv_keyboard_create(panel);
    lv_obj_set_align(keyboard, LV_ALIGN_DEFAULT);
    lv_obj_set_pos(keyboard, 28, 398);
    lv_obj_set_size(keyboard, 1060, 280);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(keyboard, ta);
    lv_obj_add_event_cb(keyboard, modal_keyboard_cb, LV_EVENT_ALL, ctx);

    ctx->modal = overlay;
    ctx->modal_ta = ta;
    ctx->modal_error = error;
    ctx->modal_list = list;
    ctx->modal_keyboard = keyboard;
    ctx->seen_search = 0;
}

static void city_switch_cb(lv_event_t *event)
{
    weather_ctx_t *ctx = lv_event_get_user_data(event);
    modal_open(ctx);
}

static void refresh_clicked_cb(lv_event_t *event)
{
    weather_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }
    panel_weather_snapshot_t weather;
    panel_services_get_weather(&weather);
    esp_err_t err = panel_services_weather_refresh();
    const char *feedback = "刷新失败";
    if (err == ESP_OK) {
        feedback = !weather.wifi_connected ? "等待网络" :
                   (!weather.calibrated ? "等待校时" : "已请求刷新");
    } else if (err == ESP_ERR_NOT_FINISHED) {
        feedback = "稍后再试";
    } else if (err == ESP_ERR_TIMEOUT) {
        feedback = "命令队列忙";
    } else if (err == ESP_ERR_INVALID_STATE) {
        feedback = !weather.wifi_connected ? "等待网络" :
                   (!weather.calibrated ? "等待校时" : "等待数据");
    }
    set_text_if_changed(ctx->refresh_feedback, feedback);
    ctx->refresh_feedback_ttl = 3;
}

static void weather_delete_cb(lv_event_t *event)
{
    weather_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }
    modal_close(ctx);
    if (ctx->timer != NULL) {
        lv_timer_delete(ctx->timer);
        ctx->timer = NULL;
    }
    if (s_ctx == ctx) {
        s_ctx = NULL;
    }
    lv_free(ctx);
}

static lv_obj_t *meta_card(lv_obj_t *parent, int x, const char *key, lv_obj_t **value)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *card = ui_card(parent, x, 548, 126, 92);
    ui_label_at(card, 14, 12, key, &ui_font_16, p->muted);
    *value = ui_label_at(card, 14, 44, "--", &ui_font_24, p->text);
    return card;
}

lv_obj_t *ui_page_weather_create(void)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *screen = ui_screen_create();
    if (ui_theme_effective() == PANEL_THEME_DARK) {
        ui_screen_gradient(screen, p->accent_soft, p->bg);
    } else {
        ui_screen_gradient(screen, lv_color_hex(0xE5F2FC), lv_color_hex(0xF9F8F7));
    }
    ui_status_bar(screen, false);
    lv_obj_t *header = ui_header(screen, "天气", NULL);

    weather_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return screen;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->screen = screen;
    s_ctx = ctx;

    ctx->city_label = ui_label_at(screen, 40, 144, "广州", &ui_font_16, p->muted);
    lv_obj_set_width(ctx->city_label, 270);
    lv_label_set_long_mode(ctx->city_label, LV_LABEL_LONG_MODE_DOTS);
    ctx->state_pill = ui_pill(screen, "等待数据", p->accent, p->accent_soft, &ui_font_16);
    lv_obj_set_pos(ctx->state_pill, 330, 142);
    /* Keep the city button in the header: the hourly card starts at y=150. */
    ui_button(header, 840, 8, 140, 44, "刷新", &ui_font_16, refresh_clicked_cb, ctx);
    ui_button(header, 1000, 8, 200, 44, "切换城市", &ui_font_16, city_switch_cb, ctx);
    ctx->refresh_feedback = ui_label_at(header, 840, 56, "", &ui_font_16, p->muted);
    lv_obj_set_width(ctx->refresh_feedback, 150);
    lv_label_set_long_mode(ctx->refresh_feedback, LV_LABEL_LONG_MODE_DOTS);

    ctx->hero_icon = ui_weather_icon(screen, UI_WEATHER_CLOUD, 116);
    lv_obj_set_pos(ctx->hero_icon, 40, 174);
    ctx->hero_kind = UI_WEATHER_CLOUD;
    ctx->temp_label = ui_label_at(screen, 40, 290, "--", &ui_font_num_128, p->text);
    ctx->condition_label = ui_label_at(screen, 44, 448, "--", &ui_font_24, p->text);
    lv_obj_set_width(ctx->condition_label, 400);
    ctx->highlow_label = ui_label_at(screen, 44, 486, "--", &ui_font_20, p->muted);
    lv_obj_set_width(ctx->highlow_label, 400);

    meta_card(screen, 40, "体感", &ctx->feels_value);
    meta_card(screen, 177, "湿度", &ctx->humidity_value);
    meta_card(screen, 314, "风速", &ctx->wind_value);

    lv_obj_t *hourly = ui_card(screen, 500, 150, 740, 168);
    ui_label_at(hourly, 24, 16, "逐小时预报", &ui_font_20, p->text);
    ctx->hour_badge = ui_pill(hourly, "等待校时", p->muted, p->surface_alt, &ui_font_16);
    lv_obj_set_pos(ctx->hour_badge, 618, 12);
    lv_obj_t *hour_parent = hourly;
    for (int i = 0; i < WEATHER_HOURS; i++) {
        int x = 22 + i * 119;
        ctx->hour_label[i] = ui_label_at(hour_parent, x, 54, "--", &ui_font_16, p->muted);
        ctx->hour_icon[i] = ui_weather_icon(hour_parent, UI_WEATHER_CLOUD, 42);
        lv_obj_set_pos(ctx->hour_icon[i], x + 36, 78);
        ctx->hour_kind[i] = UI_WEATHER_CLOUD;
        ctx->hour_temp[i] = ui_label_at(hour_parent, x + 32, 124, "--", &ui_font_20, p->text);
    }

    lv_obj_t *daily = ui_card(screen, 500, 336, 740, 354);
    ui_label_at(daily, 24, 16, "未来几天", &ui_font_20, p->text);
    ui_label_at(daily, 544, 18, "最低 / 最高", &ui_font_16, p->muted);
    for (int i = 0; i < WEATHER_DAYS; i++) {
        int y = 62 + i * 56;
        ctx->day_label[i] = ui_label_at(daily, 24, y, "--", &ui_font_20, p->text);
        ctx->day_icon[i] = ui_weather_icon(daily, UI_WEATHER_CLOUD, 32);
        lv_obj_set_pos(ctx->day_icon[i], 104, y - 2);
        ctx->day_kind[i] = UI_WEATHER_CLOUD;
        ctx->day_low[i] = ui_label_at(daily, 166, y + 2, "--", &ui_font_16, p->muted);
        lv_obj_t *track = lv_obj_create(daily);
        lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(track, 218, y + 10);
        lv_obj_set_size(track, 400, 6);
        lv_obj_set_style_radius(track, 3, 0);
        lv_obj_set_style_bg_color(track, p->surface_alt, 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(track, 0, 0);
        lv_obj_set_style_pad_all(track, 0, 0);
        lv_obj_t *fill = lv_obj_create(track);
        lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(fill, 0, 6);
        lv_obj_set_style_radius(fill, 3, 0);
        lv_obj_set_style_bg_color(fill, lv_color_hex(0x5E9FE8), 0);
        lv_obj_set_style_bg_grad_color(fill, lv_color_hex(0xEAC26B), 0);
        lv_obj_set_style_bg_grad_dir(fill, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(fill, 0, 0);
        lv_obj_set_style_pad_all(fill, 0, 0);
        ctx->day_track[i] = track;
        ctx->day_fill[i] = fill;
        ctx->day_high[i] = ui_label_at(daily, 634, y + 2, "--", &ui_font_16, p->text);
    }

    ctx->note = ui_label_at(screen, 40, 700, "等待网络与校时", &ui_font_16, p->muted);
    lv_obj_set_width(ctx->note, 1160);
    lv_label_set_long_mode(ctx->note, LV_LABEL_LONG_MODE_DOTS);
    ui_nav_bar(screen, UI_PAGE_WEATHER);

    ctx->timer = lv_timer_create(weather_timer_cb, 1000, ctx);
    lv_obj_add_event_cb(screen, weather_delete_cb, LV_EVENT_DELETE, ctx);
    panel_weather_snapshot_t weather;
    panel_services_get_weather(&weather);
    ctx->weather = weather;
    ctx->seen_weather = weather.generation;
    update_weather(ctx, &weather);
    return screen;
}

bool ui_weather_panel_back(void)
{
    if (s_ctx == NULL || s_ctx->modal == NULL) {
        return false;
    }
    modal_close(s_ctx);
    return true;
}

#ifdef WAVESHARE_HOST_SIM
void ui_weather_panel_test_open(void)
{
    modal_open(s_ctx);
}

bool ui_weather_panel_test_modal_open(void)
{
    return s_ctx != NULL && s_ctx->modal != NULL;
}

void ui_weather_panel_test_set_query(const char *query)
{
    if (s_ctx != NULL && s_ctx->modal_ta != NULL) {
        lv_textarea_set_text(s_ctx->modal_ta, query != NULL ? query : "");
    }
}

bool ui_weather_panel_test_search(void)
{
    if (s_ctx == NULL || s_ctx->modal == NULL) {
        return false;
    }
    modal_search_now(s_ctx);
    return true;
}
#endif
