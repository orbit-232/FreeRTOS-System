/* Host-only board and diagnostic stubs for the dynamic navigation harness.
 * The real panel_bsp / ui_diag code is ESP-IDF-only and is not linked here. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "lvgl.h"
#include "panel_bsp.h"
#include "panel_audio.h"
#include "panel_music.h"
#include "panel_wallpaper.h"
#include "ui_diag.h"

static int s_brightness = 70;
static bool s_diag_active;
static bool s_touch_log;
static bool s_test_boot_press;

void panel_bsp_test_set_boot_press(bool pressed)
{
    s_test_boot_press = pressed;
}

bool panel_bsp_boot_key_take_press(void)
{
    bool pressed = s_test_boot_press;
    s_test_boot_press = false;
    return pressed;
}

esp_err_t panel_bsp_init(void)
{
    return ESP_OK;
}

lv_display_t *panel_bsp_display(void)
{
    return lv_display_get_default();
}

lv_indev_t *panel_bsp_input(void)
{
    return NULL;
}

esp_err_t panel_wallpaper_init(void)
{
    return ESP_OK;
}

const panel_wallpaper_t *panel_wallpaper_get(void)
{
    return NULL;
}

esp_err_t panel_wallpaper_load_file(const char *path)
{
    (void)path;
    return ESP_ERR_NOT_SUPPORTED;
}

void panel_wallpaper_request(void)
{
}

void panel_wallpaper_release(void)
{
}

esp_err_t panel_audio_test_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t panel_audio_test_wav_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void panel_audio_get_status(panel_audio_status_t *out)
{
    if (out == NULL) return;
    out->state = PANEL_AUDIO_IDLE;
    out->source = 0;
    out->volume = 20;
    out->duration_ms = 800;
    out->frames = 0;
    out->detail[0] = '\0';
}

bool panel_bsp_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;
}

void panel_bsp_unlock(void)
{
}

esp_err_t panel_bsp_set_brightness(int percent)
{
    s_brightness = percent;
    return ESP_OK;
}

int panel_bsp_get_brightness(void)
{
    return s_brightness;
}

esp_err_t panel_bsp_backlight_on(void)
{
    return ESP_OK;
}

esp_err_t panel_bsp_backlight_off(void)
{
    return ESP_OK;
}

void panel_bsp_touch_get_last(int *raw_x, int *raw_y, int *logical_x, int *logical_y,
                              bool *valid)
{
    if (raw_x != NULL) {
        *raw_x = -1;
    }
    if (raw_y != NULL) {
        *raw_y = -1;
    }
    if (logical_x != NULL) {
        *logical_x = -1;
    }
    if (logical_y != NULL) {
        *logical_y = -1;
    }
    if (valid != NULL) {
        *valid = false;
    }
}

uint32_t panel_bsp_touch_bad_raw_count(void)
{
    return 0;
}

bool ui_diag_is_active(void)
{
    return s_diag_active;
}

void ui_diag_set_active(bool active)
{
    s_diag_active = active;
}

void ui_diag_toggle(void)
{
    s_diag_active = !s_diag_active;
}

void ui_diag_set_touch_log(bool enabled)
{
    s_touch_log = enabled;
}

bool ui_diag_touch_log_enabled(void)
{
    return s_touch_log;
}

void ui_diag_note_touch(int x, int y, const char *phase)
{
    (void)x;
    (void)y;
    (void)phase;
}

void ui_diag_start_console(void)
{
}
static unsigned s_stub_music_track;
static const char *const s_stub_titles[] = {
    "After Hours", "Starboy", "I Feel It Coming", "Blinding Lights"
};
static const char *const s_stub_artists[] = {
    "The Weeknd", "The Weeknd, Daft Punk", "The Weeknd, Daft Punk", "The Weeknd"
};
void panel_music_toggle(void) {}
void panel_music_stop(void) {}
void panel_music_next(void) { s_stub_music_track = (s_stub_music_track + 1U) % 4U; }
void panel_music_prev(void) { s_stub_music_track = (s_stub_music_track + 3U) % 4U; }
void panel_music_select(uint8_t track) { if (track < 4U) s_stub_music_track = track; }
void panel_music_set_volume(int volume) { (void)volume; }
void panel_music_get_status(panel_music_status_t *out)
{
    if (out == NULL) return;
    out->state = PANEL_MUSIC_IDLE;
    out->track = (uint8_t)s_stub_music_track;
    out->volume = 20;
    snprintf(out->title, sizeof(out->title), "%s", s_stub_titles[s_stub_music_track]);
    snprintf(out->artist, sizeof(out->artist), "%s", s_stub_artists[s_stub_music_track]);
    snprintf(out->detail, sizeof(out->detail), "%s", "未播放");
}