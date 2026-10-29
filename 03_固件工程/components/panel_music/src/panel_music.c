#include "panel_music.h"

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
#include "panel_music_index.h"
#include "panel_music_prebuilt.h"
#include "panel_music_resample.h"
#include "panel_music_tracks.h"

#define TAG "panel_music"
#define MUSIC_DIR "/panel/music"
/* The decoder's 16 KiB scratch and ~25 KiB sparse seek index live in PSRAM.
 * Target -fstack-usage: worker=1504 B and decode_with_scratch=1136 B; the old
 * full-track highwater was 19.5 KiB, which includes the 16.2 KiB scratch.
 * 12 KiB therefore leaves about 9 KiB of measured margin. */
#define MUSIC_STACK 12288U
#define MUSIC_IN_BUF (32U * 1024U)
#define MUSIC_OUT_HZ PANEL_MUSIC_OUT_HZ
#define MUSIC_QUEUE_DEPTH 4U
#define MUSIC_SEEK_END_MARGIN_MS 1000U

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_seek_lock;
static SemaphoreHandle_t s_index_lock;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static mp3dec_scratch_t *s_scratch;
static panel_mp3_index_t *s_index;
static uint32_t s_track_generation;

static bool queue_play(void);
static panel_music_status_t s_status;
static volatile bool s_stop;
static volatile bool s_pause;
static portMUX_TYPE s_flag_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;
static bool s_init_failed;
static uint8_t s_selected_track;
static uint8_t s_active_track;
static bool s_auto_play;
static bool s_liked[PANEL_MUSIC_TRACK_COUNT];
static bool s_shuffle;
static bool s_repeat;
static uint32_t s_rand_state = 0x13579BDFU;
static uint32_t s_seek_ms;
static uint32_t s_seek_seq;
static uint32_t s_seek_seen;
static bool s_seek_pending;

#ifdef PANEL_MUSIC_HOST_TEST
static uint32_t s_test_last_seek_target;
static volatile unsigned s_test_loop_iterations;
static uint32_t s_test_open_delay_ms;
static bool s_test_open_error_once;
static bool s_test_force_prebuilt_mismatch;
static bool s_test_force_prebuilt_io_error;
static uint8_t s_test_index_source;
static bool s_test_seek_header_mismatch;
static uint32_t s_test_index_publish_delay_ms;
#endif

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
    taskEXIT_CRITICAL(&s_flag_mux);
}

static uint32_t bump_generation(void)
{
    taskENTER_CRITICAL(&s_flag_mux);
    s_track_generation++;
    uint32_t gen = s_track_generation;
    taskEXIT_CRITICAL(&s_flag_mux);
    return gen;
}

static uint32_t current_generation(void)
{
    taskENTER_CRITICAL(&s_flag_mux);
    uint32_t gen = s_track_generation;
    taskEXIT_CRITICAL(&s_flag_mux);
    return gen;
}

/* Seek state lock order is always s_lock -> s_seek_lock. */
static void clear_seek_locked(void)
{
    xSemaphoreTake(s_seek_lock, portMAX_DELAY);
    s_seek_seq++;
    s_seek_ms = 0;
    s_seek_seen = s_seek_seq;
    s_seek_pending = false;
    s_status.seek_pending = false;
    xSemaphoreGive(s_seek_lock);
}

static bool consume_seek_request(uint32_t *position_ms, uint32_t *generation)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xSemaphoreTake(s_seek_lock, portMAX_DELAY);
    if (s_seek_seq != s_seek_seen) {
        s_seek_seen = s_seek_seq;
        *position_ms = s_seek_ms;
        *generation = s_seek_seq;
        found = true;
    }
    xSemaphoreGive(s_seek_lock);
    xSemaphoreGive(s_lock);
    return found;
}

static bool seek_request_unseen(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xSemaphoreTake(s_seek_lock, portMAX_DELAY);
    bool unseen = s_seek_seq != s_seek_seen;
    xSemaphoreGive(s_seek_lock);
    xSemaphoreGive(s_lock);
    return unseen;
}

static bool seek_generation_current(uint32_t generation)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xSemaphoreTake(s_seek_lock, portMAX_DELAY);
    bool current = s_seek_pending && s_seek_seq == generation;
    xSemaphoreGive(s_seek_lock);
    xSemaphoreGive(s_lock);
    return current;
}

