#include "panel_files_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#ifndef WC_ERR_INVALID_CHARS
#define WC_ERR_INVALID_CHARS 0x00000080
#endif

#define TEST_PATH_CHARS 1024

struct panel_files_platform_dir {
    HANDLE find;
    WIN32_FIND_DATAW data;
    bool first;
    char path[TEST_PATH_CHARS];
};

static char s_root[TEST_PATH_CHARS];
static bool s_mounted;

void test_fs_set_root(const char *root_utf8)
{
    if (root_utf8 == NULL) {
        s_root[0] = '\0';
        return;
    }
    snprintf(s_root, sizeof(s_root), "%s", root_utf8);
    size_t n = strlen(s_root);
    while (n > 1 && (s_root[n - 1] == '\\' || s_root[n - 1] == '/')) s_root[--n] = '\0';
}

static bool utf8_to_wide(const char *utf8, wchar_t *wide, size_t wide_count)
{
    if (utf8 == NULL || wide == NULL || wide_count == 0) return false;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, (int)wide_count);
    return n > 0;
}

static bool wide_to_utf8(const wchar_t *wide, char *out, size_t out_count, bool *truncated)
{
    if (wide == NULL || out == NULL || out_count == 0) return false;
    char temp[2048];
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, temp, (int)sizeof(temp),
                                NULL, NULL);
    if (n <= 0) return false;
    size_t bytes = (size_t)n - 1;
    *truncated = false;
    if (bytes >= out_count) {
        bytes = out_count - 1;
        while (bytes > 0 && ((unsigned char)temp[bytes] & 0xC0) == 0x80) bytes--;
        *truncated = true;
    }
    memcpy(out, temp, bytes);
    out[bytes] = '\0';
    return true;
}

static bool format_w(wchar_t *out, size_t cap, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) return false;
    return true;
}

static bool utf8_path_to_wide(const char *utf8, wchar_t *wide, size_t wide_count)
{
    char normalized[TEST_PATH_CHARS * 2];
    char prefixed[TEST_PATH_CHARS * 2 + 8];
    const char *use = utf8;
    if (strlen(utf8) >= 248 && strncmp(utf8, "\\\\?\\", 4) != 0) {
        int p = snprintf(prefixed, sizeof(prefixed), "\\\\?\\%s", utf8);
        if (p < 0 || (size_t)p >= sizeof(prefixed)) return false;
        use = prefixed;
    }
    int n = snprintf(normalized, sizeof(normalized), "%s", use);
    if (n < 0 || (size_t)n >= sizeof(normalized)) return false;
    for (char *p = normalized; *p != '\0'; p++) {
        if (*p == '/') *p = '\\';
    }
    return utf8_to_wide(normalized, wide, wide_count);
}

static bool make_full_wide(const char *path, wchar_t *wide, size_t wide_count)
{
    if (path == NULL || path[0] == '\0') return false;
    if (path[0] == '\\' || (path[1] == ':' && path[0] >= 'A' && path[0] <= 'Z') ||
        (path[1] == ':' && path[0] >= 'a' && path[0] <= 'z')) {
        return utf8_path_to_wide(path, wide, wide_count);
    }
    char full[TEST_PATH_CHARS * 2];
    int n = snprintf(full, sizeof(full), "%s/%s", s_root, path[0] == '/' ? path + 1 : path);
    if (n < 0 || (size_t)n >= sizeof(full)) return false;
    return utf8_path_to_wide(full, wide, wide_count);
}

bool panel_files_platform_mounted(void) { return s_mounted; }
const char *panel_files_platform_root(void) { return s_root; }

esp_err_t panel_files_platform_mount(void)
{
    if (s_root[0] == '\0') return ESP_ERR_NOT_FOUND;
    s_mounted = true;
    return ESP_OK;
}

void panel_files_platform_unmount(void) { s_mounted = false; }

