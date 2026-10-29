#include "panel_music_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "music_assets.h"
#include "panel_bsp.h"
#include "panel_music.h"
#include "panel_music_resample.h"
#include "panel_music_tracks.h"

#ifndef ENABLE_DISC_SPIN
/* Diagnostic build: cover rotation is temporarily disabled as the single
 * behavioural variable.  Audio/seek/status/arc rendering remain unchanged. */
#define ENABLE_DISC_SPIN 0
#endif
/* Diagnostic mitigation: 10 fps still completes one revolution in 20 s but
 * halves the transformed-image invalidations compared with 20 fps. */
#ifndef MUSIC_UI_DISC_PERIOD_MS
#define MUSIC_UI_DISC_PERIOD_MS 100U
#endif
#define MUSIC_UI_DISC_STEP 18U
#define MUSIC_UI_HEARTBEAT_MS 250U

#define SCR_W 1280
#define SCR_H 800
#define CX (SCR_W / 2)
#define CY_RECORD 266
#define RING_SIZE 380
#define RECORD_SIZE 344
#define COVER_SIZE 180
#define CARD_SIZE 160
#define CARD_LEFT_X 96
#define CARD_RIGHT_X (SCR_W - 96 - CARD_SIZE)
#define CONTROLS_Y (648 - 56 / 2)
#define PLAY_TOP (648 - PLAY_SIZE / 2)
#define PLAY_SIZE 76
#define BOTTOM_Y (SCR_H - 64)

#define COL_BG 0x101012
#define COL_TEXT 0xF4F1FA
#define COL_MUTED 0x9B96AC
#define COL_DIM 0x6F6A80
#define COL_PURPLE 0x8B7CF6
#define COL_ORANGE 0xF2A65A
#define COL_TRACK 0x2A2733
#define COL_SURFACE 0x1A1822
#define COL_SURFACE_HI 0x24202F
#define COL_LIKE 0xE05A6B
#define COL_LIKE_DIM 0xC64B5A
#define COL_ORANGE_DIM 0xC9844D

LV_FONT_DECLARE(music_font_12);
LV_FONT_DECLARE(music_font_13);
LV_FONT_DECLARE(music_font_14);
LV_FONT_DECLARE(music_font_16);
LV_FONT_DECLARE(music_font_15_bold);
LV_FONT_DECLARE(music_font_32_bold);

static const char *TAG = "music_ui";

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *center_layer;
    lv_obj_t *cards_layer;
    lv_obj_t *arc;
    lv_obj_t *cover;
    lv_obj_t *title;
    lv_obj_t *artist;
    lv_obj_t *time_label;
    lv_obj_t *status_label;
    lv_obj_t *top_state;
    lv_obj_t *top_subtitle;
    lv_obj_t *play_icon;
    lv_obj_t *shuffle_icon;
    lv_obj_t *repeat_icon;
    lv_obj_t *like_icon;
    lv_obj_t *volume_icon;
    lv_obj_t *volume_value;
    lv_obj_t *volume_slider;
    lv_obj_t *btn_play;
    lv_obj_t *btn_prev;
    lv_obj_t *btn_next;
    lv_obj_t *btn_like;
    lv_obj_t *btn_shuffle;
    lv_obj_t *btn_repeat;
    lv_obj_t *btn_list;
    lv_obj_t *btn_queue;
    lv_obj_t *btn_download;
    lv_obj_t *prev_card_img;
    lv_obj_t *next_card_img;
    lv_obj_t *prev_card_title;
    lv_obj_t *next_card_title;
    lv_timer_t *status_timer;
    lv_timer_t *disc_timer;
    lv_timer_t *heartbeat_timer;
    uint16_t disc_angle;
    uint8_t shown_track;
    bool scrubbing;
    bool switching;
    bool index_ready;
    int switch_dir;
    uint32_t scrub_ms;
} music_ctx_t;

static music_ctx_t *s_ctx;
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
static bool s_started;
static volatile bool s_disc_spin_enabled = ENABLE_DISC_SPIN;
static volatile uint32_t s_ui_heartbeat_count;
static volatile uint32_t s_ui_heartbeat_ms;

static const lv_image_dsc_t *cover_for(unsigned track)
{
    switch (track % PANEL_MUSIC_TRACK_COUNT) {
    case 0: return &music_cover_0;
    case 1: return &music_cover_1;
    case 2: return &music_cover_2;
    default: return &music_cover_3;
    }
}

static const lv_image_dsc_t *card_for(unsigned track)
{
    switch (track % PANEL_MUSIC_TRACK_COUNT) {
    case 0: return &music_card_0;
    case 1: return &music_card_1;
    case 2: return &music_card_2;
    default: return &music_card_3;
    }
}

