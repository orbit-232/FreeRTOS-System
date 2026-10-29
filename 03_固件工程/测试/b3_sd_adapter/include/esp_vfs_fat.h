#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "driver/sdmmc_host.h"
#include "esp_err.h"

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_mount_config_t;

typedef esp_vfs_fat_mount_config_t esp_vfs_fat_sdmmc_mount_config_t;

esp_err_t esp_vfs_fat_sdmmc_mount(const char *base_path, const sdmmc_host_t *host_config,
                                  const void *slot_config,
                                  const esp_vfs_fat_mount_config_t *mount_config,
                                  sdmmc_card_t **out_card);
esp_err_t esp_vfs_fat_sdcard_unmount(const char *base_path, sdmmc_card_t *card);
