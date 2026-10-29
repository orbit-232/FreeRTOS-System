#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_pages.h"
#include "ui_theme.h"
#include "ui_widgets.h"

typedef struct {
    lv_obj_t *uptime_value;
    lv_obj_t *heap_value;
    lv_obj_t *psram_value;
    lv_obj_t *task_value;
    lv_obj_t *chart;
    lv_chart_series_t *series;
    lv_obj_t *sample_label;
    uint32_t samples;
    int32_t chart_max;
} monitor_ctx_t;

static void format_uptime(uint64_t seconds, char *buf, size_t len)
{
    uint64_t days = seconds / 86400ULL;
    uint64_t hours = (seconds % 86400ULL) / 3600ULL;
    uint64_t mins = (seconds % 3600ULL) / 60ULL;
    if (days > 0) {
        snprintf(buf, len, "%" PRIu64 "天 %02" PRIu64 "小时", days, hours);
    } else {
        snprintf(buf, len, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, hours, mins,
                 seconds % 60ULL);
    }
}

static void format_kb(uint32_t bytes, char *buf, size_t len)
{
    if (bytes >= 1024U * 1024U) {
        snprintf(buf, len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, len, "%u KB", (unsigned)(bytes / 1024U));
    }
}

static void monitor_timer_cb(lv_timer_t *timer)
{
    monitor_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (ctx == NULL) {
        return;
    }
    panel_runtime_t runtime;
    panel_services_get_runtime(&runtime);

    char text[32];
    format_uptime(runtime.uptime_s, text, sizeof(text));
    lv_label_set_text(ctx->uptime_value, text);
    format_kb(runtime.heap_free, text, sizeof(text));
    lv_label_set_text(ctx->heap_value, text);
    format_kb(runtime.psram_free, text, sizeof(text));
    lv_label_set_text(ctx->psram_value, text);
    lv_label_set_text_fmt(ctx->task_value, "%u", (unsigned)runtime.task_count);

    int32_t heap_kb = (int32_t)(runtime.heap_free / 1024U);
    if (heap_kb + 32 > ctx->chart_max) {
        ctx->chart_max = heap_kb + 64;
        lv_chart_set_axis_range(ctx->chart, LV_CHART_AXIS_PRIMARY_Y, 0, ctx->chart_max);
    }
    lv_chart_set_next_value(ctx->chart, ctx->series, heap_kb);
    ctx->samples++;
    lv_label_set_text_fmt(ctx->sample_label, "已采集 %u / 60 秒 · 窗口内为实测值",
                          (unsigned)(ctx->samples > 60 ? 60 : ctx->samples));
}

static void monitor_delete_cb(lv_event_t *event)
{
    lv_timer_t *timer = lv_event_get_user_data(event);
    if (timer != NULL) {
        monitor_ctx_t *ctx = lv_timer_get_user_data(timer);
        lv_timer_delete(timer);
        lv_free(ctx);
    }
}

static lv_obj_t *stat_card(lv_obj_t *parent, int x, const char *key, lv_obj_t **value_out)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *card = ui_card(parent, x, 150, 285, 128);
    ui_label_at(card, 20, 16, key, &ui_font_16, p->muted);
    lv_obj_t *value = ui_label_at(card, 20, 48, "--", &ui_font_24, p->text);
    lv_obj_set_width(value, 245);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_CLIP);
    *value_out = value;
    return card;
}

static lv_obj_t *status_row(lv_obj_t *parent, int y, const char *name, const char *status,
                            bool ok)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(row, 20, y);
    lv_obj_set_size(row, 376, 46);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, p->border, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    ui_label_at(row, 0, 12, name, &ui_font_16, p->text);
    lv_obj_t *pill = ui_pill(row, status, ok ? p->success : p->muted,
                             ok ? p->surface_alt : p->surface_alt, &ui_font_16);
    lv_obj_align(pill, LV_ALIGN_RIGHT_MID, 0, 0);
    return row;
}

lv_obj_t *ui_page_monitor_create(void)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *screen = ui_screen_create();
    ui_status_bar(screen, false);
    ui_header(screen, "系统监视器", "运行时实测");

    monitor_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return screen;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->chart_max = 512;

    stat_card(screen, 40, "运行时间", &ctx->uptime_value);
    stat_card(screen, 345, "可用堆", &ctx->heap_value);
    stat_card(screen, 650, "PSRAM 可用", &ctx->psram_value);
    stat_card(screen, 955, "任务数", &ctx->task_value);

    lv_obj_t *chart_card = ui_card(screen, 40, 296, 760, 386);
    ui_label_at(chart_card, 20, 16, "内部 RAM 可用历史", &ui_font_20, p->text);
    lv_obj_t *badge = ui_pill(chart_card, "实测 · KiB", p->accent, p->accent_soft,
                              &ui_font_16);
    lv_obj_set_pos(badge, 612, 14);
    ctx->chart = lv_chart_create(chart_card);
    lv_obj_set_pos(ctx->chart, 20, 58);
    lv_obj_set_size(ctx->chart, 720, 250);
    lv_chart_set_type(ctx->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ctx->chart, 60);
    ctx->series = lv_chart_add_series(ctx->chart, p->accent, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(ctx->chart, ctx->series, LV_CHART_POINT_NONE);
    lv_chart_set_axis_range(ctx->chart, LV_CHART_AXIS_PRIMARY_Y, 0, ctx->chart_max);
    lv_chart_set_div_line_count(ctx->chart, 5, 6);
    lv_obj_set_style_bg_opa(ctx->chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->chart, 0, LV_PART_MAIN);
    lv_obj_set_style_line_width(ctx->chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_line_color(ctx->chart, p->accent, LV_PART_ITEMS);
    lv_obj_set_style_size(ctx->chart, 0, 0, LV_PART_INDICATOR);

    ui_label_at(chart_card, 20, 316, "60 秒前", &ui_font_16, p->muted);
    lv_obj_t *now = ui_label_at(chart_card, 676, 316, "现在", &ui_font_16, p->muted);
    (void)now;
    ctx->sample_label = ui_label_at(chart_card, 20, 348, "采样中", &ui_font_16, p->muted);

    lv_obj_t *services = ui_card(screen, 820, 296, 420, 386);
    ui_label_at(services, 20, 16, "服务状态", &ui_font_20, p->text);
    status_row(services, 58, "显示 / 背光", "已启动", true);
    status_row(services, 104, "触摸输入", "驱动已注册", true);
    status_row(services, 150, "LVGL 适配器", "运行中", true);
    status_row(services, 196, "设置存储", "NVS 已加载", true);
    status_row(services, 242, "网络服务", "未接入", false);
    status_row(services, 288, "音频服务", "未接入", false);
    ui_label_at(services, 20, 342, "触摸/显示仍需实机验收", &ui_font_16, p->warning);

    lv_obj_t *note = ui_label_at(screen, 40, 700,
                                 "CPU 占用、芯片温度和 FPS 无可靠接口，本页不显示演示数值。",
                                 &ui_font_16, p->muted);
    lv_obj_set_width(note, 1150);
    ui_nav_bar(screen, UI_PAGE_MONITOR);

    lv_timer_t *timer = lv_timer_create(monitor_timer_cb, 1000, ctx);
    monitor_timer_cb(timer);
    lv_obj_add_event_cb(screen, monitor_delete_cb, LV_EVENT_DELETE, timer);
    return screen;
}

