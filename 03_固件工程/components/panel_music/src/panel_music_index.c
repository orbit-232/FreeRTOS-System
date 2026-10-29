#include "panel_music_index.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#define INDEX_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define INDEX_FREE(p) heap_caps_free(p)
static uint64_t index_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}
#else
#include <time.h>
#define INDEX_ALLOC(n) malloc(n)
#define INDEX_FREE(p) free(p)
static uint64_t index_now_us(void)
{
    return (uint64_t)clock() * 1000000ULL / (uint64_t)CLOCKS_PER_SEC;
}
#endif

#define INDEX_IO_BUF PANEL_MP3_INDEX_IO_BUF
#define INDEX_MAX_FRAME_BYTES 1441U

typedef struct {
    uint32_t version;
    uint32_t layer;
    uint32_t bitrate_kbps;
    uint32_t sample_rate;
    uint32_t frame_bytes;
    uint32_t samples;
    uint32_t channels;
} mp3_header_t;

static const uint16_t k_bitrate_mpeg1_l3[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0,
};
static const uint16_t k_bitrate_mpeg2_l3[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0,
};
static const uint32_t k_sample_rate[3] = {44100U, 48000U, 32000U};

static bool parse_header(const uint8_t *p, size_t avail, mp3_header_t *out)
{
    if (p == NULL || out == NULL || avail < 4U) return false;
    if (p[0] != 0xFFU || (p[1] & 0xE0U) != 0xE0U) return false;
    uint8_t version_bits = (uint8_t)((p[1] >> 3) & 0x03U);
    uint8_t layer_bits = (uint8_t)((p[1] >> 1) & 0x03U);
    if (version_bits == 1U || layer_bits != 1U) return false;

    uint8_t bitrate_index = (uint8_t)((p[2] >> 4) & 0x0FU);
    uint8_t sample_index = (uint8_t)((p[2] >> 2) & 0x03U);
    uint8_t padding = (uint8_t)((p[2] >> 1) & 0x01U);
    if (bitrate_index == 0U || bitrate_index == 15U || sample_index == 3U) return false;

    uint32_t hz = k_sample_rate[sample_index];
    uint32_t kbps;
    uint32_t samples;
    uint32_t frame_bytes;
    if (version_bits == 3U) {
        kbps = k_bitrate_mpeg1_l3[bitrate_index];
        samples = 1152U;
        frame_bytes = (144U * kbps * 1000U) / hz + padding;
    } else {
        hz >>= (version_bits == 2U) ? 1U : 2U;
        kbps = k_bitrate_mpeg2_l3[bitrate_index];
        samples = 576U;
        frame_bytes = (72U * kbps * 1000U) / hz + padding;
    }
    if (hz < 8000U || hz > 48000U || kbps == 0U ||
        frame_bytes < 4U || frame_bytes > INDEX_MAX_FRAME_BYTES) {
        return false;
    }
    uint8_t mode = (uint8_t)((p[3] >> 6) & 0x03U);
    out->version = version_bits;
    out->layer = layer_bits;
    out->bitrate_kbps = kbps;
    out->sample_rate = hz;
    out->frame_bytes = frame_bytes;
    out->samples = samples;
    out->channels = (mode == 3U) ? 1U : 2U;
    return true;
}

static uint32_t clamp_u32(uint64_t value)
{
    return value > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)value;
}

static void set_error(char *error, unsigned error_size, const char *text)
{
    if (error != NULL && error_size > 0U) snprintf(error, error_size, "%s", text);
}

const char *panel_mp3_version_name(uint8_t version)
{
    switch (version) {
    case 3: return "MPEG1";
    case 2: return "MPEG2";
    case 0: return "MPEG2.5";
    default: return "unknown";
    }
}

typedef struct {
    FILE *file;
    uint32_t cursor;
    bool cursor_valid;
    bool io_error;
} file_read_ctx_t;

static size_t read_file_at(void *ctx, uint32_t offset, uint8_t *buf, size_t len,
                           bool *io_error)
{
    file_read_ctx_t *r = (file_read_ctx_t *)ctx;
    *io_error = false;
    if (r == NULL || r->file == NULL) {
        *io_error = true;
        return 0;
    }
    if (!r->cursor_valid || offset != r->cursor) {
        if (fseek(r->file, (long)offset, SEEK_SET) != 0) {
            *io_error = true;
            r->cursor_valid = false;
            return 0;
        }
    }
    size_t n = fread(buf, 1, len, r->file);
    r->cursor = offset + (uint32_t)n;
    r->cursor_valid = true;
    if (ferror(r->file)) {
        *io_error = true;
        r->io_error = true;
        r->cursor_valid = false;
    }
    return n;
}

