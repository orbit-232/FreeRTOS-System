#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Raw RGB565, 1280x800, loaded once from the TF card.  The pixels stay in
 * PSRAM and are never copied into internal RAM. */
typedef struct {
    uint16_t width;
    uint16_t height;
    const uint8_t *pixels;
} panel_wallpaper_t;

esp_err_t panel_wallpaper_init(void);
const panel_wallpaper_t *panel_wallpaper_get(void);

/* Lock-screen ownership: request before showing the lock page, release after
 * the page and its image have been deleted.  The 2 MB PSRAM is freed on
 * release so it does not stay resident while other apps run. */
void panel_wallpaper_request(void);
void panel_wallpaper_release(void);

/* Load one explicit file.  Exposed for host validation; the normal path is the
 * background worker using /panel/assets/lock/lock_wallpaper.rgb565. */
esp_err_t panel_wallpaper_load_file(const char *path);
