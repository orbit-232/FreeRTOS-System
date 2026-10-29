#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* One-shot LAN import. The port is closed until start is called, and closes
 * again after the first success or failure. token is RAM-only. */
esp_err_t panel_music_import_start(const char *token);
void panel_music_import_stop(void);
bool panel_music_import_is_active(void);
