#include "panel_files.h"
#include "fake_files.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static bool wait_generation_after(uint32_t previous, unsigned timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        panel_files_snapshot_t s;
        panel_files_get_snapshot(&s);
        if (s.generation != previous) return true;
        if (GetTickCount() - start > timeout_ms) return false;
        Sleep(2);
    }
}

static bool wait_state(panel_files_state_t state, unsigned timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        panel_files_snapshot_t s;
        panel_files_get_snapshot(&s);
        if (s.state == state) return true;
        if (GetTickCount() - start > timeout_ms) {
            printf("TIMEOUT state=%d expected=%d error=%s\n", s.state, state, s.error);
            return false;
        }
        Sleep(2);
    }
}

static int find_entry(const char *name, unsigned *index)
{
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    for (unsigned i = 0; i < s.count; i++) {
        if (strcmp(s.entries[i].name, name) == 0) {
            *index = i;
            return 0;
        }
    }
    return -1;
}

int main(void)
{
    fake_files_reset();
    fake_mount_result = ESP_ERR_NOT_FOUND;
    CHECK(panel_files_init() == ESP_OK);
    CHECK(wait_state(PANEL_FILES_STATE_NO_CARD, 3000));
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    CHECK(strstr(s.error, "TF") != NULL);
    CHECK(!panel_files_card_ready());

    fake_mount_result = ESP_OK;
    CHECK(panel_files_retry() == ESP_OK);
    CHECK(wait_state(PANEL_FILES_STATE_READY, 3000));
    CHECK(panel_files_card_ready());

    panel_files_get_snapshot(&s);
    CHECK(strcmp(s.path, "/") == 0);
    CHECK(s.count == 3);
    unsigned slow = 0;
    CHECK(find_entry("slow.txt", &slow) == 0);

    /* A slow asset read holds the shared I/O lease.  retry must wait for the
     * lease before unmounting (this is the same lock the real font reader uses
     * around its fopen/fread, so Hosted slot 1 cannot be disturbed mid-read). */
    CHECK(panel_files_io_acquire());
    int unmounts_before = fake_unmount_calls;
    CHECK(panel_files_retry() == ESP_OK);
    Sleep(80);
    CHECK(fake_unmount_calls == unmounts_before);
    panel_files_io_release();
    for (int i = 0; i < 1500 && fake_unmount_calls == unmounts_before; i++) Sleep(2);
    CHECK(fake_unmount_calls > unmounts_before);
    CHECK(wait_state(PANEL_FILES_STATE_READY, 3000));
    panel_files_get_snapshot(&s);
    CHECK(s.count == 3);

    /* Stale completion: block the preview in I/O, navigate root, then let the
     * old preview finish.  It must not overwrite the newer root publication. */
    fake_read_block = true;
    ResetEvent(fake_read_started);
    ResetEvent(fake_read_release);
    uint32_t before = s.generation;
    CHECK(panel_files_preview(slow, before) == ESP_OK);
    CHECK(WaitForSingleObject(fake_read_started, 3000) == WAIT_OBJECT_0);
    CHECK(panel_files_root(before) == ESP_OK);
    SetEvent(fake_read_release);
    CHECK(wait_state(PANEL_FILES_STATE_READY, 3000));
    unsigned waited = 0;
    while (waited < 3000) {
        panel_files_get_snapshot(&s);
        if (s.generation > before && strcmp(s.path, "/") == 0) break;
        Sleep(2);
        waited += 2;
    }
    CHECK(strcmp(s.path, "/") == 0);
    CHECK(!s.preview_open);
    CHECK(s.generation > before);

    /* A preview I/O failure must publish a visible generation/error and clear
     * the old preview, not leave the UI on a stale success. */
    fake_read_block = false;
    fake_read_fail = true;
    panel_files_get_snapshot(&s);
    before = s.generation;
    CHECK(panel_files_preview(slow, before) == ESP_OK);
    CHECK(wait_generation_after(before, 3000));
    panel_files_get_snapshot(&s);
    CHECK(!s.preview_open);
    CHECK(s.preview_error);
    CHECK(strstr(s.error, "读取") != NULL);

    fake_read_fail = false;
    before = s.generation;
    CHECK(panel_files_preview(slow, before) == ESP_OK);
    CHECK(wait_generation_after(before, 3000));
    panel_files_get_snapshot(&s);
    CHECK(s.preview_open);
    CHECK(strcmp(s.preview, "slow\n") == 0);

    /* Queue saturation is reported, not silently dropped.  A saturated API
     * leaves the previously accepted snapshot intact. */
    CHECK(panel_files_close_preview() == ESP_OK);
    Sleep(20);
    fake_read_block = true;
    ResetEvent(fake_read_started);
    ResetEvent(fake_read_release);
    panel_files_get_snapshot(&s);
    CHECK(panel_files_preview(slow, s.generation) == ESP_OK);
    CHECK(WaitForSingleObject(fake_read_started, 3000) == WAIT_OBJECT_0);
    int accepted = 0;
    int timedout = 0;
    for (int i = 0; i < 16; i++) {
        esp_err_t err = panel_files_root(0);
        if (err == ESP_OK) accepted++;
        else if (err == ESP_ERR_TIMEOUT) timedout++;
    }
    CHECK(accepted > 0);
    CHECK(timedout > 0);
    SetEvent(fake_read_release);
    Sleep(100);
    CHECK(wait_state(PANEL_FILES_STATE_READY, 5000));

    printf("B3_FILES_RACE failures=0 accepted=%d timedout=%d\n", accepted, timedout);
    return 0;
}
