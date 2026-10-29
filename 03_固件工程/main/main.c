#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "panel_bsp.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_diag.h"

static const char *TAG = "waveshare_panel";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "WavesharePanel %s starting", app->version);
    ESP_LOGI(TAG, "chip=%d cores=%d revision=%d.%d", (int)chip.model, (int)chip.cores,
             chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "idf=%s build=%s %s", esp_get_idf_version(), __DATE__, __TIME__);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Recover only the NVS partition. This is not a flash erase. */
        ESP_LOGW(TAG, "NVS needs reinitialization (%s), erasing NVS namespace only",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(panel_bsp_init());
    ESP_ERROR_CHECK(panel_services_init());

    ui_start();
    ui_diag_start_console();
    ESP_LOGI(TAG, "application started; free heap=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