static lv_color_t state_color(panel_music_state_t state)
{
    if (state == PANEL_MUSIC_PLAYING) return lv_color_hex(COL_ORANGE);
    if (state == PANEL_MUSIC_PAUSED) return lv_color_hex(COL_PURPLE);
    if (state == PANEL_MUSIC_ERROR) return lv_color_hex(0xE06C75);
    return lv_color_hex(COL_MUTED);
}

static const char *state_text(panel_music_state_t state, uint32_t position_ms)
{
    switch (state) {
    case PANEL_MUSIC_LOADING: return "正在加载";
    case PANEL_MUSIC_PLAYING: return "正在播放";
    case PANEL_MUSIC_PAUSED: return "已暂停";
    case PANEL_MUSIC_STOPPING: return "正在停止";
    case PANEL_MUSIC_STOPPED: return position_ms > 0U ? "播放结束" : "未播放";
    case PANEL_MUSIC_ERROR: return "播放失败";
    default: return "未播放";
    }
}

static void format_time(uint32_t ms, char *out, size_t cap)
{
    unsigned total = ms / 1000U;
    unsigned minute = total / 60U;
    unsigned second = total % 60U;
    snprintf(out, cap, "%u:%02u", minute, second);
}

static void pressable(lv_obj_t *obj)
{
    lv_obj_set_style_transform_scale(obj, 241, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(obj, lv_pct(50), LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_y(obj, lv_pct(50), LV_STATE_PRESSED);
}

static lv_obj_t *icon_image(lv_obj_t *parent, music_icon_t icon, bool large,
                            lv_color_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(img, music_asset_icon(icon, large));
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

static lv_obj_t *icon_button(lv_obj_t *parent, int x, int y, int size,
                             music_icon_t icon, uint32_t color, bool large,
                             lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_PURPLE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, 32, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(btn, 4);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    pressable(btn);
    lv_obj_t *img = icon_image(btn, icon, large, lv_color_hex(color));
    lv_obj_center(img);
    return btn;
}

static void toast_delete_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_toast != NULL) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    s_toast_timer = NULL;
}

static void toast(const char *text)
{
    if (text == NULL) return;
    if (s_toast_timer != NULL) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (s_toast != NULL) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x201D2A), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x3B354C), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 20, 0);
    lv_obj_set_style_pad_hor(box, 20, 0);
    lv_obj_set_style_pad_ver(box, 9, 0);
    lv_obj_set_style_shadow_width(box, 0, 0);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &music_font_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(label);
    s_toast = box;
    s_toast_timer = lv_timer_create(toast_delete_cb, 1800, NULL);
}

static void update_card_labels(music_ctx_t *ctx, unsigned track)
{
    unsigned prev = (track + PANEL_MUSIC_TRACK_COUNT - 1U) % PANEL_MUSIC_TRACK_COUNT;
    unsigned next = (track + 1U) % PANEL_MUSIC_TRACK_COUNT;
    const panel_music_track_t *p = panel_music_track_at(prev);
    const panel_music_track_t *n = panel_music_track_at(next);
    lv_image_set_src(ctx->prev_card_img, card_for(prev));
    lv_image_set_src(ctx->next_card_img, card_for(next));
    lv_label_set_text(ctx->prev_card_title, p != NULL ? p->title : "");
    lv_label_set_text(ctx->next_card_title, n != NULL ? n->title : "");
}

static void apply_track_visual(music_ctx_t *ctx, unsigned track)
{
    if (ctx->shown_track == track) return;
    ctx->shown_track = (uint8_t)track;
    lv_image_set_src(ctx->cover, cover_for(track));
    update_card_labels(ctx, track);
}

