#include "panel_music.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "minimp3.h"
#include "panel_audio.h"
#include "panel_files.h"
#include "panel_music_import.h"
#include "panel_music_resample.h"
#include "panel_music_tracks.h"

#define TAG "panel_music"
#define MUSIC_DIR "/panel/music"
/* The decoder's 16 KiB scratch is allocated once in PSRAM by panel_music_init.
 * Target -fstack-usage: worker=1504 B and decode_with_scratch=1136 B; the old
 * full-track highwater was 19.5 KiB, which includes the 16.2 KiB scratch.
 * 12 KiB therefore leaves about 9 KiB of measured margin. */
#define MUSIC_STACK 12288U
#define MUSIC_IN_BUF (32U * 1024U)
#define MUSIC_OUT_HZ PANEL_MUSIC_OUT_HZ
#define MUSIC_PAUSE_LIMIT_MS 30000U

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static mp3dec_scratch_t *s_scratch;
static panel_music_status_t s_status;
static volatile bool s_stop;
static volatile bool s_pause;
static TickType_t s_pause_since;
static portMUX_TYPE s_flag_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;
static uint8_t s_selected_track;
static uint8_t s_active_track;
static bool s_auto_play;

static const char *state_name(panel_music_state_t state)
{
    switch (state) {
    case PANEL_MUSIC_LOADING: return "LOADING";
    case PANEL_MUSIC_PLAYING: return "PLAYING";
    case PANEL_MUSIC_PAUSED: return "PAUSED";
    case PANEL_MUSIC_STOPPING: return "STOPPING";
    case PANEL_MUSIC_STOPPED: return "STOPPED";
    case PANEL_MUSIC_ERROR: return "ERROR";
    default: return "IDLE";
    }
}

static bool stop_requested(void)
{
    taskENTER_CRITICAL(&s_flag_mux);
    bool v = s_stop;
    taskEXIT_CRITICAL(&s_flag_mux);
    return v;
}

static void set_stop(bool stop)
{
    taskENTER_CRITICAL(&s_flag_mux);
    s_stop = stop;
    taskEXIT_CRITICAL(&s_flag_mux);
}

static bool pause_requested(void)
{
    taskENTER_CRITICAL(&s_flag_mux);
    bool v = s_pause;
    taskEXIT_CRITICAL(&s_flag_mux);
    return v;
}

static void set_pause(bool pause)
{
    taskENTER_CRITICAL(&s_flag_mux);
    s_pause = pause;
    if (pause) s_pause_since = xTaskGetTickCount();
    taskEXIT_CRITICAL(&s_flag_mux);
}

static uint32_t pause_elapsed_ms(void)
{
    taskENTER_CRITICAL(&s_flag_mux);
    uint32_t ms = (uint32_t)((xTaskGetTickCount() - s_pause_since) * portTICK_PERIOD_MS);
    taskEXIT_CRITICAL(&s_flag_mux);
    return ms;
}

static panel_music_state_t current_state(void)
{
    panel_music_state_t state = PANEL_MUSIC_IDLE;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        state = s_status.state;
        xSemaphoreGive(s_lock);
    }
    return state;
}

static void set_status(panel_music_state_t state, const char *detail)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    if (detail != NULL) snprintf(s_status.detail, sizeof(s_status.detail), "%s", detail);
    xSemaphoreGive(s_lock);
}

static bool make_path(uint8_t track, char *out, size_t cap)
{
    const panel_music_track_t *meta = panel_music_track_at(track);
    const char *root = panel_files_root_path();
    if (meta == NULL || root == NULL || root[0] == '\0') return false;
    int n = snprintf(out, cap, "%s%s/%s", root, MUSIC_DIR, meta->file);
    return n > 0 && (size_t)n < cap;
}

