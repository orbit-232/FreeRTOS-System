#include "minimp3.h"
#include "panel_music_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IN_BUF (64U * 1024U)
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
#define CHECK0(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 0; } } while (0)

typedef struct {
    const char *name;
    uint32_t expected_ms;
} case_t;

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

static uint32_t decode_to_target(FILE *f, const panel_mp3_index_t *idx, uint32_t target_ms)
{
    panel_mp3_seek_t seek;
    CHECK0(panel_mp3_index_seek(idx, target_ms, &seek));
    CHECK0(fseek(f, (long)seek.byte_offset, SEEK_SET) == 0);

    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_scratch_t *scratch = malloc(mp3dec_scratch_size());
    uint8_t *in = malloc(IN_BUF);
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    CHECK(scratch != NULL && in != NULL);
    size_t used = 0;
    uint64_t decoded = 0;
    int eof = 0;
    uint32_t reached = 0;
    while (!reached) {
        size_t n = fread(in + used, 1, IN_BUF - used, f);
        used += n;
        if (n == 0) eof = 1;
        size_t pos = 0;
        while (pos + 4U <= used) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame_with_scratch(&dec, scratch, in + pos,
                                                           (int)(used - pos), pcm, &info);
            if (info.frame_bytes <= 0) break;
            pos += (size_t)info.frame_bytes;
            if (samples <= 0) continue;
            decoded += (uint64_t)samples;
            uint32_t elapsed = seek.frame_floor_ms +
                               (uint32_t)(decoded * 1000ULL / idx->sample_rate);
            if (elapsed >= target_ms) {
                reached = elapsed;
                break;
            }
        }
        if (pos > 0) {
            memmove(in, in + pos, used - pos);
            used -= pos;
        }
        if (eof && !reached) break;
    }
    free(in);
    free(scratch);
    return reached;
}

static int check_case(const char *dir, const case_t *tc)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, tc->name);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    uint32_t start = skip_id3(f);

    panel_mp3_index_t *idx = calloc(1, sizeof(*idx));
    CHECK(idx != NULL);
    char error[64];
    CHECK(panel_mp3_index_build(f, start, idx, error, sizeof(error)));
    CHECK(idx->duration_ms + 80U >= tc->expected_ms);
    CHECK(idx->duration_ms <= tc->expected_ms + 80U);
    CHECK(idx->entry_count > 100U);
    CHECK(idx->entry_count <= PANEL_MP3_INDEX_MAX);
    CHECK(idx->sample_rate == 44100U);
    CHECK(idx->channels == 2U);
    CHECK(idx->entries[0].byte_offset >= start);
    for (uint32_t i = 1; i < idx->entry_count; i++) {
        CHECK(idx->entries[i].byte_offset > idx->entries[i - 1U].byte_offset);
        CHECK(idx->entries[i].time_ms > idx->entries[i - 1U].time_ms);
    }

    uint32_t targets[3] = {5000U, 120000U, idx->duration_ms > 10000U ? idx->duration_ms - 5000U : 1000U};
    for (unsigned i = 0; i < 3; i++) {
        panel_mp3_seek_t seek;
        CHECK(panel_mp3_index_seek(idx, targets[i], &seek));
        uint32_t got = decode_to_target(f, idx, targets[i]);
        CHECK(got >= targets[i]);
        CHECK(got - targets[i] <= PANEL_MP3_SEEK_TOLERANCE_MS);
        printf("SEEK_OK file=%s target_ms=%u decoded_ms=%u error_ms=%u floor_ms=%u\n",
               tc->name, (unsigned)targets[i], (unsigned)got,
               (unsigned)(got - targets[i]), (unsigned)seek.frame_floor_ms);
    }
    printf("INDEX_OK file=%s frames=%u duration_ms=%u entries=%u first=%u fmt=%s/%uHz/%uch\n",
           tc->name, (unsigned)idx->total_frames, (unsigned)idx->duration_ms,
           (unsigned)idx->entry_count, (unsigned)idx->first_frame_offset,
           panel_mp3_version_name(idx->version), (unsigned)idx->sample_rate,
           (unsigned)idx->channels);
    free(idx);
    fclose(f);
    return 0;
}

typedef struct {
    FILE *file;
    unsigned reads;
    bool force_io_error;
} lease_ctx_t;

static size_t test_read_at(void *opaque, uint32_t offset, uint8_t *buf, size_t len,
                           bool *io_error)
{
    lease_ctx_t *ctx = (lease_ctx_t *)opaque;
    *io_error = false;
    ctx->reads++;
    if (ctx->force_io_error) {
        *io_error = true;
        return 0;
    }
    if (fseek(ctx->file, (long)offset, SEEK_SET) != 0) {
        *io_error = true;
        return 0;
    }
    return fread(buf, 1, len, ctx->file);
}

static bool test_cancel_after_one(void *opaque)
{
    lease_ctx_t *ctx = (lease_ctx_t *)opaque;
    return ctx->reads >= 1U;
}

static int write_vbr_frame(FILE *f, uint32_t index)
{
    uint8_t bitrate_index[] = {9, 11, 13, 14};
    uint8_t idx = bitrate_index[index % 4U];
    uint32_t kbps = idx == 9 ? 128U : idx == 11 ? 192U : idx == 13 ? 256U : 320U;
    uint32_t frame_bytes = (144U * kbps * 1000U) / 44100U;
    uint8_t header[4] = {0xFF, 0xFB, (uint8_t)(idx << 4), 0x00};
    CHECK(fwrite(header, 1, sizeof(header), f) == sizeof(header));
    uint8_t zero[1024];
    memset(zero, 0, sizeof(zero));
    CHECK(fwrite(zero, 1, frame_bytes - sizeof(header), f) == frame_bytes - sizeof(header));
    return 0;
}

