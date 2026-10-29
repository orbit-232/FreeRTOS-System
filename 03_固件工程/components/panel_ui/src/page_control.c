#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "panel_music.h"
#include "panel_network.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_pages.h"
#include "ui_theme.h"
#include "ui_widgets.h"

typedef struct {
    lv_obj_t *dnd_tile;
    lv_obj_t *dnd_status;
    lv_obj_t *rotation_tile;
    lv_obj_t *rotation_status;
    lv_obj_t *brightness_label;
    lv_obj_t *volume_label;
    lv_obj_t *wifi_status;
    lv_obj_t *ble_status;
    lv_obj_t *music_title;
    lv_obj_t *music_artist;
    lv_obj_t *music_status;
    lv_obj_t *music_progress;
    lv_obj_t *music_play_icon;
    lv_timer_t *net_timer;
    lv_timer_t *music_timer;
    int demo_volume;
} control_ctx_t;

static void music_status_update(control_ctx_t *ctx)
{
    if (ctx == NULL || ctx->music_title == NULL) return;
    panel_music_status_t music;
    panel_music_get_status(&music);
    const char *text;
    switch (music.state) {
    case PANEL_MUSIC_LOADING: text = "加载中"; break;
    case PANEL_MUSIC_PLAYING: text = "播放中"; break;
    case PANEL_MUSIC_PAUSED: text = "已暂停"; break;
    case PANEL_MUSIC_STOPPING: text = "停止中"; break;
    case PANEL_MUSIC_STOPPED: text = music.position_ms > 0 ? "已结束" : "已停止"; break;
    case PANEL_MUSIC_ERROR: text = "失败"; break;
    default: text = "未开始"; break;
    }
    lv_label_set_text_fmt(ctx->music_status, "%u/4 · %s",
                          (unsigned)music.track + 1U, text);
    lv_label_set_text(ctx->music_title, music.title);
    lv_label_set_text(ctx->music_artist, music.artist);
    if (ctx->music_play_icon != NULL) {
        lv_label_set_text(ctx->music_play_icon,
                          music.state == PANEL_MUSIC_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if (ctx->music_progress != NULL) {
        ui_bar_set(ctx->music_progress, 0);
    }
    if (ctx->volume_label != NULL) {
        lv_label_set_text_fmt(ctx->volume_label, "%d%%", music.volume);
    }
}

static void music_timer_cb(lv_timer_t *timer)
{
    music_status_update(lv_timer_get_user_data(timer));
}

static void music_play_cb(lv_event_t *event)
{
    control_ctx_t *ctx = lv_event_get_user_data(event);
    panel_music_toggle();
    music_status_update(ctx);
}

static void music_stop_cb(lv_event_t *event)
{
    control_ctx_t *ctx = lv_event_get_user_data(event);
    panel_music_stop();
    music_status_update(ctx);
}

static void music_next_cb(lv_event_t *event)
{
    control_ctx_t *ctx = lv_event_get_user_data(event);
    panel_music_next();
    music_status_update(ctx);
}

static void music_prev_cb(lv_event_t *event)
{
    control_ctx_t *ctx = lv_event_get_user_data(event);
    panel_music_prev();
    music_status_update(ctx);
}

static void brightness_changed_cb(lv_event_t *event)
{
    control_ctx_t *ctx = lv_event_get_user_data(event);
    lv_obj_t *slider = lv_event_get_target_obj(event);
    int value = (int)lv_slider_get_value(slider);
    if (ctx != NULL && ctx->brightness_label != NULL) {
        lv_label_set_text_fmt(ctx->brightness_label, "%d%%", value);
    }
    panel_services_set_brightness(value);
}

static void brightness_released_cb(lv_event_t *event)
{
    (void)event;
    panel_services_schedule_save();
}

static void volume_changed_cb(lv_event_t *event)
{
    control_ctx_t *ctx = lv_event_get_user_data(event);
    lv_obj_t *slider = lv_event_get_target_obj(event);
    int value = (int)lv_slider_get_value(slider);
    if (ctx != NULL) {
        ctx->demo_volume = value;
        if (ctx->volume_label != NULL) {
            lv_label_set_text_fmt(ctx->volume_label, "%d%%", value);
        }
    }
    panel_music_set_volume(value);
}

static void apply_tile_state(lv_obj_t *tile, lv_obj_t *status, bool on, const char *name)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_set_style_bg_color(tile, on ? p->accent_soft : p->surface, 0);
    lv_obj_set_style_border_color(tile, on ? p->accent : p->border, 0);
    lv_label_set_text_fmt(status, "%s", on ? "开启" : "关闭");
    lv_obj_set_style_text_color(status, on ? p->accent : p->muted, 0);
    (void)name;
}

static lv_obj_t *state_tile(lv_obj_t *parent, int x, int y, const char *glyph, const char *name,
                            lv_obj_t **status_out, lv_event_cb_t cb, void *user_data)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *tile = ui_card(parent, x, y, 360, 150);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, user_data);
    ui_pressable(tile);
    lv_obj_t *icon = ui_label_at(tile, 22, 18, glyph, &lv_font_montserrat_24, p->accent);
    (void)icon;
    ui_label_at(tile, 22, 72, name, &ui_font_24, p->text);
    lv_obj_t *status = ui_label_at(tile, 22, 112, "关闭", &ui_font_16, p->muted);
    *status_out = status;
    return tile;
}