static bool finish_seek(uint32_t generation, bool success, const char *detail,
                        uint32_t position_ms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xSemaphoreTake(s_seek_lock, portMAX_DELAY);
    bool current = s_seek_seq == generation;
    if (current) {
        s_seek_pending = false;
        s_status.seek_pending = false;
        if (success) s_status.position_ms = position_ms;
        if (detail != NULL) {
            snprintf(s_status.detail, sizeof(s_status.detail), "%s", detail);
        }
    }
    xSemaphoreGive(s_seek_lock);
    xSemaphoreGive(s_lock);
    return current;
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

static bool file_open(FILE **out, const char *path, bool *id3, uint32_t *tag_bytes,
                      uint32_t *file_size, char *error, size_t error_size)
{
    *out = NULL;
    *id3 = false;
    *tag_bytes = 0;
    *file_size = 0;
    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (!panel_files_card_ready()) {
        if (error != NULL) snprintf(error, error_size, "%s", "无TF卡");
        return false;
    }
    if (!panel_files_mount_pin_acquire()) {
        if (error != NULL) snprintf(error, error_size, "%s", "TF卡正在重试，稍后再试");
        return false;
    }
    if (!panel_files_io_acquire()) {
        panel_files_mount_pin_release();
        if (error != NULL) snprintf(error, error_size, "%s", "TF卡忙");
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        panel_files_io_release();
        panel_files_mount_pin_release();
        if (error != NULL) snprintf(error, error_size, "%s", "找不到音乐文件");
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
            if (error != NULL) snprintf(error, error_size, "%s", "ID3标签异常");
            return false;
        }
        *id3 = true;
        *tag_bytes = total;
    } else {
        rewind(f);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        panel_files_io_release();
        panel_files_mount_pin_release();
        if (error != NULL) snprintf(error, error_size, "%s", "读取文件长度失败");
        return false;
    }
    long size = ftell(f);
    if (size <= (long)*tag_bytes) {
        fclose(f);
        panel_files_io_release();
        panel_files_mount_pin_release();
        if (error != NULL) snprintf(error, error_size, "%s", "音乐文件为空");
        return false;
    }
    *file_size = (uint32_t)size;
    if (fseek(f, (long)*tag_bytes, SEEK_SET) != 0) {
        fclose(f);
        panel_files_io_release();
        panel_files_mount_pin_release();
        if (error != NULL) snprintf(error, error_size, "%s", "定位音乐数据失败");
        return false;
    }
#ifdef PANEL_MUSIC_HOST_TEST
    if (s_test_open_delay_ms > 0U) vTaskDelay(pdMS_TO_TICKS(s_test_open_delay_ms));
    if (s_test_open_error_once) {
        s_test_open_error_once = false;
        fclose(f);
        panel_files_io_release();
        panel_files_mount_pin_release();
        if (error != NULL) snprintf(error, error_size, "%s", "测试打开失败");
        return false;
    }
#endif
    panel_files_io_release();
    *out = f;
    return true;
}

typedef struct {
    FILE *file;
    uint32_t cursor;
    bool cursor_valid;
} tf_read_ctx_t;

static size_t tf_read_at(void *opaque, uint32_t offset, uint8_t *buf, size_t len,
                         bool *io_error)
{
    *io_error = false;
    tf_read_ctx_t *ctx = (tf_read_ctx_t *)opaque;
    if (ctx == NULL || ctx->file == NULL || !panel_files_card_ready()) {
        *io_error = true;
        return 0;
    }
    if (!panel_files_io_acquire()) {
        *io_error = true;
        return 0;
    }
    size_t n = 0;
    if (!ctx->cursor_valid || offset != ctx->cursor) {
        if (fseek(ctx->file, (long)offset, SEEK_SET) != 0) {
            *io_error = true;
            ctx->cursor_valid = false;
        }
    }
    if (!*io_error) {
        n = fread(buf, 1, len, ctx->file);
        ctx->cursor = offset + (uint32_t)n;
        ctx->cursor_valid = true;
        if (ferror(ctx->file)) {
            *io_error = true;
            n = 0;
            ctx->cursor_valid = false;
        }
    }
    panel_files_io_release();
#ifndef PANEL_MUSIC_HOST_TEST
    /* Low-priority background index: one short lease per 16 KiB, then yield so
     * the playback worker can service TF/audio without starving.  This is not
     * a hard latency guarantee. */
    vTaskDelay(pdMS_TO_TICKS(1));
#endif
    return n;
}

static bool tf_seek_file(FILE *f, uint32_t offset)
{
    if (!panel_files_io_acquire()) return false;
    bool ok = fseek(f, (long)offset, SEEK_SET) == 0;
    if (!ok) clearerr(f);
    panel_files_io_release();
    return ok;
}

