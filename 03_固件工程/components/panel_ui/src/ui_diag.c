#include "ui_diag.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "panel_bsp.h"
#include "panel_audio.h"
#include "panel_music.h"
#include "panel_music_import.h"
#include "panel_network.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_theme.h"
#include "ui_wallpaper_import.h"

static const char *TAG = "ui_diag";

#define DIAG_TARGETS 5
#define DIAG_TARGET_RADIUS 60
#define DIAG_HIT_TOLERANCE 20
#define DIAG_AUTO_EXIT_MS 60000
#define DIAG_LOG_PERIOD_MS 250

typedef struct {
    int x;
    int y;
    const char *name;
} diag_target_t;

static const diag_target_t s_targets[DIAG_TARGETS] = {
    {80, 80, "TL"},
    {UI_WIDTH - 80, 80, "TR"},
    {80, UI_HEIGHT - 80, "BL"},
    {UI_WIDTH - 80, UI_HEIGHT - 80, "BR"},
    {UI_WIDTH / 2, UI_HEIGHT / 2, "CENTER"},
};

static lv_obj_t *s_overlay;
static lv_obj_t *s_status;
static lv_obj_t *s_marker;
static lv_obj_t *s_target_ring[DIAG_TARGETS];
static lv_timer_t *s_timeout;
static bool s_hit[DIAG_TARGETS];
static int s_hit_count;
static uint32_t s_last_activity;
static uint32_t s_last_log;
static bool s_touch_log;
static bool s_active;

static void diag_close(bool by_timeout);

bool ui_diag_is_active(void)
{
    return s_active;
}

void ui_diag_set_touch_log(bool enabled)
{
    if (!panel_bsp_lock(1000)) {
        ESP_LOGW(TAG, "could not lock LVGL to change touch logging");
        return;
    }
    s_touch_log = enabled;
    panel_bsp_unlock();
    ESP_LOGI(TAG, "throttled touch log %s", enabled ? "on" : "off");
}

bool ui_diag_touch_log_enabled(void)
{
    return s_touch_log;
}

void ui_diag_note_touch(int x, int y, const char *phase)
{
    if (!s_touch_log && !s_active) {
        return;
    }
    uint32_t now = lv_tick_get();
    if (phase != NULL && strcmp(phase, "release") != 0 && now - s_last_log < DIAG_LOG_PERIOD_MS) {
        return;
    }
    s_last_log = now;

    int raw_x = -1;
    int raw_y = -1;
    int logical_x = -1;
    int logical_y = -1;
    bool raw_valid = false;
    panel_bsp_touch_get_last(&raw_x, &raw_y, &logical_x, &logical_y, &raw_valid);
    ESP_LOGI(TAG, "touch %s raw=(%d,%d)%s logical=(%d,%d) canvas=%dx%d%s",
             phase != NULL ? phase : "?", raw_x, raw_y, raw_valid ? "" : "[unavailable]",
             x, y, UI_WIDTH, UI_HEIGHT, s_active ? " [diag]" : "");
    (void)logical_x;
    (void)logical_y;
}

static const char *nearest_target(int x, int y, int *distance)
{
    const char *name = "NONE";
    int best = 0x7FFFFFFF;
    for (int i = 0; i < DIAG_TARGETS; i++) {
        int dx = x - s_targets[i].x;
        int dy = y - s_targets[i].y;
        int d = dx * dx + dy * dy;
        if (d < best) {
            best = d;
            name = s_targets[i].name;
        }
    }
    if (distance != NULL) {
        *distance = (int)(sqrtf((float)best) + 0.5f);
    }
    return name;
}

