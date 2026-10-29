#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "panel_services.h"
#include "panel_wallpaper.h"
#include "ui.h"
#include "ui_pages.h"
#include "ui_theme.h"
#include "ui_widgets.h"

typedef struct {
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_obj_t *screen;
    lv_obj_t *wallpaper_img;
    lv_obj_t *wallpaper_scrim;
    lv_timer_t *clock_timer;
    lv_timer_t *wallpaper_timer;
    lv_obj_t *press_card;
    int press_x;
    int press_y;
    int press_base_x;
} lock_ctx_t;

static lv_image_dsc_t s_wallpaper_dsc;

static const char *weekday_cn(int wday)
{
    static const char *days[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    return (wday >= 0 && wday <= 6) ? days[wday] : "";
}

static bool apply_wallpaper(lock_ctx_t *ctx)
{
    if (ctx == NULL || ctx->screen == NULL || ctx->wallpaper_img != NULL) return false;
    const panel_wallpaper_t *wallpaper = panel_wallpaper_get();
    if (wallpaper == NULL || wallpaper->pixels == NULL) return false;

    memset(&s_wallpaper_dsc, 0, sizeof(s_wallpaper_dsc));
    s_wallpaper_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_wallpaper_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_wallpaper_dsc.header.w = wallpaper->width;
    s_wallpaper_dsc.header.h = wallpaper->height;
    s_wallpaper_dsc.header.stride = (uint32_t)wallpaper->width * 2U;
    s_wallpaper_dsc.data_size = (uint32_t)wallpaper->width * wallpaper->height * 2U;
    s_wallpaper_dsc.data = wallpaper->pixels;

    lv_obj_t *image = lv_image_create(ctx->screen);
    if (image == NULL) return false;
    lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(image, 0, 0);
    lv_obj_set_size(image, wallpaper->width, wallpaper->height);
    lv_image_set_src(image, &s_wallpaper_dsc);
    lv_obj_move_to_index(image, 0);

    lv_obj_t *scrim = lv_obj_create(ctx->screen);
    if (scrim == NULL) {
        lv_obj_delete(image);
        return false;
    }
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_size(scrim, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_move_to_index(scrim, 1);

    ctx->wallpaper_img = image;
    ctx->wallpaper_scrim = scrim;
    return true;
}

static void wallpaper_timer_cb(lv_timer_t *timer)
{
    lock_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (ctx == NULL) {
        lv_timer_delete(timer);
        return;
    }
    if (apply_wallpaper(ctx) || ctx->wallpaper_img != NULL) {
        lv_timer_delete(timer);
        ctx->wallpaper_timer = NULL;
    }
}

static void lock_timer_cb(lv_timer_t *timer)
{
    lock_ctx_t *ctx = lv_timer_get_user_data(timer);
    struct tm now;
    if (ctx == NULL || !panel_services_get_local_time(&now)) {
        return;
    }
    lv_label_set_text_fmt(ctx->time_label, "%02d:%02d", now.tm_hour, now.tm_min);
    lv_label_set_text_fmt(ctx->date_label, "%d月%d日 %s · %s", now.tm_mon + 1,
                          now.tm_mday, weekday_cn(now.tm_wday),
                          panel_services_time_is_calibrated() ? "已校时" : "未校时");
}

static void lock_delete_cb(lv_event_t *event)
{
    lock_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }
    if (ctx->clock_timer != NULL) {
        lv_timer_delete(ctx->clock_timer);
    }
    if (ctx->wallpaper_timer != NULL) {
        lv_timer_delete(ctx->wallpaper_timer);
    }
    /* The screen's LV_EVENT_DELETE is sent before its children are destroyed;
     * destroy the image explicitly so the PSRAM is never freed while an image
     * object still references it. */
    if (ctx->wallpaper_img != NULL) {
        lv_obj_delete(ctx->wallpaper_img);
        ctx->wallpaper_img = NULL;
    }
    if (ctx->wallpaper_scrim != NULL) {
        lv_obj_delete(ctx->wallpaper_scrim);
        ctx->wallpaper_scrim = NULL;
    }
    panel_wallpaper_release();
    lv_free(ctx);
}

static void unlock_cb(lv_event_t *event)
{
    (void)event;
    ui_navigate(UI_PAGE_HOME);
}

static void card_reset(lock_ctx_t *ctx)
{
    if (ctx == NULL || ctx->press_card == NULL) return;
    lv_obj_set_x(ctx->press_card, ctx->press_base_x);
    ctx->press_card = NULL;
}

static void card_pressed_cb(lv_event_t *event)
{
    lock_ctx_t *ctx = lv_event_get_user_data(event);
    lv_obj_t *card = lv_event_get_current_target_obj(event);
    lv_indev_t *indev = lv_indev_active();
    if (ctx == NULL || card == NULL || indev == NULL) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    ctx->press_card = card;
    ctx->press_x = point.x;
    ctx->press_y = point.y;
    ctx->press_base_x = lv_obj_get_x(card);
}

static void card_pressing_cb(lv_event_t *event)
{
    lock_ctx_t *ctx = lv_event_get_user_data(event);
    lv_obj_t *card = lv_event_get_current_target_obj(event);
    lv_indev_t *indev = lv_indev_active();
    if (ctx == NULL || card == NULL || ctx->press_card != card || indev == NULL) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    int dx = point.x - ctx->press_x;
    int dy = point.y - ctx->press_y;
    if (abs(dx) > 10 && abs(dx) > abs(dy)) {
        lv_obj_set_x(card, ctx->press_base_x + dx);
    }
}

static void card_released_cb(lv_event_t *event)
{
    lock_ctx_t *ctx = lv_event_get_user_data(event);
    lv_obj_t *card = lv_event_get_current_target_obj(event);
    lv_indev_t *indev = lv_indev_active();
    if (ctx == NULL || card == NULL || ctx->press_card != card || indev == NULL) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    int dx = point.x - ctx->press_x;
    int dy = point.y - ctx->press_y;
    ctx->press_card = NULL;
    if (abs(dx) >= 72 && abs(dx) > abs(dy)) {
        lv_obj_delete(card);
    } else {
        lv_obj_set_x(card, ctx->press_base_x);
    }
}

static void card_press_lost_cb(lv_event_t *event)
{
    card_reset(lv_event_get_user_data(event));
}

static lv_obj_t *notification_card(lv_obj_t *parent, int x, int y, const char *symbol,
                                   const char *title, const char *subtitle, lock_ctx_t *ctx)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, 468, 108);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_10, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    lv_obj_t *icon_box = lv_obj_create(card);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(icon_box, 18, 22);
    lv_obj_set_size(icon_box, 60, 60);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x5E9FE8), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_30, 0);
    lv_obj_set_style_radius(icon_box, 16, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_t *icon = ui_label(icon_box, symbol, &lv_font_montserrat_24,
                              lv_color_hex(0x8DBBF0));
    lv_obj_center(icon);

    ui_label_at(card, 96, 22, title, &ui_font_24, lv_color_hex(0xF5F8FC));
    ui_label_at(card, 96, 60, subtitle, &ui_font_16, lv_color_hex(0xAEBBC8));
    if (ctx != NULL) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, card_pressed_cb, LV_EVENT_PRESSED, ctx);
        lv_obj_add_event_cb(card, card_pressing_cb, LV_EVENT_PRESSING, ctx);
        lv_obj_add_event_cb(card, card_released_cb, LV_EVENT_RELEASED, ctx);
        lv_obj_add_event_cb(card, card_press_lost_cb, LV_EVENT_PRESS_LOST, ctx);
    }
    return card;
}

