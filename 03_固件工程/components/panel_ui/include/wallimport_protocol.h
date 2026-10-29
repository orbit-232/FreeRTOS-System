#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WALLIMPORT_CHUNK_BYTES 1024U
#define WALLIMPORT_CHUNK_COUNT 2000U
#define WALLIMPORT_TOTAL_BYTES 2048000U

typedef enum {
    WALLIMPORT_SEQ_EXPECTED = 0,
    WALLIMPORT_SEQ_DUPLICATE,
    WALLIMPORT_SEQ_OUT_OF_ORDER,
} wallimport_seq_t;

uint32_t wallimport_crc32(const uint8_t *data, size_t len);
uint32_t wallimport_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
bool wallimport_parse_begin(const char *line, uint32_t *crc);
bool wallimport_parse_data(const char *line, uint32_t *seq, uint32_t *crc, uint8_t *out,
                           size_t *out_len);
wallimport_seq_t wallimport_seq_classify(uint32_t expected, uint32_t last_acked, uint32_t seq);