static void close_music_file(FILE *f)
{
    if (f == NULL) return;
    if (panel_files_io_acquire()) {
        fclose(f);
        panel_files_io_release();
    } else {
        /* A failed lease would mean the files service itself is gone.  Keeping
         * the descriptor open is safer than closing while a remount owns it. */
        ESP_LOGE(TAG, "could not lease TF I/O to close music file");
    }
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

static void apply_liked_locked(void)
{
    s_status.liked = s_liked[s_status.track % PANEL_MUSIC_TRACK_COUNT];
}

static void select_locked(uint8_t track)
{
    const panel_music_track_t *meta = panel_music_track_at(track);
    if (meta == NULL) return;
    s_selected_track = track;
    s_status.track = track;
    snprintf(s_status.title, sizeof(s_status.title), "%s", meta->title);
    snprintf(s_status.artist, sizeof(s_status.artist), "%s", meta->artist);
    snprintf(s_status.file, sizeof(s_status.file), "%s/%s", MUSIC_DIR, meta->file);
    s_status.position_ms = 0;
    s_status.duration_ms = 0;
    s_status.frames = 0;
    s_status.sample_rate = 0;
    s_status.channels = 0;
    s_status.seek_pending = false;
    s_status.index_ready = false;
    s_status.index_building = false;
    s_status.index_failed = false;
    apply_liked_locked();
    clear_seek_locked();
    bump_generation();
}

static uint8_t random_next(uint8_t current)
{
    if (PANEL_MUSIC_TRACK_COUNT <= 1U) return current;
    s_rand_state = s_rand_state * 1664525U + 1013904223U;
    uint8_t offset = (uint8_t)(1U + (s_rand_state % (PANEL_MUSIC_TRACK_COUNT - 1U)));
    return (uint8_t)((current + offset) % PANEL_MUSIC_TRACK_COUNT);
}

static uint8_t chosen_next(uint8_t current)
{
    return s_shuffle ? random_next(current)
                     : (uint8_t)((current + 1U) % PANEL_MUSIC_TRACK_COUNT);
}

static bool take_auto_restart(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool restart = s_auto_play;
    if (restart) {
        s_auto_play = false;
        set_stop(false);
        set_pause(false);
        clear_seek_locked();
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

static bool prepare_repeat_after_end(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t current = s_active_track;
    bool advance = s_shuffle || s_repeat || current + 1U < PANEL_MUSIC_TRACK_COUNT;
    if (advance) {
        uint8_t next = chosen_next(current);
        select_locked(next);
        s_active_track = next;
        s_status.state = PANEL_MUSIC_LOADING;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "连续播放");
    } else {
        s_status.state = PANEL_MUSIC_STOPPED;
        s_status.position_ms = s_status.duration_ms;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "播放结束");
    }
    xSemaphoreGive(s_lock);
    return advance;
}

static void handle_open_failure(uint32_t play_gen, const char *error)
{
    bool restart = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool stale = play_gen != current_generation() || stop_requested();
    if (!stale) {
        s_status.state = PANEL_MUSIC_ERROR;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", error);
    } else if (s_auto_play) {
        s_auto_play = false;
        set_stop(false);
        set_pause(false);
        clear_seek_locked();
        s_active_track = s_selected_track;
        s_status.state = PANEL_MUSIC_LOADING;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "切换曲目");
        restart = true;
    } else if (stop_requested()) {
        s_status.state = PANEL_MUSIC_STOPPED;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "已停止");
    }
    xSemaphoreGive(s_lock);
    if (restart && !queue_play()) {
        set_status(PANEL_MUSIC_ERROR, "切换失败");
    }
}

static bool tf_cancel_scan(void *opaque)
{
    (void)opaque;
    return stop_requested();
}

static uint32_t fnv1a32(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619U;
    }
    return h;
}

/* 1 = matched and loaded, 0 = mismatch (safe to full-scan), -1 = TF error. */
static int prebuilt_try_load(FILE *f, uint8_t track, uint32_t file_size, bool id3,
                             uint32_t tag, uint32_t play_gen, char *error,
                             size_t error_size)
{
    if (track >= PANEL_MUSIC_PREBUILT_TRACKS) return 0;
#ifdef PANEL_MUSIC_HOST_TEST
    if (s_test_force_prebuilt_mismatch) return 0;
#endif
    const panel_music_prebuilt_track_t *m = &panel_music_prebuilt_tracks[track];
    if (m->file_size != file_size || m->tag_bytes != tag ||
        (m->has_id3 != 0U) != id3 ||
        m->entry_count == 0U || m->entry_count > PANEL_MP3_INDEX_MAX ||
        m->fingerprint_count == 0U ||
        m->fingerprint_count > PANEL_MUSIC_FINGERPRINT_COUNT) {
        return 0;
    }
    uint8_t buf[PANEL_MUSIC_FINGERPRINT_BYTES];
    int result = 1;
    for (uint32_t i = 0; i < m->fingerprint_count; i++) {
        if (play_gen != current_generation() || stop_requested()) {
            result = -2;
            break;
        }
        const panel_music_fingerprint_t *fp = &m->fingerprints[i];
        if (fp->len == 0U || fp->len > sizeof(buf) ||
            fp->offset > file_size || fp->len > file_size - fp->offset) {
            result = -1;
            snprintf(error, error_size, "%s", "预索引指纹无效");
            break;
        }
        if (!panel_files_io_acquire()) {
            result = -1;
            snprintf(error, error_size, "%s", "TF卡忙");
            break;
        }
#ifdef PANEL_MUSIC_HOST_TEST
        if (s_test_force_prebuilt_io_error) {
            panel_files_io_release();
            result = -1;
            snprintf(error, error_size, "%s", "测试TF读取失败");
            break;
        }
#endif
        bool io_ok = fseek(f, (long)fp->offset, SEEK_SET) == 0 &&
                     fread(buf, 1, fp->len, f) == fp->len;
        panel_files_io_release();
        if (!io_ok) {
            result = -1;
            snprintf(error, error_size, "%s", "TF指纹读取失败");
            break;
        }
        if (fnv1a32(buf, fp->len) != fp->hash) {
            result = 0;
            break;
        }
    }
    if (play_gen != current_generation() || stop_requested()) result = -2;
    if (!panel_files_io_acquire()) {
        snprintf(error, error_size, "%s", "TF卡忙");
        return -1;
    }
    if (fseek(f, (long)tag, SEEK_SET) != 0) {
        result = -1;
        snprintf(error, error_size, "%s", "TF定位失败");
    }
    panel_files_io_release();
    if (result != 1) {
        if (result == -2) snprintf(error, error_size, "%s", "cancelled");
        return result;
    }

    memset(s_index, 0, sizeof(*s_index));
    s_index->data_start = m->data_start;
    s_index->first_frame_offset = m->first_frame_offset;
    s_index->total_frames = m->total_frames;
    s_index->total_samples = m->total_samples;
    s_index->duration_ms = m->duration_ms;
    s_index->sample_rate = m->sample_rate;
    s_index->entry_count = m->entry_count;
    s_index->frame_samples = m->frame_samples;
    s_index->channels = m->channels;
    s_index->version = m->version;
    s_index->layer = m->layer;
    memcpy(s_index->entries, m->entries, m->entry_count * sizeof(m->entries[0]));
    return 1;
}