lv_obj_t *ui_page_lock_create(void)
{
    lv_obj_t *screen = ui_screen_create();
    ui_screen_gradient(screen, lv_color_hex(0x1E2A3A), lv_color_hex(0x0F161F));

    lock_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx != NULL) {
        memset(ctx, 0, sizeof(*ctx));
        ctx->screen = screen;
        panel_wallpaper_request();
        apply_wallpaper(ctx);
    }

    ui_status_bar(screen, true);

    lv_obj_t *time_label = ui_label_at(screen, 86, 104, "--:--", &ui_font_num_128,
                                       lv_color_hex(0xF7FAFC));
    lv_obj_set_style_text_letter_space(time_label, -3, 0);
    lv_obj_t *date_label = ui_label_at(screen, 92, 260, "", &ui_font_24,
                                       lv_color_hex(0xB9C5D2));

    panel_settings_t settings;
    panel_services_get_settings(&settings);
    if (settings.do_not_disturb) {
        lv_obj_t *dnd = ui_pill(screen, "勿扰已开启", lv_color_hex(0xD6E4F2),
                                lv_color_hex(0x263445), &ui_font_16);
        lv_obj_set_pos(dnd, 92, 308);
    }

    ui_label_at(screen, 760, 104, "示例通知", &ui_font_16, lv_color_hex(0x8D9AAA));
    notification_card(screen, 760, 140, LV_SYMBOL_BELL, "固件更新可用",
                      "v1.3.0 · 修复触摸校准问题", ctx);
    notification_card(screen, 760, 264, LV_SYMBOL_DOWNLOAD, "定时任务完成",
                      "数据已同步至 SD 卡 · 2 分钟前", ctx);

    lv_obj_t *hint = lv_button_create(screen);
    lv_obj_set_size(hint, 360, 78);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -58);
    lv_obj_set_style_bg_opa(hint, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hint, 0, 0);
    lv_obj_set_style_shadow_width(hint, 0, 0);
    lv_obj_t *hint_label = ui_label(hint, "向上滑动解锁", &ui_font_20,
                                    lv_color_hex(0xD4DEE9));
    lv_obj_align(hint_label, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *click_label = ui_label(hint, "也可点击解锁", &ui_font_16,
                                     lv_color_hex(0x8D9AAA));
    lv_obj_align(click_label, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(hint, unlock_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, 100, 4);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xD4DEE9), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -22);

    if (ctx != NULL) {
        ctx->time_label = time_label;
        ctx->date_label = date_label;
        ctx->clock_timer = lv_timer_create(lock_timer_cb, 1000, ctx);
        if (ctx->clock_timer != NULL) {
            lock_timer_cb(ctx->clock_timer);
        }
        if (ctx->wallpaper_img == NULL) {
            ctx->wallpaper_timer = lv_timer_create(wallpaper_timer_cb, 500, ctx);
        }
        lv_obj_add_event_cb(screen, lock_delete_cb, LV_EVENT_DELETE, ctx);
    }

    return screen;
}
