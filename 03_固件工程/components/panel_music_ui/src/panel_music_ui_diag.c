#include "panel_music_ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel_music.h"

static const char *TAG = "music_diag";
static bool s_console_started;

static void log_status(void)
{
    panel_music_status_t st;
    panel_music_get_status(&st);
    ESP_LOGI(TAG,
             "MUSIC state=%d track=%u title=%s artist=%s pos_ms=%u duration_ms=%u "
             "volume=%d liked=%d shuffle=%d repeat=%d seek_pending=%d rate=%u ch=%u "
             "frames=%llu detail=%s",
             (int)st.state, (unsigned)st.track + 1U, st.title, st.artist,
             (unsigned)st.position_ms, (unsigned)st.duration_ms, st.volume,
             st.liked ? 1 : 0, st.shuffle ? 1 : 0, st.repeat ? 1 : 0,
             st.seek_pending ? 1 : 0, (unsigned)st.sample_rate, (unsigned)st.channels,
             (unsigned long long)st.frames, st.detail);
}

static void log_mem(void)
{
    ESP_LOGI(TAG,
             "MEM internal_free=%u internal_min=%u dma_free=%u dma_largest=%u "
             "psram_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void console_task(void *arg)
{
    (void)arg;
    char line[96];
    size_t len = 0;
    setvbuf(stdin, NULL, _IONBF, 0);
    ESP_LOGI(TAG, "console ready: musicstatus, musicplay, musicpause, musicresume, "
                  "musicstop, musicnext, musicprev, seek MS, like, shuffle on|off, "
                  "repeat on|off, volume 0..100, uiheartbeat, mem, help");
    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (ch != '\r' && ch != '\n') {
            if (len + 1U < sizeof(line)) line[len++] = (char)ch;
            continue;
        }
        line[len] = '\0';
        len = 0;
        if (strcmp(line, "help") == 0) {
            ESP_LOGI(TAG, "musicstatus | musicplay | musicpause | musicresume | musicstop | "
                          "musicnext | musicprev | seek MS | like | shuffle on|off | "
                          "repeat on|off | volume 0..100 | uiheartbeat | mem");
        } else if (strcmp(line, "musicstatus") == 0) {
            log_status();
        } else if (strcmp(line, "uiheartbeat") == 0) {
            uint32_t count = 0;
            uint32_t age = 0;
            panel_music_ui_heartbeat_snapshot(&count, &age);
            ESP_LOGI(TAG, "UI_HEARTBEAT count=%u age_ms=%u disc_spin=%d",
                     (unsigned)count, (unsigned)age,
                     panel_music_ui_disc_spin_enabled() ? 1 : 0);
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
        } else if (strncmp(line, "seek ", 5) == 0) {
            char *end = NULL;
            errno = 0;
            unsigned long ms = strtoul(line + 5, &end, 10);
            if (end == line + 5 || *end != '\0' || errno != 0) {
                ESP_LOGW(TAG, "seek=invalid (expected seek MS)");
            } else {
                ESP_LOGI(TAG, "seek=%d target_ms=%lu",
                         (int)panel_music_seek((uint32_t)ms), ms);
            }
        } else if (strcmp(line, "like") == 0) {
            ESP_LOGI(TAG, "like=%d", panel_music_toggle_like() ? 1 : 0);
        } else if (strcmp(line, "shuffle on") == 0) {
            ESP_LOGI(TAG, "shuffle=%d", panel_music_set_shuffle(true) ? 1 : 0);
        } else if (strcmp(line, "shuffle off") == 0) {
            ESP_LOGI(TAG, "shuffle=%d", panel_music_set_shuffle(false) ? 1 : 0);
        } else if (strcmp(line, "repeat on") == 0) {
            ESP_LOGI(TAG, "repeat=%d", panel_music_set_repeat(true) ? 1 : 0);
        } else if (strcmp(line, "repeat off") == 0) {
            ESP_LOGI(TAG, "repeat=%d", panel_music_set_repeat(false) ? 1 : 0);
        } else if (strncmp(line, "volume ", 7) == 0) {
            char *end = NULL;
            long volume = strtol(line + 7, &end, 10);
            if (end == line + 7 || *end != '\0' || volume < 0 || volume > 100) {
                ESP_LOGW(TAG, "volume=invalid (expected 0..100)");
            } else {
                panel_music_set_volume((int)volume);
                ESP_LOGI(TAG, "volume=%ld", volume);
            }
        } else if (strcmp(line, "mem") == 0) {
            log_mem();
        } else if (line[0] != '\0') {
            ESP_LOGW(TAG, "unknown command: %s", line);
        }
    }
}

void panel_music_ui_start_console(void)
{
    if (s_console_started) return;
    if (xTaskCreate(console_task, "music_diag", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
        return;
    }
    s_console_started = true;
}