/* 1 = frame header matches, 0 = mismatch, -1 = TF error.  Restores the stream
 * file position before returning. */
static int tf_validate_frame(FILE *f, uint32_t offset, uint32_t expected,
                             char *error, size_t error_size)
{
    if (!panel_files_io_acquire()) {
        snprintf(error, error_size, "%s", "TF卡忙");
        return -1;
    }
    long saved = ftell(f);
    uint8_t h[4];
    int result = 1;
    if (saved < 0 || fseek(f, (long)offset, SEEK_SET) != 0 ||
        fread(h, 1, sizeof(h), f) != sizeof(h)) {
        result = -1;
        snprintf(error, error_size, "%s", "跳转帧读取失败");
    } else {
        uint32_t got = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                       ((uint32_t)h[2] << 8) | (uint32_t)h[3];
        result = got == expected ? 1 : 0;
#ifdef PANEL_MUSIC_HOST_TEST
        if (s_test_seek_header_mismatch) result = 0;
#endif
    }
    if (saved >= 0 && fseek(f, saved, SEEK_SET) != 0) {
        result = -1;
        snprintf(error, error_size, "%s", "跳转后文件定位恢复失败");
    }
    panel_files_io_release();
    return result;
}

static void handle_index_failure(uint32_t play_gen, bool io_error, const char *error)
{
    bool restart = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool stale = play_gen != current_generation() || stop_requested();
    s_status.index_building = false;
    s_status.index_ready = false;
    if (!stale) {
        s_status.index_failed = io_error;
        if (io_error) {
            s_status.state = PANEL_MUSIC_ERROR;
            snprintf(s_status.detail, sizeof(s_status.detail), "%s", error);
        }
    } else if (s_auto_play) {
        s_auto_play = false;
        set_stop(false);
        set_pause(false);
        clear_seek_locked();
        s_active_track = s_selected_track;
        s_status.state = PANEL_MUSIC_LOADING;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "切换曲目");
        restart = true;
    } else if (stop_requested()) {
        s_status.state = PANEL_MUSIC_STOPPED;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "已停止");
    }
    xSemaphoreGive(s_lock);
    if (restart && !queue_play()) {
        set_status(PANEL_MUSIC_ERROR, "切换失败");
    }
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t command;
        if (xQueueReceive(s_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        if (command != 1) continue;
        bool advanced = false;
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
        uint32_t play_gen;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        track = s_active_track;
        play_gen = current_generation();
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
        uint32_t file_size = 0;
        char open_error[64];
        if (!file_open(&f, path, &id3, &tag_bytes, &file_size, open_error,
                       sizeof(open_error))) {
            handle_open_failure(play_gen, open_error);
            continue;
        }
        (void)file_size;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.index_building = true;
        s_status.index_ready = false;
        s_status.index_failed = false;
        xSemaphoreGive(s_lock);
        char index_error[64];
        int prebuilt = prebuilt_try_load(f, track, file_size, id3, tag_bytes, play_gen,
                                         index_error, sizeof(index_error));
        if (prebuilt < 0) {
            close_music_file(f);
            panel_files_mount_pin_release();
            handle_index_failure(play_gen, prebuilt == -1, index_error);
            continue;
        }
        bool loaded_prebuilt = prebuilt == 1;
#ifdef PANEL_MUSIC_HOST_TEST
        s_test_index_source = loaded_prebuilt ? 1U : 2U;
#endif
        if (!loaded_prebuilt) {
            ESP_LOGI(TAG, "prebuilt index mismatch track=%u; full 16KiB scan", (unsigned)track);
            tf_read_ctx_t read_ctx = {.file = f, .cursor = tag_bytes, .cursor_valid = true};
            bool scanned = panel_mp3_index_build_leased(
                s_index, tag_bytes, file_size, tf_read_at, tf_cancel_scan, &read_ctx,
                index_error, sizeof(index_error));
            if (!scanned) {
                close_music_file(f);
                panel_files_mount_pin_release();
                handle_index_failure(play_gen, !stop_requested(), index_error);
                continue;
            }
            if (!tf_seek_file(f, tag_bytes)) {
                close_music_file(f);
                panel_files_mount_pin_release();
                handle_index_failure(play_gen, true, "索引后复位失败");
                continue;
            }
        }
#ifdef PANEL_MUSIC_HOST_TEST
        if (s_test_index_publish_delay_ms > 0U) {
            uint32_t delay = s_test_index_publish_delay_ms;
            s_test_index_publish_delay_ms = 0;
            vTaskDelay(pdMS_TO_TICKS(delay));
        }
#endif
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool publish = play_gen == current_generation() && !stop_requested();
        if (publish) {
            s_status.sample_rate = s_index->sample_rate;
            s_status.channels = s_index->channels;
            s_status.duration_ms = s_index->duration_ms;
            s_status.position_ms = 0;
            s_status.seek_pending = false;
            s_status.index_ready = true;
            s_status.index_building = false;
            s_status.index_failed = false;
        }
        xSemaphoreGive(s_lock);
        if (!publish) {
            close_music_file(f);
            panel_files_mount_pin_release();
            handle_index_failure(play_gen, false, "cancelled");
            continue;
        }
        ESP_LOGI(TAG, "MP3 index %s: frames=%u duration=%ums entries=%u %uHz %uch",
                 loaded_prebuilt ? "prebuilt" : "scanned", (unsigned)s_index->total_frames,
                 (unsigned)s_index->duration_ms, (unsigned)s_index->entry_count,
                 (unsigned)s_index->sample_rate, (unsigned)s_index->channels);

        mp3dec_t *dec = heap_caps_calloc(1, sizeof(*dec), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *in = heap_caps_malloc(MUSIC_IN_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int16_t *pcm_base = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (dec == NULL || in == NULL || pcm_base == NULL) {
            set_status(PANEL_MUSIC_ERROR, "内存不足");
            goto cleanup;
        }
        mp3dec_init(dec);
        uint32_t hz = 0;
        uint16_t channels = 0;
        uint32_t nominal_frame_samples = 1152U;
        uint64_t decoded_frames = 0;
        size_t used = 0, pos = 0;
        bool eof = false;
        bool stream = false;
        bool resampler_ready = false;
        panel_music_resampler_t resampler = {0};
        bool ok = true;
        bool discarding = false;
        bool timeline_started = true;
        uint32_t timeline_byte_offset = 0;
        uint32_t frame_file_offset = tag_bytes;
        uint32_t decoded_since_floor = 0;
        uint32_t floor_sample_base = 0;
        uint32_t target_ms = 0;
        uint32_t position_base = 0;
        uint32_t seek_gen = 0;
        bool seeking_active = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        clear_seek_locked();
        xSemaphoreGive(s_lock);

        while (!stop_requested()) {
#ifdef PANEL_MUSIC_HOST_TEST
            s_test_loop_iterations++;
#endif
            uint32_t requested_seek = 0;
            uint32_t requested_gen = 0;
            if (consume_seek_request(&requested_seek, &requested_gen)) {
                seek_gen = requested_gen;
                seeking_active = true;
                panel_mp3_seek_t seek;
                xSemaphoreTake(s_index_lock, portMAX_DELAY);
                bool mapped = panel_mp3_index_seek(s_index, requested_seek, &seek);
                xSemaphoreGive(s_index_lock);
                if (mapped) {
                    char seek_error[64];
                    int frame_ok = tf_validate_frame(f, seek.byte_offset, seek.frame_header,
                                                     seek_error, sizeof(seek_error));
                    if (frame_ok < 0) {
                        set_status(PANEL_MUSIC_ERROR, seek_error);
                        ok = false;
                        break;
                    }
                    if (frame_ok == 0) {
                        seeking_active = false;
                        finish_seek(seek_gen, false, "跳转帧校验失败，继续播放", 0);
                        continue;
                    }
                    if (tf_seek_file(f, seek.byte_offset)) {
                        mp3dec_init(dec);
                        used = 0;
                        pos = 0;
                        eof = false;
                        decoded_since_floor = 0;
                        floor_sample_base = seek.frame_floor_sample;
                        timeline_byte_offset = seek.timeline_byte_offset;
                        frame_file_offset = seek.byte_offset;
                        timeline_started = false;
                        target_ms = seek.target_ms;
                        discarding = true;
                        resampler_ready = false;
                        if (stream) {
                            /* Clear queued PCM and the I2S DMA path before the
                             * new position starts; the codec is reopened only
                             * when the first valid PCM frame is ready. */
                            panel_audio_stream_end();
                            stream = false;
                            if (panel_audio_stream_busy()) {
                                set_status(PANEL_MUSIC_ERROR, "音频流未释放");
                                ok = false;
                                break;
                            }
                        }
                        xSemaphoreTake(s_lock, portMAX_DELAY);
                        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "跳转中");
                        xSemaphoreGive(s_lock);
                        ESP_LOGI(TAG, "seek request=%ums floor=%ums byte=%u",
                                 (unsigned)seek.target_ms, (unsigned)seek.frame_floor_ms,
                                 (unsigned)seek.byte_offset);
                    } else {
                        finish_seek(seek_gen, false, "跳转读取失败", 0);
                        seeking_active = false;
                    }
                } else {
                    finish_seek(seek_gen, false, "跳转超出索引范围", 0);
                    seeking_active = false;
                }
            }

            while (pause_requested() && !stop_requested()) {
                if (seek_request_unseen()) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (stop_requested()) break;
            /* A new request woke the wait: return to the command top to consume
             * the latest request.  A consumed-but-unfinished seek must not make
             * this loop spin; it waits above and completes only after resume. */
            if (pause_requested()) continue;

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
                                                           (int)(used - pos), pcm_base, &info);
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
            uint32_t frame_start_offset = frame_file_offset;
            frame_file_offset += (uint32_t)info.frame_bytes;
            if (!timeline_started) {
                if (frame_start_offset < timeline_byte_offset) continue;
                timeline_started = true;
                decoded_since_floor = 0;
            }
            if (samples <= 0) {
                /* Physical frames (including Info/Xing or reservoir-warmup
                 * frames) still advance the sample timeline even when minimp3
                 * has no PCM to return. */
                if (discarding) decoded_since_floor += nominal_frame_samples;
                continue;
            }
            if (hz == 0U) {
                hz = (uint32_t)info.hz;
                channels = (uint16_t)info.channels;
                nominal_frame_samples = (uint32_t)samples;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.sample_rate = hz;
                s_status.channels = channels;
                xSemaphoreGive(s_lock);
            } else if (info.hz != (int)hz || info.channels != (int)channels) {
                set_status(PANEL_MUSIC_ERROR, "MP3参数变化");
                ok = false;
                break;
            }
            int16_t *pcm_view = pcm_base;
            decoded_frames += (uint64_t)samples;
            if (discarding) {
                uint32_t frame_start_samples = decoded_since_floor;
                decoded_since_floor += (uint32_t)samples;
                uint32_t target_samples =
                    (uint32_t)(((uint64_t)target_ms * hz) / 1000U);
                uint32_t target_local = target_samples > floor_sample_base
                                        ? target_samples - floor_sample_base : 0U;
                if (decoded_since_floor <= target_local) continue;
                uint32_t crop = 0;
                if (frame_start_samples < target_local) {
                    crop = target_local - frame_start_samples;
                }
                if (crop >= (uint32_t)samples) {
                    set_status(PANEL_MUSIC_ERROR, "跳转裁剪失败");
                    ok = false;
                    break;
                }
                pcm_view = pcm_base + (size_t)crop * channels;
                samples -= (int)crop;
                discarding = false;
                position_base = target_ms;
                resampler_ready = false;
            }
            if (pcm_view < pcm_base ||
                pcm_view + (size_t)samples * channels >
                    pcm_base + MINIMP3_MAX_SAMPLES_PER_FRAME) {
                set_status(PANEL_MUSIC_ERROR, "PCM视图越界");
                ok = false;
                break;
            }
            if (!stream) {
                if (seeking_active && !seek_generation_current(seek_gen)) {
                    seeking_active = false;
                    discarding = false;
                    continue;
                }
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
                if (seeking_active) {
                    if (!finish_seek(seek_gen, true, "播放中（已跳转）", position_base)) {
                        panel_audio_stream_end();
                        stream = false;
                        seeking_active = false;
                        discarding = false;
                        continue;
                    }
                    seeking_active = false;
                } else {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    if (s_status.state == PANEL_MUSIC_LOADING) {
                        s_status.state = PANEL_MUSIC_PLAYING;
                    }
                    s_status.position_ms = position_base;
                    snprintf(s_status.detail, sizeof(s_status.detail), "%s",
                             id3 ? "播放中（ID3已跳过）" : "播放中");
                    xSemaphoreGive(s_lock);
                }
                ESP_LOGI(TAG, "MP3 stream: hz=%u ch=%u id3=%d tag=%u",
                         (unsigned)hz, (unsigned)channels, id3 ? 1 : 0, (unsigned)tag_bytes);
            }
            if (!resampler_ready) {
                panel_music_resampler_init(&resampler, hz, channels);
                resampler_ready = true;
            }
            if (!panel_music_resampler_process(&resampler, pcm_view, samples, pcm_sink, NULL)) {
                set_status(PANEL_MUSIC_ERROR, "I2S写入失败");
                ok = false;
                break;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.frames = decoded_frames;
            s_status.position_ms = position_base +
                                   (uint32_t)(resampler.out_frames * 1000U / MUSIC_OUT_HZ);
            if (s_status.duration_ms > 0U &&
                s_status.position_ms > s_status.duration_ms) {
                s_status.position_ms = s_status.duration_ms;
            }
            xSemaphoreGive(s_lock);
        }
        if (ok && !stop_requested() && resampler_ready) {
            if (!panel_music_resampler_flush(&resampler, pcm_sink, NULL)) ok = false;
        }
        if (stream) panel_audio_stream_end();
        if (stop_requested()) {
            set_status(PANEL_MUSIC_STOPPED, "已停止");
        } else if (ok) {
            ESP_LOGI(TAG, "MP3 end: decoded=%llu out=%llu",
                     (unsigned long long)decoded_frames,
                     (unsigned long long)resampler.out_frames);
            advanced = prepare_repeat_after_end();
        } else if (current_state() != PANEL_MUSIC_ERROR) {
            set_status(PANEL_MUSIC_ERROR, "解码失败");
        }
cleanup:
        heap_caps_free(pcm_base);
        heap_caps_free(in);
        heap_caps_free(dec);
        if (f != NULL) {
            close_music_file(f);
            panel_files_mount_pin_release();
        }
        ESP_LOGI(TAG, "music stack total=%u free=%u scratch=%u intdma_free=%u intdma_largest=%u",
                 (unsigned)MUSIC_STACK, (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)mp3dec_scratch_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        if (!advanced && take_auto_restart()) {
            advanced = true;
        }
        if (advanced && !queue_play()) {
            set_status(PANEL_MUSIC_ERROR, "切换失败");
        }
    }
}

esp_err_t panel_music_init(void)
{
    if (s_started) return ESP_OK;
    if (s_init_failed) return ESP_ERR_NO_MEM;
    (void)s_flag_mux;
    s_lock = xSemaphoreCreateMutex();
    s_seek_lock = xSemaphoreCreateMutex();
    s_index_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(MUSIC_QUEUE_DEPTH, sizeof(uint8_t));
    if (s_lock == NULL || s_seek_lock == NULL || s_index_lock == NULL ||
        s_queue == NULL) return ESP_ERR_NO_MEM;
    s_status.state = PANEL_MUSIC_IDLE;
    s_status.track = 0;
    s_selected_track = 0;
    s_status.volume = 20;
    s_status.liked = false;
    s_status.shuffle = false;
    s_status.repeat = false;
    s_shuffle = false;
    s_repeat = false;
    memset(s_liked, 0, sizeof(s_liked));
    select_locked(0);
    snprintf(s_status.detail, sizeof(s_status.detail), "%s", "未播放");

    size_t scratch_bytes = mp3dec_scratch_size();
    s_scratch = heap_caps_malloc(scratch_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_index = heap_caps_calloc(1, sizeof(*s_index), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_scratch == NULL || s_index == NULL) {
        ESP_LOGE(TAG, "scratch/index alloc failed: %u / %u bytes",
                 (unsigned)scratch_bytes, (unsigned)sizeof(*s_index));
        heap_caps_free(s_scratch);
        heap_caps_free(s_index);
        s_scratch = NULL;
        s_index = NULL;
        s_init_failed = true;
        return ESP_ERR_NO_MEM;
    }
    memset(s_scratch, 0, scratch_bytes);
    if (xTaskCreate(worker, "panel_music", MUSIC_STACK, NULL, 4, &s_task) != pdPASS) {
        heap_caps_free(s_scratch);
        heap_caps_free(s_index);
        s_scratch = NULL;
        s_index = NULL;
        s_init_failed = true;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "worker stack=%u scratch_psram=%u index_psram=%u",
             (unsigned)MUSIC_STACK, (unsigned)scratch_bytes, (unsigned)sizeof(*s_index));
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
        int volume = s_status.volume;
        xSemaphoreGive(s_lock);
        panel_audio_stream_set_volume(volume);
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
    select_locked(track);
    s_status.state = PANEL_MUSIC_LOADING;
    s_auto_play = false;
    set_stop(false);
    set_pause(false);
    clear_seek_locked();
    snprintf(s_status.detail, sizeof(s_status.detail), "%s", "启动中");
    xSemaphoreGive(s_lock);
    if (!queue_play()) {
        set_status(PANEL_MUSIC_ERROR, "播放器忙");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void panel_music_pause(void)
{
    if (s_lock == NULL) return;
    bool muted = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state == PANEL_MUSIC_PLAYING) {
        set_pause(true);
        s_status.state = PANEL_MUSIC_PAUSED;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "已暂停");
        muted = true;
    }
    xSemaphoreGive(s_lock);
    if (muted) panel_audio_stream_set_volume(0);
}

void panel_music_resume(void)
{
    if (s_lock == NULL) return;
    int volume = 20;
    bool restore = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state == PANEL_MUSIC_PAUSED) {
        set_pause(false);
        s_status.state = PANEL_MUSIC_PLAYING;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "播放中");
        volume = s_status.volume;
        restore = true;
    }
    xSemaphoreGive(s_lock);
    if (restore) panel_audio_stream_set_volume(volume);
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
        s_status.index_building = false;
        s_status.index_ready = false;
        s_status.index_failed = false;
        bump_generation();
        xSemaphoreGive(s_lock);
        return;
    }
    set_pause(false);
    set_stop(true);
    bump_generation();
    s_auto_play = false;
    s_status.index_building = false;
    s_status.index_ready = false;
    s_status.index_failed = false;
    s_status.state = PANEL_MUSIC_STOPPING;
    clear_seek_locked();
    snprintf(s_status.detail, sizeof(s_status.detail), "%s", "停止中");
    xSemaphoreGive(s_lock);
    panel_audio_stream_set_volume(0);
}

void panel_music_select(uint8_t track)
{
    if (s_lock == NULL) return;
    const panel_music_track_t *meta = panel_music_track_at(track);
    if (meta == NULL) return;
    bool restart = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    select_locked(track);
    if (s_status.state == PANEL_MUSIC_PLAYING || s_status.state == PANEL_MUSIC_PAUSED ||
        s_status.state == PANEL_MUSIC_LOADING) {
        set_pause(false);
        set_stop(true);
        s_auto_play = true;
        s_status.state = PANEL_MUSIC_STOPPING;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "切换曲目");
        restart = true;
    } else if (s_status.state != PANEL_MUSIC_STOPPING) {
        s_status.state = PANEL_MUSIC_STOPPED;
        snprintf(s_status.detail, sizeof(s_status.detail), "%s", "已选择曲目");
    }
    xSemaphoreGive(s_lock);
    if (restart) panel_audio_stream_set_volume(0);
}

