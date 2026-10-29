#include "panel_audio_tone.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    const size_t total = panel_audio_tone_frames();
    CHECK(total == 17640U);
    CHECK(PANEL_AUDIO_TEST_VOLUME == 20);
    CHECK(PANEL_AUDIO_TONE_MS == 800U);

    uint32_t index = 0;
    int peak = 0;
    long long energy = 0;
    size_t done = 0;
    while (done < total) {
        size_t frames = total - done;
        if (frames > 256) frames = 256;
        int16_t pcm[256];
        panel_audio_fill_tone(pcm, frames, &index);
        for (size_t i = 0; i < frames; i++) {
            int v = abs((int)pcm[i]);
            if (v > peak) peak = v;
            energy += (long long)pcm[i] * pcm[i];
        }
        done += frames;
    }
    CHECK(index == total);
    CHECK(peak > PANEL_AUDIO_AMPLITUDE / 2);
    CHECK(peak <= PANEL_AUDIO_AMPLITUDE);
    CHECK(peak < 32767 / 8); /* deliberately low, not full scale */
    CHECK(energy > 0);
    printf("AUDIO_TONE_TESTS_OK frames=%u peak=%d volume=%d\n",
           (unsigned)total, peak, PANEL_AUDIO_TEST_VOLUME);
    return 0;
}
