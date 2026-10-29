#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    PANEL_AUDIO_IDLE = 0,
    PANEL_AUDIO_QUEUED,
    PANEL_AUDIO_PLAYING,
    PANEL_AUDIO_DONE,
    PANEL_AUDIO_ERROR,
} panel_audio_state_t;

typedef struct {
    panel_audio_state_t state;
    uint8_t source; /* 0 = internal tone, 1 = TF WAV */
    int volume;
    uint32_t duration_ms;
    uint32_t frames;
    char detail[96];
} panel_audio_status_t;

/* Creates the control task only; codec/I2S/PA remain off until an explicit
 * panel_audio_test_start(). */
esp_err_t panel_audio_init(void);
esp_err_t panel_audio_test_start(void);
esp_err_t panel_audio_test_wav_start(void);
void panel_audio_get_status(panel_audio_status_t *out);

/* Shared 22050 Hz mono output. The music player holds this lock for a whole
 * song; test commands must reject while it is held. */
bool panel_audio_stream_begin(void);
bool panel_audio_stream_write(const int16_t *pcm, size_t frames);
void panel_audio_stream_set_volume(int volume);
void panel_audio_stream_end(void);
bool panel_audio_stream_busy(void);
