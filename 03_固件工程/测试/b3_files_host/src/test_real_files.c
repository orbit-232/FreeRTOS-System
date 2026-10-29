#include "panel_files.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#ifndef WC_ERR_INVALID_CHARS
#define WC_ERR_INVALID_CHARS 0x00000080
#endif

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)
#define CHECK_STR_CONTAINS(hay, needle) do { if (strstr((hay), (needle)) == NULL) { \
    printf("FAIL %s:%d expected '%s' in '%s'\n", __FILE__, __LINE__, (needle), (hay)); return 1; } } while (0)

void test_fs_set_root(const char *root_utf8);

static const char *CN_TXT = "\xE4\xB8\xAD\xE6\x96\x87.txt";
static const char *CN_TEXT = "\xE4\xB8\xAD\xE6\x96\x87\xE9\xA2\x84\xE8\xA7\x88\n";

static wchar_t s_root_w[1024];
static char s_root_u8[1024];

static bool u8_to_w(const char *s, wchar_t *w, size_t cap)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, (int)cap) > 0;
}

static bool w_to_u8(const wchar_t *w, char *s, size_t cap)
{
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, s, (int)cap, NULL, NULL) > 0;
}

static bool format_w(wchar_t *out, size_t cap, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(out, cap, fmt, ap);
    va_end(ap);
    return n >= 0 && (size_t)n < cap;
}

static bool prepare_path_w(wchar_t *full, size_t cap)
{
    size_t len = wcslen(full);
    if (len < 248 || wcsncmp(full, L"\\\\?\\", 4) == 0) return len < cap;
    wchar_t prefixed[2048];
    if (!format_w(prefixed, 2048, L"\\\\?\\%ls", full)) return false;
    if (wcslen(prefixed) >= cap) return false;
    wcscpy(full, prefixed);
    return true;
}

static bool make_dir(const char *relative)
{
    wchar_t full[2048];
    wchar_t rel[1024];
    if (!u8_to_w(relative, rel, 1024)) return false;
    if (!format_w(full, 2048, L"%ls\\%ls", s_root_w, rel)) return false;
    if (!prepare_path_w(full, 2048)) return false;
    return CreateDirectoryW(full, NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool write_file(const char *relative, const unsigned char *data, size_t len)
{
    wchar_t full[2048];
    wchar_t rel[1024];
    if (!u8_to_w(relative, rel, 1024)) return false;
    if (!format_w(full, 2048, L"%ls\\%ls", s_root_w, rel)) return false;
    if (!prepare_path_w(full, 2048)) return false;

    HANDLE file = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, data, (DWORD)len, &written, NULL) && written == len;
    CloseHandle(file);
    return ok;
}

static bool write_text(const char *relative, const char *text)
{
    return write_file(relative, (const unsigned char *)text, strlen(text));
}

static void make_root(void)
{
    wchar_t cwd[1024];
    DWORD n = GetCurrentDirectoryW(1024, cwd);
    if (n == 0 || n >= 1024) {
        printf("FAIL cannot read current directory\n");
        ExitProcess(1);
    }
    wchar_t parent[1024];
    if (!format_w(parent, 1024, L"%ls\\test\\b3_files_host\\tmp", cwd)) {
        printf("FAIL cannot format parent\n");
        ExitProcess(1);
    }
    CreateDirectoryW(parent, NULL);
    if (!format_w(s_root_w, 1024, L"%ls\\b3_files_%lu", parent,
                  (unsigned long)GetCurrentProcessId())) {
        printf("FAIL cannot format temp root\n");
        ExitProcess(1);
    }
    if (!CreateDirectoryW(s_root_w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        char debug_root[1024];
        if (w_to_u8(s_root_w, debug_root, sizeof(debug_root))) {
            printf("FAIL cannot create temp root err=%lu path=%s\n",
                   (unsigned long)GetLastError(), debug_root);
        } else {
            printf("FAIL cannot create temp root err=%lu (path conversion failed)\n",
                   (unsigned long)GetLastError());
        }
        ExitProcess(1);
    }
    if (!w_to_u8(s_root_w, s_root_u8, sizeof(s_root_u8))) {
        printf("FAIL cannot convert temp root\n");
        ExitProcess(1);
    }
    test_fs_set_root(s_root_u8);
}

static bool wait_generation_after(uint32_t previous, unsigned timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        panel_files_snapshot_t s;
        panel_files_get_snapshot(&s);
        if (s.generation != previous) return true;
        if (GetTickCount() - start > timeout_ms) {
            printf("TIMEOUT previous_gen=%u state=%d error=%s\n", previous, s.state, s.error);
            return false;
        }
        Sleep(2);
    }
}

static bool wait_ready(unsigned timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        panel_files_snapshot_t s;
        panel_files_get_snapshot(&s);
        if (s.state == PANEL_FILES_STATE_READY) return true;
        if (GetTickCount() - start > timeout_ms) {
            printf("TIMEOUT waiting READY state=%d error=%s\n", s.state, s.error);
            return false;
        }
        Sleep(2);
    }
}

