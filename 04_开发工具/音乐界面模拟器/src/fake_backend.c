#include "panel_music.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel_music_tracks.h"

#define DURATION_0 361090U
#define DURATION_1 230530U
#define DURATION_2 269244U
#define DURATION_3 200124U

static panel_music_state_t s_state = PANEL_MUSIC_PLAYING;
static uint8_t s_track;
static int s_volume = 20;
static bool s_liked[4];
static bool s_shuffle;
static bool s_repeat;
static uint32_t s_position = 72000U;
static bool s_index_ready = true;
static bool s_index_building;
static bool s_index_failed;

static uint32_t duration_for(unsigned track)
{
    static const uint32_t durations[4] = {DURATION_0, DURATION_1, DURATION_2, DURATION_3};
    return durations[track % 4U];
}

static bool env_bool(const char *name, bool fallback)
{
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') return fallback;
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "on") == 0;
}

void music_fake_init_from_env(void)
{
    const char *state = getenv("WS_MUSIC_STATE");
    s_state = PANEL_MUSIC_PLAYING;
    if (state != NULL) {
        if (strcmp(state, "paused") == 0) s_state = PANEL_MUSIC_PAUSED;
        else if (strcmp(state, "idle") == 0) s_state = PANEL_MUSIC_IDLE;
        else if (strcmp(state, "stopped") == 0) s_state = PANEL_MUSIC_STOPPED;
        else if (strcmp(state, "error") == 0) s_state = PANEL_MUSIC_ERROR;
    }
    const char *track = getenv("WS_MUSIC_TRACK");
    s_track = track != NULL ? (uint8_t)atoi(track) : 0U;
    if (s_track >= 4U) s_track = 0U;
    const char *pos = getenv("WS_MUSIC_POS_MS");
    s_position = pos != NULL ? (uint32_t)strtoul(pos, NULL, 10) : 72000U;
    const char *volume = getenv("WS_MUSIC_VOLUME");
    s_volume = volume != NULL ? atoi(volume) : 20;
    s_shuffle = env_bool("WS_MUSIC_SHUFFLE", false);
    s_repeat = env_bool("WS_MUSIC_REPEAT", false);
    s_liked[s_track] = env_bool("WS_MUSIC_LIKED", false);
    const char *index = getenv("WS_MUSIC_INDEX");
    s_index_building = index != NULL && strcmp(index, "building") == 0;
    s_index_failed = index != NULL && strcmp(index, "failed") == 0;
    s_index_ready = index == NULL || (!s_index_building && !s_index_failed);
}

void music_fake_set_error_building(void)
{
    s_state = PANEL_MUSIC_ERROR;
    s_index_ready = false;
    s_index_building = true;
    s_index_failed = false;
}

void music_fake_set_stopped_building(void)
{
    s_state = PANEL_MUSIC_STOPPED;
    s_index_ready = false;
    s_index_building = true;
    s_index_failed = false;
}

void panel_music_get_status(panel_music_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    const panel_music_track_t *meta = panel_music_track_at(s_track);
    out->state = s_state;
    out->track = s_track;
    out->volume = s_volume;
    out->sample_rate = 44100U;
    out->channels = 2U;
    out->frames = (uint64_t)s_position * 44100U / 1000U;
    out->position_ms = s_position;
    out->duration_ms = duration_for(s_track);
    out->liked = s_liked[s_track];
    out->shuffle = s_shuffle;
    out->repeat = s_repeat;
    out->index_ready = s_index_ready;
    out->index_building = s_index_building;
    out->index_failed = s_index_failed;
    if (meta != NULL) {
        snprintf(out->title, sizeof(out->title), "%s", meta->title);
        snprintf(out->artist, sizeof(out->artist), "%s", meta->artist);
        snprintf(out->file, sizeof(out->file), "/panel/music/%s", meta->file);
    }
    snprintf(out->detail, sizeof(out->detail), "%s",
             s_state == PANEL_MUSIC_ERROR ? "找不到音乐文件" :
             s_state == PANEL_MUSIC_STOPPED ? "已停止" : "播放中");
}

void panel_music_toggle(void)
{
    s_state = s_state == PANEL_MUSIC_PLAYING ? PANEL_MUSIC_PAUSED : PANEL_MUSIC_PLAYING;
}

void panel_music_pause(void)
{
    s_state = PANEL_MUSIC_PAUSED;
}

void panel_music_resume(void)
{
    s_state = PANEL_MUSIC_PLAYING;
}

void panel_music_stop(void)
{
    s_state = PANEL_MUSIC_STOPPED;
}

void panel_music_select(uint8_t track)
{
    if (track < 4U) {
        s_track = track;
        s_position = 0;
    }
}

void panel_music_next(void)
{
    s_track = (uint8_t)((s_track + (s_shuffle ? 2U : 1U)) % 4U);
    s_position = 0;
    s_state = PANEL_MUSIC_PLAYING;
}

void panel_music_prev(void)
{
    s_track = (uint8_t)((s_track + 3U) % 4U);
    s_position = 0;
    s_state = PANEL_MUSIC_PLAYING;
}

void panel_music_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
}

esp_err_t panel_music_play(void)
{
    s_state = PANEL_MUSIC_PLAYING;
    return ESP_OK;
}

esp_err_t panel_music_seek(uint32_t position_ms)
{
    if (position_ms >= duration_for(s_track)) return ESP_ERR_INVALID_ARG;
    s_position = position_ms;
    return ESP_OK;
}

bool panel_music_toggle_like(void)
{
    s_liked[s_track] = !s_liked[s_track];
    return s_liked[s_track];
}

bool panel_music_set_shuffle(bool enabled)
{
    s_shuffle = enabled;
    return enabled;
}

bool panel_music_set_repeat(bool enabled)
{
    s_repeat = enabled;
    return enabled;
}

bool panel_music_is_active(void)
{
    return true;
}
