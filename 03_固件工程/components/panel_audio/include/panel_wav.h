#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PANEL_WAV_SAMPLE_RATE 22050U
#define PANEL_WAV_CHANNELS 1U
#define PANEL_WAV_BITS 16U
#define PANEL_WAV_MAX_DATA_BYTES (64U * 1024U)

typedef struct {
    const uint8_t *data;
    uint32_t data_bytes;
    uint32_t frames;
} panel_wav_info_t;

/* Strict PCM-only parser: RIFF/WAVE, format 1, mono, 22050 Hz, 16-bit,
 * little-endian, data <= 64 KiB, exact RIFF length and chunk bounds. */
bool panel_wav_parse(const uint8_t *buf, size_t len, panel_wav_info_t *out);
bool panel_wav_make_header(uint8_t out[44], uint32_t data_bytes);
