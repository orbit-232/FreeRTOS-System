#include "ui_widgets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "panel_services.h"
#include "ui.h"
#include "ui_theme.h"

static lv_obj_t *s_page_parent;
static bool s_page_nav_suppressed;
static lv_obj_t *s_system_bar;

void ui_page_parent_set(lv_obj_t *parent)
{
    s_page_parent = parent;
}

void ui_page_nav_suppress(bool suppress)
{
    s_page_nav_suppressed = suppress;
}

void ui_pressable(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }
    const ui_palette_t *p = ui_palette();
    lv_obj_set_style_bg_color(obj, p->surface_alt, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, p->border, LV_STATE_PRESSED);
    lv_obj_set_style_opa(obj, LV_OPA_90, LV_STATE_PRESSED);
}

static void header_back_cb(lv_event_t *event)
{
    (void)event;
    ui_back();
}

typedef struct {
    lv_obj_t *time_label;
    lv_obj_t *cal_pill;
    lv_obj_t *wifi_icon;
    lv_obj_t *wifi_text;
    lv_timer_t *timer;
    lv_color_t muted;
    lv_color_t connected;
} status_ctx_t;

/*
 * Runs in the LVGL timer context. This only reads the panel_services
 * snapshot and updates labels; it never performs network I/O. Time and
 * network are independent so a failed clock read cannot freeze connectivity.
 */
static void status_ctx_update(status_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    struct tm now;
    if (ctx->time_label != NULL && panel_services_get_local_time(&now)) {
        lv_label_set_text_fmt(ctx->time_label, "%02d:%02d", now.tm_hour, now.tm_min);
    }

    bool connected = panel_services_wifi_connected();
    lv_color_t color = connected ? ctx->connected : ctx->muted;
    if (ctx->wifi_icon != NULL) {
        lv_obj_set_style_text_color(ctx->wifi_icon, color, 0);
    }
    if (ctx->wifi_text != NULL) {
        lv_obj_set_style_text_color(ctx->wifi_text, color, 0);
        lv_label_set_text(ctx->wifi_text, connected ? "已连接" : "未联网");
    }

    if (ctx->cal_pill != NULL) {
        const ui_palette_t *p = ui_palette();
        bool calibrated = panel_services_time_is_calibrated();
        lv_label_set_text(ctx->cal_pill, calibrated ? "已校时" : "未校时");
        lv_obj_set_style_text_color(ctx->cal_pill,
                                    calibrated ? ctx->connected : p->accent, 0);
        lv_obj_set_style_bg_color(ctx->cal_pill,
                                  calibrated ? p->surface_alt : p->accent_soft, 0);
    }
}

static void status_timer_cb(lv_timer_t *timer)
{
    status_ctx_update((status_ctx_t *)lv_timer_get_user_data(timer));
}

static void status_bar_delete_cb(lv_event_t *event)
{
    status_ctx_t *ctx = (status_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }
    if (ctx->timer != NULL) {
        lv_timer_delete(ctx->timer);
        ctx->timer = NULL;
    }
    lv_free(ctx);
}

