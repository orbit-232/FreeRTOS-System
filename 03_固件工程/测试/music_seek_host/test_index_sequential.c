#include "panel_music_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 100U
#define FRAME_BYTES 417U
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t offsets[64];
    uint32_t lengths[64];
    unsigned calls;
    uint64_t bytes;
} trace_ctx_t;

static size_t trace_read(void *opaque, uint32_t offset, uint8_t *buf, size_t len,
                         bool *io_error)
{
    trace_ctx_t *ctx = opaque;
    *io_error = false;
    CHECK(ctx->calls < 64U);
    ctx->offsets[ctx->calls] = offset;
    if (offset >= ctx->size) return 0;
    size_t n = ctx->size - offset;
    if (n > len) n = len;
    memcpy(buf, ctx->data + offset, n);
    ctx->lengths[ctx->calls++] = (uint32_t)n;
    ctx->bytes += n;
    return n;
}

static uint8_t *make_file(uint32_t prefix, const uint8_t *tail, size_t tail_len,
                          size_t *size_out)
{
    size_t frame_bytes = FRAMES * FRAME_BYTES;
    size_t size = prefix + frame_bytes + tail_len;
    uint8_t *data = calloc(1, size);
    if (data == NULL) return NULL;
    for (uint32_t i = 0; i < FRAMES; i++) {
        uint8_t *p = data + prefix + i * FRAME_BYTES;
        p[0] = 0xFF;
        p[1] = 0xFB;
        p[2] = 0x90; /* MPEG1 Layer III, 128 kbps, 44.1 kHz, no padding */
        p[3] = 0x00;
    }
    if (tail_len > 0) memcpy(data + prefix + frame_bytes, tail, tail_len);
    *size_out = size;
    return data;
}

static int run_case(uint32_t prefix, const uint8_t *tail, size_t tail_len)
{
    size_t size = 0;
    uint8_t *data = make_file(prefix, tail, tail_len, &size);
    CHECK(data != NULL);
    trace_ctx_t trace = {.data = data, .size = (uint32_t)size};
    panel_mp3_index_t *idx = calloc(1, sizeof(*idx));
    CHECK(idx != NULL);
    char error[64];
    bool ok = panel_mp3_index_build_leased(idx, 0, (uint32_t)size, trace_read, NULL,
                                           &trace, error, sizeof(error));
    if (!ok) {
        printf("BUILD_FAIL prefix=%u tail=%u error=%s\n", (unsigned)prefix,
               (unsigned)tail_len, error);
        return 1;
    }
    CHECK(idx->total_frames == FRAMES);
    CHECK(idx->first_frame_offset == prefix);
    CHECK(idx->entry_count == (FRAMES + PANEL_MP3_INDEX_STRIDE - 1U) /
                                       PANEL_MP3_INDEX_STRIDE);
    CHECK(idx->duration_ms > 2600U && idx->duration_ms < 2700U);
    CHECK(trace.bytes == size);
    CHECK(idx->bytes_read == size);
    CHECK(idx->read_calls == trace.calls);
    for (unsigned i = 1; i < trace.calls; i++) {
        CHECK(trace.offsets[i] == trace.offsets[i - 1U] + trace.lengths[i - 1U]);
    }
    printf("CARRY_OK prefix=%u tail=%u frames=%u entries=%u calls=%u bytes=%llu "
           "scan_us=%u max_read_us=%u\n",
           (unsigned)prefix, (unsigned)tail_len, (unsigned)idx->total_frames,
           (unsigned)idx->entry_count, trace.calls, (unsigned long long)trace.bytes,
           (unsigned)idx->scan_us, (unsigned)idx->max_read_us);
    free(idx);
    free(data);
    return 0;
}

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t limit;
    uint32_t fail_at;
    uint32_t offsets[1024];
    uint32_t lengths[1024];
    unsigned calls;
    bool fail_after_limit;
} short_ctx_t;

