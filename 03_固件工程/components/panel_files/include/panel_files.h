#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_FILES_MAX_ENTRIES 12
#define PANEL_FILES_MAX_PATH 256
#define PANEL_FILES_MAX_DEPTH 12
#define PANEL_FILES_PREVIEW_BYTES 6144
#define PANEL_FILES_SCAN_LIMIT 512
#define PANEL_FILES_NAME_BYTES 128

typedef enum {
    PANEL_FILES_STATE_INIT = 0,
    PANEL_FILES_STATE_MOUNTING,
    PANEL_FILES_STATE_READY,
    PANEL_FILES_STATE_NO_CARD,
    PANEL_FILES_STATE_ERROR,
} panel_files_state_t;

typedef struct {
    char name[PANEL_FILES_NAME_BYTES];
    bool is_dir;
    bool valid_name;
    bool valid_utf8;
    bool truncated_name;
    uint64_t size;
    uint32_t mtime;
} panel_files_entry_t;

typedef struct {
    panel_files_state_t state;
    char path[PANEL_FILES_MAX_PATH];
    char error[96];
    unsigned page;
    unsigned page_count;
    unsigned total_entries;
    unsigned count;
    bool truncated;
    uint32_t generation;
    panel_files_entry_t entries[PANEL_FILES_MAX_ENTRIES];

    bool preview_open;
    char preview_name[PANEL_FILES_NAME_BYTES];
    uint64_t preview_size;
    bool preview_truncated;
    bool preview_error;
    char preview_error_text[96];
    char preview_type[24];
    uint32_t preview_mtime;
    bool preview_mtime_valid;
    char preview_mtime_text[24];
    size_t preview_len;
    char preview[PANEL_FILES_PREVIEW_BYTES + 1];
} panel_files_snapshot_t;

esp_err_t panel_files_init(void);
void panel_files_get_snapshot(panel_files_snapshot_t *out);
esp_err_t panel_files_retry(void);
esp_err_t panel_files_refresh(uint32_t generation);
esp_err_t panel_files_enter(unsigned index, uint32_t generation);
esp_err_t panel_files_up(uint32_t generation);
esp_err_t panel_files_root(uint32_t generation);
esp_err_t panel_files_page(int delta, uint32_t generation);
esp_err_t panel_files_preview(unsigned index, uint32_t generation);
esp_err_t panel_files_close_preview(void);

/* True only while the SD/FAT mount owned by panel_files is currently usable.
 * The asset loader uses this as a readiness gate; it never mounts or unmounts. */
bool panel_files_card_ready(void);

/* Mount point exposed read-only for the asset loader. */
const char *panel_files_root_path(void);

/* Serializes TF mount/unmount against background asset reads.  The caller must
 * hold this only in a worker/task, never on the LVGL task. */
bool panel_files_io_acquire(void);
void panel_files_io_release(void);

/* Persistent mount pin for a streaming file handle. Hold from open through
 * close/read; it prevents unmount/retry while short I/O still uses the lease
 * above. A retry while pinned returns busy instead of unmounting under the
 * open handle. */
bool panel_files_mount_pin_acquire(void);
void panel_files_mount_pin_release(void);
bool panel_files_mount_pin_held(void);

/* Minimum free bytes observed on the panel_files worker task.  Read-only
 * diagnostic; never blocks and never performs I/O. */
uint32_t panel_files_worker_stack_free(void);

#ifdef __cplusplus
}
#endif
