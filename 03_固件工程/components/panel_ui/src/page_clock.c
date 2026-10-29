#include "ui_pages.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"

typedef enum {
    CLOCK_TAB_CLOCK = 0,
    CLOCK_TAB_ALARM,
    CLOCK_TAB_STOPWATCH,
    CLOCK_TAB_COUNTDOWN,
    CLOCK_TAB_COUNT,
} clock_tab_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *status_pill;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_obj_t *alarm_summary;
    lv_obj_t *stopwatch_label;
    lv_obj_t *stopwatch_feedback;
    lv_obj_t *countdown_label;
    lv_obj_t *countdown_feedback;
    lv_obj_t *alarm_feedback;
    lv_obj_t *alarm_switch;
    lv_obj_t *alarm_hour;
    lv_obj_t *alarm_minute;
    lv_obj_t *countdown_hour;
    lv_obj_t *countdown_minute;
    lv_obj_t *countdown_second;
    lv_obj_t *tab_buttons[CLOCK_TAB_COUNT];
    lv_obj_t *tab_panels[CLOCK_TAB_COUNT];
    lv_timer_t *timer;
} clock_ctx_t;

static clock_ctx_t *s_ctx;
static clock_tab_t s_tab = CLOCK_TAB_CLOCK;

static const char *weekday_cn(int wd)
{
    static const char *days[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    return wd >= 0 && wd < 7 ? days[wd] : "";
}

static void clock_set_tab(clock_tab_t tab)
{
    if (tab >= CLOCK_TAB_COUNT) {
        return;
    }
    s_tab = tab;
    if (s_ctx == NULL) {
        return;
    }
    for (int i = 0; i < CLOCK_TAB_COUNT; i++) {
        if (s_ctx->tab_panels[i] == NULL) {
            continue;
        }
        if (i == (int)tab) {
            lv_obj_remove_flag(s_ctx->tab_panels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ctx->tab_panels[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_t *btn = s_ctx->tab_buttons[i];
        if (btn != NULL) {
            lv_obj_set_style_bg_color(btn, i == (int)tab ? ui_palette()->accent : ui_palette()->surface,
                                      0);
            lv_obj_set_style_bg_opa(btn, i == (int)tab ? LV_OPA_COVER : LV_OPA_60, 0);
        }
    }
}

static void clock_tab_cb(lv_event_t *event)
{
    clock_set_tab((clock_tab_t)(intptr_t)lv_event_get_user_data(event));
}

static void clock_feedback(lv_obj_t *label, const char *text)
{
    if (label != NULL) {
        lv_label_set_text(label, text);
    }
}

static void clock_command_result(lv_obj_t *label, esp_err_t err, const char *ok)
{
    if (label == NULL) {
        return;
    }
    if (err == ESP_OK) {
        lv_label_set_text(label, ok);
    } else if (err == ESP_ERR_NOT_FINISHED) {
        lv_label_set_text(label, "稍后再试");
    } else if (err == ESP_ERR_TIMEOUT) {
        lv_label_set_text(label, "命令队列忙");
    } else if (err == ESP_ERR_INVALID_STATE) {
        lv_label_set_text(label, "服务未就绪");
    } else {
        lv_label_set_text(label, "设置失败");
    }
}

static void stopwatch_start_cb(lv_event_t *event)
{
    (void)event;
    clock_command_result(s_ctx ? s_ctx->stopwatch_feedback : NULL,
                         panel_services_clock_stopwatch_start(), "秒表已开始");
}

static void stopwatch_pause_cb(lv_event_t *event)
{
    (void)event;
    clock_command_result(s_ctx ? s_ctx->stopwatch_feedback : NULL,
                         panel_services_clock_stopwatch_pause(), "秒表已暂停");
}

static void stopwatch_reset_cb(lv_event_t *event)
{
    (void)event;
    clock_command_result(s_ctx ? s_ctx->stopwatch_feedback : NULL,
                         panel_services_clock_stopwatch_reset(), "秒表已复位");
}

static uint32_t roller_value(lv_obj_t *roller)
{
    return roller != NULL ? (uint32_t)lv_roller_get_selected(roller) : 0;
}

static void countdown_start_cb(lv_event_t *event)
{
    (void)event;
    uint32_t seconds = roller_value(s_ctx->countdown_hour) * 3600U +
                       roller_value(s_ctx->countdown_minute) * 60U +
                       roller_value(s_ctx->countdown_second);
    if (seconds == 0) {
        clock_feedback(s_ctx->countdown_feedback, "请设置时间");
        return;
    }
    clock_command_result(s_ctx->countdown_feedback,
                         panel_services_clock_countdown_start(seconds), "倒计时已开始");
}

static void countdown_pause_cb(lv_event_t *event)
{
    (void)event;
    clock_command_result(s_ctx->countdown_feedback,
                         panel_services_clock_countdown_pause(), "倒计时已暂停");
}

static void countdown_resume_cb(lv_event_t *event)
{
    (void)event;
    clock_command_result(s_ctx->countdown_feedback,
                         panel_services_clock_countdown_resume(), "倒计时已继续");
}

static void countdown_reset_cb(lv_event_t *event)
{
    (void)event;
    clock_command_result(s_ctx->countdown_feedback,
                         panel_services_clock_countdown_reset(), "倒计时已复位");
}

static void alarm_save_cb(lv_event_t *event)
{
    (void)event;
    bool enabled = s_ctx->alarm_switch != NULL &&
                   lv_obj_has_state(s_ctx->alarm_switch, LV_STATE_CHECKED);
    esp_err_t err = panel_services_clock_alarm_set(
        enabled, (uint8_t)roller_value(s_ctx->alarm_hour),
        (uint8_t)roller_value(s_ctx->alarm_minute));
    clock_command_result(s_ctx->alarm_feedback, err, "设置已提交");
}

static void create_roller(lv_obj_t *parent, int x, int y, int max, lv_obj_t **out)
{
    lv_obj_t *roller = lv_roller_create(parent);
    lv_obj_set_pos(roller, x, y);
    lv_obj_set_size(roller, 130, 150);
    lv_obj_set_style_text_font(roller, &ui_font_32, 0);
    char options[256];
    size_t used = 0;
    for (int i = 0; i <= max; i++) {
        int n = snprintf(options + used, sizeof(options) - used, "%02d%s", i,
                         i == max ? "" : "\n");
        if (n < 0 || (size_t)n >= sizeof(options) - used) {
            break;
        }
        used += (size_t)n;
    }
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    *out = roller;
}

static lv_obj_t *make_tab_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_size(panel, 1200, 528);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    return panel;
}

static void create_clock_panel(clock_ctx_t *ctx, lv_obj_t *content)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *panel = make_tab_panel(content);
    ctx->tab_panels[CLOCK_TAB_CLOCK] = panel;
    lv_obj_t *card = ui_card(panel, 40, 18, 1120, 500);
    ctx->time_label = ui_label_at(card, 50, 70, "--:--:--", &ui_font_num_80, p->text);
    ctx->date_label = ui_label_at(card, 54, 196, "等待校时", &ui_font_24, p->muted);
    lv_obj_t *hint = ui_label_at(card, 54, 250, "北京本地时间 · 校时后自动更新",
                                 &ui_font_16, p->muted);
    (void)hint;
    ctx->alarm_summary = ui_label_at(card, 54, 330, "闹钟未启用", &ui_font_20, p->text);
}

static void create_stopwatch_panel(clock_ctx_t *ctx, lv_obj_t *content)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *panel = make_tab_panel(content);
    ctx->tab_panels[CLOCK_TAB_STOPWATCH] = panel;
    lv_obj_t *card = ui_card(panel, 40, 18, 1120, 500);
    ctx->stopwatch_label = ui_label_at(card, 54, 80, "00:00.0", &ui_font_num_80, p->text);
    ui_button(card, 54, 300, 230, 58, "开始", &ui_font_20, stopwatch_start_cb, ctx);
    ui_button(card, 304, 300, 230, 58, "暂停", &ui_font_20, stopwatch_pause_cb, ctx);
    ui_button(card, 554, 300, 230, 58, "复位", &ui_font_20, stopwatch_reset_cb, ctx);
    ctx->stopwatch_feedback = ui_label_at(card, 54, 390, "", &ui_font_16, p->muted);
}

static void create_countdown_panel(clock_ctx_t *ctx, lv_obj_t *content)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *panel = make_tab_panel(content);
    ctx->tab_panels[CLOCK_TAB_COUNTDOWN] = panel;
    lv_obj_t *card = ui_card(panel, 40, 18, 1120, 500);
    ctx->countdown_label = ui_label_at(card, 54, 40, "00:00:00", &ui_font_num_48, p->text);
    create_roller(card, 54, 150, 23, &ctx->countdown_hour);
    create_roller(card, 204, 150, 59, &ctx->countdown_minute);
    create_roller(card, 354, 150, 59, &ctx->countdown_second);
    ui_label_at(card, 54, 310, "时", &ui_font_16, p->muted);
    ui_label_at(card, 204, 310, "分", &ui_font_16, p->muted);
    ui_label_at(card, 354, 310, "秒", &ui_font_16, p->muted);
    ui_button(card, 54, 360, 180, 54, "开始", &ui_font_20, countdown_start_cb, ctx);
    ui_button(card, 250, 360, 180, 54, "暂停", &ui_font_20, countdown_pause_cb, ctx);
    ui_button(card, 446, 360, 180, 54, "继续", &ui_font_20, countdown_resume_cb, ctx);
    ui_button(card, 642, 360, 180, 54, "复位", &ui_font_20, countdown_reset_cb, ctx);
    ctx->countdown_feedback = ui_label_at(card, 54, 440, "", &ui_font_16, p->muted);
}