static void refresh_ui(music_ctx_t *ctx)
{
    if (ctx == NULL) return;
    panel_music_status_t st;
    panel_music_get_status(&st);
    if (st.track >= PANEL_MUSIC_TRACK_COUNT) st.track = 0;
    if (!ctx->switching) apply_track_visual(ctx, st.track);

    lv_label_set_text(ctx->top_state, state_text(st.state, st.position_ms));
    lv_obj_set_style_text_color(ctx->top_state, state_color(st.state), 0);
    lv_label_set_text_fmt(ctx->top_subtitle, "TF 音乐 · %u 首", PANEL_MUSIC_TRACK_COUNT);

    lv_label_set_text(ctx->title, st.title[0] ? st.title : "未知曲目");
    lv_label_set_text(ctx->artist, st.artist[0] ? st.artist : "未知歌手");

    char pos_text[16];
    char dur_text[16];
    uint32_t shown_pos = ctx->scrubbing ? ctx->scrub_ms : st.position_ms;
    format_time(shown_pos, pos_text, sizeof(pos_text));
    if (st.duration_ms > 0U) {
        format_time(st.duration_ms, dur_text, sizeof(dur_text));
        lv_label_set_text_fmt(ctx->time_label, "%s / %s", pos_text, dur_text);
    } else {
        lv_label_set_text_fmt(ctx->time_label, "%s / --:--", pos_text);
    }

    ctx->index_ready = st.index_ready;
    if (st.state == PANEL_MUSIC_ERROR) {
        lv_label_set_text(ctx->status_label,
                          st.detail[0] ? st.detail : "播放失败");
    } else if (st.state == PANEL_MUSIC_STOPPING) {
        lv_label_set_text(ctx->status_label, "正在停止");
    } else if (st.state == PANEL_MUSIC_STOPPED) {
        lv_label_set_text(ctx->status_label, st.detail);
    } else if (st.index_failed) {
        lv_label_set_text(ctx->status_label, "索引不可用 · 仍可播放，暂不能跳转");
    } else if (st.index_building) {
        lv_label_set_text(ctx->status_label, "索引构建中 · 暂不能跳转");
    } else if (st.state == PANEL_MUSIC_PLAYING) {
        lv_label_set_text_fmt(ctx->status_label, "音源 %u Hz/%s · 输出 %u Hz/单声道",
                              (unsigned)st.sample_rate,
                              st.channels == 1U ? "单声道" : "立体声",
                              (unsigned)PANEL_MUSIC_OUT_HZ);
    } else if (st.state == PANEL_MUSIC_LOADING) {
        lv_label_set_text(ctx->status_label, "正在读取TF卡");
    } else if (st.state == PANEL_MUSIC_PAUSED || st.state == PANEL_MUSIC_STOPPED) {
        lv_label_set_text(ctx->status_label, st.detail);
    } else {
        lv_label_set_text(ctx->status_label, "按播放开始 · 索引将在后台构建");
    }

    if (!ctx->index_ready || st.duration_ms == 0U) {
        lv_obj_remove_flag(ctx->arc, LV_OBJ_FLAG_CLICKABLE);
        lv_arc_set_range(ctx->arc, 0, 1000);
        lv_arc_set_value(ctx->arc, 0);
    } else if (!ctx->scrubbing) {
        lv_obj_add_flag(ctx->arc, LV_OBJ_FLAG_CLICKABLE);
        int32_t max = (int32_t)st.duration_ms;
        lv_arc_set_range(ctx->arc, 0, max);
        int32_t value = st.position_ms > st.duration_ms ? max : (int32_t)st.position_ms;
        lv_arc_set_value(ctx->arc, value);
    }

    lv_image_set_src(ctx->play_icon,
                     music_asset_icon(st.state == PANEL_MUSIC_PLAYING ? MUSIC_ICON_PAUSE
                                                                    : MUSIC_ICON_PLAY,
                                      true));
    lv_image_set_src(ctx->shuffle_icon,
                     music_asset_icon(MUSIC_ICON_SHUFFLE, true));
    lv_obj_set_style_image_recolor(ctx->shuffle_icon,
                                   lv_color_hex(st.shuffle ? COL_ORANGE : COL_ORANGE_DIM), 0);
    lv_obj_set_style_image_recolor(ctx->repeat_icon,
                                   lv_color_hex(st.repeat ? COL_ORANGE : COL_MUTED), 0);
    lv_image_set_src(ctx->like_icon, music_asset_icon(MUSIC_ICON_HEART_FILLED, false));
    lv_obj_set_style_image_recolor(ctx->like_icon,
                                   lv_color_hex(st.liked ? COL_LIKE : COL_LIKE_DIM), 0);

    if (lv_slider_get_value(ctx->volume_slider) != st.volume) {
        lv_slider_set_value(ctx->volume_slider, st.volume, LV_ANIM_OFF);
    }
    lv_label_set_text_fmt(ctx->volume_value, "%d%%", st.volume);
    lv_image_set_src(ctx->volume_icon,
                     music_asset_icon(st.volume == 0 ? MUSIC_ICON_VOLUME_MUTE : MUSIC_ICON_VOLUME,
                                      false));
    lv_obj_set_style_image_recolor(ctx->volume_icon,
                                   lv_color_hex(st.volume == 0 ? COL_DIM : COL_MUTED), 0);
}

static void status_timer_cb(lv_timer_t *timer)
{
    refresh_ui(lv_timer_get_user_data(timer));
}

