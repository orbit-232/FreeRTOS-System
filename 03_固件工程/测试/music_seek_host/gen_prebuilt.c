#include "panel_music_index.h"
#include "panel_music_prebuilt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619U;
    }
    return h;
}

static uint32_t skip_id3(FILE *f, bool *has_id3)
{
    uint8_t h[10];
    *has_id3 = false;
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) return 0;
    if (memcmp(h, "ID3", 3) != 0) {
        fseek(f, 0, SEEK_SET);
        return 0;
    }
    uint32_t size = ((uint32_t)(h[6] & 0x7f) << 21) | ((uint32_t)(h[7] & 0x7f) << 14) |
                    ((uint32_t)(h[8] & 0x7f) << 7) | (uint32_t)(h[9] & 0x7f);
    uint32_t total = 10U + size + ((h[5] & 0x10) ? 10U : 0U);
    *has_id3 = true;
    fseek(f, (long)total, SEEK_SET);
    return total;
}

static int generate(const char *path, FILE *out, unsigned track)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 1;
    bool has_id3 = false;
    uint32_t tag = skip_id3(f, &has_id3);
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    panel_mp3_index_t *idx = calloc(1, sizeof(*idx));
    if (idx == NULL) return 2;
    char error[64];
    if (!panel_mp3_index_build(f, tag, idx, error, sizeof(error))) {
        fprintf(stderr, "index failed %s: %s\n", path, error);
        return 3;
    }

    panel_music_fingerprint_t fp[PANEL_MUSIC_FINGERPRINT_COUNT];
    uint8_t buf[PANEL_MUSIC_FINGERPRINT_BYTES];
    memset(fp, 0, sizeof(fp));
    for (unsigned i = 0; i < PANEL_MUSIC_FINGERPRINT_COUNT; i++) {
        uint32_t off;
        if (i == 0U) {
            off = 0;
        } else {
            uint32_t tail = (uint32_t)file_size > PANEL_MUSIC_FINGERPRINT_BYTES
                                ? (uint32_t)file_size - PANEL_MUSIC_FINGERPRINT_BYTES
                                : tag;
            if (tail < tag) tail = tag;
            off = tag + (uint32_t)(((uint64_t)(tail - tag) * (i - 1U)) /
                                   (PANEL_MUSIC_FINGERPRINT_COUNT - 2U));
        }
        uint32_t len = PANEL_MUSIC_FINGERPRINT_BYTES;
        if (off + len > (uint32_t)file_size) len = (uint32_t)file_size - off;
        fseek(f, (long)off, SEEK_SET);
        if (fread(buf, 1, len, f) != len) return 4;
        uint32_t hash = fnv1a(buf, len);
        fp[i].offset = off;
        fp[i].len = (uint16_t)len;
        fp[i].hash = hash;
    }

    fprintf(out, "/* Generated offline from the four validated PC MP3s. No audio data. */\n");
    fprintf(out, "#include \"panel_music_prebuilt.h\"\n\n");
    fprintf(out, "static const panel_mp3_index_entry_t entries_%u[] = {\n", track);
    for (uint32_t i = 0; i < idx->entry_count; i++) {
        const panel_mp3_index_entry_t *e = &idx->entries[i];
        fprintf(out, "    {%uU,%uU,%uU,%uU,0x%08XU},\n", (unsigned)e->byte_offset,
                (unsigned)e->frame_index, (unsigned)e->time_ms,
                (unsigned)e->sample_index, (unsigned)e->frame_header);
    }
    fprintf(out, "};\n\nstatic const panel_music_fingerprint_t fp_%u[] = {\n", track);
    for (unsigned i = 0; i < PANEL_MUSIC_FINGERPRINT_COUNT; i++) {
        fprintf(out, "    {%uU,0x%08XU,%uU,0U},\n", (unsigned)fp[i].offset,
                (unsigned)fp[i].hash, (unsigned)fp[i].len);
    }
    fprintf(out, "};\n\n");
    free(idx);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: gen_prebuilt <out.c> <0.mp3> <1.mp3> <2.mp3> <3.mp3>\n");
        return 2;
    }
    FILE *out = fopen(argv[1], "wb");
    if (out == NULL) {
        perror("open output");
        return 3;
    }
    for (unsigned t = 0; t < 4U; t++) {
        if (generate(argv[t + 2], out, t) != 0) return 4;
    }

    fprintf(out, "const panel_music_prebuilt_track_t panel_music_prebuilt_tracks[4] = {\n");
    /* Metadata must be re-read here because arrays above were emitted in tmp. */
    for (unsigned t = 0; t < 4U; t++) {
        FILE *f = fopen(argv[t + 2], "rb");
        if (f == NULL) return 5;
        bool has_id3 = false;
        uint32_t tag = skip_id3(f, &has_id3);
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        panel_mp3_index_t *idx = calloc(1, sizeof(*idx));
        if (idx == NULL) return 6;
        char error[64];
        if (!panel_mp3_index_build(f, tag, idx, error, sizeof(error))) return 7;
        fprintf(out,
                "    {%uU,%uU,%uU,%uU,%uU,%uU,%uU,%uU,%uU,%uU,%uU,%uU,%uU,%uU,%uU,"
                "entries_%u,fp_%u},\n",
                (unsigned)size, (unsigned)tag, (unsigned)tag,
                (unsigned)idx->first_frame_offset, (unsigned)idx->duration_ms,
                (unsigned)idx->sample_rate, (unsigned)idx->total_frames,
                (unsigned)idx->total_samples, (unsigned)idx->entry_count,
                (unsigned)PANEL_MUSIC_FINGERPRINT_COUNT, (unsigned)idx->frame_samples,
                (unsigned)idx->channels, (unsigned)idx->version, (unsigned)idx->layer,
                has_id3 ? 1U : 0U, t, t);
        free(idx);
        fclose(f);
    }
    fprintf(out, "};\n");
    fclose(out);
    return 0;
}
