#include "panel_music.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_audio.h"
#include "panel_files.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static int s_dummy;

void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
void *heap_caps_calloc(size_t n, size_t s, unsigned caps) { (void)caps; return calloc(n, s); }
void heap_caps_free(void *p) { free(p); }

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_dummy; }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
QueueHandle_t xQueueCreate(unsigned len, unsigned item) { (void)len; (void)item; return &s_dummy; }
int xQueueSend(QueueHandle_t q, const void *item, TickType_t t) { (void)q; (void)item; (void)t; return pdTRUE; }
int xQueueReceive(QueueHandle_t q, void *item, TickType_t t) { (void)q; (void)item; (void)t; return pdFALSE; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned prio,
                TaskHandle_t *out) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    if (out != NULL) *out = &s_dummy;
    return pdPASS;
}
void vTaskDelay(TickType_t t) { (void)t; }
void vTaskDelete(TaskHandle_t t) { (void)t; }
TickType_t xTaskGetTickCount(void) { return 0; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 4096; }

const char *panel_files_root_path(void) { return "/sdcard"; }
bool panel_files_card_ready(void) { return true; }
bool panel_files_io_acquire(void) { return true; }
void panel_files_io_release(void) {}
bool panel_files_mount_pin_acquire(void) { return true; }
void panel_files_mount_pin_release(void) {}
bool panel_files_mount_pin_held(void) { return false; }

bool panel_audio_stream_begin(void) { return true; }
bool panel_audio_stream_write(const int16_t *pcm, size_t frames) {
    (void)pcm; (void)frames; return true;
}
void panel_audio_stream_set_volume(int volume) { (void)volume; }
void panel_audio_stream_end(void) {}
bool panel_audio_stream_busy(void) { return false; }

bool panel_music_import_is_active(void) { return false; }

int main(void)
{
    static const char *titles[4] = {
        "After Hours", "Starboy", "I Feel It Coming", "Blinding Lights"
    };
    static const char *files[4] = {
        "/panel/music/after_hours.mp3", "/panel/music/starboy.mp3",
        "/panel/music/i_feel_it_coming.mp3", "/panel/music/blinding_lights.mp3"
    };
    CHECK(panel_music_init() == ESP_OK);
    for (uint8_t i = 0; i < 4; i++) {
        panel_music_select(i);
        panel_music_status_t st;
        panel_music_get_status(&st);
        CHECK(st.track == i);
        CHECK(strcmp(st.title, titles[i]) == 0);
        CHECK(strcmp(st.file, files[i]) == 0);
    }

    panel_music_select(2);
    CHECK(panel_music_play() == ESP_OK);
    CHECK(panel_music_test_active_track() == 2);
    panel_music_status_t st;
    panel_music_get_status(&st);
    CHECK(st.track == 2);
    CHECK(strcmp(st.file, files[2]) == 0);
    CHECK(strcmp(st.title, titles[2]) == 0);
    printf("MUSIC_SELECTION_STATE_OK active=%u title=%s file=%s\n",
           (unsigned)panel_music_test_active_track(), st.title, st.file);
    return 0;
}
