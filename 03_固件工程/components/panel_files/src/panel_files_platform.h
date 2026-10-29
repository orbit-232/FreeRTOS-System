#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct panel_files_platform_dir panel_files_platform_dir_t;

typedef struct {
    char name[256];
    bool name_truncated;
    bool is_dir;
    uint64_t size;
    uint32_t mtime;
} panel_files_platform_dirent_t;

esp_err_t panel_files_platform_mount(void);
void panel_files_platform_unmount(void);
bool panel_files_platform_mounted(void);
const char *panel_files_platform_root(void);

esp_err_t panel_files_platform_opendir(const char *path, panel_files_platform_dir_t **out);
esp_err_t panel_files_platform_readdir(panel_files_platform_dir_t *dir,
                                       panel_files_platform_dirent_t *out);
void panel_files_platform_closedir(panel_files_platform_dir_t *dir);
esp_err_t panel_files_platform_stat(const char *path, bool *is_dir, uint64_t *size,
                                    uint32_t *mtime);
esp_err_t panel_files_platform_read(const char *path, char *buf, size_t cap,
                                    size_t *out_len, bool *truncated);

#ifdef __cplusplus
}
#endif
