#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool fake_host_ready = true;
int fake_slot_count = 1;
bool fake_slot0_present = false;
bool fake_slot1_present = true;
esp_err_t fake_mount_result = ESP_OK;
int fake_mount_calls = 0;
int fake_unmount_calls = 0;
int fake_ldo_new = 0;
int fake_ldo_del = 0;
int fake_init_calls = 0;
int fake_deinit_slot0 = 0;
int fake_deinit_slot1 = 0;
const sdmmc_host_t *fake_last_host = NULL;
bool fake_format_flag = false;

esp_err_t sdmmc_host_get_state(sdmmc_host_state_t *state)
{
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    state->host_initialized = fake_host_ready;
    state->num_of_init_slots = fake_slot_count;
    return ESP_OK;
}

esp_err_t sdmmc_host_deinit_slot(int slot)
{
    if (slot == 0) {
        fake_deinit_slot0++;
        if (fake_slot0_present) {
            fake_slot0_present = false;
            if (fake_slot_count > 0) fake_slot_count--;
        }
        return ESP_OK;
    }
    if (slot == 1) {
        fake_deinit_slot1++;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t sd_pwr_ctrl_new_on_chip_ldo(const sd_pwr_ctrl_ldo_config_t *configs,
                                      sd_pwr_ctrl_handle_t *ret_drv)
{
    if (configs == NULL || ret_drv == NULL || configs->ldo_chan_id != 4) return ESP_ERR_INVALID_ARG;
    fake_ldo_new++;
    *ret_drv = (void *)(uintptr_t)0x4c44;
    return ESP_OK;
}

esp_err_t sd_pwr_ctrl_del_on_chip_ldo(sd_pwr_ctrl_handle_t ctrl_handle)
{
    if (ctrl_handle == NULL) return ESP_ERR_INVALID_ARG;
    fake_ldo_del++;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_sdmmc_mount(const char *base_path, const sdmmc_host_t *host_config,
                                  const void *slot_config,
                                  const esp_vfs_fat_mount_config_t *mount_config,
                                  sdmmc_card_t **out_card)
{
    (void)slot_config;
    if (base_path == NULL || host_config == NULL || mount_config == NULL || out_card == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    fake_mount_calls++;
    fake_last_host = host_config;
    fake_format_flag = mount_config->format_if_mount_failed;
    if (host_config->init != NULL) {
        fake_init_calls++;
        esp_err_t err = host_config->init();
        if (err != ESP_OK) return err;
    }
    if (fake_mount_result != ESP_OK) {
        if (host_config->deinit_p != NULL) host_config->deinit_p(host_config->slot);
        return fake_mount_result;
    }
    fake_slot0_present = true;
    fake_slot_count = 2;
    *out_card = (sdmmc_card_t *)(uintptr_t)0x1;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_sdcard_unmount(const char *base_path, sdmmc_card_t *card)
{
    (void)base_path;
    if (card == NULL) return ESP_ERR_INVALID_ARG;
    fake_unmount_calls++;
    if (fake_last_host != NULL && fake_last_host->deinit_p != NULL) {
        fake_last_host->deinit_p(fake_last_host->slot);
    }
    fake_slot0_present = false;
    fake_slot_count = 1;
    return ESP_OK;
}