static int check_leased_errors(void)
{
    FILE *f = fopen("vbr_index_test.mp3", "rb");
    CHECK(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    CHECK(size > 1000);

    panel_mp3_index_t *idx = calloc(1, sizeof(*idx));
    CHECK(idx != NULL);
    char error[64];
    lease_ctx_t ctx = {.file = f, .reads = 0};

    CHECK(!panel_mp3_index_build_leased(idx, 0, (uint32_t)size - 200U, test_read_at,
                                        NULL, &ctx, error, sizeof(error)));
    printf("TRUNCATED_INDEX_REJECTED error=%s\n", error);

    ctx.reads = 0;
    CHECK(!panel_mp3_index_build_leased(idx, 0, (uint32_t)size, test_read_at,
                                        test_cancel_after_one, &ctx, error, sizeof(error)));
    CHECK(strcmp(error, "cancelled") == 0);
    printf("CANCEL_INDEX_OK reads=%u\n", ctx.reads);

    ctx.reads = 0;
    ctx.force_io_error = true;
    CHECK(!panel_mp3_index_build_leased(idx, 0, (uint32_t)size, test_read_at,
                                        NULL, &ctx, error, sizeof(error)));
    CHECK(strcmp(error, "TF read failed") == 0);
    printf("IO_ERROR_INDEX_REJECTED error=%s\n", error);

    free(idx);
    fclose(f);
    return 0;
}

static int check_long_index(void)
{
    const uint32_t frames = 20000U;
    FILE *f = fopen("long_index_test.mp3", "w+b");
    CHECK(f != NULL);
    for (uint32_t i = 0; i < frames; i++) {
        if (write_vbr_frame(f, i) != 0) return 1;
    }
    panel_mp3_index_t *idx = calloc(1, sizeof(*idx));
    CHECK(idx != NULL);
    char error[64];
    CHECK(panel_mp3_index_build(f, 0, idx, error, sizeof(error)));
    CHECK(idx->entry_count <= PANEL_MP3_INDEX_MAX);
    CHECK(idx->entry_count > 1000U);
    CHECK(idx->duration_ms > 500000U);
    panel_mp3_seek_t seek;
    CHECK(panel_mp3_index_seek(idx, (uint32_t)((uint64_t)idx->duration_ms * 90U / 100U),
                               &seek));
    CHECK(seek.frame_floor_ms > idx->duration_ms * 85U / 100U);
    printf("LONG_INDEX_OK frames=%u duration_ms=%u entries=%u floor_ms=%u\n",
           (unsigned)idx->total_frames, (unsigned)idx->duration_ms,
           (unsigned)idx->entry_count, (unsigned)seek.frame_floor_ms);
    free(idx);
    fclose(f);
    remove("long_index_test.mp3");
    return 0;
}

static int check_vbr(void)
{
    const uint32_t frames = 400U;
    FILE *f = fopen("vbr_index_test.mp3", "w+b");
    CHECK(f != NULL);
    for (uint32_t i = 0; i < frames; i++) {
        if (write_vbr_frame(f, i) != 0) return 1;
    }
    panel_mp3_index_t *idx = calloc(1, sizeof(*idx));
    CHECK(idx != NULL);
    char error[64];
    CHECK(panel_mp3_index_build(f, 0, idx, error, sizeof(error)));
    CHECK(idx->total_frames == frames);
    CHECK(idx->entry_count == frames / PANEL_MP3_INDEX_STRIDE);
    CHECK(idx->duration_ms > 10400U && idx->duration_ms < 10500U);
    CHECK(idx->entry_count > 10U);
    panel_mp3_seek_t seek;
    CHECK(panel_mp3_index_seek(idx, 5000U, &seek));
    CHECK(seek.frame_floor_ms < 5000U);
    CHECK(5000U - seek.frame_floor_ms >= 150U);
    CHECK(fseek(f, (long)seek.byte_offset, SEEK_SET) == 0);
    uint8_t header[4];
    CHECK(fread(header, 1, sizeof(header), f) == sizeof(header));
    CHECK(header[0] == 0xFFU && header[1] == 0xFBU);
    printf("VBR_INDEX_OK frames=%u duration_ms=%u entries=%u target_ms=%u floor_ms=%u\n",
           (unsigned)frames, (unsigned)idx->duration_ms, (unsigned)idx->entry_count,
           (unsigned)seek.target_ms, (unsigned)seek.frame_floor_ms);
    free(idx);
    fclose(f);
    if (check_leased_errors() != 0) return 1;
    if (check_long_index() != 0) return 1;
    remove("vbr_index_test.mp3");
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const case_t cases[] = {
        {"after_hours.mp3", 361091U},
        {"starboy.mp3", 230531U},
        {"i_feel_it_coming.mp3", 269244U},
        {"blinding_lights.mp3", 200124U},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (check_case(argv[1], &cases[i]) != 0) return 1;
    }
    if (check_vbr() != 0) return 1;
    printf("MP3_INDEX_SEEK_TESTS_OK cases=%u\n", (unsigned)(sizeof(cases) / sizeof(cases[0])));
    return 0;
}