static bool file_open(FILE **out, const char *path, bool *id3, uint32_t *tag_bytes)
{
    *out = NULL;
    *id3 = false;
    *tag_bytes = 0;
    if (!panel_files_card_ready()) {
        set_status(PANEL_MUSIC_ERROR, "无TF卡");
        return false;
    }
    if (!panel_files_mount_pin_acquire()) {
        set_status(PANEL_MUSIC_ERROR, "TF卡正在重试，稍后再试");
        return false;
    }
    if (!panel_files_io_acquire()) {
        panel_files_mount_pin_release();
        set_status(PANEL_MUSIC_ERROR, "TF卡忙");
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        panel_files_io_release();
        panel_files_mount_pin_release();
        set_status(PANEL_MUSIC_ERROR, "找不到音乐文件");
        return false;
    }
    uint8_t h[10];
    size_t n = fread(h, 1, sizeof(h), f);
    if (n == sizeof(h) && memcmp(h, "ID3", 3) == 0) {
        uint32_t size = ((uint32_t)(h[6] & 0x7f) << 21) | ((uint32_t)(h[7] & 0x7f) << 14) |
                        ((uint32_t)(h[8] & 0x7f) << 7) | (uint32_t)(h[9] & 0x7f);
        uint32_t total = 10U + size + ((h[5] & 0x10) ? 10U : 0U);
        if (total > 4U * 1024U * 1024U || fseek(f, (long)total, SEEK_SET) != 0) {
            fclose(f);
            panel_files_io_release();
            panel_files_mount_pin_release();
            set_status(PANEL_MUSIC_ERROR, "ID3标签异常");
            return false;
        }
        *id3 = true;
        *tag_bytes = total;
    } else {
        rewind(f);
    }
    panel_files_io_release();
    *out = f;
    return true;
}

static size_t read_stream(FILE *f, uint8_t *buf, size_t cap, bool *eof)
{
    if (!panel_files_card_ready()) return 0;
    if (!panel_files_io_acquire()) return 0;
    size_t n = fread(buf, 1, cap, f);
    bool err = ferror(f) != 0;
    panel_files_io_release();
    if (n == 0) {
        if (err) return 0;
        *eof = true;
    }
    return n;
}

static bool pcm_sink(void *ctx, const int16_t *pcm, size_t frames)
{
    (void)ctx;
    return panel_audio_stream_write(pcm, frames);
}

static bool take_auto_restart(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool restart = s_auto_play;
    if (restart) {
        s_auto_play = false;
        set_stop(false);
        set_pause(false);
        s_active_track = s_selected_track;
        s_status.state = PANEL_MUSIC_LOADING;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "切换曲目");
    }
    xSemaphoreGive(s_lock);
    return restart;
}