void panel_music_next(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t next = chosen_next(s_selected_track);
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
    bool paused = false;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.volume = volume;
        paused = s_status.state == PANEL_MUSIC_PAUSED;
        xSemaphoreGive(s_lock);
    }
    if (!paused) panel_audio_stream_set_volume(volume);
}

esp_err_t panel_music_seek(uint32_t position_ms)
{
    if (s_lock == NULL || !s_started) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state != PANEL_MUSIC_PLAYING && s_status.state != PANEL_MUSIC_PAUSED) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_status.duration_ms == 0U) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_status.index_ready) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t max_ms = s_status.duration_ms > MUSIC_SEEK_END_MARGIN_MS
                      ? s_status.duration_ms - MUSIC_SEEK_END_MARGIN_MS : 0U;
    if (position_ms > max_ms) position_ms = max_ms;
#ifdef PANEL_MUSIC_HOST_TEST
    s_test_last_seek_target = position_ms;
#endif
    s_status.seek_pending = true;
    snprintf(s_status.detail, sizeof(s_status.detail), "%s", "跳转中");
    xSemaphoreTake(s_seek_lock, portMAX_DELAY);
    s_seek_ms = position_ms;
    s_seek_seq++;
    s_seek_pending = true;
    xSemaphoreGive(s_seek_lock);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool panel_music_toggle_like(void)
{
    bool liked = false;
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_liked[s_status.track % PANEL_MUSIC_TRACK_COUNT] =
        !s_liked[s_status.track % PANEL_MUSIC_TRACK_COUNT];
    apply_liked_locked();
    liked = s_status.liked;
    xSemaphoreGive(s_lock);
    return liked;
}