static void open_settings_network_cb(lv_event_t *event)
{
    (void)event;
    ui_page_settings_restore_section(0);
    ui_navigate(UI_PAGE_SETTINGS);
}

static lv_obj_t *info_tile(lv_obj_t *parent, int x, int y, const char *glyph, const char *name,
                           const char *status, const char *badge_text, lv_event_cb_t cb,
                           lv_obj_t **status_out)
{
    lv_obj_t *tile = ui_card(parent, x, y, 360, 150);
    lv_obj_set_style_bg_color(tile, ui_palette()->accent, 0);
    lv_obj_set_style_border_color(tile, ui_palette()->accent, 0);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);
    ui_pressable(tile);
    lv_obj_t *icon = ui_label_at(tile, 22, 18, glyph, &lv_font_montserrat_24, lv_color_white());
    (void)icon;
    ui_label_at(tile, 22, 72, name, &ui_font_24, lv_color_white());
    lv_obj_t *line = ui_label_at(tile, 22, 112, status, &ui_font_16, lv_color_hex(0xDCEBFA));
    lv_obj_set_width(line, 310);
    lv_label_set_long_mode(line, LV_LABEL_LONG_MODE_CLIP);
    if (status_out != NULL) *status_out = line;
    lv_obj_t *badge = ui_pill(tile, badge_text, lv_color_white(), lv_color_hex(0xFFFFFF),
                              &ui_font_16);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
    lv_obj_set_pos(badge, 260, 20);
    return tile;
}

static void dnd_clicked_cb(lv_event_t *event)
{
    control_ctx_t *ctx = lv_event_get_user_data(event);
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    settings.do_not_disturb = !settings.do_not_disturb;
    if (panel_services_update_settings(&settings)) {
        apply_tile_state(ctx->dnd_tile, ctx->dnd_status, settings.do_not_disturb, "勿扰");
    }
}

static void rotation_clicked_cb(lv_event_t *event)
{
    control_ctx_t *ctx = lv_event_get_user_data(event);
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    settings.rotation_locked = !settings.rotation_locked;
    if (panel_services_update_settings(&settings)) {
        apply_tile_state(ctx->rotation_tile, ctx->rotation_status, settings.rotation_locked,
                         "旋转锁定");
    }
}

static void control_delete_cb(lv_event_t *event)
{
    control_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx != NULL && ctx->net_timer != NULL) {
        lv_timer_delete(ctx->net_timer);
        ctx->net_timer = NULL;
    }
    if (ctx != NULL && ctx->music_timer != NULL) {
        lv_timer_delete(ctx->music_timer);
        ctx->music_timer = NULL;
    }
    lv_free(ctx);
}

static void control_net_timer_cb(lv_timer_t *timer)
{
    control_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (ctx == NULL) return;
    panel_wifi_snapshot_t wifi;
    panel_ble_snapshot_t ble;
    panel_network_wifi_get(&wifi);
    panel_network_ble_get(&ble);
    if (ctx->wifi_status != NULL) {
        if (wifi.connected) {
            lv_label_set_text_fmt(ctx->wifi_status, "%s · %s", wifi.ssid, wifi.ip);
        } else {
            lv_label_set_text(ctx->wifi_status,
                              wifi.state == PANEL_WIFI_UNAVAILABLE ? "不可用" : "未连接");
        }
    }
    if (ctx->ble_status != NULL) {
        if (ble.state == PANEL_BLE_CONNECTED) {
            lv_label_set_text_fmt(ctx->ble_status, "%s · 服务 %u",
                                  ble.connected_name[0] ? ble.connected_name : "(匿名)",
                                  ble.service_count);
        } else {
            lv_label_set_text(ctx->ble_status,
                              ble.state == PANEL_BLE_UNAVAILABLE ? "不可用" : "未连接");
        }
    }
}