static bool queue_play(void)
{
    uint8_t command = 1;
    return xQueueSend(s_queue, &command, 0) == pdTRUE;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t command;
        if (xQueueReceive(s_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        if (command != 1) continue;
        if (stop_requested()) {
            if (take_auto_restart()) {
                if (!queue_play()) set_status(PANEL_MUSIC_ERROR, "切换失败");
            } else {
                set_status(PANEL_MUSIC_STOPPED, "已停止");
            }
            continue;
        }
        set_status(PANEL_MUSIC_LOADING, "打开TF音乐");

        uint8_t track;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        track = s_active_track;
        xSemaphoreGive(s_lock);
        char path[224];
        const panel_music_track_t *opened = panel_music_track_at(track);
        if (opened == NULL || !make_path(track, path, sizeof(path))) {
            set_status(PANEL_MUSIC_ERROR, "路径无效");
            continue;
        }
        ESP_LOGI(TAG, "opening track=%u id=%s file=%s", (unsigned)track, opened->id,
                 opened->file);
        FILE *f = NULL;
        bool id3 = false;
        uint32_t tag_bytes = 0;
        if (!file_open(&f, path, &id3, &tag_bytes)) {
            continue;
        }
        mp3dec_t *dec = heap_caps_calloc(1, sizeof(*dec), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *in = heap_caps_malloc(MUSIC_IN_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (dec == NULL || in == NULL || pcm == NULL) {
            set_status(PANEL_MUSIC_ERROR, "内存不足");
            goto cleanup;
        }
        mp3dec_init(dec);
        uint32_t hz = 0;
        uint16_t channels = 0;
        uint64_t decoded_frames = 0;
        size_t used = 0, pos = 0;
        bool eof = false;
        bool stream = false;
        panel_music_resampler_t resampler = {0};
        bool resampler_ready = false;
        bool ok = true;
        while (!stop_requested()) {
            while (pause_requested() && !stop_requested()) {
                if (pause_elapsed_ms() > MUSIC_PAUSE_LIMIT_MS) {
                    set_stop(true);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (stop_requested()) break;
            if (pos > 0 && pos < used) {
                memmove(in, in + pos, used - pos);
                used -= pos;
                pos = 0;
            } else if (pos == used) {
                used = 0;
                pos = 0;
            }
            if (!eof && used < MUSIC_IN_BUF) {
                size_t n = read_stream(f, in + used, MUSIC_IN_BUF - used, &eof);
                used += n;
            }
            if (used == 0 && eof) break;
            if (used == 0) {
                ok = false;
                break;
            }
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame_with_scratch(dec, s_scratch, in + pos,
                                                           (int)(used - pos), pcm, &info);
            if (info.frame_bytes <= 0) {
                if (eof) break;
                if (used == MUSIC_IN_BUF) {
                    ok = false;
                    break;
                }
                continue;
            }
            if ((size_t)info.frame_bytes > used - pos) {
                if (eof || used == MUSIC_IN_BUF) {
                    ok = false;
                    break;
                }
                continue;
            }
            pos += (size_t)info.frame_bytes;
            if (samples <= 0) continue;
            if (!stream) {
                if (info.hz < 8000 || info.hz > 48000 ||
                    (info.channels != 1 && info.channels != 2)) {
                    set_status(PANEL_MUSIC_ERROR, "MP3格式不支持");
                    ok = false;
                    break;
                }
                hz = (uint32_t)info.hz;
                channels = (uint16_t)info.channels;
                panel_music_resampler_init(&resampler, hz, channels);
                resampler_ready = true;
                if (!panel_audio_stream_begin()) {
                    set_status(PANEL_MUSIC_ERROR, "音频输出忙");
                    ok = false;
                    break;
                }
                stream = true;
                int volume = 20;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                volume = s_status.volume;
                xSemaphoreGive(s_lock);
                panel_audio_stream_set_volume(volume);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.sample_rate = hz;
                s_status.channels = channels;
                xSemaphoreGive(s_lock);
                set_status(PANEL_MUSIC_PLAYING, id3 ? "播放中（ID3已跳过）" : "播放中");
                ESP_LOGI(TAG, "MP3 stream: hz=%u ch=%u id3=%d tag=%u",
                         (unsigned)hz, (unsigned)channels, id3 ? 1 : 0, (unsigned)tag_bytes);
            }
            if (info.hz != (int)hz || info.channels != (int)channels) {
                set_status(PANEL_MUSIC_ERROR, "MP3参数变化");
                ok = false;
                break;
            }
            int frames = samples;
            if (!resampler_ready ||
                !panel_music_resampler_process(&resampler, pcm, frames, pcm_sink, NULL)) {
                set_status(PANEL_MUSIC_ERROR, "I2S写入失败");
                ok = false;
                break;
            }
            decoded_frames += (uint64_t)frames;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.frames = decoded_frames;
            s_status.position_ms = (uint32_t)(resampler.out_frames * 1000U / MUSIC_OUT_HZ);
            xSemaphoreGive(s_lock);
        }
        if (ok && !stop_requested() && resampler_ready) {
            if (!panel_music_resampler_flush(&resampler, pcm_sink, NULL)) ok = false;
        }
        if (stream) panel_audio_stream_end();
        if (stop_requested()) {
            set_status(PANEL_MUSIC_STOPPED, "已停止");
        } else if (ok) {
            set_status(PANEL_MUSIC_STOPPED, "播放结束");
            ESP_LOGI(TAG, "MP3 end: frames=%llu out=%llu",
                     (unsigned long long)decoded_frames,
                     (unsigned long long)resampler.out_frames);
        } else if (current_state() != PANEL_MUSIC_ERROR) {
            set_status(PANEL_MUSIC_ERROR, "解码失败");
        }
cleanup:
        heap_caps_free(pcm);
        heap_caps_free(in);
        heap_caps_free(dec);
        if (f != NULL) {
            if (panel_files_io_acquire()) {
                fclose(f);
                panel_files_io_release();
            } else {
                fclose(f);
            }
            panel_files_mount_pin_release();
        }
        ESP_LOGI(TAG, "music stack total=%u free=%u scratch=%u intdma_free=%u intdma_largest=%u",
                 (unsigned)MUSIC_STACK, (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)mp3dec_scratch_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        if (take_auto_restart() && !queue_play()) {
            set_status(PANEL_MUSIC_ERROR, "切换失败");
        }
    }
}

esp_err_t panel_music_init(void)
{
    if (s_started) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(1, sizeof(uint8_t));
    if (s_lock == NULL || s_queue == NULL) return ESP_ERR_NO_MEM;
    s_status.state = PANEL_MUSIC_IDLE;
    s_status.track = 0;
    s_selected_track = 0;
    s_status.volume = 20;
    const panel_music_track_t *meta = panel_music_track_at(0);
    snprintf(s_status.title, sizeof(s_status.title), "%s", meta->title);
    snprintf(s_status.artist, sizeof(s_status.artist), "%s", meta->artist);
    snprintf(s_status.file, sizeof(s_status.file), "%s/%s", MUSIC_DIR, meta->file);
    snprintf(s_status.detail, sizeof(s_status.detail), "%s", "未播放");
    size_t scratch_bytes = mp3dec_scratch_size();
    s_scratch = heap_caps_malloc(scratch_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_scratch == NULL) {
        ESP_LOGE(TAG, "scratch alloc failed: %u bytes", (unsigned)scratch_bytes);
        return ESP_ERR_NO_MEM;
    }
    memset(s_scratch, 0, scratch_bytes);
    if (xTaskCreate(worker, "panel_music", MUSIC_STACK, NULL, 4, &s_task) != pdPASS) {
        heap_caps_free(s_scratch);
        s_scratch = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "worker stack=%u scratch_psram=%u", (unsigned)MUSIC_STACK,
             (unsigned)scratch_bytes);
    s_started = true;
    return ESP_OK;
}

esp_err_t panel_music_play(void)
{
    if (!s_started && panel_music_init() != ESP_OK) return ESP_ERR_NO_MEM;
    if (panel_music_import_is_active()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state == PANEL_MUSIC_PLAYING) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_status.state == PANEL_MUSIC_PAUSED) {
        set_pause(false);
        s_status.state = PANEL_MUSIC_PLAYING;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_status.state == PANEL_MUSIC_LOADING || s_status.state == PANEL_MUSIC_STOPPING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t track = s_selected_track;
    const panel_music_track_t *meta = panel_music_track_at(track);
    if (meta == NULL) {
        xSemaphoreGive(s_lock);
        set_status(PANEL_MUSIC_ERROR, "曲目无效");
        return ESP_ERR_INVALID_ARG;
    }
    s_active_track = track;
    s_status.track = track;
    snprintf(s_status.title, sizeof(s_status.title), "%s", meta->title);
    snprintf(s_status.artist, sizeof(s_status.artist), "%s", meta->artist);
    snprintf(s_status.file, sizeof(s_status.file), "%s/%s", MUSIC_DIR, meta->file);
    s_status.state = PANEL_MUSIC_LOADING;
    s_auto_play = false;
    set_stop(false);
    set_pause(false);
    snprintf(s_status.detail, sizeof(s_status.detail), "%s", "启动中");
    xSemaphoreGive(s_lock);
    uint8_t command = 1;
    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        set_status(PANEL_MUSIC_ERROR, "播放器忙");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void panel_music_pause(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state == PANEL_MUSIC_PLAYING) {
        set_pause(true);
        s_status.state = PANEL_MUSIC_PAUSED;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "已暂停");
    }
    xSemaphoreGive(s_lock);
}

void panel_music_resume(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state == PANEL_MUSIC_PAUSED) {
        set_pause(false);
        s_status.state = PANEL_MUSIC_PLAYING;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "播放中");
    }
    xSemaphoreGive(s_lock);
}

void panel_music_toggle(void)
{
    if (s_lock == NULL) return;
    panel_music_state_t state;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    state = s_status.state;
    xSemaphoreGive(s_lock);
    if (state == PANEL_MUSIC_PLAYING) panel_music_pause();
    else if (state == PANEL_MUSIC_PAUSED) panel_music_resume();
    else (void)panel_music_play();
}

void panel_music_stop(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state == PANEL_MUSIC_IDLE || s_status.state == PANEL_MUSIC_STOPPED ||
        s_status.state == PANEL_MUSIC_ERROR) {
        s_status.state = PANEL_MUSIC_STOPPED;
        xSemaphoreGive(s_lock);
        return;
    }
    set_pause(false);
    set_stop(true);
    s_auto_play = false;
    s_status.state = PANEL_MUSIC_STOPPING;
    snprintf(s_status.detail, sizeof(s_status.detail), "%s", "停止中");
    xSemaphoreGive(s_lock);
}

void panel_music_select(uint8_t track)
{
    if (s_lock == NULL) return;
    const panel_music_track_t *meta = panel_music_track_at(track);
    if (meta == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_selected_track = track;
    s_status.track = track;
    snprintf(s_status.title, sizeof(s_status.title), "%s", meta->title);
    snprintf(s_status.artist, sizeof(s_status.artist), "%s", meta->artist);
    snprintf(s_status.file, sizeof(s_status.file), "%s/%s", MUSIC_DIR, meta->file);
    if (s_status.state == PANEL_MUSIC_PLAYING || s_status.state == PANEL_MUSIC_PAUSED ||
        s_status.state == PANEL_MUSIC_LOADING) {
        set_pause(false);
        set_stop(true);
        s_auto_play = true;
        s_status.state = PANEL_MUSIC_STOPPING;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "切换曲目");
    } else if (s_status.state != PANEL_MUSIC_STOPPING) {
        s_status.state = PANEL_MUSIC_STOPPED;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "已选择曲目");
    }
    xSemaphoreGive(s_lock);
}

void panel_music_next(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t next = (uint8_t)((s_selected_track + 1U) % PANEL_MUSIC_TRACK_COUNT);
    xSemaphoreGive(s_lock);
    panel_music_select(next);
}

void panel_music_prev(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t prev = (uint8_t)((s_selected_track + PANEL_MUSIC_TRACK_COUNT - 1U) %
                             PANEL_MUSIC_TRACK_COUNT);
    xSemaphoreGive(s_lock);
    panel_music_select(prev);
}

void panel_music_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.volume = volume;
        xSemaphoreGive(s_lock);
    }
    panel_audio_stream_set_volume(volume);
}

void panel_music_get_status(panel_music_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        out->state = PANEL_MUSIC_IDLE;
        out->volume = 20;
        snprintf(out->detail, sizeof(out->detail), "%s", "未初始化");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

#ifdef PANEL_MUSIC_HOST_TEST
uint8_t panel_music_test_active_track(void)
{
    uint8_t track = 0;
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    track = s_active_track;
    xSemaphoreGive(s_lock);
    return track;
}
#endif

bool panel_music_is_active(void)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_status.state == PANEL_MUSIC_LOADING ||
                  s_status.state == PANEL_MUSIC_PLAYING ||
                  s_status.state == PANEL_MUSIC_PAUSED ||
                  s_status.state == PANEL_MUSIC_STOPPING;
    xSemaphoreGive(s_lock);
    return active;
}