static int find_entry(const char *name, unsigned *index)
{
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    for (unsigned p = 0; p < s.page_count; p++) {
        if (p != s.page) {
            uint32_t gen = s.generation;
            if (panel_files_page((int)p - (int)s.page, gen) != ESP_OK) return -1;
            if (!wait_generation_after(gen, 3000)) return -1;
            panel_files_get_snapshot(&s);
        }
        for (unsigned i = 0; i < s.count; i++) {
            if (strcmp(s.entries[i].name, name) == 0) {
                *index = i;
                return 0;
            }
        }
    }
    return -1;
}

static int find_prefix(const char *prefix, unsigned *index, panel_files_entry_t *entry)
{
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    for (unsigned i = 0; i < s.count; i++) {
        if (strncmp(s.entries[i].name, prefix, strlen(prefix)) == 0) {
            *index = i;
            if (entry != NULL) *entry = s.entries[i];
            return 0;
        }
    }
    return -1;
}

static int enter_path(const char *name)
{
    unsigned index = 0;
    CHECK(find_entry(name, &index) == 0);
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    uint32_t gen = s.generation;
    CHECK(panel_files_enter(index, gen) == ESP_OK);
    CHECK(wait_generation_after(gen, 3000));
    return 0;
}

static int preview_name(const char *name, panel_files_snapshot_t *out)
{
    unsigned index = 0;
    CHECK(find_entry(name, &index) == 0);
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    uint32_t gen = s.generation;
    CHECK(panel_files_preview(index, gen) == ESP_OK);
    CHECK(wait_generation_after(gen, 3000));
    panel_files_get_snapshot(out);
    return 0;
}

static int test_root_and_preview(void)
{
    CHECK(panel_files_init() == ESP_OK);
    CHECK(wait_ready(5000));

    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    CHECK(s.state == PANEL_FILES_STATE_READY);
    CHECK(strcmp(s.path, "/") == 0);
    CHECK(s.page == 0);
    CHECK(s.page_count == 1);
    CHECK(s.count >= 6);
    CHECK(s.error[0] == '\0');

    CHECK(preview_name("readme.txt", &s) == 0);
    CHECK(s.preview_open);
    CHECK(s.preview_error == false);
    CHECK_STR_CONTAINS(s.preview, "hello");
    CHECK(strcmp(s.preview, "hello\nworld\n") == 0);
    CHECK(s.preview_len == strlen("hello\nworld\n"));
    CHECK(strcmp(s.preview_type, "UTF-8 文本") == 0);
    CHECK(s.preview_mtime_valid);
    CHECK(s.preview_mtime_text[0] != '\0');

    CHECK(preview_name(CN_TXT, &s) == 0);
    CHECK(s.preview_open);
    CHECK(strcmp(s.preview, CN_TEXT) == 0);
    CHECK(s.preview_len == strlen(CN_TEXT));

    CHECK(panel_files_close_preview() == ESP_OK);
    uint32_t gen = s.generation;
    CHECK(wait_generation_after(gen, 3000));
    panel_files_get_snapshot(&s);
    CHECK(!s.preview_open);
    return 0;
}