bool panel_music_set_shuffle(bool enabled)
{
    if (s_lock == NULL) return enabled;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_shuffle = enabled;
    s_status.shuffle = enabled;
    xSemaphoreGive(s_lock);
    return enabled;
}

bool panel_music_set_repeat(bool enabled)
{
    if (s_lock == NULL) return enabled;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_repeat = enabled;
    s_status.repeat = enabled;
    xSemaphoreGive(s_lock);
    return enabled;
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

uint32_t panel_music_test_last_seek_target(void)
{
    return s_test_last_seek_target;
}

unsigned panel_music_test_loop_iterations(void)
{
    return s_test_loop_iterations;
}

uint8_t panel_music_test_index_track(void)
{
    uint8_t track = 0xFF;
    if (s_lock == NULL) return track;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.index_ready) track = s_status.track;
    xSemaphoreGive(s_lock);
    return track;
}

void panel_music_test_set_open_delay(uint32_t delay_ms)
{
    s_test_open_delay_ms = delay_ms;
}

void panel_music_test_set_open_error_once(bool enabled)
{
    s_test_open_error_once = enabled;
}

void panel_music_test_set_prebuilt_mismatch(bool enabled)
{
    s_test_force_prebuilt_mismatch = enabled;
}

void panel_music_test_set_prebuilt_io_error(bool enabled)
{
    s_test_force_prebuilt_io_error = enabled;
}

