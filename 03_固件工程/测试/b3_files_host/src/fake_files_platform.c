#include "panel_files_platform.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

struct panel_files_platform_dir {
    int next;
    char path[320];
};

int fake_mount_result = ESP_OK;
bool fake_read_block = false;
bool fake_read_fail = false;
int fake_unmount_calls = 0;
HANDLE fake_read_started;
HANDLE fake_read_release;

static bool s_mounted;

static void ensure_events(void)
{
    if (fake_read_started == NULL) {
        fake_read_started = CreateEvent(NULL, TRUE, FALSE, NULL);
        fake_read_release = CreateEvent(NULL, TRUE, FALSE, NULL);
    }
}

void fake_files_reset(void)
{
    ensure_events();
    s_mounted = false;
    fake_mount_result = ESP_OK;
    fake_read_block = false;
    fake_read_fail = false;
    fake_unmount_calls = 0;
    ResetEvent(fake_read_started);
    ResetEvent(fake_read_release);
}

bool panel_files_platform_mounted(void) { return s_mounted; }
const char *panel_files_platform_root(void) { return "X:/fake"; }

esp_err_t panel_files_platform_mount(void)
{
    ensure_events();
    if (fake_mount_result != ESP_OK) return fake_mount_result;
    s_mounted = true;
    return ESP_OK;
}

void panel_files_platform_unmount(void)
{
    fake_unmount_calls++;
    s_mounted = false;
}

esp_err_t panel_files_platform_opendir(const char *path, panel_files_platform_dir_t **out)
{
    if (!s_mounted || path == NULL || out == NULL) return ESP_ERR_NOT_FOUND;
    panel_files_platform_dir_t *d = calloc(1, sizeof(*d));
    if (d == NULL) return ESP_ERR_NO_MEM;
    snprintf(d->path, sizeof(d->path), "%s", path);
    d->next = 0;
    *out = d;
    return ESP_OK;
}

esp_err_t panel_files_platform_readdir(panel_files_platform_dir_t *dir,
                                       panel_files_platform_dirent_t *out)
{
    if (dir == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (dir->next == 0) {
        snprintf(out->name, sizeof(out->name), "%s", "docs");
        out->is_dir = true;
    } else if (dir->next == 1) {
        snprintf(out->name, sizeof(out->name), "%s", "slow.txt");
        out->size = 5;
    } else if (dir->next == 2) {
        snprintf(out->name, sizeof(out->name), "%s", "bad.txt");
        out->size = 5;
    } else {
        return ESP_ERR_NOT_FOUND;
    }
    dir->next++;
    return ESP_OK;
}

void panel_files_platform_closedir(panel_files_platform_dir_t *dir) { free(dir); }

esp_err_t panel_files_platform_stat(const char *path, bool *is_dir, uint64_t *size,
                                    uint32_t *mtime)
{
    (void)path;
    if (is_dir) *is_dir = false;
    if (size) *size = 5;
    if (mtime) *mtime = 0;
    return ESP_OK;
}

esp_err_t panel_files_platform_read(const char *path, char *buf, size_t cap,
                                    size_t *out_len, bool *truncated)
{
    (void)path;
    ensure_events();
    SetEvent(fake_read_started);
    if (fake_read_block) {
        WaitForSingleObject(fake_read_release, 5000);
    }
    if (fake_read_fail) return ESP_FAIL;
    const char *text = "slow\n";
    size_t n = strlen(text);
    if (n > cap) n = cap;
    memcpy(buf, text, n);
    *out_len = n;
    if (truncated) *truncated = false;
    return ESP_OK;
}
