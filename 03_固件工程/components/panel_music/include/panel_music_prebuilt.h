#pragma once

#include "panel_music_index.h"

#define PANEL_MUSIC_PREBUILT_TRACKS 4U
/* Sampled fingerprints: they are not a whole-file hash.  Modifications outside
 * these regions are not detected by the fast match and intentionally require
 * the full 16 KiB scan if any sampled region or the size/tag differs. */
#define PANEL_MUSIC_FINGERPRINT_BYTES 64U
#define PANEL_MUSIC_FINGERPRINT_COUNT 16U

typedef struct {
    uint32_t offset;
    uint32_t hash;
    uint16_t len;
    uint16_t reserved;
} panel_music_fingerprint_t;

typedef struct {
    uint32_t file_size;
    uint32_t tag_bytes;
    uint32_t data_start;
    uint32_t first_frame_offset;
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint32_t total_frames;
    uint32_t total_samples;
    uint32_t entry_count;
    uint32_t fingerprint_count;
    uint16_t frame_samples;
    uint8_t channels;
    uint8_t version;
    uint8_t layer;
    uint8_t has_id3;
    const panel_mp3_index_entry_t *entries;
    const panel_music_fingerprint_t *fingerprints;
} panel_music_prebuilt_track_t;

extern const panel_music_prebuilt_track_t
    panel_music_prebuilt_tracks[PANEL_MUSIC_PREBUILT_TRACKS];
