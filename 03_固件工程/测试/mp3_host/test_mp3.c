#include "minimp3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IN_BUF (64 * 1024)
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

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
    if (fseek(f, (long)(10 + size + ((h[5] & 0x10) ? 10 : 0)), SEEK_SET) != 0) return -1;
    return 1;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    CHECK(f != NULL);
    int id3 = skip_id3(f);
    CHECK(id3 >= 0);

    mp3dec_t *dec = calloc(1, sizeof(*dec));
    mp3dec_init(dec);
    size_t scratch_bytes = mp3dec_scratch_size();
    mp3dec_scratch_t *scratch = malloc(scratch_bytes);
    uint8_t *in = malloc(IN_BUF);
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    CHECK(dec != NULL && scratch != NULL && in != NULL);
    CHECK(scratch_bytes > 16000 && scratch_bytes < 16400);
    size_t used = 0, pos = 0;
    uint64_t frames = 0, samples = 0;
    int hz = 0, ch = 0;
    int got_frame = 0;
    int eof = 0;
    for (;;) {
        if (pos > 0 && pos == used) {
            used = 0;
            pos = 0;
        } else if (pos > 0) {
            memmove(in, in + pos, used - pos);
            used -= pos;
            pos = 0;
        }
        if (!eof && used < IN_BUF) {
            size_t n = fread(in + used, 1, IN_BUF - used, f);
            used += n;
            if (n == 0) eof = 1;
        }
        if (used == 0 && eof) break;
        mp3dec_frame_info_t info;
        int frame_samples = mp3dec_decode_frame_with_scratch(dec, scratch, in + pos,
                                                             (int)(used - pos), pcm, &info);
        if (info.frame_bytes > 0) {
            pos += (size_t)info.frame_bytes;
            if (frame_samples > 0) {
                if (!got_frame) {
                    hz = info.hz;
                    ch = info.channels;
                    got_frame = 1;
                }
                CHECK(info.hz == hz);
                CHECK(info.channels == ch);
                frames++;
                samples += (uint64_t)frame_samples * (uint64_t)info.channels;
            }
            continue;
        }
        if (eof) break;
        if (used == IN_BUF) {
            printf("FAIL no valid frame in %u bytes\n", IN_BUF);
            return 1;
        }
    }
    fclose(f);
    free(scratch);
    free(in);
    free(dec);
    printf("MP3_DECODE_OK frames=%llu samples=%llu hz=%d ch=%d id3=%d scratch=%u\n",
           (unsigned long long)frames, (unsigned long long)samples, hz, ch, id3,
           (unsigned)scratch_bytes);
    CHECK(got_frame);
    CHECK(frames > 1000);
    CHECK(samples > 1000000);
    return 0;
}
