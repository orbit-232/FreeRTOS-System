#include "panel_files.h"
#include "fake_files.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    fake_files_reset();
    fake_mount_result = ESP_OK;
    CHECK(panel_files_init() == ESP_OK);

    /* Wake the worker immediately, before its startup mount publication.  The
     * request must not be lost and startup must not overwrite it. */
    CHECK(panel_files_root(0) == ESP_OK);
    CHECK(panel_files_retry() == ESP_OK);

    bool ready = false;
    for (int i = 0; i < 1000; i++) {
        panel_files_snapshot_t s;
        panel_files_get_snapshot(&s);
        if (s.state == PANEL_FILES_STATE_READY && s.generation > 0 &&
            strcmp(s.path, "/") == 0) {
            ready = true;
            break;
        }
        Sleep(2);
    }
    CHECK(ready);
    panel_files_snapshot_t s;
    panel_files_get_snapshot(&s);
    CHECK(s.count == 3);
    CHECK(s.error[0] == '\0');
    printf("B3_FILES_INIT_RACE failures=0 gen=%u\n", s.generation);
    return 0;
}
