#include "panel_settings.h"

#include <stddef.h>

void panel_settings_set_defaults(panel_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }
    settings->brightness = 70;
    settings->auto_brightness = false;
    settings->theme = PANEL_THEME_LIGHT;
    settings->animations = true;
    settings->sleep_after_s = PANEL_SETTINGS_SLEEP_DEFAULT_S;
    settings->do_not_disturb = false;
    settings->rotation_locked = false;
    settings->notification_preview = true;
}

bool panel_settings_sleep_allowed(uint16_t seconds)
{
    switch (seconds) {
    case 0:
    case 30:
    case 60:
    case 120:
    case 300:
        return true;
    default:
        return false;
    }
}

bool panel_settings_validate(const panel_settings_t *settings)
{
    if (settings == NULL) {
        return false;
    }
    if (settings->brightness < PANEL_BRIGHTNESS_MIN ||
        settings->brightness > PANEL_BRIGHTNESS_MAX) {
        return false;
    }
    if (settings->theme != PANEL_THEME_LIGHT &&
        settings->theme != PANEL_THEME_DARK &&
        settings->theme != PANEL_THEME_AUTO) {
        return false;
    }
    if (!panel_settings_sleep_allowed(settings->sleep_after_s)) {
        return false;
    }
    /* Auto brightness cannot be enabled on this board: there is no ambient
     * light sensor in the BSP, and pretending otherwise would be misleading. */
    if (settings->auto_brightness) {
        return false;
    }
    return true;
}

bool panel_settings_migrate(panel_settings_t *settings, uint16_t from_version, uint32_t *flags,
                            panel_settings_migration_t *result)
{
    if (settings == NULL || flags == NULL) {
        return false;
    }
    if (result != NULL) {
        result->from_version = from_version;
        result->to_version = from_version;
        result->flags = *flags;
        result->animations_forced_on = false;
        result->changed = false;
    }

    if (from_version == PANEL_SETTINGS_SCHEMA_V2) {
        if (result != NULL) {
            result->flags = *flags;
        }
        return true;
    }
    if (from_version != PANEL_SETTINGS_SCHEMA_V1) {
        return false;
    }

    bool forced = false;
    if (!settings->animations) {
        settings->animations = true;
        forced = true;
    }
    *flags |= PANEL_SETTINGS_FLAG_ANIMATIONS_MIGRATED;
    if (result != NULL) {
        result->from_version = PANEL_SETTINGS_SCHEMA_V1;
        result->to_version = PANEL_SETTINGS_SCHEMA_V2;
        result->flags = *flags;
        result->animations_forced_on = forced;
        result->changed = true;
    }
    return true;
}