static void diag_mark_hit(int x, int y)
{
    for (int i = 0; i < DIAG_TARGETS; i++) {
        int dx = x - s_targets[i].x;
        int dy = y - s_targets[i].y;
        if (dx * dx + dy * dy <= DIAG_HIT_TOLERANCE * DIAG_HIT_TOLERANCE && !s_hit[i]) {
            s_hit[i] = true;
            s_hit_count++;
            if (s_target_ring[i] != NULL) {
                lv_obj_set_style_border_color(s_target_ring[i], lv_color_hex(0x42D392), 0);
                lv_obj_set_style_bg_opa(s_target_ring[i], LV_OPA_20, 0);
            }
            ESP_LOGI(TAG, "target %s hit (%d/%d)", s_targets[i].name, s_hit_count,
                     DIAG_TARGETS);
        }
    }
}

static void diag_update_status(int x, int y)
{
    if (s_status == NULL) {
        return;
    }
    int dist = 0;
    const char *target = nearest_target(x, y, &dist);
    int raw_x = -1;
    int raw_y = -1;
    int logical_x = -1;
    int logical_y = -1;
    bool raw_valid = false;
    panel_bsp_touch_get_last(&raw_x, &raw_y, &logical_x, &logical_y, &raw_valid);
    lv_label_set_text_fmt(s_status,
                          "命中 %d/%d   实测 raw(%d,%d) -> 逻辑(%d,%d)   最近 %s (%d px)",
                          s_hit_count, DIAG_TARGETS, raw_valid ? raw_x : -1,
                          raw_valid ? raw_y : -1, x, y, target, dist);
    if (s_marker != NULL) {
        lv_obj_clear_flag(s_marker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_marker, x - 12, y - 12);
    }
}

static void overlay_pressed_cb(lv_event_t *event)
{
    (void)event;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_event_code_t code = lv_event_get_code(event);
    s_last_activity = lv_tick_get();
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        diag_mark_hit(point.x, point.y);
        diag_update_status(point.x, point.y);
        ui_diag_note_touch(point.x, point.y, code == LV_EVENT_PRESSED ? "press" : "move");
    } else if (code == LV_EVENT_RELEASED) {
        diag_mark_hit(point.x, point.y);
        diag_update_status(point.x, point.y);
        ui_diag_note_touch(point.x, point.y, "release");
    }
}

static void exit_clicked_cb(lv_event_t *event)
{
    (void)event;
    diag_close(false);
}

static void timeout_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_active) {
        return;
    }
    if (lv_tick_get() - s_last_activity >= DIAG_AUTO_EXIT_MS) {
        ESP_LOGW(TAG, "no touch for %d ms, closing diagnostic automatically",
                 DIAG_AUTO_EXIT_MS);
        diag_close(true);
    }
}

