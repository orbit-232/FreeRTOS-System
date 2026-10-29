#include "panel_ppa_guard.h"

#include <stddef.h>

bool panel_ppa_guard_block_supported(uint32_t block_h)
{
    return block_h > 0 && block_h < PANEL_PPA_GUARD_MAX_BLOCK_H;
}

bool panel_ppa_guard_map_rot90(uint32_t block_w, uint32_t block_h,
                               uint32_t x, uint32_t y,
                               uint32_t out_offset_x, uint32_t out_offset_y,
                               uint32_t *dest_x, uint32_t *dest_y)
{
    if (dest_x == NULL || dest_y == NULL || block_w == 0 || block_h == 0 ||
        x >= block_w || y >= block_h) {
        return false;
    }
    *dest_x = out_offset_x + y;
    *dest_y = out_offset_y + (block_w - 1 - x);
    return true;
}

bool panel_ppa_guard_copy_rgb565_rot90(const uint16_t *src, uint16_t *dst,
                                       uint32_t in_pic_w, uint32_t in_pic_h,
                                       uint32_t in_offset_x, uint32_t in_offset_y,
                                       uint32_t block_w, uint32_t block_h,
                                       uint32_t out_pic_w, uint32_t out_pic_h,
                                       uint32_t out_offset_x, uint32_t out_offset_y)
{
    if (src == NULL || dst == NULL || in_pic_w == 0 || in_pic_h == 0 ||
        out_pic_w == 0 || out_pic_h == 0 || block_w == 0 ||
        !panel_ppa_guard_block_supported(block_h)) {
        return false;
    }
    if (in_offset_x > in_pic_w || in_offset_y > in_pic_h ||
        block_w > in_pic_w - in_offset_x || block_h > in_pic_h - in_offset_y) {
        return false;
    }
    if (out_offset_x > out_pic_w || out_offset_y > out_pic_h ||
        block_h > out_pic_w - out_offset_x || block_w > out_pic_h - out_offset_y) {
        return false;
    }

    for (uint32_t x = 0; x < block_w; x++) {
        uint32_t dest_y = out_offset_y + (block_w - 1 - x);
        for (uint32_t y = 0; y < block_h; y++) {
            uint32_t dest_x = out_offset_x + y;
            dst[(size_t)dest_y * out_pic_w + dest_x] =
                src[(size_t)(in_offset_y + y) * in_pic_w + in_offset_x + x];
        }
    }
    return true;
}