static void create_alarm_panel(clock_ctx_t *ctx, lv_obj_t *content)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *panel = make_tab_panel(content);
    ctx->tab_panels[CLOCK_TAB_ALARM] = panel;
    lv_obj_t *card = ui_card(panel, 40, 18, 1120, 500);
    ui_label_at(card, 54, 40, "每日闹钟", &ui_font_32, p->text);
    create_roller(card, 54, 120, 23, &ctx->alarm_hour);
    create_roller(card, 204, 120, 59, &ctx->alarm_minute);
    ui_label_at(card, 54, 280, "时", &ui_font_16, p->muted);
    ui_label_at(card, 204, 280, "分", &ui_font_16, p->muted);
    ctx->alarm_switch = lv_switch_create(card);
    lv_obj_set_pos(ctx->alarm_switch, 380, 150);
    lv_obj_set_size(ctx->alarm_switch, 90, 44);
    ui_button(card, 54, 340, 220, 58, "保存", &ui_font_20, alarm_save_cb, ctx);
    ui_label_at(card, 54, 420, "仅在校时后触发 · 屏幕提醒", &ui_font_16, p->muted);
    ctx->alarm_feedback = ui_label_at(card, 300, 350, "", &ui_font_16, p->muted);
}

static void clock_timer_cb(lv_timer_t *timer)
{
    clock_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (ctx == NULL) {
        return;
    }
    panel_clock_snapshot_t snap;
    panel_services_get_clock(&snap);
    if (snap.valid) {
        lv_label_set_text_fmt(ctx->time_label, "%02d:%02d:%02d", snap.hour, snap.minute,
                              snap.second);
        lv_label_set_text_fmt(ctx->date_label, "%d年%d月%d日 %s", snap.year, snap.month,
                              snap.day, weekday_cn(snap.weekday));
    } else {
        lv_label_set_text(ctx->time_label, "--:--:--");
        lv_label_set_text(ctx->date_label, "等待校时");
    }
    lv_label_set_text(ctx->status_pill, snap.calibrated ? "已校时" : "未校时");
    if (ctx->alarm_summary != NULL) {
        if (!snap.alarm_enabled) {
            lv_label_set_text(ctx->alarm_summary, "闹钟未启用");
        } else {
            lv_label_set_text_fmt(ctx->alarm_summary, "每日 %02u:%02u · %s",
                                  snap.alarm_hour, snap.alarm_minute,
                                  snap.calibrated ? "已校时" : "等待校时");
        }
    }
    if (ctx->alarm_feedback != NULL && snap.alarm_persist_failed) {
        lv_label_set_text(ctx->alarm_feedback, "保存失败");
    }
    uint32_t ms = snap.stopwatch_ms;
    lv_label_set_text_fmt(ctx->stopwatch_label, "%02u:%02u.%u",
                          (unsigned)(ms / 60000U), (unsigned)((ms / 1000U) % 60U),
                          (unsigned)((ms / 100U) % 10U));
    uint32_t rem = snap.countdown_remaining_ms / 1000U;
    lv_label_set_text_fmt(ctx->countdown_label, "%02u:%02u:%02u",
                          (unsigned)(rem / 3600U), (unsigned)((rem / 60U) % 60U),
                          (unsigned)(rem % 60U));
}

