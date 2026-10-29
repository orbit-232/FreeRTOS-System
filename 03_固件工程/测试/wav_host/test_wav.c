#include "panel_wav.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static size_t make_wav(uint8_t *buf, size_t cap, uint32_t frames, uint16_t format,
                       uint16_t channels, uint32_t rate, uint16_t bits)
{
    uint32_t data = frames * 2U;
    if (cap < 44U + data) return 0;
    memset(buf, 0, 44U + data);
    if (!panel_wav_make_header(buf, data)) return 0;
    for (uint32_t i = 0; i < frames; i++) put16(buf + 44U + i * 2U, (uint16_t)(i * 7U));
    return 44U + data;
}

int main(void)
{
    uint8_t wav[44 + 35280];
    size_t len = make_wav(wav, sizeof(wav), 17640U, 1, 1, 22050U, 16);
    CHECK(len == sizeof(wav));
    panel_wav_info_t info;
    CHECK(panel_wav_parse(wav, len, &info));
    CHECK(info.data_bytes == 35280U && info.frames == 17640U);
    CHECK(info.data == wav + 44);

    uint8_t bad[sizeof(wav)];
    memcpy(bad, wav, len);
    bad[0] = 'X';
    CHECK(!panel_wav_parse(bad, len, &info));
    memcpy(bad, wav, len);
    bad[20] = 3; /* IEEE float */
    CHECK(!panel_wav_parse(bad, len, &info));
    memcpy(bad, wav, len);
    put32(bad + 40, 35279U);
    CHECK(!panel_wav_parse(bad, len - 1U, &info));
    memcpy(bad, wav, len);
    put32(bad + 24, 44100U);
    CHECK(!panel_wav_parse(bad, len, &info));
    memcpy(bad, wav, len);
    put32(bad + 4, 36U + 35280U + 1U);
    CHECK(!panel_wav_parse(bad, len, &info));
    CHECK(!panel_wav_parse(wav, 43U, &info));

    uint8_t too_big[PANEL_WAV_MAX_DATA_BYTES + 2048];
    uint32_t big_data = PANEL_WAV_MAX_DATA_BYTES + 2U;
    size_t big_len = 44U + big_data;
    memset(too_big, 0, big_len);
    memcpy(too_big, "RIFF", 4);
    put32(too_big + 4, 36U + big_data);
    memcpy(too_big + 8, "WAVEfmt ", 8);
    put32(too_big + 16, 16U);
    put16(too_big + 20, 1U);
    put16(too_big + 22, 1U);
    put32(too_big + 24, 22050U);
    put32(too_big + 28, 44100U);
    put16(too_big + 32, 2U);
    put16(too_big + 34, 16U);
    memcpy(too_big + 36, "data", 4);
    put32(too_big + 40, big_data);
    CHECK(!panel_wav_parse(too_big, big_len, &info));
    printf("WAV_PARSER_TESTS_OK frames=%u bytes=%u\n",
           (unsigned)info.frames, (unsigned)info.data_bytes);
    return 0;
}
