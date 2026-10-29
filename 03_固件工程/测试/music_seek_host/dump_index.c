#include "panel_music_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) return 3;
    uint32_t start = skip_id3(f);
    panel_mp3_index_t *idx = calloc(1, sizeof(*idx));
    if (idx == NULL) return 4;
    char err[64];
    if (!panel_mp3_index_build(f, start, idx, err, sizeof(err))) {
        fprintf(stderr, "build failed: %s\n", err);
        return 5;
    }
    FILE *out = fopen(argv[2], "wb");
    if (out == NULL) return 6;
    fprintf(out, "duration_ms=%u\n", (unsigned)idx->duration_ms);
    fprintf(out, "sample_rate=%u\n", (unsigned)idx->sample_rate);
    fprintf(out, "channels=%u\n", (unsigned)idx->channels);
    fprintf(out, "frame_samples=%u\n", (unsigned)idx->frame_samples);
    fprintf(out, "total_frames=%u\n", (unsigned)idx->total_frames);
    fprintf(out, "total_samples=%u\n", (unsigned)idx->total_samples);
    fprintf(out, "entries=%u\n", (unsigned)idx->entry_count);
    for (uint32_t i = 0; i < idx->entry_count; i++) {
        fprintf(out, "%u,%u,%u,%u\n", (unsigned)idx->entries[i].byte_offset,
                (unsigned)idx->entries[i].frame_index, (unsigned)idx->entries[i].time_ms,
                (unsigned)idx->entries[i].sample_index);
    }
    fclose(out);
    fprintf(stderr, "STATS %s scan_us=%u read_calls=%u bytes=%u max_read_us=%u\n",
            argv[1], (unsigned)idx->scan_us, (unsigned)idx->read_calls,
            (unsigned)idx->bytes_read, (unsigned)idx->max_read_us);
    free(idx);
    fclose(f);
    return 0;
}