static lv_obj_t *media_card(lv_obj_t *parent, control_ctx_t *ctx)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *card = ui_card(parent, 40, 510, 1200, 210);
    lv_obj_t *cover = lv_obj_create(card);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(cover, 24, 32);
    lv_obj_set_size(cover, 146, 146);
    lv_obj_set_style_radius(cover, 24, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x5E9FE8), 0);
    lv_obj_set_style_bg_grad_color(cover, lv_color_hex(0xBF8EDA), 0);
    lv_obj_set_style_bg_grad_dir(cover, LV_GRAD_DIR_HOR, 0);
    lv_obj_t *music = ui_label(cover, LV_SYMBOL_AUDIO, &lv_font_montserrat_40,
                               lv_color_white());
    lv_obj_center(music);

    ctx->music_title = ui_label_at(card, 196, 34, "After Hours", &ui_font_32, p->text);
    ctx->music_artist = ui_label_at(card, 198, 82, "The Weeknd · TF 音乐", &ui_font_16,
                                    p->muted);
    ctx->music_status = ui_pill(card, "未开始", p->muted, p->surface_alt, &ui_font_16);
    lv_obj_set_pos(ctx->music_status, 198, 112);
    ctx->music_progress = ui_bar_create(card, 196, 160, 560, 6);
    ui_bar_set(ctx->music_progress, 0);

    lv_obj_t *prev = lv_button_create(card);
    lv_obj_set_size(prev, 96, 50);
    lv_obj_align(prev, LV_ALIGN_RIGHT_MID, -330, 0);
    lv_obj_set_style_radius(prev, 25, 0);
    lv_obj_set_style_bg_color(prev, p->surface_alt, 0);
    lv_obj_set_style_border_width(prev, 0, 0);
    lv_obj_add_event_cb(prev, music_prev_cb, LV_EVENT_CLICKED, ctx);
    ui_pressable(prev);
    lv_obj_t *prev_label = ui_label(prev, "上一个", &ui_font_16, p->text);
    lv_obj_center(prev_label);

    lv_obj_t *next = lv_button_create(card);
    lv_obj_set_size(next, 96, 50);
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_radius(next, 25, 0);
    lv_obj_set_style_bg_color(next, p->surface_alt, 0);
    lv_obj_set_style_border_width(next, 0, 0);
    lv_obj_add_event_cb(next, music_next_cb, LV_EVENT_CLICKED, ctx);
    ui_pressable(next);
    lv_obj_t *next_label = ui_label(next, "下一个", &ui_font_16, p->text);
    lv_obj_center(next_label);

    lv_obj_t *stop = lv_button_create(card);
    lv_obj_set_size(stop, 52, 52);
    lv_obj_align(stop, LV_ALIGN_RIGHT_MID, -220, 0);
    lv_obj_set_style_radius(stop, 26, 0);
    lv_obj_set_style_bg_color(stop, p->surface_alt, 0);
    lv_obj_set_style_border_width(stop, 0, 0);
    lv_obj_add_event_cb(stop, music_stop_cb, LV_EVENT_CLICKED, ctx);
    ui_pressable(stop);
    lv_obj_t *stop_icon = ui_label(stop, LV_SYMBOL_STOP, &lv_font_montserrat_24, p->text);
    lv_obj_center(stop_icon);

    lv_obj_t *play = lv_button_create(card);
    lv_obj_set_size(play, 72, 72);
    lv_obj_align(play, LV_ALIGN_RIGHT_MID, -120, 0);
    lv_obj_set_style_radius(play, 36, 0);
    lv_obj_set_style_bg_color(play, p->text, 0);
    lv_obj_set_style_border_width(play, 0, 0);
    lv_obj_add_event_cb(play, music_play_cb, LV_EVENT_CLICKED, ctx);
    ui_pressable(play);
    ctx->music_play_icon = ui_label(play, LV_SYMBOL_PLAY, &lv_font_montserrat_32,
                                    p->surface);
    lv_obj_center(ctx->music_play_icon);
    return card;
}