static void clock_delete_cb(lv_event_t *event)
{
    clock_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }
    if (ctx->timer != NULL) {
        lv_timer_delete(ctx->timer);
    }
    if (s_ctx == ctx) {
        s_ctx = NULL;
    }
    lv_free(ctx);
}

void ui_page_clock_ui_state_reset(void)
{
    s_tab = CLOCK_TAB_CLOCK;
}

void ui_page_clock_state_summary(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    panel_clock_snapshot_t snap;
    panel_services_get_clock(&snap);
    if (snap.valid) {
        snprintf(buf, len, "%02d:%02d · %s", snap.hour, snap.minute,
                 snap.alarm_enabled ? "闹钟开" : "闹钟关");
    } else {
        snprintf(buf, len, "时钟等待校时");
    }
}

lv_obj_t *ui_page_clock_create(void)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *screen = ui_screen_create();
    if (ui_theme_effective() == PANEL_THEME_DARK) {
        ui_screen_gradient(screen, p->accent_soft, p->bg);
    } else {
        ui_screen_gradient(screen, lv_color_hex(0xEAF1FA), lv_color_hex(0xF9F8F7));
    }
    ui_status_bar(screen, false);
    ui_header(screen, "时钟", NULL);

    clock_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return screen;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->screen = screen;
    s_ctx = ctx;

    static const char *tabs[] = {"时钟", "闹钟", "秒表", "倒计时"};
    for (int i = 0; i < CLOCK_TAB_COUNT; i++) {
        ctx->tab_buttons[i] = ui_button(screen, 250 + i * 190, 138, 180, 44, tabs[i],
                                         &ui_font_16, clock_tab_cb, (void *)(intptr_t)i);
    }
    ctx->status_pill = ui_pill(screen, "未校时", p->accent, p->accent_soft, &ui_font_16);
    lv_obj_set_pos(ctx->status_pill, 1040, 146);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(content, 40, 192);
    lv_obj_set_size(content, 1200, 528);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    create_clock_panel(ctx, content);
    create_alarm_panel(ctx, content);
    create_stopwatch_panel(ctx, content);
    create_countdown_panel(ctx, content);

    panel_clock_snapshot_t snap;
    panel_services_get_clock(&snap);
    lv_roller_set_selected(ctx->alarm_hour, snap.alarm_enabled ? snap.alarm_hour : 7,
                           LV_ANIM_OFF);
    lv_roller_set_selected(ctx->alarm_minute, snap.alarm_minute, LV_ANIM_OFF);
    if (snap.alarm_enabled) {
        lv_obj_add_state(ctx->alarm_switch, LV_STATE_CHECKED);
    }
    lv_roller_set_selected(ctx->countdown_minute, 5, LV_ANIM_OFF);

    ui_nav_bar(screen, UI_PAGE_CLOCK);
    ctx->timer = lv_timer_create(clock_timer_cb, 100, ctx);
    lv_obj_add_event_cb(screen, clock_delete_cb, LV_EVENT_DELETE, ctx);
    clock_set_tab(s_tab);
    clock_timer_cb(ctx->timer);
    return screen;
}
