#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PANEL_MUSIC_OUT_HZ 22050U
#define PANEL_MUSIC_RESAMPLE_CHUNK 512U

typedef struct {
    uint32_t hz;
    uint16_t channels;
    double step;
    double pos;
    uint64_t out_frames;
    size_t out_n;
    int16_t out[PANEL_MUSIC_RESAMPLE_CHUNK];
} panel_music_resampler_t;

typedef bool (*panel_music_sink_t)(void *ctx, const int16_t *pcm, size_t frames);

void panel_music_resampler_init(panel_music_resampler_t *r, uint32_t hz, uint16_t channels);
bool panel_music_resampler_process(panel_music_resampler_t *r, const int16_t *pcm, int frames,
                                   panel_music_sink_t sink, void *ctx);
bool panel_music_resampler_flush(panel_music_resampler_t *r, panel_music_sink_t sink, void *ctx);
