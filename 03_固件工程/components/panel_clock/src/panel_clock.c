#include "panel_clock.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "panel_clock_platform.h"
#include "panel_online.h"

static const char *TAG = "panel_clock";

#define CLOCK_NAMESPACE "panel_clock"
#define CLOCK_ALARM_KEY "alarm"
#define CLOCK_ALARM_MAGIC 0x50434C4BUL
#define CLOCK_ALARM_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t enabled;
    uint8_t hour;
    uint8_t minute;
    uint8_t reserved[3];
} alarm_blob_t;

typedef enum {
    CMD_STOPWATCH_START = 1,
    CMD_STOPWATCH_PAUSE,
    CMD_STOPWATCH_RESET,
    CMD_COUNTDOWN_START,
    CMD_COUNTDOWN_PAUSE,
    CMD_COUNTDOWN_RESUME,
    CMD_COUNTDOWN_RESET,
    CMD_COUNTDOWN_ACK,
    CMD_ALARM_SET,
    CMD_ALARM_ACK,
} clock_cmd_type_t;

typedef struct {
    clock_cmd_type_t type;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
} clock_cmd_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static bool s_started;
static panel_clock_snapshot_t s_snapshot;

static bool s_sw_running;
static uint32_t s_sw_base_ms;
static int64_t s_sw_started_us;

static bool s_cd_running;
static uint32_t s_cd_remaining_at_start_ms;
static int64_t s_cd_started_us;
static bool s_cd_done;
static uint32_t s_cd_done_generation;

static bool s_alarm_enabled;
static uint8_t s_alarm_hour = 7;
static uint8_t s_alarm_minute;
static int s_alarm_last_date;
static bool s_alarm_pending;
static uint32_t s_alarm_generation;
static bool s_alarm_persist_failed;
static bool s_prev_valid;
static int s_prev_date;
static int s_prev_minute;
static int64_t s_prev_epoch;
static uint32_t s_generation;

static void clock_lock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void clock_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

static uint32_t sat_add_ms(uint32_t base, uint32_t elapsed)
{
    if (UINT32_MAX - base < elapsed) {
        return UINT32_MAX;
    }
    return base + elapsed;
}