static int test_subdir_target(void)
{
    CHECK(enter_path("Docs") == 0);
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    CHECK(strcmp(s.path, "/Docs") == 0);

    CHECK(preview_name("readme.txt", &s) == 0);
    CHECK(strcmp(s.preview, "sub-readme\n") == 0);

    uint32_t gen = s.generation;
    CHECK(panel_files_up(gen) == ESP_OK);
    CHECK(wait_generation_after(gen, 3000));
    panel_files_get_snapshot(&s);
    CHECK(strcmp(s.path, "/") == 0);

    CHECK(preview_name("readme.txt", &s) == 0);
    CHECK(strcmp(s.preview, "hello\nworld\n") == 0);
    return 0;
}

static int test_pagination(void)
{
    CHECK(enter_path("Many") == 0);
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    CHECK(s.page_count == 3);
    CHECK(s.page == 0);
    CHECK(s.count == 12);
    uint32_t gen = s.generation;
    CHECK(panel_files_page(1, gen) == ESP_OK);
    CHECK(wait_generation_after(gen, 3000));
    panel_files_get_snapshot(&s);
    CHECK(s.page == 1);
    CHECK(s.count == 12);
    gen = s.generation;
    CHECK(panel_files_page(1, gen) == ESP_OK);
    CHECK(wait_generation_after(gen, 3000));
    panel_files_get_snapshot(&s);
    CHECK(s.page == 2);
    CHECK(s.count == 1);
    CHECK(strcmp(s.entries[0].name, "p_24.txt") == 0);

    gen = s.generation;
    CHECK(panel_files_up(gen) == ESP_OK);
    CHECK(wait_generation_after(gen, 3000));
    return 0;
}

static int test_empty(void)
{
    CHECK(enter_path("Empty") == 0);
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    CHECK(s.count == 0);
    CHECK(s.page_count == 1);
    CHECK(s.total_entries == 0);
    uint32_t gen = s.generation;
    CHECK(panel_files_up(gen) == ESP_OK);
    CHECK(wait_generation_after(gen, 3000));
    return 0;
}

static int test_long_name(void)
{
    char prefix[64];
    memset(prefix, 0, sizeof(prefix));
    for (int i = 0; i < 10; i++) {
        memcpy(prefix + i * 3, "\xE9\x95\xBF", 3);
    }
    unsigned index = 0;
    panel_files_entry_t entry;
    CHECK(find_prefix(prefix, &index, &entry) == 0);
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    CHECK(entry.truncated_name);
    CHECK(!entry.valid_name);
    CHECK(entry.valid_utf8);
    CHECK(strlen(entry.name) == 126);
    CHECK((unsigned char)entry.name[123] == 0xE9);
    CHECK((unsigned char)entry.name[124] == 0x95);
    CHECK((unsigned char)entry.name[125] == 0xBF);
    CHECK(panel_files_enter(index, s.generation) == ESP_ERR_INVALID_ARG);
    return 0;
}

static int preview_in_dir(const char *dir, const char *file, panel_files_snapshot_t *out)
{
    CHECK(enter_path(dir) == 0);
    return preview_name(file, out);
}

static int test_edge_previews(void)
{
    panel_files_snapshot_t s;
    CHECK(preview_in_dir("Edge", "overlong.txt", &s) == 0);
    CHECK(!s.preview_open && s.preview_error);
    CHECK_STR_CONTAINS(s.error, "UTF-8");

    CHECK(preview_name("surrogate.txt", &s) == 0);
    CHECK(!s.preview_open && s.preview_error);
    CHECK_STR_CONTAINS(s.error, "UTF-8");

    CHECK(preview_name("toobig.txt", &s) == 0);
    CHECK(!s.preview_open && s.preview_error);

    CHECK(preview_name("split.txt", &s) == 0);
    CHECK(!s.preview_open && s.preview_error);
    CHECK_STR_CONTAINS(s.error, "UTF-8");

    CHECK(preview_name("nul.txt", &s) == 0);
    CHECK(!s.preview_open && s.preview_error);
    CHECK_STR_CONTAINS(s.error, "二进制");
    CHECK(strcmp(s.preview_type, "二进制/非文本") == 0);
    CHECK(s.preview_mtime_valid);

    CHECK(preview_name("exact6k.txt", &s) == 0);
    CHECK(s.preview_open);
    CHECK(!s.preview_truncated);
    CHECK(s.preview_len == PANEL_FILES_PREVIEW_BYTES);
    CHECK((unsigned char)s.preview[s.preview_len - 3] == 0xE4);
    CHECK((unsigned char)s.preview[s.preview_len - 2] == 0xB8);
    CHECK((unsigned char)s.preview[s.preview_len - 1] == 0xAD);

    CHECK(preview_name("trunc6k.txt", &s) == 0);
    CHECK(s.preview_open);
    CHECK(s.preview_truncated);
    CHECK(s.preview_len == PANEL_FILES_PREVIEW_BYTES - 1);
    CHECK((unsigned char)s.preview[s.preview_len - 1] == 'a');

    CHECK(preview_name("unsupported.bin", &s) == 0);
    CHECK(!s.preview_open && s.preview_error);
    CHECK_STR_CONTAINS(s.error, "不支持");
    CHECK(strcmp(s.preview_type, "其他文件") == 0);
    CHECK(s.preview_size == 8);
    CHECK(s.preview_mtime_valid);
    CHECK(s.preview_mtime_text[0] != '\0');
    return 0;
}

