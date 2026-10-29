#include "panel_music_resample.h"

#include <stddef.h>

void panel_music_resampler_init(panel_music_resampler_t *r, uint32_t hz, uint16_t channels)
{
    if (r == NULL) return;
    r->hz = hz;
    r->channels = channels;
    r->step = (double)hz / (double)PANEL_MUSIC_OUT_HZ;
    r->pos = 0.0;
    r->out_frames = 0;
    r->out_n = 0;
}

bool panel_music_resampler_process(panel_music_resampler_t *r, const int16_t *pcm, int frames,
                                   panel_music_sink_t sink, void *ctx)
{
    if (r == NULL || pcm == NULL || sink == NULL || frames <= 0) return false;
    while (r->pos < (double)frames) {
        int i = (int)r->pos;
        int j = i + 1 < frames ? i + 1 : i;
        double frac = r->pos - (double)i;
        int32_t v;
        if (r->channels == 2) {
            int32_t a = ((int32_t)pcm[i * 2] + pcm[i * 2 + 1]) / 2;
            int32_t b = ((int32_t)pcm[j * 2] + pcm[j * 2 + 1]) / 2;
            v = (int32_t)((double)a * (1.0 - frac) + (double)b * frac);
        } else {
            v = (int32_t)((double)pcm[i] * (1.0 - frac) + (double)pcm[j] * frac);
        }
        r->out[r->out_n++] = (int16_t)v;
        r->out_frames++;
        r->pos += r->step;
        if (r->out_n == PANEL_MUSIC_RESAMPLE_CHUNK) {
            if (!sink(ctx, r->out, r->out_n)) return false;
            r->out_n = 0;
        }
    }
    r->pos -= (double)frames;
    return true;
}

bool panel_music_resampler_flush(panel_music_resampler_t *r, panel_music_sink_t sink, void *ctx)
{
    if (r == NULL || sink == NULL) return false;
    if (r->out_n == 0) return true;
    bool ok = sink(ctx, r->out, r->out_n);
    r->out_n = 0;
    return ok;
}
