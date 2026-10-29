/*
 * Host continuous-frame evidence for the real panel_ui transition engine.
 * Each sequence writes start / middle / end PPM frames from the actual LVGL
 * pages, router and animations.  This is a software render, not a panel photo.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_pages.h"
#include "ui_theme.h"

#ifdef WAVESHARE_HOST_SIM
void panel_services_test_set_wifi_connected(bool connected);
void panel_services_test_set_time_calibrated(bool calibrated);
void panel_services_test_set_weather(const panel_weather_snapshot_t *snapshot);
void panel_services_test_set_clock(const panel_clock_snapshot_t *snapshot);
void panel_services_test_set_files(const panel_files_snapshot_t *snapshot);
#endif

#define WIDTH 1280
#define HEIGHT 800

static uint16_t framebuffer[WIDTH * HEIGHT];
static uint8_t draw_buffer[WIDTH * HEIGHT * 2] __attribute__((aligned(4)));
static const char *s_out_dir;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    const uint16_t *src = (const uint16_t *)px_map;
    for (int32_t y = 0; y < h; y++) {
        int32_t dy = area->y1 + y;
        if (dy < 0 || dy >= HEIGHT) {
            continue;
        }
        for (int32_t x = 0; x < w; x++) {
            int32_t dx = area->x1 + x;
            if (dx >= 0 && dx < WIDTH) {
                framebuffer[dy * WIDTH + dx] = src[y * w + x];
            }
        }
    }
    lv_display_flush_ready(disp);
}

static void advance_ms(uint32_t ms)
{
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
}

static bool wait_idle(uint32_t max_ms)
{
    for (uint32_t elapsed = 0; elapsed < max_ms; elapsed += 10) {
        if (ui_nav_idle()) {
            return true;
        }
        lv_tick_inc(10);
        lv_timer_handler();
    }
    return ui_nav_idle();
}

static void capture(const char *name)
{
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *top = lv_layer_top();
    if (root != NULL) {
        lv_obj_invalidate(root);
    }
    if (top != NULL) {
        lv_obj_invalidate(top);
    }
    lv_refr_now(NULL);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", s_out_dir, name);
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return;
    }
    fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        uint16_t c = framebuffer[i];
        uint8_t rgb[3];
        rgb[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        rgb[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
        rgb[2] = (uint8_t)((c & 0x1F) * 255 / 31);
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
    printf("captured %s\n", path);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: render_transition_frames <out_dir>\n");
        return 2;
    }
    s_out_dir = argv[1];

    lv_init();
    lv_tick_inc(1);
    lv_display_t *disp = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_default(disp);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, draw_buffer, NULL, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    panel_settings_t settings;
    panel_services_get_settings(&settings);
    settings.animations = true;
    settings.theme = PANEL_THEME_LIGHT;
    panel_services_update_settings(&settings);
    ui_start();
    advance_ms(20);

    ui_navigate(UI_PAGE_HOME);
    capture("00_unlock_start");
    advance_ms(120);
    capture("00_unlock_mid");
    advance_ms(180);
    wait_idle(1000);
    capture("00_unlock_end");

    ui_navigate(UI_PAGE_SETTINGS);
    capture("01_enter_start");
    advance_ms(100);
    capture("01_enter_mid");
    advance_ms(180);
    wait_idle(1000);
    capture("01_enter_end");

    ui_back();
    capture("02_back_start");
    advance_ms(100);
    capture("02_back_mid");
    advance_ms(160);
    wait_idle(1000);
    capture("02_back_end");

    ui_open_control();
    capture("03_control_open_start");
    advance_ms(100);
    capture("03_control_open_mid");
    advance_ms(160);
    wait_idle(1000);
    capture("03_control_open_end");

    ui_close_control();
    capture("04_control_close_start");
    advance_ms(100);
    capture("04_control_close_mid");
    advance_ms(160);
    wait_idle(1000);
    capture("04_control_close_end");

    ui_page_home_swipe(1);
    capture("05_home_page_start");
    advance_ms(100);
    capture("05_home_page_mid");
    advance_ms(160);
    wait_idle(1000);
    capture("05_home_page_end");

    ui_show_notice("测试弹窗", "动态测试", "返回应优先关闭弹窗，而不是离开当前页面。");
    capture("06_modal_start");
    advance_ms(70);
    capture("06_modal_mid");
    advance_ms(120);
    capture("06_modal_end");
    ui_back();
    wait_idle(1000);

    /* Fill the bounded MRU with all four implemented apps, then capture the
     * Overview with the fixed system bar anchored below it. */
    ui_navigate(UI_PAGE_SETTINGS);
    wait_idle(1000);
    ui_home();
    wait_idle(1000);
    ui_navigate(UI_PAGE_WEATHER);
    wait_idle(1000);
    ui_home();
    wait_idle(1000);
    ui_navigate(UI_PAGE_MONITOR);
    wait_idle(1000);
    ui_home();
    wait_idle(1000);
    ui_navigate(UI_PAGE_CALCULATOR);
    wait_idle(1000);
    ui_home();
    wait_idle(1000);

    ui_open_overview();
    capture("07_overview_light_start");
    advance_ms(100);
    capture("07_overview_light_mid");
    advance_ms(140);
    capture("07_overview_light_end");
    ui_close_overview();
    advance_ms(260);
    wait_idle(1000);

    panel_services_get_settings(&settings);
    settings.theme = PANEL_THEME_DARK;
    panel_services_update_settings(&settings);
    ui_theme_set(PANEL_THEME_DARK);
    ui_rebuild_current_async();
    advance_ms(300);
    wait_idle(1000);

    ui_open_overview();
    capture("08_overview_dark_start");
    advance_ms(100);
    capture("08_overview_dark_mid");
    advance_ms(140);
    capture("08_overview_dark_end");
    ui_close_overview();
    advance_ms(260);
    wait_idle(1000);

    panel_weather_snapshot_t weather;
    memset(&weather, 0, sizeof(weather));
    weather.has_data = true;
    weather.calibrated = true;
    weather.wifi_connected = true;
    weather.updated_epoch = 1792837200;
    weather.data_epoch = 1792836900;
    weather.current_temp_c = 26;
    weather.apparent_temp_c = 27;
    weather.humidity_pct = 62;
    weather.wind_speed_kmh = 12.3;
    weather.weather_code = 0;
    weather.kind = PANEL_WEATHER_CLEAR;
    weather.hour_count = 6;
    weather.day_count = 5;
    snprintf(weather.city, sizeof(weather.city), "广州");
    snprintf(weather.region, sizeof(weather.region), "广东");
    snprintf(weather.country, sizeof(weather.country), "中国");
    weather.latitude = 23.11667;
    weather.longitude = 113.25;
    for (int i = 0; i < 6; i++) {
        weather.hours[i].hour = 18 + i;
        weather.hours[i].temp_c = 24 + i;
        weather.hours[i].kind = PANEL_WEATHER_CLEAR;
    }
    for (int i = 0; i < 5; i++) {
        weather.days[i].day_offset = i;
        weather.days[i].weekday = (6 + i) % 7;
        weather.days[i].temp_max_c = 29 + i;
        weather.days[i].temp_min_c = 21 + i;
        weather.days[i].kind = PANEL_WEATHER_CLEAR;
    }
    panel_services_test_set_wifi_connected(true);
    panel_services_test_set_time_calibrated(true);
    panel_services_test_set_weather(&weather);
    ui_navigate(UI_PAGE_WEATHER);
    wait_idle(1000);
    advance_ms(200);
    capture("09_weather_online");
    ui_weather_panel_test_open();
    advance_ms(200);
    capture("10_weather_city_modal");
    ui_back();
    advance_ms(200);
    wait_idle(1000);

    ui_navigate(UI_PAGE_CALENDAR);
    wait_idle(1000);
    advance_ms(200);
    capture("11_calendar");

    panel_clock_snapshot_t clock;
    memset(&clock, 0, sizeof(clock));
    clock.valid = true;
    clock.calibrated = true;
    clock.year = 2026;
    clock.month = 10;
    clock.day = 24;
    clock.hour = 10;
    clock.minute = 24;
    clock.second = 5;
    clock.weekday = 5;
    clock.alarm_enabled = true;
    clock.alarm_hour = 7;
    clock.alarm_minute = 30;
    clock.stopwatch_ms = 1500;
    clock.countdown_remaining_ms = 300000;
    panel_services_test_set_clock(&clock);
    ui_navigate(UI_PAGE_CLOCK);
    wait_idle(1000);
    advance_ms(200);
    capture("12_clock");
    ui_navigate(UI_PAGE_HOME);
    wait_idle(1000);

    clock.alarm_pending = true;
    clock.alarm_generation = 77;
    panel_services_test_set_clock(&clock);
    advance_ms(450);
    capture("13_clock_reminder");
    ui_back();
    advance_ms(200);
    wait_idle(1000);

    panel_files_snapshot_t files;
    memset(&files, 0, sizeof(files));
    files.state = PANEL_FILES_STATE_NO_CARD;
    snprintf(files.error, sizeof(files.error), "未检测到可用TF卡");
    files.generation = 1;
    panel_services_test_set_files(&files);
    ui_navigate(UI_PAGE_FILES);
    wait_idle(1000);
    capture("14_files_no_card");

    memset(&files, 0, sizeof(files));
    files.state = PANEL_FILES_STATE_READY;
    snprintf(files.path, sizeof(files.path), "/");
    files.generation = 2;
    files.page_count = 1;
    files.total_entries = 3;
    files.count = 3;
    snprintf(files.entries[0].name, sizeof(files.entries[0].name), "文档");
    files.entries[0].is_dir = true;
    files.entries[0].valid_name = true;
    files.entries[0].valid_utf8 = true;
    snprintf(files.entries[1].name, sizeof(files.entries[1].name), "readme.txt");
    files.entries[1].valid_name = true;
    files.entries[1].valid_utf8 = true;
    files.entries[1].size = 42;
    snprintf(files.entries[2].name, sizeof(files.entries[2].name), "中文预览.txt");
    files.entries[2].valid_name = true;
    files.entries[2].valid_utf8 = true;
    files.entries[2].size = 18;
    panel_services_test_set_files(&files);
    advance_ms(300);
    capture("15_files_chinese_list");

    files.preview_open = true;
    snprintf(files.preview_name, sizeof(files.preview_name), "中文预览.txt");
    snprintf(files.preview, sizeof(files.preview), "中文文本预览\nBOM/CRLF 已处理\n");
    files.preview_len = strlen(files.preview);
    files.preview_size = 18;
    files.generation = 3;
    panel_services_test_set_files(&files);
    advance_ms(300);
    capture("16_files_chinese_preview");

    printf("PASS: continuous frames captured by real panel_ui\n");
    return 0;
}
