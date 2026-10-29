#include "fake_idf.h"

static unsigned char g_blob[512];
static size_t g_blob_len;
static bool g_has;

void fake_nvs_reset(void)
{
    memset(g_blob, 0, sizeof(g_blob));
    g_blob_len = 0;
    g_has = false;
}
bool fake_nvs_has_creds(void) { return g_has; }

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out)
{
    (void)ns; (void)mode;
    if (out) *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h; (void)key;
    if (!g_has) return ESP_ERR_NVS_NOT_FOUND;
    if (!len || !out) return ESP_ERR_INVALID_ARG;
    if (*len < g_blob_len) {
        *len = g_blob_len;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, g_blob, g_blob_len);
    *len = g_blob_len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t len)
{
    (void)h; (void)key;
    if (!data || len > sizeof(g_blob)) return ESP_ERR_INVALID_ARG;
    memcpy(g_blob, data, len);
    g_blob_len = len;
    g_has = true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h; (void)key;
    g_has = false;
    g_blob_len = 0;
    return ESP_OK;
}
