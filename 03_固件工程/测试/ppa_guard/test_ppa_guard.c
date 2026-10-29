#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "panel_ppa_guard.h"

#define ARRAY_W 32
#define ARRAY_H 32

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void fill_src(uint16_t *src, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            src[(size_t)y * w + x] = (uint16_t)(0x1000u + y * 37u + x);
        }
    }
}

static void fill_dst(uint16_t *dst, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        dst[i] = 0xDEADu;
    }
}

static void test_h(uint32_t block_h)
{
    const uint32_t in_w = 13, in_h = 20;
    const uint32_t out_w = 24, out_h = 24;
    const uint32_t block_w = 7;
    const uint32_t in_off_x = 2, in_off_y = 3;
    const uint32_t out_off_x = 5, out_off_y = 6;
    uint16_t src[ARRAY_W * ARRAY_H];
    uint16_t dst[ARRAY_W * ARRAY_H];
    uint16_t expected[ARRAY_W * ARRAY_H];
    const size_t cells = (size_t)out_w * out_h;

    check((size_t)in_w * in_h <= sizeof(src) / sizeof(src[0]), "src array");
    check((size_t)out_w * out_h <= sizeof(dst) / sizeof(dst[0]), "dst array");
    fill_src(src, in_w, in_h);
    fill_dst(dst, cells);
    fill_dst(expected, cells);

    for (uint32_t x = 0; x < block_w; x++) {
        for (uint32_t y = 0; y < block_h; y++) {
            uint32_t dx, dy;
            check(panel_ppa_guard_map_rot90(block_w, block_h, x, y,
                                            out_off_x, out_off_y, &dx, &dy),
                  "map_rot90 accepts in-range pixel");
            expected[(size_t)dy * out_w + dx] =
                src[(size_t)(in_off_y + y) * in_w + in_off_x + x];
        }
    }

    check(panel_ppa_guard_copy_rgb565_rot90(src, dst, in_w, in_h,
                                            in_off_x, in_off_y, block_w, block_h,
                                            out_w, out_h, out_off_x, out_off_y),
          "copy accepts supported block");
    check(memcmp(dst, expected, cells * sizeof(dst[0])) == 0,
          "copy pixels match rot90 expected mapping");

    uint32_t changed = 0;
    for (size_t i = 0; i < cells; i++) {
        if (dst[i] != 0xDEADu) {
            changed++;
        }
    }
    check(changed == block_w * block_h, "copy writes exactly the block");
}

int main(void)
{
    check(panel_ppa_guard_block_supported(1), "h=1 supported");
    check(panel_ppa_guard_block_supported(2), "h=2 supported");
    check(panel_ppa_guard_block_supported(3), "h=3 supported");
    check(panel_ppa_guard_block_supported(15), "h=15 supported");
    check(!panel_ppa_guard_block_supported(16), "h=16 rejected");
    check(!panel_ppa_guard_block_supported(0), "h=0 rejected");

    test_h(1);
    test_h(2);
    test_h(3);
    test_h(15);

    {
        const uint32_t in_w = 10, in_h = 10;
        const uint32_t out_w = 10, out_h = 10;
        uint16_t src[ARRAY_W * ARRAY_H];
        uint16_t dst[ARRAY_W * ARRAY_H];
        fill_src(src, in_w, in_h);
        fill_dst(dst, sizeof(dst) / sizeof(dst[0]));

        /* Exact fit at nonzero offsets. */
        check(panel_ppa_guard_copy_rgb565_rot90(src, dst, in_w, in_h, 3, 7, 7, 3,
                                                out_w, out_h, 7, 3),
              "exact in/out boundary accepted");

        /* One pixel past either input edge must be rejected without writes. */
        fill_dst(dst, sizeof(dst) / sizeof(dst[0]));
        check(!panel_ppa_guard_copy_rgb565_rot90(src, dst, in_w, in_h, 4, 7, 7, 3,
                                                 out_w, out_h, 7, 3),
              "input x overflow rejected");
        check(!panel_ppa_guard_copy_rgb565_rot90(src, dst, in_w, in_h, 3, 8, 7, 3,
                                                 out_w, out_h, 7, 3),
              "input y overflow rejected");
        check(!panel_ppa_guard_copy_rgb565_rot90(src, dst, in_w, in_h, 3, 7, 7, 3,
                                                 out_w, out_h, 8, 3),
              "output x overflow rejected");
        check(!panel_ppa_guard_copy_rgb565_rot90(src, dst, in_w, in_h, 3, 7, 7, 3,
                                                 out_w, out_h, 7, 4),
              "output y overflow rejected");
        check(!panel_ppa_guard_copy_rgb565_rot90(NULL, dst, in_w, in_h, 3, 7, 7, 3,
                                                 out_w, out_h, 7, 3),
              "null input rejected");
        check(!panel_ppa_guard_copy_rgb565_rot90(src, NULL, in_w, in_h, 3, 7, 7, 3,
                                                 out_w, out_h, 7, 3),
              "null output rejected");
        check(!panel_ppa_guard_copy_rgb565_rot90(src, dst, in_w, in_h, 3, 7, 0, 3,
                                                 out_w, out_h, 7, 3),
              "zero width rejected");
    }

    {
        uint32_t dx = 0, dy = 0;
        check(!panel_ppa_guard_map_rot90(7, 3, 7, 0, 5, 6, &dx, &dy),
              "map rejects x out of range");
        check(!panel_ppa_guard_map_rot90(7, 3, 0, 3, 5, 6, &dx, &dy),
              "map rejects y out of range");
        check(!panel_ppa_guard_map_rot90(7, 3, 0, 0, 5, 6, NULL, &dy),
              "map rejects null dx");
    }

    if (failures != 0) {
        fprintf(stderr, "PPA_GUARD_TEST failures=%d\n", failures);
        return 1;
    }
    printf("PPA_GUARD_TEST PASS h=1,2,3,15; h=16 rejected; stride/offset/boundary/pixels\n");
    return 0;
}
