#include "panel_files.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

extern int fake_dir_count;
extern bool fake_opendir_fail;
extern bool fake_readdir_fail;

static bool wait_gen(uint32_t previous, unsigned ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        panel_files_snapshot_t s;
        panel_files_get_snapshot(&s);
        if (s.generation != previous) return true;
        if (GetTickCount() - start > ms) return false;
        Sleep(2);
    }
}

static bool wait_state(panel_files_state_t state, unsigned ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        panel_files_snapshot_t s;
        panel_files_get_snapshot(&s);
        if (s.state == state) return true;
        if (GetTickCount() - start > ms) return false;
        Sleep(2);
    }
}

int main(void)
{
    CHECK(panel_files_init() == ESP_OK);
    CHECK(wait_state(PANEL_FILES_STATE_READY, 3000));
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    CHECK(s.page_count == 2);
    CHECK(s.count == PANEL_FILES_MAX_ENTRIES);

    CHECK(!s.entries[0].valid_name);
    CHECK(strcmp(s.entries[0].name, "/absolute") == 0);
    CHECK(!s.entries[1].valid_name);
    CHECK(strcmp(s.entries[1].name, "bad\nname") == 0);
    CHECK(!s.entries[2].valid_utf8);
    CHECK(s.entries[3].truncated_name);
    CHECK(!s.entries[3].valid_name);
    CHECK(strlen(s.entries[3].name) == 127);
    CHECK(s.entries[4].valid_name);
    CHECK(s.entries[4].size == 0x100000005ULL);

    CHECK(panel_files_enter(0, s.generation) == ESP_ERR_INVALID_ARG);
    CHECK(panel_files_enter(1, s.generation) == ESP_ERR_INVALID_ARG);
    CHECK(panel_files_enter(2, s.generation) == ESP_ERR_INVALID_ARG);
    CHECK(panel_files_enter(3, s.generation) == ESP_ERR_INVALID_ARG);
    CHECK(panel_files_preview(4, s.generation) == ESP_OK);
    CHECK(wait_gen(s.generation, 3000));
    panel_files_get_snapshot(&s);
    CHECK(s.preview_open);
    CHECK(s.preview_size == 0x100000005ULL);
    CHECK(strcmp(s.preview, "ok\n") == 0);
    unsigned long long huge = (unsigned long long)s.preview_size;

    /* Page 1 then shrink to one page: the worker must clamp and re-scan. */
    uint32_t gen = s.generation;
    CHECK(panel_files_page(1, gen) == ESP_OK);
    CHECK(wait_gen(gen, 3000));
    panel_files_get_snapshot(&s);
    CHECK(s.page == 1);
    CHECK(s.count == 2);
    fake_dir_count = 3;
    gen = s.generation;
    CHECK(panel_files_refresh(gen) == ESP_OK);
    CHECK(wait_gen(gen, 3000));
    panel_files_get_snapshot(&s);
    CHECK(s.page == 0);
    CHECK(s.page_count == 1);
    CHECK(s.count == 2);

    /* readdir failure must be an ERROR, not a short successful page. */
    fake_readdir_fail = true;
    gen = s.generation;
    CHECK(panel_files_refresh(gen) == ESP_OK);
    CHECK(wait_gen(gen, 3000));
    panel_files_get_snapshot(&s);
    CHECK(s.state == PANEL_FILES_STATE_ERROR);
    CHECK(strstr(s.error, "读取") != NULL);
    fake_readdir_fail = false;

    /* opendir failure and retry recovery. */
    fake_opendir_fail = true;
    gen = s.generation;
    CHECK(panel_files_retry() == ESP_OK);
    CHECK(wait_gen(gen, 3000));
    panel_files_get_snapshot(&s);
    CHECK(s.state == PANEL_FILES_STATE_ERROR);
    fake_opendir_fail = false;
    CHECK(panel_files_retry() == ESP_OK);
    CHECK(wait_state(PANEL_FILES_STATE_READY, 3000));

    printf("B3_FILES_ERRORS failures=0 huge=0x%llx\n", huge);
    return 0;
}