lv_obj_t *ui_page_control_create_on(lv_obj_t *parent)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *screen;
    if (parent == NULL) {
        screen = ui_screen_create();
    } else {
        screen = lv_obj_create(parent);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(screen, 0, 0);
        lv_obj_set_size(screen, UI_WIDTH, UI_HEIGHT);
        lv_obj_set_style_bg_color(screen, p->bg, 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(screen, 0, 0);
        lv_obj_set_style_radius(screen, 0, 0);
        lv_obj_set_style_pad_all(screen, 0, 0);
        lv_obj_set_style_text_color(screen, p->text, 0);
        lv_obj_set_style_text_font(screen, &ui_font_20, 0);
    }
    ui_status_bar(screen, false);

    ui_label_at(screen, 40, 70, "控制中心", &ui_font_32, p->text);
    ui_label_at(screen, 40, 112, "顶部下滑打开 · 底部上滑或返回关闭", &ui_font_16,
                p->muted);

    control_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return screen;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->demo_volume = 20;

    panel_settings_t settings;
    panel_services_get_settings(&settings);

    panel_wifi_snapshot_t wifi;
    panel_ble_snapshot_t ble;
    panel_network_wifi_get(&wifi);
    panel_network_ble_get(&ble);
    char wifi_text[72];
    char ble_text[72];
    const char *wifi_badge = "未连接";
    const char *ble_badge = "未连接";
    if (wifi.connected) {
        snprintf(wifi_text, sizeof(wifi_text), "%s · %s", wifi.ssid, wifi.ip);
        wifi_badge = "已连接";
    } else {
        snprintf(wifi_text, sizeof(wifi_text), "%s", wifi.state == PANEL_WIFI_UNAVAILABLE
                 ? "不可用" : "未连接");
    }
    if (ble.state == PANEL_BLE_CONNECTED) {
        snprintf(ble_text, sizeof(ble_text), "%s · 服务 %u",
                 ble.connected_name[0] ? ble.connected_name : "(匿名)", ble.service_count);
        ble_badge = "已连接";
    } else {
        snprintf(ble_text, sizeof(ble_text), "%s", ble.state == PANEL_BLE_UNAVAILABLE
                 ? "不可用" : "未连接");
    }

    info_tile(screen, 40, 166, LV_SYMBOL_WIFI, "Wi-Fi", wifi_text, wifi_badge,
              open_settings_network_cb, &ctx->wifi_status);
    info_tile(screen, 420, 166, LV_SYMBOL_BLUETOOTH, "蓝牙", ble_text, ble_badge,
              open_settings_network_cb, &ctx->ble_status);
    ctx->dnd_tile = state_tile(screen, 40, 336, LV_SYMBOL_BELL, "勿扰", &ctx->dnd_status,
                              dnd_clicked_cb, ctx);
    ctx->rotation_tile = state_tile(screen, 420, 336, LV_SYMBOL_REFRESH, "旋转锁定",
                                   &ctx->rotation_status, rotation_clicked_cb, ctx);
    apply_tile_state(ctx->dnd_tile, ctx->dnd_status, settings.do_not_disturb, "勿扰");
    apply_tile_state(ctx->rotation_tile, ctx->rotation_status, settings.rotation_locked,
                     "旋转锁定");

    lv_obj_t *bright = ui_card(screen, 800, 166, 440, 150);
    ui_label_at(bright, 22, 18, "亮度 · 真实背光", &ui_font_20, p->text);
    ctx->brightness_label = ui_label_at(bright, 350, 14, "70%", &ui_font_24, p->text);
    lv_obj_set_width(ctx->brightness_label, 70);
    lv_obj_set_style_text_align(ctx->brightness_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *bright_icon = ui_label_at(bright, 22, 82, LV_SYMBOL_TINT,
                                        &lv_font_montserrat_24, p->muted);
    (void)bright_icon;
    lv_obj_t *brightness = ui_slider_create(bright, 62, 78, 320, PANEL_BRIGHTNESS_MIN,
                                            PANEL_BRIGHTNESS_MAX, settings.brightness);
    lv_obj_add_event_cb(brightness, brightness_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(brightness, brightness_released_cb, LV_EVENT_RELEASED, NULL);
    lv_label_set_text_fmt(ctx->brightness_label, "%u%%", settings.brightness);

    lv_obj_t *volume = ui_card(screen, 800, 336, 440, 150);
    ui_label_at(volume, 22, 18, "音量 · 音乐", &ui_font_20, p->text);
    ctx->volume_label = ui_label_at(volume, 350, 14, "20%", &ui_font_24, p->text);
    lv_obj_set_width(ctx->volume_label, 70);
    lv_obj_set_style_text_align(ctx->volume_label, LV_TEXT_ALIGN_RIGHT, 0);
    ui_label_at(volume, 22, 82, LV_SYMBOL_VOLUME_MID, &lv_font_montserrat_24, p->muted);
    lv_obj_t *volume_slider = ui_slider_create(volume, 62, 78, 320, 0, 100, ctx->demo_volume);
    lv_obj_add_event_cb(volume_slider, volume_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_t *badge = ui_pill(volume, "TF 音乐", p->muted, p->surface_alt, &ui_font_16);
    lv_obj_set_pos(badge, 226, 19);
    lv_label_set_text_fmt(ctx->volume_label, "%d%%", ctx->demo_volume);

    media_card(screen, ctx);
    ctx->music_timer = lv_timer_create(music_timer_cb, 250, ctx);
    music_status_update(ctx);
    ctx->net_timer = lv_timer_create(control_net_timer_cb, 1000, ctx);
    ui_nav_bar(screen, UI_PAGE_CONTROL);
    lv_obj_add_event_cb(screen, control_delete_cb, LV_EVENT_DELETE, ctx);
    return screen;
}

lv_obj_t *ui_page_control_create(void)
{
    return ui_page_control_create_on(NULL);
}
