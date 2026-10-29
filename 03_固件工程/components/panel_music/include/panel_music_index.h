#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Sparse byte index for real MP3 seek.  Entries are built from actual frame
 * boundaries (not a byte-rate estimate), so VBR files map correctly. */
#define PANEL_MP3_INDEX_MAX 2048U
#define PANEL_MP3_INDEX_IO_BUF (16U * 1024U)
#define PANEL_MP3_INDEX_STRIDE 8U
#define PANEL_MP3_INDEX_STRIDE_MAX 64U
#define PANEL_MP3_SEEK_PREROLL_ENTRIES 1U
#define PANEL_MP3_SEEK_WARMUP_ENTRIES 2U
#define PANEL_MP3_SEEK_TOLERANCE_MS 50U
#define PANEL_MP3_INDEX_COVERAGE_MARGIN_MS 3000U

typedef struct {
    uint32_t byte_offset;
    uint32_t frame_index;
    uint32_t time_ms;
    uint32_t sample_index;
    uint32_t frame_header;
} panel_mp3_index_entry_t;

typedef struct {
    uint32_t data_start;
    uint32_t first_frame_offset;
    uint32_t total_frames;
    uint32_t total_samples;
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint32_t entry_count;
    uint32_t scan_us;
    uint32_t read_calls;
    uint32_t bytes_read;
    uint32_t max_read_us;
    uint16_t frame_samples;
    uint8_t channels;
    uint8_t version;
    uint8_t layer;
    panel_mp3_index_entry_t entries[PANEL_MP3_INDEX_MAX];
} panel_mp3_index_t;

typedef struct {
    uint32_t byte_offset;
    uint32_t timeline_byte_offset;
    uint32_t frame_floor_ms;
    uint32_t frame_floor_sample;
    uint32_t target_ms;
    uint32_t frame_header;
} panel_mp3_seek_t;

/* Random read at an absolute file offset; at most len bytes.  Set *io_error
 * only for a real I/O failure, not clean EOF.  The callback must lease the TF
 * I/O lock for the read only, never for the entire scan. */
typedef size_t (*panel_mp3_read_at_t)(void *ctx, uint32_t offset, uint8_t *buf,
                                      size_t len, bool *io_error);
/* Polled before every 16 KiB refill; return true to abandon the scan. */
typedef bool (*panel_mp3_cancel_t)(void *ctx);

bool panel_mp3_index_build_leased(panel_mp3_index_t *out, uint32_t data_start,
                                  uint32_t file_size, panel_mp3_read_at_t read_at,
                                  panel_mp3_cancel_t cancel, void *ctx,
                                  char *error, unsigned error_size);

/* FILE-based convenience wrapper for host tests. */
bool panel_mp3_index_build(FILE *file, uint32_t data_start, panel_mp3_index_t *out,
                           char *error, unsigned error_size);

/* Resolve a timestamp to a real frame boundary.  Returns false when the file
 * contains no usable frames, the timestamp is outside the file, or it is
 * beyond the sparse index coverage. */
bool panel_mp3_index_seek(const panel_mp3_index_t *index, uint32_t target_ms,
                          panel_mp3_seek_t *out);

const char *panel_mp3_version_name(uint8_t version);
