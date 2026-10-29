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
    uint32_t duration_ms;
    bool liked;
    bool shuffle;
    bool repeat;
    bool seek_pending;
    bool index_ready;
    bool index_building;
    bool index_failed;
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

/* Real byte-index seek.  The request is consumed by the single audio worker;
 * it restarts at a sparse frame boundary, decodes pre-roll frames to rebuild
 * the MP3 bit reservoir, and only reports a new time after validation. */
esp_err_t panel_music_seek(uint32_t position_ms);

/* Session state; toggles take effect immediately and apply to real tracks. */
bool panel_music_toggle_like(void);
bool panel_music_set_shuffle(bool enabled);
bool panel_music_set_repeat(bool enabled);

#ifdef PANEL_MUSIC_HOST_TEST
uint8_t panel_music_test_active_track(void);
uint32_t panel_music_test_last_seek_target(void);
unsigned panel_music_test_loop_iterations(void);
uint8_t panel_music_test_index_track(void);
void panel_music_test_set_open_delay(uint32_t delay_ms);
void panel_music_test_set_open_error_once(bool enabled);
void panel_music_test_set_prebuilt_mismatch(bool enabled);
void panel_music_test_set_prebuilt_io_error(bool enabled);
void panel_music_test_set_seek_header_mismatch(bool enabled);
void panel_music_test_set_index_publish_delay(uint32_t delay_ms);
uint8_t panel_music_test_index_source(void);
int panel_music_test_probe_prebuilt(const char *path, uint8_t track);
#endif
