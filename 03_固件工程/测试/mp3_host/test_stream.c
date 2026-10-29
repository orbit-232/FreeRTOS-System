#include "minimp3.h"
#include "panel_music_resample.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IN_BUF (32 * 1024)
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

typedef struct {
    uint64_t frames;
    uint64_t chunks;
    int stop_after;
} sink_t;

static bool sink(void *ctx, const int16_t *pcm, size_t frames)
{
    sink_t *s = ctx;
    (void)pcm;
    s->frames += frames;
    s->chunks++;
    if (s->stop_after > 0 && s->chunks >= (uint64_t)s->stop_after) return false;
    return true;
}

static int skip_id3(FILE *f)
{
    uint8_t h[10];
    if (fread(h, 1, 10, f) != 10) return -1;
    if (memcmp(h, "ID3", 3) != 0) {
        fseek(f, 0, SEEK_SET);
        return 0;
    }
    uint32_t size = ((uint32_t)(h[6] & 0x7f) << 21) | ((uint32_t)(h[7] & 0x7f) << 14) |
                    ((uint32_t)(h[8] & 0x7f) << 7) | (uint32_t)(h[9] & 0x7f);
    return fseek(f, (long)(10 + size + ((h[5] & 0x10) ? 10 : 0)), SEEK_SET) == 0 ? 1 : -1;
}

static int decode(const char *path, int stop_after, uint64_t *input_frames,
                  uint64_t *output_frames, int *rate)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    CHECK(skip_id3(f) >= 0);
    mp3dec_t *dec = calloc(1, sizeof(*dec));
    size_t scratch_bytes = mp3dec_scratch_size();
    mp3dec_scratch_t *scratch = malloc(scratch_bytes);
    uint8_t *in = malloc(IN_BUF);
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    CHECK(dec != NULL && scratch != NULL && in != NULL);
    mp3dec_init(dec);
    panel_music_resampler_t r = {0};
    bool ready = false;
    sink_t s = {.stop_after = stop_after};
    size_t used = 0, pos = 0;
    int eof = 0;
    uint64_t in_frames = 0;
    for (;;) {
        if (pos > 0 && pos < used) {
            memmove(in, in + pos, used - pos);
            used -= pos;
            pos = 0;
        } else if (pos == used) {
            used = 0;
            pos = 0;
        }
        if (!eof && used < IN_BUF) {
            size_t n = fread(in + used, 1, IN_BUF - used, f);
            used += n;
            if (n == 0) eof = 1;
        }
        if (used == 0 && eof) break;
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame_with_scratch(dec, scratch, in + pos,
                                                       (int)(used - pos), pcm, &info);
        if (info.frame_bytes <= 0) {
            if (eof) break;
            CHECK(used < IN_BUF);
            continue;
        }
        if ((size_t)info.frame_bytes > used - pos) {
            if (eof) break;
            CHECK(used < IN_BUF);
            continue;
        }
        pos += (size_t)info.frame_bytes;
        if (samples <= 0) continue;
        if (!ready) {
            *rate = info.hz;
            panel_music_resampler_init(&r, (uint32_t)info.hz, (uint16_t)info.channels);
            ready = true;
        }
        in_frames += (uint64_t)samples;
        if (!panel_music_resampler_process(&r, pcm, samples, sink, &s)) break;
    }
    if (ready) (void)panel_music_resampler_flush(&r, sink, &s);
    fclose(f);
    free(scratch);
    free(in);
    free(dec);
    *input_frames = in_frames;
    *output_frames = r.out_frames;
    return 0;
}

static int make_prefix(const char *src, const char *dst, size_t limit)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    CHECK(in != NULL && out != NULL);
    uint8_t buf[4096];
    size_t total = 0;
    while (total < limit) {
        size_t want = limit - total;
        if (want > sizeof(buf)) want = sizeof(buf);
        size_t n = fread(buf, 1, want, in);
        if (n == 0) break;
        CHECK(fwrite(buf, 1, n, out) == n);
        total += n;
    }
    fclose(in);
    fclose(out);
    return total > 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    uint64_t in_frames = 0, out_frames = 0;
    int rate = 0;
    CHECK(decode(argv[1], 0, &in_frames, &out_frames, &rate) == 0);
    CHECK(rate == 44100);
    CHECK(in_frames > 15000000);
    CHECK(out_frames > 7000000);
    CHECK(out_frames * 44100 / 22050 == in_frames);

    uint64_t stopped_in = 0, stopped_out = 0;
    int stopped_rate = 0;
    CHECK(decode(argv[1], 3, &stopped_in, &stopped_out, &stopped_rate) == 0);
    CHECK(stopped_out < out_frames);
    char bad_path[512];
    snprintf(bad_path, sizeof(bad_path), "%s.truncated", argv[1]);
    CHECK(make_prefix(argv[1], bad_path, 1024 * 1024) == 0);
    uint64_t bad_in = 0, bad_out = 0;
    int bad_rate = 0;
    CHECK(decode(bad_path, 0, &bad_in, &bad_out, &bad_rate) == 0);
    remove(bad_path);
    printf("MP3_STREAM_TESTS_OK rate=%d in_frames=%llu out_frames=%llu stopped_out=%llu bad_in=%llu\n",
           rate, (unsigned long long)in_frames, (unsigned long long)out_frames,
           (unsigned long long)stopped_out, (unsigned long long)bad_in);
    return 0;
}