#if ENABLE_DISC_SPIN
static void disc_timer_cb(lv_timer_t *timer)
{
    music_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (ctx == NULL || ctx->cover == NULL || !s_disc_spin_enabled) return;
    panel_music_status_t st;
    panel_music_get_status(&st);
    if (st.state != PANEL_MUSIC_PLAYING) return;
    ctx->disc_angle = (uint16_t)((ctx->disc_angle + MUSIC_UI_DISC_STEP) % 3600U);
    lv_image_set_rotation(ctx->cover, (int32_t)ctx->disc_angle);
}
#endif

static void heartbeat_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_ui_heartbeat_count++;
    s_ui_heartbeat_ms = lv_tick_get();
}

static void play_cb(lv_event_t *event)
{
    music_ctx_t *ctx = lv_event_get_user_data(event);
    panel_music_toggle();
    refresh_ui(ctx);
}

static void prev_cb(lv_event_t *event);
static void next_cb(lv_event_t *event);

static void switch_in_completed(lv_anim_t *anim)
{
    music_ctx_t *ctx = lv_anim_get_user_data(anim);
    if (ctx != NULL) ctx->switching = false;
}

static void switch_midpoint(lv_anim_t *anim)
{
    music_ctx_t *ctx = lv_anim_get_user_data(anim);
    if (ctx == NULL) return;
    int dir = ctx->switch_dir;
    ctx->switching = false;
    lv_obj_set_x(ctx->center_layer, dir * 40);
    lv_obj_set_x(ctx->cards_layer, dir * 24);
    refresh_ui(ctx);

    lv_anim_t in;
    lv_anim_init(&in);
    lv_anim_set_var(&in, ctx->center_layer);
    lv_anim_set_values(&in, dir * 40, 0);
    lv_anim_set_duration(&in, 150);
    lv_anim_set_path_cb(&in, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&in, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_completed_cb(&in, switch_in_completed);
    lv_anim_set_user_data(&in, ctx);
    lv_anim_start(&in);

    lv_anim_t cards;
    lv_anim_init(&cards);
    lv_anim_set_var(&cards, ctx->cards_layer);
    lv_anim_set_values(&cards, dir * 24, 0);
    lv_anim_set_duration(&cards, 150);
    lv_anim_set_path_cb(&cards, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&cards, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_start(&cards);
}

static void start_switch_animation(music_ctx_t *ctx, int dir)
{
    if (ctx->switching) return;
    ctx->switching = true;
    ctx->switch_dir = dir;

    lv_anim_t out;
    lv_anim_init(&out);
    lv_anim_set_var(&out, ctx->center_layer);
    lv_anim_set_values(&out, 0, -dir * 40);
    lv_anim_set_duration(&out, 150);
    lv_anim_set_path_cb(&out, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&out, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_completed_cb(&out, switch_midpoint);
    lv_anim_set_user_data(&out, ctx);
    lv_anim_start(&out);

    lv_anim_t cards;
    lv_anim_init(&cards);
    lv_anim_set_var(&cards, ctx->cards_layer);
    lv_anim_set_values(&cards, 0, -dir * 24);
    lv_anim_set_duration(&cards, 150);
    lv_anim_set_path_cb(&cards, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&cards, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_start(&cards);
}

static void prev_cb(lv_event_t *event)
{
    music_ctx_t *ctx = lv_event_get_user_data(event);
    panel_music_prev();
    start_switch_animation(ctx, -1);
}

static void next_cb(lv_event_t *event)
{
    music_ctx_t *ctx = lv_event_get_user_data(event);
    panel_music_next();
    start_switch_animation(ctx, 1);
}

static void like_cb(lv_event_t *event)
{
    music_ctx_t *ctx = lv_event_get_user_data(event);
    bool liked = panel_music_toggle_like();
    refresh_ui(ctx);
    toast(liked ? "已加入收藏" : "已取消收藏");
}

static void shuffle_cb(lv_event_t *event)
{
    music_ctx_t *ctx = lv_event_get_user_data(event);
    panel_music_status_t st;
    panel_music_get_status(&st);
    bool on = panel_music_set_shuffle(!st.shuffle);
    refresh_ui(ctx);
    toast(on ? "随机播放已开启" : "随机播放已关闭");
}

static void repeat_cb(lv_event_t *event)
{
    music_ctx_t *ctx = lv_event_get_user_data(event);
    panel_music_status_t st;
    panel_music_get_status(&st);
    bool on = panel_music_set_repeat(!st.repeat);
    refresh_ui(ctx);
    toast(on ? "列表循环已开启" : "列表循环已关闭");
}

static void unavailable_list_cb(lv_event_t *event)
{
    (void)event;
    toast("列表/歌单功能尚未实现，当前不可用");
}

static void unavailable_queue_cb(lv_event_t *event)
{
    (void)event;
    toast("播放队列功能尚未实现，当前不可用");
}

static void unavailable_download_cb(lv_event_t *event)
{
    (void)event;
    toast("下载功能尚未实现，当前不可用");
}

static void unavailable_collapse_cb(lv_event_t *event)
{
    (void)event;
    toast("最小化功能尚未实现，本机只保留音乐界面");
}

static void unavailable_more_cb(lv_event_t *event)
{
    (void)event;
    toast("更多功能尚未实现，当前不可用");
}

static void arc_pressed_cb(lv_event_t *event)
{
    music_ctx_t *ctx = lv_event_get_user_data(event);
    ctx->scrubbing = true;
    ctx->scrub_ms = (uint32_t)lv_arc_get_value(ctx->arc);
}

static void arc_changed_cb(lv_event_t *event)
{
    music_ctx_t *ctx = lv_event_get_user_data(event);
    if (!ctx->scrubbing) return;
    ctx->scrub_ms = (uint32_t)lv_arc_get_value(ctx->arc);
    panel_music_status_t st;
    panel_music_get_status(&st);
    char pos_text[16], dur_text[16];
    format_time(ctx->scrub_ms, pos_text, sizeof(pos_text));
    if (st.duration_ms > 0U) {
        format_time(st.duration_ms, dur_text, sizeof(dur_text));
        lv_label_set_text_fmt(ctx->time_label, "%s / %s", pos_text, dur_text);
    } else {
        lv_label_set_text_fmt(ctx->time_label, "%s / --:--", pos_text);
    }
    lv_label_set_text(ctx->status_label, "松开即按索引跳转");
}

static void arc_released_cb(lv_event_t *event)
{
    music_ctx_t *ctx = lv_event_get_user_data(event);
    if (!ctx->scrubbing) return;
    uint32_t target = (uint32_t)lv_arc_get_value(ctx->arc);
    ctx->scrubbing = false;
    if (panel_music_seek(target) != ESP_OK) {
        toast("当前状态无法跳转");
    }
    refresh_ui(ctx);
}

static void volume_changed_cb(lv_event_t *event)
{
    music_ctx_t *ctx = lv_event_get_user_data(event);
    int value = (int)lv_slider_get_value(lv_event_get_target_obj(event));
    panel_music_set_volume(value);
    lv_label_set_text_fmt(ctx->volume_value, "%d%%", value);
    lv_image_set_src(ctx->volume_icon,
                     music_asset_icon(value == 0 ? MUSIC_ICON_VOLUME_MUTE : MUSIC_ICON_VOLUME,
                                      false));
    lv_obj_set_style_image_recolor(ctx->volume_icon,
                                   lv_color_hex(value == 0 ? COL_DIM : COL_MUTED), 0);
}

static lv_obj_t *small_label(lv_obj_t *parent, int x, int y, int w, const char *text,
                             const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *bottom_button(lv_obj_t *parent, int x, music_icon_t icon,
                               const char *label, lv_event_cb_t cb, void *user_data,
                               lv_obj_t **icon_out)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, 736);
    lv_obj_set_size(btn, 48, 56);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_PURPLE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, 28, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    pressable(btn);
    lv_obj_t *img = icon_image(btn, icon, false, lv_color_hex(COL_MUTED));
    lv_obj_set_pos(img, 12, 5);
    small_label(btn, 0, 31, 48, label, &music_font_12, COL_DIM);
    if (icon_out != NULL) *icon_out = img;
    return btn;
}

static lv_obj_t *make_card(music_ctx_t *ctx, int x, bool previous)
{
    (void)ctx;
    lv_obj_t *btn = lv_button_create(s_ctx->cards_layer);
    lv_obj_set_pos(btn, x, 186);
    lv_obj_set_size(btn, 160, 160);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_PURPLE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, 26, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, previous ? prev_cb : next_cb, LV_EVENT_CLICKED, s_ctx);
    pressable(btn);
    lv_obj_t *img = lv_image_create(btn);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_set_size(img, 160, 160);
    lv_image_set_src(img, card_for(previous ? 3 : 1));
    lv_obj_t *hint = small_label(s_ctx->cards_layer, x, 356, 160,
                                 previous ? "上一首" : "下一首", &music_font_12, COL_DIM);
    (void)hint;
    lv_obj_t *title = small_label(s_ctx->cards_layer, x, 376, 160,
                                  previous ? "After Hours" : "Starboy", &music_font_13,
                                  COL_MUTED);
    if (previous) {
        s_ctx->prev_card_img = img;
        s_ctx->prev_card_title = title;
    } else {
        s_ctx->next_card_img = img;
        s_ctx->next_card_title = title;
    }
    return btn;
}

static void ctx_delete_cb(lv_event_t *event)
{
    music_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx == NULL) return;
    if (ctx->status_timer != NULL) lv_timer_delete(ctx->status_timer);
    if (ctx->disc_timer != NULL) lv_timer_delete(ctx->disc_timer);
    if (ctx->heartbeat_timer != NULL) lv_timer_delete(ctx->heartbeat_timer);
    if (s_toast_timer != NULL) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (s_toast != NULL) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    s_ctx = NULL;
    lv_free(ctx);
}

static lv_obj_t *build_page(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    if (screen == NULL) return NULL;
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *bg = lv_image_create(screen);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(bg, 0, 0);
    lv_image_set_src(bg, &music_bg);
    lv_obj_move_to_index(bg, 0);

    music_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        lv_obj_delete(screen);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->screen = screen;
    ctx->shown_track = 0xFF;
    s_ctx = ctx;

    /* 顶部：44 px 左右按钮 + 双行标题。 */
    icon_button(screen, 40, 20, 44, MUSIC_ICON_CHEVRON_DOWN, COL_TEXT, false,
                unavailable_collapse_cb, ctx);
    icon_button(screen, SCR_W - 84, 20, 44, MUSIC_ICON_MORE, COL_TEXT, false,
                unavailable_more_cb, ctx);
    ctx->top_state = small_label(screen, 340, 8, 600, "未播放", &music_font_14, COL_MUTED);
    ctx->top_subtitle = lv_label_create(screen);
    lv_label_set_text_fmt(ctx->top_subtitle, "TF 音乐 · %u 首", PANEL_MUSIC_TRACK_COUNT);
    lv_obj_set_style_text_font(ctx->top_subtitle, &music_font_15_bold, 0);
    lv_obj_set_style_text_color(ctx->top_subtitle, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_width(ctx->top_subtitle, 600);
    lv_obj_set_style_text_align(ctx->top_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ctx->top_subtitle, 340, 29);

    /* 中央：380 px 进度环 + 344 px 唱片 + 180 px 封面。 */
    ctx->center_layer = lv_obj_create(screen);
    lv_obj_remove_flag(ctx->center_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(ctx->center_layer, 0, 0);
    lv_obj_set_size(ctx->center_layer, SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(ctx->center_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->center_layer, 0, 0);
    lv_obj_set_style_pad_all(ctx->center_layer, 0, 0);

    lv_obj_t *arc = lv_arc_create(ctx->center_layer);
    ctx->arc = arc;
    lv_obj_set_size(arc, RING_SIZE, RING_SIZE);
    lv_obj_set_pos(arc, CX - RING_SIZE / 2, CY_RECORD - RING_SIZE / 2);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arc, 0, 0);
    lv_obj_set_style_pad_all(arc, 0, 0);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_range(arc, 0, 1000);
    lv_arc_set_value(arc, 0);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x4B3F66), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(arc, lv_color_hex(0xF7F4FF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(arc, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(arc, arc_pressed_cb, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(arc, arc_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(arc, arc_released_cb, LV_EVENT_RELEASED, ctx);

    lv_obj_t *mask = lv_obj_create(ctx->center_layer);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(mask, CX - 172, CY_RECORD - 172);
    lv_obj_set_size(mask, 344, 344);
    lv_obj_set_style_radius(mask, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);

    lv_obj_t *vinyl = lv_image_create(ctx->center_layer);
    lv_obj_remove_flag(vinyl, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(vinyl, CX - 172, CY_RECORD - 172);
    lv_image_set_src(vinyl, &music_vinyl);

    ctx->cover = lv_image_create(ctx->center_layer);
    lv_obj_remove_flag(ctx->cover, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(ctx->cover, CX - 90, CY_RECORD - 90);
    lv_image_set_src(ctx->cover, &music_cover_0);
    lv_image_set_pivot(ctx->cover, 90, 90);
    lv_image_set_rotation(ctx->cover, 0);

    lv_obj_t *hole = lv_obj_create(ctx->center_layer);
    lv_obj_remove_flag(hole, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(hole, CX - 5, CY_RECORD - 5);
    lv_obj_set_size(hole, 10, 10);
    lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hole, lv_color_hex(0x07070A), 0);
    lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hole, lv_color_hex(0x5A4B76), 0);
    lv_obj_set_style_border_width(hole, 1, 0);
    lv_obj_set_style_pad_all(hole, 0, 0);

    ctx->title = small_label(ctx->center_layer, 140, 484, 1000, "After Hours",
                             &music_font_32_bold, COL_TEXT);
    lv_obj_set_style_text_align(ctx->title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ctx->title, LV_LABEL_LONG_MODE_CLIP);
    ctx->artist = small_label(ctx->center_layer, 140, 536, 1000, "The Weeknd",
                              &music_font_16, COL_MUTED);
    ctx->time_label = small_label(ctx->center_layer, 140, 576, 1000, "0:00 / --:--",
                                  &music_font_13, COL_DIM);
    ctx->status_label = small_label(ctx->center_layer, 140, 592, 1000,
                                    "按播放开始 · 拖动进度环可跳转", &music_font_12,
                                    COL_MUTED);

    /* 两侧曲目卡。 */
    ctx->cards_layer = lv_obj_create(screen);
    lv_obj_remove_flag(ctx->cards_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(ctx->cards_layer, 0, 0);
    lv_obj_set_size(ctx->cards_layer, SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(ctx->cards_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->cards_layer, 0, 0);
    lv_obj_set_style_pad_all(ctx->cards_layer, 0, 0);
    make_card(ctx, CARD_LEFT_X, true);
    make_card(ctx, CARD_RIGHT_X, false);

    /* 底部居中播放控制。 */
    lv_obj_t *shuffle = icon_button(screen, CX - 182, CONTROLS_Y, 56, MUSIC_ICON_SHUFFLE,
                                    COL_ORANGE, true, shuffle_cb, ctx);
    ctx->btn_shuffle = shuffle;
    ctx->shuffle_icon = lv_obj_get_child(shuffle, 0);
    ctx->btn_prev = icon_button(screen, CX - 98, CONTROLS_Y, 56, MUSIC_ICON_PREV, COL_TEXT,
                                true, prev_cb, ctx);
    lv_obj_t *play = lv_button_create(screen);
    ctx->btn_play = play;
    lv_obj_set_pos(play, CX - PLAY_SIZE / 2, PLAY_TOP);
    lv_obj_set_size(play, 76, 76);
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(0xF7F4FF), 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(play, 0, 0);
    lv_obj_set_style_shadow_width(play, 0, 0);
    lv_obj_set_style_pad_all(play, 0, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(0xDAD3FF), LV_STATE_PRESSED);
    lv_obj_add_event_cb(play, play_cb, LV_EVENT_CLICKED, ctx);
    pressable(play);
    ctx->play_icon = icon_image(play, MUSIC_ICON_PLAY, true, lv_color_hex(COL_BG));
    lv_obj_center(ctx->play_icon);

    ctx->btn_next = icon_button(screen, CX + 42, CONTROLS_Y, 56, MUSIC_ICON_NEXT, COL_TEXT,
                                true, next_cb, ctx);
    lv_obj_t *repeat = icon_button(screen, CX + 126, CONTROLS_Y, 56, MUSIC_ICON_REPEAT,
                                   COL_MUTED, true, repeat_cb, ctx);
    ctx->btn_repeat = repeat;
    ctx->repeat_icon = lv_obj_get_child(repeat, 0);

    /* 底栏：收藏、列表、音量、队列、下载。 */
    lv_obj_t *bottom_scrim = lv_obj_create(screen);
    lv_obj_remove_flag(bottom_scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(bottom_scrim, 0, 724);
    lv_obj_set_size(bottom_scrim, SCR_W, 76);
    lv_obj_set_style_bg_color(bottom_scrim, lv_color_hex(0x08080B), 0);
    lv_obj_set_style_bg_opa(bottom_scrim, 52, 0);
    lv_obj_set_style_border_width(bottom_scrim, 0, 0);
    lv_obj_set_style_radius(bottom_scrim, 0, 0);
    lv_obj_set_style_pad_all(bottom_scrim, 0, 0);
    ctx->btn_like = bottom_button(screen, 32, MUSIC_ICON_HEART_FILLED, "收藏", like_cb, ctx,
                                  &ctx->like_icon);
    ctx->btn_list = bottom_button(screen, 88, MUSIC_ICON_LIST, "列表",
                                  unavailable_list_cb, ctx, NULL);
    ctx->btn_queue = bottom_button(screen, SCR_W - 152, MUSIC_ICON_QUEUE, "队列",
                                   unavailable_queue_cb, ctx, NULL);
    ctx->btn_download = bottom_button(screen, SCR_W - 96, MUSIC_ICON_DOWNLOAD, "下载",
                                      unavailable_download_cb, ctx, NULL);

    ctx->volume_icon = icon_image(screen, MUSIC_ICON_VOLUME, false, lv_color_hex(COL_MUTED));
    lv_obj_set_pos(ctx->volume_icon, 500, 748);
    ctx->volume_slider = lv_slider_create(screen);
    lv_obj_set_pos(ctx->volume_slider, 532, 756);
    lv_obj_set_size(ctx->volume_slider, 190, 8);
    lv_slider_set_range(ctx->volume_slider, 0, 100);
    lv_slider_set_value(ctx->volume_slider, 20, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ctx->volume_slider, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->volume_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ctx->volume_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx->volume_slider, lv_color_hex(COL_PURPLE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ctx->volume_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ctx->volume_slider, lv_color_hex(COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(ctx->volume_slider, 5, LV_PART_KNOB);
    lv_obj_set_ext_click_area(ctx->volume_slider, 20);
    lv_obj_add_event_cb(ctx->volume_slider, volume_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);
    ctx->volume_value = small_label(screen, 728, 746, 56, "20%", &music_font_13, COL_MUTED);
    lv_obj_set_style_text_align(ctx->volume_value, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_add_event_cb(screen, ctx_delete_cb, LV_EVENT_DELETE, ctx);
    update_card_labels(ctx, 0);
    ctx->shown_track = 0;
    refresh_ui(ctx);
    ctx->status_timer = lv_timer_create(status_timer_cb, 1000, ctx);
    ctx->heartbeat_timer = lv_timer_create(heartbeat_timer_cb, MUSIC_UI_HEARTBEAT_MS, NULL);
#if ENABLE_DISC_SPIN
    ctx->disc_timer = lv_timer_create(disc_timer_cb, MUSIC_UI_DISC_PERIOD_MS, ctx);
#endif
    return screen;
}

void panel_music_ui_start(void)
{
    if (s_started) return;
    if (!panel_bsp_lock(2000)) {
        ESP_LOGE(TAG, "could not lock LVGL for music screen");
        return;
    }
    lv_obj_t *screen = build_page();
    if (screen == NULL) {
        panel_bsp_unlock();
        ESP_LOGE(TAG, "could not create music screen");
        return;
    }
    lv_obj_t *old = lv_screen_active();
    lv_screen_load(screen);
    if (old != NULL && old != screen) {
        lv_obj_delete(old);
    }
    s_started = true;
    s_ui_heartbeat_count = 0;
    s_ui_heartbeat_ms = lv_tick_get();
    panel_bsp_unlock();
    ESP_LOGI(TAG, "music screen started: %dx%d, disc_spin=%d period=%ums", SCR_W, SCR_H,
             s_disc_spin_enabled ? 1 : 0, MUSIC_UI_DISC_PERIOD_MS);
}

void panel_music_ui_set_disc_spin(bool enabled)
{
    s_disc_spin_enabled = enabled;
}

bool panel_music_ui_disc_spin_enabled(void)
{
    return s_disc_spin_enabled;
}

void panel_music_ui_heartbeat_snapshot(uint32_t *count, uint32_t *age_ms)
{
    uint32_t now = lv_tick_get();
    uint32_t last = s_ui_heartbeat_ms;
    if (count != NULL) *count = s_ui_heartbeat_count;
    if (age_ms != NULL) *age_ms = now - last;
}

#ifdef PANEL_MUSIC_UI_HOST_TEST
lv_obj_t *panel_music_ui_page_create(void)
{
    return build_page();
}

bool panel_music_ui_test_click(const char *action)
{
    if (s_ctx == NULL || action == NULL) return false;
    lv_obj_t *target = NULL;
    if (strcmp(action, "play") == 0) target = s_ctx->btn_play;
    else if (strcmp(action, "prev") == 0) target = s_ctx->btn_prev;
    else if (strcmp(action, "next") == 0) target = s_ctx->btn_next;
    else if (strcmp(action, "like") == 0) target = s_ctx->btn_like;
    else if (strcmp(action, "shuffle") == 0) target = s_ctx->btn_shuffle;
    else if (strcmp(action, "repeat") == 0) target = s_ctx->btn_repeat;
    else if (strcmp(action, "list") == 0) target = s_ctx->btn_list;
    else if (strcmp(action, "queue") == 0) target = s_ctx->btn_queue;
    else if (strcmp(action, "download") == 0) target = s_ctx->btn_download;
    if (target == NULL) return false;
    lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);
    return true;
}

const char *panel_music_ui_test_status_text(void)
{
    if (s_ctx == NULL || s_ctx->status_label == NULL) return "";
    return lv_label_get_text(s_ctx->status_label);
}

void panel_music_ui_test_refresh(void)
{
    refresh_ui(s_ctx);
}
#endif
