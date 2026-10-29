#include "panel_boot_key.h"

#include <stddef.h>

#define BOOT_KEY_DEBOUNCE_MS 30U

void panel_boot_key_init(panel_boot_key_t *state, bool raw_low, uint32_t now_ms,
                         uint32_t arm_delay_ms)
{
    if (state == NULL) return;
    state->initialized = true;
    state->raw_low = raw_low;
    state->stable_low = raw_low;
    state->consumed = raw_low;
    state->armed = false;
    state->last_change_ms = now_ms;
    state->arm_at_ms = now_ms + arm_delay_ms;
}

bool panel_boot_key_update(panel_boot_key_t *state, bool raw_low, uint32_t now_ms)
{
    if (state == NULL || !state->initialized) return false;

    if (raw_low != state->raw_low) {
        state->raw_low = raw_low;
        state->last_change_ms = now_ms;
    }

    if ((uint32_t)(now_ms - state->last_change_ms) < BOOT_KEY_DEBOUNCE_MS) {
        return false;
    }

    bool event = false;
    if (state->stable_low != raw_low) {
        state->stable_low = raw_low;
        if (state->stable_low) {
            if (state->armed && !state->consumed) {
                state->consumed = true;
                event = true;
            } else if (!state->armed) {
                state->consumed = true;
            }
        } else {
            state->consumed = false;
        }
    }

    if (!state->armed && !state->stable_low &&
        (uint32_t)(now_ms - state->arm_at_ms) < 0x80000000U) {
        state->armed = true;
    }
    return event;
}
