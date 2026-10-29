#include "panel_audio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_audio_tone.h"
#include "panel_files.h"
#include "panel_wav.h"

#define TAG "panel_audio"
#define AUDIO_TASK_STACK 8192U
#define AUDIO_CHUNK_FRAMES 256U
#define AUDIO_WAV_DIR "/panel/audio"
#define AUDIO_WAV_NAME "test_tone.wav"
#define AUDIO_WAV_MAX_FILE (PANEL_WAV_MAX_DATA_BYTES + 1024U)

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_stream_lock;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static esp_codec_dev_handle_t s_codec;
static panel_audio_status_t s_status;
static bool s_started;
static bool s_worker_started;

static void set_status(panel_audio_state_t state, const char *detail)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    snprintf(s_status.detail, sizeof(s_status.detail), "%s", detail != NULL ? detail : "");
    xSemaphoreGive(s_lock);
}

static bool write_test_wav(const char *path)
{
    size_t frames = panel_audio_tone_frames();
    uint32_t data_bytes = (uint32_t)(frames * 2U);
    uint8_t header[44];
    if (!panel_wav_make_header(header, data_bytes)) return false;

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "wb");
    if (f == NULL) {
        close(fd);
        remove(path);
        return false;
    }
    bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    uint32_t sample_index = 0;
    size_t done = 0;
    while (ok && done < frames) {
        size_t chunk = frames - done;
        if (chunk > AUDIO_CHUNK_FRAMES) chunk = AUDIO_CHUNK_FRAMES;
        int16_t pcm[AUDIO_CHUNK_FRAMES];
        panel_audio_fill_tone(pcm, chunk, &sample_index);
        ok = fwrite(pcm, sizeof(int16_t), chunk, f) == chunk;
        done += chunk;
    }
    if (fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0 && errno != EINVAL && errno != ENOSYS && errno != ENOTSUP) {
        ok = false;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}

static bool read_file_psram(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size <= 0 || size > (long)AUDIO_WAV_MAX_FILE) {
        fclose(f);
        return false;
    }
    rewind(f);
    uint8_t *data = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        fclose(f);
        return false;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        heap_caps_free(data);
        return false;
    }
    *out = data;
    *out_len = (size_t)size;
    return true;
}

static bool load_tf_wav(uint8_t **out_data, size_t *out_len, panel_wav_info_t *info,
                        bool *created)
{
    *out_data = NULL;
    *out_len = 0;
    *created = false;
    if (!panel_files_card_ready()) {
        set_status(PANEL_AUDIO_ERROR, "TF WAV：无TF卡");
        return false;
    }
    const char *root = panel_files_root_path();
    if (root == NULL || root[0] == '\0') {
        set_status(PANEL_AUDIO_ERROR, "TF WAV：无挂载点");
        return false;
    }
    char dir[192];
    char path[224];
    int n = snprintf(dir, sizeof(dir), "%s%s", root, AUDIO_WAV_DIR);
    if (n <= 0 || (size_t)n >= sizeof(dir)) {
        set_status(PANEL_AUDIO_ERROR, "TF WAV：路径过长");
        return false;
    }
    n = snprintf(path, sizeof(path), "%s/%s", dir, AUDIO_WAV_NAME);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        set_status(PANEL_AUDIO_ERROR, "TF WAV：路径过长");
        return false;
    }
    if (!panel_files_io_acquire()) {
        set_status(PANEL_AUDIO_ERROR, "TF WAV：卡忙");
        return false;
    }

    bool ok = true;
    bool exists = false;
    FILE *probe = fopen(path, "rb");
    if (probe != NULL) {
        exists = true;
        fclose(probe);
    }
    if (!exists) {
        char base[192];
        int bn = snprintf(base, sizeof(base), "%s/panel", root);
        if (bn <= 0 || (size_t)bn >= sizeof(base) ||
            (mkdir(base, 0777) != 0 && errno != EEXIST) ||
            (mkdir(dir, 0777) != 0 && errno != EEXIST)) {
            ok = false;
        } else if (!write_test_wav(path)) {
            ok = false;
        } else {
            *created = true;
        }
    }
    if (ok) {
        uint8_t *data = NULL;
        size_t len = 0;
        ok = read_file_psram(path, &data, &len);
        if (ok && !panel_wav_parse(data, len, info)) {
            ok = false;
        }
        if (ok) {
            *out_data = data;
            *out_len = len;
            ESP_LOGI(TAG, "TF WAV source=card path=%s created=%d file=%u data=%u frames=%u "
                          "rate=%u",
                     path, *created ? 1 : 0, (unsigned)len, (unsigned)info->data_bytes,
                     (unsigned)info->frames, PANEL_WAV_SAMPLE_RATE);
        } else if (data != NULL) {
            heap_caps_free(data);
        }
    }
    panel_files_io_release();
    if (!ok) set_status(PANEL_AUDIO_ERROR, "TF WAV：格式/读取失败");
    return ok;
}