static size_t short_read(void *opaque, uint32_t offset, uint8_t *buf, size_t len,
                         bool *io_error)
{
    short_ctx_t *ctx = opaque;
    *io_error = false;
    CHECK(ctx->calls < 1024U);
    ctx->offsets[ctx->calls] = offset;
    if (ctx->fail_after_limit && offset >= ctx->fail_at) return 0;
    if (offset >= ctx->size) return 0;
    size_t n = ctx->size - offset;
    if (n > len) n = len;
    if (n > ctx->limit) n = ctx->limit;
    if (ctx->fail_after_limit && offset + n > ctx->fail_at) {
        n = ctx->fail_at - offset;
    }
    memcpy(buf, ctx->data + offset, n);
    ctx->lengths[ctx->calls++] = (uint32_t)n;
    return n;
}

static bool cancel_after_two(void *opaque)
{
    short_ctx_t *ctx = opaque;
    return ctx->calls >= 2U;
}

static int run_short_cases(void)
{
    size_t size = 0;
    uint8_t *data = make_file(0, NULL, 0U, &size);
    CHECK(data != NULL);
    panel_mp3_index_t *idx = calloc(1, sizeof(*idx));
    CHECK(idx != NULL);
    short_ctx_t ctx = {.data = data, .size = (uint32_t)size, .limit = 9000U};
    char error[64];
    bool ok = panel_mp3_index_build_leased(idx, 0, (uint32_t)size, short_read, NULL,
                                           &ctx, error, sizeof(error));
    CHECK(ok);
    CHECK(idx->total_frames == FRAMES);
    CHECK(ctx.calls > 4U);
    for (unsigned i = 1; i < ctx.calls; i++) {
        CHECK(ctx.offsets[i] == ctx.offsets[i - 1U] + ctx.lengths[i - 1U]);
    }
    printf("SHORT_READ_RETRY_OK calls=%u frames=%u\n", ctx.calls,
           (unsigned)idx->total_frames);

    memset(idx, 0, sizeof(*idx));
    short_ctx_t eof = {.data = data, .size = (uint32_t)size, .limit = 5000U,
                       .fail_at = 5000U, .fail_after_limit = true};
    ok = panel_mp3_index_build_leased(idx, 0, (uint32_t)size, short_read, NULL, &eof,
                                      error, sizeof(error));
    CHECK(!ok);
    CHECK(strcmp(error, "short read before EOF") == 0 || strstr(error, "short read") != NULL);
    printf("EARLY_EOF_REJECTED error=%s calls=%u\n", error, eof.calls);

    memset(idx, 0, sizeof(*idx));
    short_ctx_t cancel = {.data = data, .size = (uint32_t)size, .limit = 9000U};
    ok = panel_mp3_index_build_leased(idx, 0, (uint32_t)size, short_read, cancel_after_two,
                                      &cancel, error, sizeof(error));
    CHECK(!ok);
    CHECK(strcmp(error, "cancelled") == 0);
    CHECK(cancel.calls == 2U);
    printf("SHORT_READ_CANCEL_OK calls=%u\n", cancel.calls);
    free(idx);
    free(data);
    return 0;
}

int main(void)
{
    const uint32_t buffer = PANEL_MP3_INDEX_IO_BUF;
    /* Headers beginning in the last 1..4 bytes exercise carry and in-buffer
     * skip-after-header paths. */
    CHECK(run_case(buffer - 1U, NULL, 0U) == 0);
    CHECK(run_case(buffer - 2U, NULL, 0U) == 0);
    CHECK(run_case(buffer - 3U, NULL, 0U) == 0);
    CHECK(run_case(buffer - 4U, NULL, 0U) == 0);
    CHECK(run_case(0U, NULL, 0U) == 0);
    /* A sub-4-byte tail must be ignored, not treated as a complete frame. */
    for (size_t tail = 1; tail <= 3; tail++) {
        uint8_t bytes[3] = {0, 0, 0};
        CHECK(run_case(0U, bytes, tail) == 0);
    }
    CHECK(run_short_cases() == 0);
    printf("INDEX_SEQUENTIAL_CARRY_TESTS_OK buffer=%u\n", (unsigned)buffer);
    return 0;
}
