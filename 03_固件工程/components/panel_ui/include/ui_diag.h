#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Full-screen five-point / crosshair touch diagnostic.  Off by default.
 *
 * Entry and exit do not depend on the touch panel working:
 *   - serial console: "diag on", "diag off", "diag" (toggle), "help"
 *   - exit button on screen (works once touch is at least partially usable)
 *   - automatic exit after 60 s without a touch
 * The overlay sits on lv_layer_top(), so it cannot be entered accidentally and
 * cannot trap the user if touches land in the wrong place.
 */
void ui_diag_set_active(bool active);
void ui_diag_toggle(void);
bool ui_diag_is_active(void);

/* Throttled touch logging independent of the overlay ("touchlog on|off"). */
void ui_diag_set_touch_log(bool enabled);
bool ui_diag_touch_log_enabled(void);
void ui_diag_note_touch(int x, int y, const char *phase);

/* Starts a small UART console reader task for the commands above. */
void ui_diag_start_console(void);

#ifdef __cplusplus
}
#endif
