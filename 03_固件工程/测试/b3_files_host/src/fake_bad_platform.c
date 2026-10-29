#include "panel_files_platform.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

struct panel_files_platform_dir {
    int next;
};

int fake_dir_count = 15;
bool fake_opendir_fail = false;
bool fake_readdir_fail = false;
bool fake_mounted = false;

static void make_name(int index, char *out, size_t cap)
{
    if (index == 0) {
        snprintf(out, cap, "%s", "..");
    } else if (index == 1) {
        snprintf(out, cap, "%s", "/absolute");
    } else if (index == 2) {
        snprintf(out, cap, "%s", "bad\nname");
    } else if (index == 3) {
        out[0] = (char)0xE4;
        out[1] = (char)0x41;
        out[2] = '\0';
    } else if (index == 4) {
        memset(out, 'x', 200);
        out[200] = '\0';
    } else if (index == 5) {
        snprintf(out, cap, "%s", "big.txt");
    } else {
        snprintf(out, cap, "e%02d.txt", index);
    }
}

bool panel_files_platform_mounted(void) { return fake_mounted; }
const char *panel_files_platform_root(void) { return "X:/fake"; }

esp_err_t panel_files_platform_mount(void)
{
    fake_mounted = true;
    return ESP_OK;
}

void panel_files_platform_unmount(void) { fake_mounted = false; }

esp_err_t panel_files_platform_opendir(const char *path, panel_files_platform_dir_t **out)
{
    (void)path;
    if (!fake_mounted || out == NULL || fake_opendir_fail) return ESP_FAIL;
    panel_files_platform_dir_t *d = calloc(1, sizeof(*d));
    if (d == NULL) return ESP_ERR_NO_MEM;
    *out = d;
    return ESP_OK;
}

esp_err_t panel_files_platform_readdir(panel_files_platform_dir_t *dir,
                                       panel_files_platform_dirent_t *out)
{
    if (dir == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    if (fake_readdir_fail && dir->next >= 2) return ESP_FAIL;
    if (dir->next >= fake_dir_count) return ESP_ERR_NOT_FOUND;
    memset(out, 0, sizeof(*out));
    make_name(dir->next, out->name, sizeof(out->name));
    out->is_dir = (dir->next == 6);
    out->size = (dir->next == 5) ? 0x100000005ULL : 4;
    dir->next++;
    return ESP_OK;
}

void panel_files_platform_closedir(panel_files_platform_dir_t *dir) { free(dir); }

esp_err_t panel_files_platform_stat(const char *path, bool *is_dir, uint64_t *size,
                                    uint32_t *mtime)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    if (is_dir) *is_dir = strstr(path, "e06") != NULL;
    if (size) *size = strstr(path, "big.txt") ? 0x100000005ULL : 4;
    if (mtime) *mtime = 0;
    return ESP_OK;
}

esp_err_t panel_files_platform_read(const char *path, char *buf, size_t cap,
                                    size_t *out_len, bool *truncated)
{
    (void)path;
    const char *text = "ok\n";
    size_t n = strlen(text);
    if (n > cap) n = cap;
    memcpy(buf, text, n);
    *out_len = n;
    if (truncated) *truncated = false;
    return ESP_OK;
}
