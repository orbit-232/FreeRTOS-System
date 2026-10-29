#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_SNTP_OPMODE_POLL,
    ESP_SNTP_OPMODE_LISTENONLY,
} esp_sntp_operatingmode_t;

typedef enum {
    SNTP_SYNC_MODE_IMMED,
    SNTP_SYNC_MODE_SMOOTH,
} sntp_sync_mode_t;

typedef void (*sntp_sync_time_cb_t)(struct timeval *tv);

void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t mode);
void esp_sntp_set_sync_mode(sntp_sync_mode_t mode);
void esp_sntp_setservername(uint8_t idx, const char *server);
void esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t cb);
void esp_sntp_set_sync_interval(uint32_t interval_ms);
void esp_sntp_init(void);
void esp_sntp_stop(void);
bool esp_sntp_restart(void);

#ifdef __cplusplus
}
#endif
