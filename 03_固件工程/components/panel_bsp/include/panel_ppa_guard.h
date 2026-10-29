#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PANEL_PPA_GUARD_ENABLED 1
#define PANEL_PPA_GUARD_MAX_BLOCK_H 16u

/* Narrow-strip CPU fallback predicates, kept free of ESP-IDF headers so the
 * mapping and pixel copy can be tested on the host. */
bool panel_ppa_guard_block_supported(uint32_t block_h);

bool panel_ppa_guard_map_rot90(uint32_t block_w, uint32_t block_h,
                               uint32_t x, uint32_t y,
                               uint32_t out_offset_x, uint32_t out_offset_y,
                               uint32_t *dest_x, uint32_t *dest_y);

bool panel_ppa_guard_copy_rgb565_rot90(const uint16_t *src, uint16_t *dst,
                                       uint32_t in_pic_w, uint32_t in_pic_h,
                                       uint32_t in_offset_x, uint32_t in_offset_y,
                                       uint32_t block_w, uint32_t block_h,
                                       uint32_t out_pic_w, uint32_t out_pic_h,
                                       uint32_t out_offset_x, uint32_t out_offset_y);

uint32_t panel_ppa_guard_fallback_count(void);
