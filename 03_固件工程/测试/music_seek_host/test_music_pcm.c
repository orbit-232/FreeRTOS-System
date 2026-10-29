#include "minimp3.h"
#include "panel_music_index.h"
#include "panel_music_prebuilt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IN_BUF (64U * 1024U)
#define COMPARE_FRAMES 4096U
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

typedef struct {
    FILE *file;
    mp3dec_t dec;
    mp3dec_scratch_t *scratch;
    uint8_t *in;
    size_t used;
    size_t pos;
    bool eof;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
} decoder_t;

static bool decoder_open(decoder_t *d, FILE *file, long offset)
{
    memset(d, 0, sizeof(*d));
    d->file = file;
    if (fseek(file, offset, SEEK_SET) != 0) return false;
    d->scratch = malloc(mp3dec_scratch_size());
    d->in = malloc(IN_BUF);
    if (d->scratch == NULL || d->in == NULL) return false;
    mp3dec_init(&d->dec);
    return true;
}

static void decoder_close(decoder_t *d)
{
    free(d->scratch);
    free(d->in);
    d->scratch = NULL;
    d->in = NULL;
}

/* Returns false only at a real end of stream; samples==0 frames are skipped. */
static bool decoder_next(decoder_t *d, int16_t **pcm, int *samples,
                         mp3dec_frame_info_t *info_out)
{
    for (;;) {
        if (d->pos == d->used) {
            d->used = 0;
            d->pos = 0;
        } else if (d->pos > 0) {
            memmove(d->in, d->in + d->pos, d->used - d->pos);
            d->used -= d->pos;
            d->pos = 0;
        }
        if (!d->eof && d->used < IN_BUF) {
            size_t n = fread(d->in + d->used, 1, IN_BUF - d->used, d->file);
            d->used += n;
            if (n == 0) d->eof = true;
        }
        if (d->used == 0 && d->eof) return false;
        if (d->used == 0) continue;

        mp3dec_frame_info_t info;
        int got = mp3dec_decode_frame_with_scratch(&d->dec, d->scratch,
                                                   d->in + d->pos, (int)(d->used - d->pos),
                                                   d->pcm, &info);
        if (info.frame_bytes <= 0) {
            if (d->eof) return false;
            if (d->used == IN_BUF) return false;
            continue;
        }
        d->pos += (size_t)info.frame_bytes;
        *pcm = d->pcm;
        *samples = got;
        if (info_out != NULL) *info_out = info;
        return true;
    }
}

static uint32_t skip_id3(FILE *f)
{
    uint8_t h[10];
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) return 0;
    if (memcmp(h, "ID3", 3) != 0) {
        fseek(f, 0, SEEK_SET);
        return 0;
    }
    uint32_t size = ((uint32_t)(h[6] & 0x7f) << 21) | ((uint32_t)(h[7] & 0x7f) << 14) |
                    ((uint32_t)(h[8] & 0x7f) << 7) | (uint32_t)(h[9] & 0x7f);
    uint32_t total = 10U + size + ((h[5] & 0x10) ? 10U : 0U);
    fseek(f, (long)total, SEEK_SET);
    return total;
}

static uint32_t expected_duration(const char *name)
{
    if (strcmp(name, "after_hours.mp3") == 0) return 361090U;
    if (strcmp(name, "starboy.mp3") == 0) return 230530U;
    if (strcmp(name, "i_feel_it_coming.mp3") == 0) return 269244U;
    if (strcmp(name, "blinding_lights.mp3") == 0) return 200124U;
    return 0;
}