int main(void)
{
    make_root();
    CHECK(make_dir("Docs"));
    CHECK(make_dir("Empty"));
    CHECK(make_dir("Many"));
    CHECK(make_dir("Edge"));

    CHECK(write_text("readme.txt", "\xEF\xBB\xBFhello\r\nworld\n"));
    CHECK(write_text(CN_TXT, CN_TEXT));
    CHECK(write_text("Docs/readme.txt", "sub-readme\n"));
    CHECK(write_text("Docs/sub.txt", "subfolder\n"));
    CHECK(write_text("Edge/split.txt", "\xE4\x41"));

    CHECK(write_file("Edge/overlong.txt", (const unsigned char *)"\xC0\xAF", 2));
    CHECK(write_file("Edge/surrogate.txt", (const unsigned char *)"\xED\xA0\x80", 3));
    CHECK(write_file("Edge/toobig.txt", (const unsigned char *)"\xF4\x90\x80\x80", 4));
    CHECK(write_file("Edge/nul.txt", (const unsigned char *)"a\0b", 3));
    CHECK(write_text("Edge/unsupported.bin", "ignored\n"));

    unsigned char exact[PANEL_FILES_PREVIEW_BYTES];
    memset(exact, 'a', sizeof(exact));
    exact[PANEL_FILES_PREVIEW_BYTES - 3] = 0xE4;
    exact[PANEL_FILES_PREVIEW_BYTES - 2] = 0xB8;
    exact[PANEL_FILES_PREVIEW_BYTES - 1] = 0xAD;
    CHECK(write_file("Edge/exact6k.txt", exact, sizeof(exact)));

    unsigned char trunc[PANEL_FILES_PREVIEW_BYTES + 2];
    memset(trunc, 'a', sizeof(trunc));
    trunc[PANEL_FILES_PREVIEW_BYTES - 1] = 0xE4;
    trunc[PANEL_FILES_PREVIEW_BYTES] = 0xB8;
    trunc[PANEL_FILES_PREVIEW_BYTES + 1] = 0xAD;
    CHECK(write_file("Edge/trunc6k.txt", trunc, sizeof(trunc)));

    for (int i = 0; i < 25; i++) {
        char name[64];
        snprintf(name, sizeof(name), "Many/p_%02d.txt", i);
        CHECK(write_text(name, "page\n"));
    }

    char long_name[512];
    memset(long_name, 0, sizeof(long_name));
    for (int i = 0; i < 80; i++) memcpy(long_name + i * 3, "\xE9\x95\xBF", 3);
    strcat(long_name, ".txt");
    CHECK(write_text(long_name, "long\n"));

    CHECK(test_root_and_preview() == 0);
    CHECK(test_subdir_target() == 0);
    CHECK(test_pagination() == 0);
    CHECK(test_empty() == 0);
    CHECK(test_long_name() == 0);
    CHECK(test_edge_previews() == 0);

    printf("B3_FILES_REAL_FS failures=0 root=%s\n", s_root_u8);
    return 0;
}