void panel_music_test_set_seek_header_mismatch(bool enabled)
{
    s_test_seek_header_mismatch = enabled;
}

void panel_music_test_set_index_publish_delay(uint32_t delay_ms)
{
    s_test_index_publish_delay_ms = delay_ms;
}

uint8_t panel_music_test_index_source(void)
{
    return s_test_index_source;
}

int panel_music_test_probe_prebuilt(const char *path, uint8_t track)
{
    if (path == NULL) return -1;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    bool id3 = false;
    uint8_t h[10];
    uint32_t tag = 0;
    if (fread(h, 1, sizeof(h), f) == sizeof(h) && memcmp(h, "ID3", 3) == 0) {
        uint32_t size = ((uint32_t)(h[6] & 0x7f) << 21) |
                        ((uint32_t)(h[7] & 0x7f) << 14) |
                        ((uint32_t)(h[8] & 0x7f) << 7) | (uint32_t)(h[9] & 0x7f);
        tag = 10U + size + ((h[5] & 0x10) ? 10U : 0U);
        id3 = true;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char error[64];
    int result = size > 0 ? prebuilt_try_load(f, track, (uint32_t)size, id3, tag,
                                               current_generation(), error,
                                               sizeof(error)) : -1;
    fclose(f);
    return result;
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
