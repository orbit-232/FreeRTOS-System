#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "panel_music.h"
#include "panel_music_ui.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

void music_fake_init_from_env(void);
void music_fake_set_error_building(void);
void music_fake_set_stopped_building(void);

static uint8_t draw_buffer[1280 * 800 * 2] __attribute__((aligned(4)));

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

int main(void)
{
    setenv("WS_MUSIC_STATE", "playing", 1);
    setenv("WS_MUSIC_TRACK", "0", 1);
    setenv("WS_MUSIC_POS_MS", "72000", 1);
    music_fake_init_from_env();
    lv_init();
    lv_tick_inc(100);
    lv_display_t *disp = lv_display_create(1280, 800);
    lv_display_set_default(disp);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, draw_buffer, NULL, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_obj_t *screen = panel_music_ui_page_create();
    CHECK(screen != NULL);
    lv_screen_load(screen);
    lv_obj_update_layout(screen);

    panel_music_status_t st;
    panel_music_get_status(&st);
    CHECK(st.track == 0);
    CHECK(st.state == PANEL_MUSIC_PLAYING);
    CHECK(strcmp(st.title, "After Hours") == 0);
    CHECK(strcmp(st.artist, "The Weeknd") == 0);
    CHECK(st.duration_ms > 300000U);

    CHECK(panel_music_ui_test_click("like"));
    panel_music_get_status(&st);
    CHECK(st.liked);
    CHECK(panel_music_ui_test_click("play"));
    panel_music_get_status(&st);
    CHECK(st.state == PANEL_MUSIC_PAUSED);
    CHECK(panel_music_ui_test_click("play"));
    panel_music_get_status(&st);
    CHECK(st.state == PANEL_MUSIC_PLAYING);

    CHECK(panel_music_ui_test_click("next"));
    panel_music_get_status(&st);
    CHECK(st.track == 1U);
    CHECK(strcmp(st.title, "Starboy") == 0);
    CHECK(strcmp(st.artist, "The Weeknd, Daft Punk") == 0);
    CHECK(panel_music_ui_test_click("prev"));
    panel_music_get_status(&st);
    CHECK(st.track == 0U);

    CHECK(panel_music_ui_test_click("shuffle"));
    panel_music_get_status(&st);
    CHECK(st.shuffle);
    CHECK(panel_music_ui_test_click("repeat"));
    panel_music_get_status(&st);
    CHECK(st.repeat);
    CHECK(panel_music_ui_test_click("list"));
    CHECK(panel_music_ui_test_click("queue"));
    CHECK(panel_music_ui_test_click("download"));

    music_fake_set_error_building();
    panel_music_ui_test_refresh();
    const char *status_text = panel_music_ui_test_status_text();
    CHECK(strstr(status_text, "找不到音乐文件") != NULL);
    CHECK(strstr(status_text, "索引构建中") == NULL);
    printf("MUSIC_UI_ERROR_PRIORITY_OK status=%s\n", status_text);

    music_fake_set_stopped_building();
    panel_music_ui_test_refresh();
    status_text = panel_music_ui_test_status_text();
    CHECK(strstr(status_text, "索引构建中") == NULL);
    CHECK(strstr(status_text, "播放结束") != NULL || strstr(status_text, "未播放") != NULL ||
          strstr(status_text, "已停止") != NULL);
    printf("MUSIC_UI_STOPPED_PRIORITY_OK status=%s\n", status_text);

    printf("MUSIC_UI_HOST_TESTS_OK track=%u title=%s artist=%s liked=%d shuffle=%d repeat=%d\n",
           (unsigned)st.track, st.title, st.artist, st.liked ? 1 : 0, st.shuffle ? 1 : 0,
           st.repeat ? 1 : 0);
    return 0;
}
