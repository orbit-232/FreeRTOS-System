#pragma once

#include <stddef.h>
#include <stdint.h>

#define PANEL_AUDIO_SAMPLE_RATE 22050U
#define PANEL_AUDIO_TONE_MS 800U
#define PANEL_AUDIO_TONE_HZ 660U
#define PANEL_AUDIO_AMPLITUDE 3000
#define PANEL_AUDIO_TEST_VOLUME 20

size_t panel_audio_tone_frames(void);
void panel_audio_fill_tone(int16_t *out, size_t frames, uint32_t *sample_index);