static int compare_pcm(const char *name, uint32_t target_ms, const int16_t *ref,
                       const int16_t *got, size_t frames, int channels)
{
    long long sum = 0;
    int max_diff = 0;
    for (size_t i = 0; i < frames * (size_t)channels; i++) {
        int diff = abs((int)ref[i] - (int)got[i]);
        if (diff > max_diff) max_diff = diff;
        sum += (long long)diff * diff;
    }
    double rms = sqrt((double)sum / (double)(frames * (size_t)channels));
    if (max_diff != 0) {
        for (int shift = -2304; shift <= 2304; shift++) {
            int window = 1024;
            if (shift < 0) {
                if (abs(shift) + window > (int)frames) continue;
            } else if (shift + window > (int)frames) {
                continue;
            }
            long long s = 0;
            for (int i = 0; i < window; i++) {
                int ri = (i + (shift < 0 ? -shift : 0)) * channels;
                int gi = (i + (shift > 0 ? shift : 0)) * channels;
                int diff = abs((int)ref[ri] - (int)got[gi]);
                s += diff;
            }
            if (s < 40000) {
                printf("PCM_LAG track=%s target=%u shift=%d mean_abs=%lld\n", name,
                       (unsigned)target_ms, shift, s / (window * channels));
                break;
            }
        }
    }
    printf("PCM_%s track=%s target_ms=%u frames=%u max_diff=%d rms=%.3f\n",
           max_diff == 0 ? "MATCH" : "DIFF", name, (unsigned)target_ms,
           (unsigned)frames, max_diff, rms);
    if (max_diff > 2048) {
        printf("FAIL: PCM discontinuity too large\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *names[] = {
        "after_hours.mp3", "starboy.mp3", "i_feel_it_coming.mp3", "blinding_lights.mp3"
    };
    static int16_t ref[4][COMPARE_FRAMES * 2];
    static int16_t got[COMPARE_FRAMES * 2];
    unsigned checked = 0;

    for (unsigned t = 0; t < 4; t++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", argv[1], names[t]);
        FILE *f = fopen(path, "rb");
        CHECK(f != NULL);
        uint32_t data_start = skip_id3(f);
        panel_mp3_index_t *idx = calloc(1, sizeof(*idx));
        CHECK(idx != NULL);
        char error[64];
        if (getenv("PCM_USE_PREBUILT") != NULL) {
            const panel_music_prebuilt_track_t *m = &panel_music_prebuilt_tracks[t];
            idx->data_start = m->data_start;
            idx->first_frame_offset = m->first_frame_offset;
            idx->duration_ms = m->duration_ms;
            idx->sample_rate = m->sample_rate;
            idx->total_frames = m->total_frames;
            idx->total_samples = m->total_samples;
            idx->entry_count = m->entry_count;
            idx->frame_samples = m->frame_samples;
            idx->channels = m->channels;
            idx->version = m->version;
            idx->layer = m->layer;
            memcpy(idx->entries, m->entries, m->entry_count * sizeof(m->entries[0]));
        } else {
            CHECK(panel_mp3_index_build(f, data_start, idx, error, sizeof(error)));
        }
        CHECK(idx->duration_ms + 80U >= expected_duration(names[t]));
        CHECK(idx->duration_ms <= expected_duration(names[t]) + 80U);

        uint32_t targets[4] = {
            0U,
            (uint32_t)((uint64_t)idx->duration_ms * 25U / 100U),
            (uint32_t)((uint64_t)idx->duration_ms * 50U / 100U),
            (uint32_t)((uint64_t)idx->duration_ms * 90U / 100U),
        };

        /* One linear decode produces all four reference windows. */
        decoder_t d;
        CHECK(decoder_open(&d, f, (long)data_start));
        uint64_t decoded = 0;
        size_t ref_copied[4] = {0, 0, 0, 0};
        bool ref_started[4] = {false, false, false, false};
        unsigned ref_done = 0;
        while (ref_done < 4) {
            int16_t *pcm;
            int samples;
            if (!decoder_next(&d, &pcm, &samples, NULL)) break;
            uint64_t frame_start = decoded;
            decoded += idx->frame_samples;
            if (samples <= 0) continue;
            for (unsigned seg = 0; seg < 4; seg++) {
                if (ref_copied[seg] >= COMPARE_FRAMES) continue;
                uint32_t target_sample = (uint32_t)(((uint64_t)targets[seg] * idx->sample_rate) /
                                                    1000U);
                if (decoded <= target_sample) continue;
                uint32_t offset = 0;
                if (!ref_started[seg] && target_sample > frame_start) {
                    offset = (uint32_t)((uint64_t)target_sample - frame_start);
                }
                if (offset >= (uint32_t)samples) continue;
                ref_started[seg] = true;
                size_t frames = (size_t)samples - offset;
                if (frames > COMPARE_FRAMES - ref_copied[seg]) {
                    frames = COMPARE_FRAMES - ref_copied[seg];
                }
                memcpy(ref[seg] + ref_copied[seg] * idx->channels,
                       pcm + (size_t)offset * idx->channels,
                       frames * idx->channels * sizeof(int16_t));
                ref_copied[seg] += frames;
                if (ref_copied[seg] == COMPARE_FRAMES) ref_done++;
            }
        }
        decoder_close(&d);
        if (ref_done != 4) {
            printf("REF_SHORT file=%s seg0=%u seg1=%u seg2=%u seg3=%u decoded=%llu targets=%u/%u/%u/%u\n",
                   names[t], (unsigned)ref_copied[0], (unsigned)ref_copied[1],
                   (unsigned)ref_copied[2], (unsigned)ref_copied[3],
                   (unsigned long long)decoded, (unsigned)targets[0], (unsigned)targets[1],
                   (unsigned)targets[2], (unsigned)targets[3]);
        }
        CHECK(ref_done == 4);

        for (unsigned s = 0; s < 4; s++) {
            panel_mp3_seek_t seek;
            CHECK(panel_mp3_index_seek(idx, targets[s], &seek));
            CHECK(decoder_open(&d, f, (long)seek.byte_offset));
            uint32_t floor_sample = seek.frame_floor_sample;
            uint32_t target_sample = (uint32_t)(((uint64_t)targets[s] * idx->sample_rate) /
                                                1000U);
            uint32_t local_target = target_sample - floor_sample;
            uint64_t pos = 0;
            size_t copied = 0;
            bool started = false;
            bool timeline_started = false;
            uint32_t frame_file_offset = seek.byte_offset;
            while (copied < COMPARE_FRAMES) {
                int16_t *pcm;
                int samples;
                mp3dec_frame_info_t info;
                if (!decoder_next(&d, &pcm, &samples, &info)) break;
                uint32_t frame_start_offset = frame_file_offset;
                frame_file_offset += (uint32_t)info.frame_bytes;
                if (!timeline_started) {
                    if (frame_start_offset < seek.timeline_byte_offset) continue;
                    timeline_started = true;
                    pos = 0;
                }
                uint64_t frame_start = pos;
                pos += idx->frame_samples;
                if (samples <= 0) continue;
                if (pos <= local_target) continue;
                uint32_t offset = 0;
                if (!started && frame_start < local_target) {
                    offset = (uint32_t)((uint64_t)local_target - frame_start);
                }
                if (offset >= (uint32_t)samples) continue;
                started = true;
                size_t frames = (size_t)samples - offset;
                if (frames > COMPARE_FRAMES - copied) frames = COMPARE_FRAMES - copied;
                memcpy(got + copied * idx->channels, pcm + (size_t)offset * idx->channels,
                       frames * idx->channels * sizeof(int16_t));
                copied += frames;
            }
            decoder_close(&d);
            printf("PCM_SEEK_INFO file=%s target_ms=%u floor_ms=%u floor_sample=%u local_target=%u\n",
                   names[t], (unsigned)targets[s], (unsigned)seek.frame_floor_ms,
                   (unsigned)seek.frame_floor_sample, (unsigned)local_target);
            CHECK(copied == COMPARE_FRAMES);
            CHECK(compare_pcm(names[t], targets[s], ref[s], got, COMPARE_FRAMES,
                              idx->channels) == 0);
            checked++;
        }

        printf("PCM_TRACK_OK file=%s duration_ms=%u targets=%u/%u/%u/%u\n", names[t],
               (unsigned)idx->duration_ms, (unsigned)targets[0], (unsigned)targets[1],
               (unsigned)targets[2], (unsigned)targets[3]);
        free(idx);
        fclose(f);
    }
    printf("MP3_PCM_SEEK_TESTS_OK checks=%u compare_frames=%u\n", checked,
           (unsigned)COMPARE_FRAMES);
    return 0;
}
