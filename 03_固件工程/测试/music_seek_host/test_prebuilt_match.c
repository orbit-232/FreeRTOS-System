#include "panel_music_index.h"
#include "panel_music_prebuilt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static uint32_t skip_id3(FILE *f)
{
    uint8_t h[10];
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) return 0;
    if (memcmp(h, "ID3", 3) != 0) { fseek(f, 0, SEEK_SET); return 0; }
    uint32_t size = ((uint32_t)(h[6] & 0x7f) << 21) | ((uint32_t)(h[7] & 0x7f) << 14) |
                    ((uint32_t)(h[8] & 0x7f) << 7) | (uint32_t)(h[9] & 0x7f);
    uint32_t total = 10U + size + ((h[5] & 0x10) ? 10U : 0U);
    fseek(f, (long)total, SEEK_SET);
    return total;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *names[4] = {"after_hours.mp3", "starboy.mp3", "i_feel_it_coming.mp3",
                            "blinding_lights.mp3"};
    for (unsigned t = 0; t < 4; t++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", argv[1], names[t]);
        FILE *f = fopen(path, "rb");
        CHECK(f != NULL);
        uint32_t tag = skip_id3(f);
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, (long)tag, SEEK_SET);
        panel_mp3_index_t *idx = calloc(1, sizeof(*idx));
        CHECK(idx != NULL);
        char error[64];
        CHECK(panel_mp3_index_build(f, tag, idx, error, sizeof(error)));
        const panel_music_prebuilt_track_t *m = &panel_music_prebuilt_tracks[t];
        CHECK(m->file_size == (uint32_t)file_size);
        CHECK(m->tag_bytes == tag);
        CHECK(m->duration_ms == idx->duration_ms);
        CHECK(m->total_frames == idx->total_frames);
        CHECK(m->entry_count == idx->entry_count);
        for (uint32_t i = 0; i < idx->entry_count; i++) {
            CHECK(m->entries[i].byte_offset == idx->entries[i].byte_offset);
            CHECK(m->entries[i].frame_index == idx->entries[i].frame_index);
            CHECK(m->entries[i].time_ms == idx->entries[i].time_ms);
            CHECK(m->entries[i].sample_index == idx->entries[i].sample_index);
            CHECK(m->entries[i].frame_header == idx->entries[i].frame_header);
        }
        printf("PREBUILT_ENTRIES_OK track=%u entries=%u\n", t, (unsigned)idx->entry_count);
        free(idx);
        fclose(f);
    }
    printf("PREBUILT_ALL_OK\n");
    return 0;
}