static uint32_t elapsed_ms(int64_t start_us, int64_t now_us)
{
    if (now_us <= start_us) {
        return 0;
    }
    uint64_t ms = (uint64_t)(now_us - start_us) / 1000U;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static bool local_time_now(int64_t *epoch_out, struct tm *tm_out)
{
    int64_t epoch = panel_clock_platform_epoch();
    if (epoch <= 0 || tm_out == NULL) {
        return false;
    }
    time_t value = (time_t)epoch;
    struct tm *tmp = localtime(&value);
    if (tmp == NULL) {
        return false;
    }
    *tm_out = *tmp;
    if (epoch_out != NULL) {
        *epoch_out = epoch;
    }
    return true;
}

static bool save_alarm_values(bool enabled, uint8_t hour, uint8_t minute)
{
    nvs_handle_t handle;
    if (nvs_open(CLOCK_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    alarm_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = CLOCK_ALARM_MAGIC;
    blob.version = CLOCK_ALARM_VERSION;
    blob.enabled = enabled ? 1U : 0U;
    blob.hour = hour;
    blob.minute = minute;
    esp_err_t err = nvs_set_blob(handle, CLOCK_ALARM_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "alarm save failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void load_alarm(void)
{
    nvs_handle_t handle;
    if (nvs_open(CLOCK_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    alarm_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(handle, CLOCK_ALARM_KEY, &blob, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return;
    }
    if (len != sizeof(blob) || blob.magic != CLOCK_ALARM_MAGIC ||
        blob.version != CLOCK_ALARM_VERSION || blob.enabled > 1 ||
        blob.hour > 23 || blob.minute > 59) {
        ESP_LOGW(TAG, "saved alarm invalid, using disabled default");
        return;
    }
    s_alarm_enabled = blob.enabled != 0;
    s_alarm_hour = blob.hour;
    s_alarm_minute = blob.minute;
}

static void countdown_finish_locked(void)
{
    if (!s_cd_done) {
        s_cd_done = true;
        s_cd_done_generation++;
    }
    s_cd_running = false;
    s_cd_remaining_at_start_ms = 0;
}

static void countdown_tick_locked(void)
{
    if (!s_cd_running) {
        return;
    }
    uint32_t elapsed = elapsed_ms(s_cd_started_us, panel_clock_platform_monotonic_us());
    if (elapsed >= s_cd_remaining_at_start_ms) {
        countdown_finish_locked();
    }
}

static void update_snapshot_locked(void)
{
    int64_t epoch = 0;
    struct tm tm_now;
    bool calibrated = panel_online_time_is_calibrated();
    bool valid = calibrated && local_time_now(&epoch, &tm_now);

    if (valid) {
        s_snapshot.valid = true;
        s_snapshot.calibrated = true;
        s_snapshot.year = tm_now.tm_year + 1900;
        s_snapshot.month = tm_now.tm_mon + 1;
        s_snapshot.day = tm_now.tm_mday;
        s_snapshot.hour = tm_now.tm_hour;
        s_snapshot.minute = tm_now.tm_min;
        s_snapshot.second = tm_now.tm_sec;
        s_snapshot.weekday = (tm_now.tm_wday + 6) % 7;

        int minute_of_day = s_snapshot.hour * 60 + s_snapshot.minute;
        int date_key = s_snapshot.year * 10000 + s_snapshot.month * 100 + s_snapshot.day;
        bool same_date_next = s_prev_valid && date_key == s_prev_date &&
                              s_prev_minute + 1 == minute_of_day;
        bool midnight_step = s_prev_valid && date_key > s_prev_date &&
                             s_prev_minute == 23 * 60 + 59 && minute_of_day == 0 &&
                             epoch > s_prev_epoch && epoch - s_prev_epoch <= 120;
        bool crossed_minute = same_date_next || midnight_step;
        if (s_alarm_enabled && minute_of_day == s_alarm_hour * 60 + s_alarm_minute &&
            crossed_minute && date_key > s_alarm_last_date) {
            s_alarm_last_date = date_key;
            s_alarm_pending = true;
            s_alarm_generation++;
        }
        s_prev_valid = true;
        s_prev_date = date_key;
        s_prev_minute = minute_of_day;
        s_prev_epoch = epoch;
    } else {
        s_prev_valid = false;
        s_snapshot.valid = false;
        s_snapshot.calibrated = false;
        s_snapshot.year = 0;
        s_snapshot.month = 0;
        s_snapshot.day = 0;
        s_snapshot.hour = 0;
        s_snapshot.minute = 0;
        s_snapshot.second = 0;
        s_snapshot.weekday = 0;
    }

    if (s_sw_running) {
        s_snapshot.stopwatch_ms =
            sat_add_ms(s_sw_base_ms, elapsed_ms(s_sw_started_us,
                                                panel_clock_platform_monotonic_us()));
    } else {
        s_snapshot.stopwatch_ms = s_sw_base_ms;
    }
    s_snapshot.stopwatch_running = s_sw_running;

    if (s_cd_running) {
        uint32_t elapsed = elapsed_ms(s_cd_started_us, panel_clock_platform_monotonic_us());
        s_snapshot.countdown_remaining_ms =
            elapsed >= s_cd_remaining_at_start_ms ? 0 : s_cd_remaining_at_start_ms - elapsed;
    } else {
        s_snapshot.countdown_remaining_ms = s_cd_remaining_at_start_ms;
    }
    s_snapshot.countdown_running = s_cd_running;
    s_snapshot.countdown_done = s_cd_done;
    s_snapshot.countdown_done_generation = s_cd_done_generation;
    s_snapshot.alarm_enabled = s_alarm_enabled;
    s_snapshot.alarm_hour = s_alarm_hour;
    s_snapshot.alarm_minute = s_alarm_minute;
    s_snapshot.alarm_pending = s_alarm_pending;
    s_snapshot.alarm_generation = s_alarm_generation;
    s_snapshot.alarm_persist_failed = s_alarm_persist_failed;
    s_snapshot.generation = ++s_generation;
}

static void handle_command(const clock_cmd_t *cmd)
{
    if (cmd == NULL) {
        return;
    }
    int64_t now_us = panel_clock_platform_monotonic_us();
    bool save_needed = false;
    bool save_enabled = false;
    uint8_t save_hour = 0;
    uint8_t save_minute = 0;
    clock_lock();
    switch (cmd->type) {
    case CMD_STOPWATCH_START:
        if (!s_sw_running) {
            s_sw_running = true;
            s_sw_started_us = now_us;
        }
        break;
    case CMD_STOPWATCH_PAUSE:
        if (s_sw_running) {
            s_sw_base_ms = sat_add_ms(s_sw_base_ms, elapsed_ms(s_sw_started_us, now_us));
            s_sw_running = false;
        }
        break;
    case CMD_STOPWATCH_RESET:
        s_sw_running = false;
        s_sw_base_ms = 0;
        s_sw_started_us = 0;
        break;
    case CMD_COUNTDOWN_START:
        if (cmd->arg0 >= 1 && cmd->arg0 <= PANEL_CLOCK_COUNTDOWN_MAX_SECONDS) {
            s_cd_remaining_at_start_ms = cmd->arg0 * 1000U;
            s_cd_started_us = now_us;
            s_cd_running = true;
            s_cd_done = false;
        }
        break;
    case CMD_COUNTDOWN_PAUSE:
        if (s_cd_running) {
            uint32_t elapsed = elapsed_ms(s_cd_started_us, now_us);
            s_cd_remaining_at_start_ms =
                elapsed >= s_cd_remaining_at_start_ms ? 0 : s_cd_remaining_at_start_ms - elapsed;
            s_cd_running = false;
            if (s_cd_remaining_at_start_ms == 0) {
                countdown_finish_locked();
            }
        }
        break;
    case CMD_COUNTDOWN_RESUME:
        if (!s_cd_running && s_cd_remaining_at_start_ms > 0) {
            s_cd_started_us = now_us;
            s_cd_running = true;
        }
        break;
    case CMD_COUNTDOWN_RESET:
        s_cd_running = false;
        s_cd_remaining_at_start_ms = 0;
        s_cd_done = false;
        break;
    case CMD_COUNTDOWN_ACK:
        if (cmd->arg0 == s_cd_done_generation) {
            s_cd_done = false;
        }
        break;
    case CMD_ALARM_SET:
        if (cmd->arg1 <= 23 && cmd->arg2 <= 59) {
            bool enabled = cmd->arg0 != 0;
            uint8_t hour = (uint8_t)cmd->arg1;
            uint8_t minute = (uint8_t)cmd->arg2;
            if (enabled != s_alarm_enabled || hour != s_alarm_hour ||
                minute != s_alarm_minute || s_alarm_persist_failed) {
                s_alarm_enabled = enabled;
                s_alarm_hour = hour;
                s_alarm_minute = minute;
                save_needed = true;
                save_enabled = enabled;
                save_hour = hour;
                save_minute = minute;
            }
        }
        break;
    case CMD_ALARM_ACK:
        if (cmd->arg0 == s_alarm_generation) {
            s_alarm_pending = false;
        }
        break;
    default:
        break;
    }
    countdown_tick_locked();
    update_snapshot_locked();
    clock_unlock();

    if (save_needed) {
        bool saved = save_alarm_values(save_enabled, save_hour, save_minute);
        clock_lock();
        s_alarm_persist_failed = !saved;
        update_snapshot_locked();
        clock_unlock();
    }
}

static void clock_worker(void *arg)
{
    (void)arg;
    clock_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            handle_command(&cmd);
        } else {
            clock_lock();
            countdown_tick_locked();
            update_snapshot_locked();
            clock_unlock();
        }
    }
}

static esp_err_t send_command(clock_cmd_type_t type, uint32_t a0, uint32_t a1, uint32_t a2)
{
    if (!s_started || s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    clock_cmd_t cmd = {.type = type, .arg0 = a0, .arg1 = a1, .arg2 = a2};
    return xQueueSend(s_queue, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t panel_clock_init(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_queue == NULL) {
        s_queue = xQueueCreate(8, sizeof(clock_cmd_t));
    }
    if (s_lock == NULL || s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    load_alarm();
    clock_lock();
    update_snapshot_locked();
    clock_unlock();
    if (xTaskCreate(clock_worker, "panel_clock", 4096, NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

void panel_clock_get_snapshot(panel_clock_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    clock_lock();
    *out = s_snapshot;
    clock_unlock();
}

esp_err_t panel_clock_stopwatch_start(void)
{
    return send_command(CMD_STOPWATCH_START, 0, 0, 0);
}

esp_err_t panel_clock_stopwatch_pause(void)
{
    return send_command(CMD_STOPWATCH_PAUSE, 0, 0, 0);
}

esp_err_t panel_clock_stopwatch_reset(void)
{
    return send_command(CMD_STOPWATCH_RESET, 0, 0, 0);
}

esp_err_t panel_clock_countdown_start(uint32_t seconds)
{
    if (seconds < 1 || seconds > PANEL_CLOCK_COUNTDOWN_MAX_SECONDS) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_command(CMD_COUNTDOWN_START, seconds, 0, 0);
}

esp_err_t panel_clock_countdown_pause(void)
{
    return send_command(CMD_COUNTDOWN_PAUSE, 0, 0, 0);
}

esp_err_t panel_clock_countdown_resume(void)
{
    return send_command(CMD_COUNTDOWN_RESUME, 0, 0, 0);
}

esp_err_t panel_clock_countdown_reset(void)
{
    return send_command(CMD_COUNTDOWN_RESET, 0, 0, 0);
}

esp_err_t panel_clock_countdown_ack(uint32_t generation)
{
    return send_command(CMD_COUNTDOWN_ACK, generation, 0, 0);
}

esp_err_t panel_clock_alarm_set(bool enabled, uint8_t hour, uint8_t minute)
{
    if (hour > 23 || minute > 59) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_command(CMD_ALARM_SET, enabled ? 1 : 0, hour, minute);
}

esp_err_t panel_clock_alarm_ack(uint32_t generation)
{
    return send_command(CMD_ALARM_ACK, generation, 0, 0);
}
