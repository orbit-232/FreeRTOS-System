#include "panel_audio_tone.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

size_t panel_audio_tone_frames(void)
{
    return (size_t)((uint64_t)PANEL_AUDIO_SAMPLE_RATE * PANEL_AUDIO_TONE_MS / 1000U);
}

void panel_audio_fill_tone(int16_t *out, size_t frames, uint32_t *sample_index)
{
    if (out == NULL || sample_index == NULL) return;
    for (size_t i = 0; i < frames; i++) {
        double t = (double)(*sample_index) / (double)PANEL_AUDIO_SAMPLE_RATE;
        out[i] = (int16_t)(sin(2.0 * M_PI * (double)PANEL_AUDIO_TONE_HZ * t) *
                           PANEL_AUDIO_AMPLITUDE);
        (*sample_index)++;
    }
}
