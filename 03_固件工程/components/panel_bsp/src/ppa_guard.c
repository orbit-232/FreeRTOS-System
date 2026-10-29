#include "panel_ppa_guard.h"
#include "panel_touch_map.h"

#include <stddef.h>
#include <stdint.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_err.h"

static uint32_t s_fallback_count;

uint32_t panel_ppa_guard_fallback_count(void)
{
    return s_fallback_count;
}

static bool config_supported(const ppa_srm_oper_config_t *config)
{
    if (config == NULL || config->in.buffer == NULL || config->out.buffer == NULL) {
        return false;
    }
    /* This guard is local to this panel, not a general replacement for PPA.
     * Keep the original driver's alignment and non-overlap requirements. */
    if (config->out.pic_w != PANEL_NATIVE_WIDTH ||
        config->out.pic_h != PANEL_NATIVE_HEIGHT ||
        config->in.pic_w > PANEL_NATIVE_HEIGHT ||
        config->in.pic_h > PANEL_NATIVE_WIDTH ||
        ((uintptr_t)config->in.buffer & 1u) ||
        ((uintptr_t)config->out.buffer & 1u)) {
        return false;
    }
    size_t out_line = esp_cache_get_line_size_by_addr(config->out.buffer);
    if (out_line && (((uintptr_t)config->out.buffer % out_line) ||
                     (config->out.buffer_size % out_line))) {
        return false;
    }
    uint64_t in_start = (uintptr_t)config->in.buffer;
    uint64_t out_start = (uintptr_t)config->out.buffer;
    uint64_t in_end = in_start + (uint64_t)config->in.pic_w * config->in.pic_h * 2u;
    uint64_t out_end = out_start + config->out.buffer_size;
    if (in_end > UINTPTR_MAX || out_end > UINTPTR_MAX ||
        (in_start < out_end && out_start < in_end)) {
        return false;
    }
    if (config->in.srm_cm != PPA_SRM_COLOR_MODE_RGB565 ||
        config->out.srm_cm != PPA_SRM_COLOR_MODE_RGB565) {
        return false;
    }
    if (config->rotation_angle != PPA_SRM_ROTATION_ANGLE_90 ||
        config->scale_x != 1.0f || config->scale_y != 1.0f ||
        config->mirror_x || config->mirror_y ||
        config->rgb_swap || config->byte_swap ||
        config->alpha_update_mode != PPA_ALPHA_NO_CHANGE ||
        config->mode != PPA_TRANS_MODE_BLOCKING ||
        config->user_data != NULL) {
        return false;
    }
    if (!panel_ppa_guard_block_supported(config->in.block_h) ||
        config->in.block_w == 0 || config->in.pic_w == 0 || config->in.pic_h == 0 ||
        config->out.pic_w == 0 || config->out.pic_h == 0) {
        return false;
    }
    if (config->in.block_offset_x > config->in.pic_w ||
        config->in.block_offset_y > config->in.pic_h ||
        config->in.block_w > config->in.pic_w - config->in.block_offset_x ||
        config->in.block_h > config->in.pic_h - config->in.block_offset_y) {
        return false;
    }
    if (config->out.block_offset_x > config->out.pic_w ||
        config->out.block_offset_y > config->out.pic_h ||
        config->in.block_h > config->out.pic_w - config->out.block_offset_x ||
        config->in.block_w > config->out.pic_h - config->out.block_offset_y) {
        return false;
    }
    uint64_t out_bytes = (uint64_t)config->out.pic_w * config->out.pic_h * 2u;
    if (out_bytes > config->out.buffer_size) {
        return false;
    }
    return true;
}

static esp_err_t cache_range(void *addr, size_t len, int flags)
{
    size_t line = esp_cache_get_line_size_by_addr(addr);
    if (line == 0) {
        return ESP_OK;
    }
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(line - 1u);
    uintptr_t end = ((uintptr_t)addr + len + line - 1u) & ~(uintptr_t)(line - 1u);
    return esp_cache_msync((void *)start, (size_t)(end - start), flags);
}

static esp_err_t cpu_fallback(const ppa_srm_oper_config_t *config)
{
    const size_t out_stride = (size_t)config->out.pic_w * 2u;
    const size_t row_bytes = (size_t)config->in.block_h * 2u;
    uint8_t *out = (uint8_t *)config->out.buffer;

    /* Match the PPA contract: make the destination lines coherent with memory
     * before CPU writes, then write back only the touched rows. */
    for (uint32_t dy = 0; dy < config->in.block_w; dy++) {
        uint8_t *row = out + (size_t)(config->out.block_offset_y + dy) * out_stride +
                       (size_t)config->out.block_offset_x * 2u;
        esp_err_t err = cache_range(row, row_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (!panel_ppa_guard_copy_rgb565_rot90(
            (const uint16_t *)config->in.buffer,
            (uint16_t *)config->out.buffer,
            config->in.pic_w, config->in.pic_h,
            config->in.block_offset_x, config->in.block_offset_y,
            config->in.block_w, config->in.block_h,
            config->out.pic_w, config->out.pic_h,
            config->out.block_offset_x, config->out.block_offset_y)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t dy = 0; dy < config->in.block_w; dy++) {
        uint8_t *row = out + (size_t)(config->out.block_offset_y + dy) * out_stride +
                       (size_t)config->out.block_offset_x * 2u;
        esp_err_t err = cache_range(row, row_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        if (err != ESP_OK) {
            return err;
        }
    }

    s_fallback_count++;
    return ESP_OK;
}

esp_err_t __real_ppa_do_scale_rotate_mirror(ppa_client_handle_t ppa_client,
                                            const ppa_srm_oper_config_t *config);

esp_err_t __wrap_ppa_do_scale_rotate_mirror(ppa_client_handle_t ppa_client,
                                            const ppa_srm_oper_config_t *config)
{
    if (ppa_client == NULL || !config_supported(config)) {
        return __real_ppa_do_scale_rotate_mirror(ppa_client, config);
    }
    return cpu_fallback(config);
}
