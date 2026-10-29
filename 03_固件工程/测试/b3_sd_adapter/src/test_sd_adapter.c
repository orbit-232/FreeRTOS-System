#include "panel_files_platform.h"
#include "driver/sdmmc_host.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

extern bool fake_host_ready;
extern int fake_slot_count;
extern bool fake_slot0_present;
extern bool fake_slot1_present;
extern esp_err_t fake_mount_result;
extern int fake_mount_calls;
extern int fake_unmount_calls;
extern int fake_ldo_new;
extern int fake_ldo_del;
extern int fake_init_calls;
extern int fake_deinit_slot0;
extern int fake_deinit_slot1;
extern const sdmmc_host_t *fake_last_host;
extern bool fake_format_flag;

static void reset_fake(void)
{
    fake_host_ready = true;
    fake_slot_count = 1;
    fake_slot0_present = false;
    fake_slot1_present = true;
    fake_mount_result = ESP_OK;
    fake_mount_calls = 0;
    fake_unmount_calls = 0;
    fake_ldo_new = 0;
    fake_ldo_del = 0;
    fake_init_calls = 0;
    fake_deinit_slot0 = 0;
    fake_deinit_slot1 = 0;
    fake_last_host = NULL;
    fake_format_flag = true;
}

int main(void)
{
    reset_fake();
    CHECK(panel_files_platform_mount() == ESP_OK);
    CHECK(panel_files_platform_mounted());
    CHECK(fake_mount_calls == 1);
    CHECK(fake_ldo_new == 1);
    CHECK(fake_ldo_del == 0);
    CHECK(fake_init_calls == 1);
    CHECK(fake_slot0_present);
    CHECK(fake_slot1_present);
    CHECK(fake_slot_count == 2);
    CHECK(fake_format_flag == false);
    CHECK(fake_last_host != NULL);
    CHECK(fake_last_host->slot == 0);
    CHECK(fake_last_host->pwr_ctrl_handle != NULL);
    CHECK(fake_last_host->deinit_p != NULL);

    panel_files_platform_unmount();
    CHECK(!panel_files_platform_mounted());
    CHECK(fake_unmount_calls == 1);
    CHECK(fake_ldo_del == 1);
    CHECK(!fake_slot0_present);
    CHECK(fake_slot1_present);
    CHECK(fake_slot_count == 1);
    CHECK(fake_deinit_slot0 == 1);
    CHECK(fake_deinit_slot1 == 0);

    /* Three failed mounts in a row must not leak LDO handles, must clean only
     * slot 0 and must leave the Hosted slot 1 present. */
    fake_mount_result = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        CHECK(panel_files_platform_mount() != ESP_OK);
        CHECK(!panel_files_platform_mounted());
        CHECK(fake_ldo_new == fake_ldo_del);
        CHECK(!fake_slot0_present);
        CHECK(fake_slot1_present);
        CHECK(fake_slot_count == 1);
    }
    CHECK(fake_deinit_slot0 == 4);
    CHECK(fake_deinit_slot1 == 0);

    /* Success after failures. */
    fake_mount_result = ESP_OK;
    CHECK(panel_files_platform_mount() == ESP_OK);
    CHECK(fake_ldo_new == fake_ldo_del + 1);
    CHECK(fake_slot_count == 2);
    panel_files_platform_unmount();
    CHECK(fake_ldo_new == fake_ldo_del);
    CHECK(fake_slot_count == 1);
    CHECK(fake_slot1_present);

    /* Host not ready is a bounded error, not a blind retry loop, and must not
     * create an LDO or touch either slot. */
    fake_host_ready = false;
    int ldo_before = fake_ldo_new;
    int mount_before = fake_mount_calls;
    CHECK(panel_files_platform_mount() == ESP_ERR_INVALID_STATE);
    CHECK(fake_ldo_new == ldo_before);
    CHECK(fake_mount_calls == mount_before);
    CHECK(fake_slot1_present);

    printf("B3_SD_ADAPTER failures=0 ldo_new=%d ldo_del=%d deinit0=%d deinit1=%d\n",
           fake_ldo_new, fake_ldo_del, fake_deinit_slot0, fake_deinit_slot1);
    return 0;
}
