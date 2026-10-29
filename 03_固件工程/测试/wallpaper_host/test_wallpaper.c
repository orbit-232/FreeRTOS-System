#include "panel_wallpaper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "panel_files.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

bool panel_files_card_ready(void) { return false; }
const char *panel_files_root_path(void) { return "."; }
bool panel_files_io_acquire(void) { return true; }
void panel_files_io_release(void) { }

static int write_short_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return 1;
    int ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok ? 0 : 2;
}

int main(void)
{
    CHECK(panel_wallpaper_get() == NULL);
    CHECK(panel_wallpaper_load_file("test/wallpaper_host/missing.rgb565") != ESP_OK);
    CHECK(panel_wallpaper_get() == NULL);

    unsigned char bad[64];
    memset(bad, 0x5A, sizeof(bad));
    memcpy(bad, "BAD!", 4);
    CHECK(write_short_file("test/wallpaper_host/corrupt.rgb565", (const char *)bad,
                           sizeof(bad)) == 0);
    CHECK(panel_wallpaper_load_file("test/wallpaper_host/corrupt.rgb565") != ESP_OK);
    CHECK(panel_wallpaper_get() == NULL);

    unsigned char header[24];
    FILE *src = fopen("assets/lockscreen/lock_wallpaper.rgb565", "rb");
    CHECK(src != NULL);
    CHECK(fread(header, 1, sizeof(header), src) == sizeof(header));
    fclose(src);
    CHECK(write_short_file("test/wallpaper_host/truncated.rgb565", (const char *)header,
                           sizeof(header)) == 0);
    CHECK(panel_wallpaper_load_file("test/wallpaper_host/truncated.rgb565") != ESP_OK);
    CHECK(panel_wallpaper_get() == NULL);

    FILE *in = fopen("assets/lockscreen/lock_wallpaper.rgb565", "rb");
    CHECK(in != NULL);
    FILE *out = fopen("test/wallpaper_host/badcrc.rgb565", "wb");
    CHECK(out != NULL);
    unsigned char copy[4096];
    size_t n;
    while ((n = fread(copy, 1, sizeof(copy), in)) > 0) {
        CHECK(fwrite(copy, 1, n, out) == n);
    }
    fclose(in);
    out = fopen("test/wallpaper_host/badcrc.rgb565", "r+b");
    CHECK(out != NULL);
    fseek(out, 24, SEEK_SET);
    int byte = fgetc(out);
    CHECK(byte != EOF);
    fseek(out, 24, SEEK_SET);
    CHECK(fputc(byte ^ 0xFF, out) != EOF);
    fclose(out);
    CHECK(panel_wallpaper_load_file("test/wallpaper_host/badcrc.rgb565") != ESP_OK);
    CHECK(panel_wallpaper_get() == NULL);

    CHECK(panel_wallpaper_init() == ESP_OK);
    Sleep(80);
    CHECK(panel_wallpaper_get() == NULL); /* no card: lock screen keeps gradient */

    panel_wallpaper_request();
    CHECK(panel_wallpaper_load_file("assets/lockscreen/lock_wallpaper.rgb565") == ESP_OK);
    const panel_wallpaper_t *wp = panel_wallpaper_get();
    CHECK(wp != NULL);
    CHECK(wp->width == 1280);
    CHECK(wp->height == 800);
    CHECK(wp->pixels != NULL);
    panel_wallpaper_release();
    CHECK(panel_wallpaper_get() == NULL);

    /* Release while a load is in flight: no publish after ownership is gone. */
    panel_wallpaper_request();
    HANDLE thread = CreateThread(NULL, 0,
                                 (LPTHREAD_START_ROUTINE)panel_wallpaper_load_file,
                                 (LPVOID)"assets/lockscreen/lock_wallpaper.rgb565", 0, NULL);
    CHECK(thread != NULL);
    Sleep(1);
    panel_wallpaper_release();
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    CHECK(panel_wallpaper_get() == NULL);

    panel_wallpaper_request();
    CHECK(panel_wallpaper_load_file("assets/lockscreen/lock_wallpaper.rgb565") == ESP_OK);
    CHECK(panel_wallpaper_get() != NULL);
    panel_wallpaper_release();
    CHECK(panel_wallpaper_get() == NULL);
    printf("WALLPAPER_TESTS_OK bytes=%u\n", 1280U * 800U * 2U);
    return 0;
}
