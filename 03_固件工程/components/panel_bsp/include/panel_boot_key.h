#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Pure debounce/single-trigger state for the GPIO35 BOOT key.  The BSP supplies
 * sampled active-low levels; this type has no GPIO or LVGL dependency so the
 * startup/repeat behavior is host-testable. */
typedef struct {
    bool initialized;
    bool raw_low;
    bool stable_low;
    bool consumed;
    bool armed;
    uint32_t last_change_ms;
    uint32_t arm_at_ms;
} panel_boot_key_t;

void panel_boot_key_init(panel_boot_key_t *state, bool raw_low, uint32_t now_ms,
                         uint32_t arm_delay_ms);

/* Returns true once for a debounced press after arming.  A key already low at
 * boot is consumed and cannot generate an event until it is released. */
bool panel_boot_key_update(panel_boot_key_t *state, bool raw_low, uint32_t now_ms);