static lv_obj_t *add_ring(lv_obj_t *parent, int x, int y, int size, lv_color_t color, int border)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(obj, size, size);
    lv_obj_set_pos(obj, x - size / 2, y - size / 2);
    lv_obj_set_style_radius(obj, size / 2, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_10, 0);
    lv_obj_set_style_border_color(obj, color, 0);
    lv_obj_set_style_border_width(obj, border, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static void diag_build(void)
{
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_size(s_overlay, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_add_event_cb(s_overlay, overlay_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_overlay, overlay_pressed_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_overlay, overlay_pressed_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, "触摸五点诊断 · 270° 横屏 1280×800");
    lv_obj_set_style_text_font(title, &ui_font_32, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_pos(title, 32, 18);

    lv_obj_t *hint = lv_label_create(s_overlay);
    lv_label_set_text(hint, "依次点按四角与中心；60 秒无触摸自动退出；串口发送 diag off 也可退出");
    lv_obj_set_style_text_font(hint, &ui_font_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xC9D4DF), 0);
    lv_obj_set_pos(hint, 32, 62);

    s_status = lv_label_create(s_overlay);
    lv_label_set_text(s_status, "命中 0/5   最后点 (--,--)");
    lv_obj_set_style_text_font(s_status, &ui_font_20, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x8FE3B0), 0);
    lv_obj_set_pos(s_status, 32, 590);

    /* Centre crosshair. */
    lv_obj_t *hline = lv_obj_create(s_overlay);
    lv_obj_remove_flag(hline, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(hline, 0, UI_HEIGHT / 2);
    lv_obj_set_size(hline, UI_WIDTH, 1);
    lv_obj_set_style_bg_color(hline, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(hline, LV_OPA_30, 0);
    lv_obj_set_style_border_width(hline, 0, 0);

    lv_obj_t *vline = lv_obj_create(s_overlay);
    lv_obj_remove_flag(vline, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(vline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(vline, UI_WIDTH / 2, 0);
    lv_obj_set_size(vline, 1, UI_HEIGHT);
    lv_obj_set_style_bg_color(vline, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(vline, LV_OPA_30, 0);
    lv_obj_set_style_border_width(vline, 0, 0);

    for (int i = 0; i < DIAG_TARGETS; i++) {
        s_target_ring[i] = add_ring(s_overlay, s_targets[i].x, s_targets[i].y,
                                    DIAG_TARGET_RADIUS * 2, lv_color_hex(0xF2C94C), 3);
        lv_obj_t *label = lv_label_create(s_target_ring[i]);
        lv_label_set_text(label, s_targets[i].name);
        lv_obj_set_style_text_font(label, &ui_font_20, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
    }

    s_marker = add_ring(s_overlay, 0, 0, 24, lv_color_hex(0xFF5C5C), 0);
    lv_obj_set_style_bg_opa(s_marker, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_marker, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *exit = lv_button_create(s_overlay);
    lv_obj_set_size(exit, 200, 64);
    lv_obj_align(exit, LV_ALIGN_TOP_RIGHT, -32, 24);
    lv_obj_set_style_radius(exit, 20, 0);
    lv_obj_set_style_bg_color(exit, lv_color_hex(0xD64545), 0);
    lv_obj_set_style_bg_opa(exit, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(exit, 0, 0);
    lv_obj_add_event_cb(exit, exit_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *exit_label = lv_label_create(exit);
    lv_label_set_text(exit_label, "退出诊断");
    lv_obj_set_style_text_font(exit_label, &ui_font_24, 0);
    lv_obj_set_style_text_color(exit_label, lv_color_white(), 0);
    lv_obj_center(exit_label);

    memset(s_hit, 0, sizeof(s_hit));
    s_hit_count = 0;
    s_last_activity = lv_tick_get();
    s_last_log = 0;
    s_timeout = lv_timer_create(timeout_timer_cb, 1000, NULL);
}

static void diag_destroy(void)
{
    if (s_timeout != NULL) {
        lv_timer_delete(s_timeout);
        s_timeout = NULL;
    }
    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }
    s_status = NULL;
    s_marker = NULL;
    memset(s_target_ring, 0, sizeof(s_target_ring));
}

static void diag_close(bool by_timeout)
{
    if (!s_active) {
        return;
    }
    diag_destroy();
    s_active = false;
    ui_system_bar_refresh();
    ESP_LOGI(TAG, "diagnostic closed (%s), hits=%d/%d", by_timeout ? "timeout" : "user",
             s_hit_count, DIAG_TARGETS);
}

static void set_active_async(void *arg)
{
    bool active = (bool)(uintptr_t)arg;
    if (active == s_active) {
        return;
    }
    if (active) {
        if (s_overlay != NULL) {
            return;
        }
        diag_build();
        s_active = true;
        ui_system_bar_refresh();
        ESP_LOGI(TAG, "diagnostic opened: tap corners and centre; auto-exit in %d s",
                 DIAG_AUTO_EXIT_MS / 1000);
    } else {
        diag_close(false);
    }
}

void ui_diag_set_active(bool active)
{
    /* May be called from the UART task: marshal to the LVGL task.  The adapter
     * lock is recursive, so this is also safe if a UI callback calls it. */
    if (panel_bsp_lock(1000)) {
        lv_async_call(set_active_async, (void *)(uintptr_t)active);
        panel_bsp_unlock();
    } else {
        ESP_LOGW(TAG, "could not lock LVGL to change diagnostic state");
    }
}

void ui_diag_toggle(void)
{
    if (panel_bsp_lock(1000)) {
        bool next = !s_active;
        lv_async_call(set_active_async, (void *)(uintptr_t)next);
        panel_bsp_unlock();
    }
}

static void log_net_status(void)
{
    panel_wifi_snapshot_t wifi;
    panel_ble_snapshot_t ble;
    panel_network_wifi_get(&wifi);
    panel_network_ble_get(&ble);
    ESP_LOGI(TAG, "NET wifi enabled=%d state=%d scan=%d connected=%d aps=%u ip=%s",
             wifi.enabled, (int)wifi.state, wifi.scanning, wifi.connected,
             wifi.ap_count, wifi.ip[0] ? wifi.ip : "-");
    ESP_LOGI(TAG, "NET ble enabled=%d state=%d scan=%d devices=%u services=%u",
             ble.enabled, (int)ble.state, ble.scanning, ble.device_count,
             ble.service_count);
}

static void log_files_status(void)
{
    panel_files_snapshot_t *snap =
        heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (snap == NULL) {
        ESP_LOGW(TAG, "filestatus=no_mem");
        return;
    }
    panel_services_get_files(snap);
    panel_assets_status_t asset;
    panel_services_get_assets(&asset);
    ESP_LOGI(TAG, "FILES state=%d gen=%u path=%s page=%u/%u count=%u total=%u trunc=%d "
                  "preview=%d preview_err=%d err=%s stack_free=%u",
             (int)snap->state, (unsigned)snap->generation, snap->path[0] ? snap->path : "/",
             snap->page + 1, snap->page_count, snap->count, snap->total_entries,
             snap->truncated, snap->preview_open, snap->preview_error,
             snap->error[0] ? snap->error : "-",
             (unsigned)panel_files_worker_stack_free());
    ESP_LOGI(TAG, "ASSET state=%d pack=%u file=%u ram=%u refs=%u hash=%d stack_free=%u "
                  "detail=%s",
             (int)asset.state, (unsigned)asset.pack_version, (unsigned)asset.font_file_bytes,
             (unsigned)asset.font_ram_bytes, asset.refs, asset.hash_checked,
             (unsigned)panel_assets_worker_stack_free(),
             asset.detail[0] ? asset.detail : "-");
    heap_caps_free(snap);
}

static void log_mem_status(void)
{
    const uint32_t intdma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    ESP_LOGI(TAG, "MEM internal_free=%u internal_min=%u internal_largest=%u "
                  "intdma_free=%u intdma_min=%u intdma_largest=%u "
                  "psram_free=%u psram_largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(intdma),
             (unsigned)heap_caps_get_minimum_free_size(intdma),
             (unsigned)heap_caps_get_largest_free_block(intdma),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static void log_clock_status(void){
    panel_clock_snapshot_t clock;
    panel_services_get_clock(&clock);
    ESP_LOGI(TAG, "CLOCK calibrated=%d y=%d mo=%d d=%d h=%d mi=%d s=%d "
                  "sw_running=%d sw_ms=%u cd_running=%d cd_remaining=%u cd_done=%d "
                  "cd_gen=%u alarm_enabled=%d alarm=%02u:%02u pending=%d persist_failed=%d",
             clock.calibrated, clock.year, clock.month, clock.day, clock.hour, clock.minute,
             clock.second, clock.stopwatch_running, (unsigned)clock.stopwatch_ms,
             clock.countdown_running, (unsigned)clock.countdown_remaining_ms,
             clock.countdown_done, (unsigned)clock.countdown_done_generation,
             clock.alarm_enabled, clock.alarm_hour, clock.alarm_minute, clock.alarm_pending,
             clock.alarm_persist_failed);
}

static void log_online_status(void)
{
    panel_weather_snapshot_t weather;
    panel_city_search_snapshot_t search;
    panel_services_get_weather(&weather);
    panel_services_get_city_search(&search);
    ESP_LOGI(TAG, "ONLINE wifi=%d calibrated=%d city=%s lat=%.5f lon=%.5f "
                  "has_data=%d stale=%d refreshing=%d temp=%d humidity=%d wind=%.1f "
                  "updated=%d data=%d reason=%s",
             weather.wifi_connected, weather.calibrated,
             weather.city[0] ? weather.city : "-", weather.latitude, weather.longitude,
             weather.has_data, weather.stale, weather.refreshing,
             weather.current_temp_c, weather.humidity_pct, weather.wind_speed_kmh,
             (int)weather.updated_epoch, (int)weather.data_epoch,
             weather.reason[0] ? weather.reason : "-");
    ESP_LOGI(TAG, "ONLINE search searching=%d count=%u error=%s",
             search.searching, search.count, search.error[0] ? search.error : "-");
}

static void console_task(void *arg)
{
    (void)arg;
    char line[64];
    size_t len = 0;
    setvbuf(stdin, NULL, _IONBF, 0);
    ESP_LOGI(TAG, "console ready: diag on|off, touchlog on|off, navstats, "
                  "navbench 1..3|stop, navdemo N|stop, netstatus, wifion, "
                  "wifiscan, wifioff, bleon, blescan, blestop, bleoff, online, "
                  "onlinerefresh, clockstatus, swstart, swpause, swreset, "
                  "cdstart N, cdpause, cdresume, cdreset, filestatus, filesretry, "
                  "mem, wallimport, audiotest, wavtest, audiostatus, musicplay, musicpause, musicstop, musicnext, musicprev, musicstatus, musicimport, help");

    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            line[len] = '\0';
            len = 0;
            if (strcmp(line, "help") == 0) {
                ESP_LOGI(TAG, "commands: diag on | diag off | diag | touchlog on | "
                              "touchlog off | navstats | navbench 1..3 | navbench stop | "
                              "navdemo N | navdemo stop | netstatus | wifion | wifiscan | "
                              "wifioff | bleon | blescan | blestop | bleoff | online | "
                              "onlinerefresh | clockstatus | swstart | swpause | swreset | "
                              "cdstart N | cdpause | cdresume | cdreset | filestatus | "
                              "filesretry | mem | wallimport | audiotest | wavtest | audiostatus | musicplay | musicpause | musicstop | musicnext | musicprev | musicstatus | musicimport start TOKEN | musicimport stop | musicimport status | help");
            } else if (strcmp(line, "netstatus") == 0) {
                log_net_status();
            } else if (strcmp(line, "online") == 0) {
                log_online_status();
            } else if (strcmp(line, "onlinerefresh") == 0) {
                ESP_LOGI(TAG, "onlinerefresh=%d", (int)panel_services_weather_refresh());
            } else if (strcmp(line, "clockstatus") == 0) {
                log_clock_status();
            } else if (strcmp(line, "filestatus") == 0) {
                log_files_status();
            } else if (strcmp(line, "mem") == 0) {
                log_mem_status();
            } else if (strcmp(line, "filesretry") == 0) {
                ESP_LOGI(TAG, "filesretry=%d", (int)panel_services_files_retry());
                ESP_LOGI(TAG, "assetretry=%d", (int)panel_services_assets_request_font());
            } else if (strcmp(line, "wallimport") == 0) {
                panel_wallpaper_import_run();
            } else if (strcmp(line, "audiotest") == 0) {
                ESP_LOGI(TAG, "audiotest=%d", (int)panel_audio_test_start());
            } else if (strcmp(line, "wavtest") == 0) {
                ESP_LOGI(TAG, "wavtest=%d", (int)panel_audio_test_wav_start());
            } else if (strcmp(line, "audiostatus") == 0) {
                panel_audio_status_t audio;
                panel_audio_get_status(&audio);
                ESP_LOGI(TAG, "AUDIO state=%d source=%u volume=%d duration_ms=%u frames=%u detail=%s",
                         (int)audio.state, (unsigned)audio.source, audio.volume,
                         (unsigned)audio.duration_ms, (unsigned)audio.frames, audio.detail);
            } else if (strcmp(line, "musicplay") == 0) {
                ESP_LOGI(TAG, "musicplay=%d", (int)panel_music_play());
            } else if (strcmp(line, "musicpause") == 0) {
                panel_music_pause();
            } else if (strcmp(line, "musicresume") == 0) {
                panel_music_resume();
            } else if (strcmp(line, "musicstop") == 0) {
                panel_music_stop();
            } else if (strcmp(line, "musicnext") == 0) {
                panel_music_next();
            } else if (strcmp(line, "musicprev") == 0) {
                panel_music_prev();
            } else if (strcmp(line, "musicstatus") == 0) {
                panel_music_status_t music;
                panel_music_get_status(&music);
                ESP_LOGI(TAG, "MUSIC state=%d track=%u volume=%d rate=%u ch=%u frames=%llu pos_ms=%u title=%s artist=%s detail=%s",
                         (int)music.state, (unsigned)music.track, music.volume,
                         (unsigned)music.sample_rate, (unsigned)music.channels,
                         (unsigned long long)music.frames, (unsigned)music.position_ms,
                         music.title, music.artist, music.detail);
            } else if (strncmp(line, "musicimport start ", 18) == 0) {
                ESP_LOGI(TAG, "musicimport=%d", (int)panel_music_import_start(line + 18));
            } else if (strcmp(line, "musicimport stop") == 0) {
                panel_music_import_stop();
                ESP_LOGI(TAG, "musicimport stopped");
            } else if (strcmp(line, "musicimport status") == 0) {
                ESP_LOGI(TAG, "musicimport active=%d", panel_music_import_is_active() ? 1 : 0);
            } else if (strcmp(line, "swstart") == 0) {
                ESP_LOGI(TAG, "swstart=%d", (int)panel_services_clock_stopwatch_start());
            } else if (strcmp(line, "swpause") == 0) {
                ESP_LOGI(TAG, "swpause=%d", (int)panel_services_clock_stopwatch_pause());
            } else if (strcmp(line, "swreset") == 0) {
                ESP_LOGI(TAG, "swreset=%d", (int)panel_services_clock_stopwatch_reset());
            } else if (strncmp(line, "cdstart", 7) == 0) {
                const char *arg = line + 7;
                while (*arg == ' ') arg++;
                char *end = NULL;
                errno = 0;
                unsigned long seconds = strtoul(arg, &end, 10);
                if (arg[0] < '0' || arg[0] > '9' || errno != 0 || end == arg ||
                    *end != '\0' || seconds < 1 ||
                    seconds > PANEL_CLOCK_COUNTDOWN_MAX_SECONDS) {
                    ESP_LOGW(TAG, "cdstart=invalid (expected 1..%u)",
                             (unsigned)PANEL_CLOCK_COUNTDOWN_MAX_SECONDS);
                } else {
                    ESP_LOGI(TAG, "cdstart=%d seconds=%u",
                             (int)panel_services_clock_countdown_start((uint32_t)seconds),
                             (unsigned)seconds);
                }
            } else if (strcmp(line, "cdpause") == 0) {
                ESP_LOGI(TAG, "cdpause=%d", (int)panel_services_clock_countdown_pause());
            } else if (strcmp(line, "cdresume") == 0) {
                ESP_LOGI(TAG, "cdresume=%d", (int)panel_services_clock_countdown_resume());
            } else if (strcmp(line, "cdreset") == 0) {
                ESP_LOGI(TAG, "cdreset=%d", (int)panel_services_clock_countdown_reset());
            } else if (strcmp(line, "wifion") == 0) {
                ESP_LOGI(TAG, "wifion=%d", (int)panel_network_wifi_set_enabled(true));
            } else if (strcmp(line, "wifiscan") == 0) {
                ESP_LOGI(TAG, "wifiscan=%d", (int)panel_network_wifi_scan());
            } else if (strcmp(line, "wifioff") == 0) {
                ESP_LOGI(TAG, "wifioff=%d", (int)panel_network_wifi_set_enabled(false));
            } else if (strcmp(line, "bleon") == 0) {
                ESP_LOGI(TAG, "bleon=%d", (int)panel_network_ble_set_enabled(true));
            } else if (strcmp(line, "blescan") == 0) {
                ESP_LOGI(TAG, "blescan=%d", (int)panel_network_ble_scan());
            } else if (strcmp(line, "blestop") == 0) {
                ESP_LOGI(TAG, "blestop=%d", (int)panel_network_ble_stop());
            } else if (strcmp(line, "bleoff") == 0) {
                ESP_LOGI(TAG, "bleoff=%d", (int)panel_network_ble_set_enabled(false));
            } else if (strcmp(line, "diag on") == 0) {
                ui_diag_set_active(true);
            } else if (strcmp(line, "diag off") == 0) {
                ui_diag_set_active(false);
            } else if (strcmp(line, "diag") == 0) {
                ui_diag_toggle();
            } else if (strcmp(line, "touchlog on") == 0) {
                ui_diag_set_touch_log(true);
            } else if (strcmp(line, "touchlog off") == 0) {
                ui_diag_set_touch_log(false);
            } else if (strcmp(line, "navstats") == 0) {
                ui_nav_log_stats();
            } else if (strcmp(line, "navbench stop") == 0) {
                ui_nav_bench_stop();
                ESP_LOGI(TAG, "navigation benchmark stop requested");
            } else if (strncmp(line, "navbench", 8) == 0) {
                unsigned cycles = 3;
                if (line[8] == ' ') {
                    int parsed = sscanf(line + 9, "%u", &cycles);
                    if (parsed != 1) {
                        ESP_LOGW(TAG, "usage: navbench 1..3 | navbench stop");
                        continue;
                    }
                }
                if (cycles < 1) {
                    cycles = 1;
                }
                if (cycles > 3) {
                    cycles = 3;
                }
                if (ui_nav_bench_start(cycles)) {
                    ESP_LOGI(TAG, "navigation benchmark request accepted: cycles=%u", cycles);
                } else {
                    ESP_LOGW(TAG, "navigation benchmark request failed");
                }
            } else if (strcmp(line, "navdemo stop") == 0) {
                ui_nav_demo_stop();
                ESP_LOGI(TAG, "navigation demo stop requested");
            } else if (strncmp(line, "navdemo", 7) == 0) {
                unsigned cycles = 1;
                if (line[7] == ' ') {
                    int parsed = sscanf(line + 8, "%u", &cycles);
                    if (parsed != 1) {
                        ESP_LOGW(TAG, "usage: navdemo 1..10 | navdemo stop");
                        continue;
                    }
                }
                if (cycles < 1) {
                    cycles = 1;
                }
                if (cycles > 10) {
                    cycles = 10;
                }
                if (ui_nav_demo_start(cycles)) {
                    ESP_LOGI(TAG, "navigation demo request accepted: cycles=%u", cycles);
                } else {
                    ESP_LOGW(TAG, "navigation demo request failed");
                }
            } else if (line[0] != '\0') {
                ESP_LOGW(TAG, "unknown command '%s' (send help)", line);
            }
            continue;
        }
        if (ch == '\b' || ch == 0x7F) {
            if (len > 0) {
                len--;
            }
            continue;
        }
        if (len + 1 < sizeof(line)) {
            line[len++] = (char)ch;
        }
    }
}

void ui_diag_start_console(void)
{
    BaseType_t ok = xTaskCreate(console_task, "ui_diag_uart", 8192, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "could not start diag console task");
    }
}
