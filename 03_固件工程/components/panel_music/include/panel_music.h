#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    PANEL_MUSIC_IDLE = 0,
    PANEL_MUSIC_LOADING,
    PANEL_MUSIC_PLAYING,
    PANEL_MUSIC_PAUSED,
    PANEL_MUSIC_STOPPING,
    PANEL_MUSIC_STOPPED,
    PANEL_MUSIC_ERROR,
} panel_music_state_t;

typedef struct {
    panel_music_state_t state;
    uint8_t track;
    int volume;
    uint32_t sample_rate;
    uint16_t channels;
    uint64_t frames;
    uint32_t position_ms;
    char title[64];
    char artist[64];
    char file[64];
    char detail[96];
} panel_music_status_t;

esp_err_t panel_music_init(void);
esp_err_t panel_music_play(void);
void panel_music_pause(void);
void panel_music_resume(void);
void panel_music_toggle(void);
void panel_music_stop(void);
void panel_music_select(uint8_t track);
void panel_music_next(void);
void panel_music_prev(void);
void panel_music_set_volume(int volume);
void panel_music_get_status(panel_music_status_t *out);
bool panel_music_is_active(void);

#ifdef PANEL_MUSIC_HOST_TEST
uint8_t panel_music_test_active_track(void);
#endif
