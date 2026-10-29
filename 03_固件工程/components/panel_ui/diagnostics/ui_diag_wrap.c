/*
 * Diagnostic-only --wrap probes for the display flush path.
 *
 * These wrappers live in panel_ui (not managed_components) and are enabled by
 * PANEL_UI_DIAG_WRAP in CMake.  They do not change behavior or print per
 * frame; each enter/exit only updates the existing ui_perf_diag stage, whose
 * 5-second timer emits NAVBENCH_DIAG when the stage stops moving.
 */
#include "driver/ppa.h"
#include "esp_err.h"
#include "ui_perf.h"

void *__real_display_bridge_pipeline_wait_free_buf(void *pipeline);
esp_err_t __real_display_bridge_dma2d_copy_sync(void *trans_desc, uint32_t timeout_ms);
esp_err_t __real_esp_lcd_panel_draw_bitmap(void *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data);
esp_err_t __real_ppa_do_scale_rotate_mirror(ppa_client_handle_t ppa_client,
                                            const ppa_srm_oper_config_t *config);

void *__wrap_display_bridge_pipeline_wait_free_buf(void *pipeline)
{
    ui_perf_diag_mark("wrap-pipeline-wait-enter");
    void *result = __real_display_bridge_pipeline_wait_free_buf(pipeline);
    ui_perf_diag_mark("wrap-pipeline-wait-exit");
    return result;
}

esp_err_t __wrap_display_bridge_dma2d_copy_sync(void *trans_desc, uint32_t timeout_ms)
{
    ui_perf_diag_mark("wrap-dma2d-sync-enter");
    esp_err_t result = __real_display_bridge_dma2d_copy_sync(trans_desc, timeout_ms);
    ui_perf_diag_mark("wrap-dma2d-sync-exit");
    return result;
}

esp_err_t __wrap_esp_lcd_panel_draw_bitmap(void *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data)
{
    ui_perf_diag_mark("wrap-panel-draw-enter");
    esp_err_t result = __real_esp_lcd_panel_draw_bitmap(panel, x_start, y_start,
                                                        x_end, y_end, color_data);
    ui_perf_diag_mark("wrap-panel-draw-exit");
    return result;
}

static uint32_t ppa_bpp(ppa_srm_color_mode_t color)
{
    switch (color) {
    case PPA_SRM_COLOR_MODE_ARGB8888:
        return 4;
    case PPA_SRM_COLOR_MODE_RGB888:
        return 3;
    case PPA_SRM_COLOR_MODE_RGB565:
        return 2;
    case PPA_SRM_COLOR_MODE_GRAY8:
        return 1;
    default:
        return 0;
    }
}

esp_err_t __wrap_ppa_do_scale_rotate_mirror(ppa_client_handle_t ppa_client,
                                            const ppa_srm_oper_config_t *config)
{
    ui_perf_diag_ppa_t snapshot = {0};
    if (config != NULL) {
        snapshot.client = (uintptr_t)ppa_client;
        snapshot.in_buffer = (uintptr_t)config->in.buffer;
        snapshot.in_pic_w = config->in.pic_w;
        snapshot.in_pic_h = config->in.pic_h;
        snapshot.in_block_w = config->in.block_w;
        snapshot.in_block_h = config->in.block_h;
        snapshot.in_offset_x = config->in.block_offset_x;
        snapshot.in_offset_y = config->in.block_offset_y;
        snapshot.in_color = (uint32_t)config->in.srm_cm;
        snapshot.in_bpp = ppa_bpp(config->in.srm_cm);
        snapshot.in_stride_bytes = config->in.pic_w * snapshot.in_bpp;
        snapshot.out_buffer = (uintptr_t)config->out.buffer;
        snapshot.out_pic_w = config->out.pic_w;
        snapshot.out_pic_h = config->out.pic_h;
        bool swap_block = config->rotation_angle == PPA_SRM_ROTATION_ANGLE_90 ||
                          config->rotation_angle == PPA_SRM_ROTATION_ANGLE_270;
        snapshot.out_block_w = swap_block ? config->in.block_h : config->in.block_w;
        snapshot.out_block_h = swap_block ? config->in.block_w : config->in.block_h;
        snapshot.out_offset_x = config->out.block_offset_x;
        snapshot.out_offset_y = config->out.block_offset_y;
        snapshot.out_color = (uint32_t)config->out.srm_cm;
        snapshot.out_bpp = ppa_bpp(config->out.srm_cm);
        snapshot.out_stride_bytes = config->out.pic_w * snapshot.out_bpp;
        snapshot.out_buffer_size = config->out.buffer_size;
        snapshot.rotation = (uint32_t)config->rotation_angle;
        snapshot.mirror_x = config->mirror_x ? 1 : 0;
        snapshot.mirror_y = config->mirror_y ? 1 : 0;
        snapshot.mode = (uint32_t)config->mode;
        snapshot.scale_x_milli = (int32_t)(config->scale_x * 1000.0f);
        snapshot.scale_y_milli = (int32_t)(config->scale_y * 1000.0f);
    }
    ui_perf_diag_ppa_enter(&snapshot);
    esp_err_t result = __real_ppa_do_scale_rotate_mirror(ppa_client, config);
    ui_perf_diag_ppa_exit((int)result);
    return result;
}