lv_obj_t *ui_screen_create(void)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *screen = lv_obj_create(s_page_parent);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(screen, 0, 0);
    lv_obj_set_size(screen, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_style_bg_color(screen, p->bg, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, p->text, 0);
    lv_obj_set_style_text_font(screen, &ui_font_20, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    return screen;
}

void ui_screen_gradient(lv_obj_t *screen, lv_color_t top, lv_color_t bottom)
{
    lv_obj_set_style_bg_color(screen, top, 0);
    lv_obj_set_style_bg_grad_color(screen, bottom, 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
}

lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, p->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_border_color(card, p->border, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_shadow_color(card, p->shadow, 0);
    lv_obj_set_style_shadow_width(card, 12, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_10, 0);
    lv_obj_set_style_shadow_offset_y(card, 3, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    return card;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text != NULL ? text : "");
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

lv_obj_t *ui_label_at(lv_obj_t *parent, int x, int y, const char *text, const lv_font_t *font,
                      lv_color_t color)
{
    lv_obj_t *label = ui_label(parent, text, font, color);
    lv_obj_set_pos(label, x, y);
    return label;
}

void ui_label_set_text(lv_obj_t *label, const char *text)
{
    if (label != NULL) {
        lv_label_set_text(label, text != NULL ? text : "");
    }
}

lv_obj_t *ui_pill(lv_obj_t *parent, const char *text, lv_color_t fg, lv_color_t bg,
                  const lv_font_t *font)
{
    lv_obj_t *label = ui_label(parent, text, font != NULL ? font : &ui_font_16, fg);
    lv_obj_set_style_bg_color(label, bg, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(label, 12, 0);
    lv_obj_set_style_pad_hor(label, 12, 0);
    lv_obj_set_style_pad_ver(label, 5, 0);
    return label;
}

lv_obj_t *ui_bar_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(track, x, y);
    lv_obj_set_size(track, w, h);
    lv_obj_set_style_bg_color(track, p->surface_alt, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, h / 2, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);

    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_size(fill, 0, h);
    lv_obj_set_style_bg_color(fill, p->accent, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, h / 2, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_pad_all(fill, 0, 0);
    return track;
}

void ui_bar_set(lv_obj_t *bar, int percent)
{
    if (bar == NULL) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    lv_obj_t *fill = lv_obj_get_child(bar, 0);
    if (fill != NULL) {
        lv_obj_set_width(fill, lv_pct(percent));
    }
}

lv_obj_t *ui_status_bar(lv_obj_t *parent, bool dark)
{
    const ui_palette_t *p = ui_palette();
    lv_color_t fg = dark ? lv_color_hex(0xF2F5F8) : p->text;
    lv_color_t muted = dark ? lv_color_hex(0xB9C4CF) : p->muted;
    lv_color_t pill_bg = dark ? lv_color_hex(0x263445) : p->accent_soft;
    lv_color_t pill_fg = dark ? lv_color_hex(0xD6E4F2) : p->accent;
    lv_color_t connected_color = p->success;

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, UI_WIDTH, UI_STATUS_HEIGHT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    lv_obj_t *time_label = ui_label_at(bar, 32, 15, "--:--", &ui_font_16, fg);
    lv_obj_t *uncal = ui_pill(bar, "未校时", pill_fg, pill_bg, &ui_font_16);
    lv_obj_align_to(uncal, time_label, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    lv_obj_t *wifi = ui_label(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_20, muted);
    lv_obj_align(wifi, LV_ALIGN_TOP_RIGHT, -268, 14);
    lv_obj_t *wifi_text = ui_label(bar, "未联网", &ui_font_16, muted);
    lv_obj_align_to(wifi_text, wifi, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    lv_obj_t *usb = ui_label(bar, LV_SYMBOL_USB, &lv_font_montserrat_20, muted);
    lv_obj_align(usb, LV_ALIGN_TOP_RIGHT, -118, 14);
    lv_obj_t *usb_text = ui_label(bar, "USB 供电", &ui_font_16, muted);
    lv_obj_align_to(usb_text, usb, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    status_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx != NULL) {
        memset(ctx, 0, sizeof(*ctx));
        ctx->time_label = time_label;
        ctx->cal_pill = uncal;
        ctx->wifi_icon = wifi;
        ctx->wifi_text = wifi_text;
        ctx->muted = muted;
        ctx->connected = connected_color;
        ctx->timer = lv_timer_create(status_timer_cb, 1000, ctx);
        if (ctx->timer != NULL) {
            status_ctx_update(ctx);
            lv_obj_add_event_cb(bar, status_bar_delete_cb, LV_EVENT_DELETE, ctx);
        } else {
            lv_free(ctx);
        }
    }
    return bar;
}

lv_obj_t *ui_header(lv_obj_t *parent, const char *title, const char *right_text)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(header, 0, UI_STATUS_HEIGHT);
    lv_obj_set_size(header, UI_WIDTH, UI_HEADER_HEIGHT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_set_size(back, 48, 48);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_radius(back, 24, 0);
    lv_obj_set_style_bg_color(back, p->surface, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(back, p->border, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, header_back_cb, LV_EVENT_CLICKED, NULL);
    ui_pressable(back);
    lv_obj_t *back_icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_24, p->text);
    lv_obj_center(back_icon);

    lv_obj_t *title_label = ui_label_at(header, 84, 20, title, &ui_font_32, p->text);
    (void)title_label;
    if (right_text != NULL) {
        lv_obj_t *right = ui_pill(header, right_text, p->muted, p->surface_alt, &ui_font_16);
        lv_obj_align(right, LV_ALIGN_RIGHT_MID, -32, 0);
    }
    return header;
}

static void nav_back_cb(lv_event_t *event)
{
    (void)event;
    ui_back();
}

static void nav_home_cb(lv_event_t *event)
{
    (void)event;
    ui_home();
}

static void nav_overview_cb(lv_event_t *event)
{
    (void)event;
    ui_open_overview();
}

static lv_obj_t *system_key_button(lv_obj_t *bar, int center_x, lv_event_cb_t cb)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *button = lv_button_create(bar);
    lv_obj_set_pos(button, center_x - 64, 8);
    lv_obj_set_size(button, 128, 48);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
    ui_pressable(button);
    (void)p;
    return button;
}

static void system_icon_back(lv_obj_t *button)
{
    static const lv_point_precise_t points[] = {
        {18, 4}, {18, 20}, {4, 12}, {18, 4},
    };
    const ui_palette_t *p = ui_palette();
    lv_obj_t *line = lv_line_create(button);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(line, 52, 12);
    lv_obj_set_size(line, 24, 24);
    lv_line_set_points(line, points, 4);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_color(line, p->text, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
}

static void system_icon_home(lv_obj_t *button)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *ring = lv_obj_create(button);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ring, 22, 22);
    lv_obj_set_style_radius(ring, 11, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, p->text, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_center(ring);
}

static void system_icon_overview(lv_obj_t *button)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *square = lv_obj_create(button);
    lv_obj_remove_flag(square, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(square, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(square, 20, 20);
    lv_obj_set_style_radius(square, 3, 0);
    lv_obj_set_style_bg_opa(square, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(square, 2, 0);
    lv_obj_set_style_border_color(square, p->text, 0);
    lv_obj_set_style_pad_all(square, 0, 0);
    lv_obj_center(square);
}

static lv_obj_t *build_system_bar(lv_obj_t *parent)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(bar, 0, UI_NAV_Y);
    lv_obj_set_size(bar, UI_WIDTH, UI_NAV_HEIGHT);
    lv_obj_set_style_bg_color(bar, p->surface, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, p->border, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    lv_obj_set_user_data(bar, (void *)(uintptr_t)UI_NAV_BAR_MAGIC);

    lv_obj_t *back = system_key_button(bar, 480, nav_back_cb);
    lv_obj_t *home = system_key_button(bar, 640, nav_home_cb);
    lv_obj_t *overview = system_key_button(bar, 800, nav_overview_cb);
    system_icon_back(back);
    system_icon_home(home);
    system_icon_overview(overview);
    return bar;
}

lv_obj_t *ui_system_bar_create(void)
{
    if (s_system_bar != NULL) {
        return s_system_bar;
    }
    s_system_bar = build_system_bar(lv_layer_sys());
    lv_obj_add_flag(s_system_bar, LV_OBJ_FLAG_HIDDEN);
    return s_system_bar;
}

void ui_system_bar_set_visible(bool visible)
{
    if (s_system_bar == NULL) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(s_system_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_system_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_system_bar_rebuild(void)
{
    bool visible = ui_system_bar_is_visible();
    if (s_system_bar != NULL) {
        lv_obj_delete(s_system_bar);
        s_system_bar = NULL;
    }
    ui_system_bar_create();
    ui_system_bar_set_visible(visible);
}

bool ui_system_bar_is_visible(void)
{
    return s_system_bar != NULL && !lv_obj_has_flag(s_system_bar, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *ui_nav_bar(lv_obj_t *parent, ui_page_t page)
{
    (void)page;
    if (parent == NULL || s_page_nav_suppressed) {
        return NULL;
    }
    return build_system_bar(parent);
}

lv_obj_t *ui_grabber(lv_obj_t *parent)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g, 80, 6);
    lv_obj_set_style_radius(g, 3, 0);
    lv_obj_set_style_bg_color(g, p->border, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_align(g, LV_ALIGN_BOTTOM_MID, 0, -16);
    return g;
}

lv_obj_t *ui_slider_create(lv_obj_t *parent, int x, int y, int w, int min, int max, int value)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_pos(slider, x, y);
    lv_obj_set_size(slider, w, 32);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, p->surface_alt, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, p->accent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 16, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, p->surface, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, p->border, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 1, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_width(slider, 28, LV_PART_KNOB);
    lv_obj_set_style_height(slider, 28, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(slider, LV_OPA_20, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(slider, p->shadow, LV_PART_KNOB);
    return slider;
}

lv_obj_t *ui_switch_create(lv_obj_t *parent, int x, int y, bool checked)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_size(sw, 56, 32);
    lv_obj_set_style_bg_color(sw, p->accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, p->surface_alt, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, p->surface, LV_PART_KNOB);
    lv_obj_set_style_radius(sw, 16, LV_PART_MAIN);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return sw;
}

static lv_obj_t *add_round_rect(lv_obj_t *parent, int x, int y, int w, int h, int radius,
                                lv_color_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static void line_delete_cb(lv_event_t *event)
{
    lv_free(lv_event_get_user_data(event));
}

static lv_obj_t *add_line(lv_obj_t *parent, int x1, int y1, int x2, int y2, int width,
                          lv_color_t color)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_point_precise_t *points = lv_malloc(sizeof(lv_point_precise_t) * 2);
    if (points == NULL) {
        return line;
    }
    points[0].x = x1;
    points[0].y = y1;
    points[1].x = x2;
    points[1].y = y2;
    lv_line_set_points(line, points, 2);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_size(line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(line, line_delete_cb, LV_EVENT_DELETE, points);
    return line;
}

static void draw_cloud(lv_obj_t *parent, int size, int offset_x, int offset_y, lv_color_t color)
{
    int base_h = size / 3;
    int base_y = offset_y + (size * 2) / 3;
    add_round_rect(parent, offset_x + size / 6, base_y, size - size / 4, base_h, base_h / 2,
                   color);
    int c1 = size / 3;
    lv_obj_t *circle1 = add_round_rect(parent, offset_x + size / 8, base_y - c1 / 2, c1, c1,
                                       c1 / 2, color);
    (void)circle1;
    int c2 = size / 2;
    add_round_rect(parent, offset_x + size / 3, base_y - c2 / 2 - size / 14, c2, c2, c2 / 2,
                   color);
    int c3 = size / 4;
    add_round_rect(parent, offset_x + (size * 2) / 3, base_y - c3 / 2, c3, c3, c3 / 2, color);
}

lv_obj_t *ui_weather_icon(lv_obj_t *parent, ui_weather_kind_t kind, int size)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(box, size, size);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);

    int sun_d = size / 2;
    int sx = size / 4;
    int sy = size / 4;
    if (kind == UI_WEATHER_SUN || kind == UI_WEATHER_PARTLY) {
        if (kind == UI_WEATHER_PARTLY) {
            sun_d = size / 3;
            sx = size / 8;
            sy = size / 8;
        }
        add_round_rect(box, sx, sy, sun_d, sun_d, sun_d / 2, p->warning);
        int cx = sx + sun_d / 2;
        int cy = sy + sun_d / 2;
        int r1 = sun_d / 2 + size / 11;
        int r2 = sun_d / 2 + size / 5;
        add_line(box, cx - r2, cy, cx - r1, cy, 3, p->warning);
        add_line(box, cx + r1, cy, cx + r2, cy, 3, p->warning);
        add_line(box, cx, cy - r2, cx, cy - r1, 3, p->warning);
        add_line(box, cx, cy + r1, cx, cy + r2, 3, p->warning);
        int diag = (size * 3) / 10;
        add_line(box, cx - diag, cy - diag, cx - diag / 2, cy - diag / 2, 3, p->warning);
        add_line(box, cx + diag / 2, cy + diag / 2, cx + diag, cy + diag, 3, p->warning);
        add_line(box, cx + diag, cy - diag, cx + diag / 2, cy - diag / 2, 3, p->warning);
        add_line(box, cx - diag / 2, cy + diag / 2, cx - diag, cy + diag, 3, p->warning);
    }

    if (kind == UI_WEATHER_CLOUD || kind == UI_WEATHER_RAIN || kind == UI_WEATHER_PARTLY) {
        int cy = kind == UI_WEATHER_PARTLY ? size / 3 : 0;
        draw_cloud(box, size, 0, cy, p->accent);
    }
    if (kind == UI_WEATHER_RAIN) {
        for (int i = 0; i < 3; i++) {
            int x = size / 4 + i * size / 4;
            add_line(box, x, (size * 7) / 9, x - 3, size - 3, 3, p->accent);
        }
    }
    return box;
}

lv_obj_t *ui_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                    const lv_font_t *font, lv_event_cb_t cb, void *user_data)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_bg_color(button, p->surface, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, p->border, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_t *label = ui_label(button, text, font != NULL ? font : &ui_font_20, p->text);
    lv_obj_center(label);
    if (cb != NULL) {
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
    }
    ui_pressable(button);
    return button;
}