bool panel_mp3_index_build_leased(panel_mp3_index_t *out, uint32_t data_start,
                                  uint32_t file_size, panel_mp3_read_at_t read_at,
                                  panel_mp3_cancel_t cancel, void *ctx,
                                  char *error, unsigned error_size)
{
    set_error(error, error_size, "");
    if (out == NULL || read_at == NULL) {
        set_error(error, error_size, "invalid index args");
        return false;
    }
    if (file_size <= data_start) {
        set_error(error, error_size, "empty stream");
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->data_start = data_start;

    uint8_t *buf = (uint8_t *)INDEX_ALLOC(INDEX_IO_BUF);
    if (buf == NULL) {
        set_error(error, error_size, "no PSRAM scan buffer");
        return false;
    }

    uint32_t pos = data_start;
    uint32_t buf_start = data_start;
    size_t buf_len = 0;
    uint32_t frames = 0;
    uint64_t samples = 0;
    uint32_t hz = 0;
    uint32_t channels = 0;
    uint32_t frame_samples = 0;
    uint8_t version = 0;
    bool have_format = false;
    bool truncated = false;
    uint32_t stride = PANEL_MP3_INDEX_STRIDE;
    uint32_t next_entry_frame = 0;
    uint64_t scan_start = index_now_us();

    while (pos + 4U <= file_size) {
        if (cancel != NULL && cancel(ctx)) {
            set_error(error, error_size, "cancelled");
            INDEX_FREE(buf);
            return false;
        }

        uint32_t off = pos - buf_start;
        if (off + 4U > buf_len) {
            uint32_t old_end = buf_start + (uint32_t)buf_len;
            if (old_end >= file_size) break;
            size_t carry = 0;
            uint32_t skip = 0;
            if (pos <= old_end) {
                carry = old_end - pos;
                if (carry > 3U) {
                    set_error(error, error_size, "index refill overlap");
                    INDEX_FREE(buf);
                    return false;
                }
                if (carry > 0U) {
                    memmove(buf, buf + (pos - buf_start), carry);
                }
                buf_start = pos;
            } else {
                skip = pos - old_end;
                buf_start = old_end;
            }

            uint32_t read_offset = old_end;
            size_t got = 0;
            size_t want = INDEX_IO_BUF - carry;
            while (got < want && read_offset + got < file_size) {
                if (cancel != NULL && cancel(ctx)) {
                    set_error(error, error_size, "cancelled");
                    INDEX_FREE(buf);
                    return false;
                }
                bool io_error = false;
                uint64_t read_start = index_now_us();
                size_t n = read_at(ctx, read_offset + (uint32_t)got, buf + carry + got,
                                   want - got, &io_error);
                uint32_t read_us = (uint32_t)(index_now_us() - read_start);
                if (read_us > out->max_read_us) out->max_read_us = read_us;
                if (io_error) {
                    set_error(error, error_size, "TF read failed");
                    INDEX_FREE(buf);
                    return false;
                }
                if (n == 0U) {
                    if (read_offset + got < file_size) {
                        set_error(error, error_size, "short read before EOF");
                        INDEX_FREE(buf);
                        return false;
                    }
                    break;
                }
                got += n;
                out->read_calls++;
                out->bytes_read += (uint32_t)n;
                if (read_offset + got >= file_size) break;
            }
            if (got < want && read_offset + got < file_size) {
                set_error(error, error_size, "short read retry exhausted");
                INDEX_FREE(buf);
                return false;
            }
            buf_len = carry + got;
            off = carry > 0U ? 0U : skip;
            if (off + 4U > buf_len) {
                if (pos + 4U <= file_size) {
                    set_error(error, error_size, "short read cannot cover header");
                    INDEX_FREE(buf);
                    return false;
                }
                break;
            }
        }

        mp3_header_t h;
        if (!parse_header(buf + off, buf_len - off, &h)) {
            pos++;
            continue;
        }
        if (pos + h.frame_bytes > file_size) {
            truncated = true;
            break;
        }
        if (!have_format) {
            have_format = true;
            hz = h.sample_rate;
            channels = h.channels;
            frame_samples = h.samples;
            version = (uint8_t)h.version;
            out->first_frame_offset = pos;
        } else if (h.sample_rate != hz || h.channels != channels ||
                   h.samples != frame_samples) {
            /* One seek timeline cannot represent a mid-stream format change. */
            break;
        }

        if (frames >= next_entry_frame) {
            if (out->entry_count == PANEL_MP3_INDEX_MAX && stride < PANEL_MP3_INDEX_STRIDE_MAX) {
                /* Keep the whole song covered by decimating every other entry.
                 * The resulting pre-roll is bounded to ~1.7 s. */
                uint32_t dst = 0;
                for (uint32_t src = 1U; src < out->entry_count; src += 2U) {
                    out->entries[dst++] = out->entries[src];
                }
                out->entry_count = dst;
                stride *= 2U;
            }
            if (out->entry_count < PANEL_MP3_INDEX_MAX) {
                panel_mp3_index_entry_t *entry = &out->entries[out->entry_count++];
                entry->byte_offset = pos;
                entry->frame_index = frames;
                entry->time_ms = clamp_u32(samples * 1000ULL / hz);
                entry->sample_index = clamp_u32(samples);
                entry->frame_header = ((uint32_t)buf[off] << 24) |
                                      ((uint32_t)buf[off + 1U] << 16) |
                                      ((uint32_t)buf[off + 2U] << 8) |
                                      (uint32_t)buf[off + 3U];
            }
            next_entry_frame = frames + stride;
        }
        samples += h.samples;
        frames++;
        pos += h.frame_bytes;
    }

    INDEX_FREE(buf);
    if (truncated) {
        set_error(error, error_size, "truncated MP3 frame");
        return false;
    }
    if (!have_format || frames == 0U) {
        set_error(error, error_size, "no MPEG Layer III frame");
        return false;
    }

    out->sample_rate = hz;
    out->channels = (uint8_t)channels;
    out->frame_samples = (uint16_t)frame_samples;
    out->version = version;
    out->layer = 1U;
    out->total_frames = frames;
    out->total_samples = clamp_u32(samples);
    out->duration_ms = clamp_u32(samples * 1000ULL / hz);
    out->scan_us = (uint32_t)(index_now_us() - scan_start);
    if (out->duration_ms == 0U || out->entry_count == 0U) {
        set_error(error, error_size, "MP3 index empty");
        return false;
    }
    return true;
}

bool panel_mp3_index_build(FILE *file, uint32_t data_start, panel_mp3_index_t *out,
                           char *error, unsigned error_size)
{
    if (file == NULL) {
        set_error(error, error_size, "invalid file");
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        set_error(error, error_size, "seek end failed");
        return false;
    }
    long end = ftell(file);
    if (end < 0) {
        set_error(error, error_size, "tell failed");
        return false;
    }
    if (fseek(file, (long)data_start, SEEK_SET) != 0) {
        set_error(error, error_size, "seek start failed");
        return false;
    }
    file_read_ctx_t ctx = {.file = file, .cursor = data_start, .cursor_valid = true,
                           .io_error = false};
    bool ok = panel_mp3_index_build_leased(out, data_start, (uint32_t)end,
                                           read_file_at, NULL, &ctx, error, error_size);
    if (fseek(file, (long)data_start, SEEK_SET) != 0 && ok) {
        set_error(error, error_size, "seek reset failed");
        return false;
    }
    return ok;
}

bool panel_mp3_index_seek(const panel_mp3_index_t *index, uint32_t target_ms,
                          panel_mp3_seek_t *out)
{
    if (index == NULL || out == NULL || index->entry_count == 0U ||
        index->duration_ms == 0U || target_ms > index->duration_ms) {
        return false;
    }
    uint32_t last_time = index->entries[index->entry_count - 1U].time_ms;
    if (target_ms > last_time + PANEL_MP3_INDEX_COVERAGE_MARGIN_MS) {
        return false;
    }

    uint32_t lo = 0U;
    uint32_t hi = index->entry_count - 1U;
    uint32_t best = 0U;
    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2U;
        if (index->entries[mid].time_ms <= target_ms) {
            best = mid;
            lo = mid + 1U;
        } else {
            if (mid == 0U) break;
            hi = mid - 1U;
        }
    }
    if (best > PANEL_MP3_SEEK_PREROLL_ENTRIES) {
        best -= PANEL_MP3_SEEK_PREROLL_ENTRIES;
    } else {
        best = 0U;
    }
    uint32_t timeline = best;
    uint32_t warmup = timeline > PANEL_MP3_SEEK_WARMUP_ENTRIES
                      ? timeline - PANEL_MP3_SEEK_WARMUP_ENTRIES : 0U;

    out->byte_offset = index->entries[warmup].byte_offset;
    out->timeline_byte_offset = index->entries[timeline].byte_offset;
    out->frame_floor_ms = index->entries[timeline].time_ms;
    out->frame_floor_sample = index->entries[timeline].sample_index;
    out->target_ms = target_ms;
    out->frame_header = index->entries[warmup].frame_header;
    return true;
}
