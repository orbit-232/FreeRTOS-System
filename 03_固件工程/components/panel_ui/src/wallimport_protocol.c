#include "wallimport_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t wallimport_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return crc;
}

uint32_t wallimport_crc32(const uint8_t *data, size_t len)
{
    return ~wallimport_crc32_update(0xFFFFFFFFU, data, len);
}

bool wallimport_parse_begin(const char *line, uint32_t *crc)
{
    unsigned width = 0;
    unsigned height = 0;
    unsigned format = 0;
    unsigned long bytes = 0;
    unsigned long value = 0;
    if (line == NULL || crc == NULL) return false;
    if (sscanf(line, "WALLIMPORT BEGIN %u %u %u %lu %lx", &width, &height, &format,
               &bytes, &value) != 5) {
        return false;
    }
    if (width != 1280U || height != 800U || format != 1U ||
        bytes != WALLIMPORT_TOTAL_BYTES) {
        return false;
    }
    *crc = (uint32_t)value;
    return true;
}

bool wallimport_parse_data(const char *line, uint32_t *seq, uint32_t *crc, uint8_t *out,
                           size_t *out_len)
{
    const char *p = line;
    if (p == NULL || seq == NULL || crc == NULL || out == NULL || out_len == NULL) return false;
    if (strncmp(p, "WALLIMPORT DATA ", 16) != 0) return false;
    p += 16;
    char *end = NULL;
    unsigned long s = strtoul(p, &end, 10);
    if (end == p || *end != ' ') return false;
    p = end + 1;
    unsigned long c = strtoul(p, &end, 16);
    if (end == p || *end != ' ') return false;
    p = end + 1;
    size_t hex_len = strlen(p);
    if (hex_len == 0 || (hex_len & 1U) != 0 || hex_len / 2U > WALLIMPORT_CHUNK_BYTES) {
        return false;
    }
    size_t len = hex_len / 2U;
    for (size_t i = 0; i < len; i++) {
        unsigned v = 0;
        for (int j = 0; j < 2; j++) {
            char h = p[i * 2 + j];
            unsigned digit;
            if (h >= '0' && h <= '9') digit = (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') digit = (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') digit = (unsigned)(h - 'A' + 10);
            else return false;
            v = (v << 4) | digit;
        }
        out[i] = (uint8_t)v;
    }
    *seq = (uint32_t)s;
    *crc = (uint32_t)c;
    *out_len = len;
    return true;
}

wallimport_seq_t wallimport_seq_classify(uint32_t expected, uint32_t last_acked, uint32_t seq)
{
    if (seq == last_acked && last_acked != UINT32_MAX) return WALLIMPORT_SEQ_DUPLICATE;
    if (seq == expected) return WALLIMPORT_SEQ_EXPECTED;
    return WALLIMPORT_SEQ_OUT_OF_ORDER;
}
