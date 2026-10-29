#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_ASSETS_SCHEMA "panel-assets-v1"
#define PANEL_ASSETS_PACK_VERSION 1
#define PANEL_ASSETS_MIN_FIRMWARE "1.2.0-apps-b3"
#define PANEL_ASSETS_FONT_NAME "file_cjk_20"
#define PANEL_ASSETS_MAX_MANIFEST_BYTES (16U * 1024U)
#define PANEL_ASSETS_MAX_FONT_BYTES (2U * 1024U * 1024U)
#define PANEL_ASSETS_MAX_GLYPHS 12000U
#define PANEL_ASSETS_MAX_PATH_BYTES 160U

typedef enum {
    PANEL_ASSETS_STATE_IDLE = 0,
    PANEL_ASSETS_STATE_LOADING,
    PANEL_ASSETS_STATE_READY,
    PANEL_ASSETS_STATE_NO_CARD,
    PANEL_ASSETS_STATE_MISSING,
    PANEL_ASSETS_STATE_INVALID,
    PANEL_ASSETS_STATE_CORRUPT,
    PANEL_ASSETS_STATE_NO_MEM,
    PANEL_ASSETS_STATE_IO_ERROR,
} panel_assets_state_t;

typedef struct {
    panel_assets_state_t state;
    char detail[96];
    uint32_t pack_version;
    char font_name[32];
    size_t font_file_bytes;
    size_t font_ram_bytes;
    bool hash_checked;
    unsigned refs;
} panel_assets_status_t;

esp_err_t panel_assets_init(void);

/* Ask the worker to load the single external file-view font.  Safe to call from
 * the LVGL task: it only queues work, never performs card I/O there. */
esp_err_t panel_assets_request_font(void);
void panel_assets_cancel_font(void);

/* Immutable font with an explicit reference.  Returns NULL unless READY. */
const lv_font_t *panel_assets_acquire_font(void);
void panel_assets_release_font(void);
void panel_assets_get_status(panel_assets_status_t *out);
uint32_t panel_assets_worker_stack_free(void);

#ifdef __cplusplus
}
#endif
