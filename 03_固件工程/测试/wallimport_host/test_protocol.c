#include "wallimport_protocol.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    uint32_t crc = 0;
    CHECK(wallimport_parse_begin("WALLIMPORT BEGIN 1280 800 1 2048000 f7973f48", &crc));
    CHECK(crc == 0xf7973f48U);
    CHECK(!wallimport_parse_begin("WALLIMPORT BEGIN 1279 800 1 2048000 f7973f48", &crc));
    CHECK(!wallimport_parse_begin("WALLIMPORT BEGIN 1280 800 1 2047999 f7973f48", &crc));

    uint8_t payload[WALLIMPORT_CHUNK_BYTES];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 131U + 7U);
    uint32_t payload_crc = wallimport_crc32(payload, sizeof(payload));
    char line[2304];
    int n = snprintf(line, sizeof(line), "WALLIMPORT DATA 0 %08x ", (unsigned)payload_crc);
    CHECK(n > 0);
    for (size_t i = 0; i < sizeof(payload); i++) {
        snprintf(line + n, sizeof(line) - (size_t)n, "%02x", payload[i]);
        n += 2;
    }
    line[n] = '\0';

    uint8_t decoded[WALLIMPORT_CHUNK_BYTES];
    uint32_t seq = 0;
    uint32_t got_crc = 0;
    size_t len = 0;
    CHECK(wallimport_parse_data(line, &seq, &got_crc, decoded, &len));
    CHECK(seq == 0 && len == sizeof(payload) && got_crc == payload_crc);
    CHECK(memcmp(decoded, payload, sizeof(payload)) == 0);
    CHECK(wallimport_crc32(decoded, len) == got_crc);
    CHECK(!wallimport_parse_data("WALLIMPORT DATA 0 00000000 zz", &seq, &got_crc, decoded, &len));
    CHECK(!wallimport_parse_data("WALLIMPORT DATA 0 00000000 0", &seq, &got_crc, decoded, &len));

    CHECK(wallimport_seq_classify(0, UINT32_MAX, 0) == WALLIMPORT_SEQ_EXPECTED);
    CHECK(wallimport_seq_classify(1, 0, 0) == WALLIMPORT_SEQ_DUPLICATE);
    CHECK(wallimport_seq_classify(1, 0, 2) == WALLIMPORT_SEQ_OUT_OF_ORDER);
    CHECK(WALLIMPORT_CHUNK_COUNT == 2000U);
    printf("WALLIMPORT_C_PROTOCOL_TESTS_OK chunk=%u count=%u\n",
           (unsigned)WALLIMPORT_CHUNK_BYTES, (unsigned)WALLIMPORT_CHUNK_COUNT);
    return 0;
}