esp_err_t panel_files_platform_opendir(const char *path, panel_files_platform_dir_t **out)
{
    if (!s_mounted || path == NULL || out == NULL) return ESP_ERR_NOT_FOUND;
    wchar_t wide[TEST_PATH_CHARS];
    if (!make_full_wide(path, wide, TEST_PATH_CHARS)) return ESP_ERR_INVALID_ARG;

    wchar_t pattern[TEST_PATH_CHARS + 4];
    if (!format_w(pattern, TEST_PATH_CHARS + 4, L"%ls\\*", wide)) return ESP_ERR_INVALID_ARG;

    panel_files_platform_dir_t *wrap = calloc(1, sizeof(*wrap));
    if (wrap == NULL) return ESP_ERR_NO_MEM;
    wrap->find = FindFirstFileW(pattern, &wrap->data);
    if (wrap->find == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        free(wrap);
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                   ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    snprintf(wrap->path, sizeof(wrap->path), "%s", path);
    wrap->first = true;
    *out = wrap;
    return ESP_OK;
}

static void filetime_to_unix(const FILETIME *ft, uint32_t *out)
{
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    const ULONGLONG epoch = 116444736000000000ULL;
    if (u.QuadPart > epoch) {
        ULONGLONG secs = (u.QuadPart - epoch) / 10000000ULL;
        *out = secs > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)secs;
    } else {
        *out = 0;
    }
}

static esp_err_t copy_current(panel_files_platform_dir_t *dir,
                              panel_files_platform_dirent_t *out)
{
    memset(out, 0, sizeof(*out));
    bool truncated = false;
    if (!wide_to_utf8(dir->data.cFileName, out->name, sizeof(out->name), &truncated)) {
        return ESP_FAIL;
    }
    out->name_truncated = truncated;
    out->is_dir = (dir->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ULARGE_INTEGER size;
    size.LowPart = dir->data.nFileSizeLow;
    size.HighPart = dir->data.nFileSizeHigh;
    out->size = size.QuadPart;
    filetime_to_unix(&dir->data.ftLastWriteTime, &out->mtime);
    return ESP_OK;
}

esp_err_t panel_files_platform_readdir(panel_files_platform_dir_t *dir,
                                       panel_files_platform_dirent_t *out)
{
    if (dir == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    if (dir->first) {
        dir->first = false;
        return copy_current(dir, out);
    }
    if (!FindNextFileW(dir->find, &dir->data)) {
        DWORD err = GetLastError();
        return err == ERROR_NO_MORE_FILES ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    return copy_current(dir, out);
}

void panel_files_platform_closedir(panel_files_platform_dir_t *dir)
{
    if (dir != NULL) {
        if (dir->find != INVALID_HANDLE_VALUE) FindClose(dir->find);
        free(dir);
    }
}

esp_err_t panel_files_platform_stat(const char *path, bool *is_dir, uint64_t *size,
                                    uint32_t *mtime)
{
    if (!s_mounted || path == NULL) return ESP_ERR_NOT_FOUND;
    wchar_t wide[TEST_PATH_CHARS];
    if (!make_full_wide(path, wide, TEST_PATH_CHARS)) return ESP_ERR_INVALID_ARG;

    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &attr)) {
        DWORD err = GetLastError();
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                   ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (is_dir != NULL) *is_dir = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (size != NULL) {
        ULARGE_INTEGER u;
        u.LowPart = attr.nFileSizeLow;
        u.HighPart = attr.nFileSizeHigh;
        *size = u.QuadPart;
    }
    if (mtime != NULL) filetime_to_unix(&attr.ftLastWriteTime, mtime);
    return ESP_OK;
}

esp_err_t panel_files_platform_read(const char *path, char *buf, size_t cap,
                                    size_t *out_len, bool *truncated)
{
    if (!s_mounted || path == NULL || buf == NULL || out_len == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    wchar_t wide[TEST_PATH_CHARS];
    if (!make_full_wide(path, wide, TEST_PATH_CHARS)) return ESP_ERR_INVALID_ARG;

    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return ESP_ERR_NOT_FOUND;

    DWORD got = 0;
    if (!ReadFile(file, buf, (DWORD)cap, &got, NULL)) {
        CloseHandle(file);
        return ESP_FAIL;
    }
    *out_len = (size_t)got;
    if (truncated != NULL) {
        unsigned char extra = 0;
        DWORD one = 0;
        if (!ReadFile(file, &extra, 1, &one, NULL)) {
            CloseHandle(file);
            return ESP_FAIL;
        }
        *truncated = one == 1;
    }
    CloseHandle(file);
    return ESP_OK;
}
