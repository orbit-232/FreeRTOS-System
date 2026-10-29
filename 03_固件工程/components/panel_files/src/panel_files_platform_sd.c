#include "panel_files_platform.h"
#include "panel_files.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp/esp32_p4_wifi6_touch_lcd_x.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SD_PATH_BYTES (PANEL_FILES_MAX_PATH + 384)
#ifndef SD_HOST_READY_WAIT_MS
#define SD_HOST_READY_WAIT_MS 10000
#endif
#ifndef SD_HOST_POLL_MS
#define SD_HOST_POLL_MS 50
#endif

struct panel_files_platform_dir {
    DIR *dir;
    char path[SD_PATH_BYTES];
};

static const char *TAG = "panel_files_sd";
static sd_pwr_ctrl_handle_t s_pwr;
static sdmmc_card_t *s_card;
static bool s_vfs_mounted;

static esp_err_t host_init_shared(void)
{
    sdmmc_host_state_t state;
    memset(&state, 0, sizeof(state));
    esp_err_t err = sdmmc_host_get_state(&state);
    if (err != ESP_OK) return err;
    if (!state.host_initialized || state.num_of_init_slots < 1) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static esp_err_t host_deinit_slot0(int slot)
{
    /* Never touch slot 1: Hosted owns it.  Calling with an uninitialised slot0
     * is safe (sd_host_remove_slot(NULL) rejects it) and must not escalate to
     * sdmmc_host_deinit(), which would remove both slots and the controller. */
    if (slot != 0) return ESP_ERR_INVALID_ARG;
    return sdmmc_host_deinit_slot(0);
}

bool panel_files_platform_mounted(void) { return s_vfs_mounted; }
const char *panel_files_platform_root(void) { return BSP_SD_MOUNT_POINT; }

static void release_power(void)
{
    if (s_pwr != NULL) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
        s_pwr = NULL;
    }
}

esp_err_t panel_files_platform_mount(void)
{
    if (s_vfs_mounted) return ESP_OK;

    bool ready = false;
    int waited = 0;
    while (waited <= SD_HOST_READY_WAIT_MS) {
        sdmmc_host_state_t state;
        memset(&state, 0, sizeof(state));
        if (sdmmc_host_get_state(&state) == ESP_OK && state.host_initialized &&
            state.num_of_init_slots >= 1) {
            ready = true;
            break;
        }
        if (waited >= SD_HOST_READY_WAIT_MS) break;
        vTaskDelay(pdMS_TO_TICKS(SD_HOST_POLL_MS));
        waited += SD_HOST_POLL_MS;
    }
    if (!ready) return ESP_ERR_INVALID_STATE;

    /* One persistent LDO handle across attempts in this mount call; every exit
     * path either keeps it for the successful mount or deletes it. */
    if (s_pwr == NULL) {
        sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = 4};
        esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
        if (err != ESP_OK) {
            s_pwr = NULL;
            ESP_LOGW(TAG, "LDO4 power control create failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = 0;
    host.init = host_init_shared;
    host.deinit_p = host_deinit_slot0;
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.flags = 0;
    slot.clk = BSP_SD_CLK;
    slot.cmd = BSP_SD_CMD;
    slot.d0 = BSP_SD_D0;
    slot.d1 = BSP_SD_D1;
    slot.d2 = BSP_SD_D2;
    slot.d3 = BSP_SD_D3;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    s_card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot, &cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        s_vfs_mounted = false;
        release_power();
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_vfs_mounted = true;
    return ESP_OK;
}

void panel_files_platform_unmount(void)
{
    if (s_vfs_mounted) {
        if (s_card != NULL) {
            esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
        }
        s_vfs_mounted = false;
    }
    s_card = NULL;
    release_power();
}

static esp_err_t errno_to_esp(int err)
{
    if (err == ENOENT || err == ENOTDIR) return ESP_ERR_NOT_FOUND;
    return ESP_FAIL;
}

esp_err_t panel_files_platform_opendir(const char *path, panel_files_platform_dir_t **out)
{
    if (path == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    if (strlen(path) >= SD_PATH_BYTES) return ESP_ERR_INVALID_SIZE;
    panel_files_platform_dir_t *wrap = calloc(1, sizeof(*wrap));
    if (wrap == NULL) return ESP_ERR_NO_MEM;
    errno = 0;
    wrap->dir = opendir(path);
    if (wrap->dir == NULL) {
        int e = errno;
        free(wrap);
        return errno_to_esp(e);
    }
    snprintf(wrap->path, sizeof(wrap->path), "%s", path);
    *out = wrap;
    return ESP_OK;
}

esp_err_t panel_files_platform_readdir(panel_files_platform_dir_t *dir,
                                       panel_files_platform_dirent_t *out)
{
    if (dir == NULL || dir->dir == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    errno = 0;
    struct dirent *ent = readdir(dir->dir);
    if (ent == NULL) {
        int e = errno;
        return e == 0 ? ESP_ERR_NOT_FOUND : errno_to_esp(e);
    }

    memset(out, 0, sizeof(*out));
    size_t n = 0;
    while (n < 256 && ent->d_name[n] != '\0') n++;
    out->name_truncated = n >= sizeof(out->name);
    snprintf(out->name, sizeof(out->name), "%s", ent->d_name);

    char full[SD_PATH_BYTES];
    struct stat st;
    size_t dl = strlen(dir->path);
    size_t nl = strlen(out->name);
    if (dl + nl + 2 > sizeof(full)) return ESP_ERR_INVALID_SIZE;
    memcpy(full, dir->path, dl);
    full[dl] = '/';
    memcpy(full + dl + 1, out->name, nl);
    full[dl + 1 + nl] = '\0';

    errno = 0;
    if (stat(full, &st) != 0) {
        return errno_to_esp(errno);
    }
    out->is_dir = S_ISDIR(st.st_mode);
    out->size = (uint64_t)st.st_size;
    out->mtime = (uint32_t)st.st_mtime;
    return ESP_OK;
}

void panel_files_platform_closedir(panel_files_platform_dir_t *dir)
{
    if (dir != NULL) {
        if (dir->dir != NULL) closedir(dir->dir);
        free(dir);
    }
}

esp_err_t panel_files_platform_stat(const char *path, bool *is_dir, uint64_t *size,
                                    uint32_t *mtime)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    struct stat st;
    errno = 0;
    if (stat(path, &st) != 0) return errno_to_esp(errno);
    if (is_dir != NULL) *is_dir = S_ISDIR(st.st_mode);
    if (size != NULL) *size = (uint64_t)st.st_size;
    if (mtime != NULL) *mtime = (uint32_t)st.st_mtime;
    return ESP_OK;
}

esp_err_t panel_files_platform_read(const char *path, char *buf, size_t cap,
                                    size_t *out_len, bool *truncated)
{
    if (path == NULL || buf == NULL || out_len == NULL || cap == 0) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return errno_to_esp(errno);
    size_t n = fread(buf, 1, cap, file);
    if (ferror(file)) {
        fclose(file);
        return ESP_FAIL;
    }
    *out_len = n;
    if (truncated != NULL) {
        int c = fgetc(file);
        if (c == EOF && ferror(file)) {
            fclose(file);
            return ESP_FAIL;
        }
        *truncated = c != EOF;
    }
    fclose(file);
    return ESP_OK;
}
