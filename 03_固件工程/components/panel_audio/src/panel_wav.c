#include "panel_wav.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool panel_wav_make_header(uint8_t out[44], uint32_t data_bytes)
{
    if (out == NULL || data_bytes == 0 || data_bytes > PANEL_WAV_MAX_DATA_BYTES ||
        (data_bytes & 1U) != 0) {
        return false;
    }
    memset(out, 0, 44);
    memcpy(out, "RIFF", 4);
    uint32_t riff = 36U + data_bytes;
    out[4] = (uint8_t)riff;
    out[5] = (uint8_t)(riff >> 8);
    out[6] = (uint8_t)(riff >> 16);
    out[7] = (uint8_t)(riff >> 24);
    memcpy(out + 8, "WAVEfmt ", 8);
    out[16] = 16;
    out[20] = 1; /* PCM */
    out[22] = (uint8_t)PANEL_WAV_CHANNELS;
    uint32_t rate = PANEL_WAV_SAMPLE_RATE;
    uint32_t byte_rate = rate * PANEL_WAV_CHANNELS * (PANEL_WAV_BITS / 8U);
    uint16_t block_align = (uint16_t)(PANEL_WAV_CHANNELS * (PANEL_WAV_BITS / 8U));
    out[24] = (uint8_t)rate;
    out[25] = (uint8_t)(rate >> 8);
    out[26] = (uint8_t)(rate >> 16);
    out[27] = (uint8_t)(rate >> 24);
    out[28] = (uint8_t)byte_rate;
    out[29] = (uint8_t)(byte_rate >> 8);
    out[30] = (uint8_t)(byte_rate >> 16);
    out[31] = (uint8_t)(byte_rate >> 24);
    out[32] = (uint8_t)block_align;
    out[33] = (uint8_t)(block_align >> 8);
    out[34] = (uint8_t)PANEL_WAV_BITS;
    memcpy(out + 36, "data", 4);
    out[40] = (uint8_t)data_bytes;
    out[41] = (uint8_t)(data_bytes >> 8);
    out[42] = (uint8_t)(data_bytes >> 16);
    out[43] = (uint8_t)(data_bytes >> 24);
    return true;
}

bool panel_wav_parse(const uint8_t *buf, size_t len, panel_wav_info_t *out)
{
    if (buf == NULL || out == NULL || len < 44 || len > PANEL_WAV_MAX_DATA_BYTES + 1024U) {
        return false;
    }
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return false;
    }
    uint32_t riff_size = rd32(buf + 4);
    if (riff_size != len - 8U) return false;

    bool have_fmt = false;
    bool have_data = false;
    uint32_t data_off = 0;
    uint32_t data_size = 0;
    size_t off = 12;
    while (off + 8U <= len) {
        const uint8_t *chunk = buf + off;
        uint32_t chunk_size = rd32(chunk + 4);
        if (chunk_size > len - off - 8U) return false;
        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16U) return false;
            const uint8_t *fmt = chunk + 8;
            if (rd16(fmt) != 1U || rd16(fmt + 2) != PANEL_WAV_CHANNELS ||
                rd32(fmt + 4) != PANEL_WAV_SAMPLE_RATE ||
                rd32(fmt + 8) != PANEL_WAV_SAMPLE_RATE * PANEL_WAV_CHANNELS *
                                         (PANEL_WAV_BITS / 8U) ||
                rd16(fmt + 12) != PANEL_WAV_CHANNELS * (PANEL_WAV_BITS / 8U) ||
                rd16(fmt + 14) != PANEL_WAV_BITS) {
                return false;
            }
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (chunk_size == 0 || chunk_size > PANEL_WAV_MAX_DATA_BYTES ||
                (chunk_size & 1U) != 0) {
                return false;
            }
            data_off = (uint32_t)(off + 8U);
            data_size = chunk_size;
            have_data = true;
        }
        size_t advance = 8U + chunk_size + (chunk_size & 1U);
        if (advance > len - off) return false;
        off += advance;
    }
    if (!have_fmt || !have_data || data_off + data_size > len) return false;

    out->data = buf + data_off;
    out->data_bytes = data_size;
    out->frames = data_size / 2U;
    return true;
}