bool panel_audio_stream_begin(void)
{
    if (xSemaphoreTake(s_stream_lock, 0) != pdTRUE) return false;
    if (s_codec == NULL) {
        s_codec = bsp_audio_codec_speaker_init();
        if (s_codec == NULL) {
            set_status(PANEL_AUDIO_ERROR, "ES8311/I2S 初始化失败");
            xSemaphoreGive(s_stream_lock);
            return false;
        }
        (void)esp_codec_dev_set_out_vol(s_codec, 0);
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = PANEL_AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    int err = esp_codec_dev_open(s_codec, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        set_status(PANEL_AUDIO_ERROR, "ES8311 打开失败");
        xSemaphoreGive(s_stream_lock);
        return false;
    }
    err = esp_codec_dev_set_out_vol(s_codec, PANEL_AUDIO_TEST_VOLUME);
    if (err != ESP_CODEC_DEV_OK) {
        (void)esp_codec_dev_set_out_vol(s_codec, 0);
        (void)esp_codec_dev_close(s_codec);
        set_status(PANEL_AUDIO_ERROR, "音量设置失败，已停止");
        xSemaphoreGive(s_stream_lock);
        return false;
    }
    return true;
}

bool panel_audio_stream_write(const int16_t *pcm, size_t frames)
{
    size_t done = 0;
    while (done < frames) {
        size_t chunk = frames - done;
        if (chunk > AUDIO_CHUNK_FRAMES) chunk = AUDIO_CHUNK_FRAMES;
        if (esp_codec_dev_write(s_codec, (void *)(pcm + done),
                                (int)(chunk * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
            return false;
        }
        done += chunk;
    }
    return true;
}

void panel_audio_stream_set_volume(int volume)
{
    if (s_codec == NULL) return;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    (void)esp_codec_dev_set_out_vol(s_codec, volume);
}

void panel_audio_stream_end(void)
{
    if (s_codec != NULL) {
        (void)esp_codec_dev_set_out_vol(s_codec, 0);
        (void)esp_codec_dev_close(s_codec);
    }
    xSemaphoreGive(s_stream_lock);
}

bool panel_audio_stream_busy(void)
{
    if (s_stream_lock == NULL) return false;
    if (xSemaphoreTake(s_stream_lock, 0) == pdTRUE) {
        xSemaphoreGive(s_stream_lock);
        return false;
    }
    return true;
}

static void audio_worker(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t command;
        if (xQueueReceive(s_queue, &command, portMAX_DELAY) != pdTRUE) continue;

        if (command == 2) {
            uint8_t *wav_data = NULL;
            size_t wav_len = 0;
            panel_wav_info_t info;
            bool created = false;
            set_status(PANEL_AUDIO_PLAYING, "TF WAV：正在读取/校验");
            if (!load_tf_wav(&wav_data, &wav_len, &info, &created)) {
                continue;
            }
            (void)created;
            set_status(PANEL_AUDIO_PLAYING, "TF WAV：正在播放");
            if (!panel_audio_stream_begin()) {
                heap_caps_free(wav_data);
                set_status(PANEL_AUDIO_ERROR, "音频输出忙");
                continue;
            }
            bool ok = panel_audio_stream_write((const int16_t *)info.data, info.frames);
            panel_audio_stream_end();
            heap_caps_free(wav_data);
            if (!ok) {
                set_status(PANEL_AUDIO_ERROR, "TF WAV：I2S 写入失败");
                continue;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.frames = info.frames;
            s_status.duration_ms = info.frames * 1000U / PANEL_WAV_SAMPLE_RATE;
            xSemaphoreGive(s_lock);
            set_status(PANEL_AUDIO_DONE, "TF WAV 已从卡读取并提交；未验证扬声器");
            ESP_LOGI(TAG, "TF WAV playback submitted: frames=%u data=%u file=%u",
                     (unsigned)info.frames, (unsigned)info.data_bytes, (unsigned)wav_len);
            continue;
        }

        set_status(PANEL_AUDIO_PLAYING, "正在播放内置测试音");
        if (!panel_audio_stream_begin()) {
            set_status(PANEL_AUDIO_ERROR, "音频输出忙");
            continue;
        }
        size_t total = panel_audio_tone_frames();
        uint32_t sample_index = 0;
        size_t done = 0;
        bool ok = true;
        while (done < total) {
            size_t frames = total - done;
            if (frames > AUDIO_CHUNK_FRAMES) frames = AUDIO_CHUNK_FRAMES;
            int16_t pcm[AUDIO_CHUNK_FRAMES];
            panel_audio_fill_tone(pcm, frames, &sample_index);
            if (!panel_audio_stream_write(pcm, frames)) {
                ok = false;
                break;
            }
            done += frames;
        }
        panel_audio_stream_end();
        if (!ok) {
            set_status(PANEL_AUDIO_ERROR, "I2S 写入失败，未确认发声");
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.frames = (uint32_t)done;
        s_status.duration_ms = PANEL_AUDIO_TONE_MS;
        xSemaphoreGive(s_lock);
        set_status(PANEL_AUDIO_DONE,
                   "I2S/ES8311 写入完成；请确认是否听到（未验证扬声器）");
        ESP_LOGI(TAG, "test tone submitted: %u frames, volume=%d, %u ms",
                 (unsigned)done, PANEL_AUDIO_TEST_VOLUME, PANEL_AUDIO_TONE_MS);
    }
}

esp_err_t panel_audio_init(void)
{
    if (s_started) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_stream_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(1, sizeof(uint8_t));
    if (s_lock == NULL || s_stream_lock == NULL || s_queue == NULL) return ESP_ERR_NO_MEM;
    s_status.state = PANEL_AUDIO_IDLE;
    s_status.source = 0;
    s_status.volume = PANEL_AUDIO_TEST_VOLUME;
    s_status.duration_ms = PANEL_AUDIO_TONE_MS;
    s_status.frames = 0;
    snprintf(s_status.detail, sizeof(s_status.detail), "%s",
             "未测试（诊断任务未创建）");
    s_started = true;
    ESP_LOGI(TAG, "audio test worker deferred to audiotest/wavtest");
    return ESP_OK;
}

static esp_err_t ensure_worker(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_worker_started) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (xTaskCreate(audio_worker, "panel_audio", AUDIO_TASK_STACK, NULL, 4, &s_task) !=
        pdPASS) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    s_worker_started = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "diagnostic worker created: stack=%u", (unsigned)AUDIO_TASK_STACK);
    return ESP_OK;
}

esp_err_t panel_audio_test_start(void)
{
    if (!s_started && panel_audio_init() != ESP_OK) return ESP_ERR_NO_MEM;
    if (ensure_worker() != ESP_OK) {
        set_status(PANEL_AUDIO_ERROR, "音频诊断任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    if (panel_audio_stream_busy()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state == PANEL_AUDIO_QUEUED || s_status.state == PANEL_AUDIO_PLAYING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = PANEL_AUDIO_QUEUED;
    s_status.source = 0;
    s_status.frames = 0;
    snprintf(s_status.detail, sizeof(s_status.detail), "%s",
             "已排队：低音量 0.8 秒");
    xSemaphoreGive(s_lock);
    uint8_t command = 1;
    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        set_status(PANEL_AUDIO_IDLE, "音频忙，未启动");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t panel_audio_test_wav_start(void)
{
    if (!s_started && panel_audio_init() != ESP_OK) return ESP_ERR_NO_MEM;
    if (ensure_worker() != ESP_OK) {
        set_status(PANEL_AUDIO_ERROR, "音频诊断任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    if (panel_audio_stream_busy()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state == PANEL_AUDIO_QUEUED || s_status.state == PANEL_AUDIO_PLAYING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = PANEL_AUDIO_QUEUED;
    s_status.source = 1;
    s_status.frames = 0;
    snprintf(s_status.detail, sizeof(s_status.detail), "%s", "TF WAV 已排队");
    xSemaphoreGive(s_lock);
    uint8_t command = 2;
    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        set_status(PANEL_AUDIO_IDLE, "音频忙，未启动");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void panel_audio_get_status(panel_audio_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        out->state = PANEL_AUDIO_IDLE;
        out->volume = PANEL_AUDIO_TEST_VOLUME;
        out->duration_ms = PANEL_AUDIO_TONE_MS;
        snprintf(out->detail, sizeof(out->detail), "%s", "未初始化");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}
