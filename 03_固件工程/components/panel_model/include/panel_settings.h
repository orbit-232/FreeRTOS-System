#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANEL_THEME_LIGHT = 0,
    PANEL_THEME_DARK,
    PANEL_THEME_AUTO,
} panel_theme_t;

#define PANEL_BRIGHTNESS_MIN 12
#define PANEL_BRIGHTNESS_MAX 100
#define PANEL_SETTINGS_SLEEP_DEFAULT_S 60

#define PANEL_SETTINGS_SCHEMA_V1 1u
#define PANEL_SETTINGS_SCHEMA_V2 2u
#define PANEL_SETTINGS_FLAG_ANIMATIONS_MIGRATED 0x00000001u

typedef struct {
    uint8_t brightness;       /* 12..100, real backlight percent */
    bool auto_brightness;     /* unsupported: no ambient-light sensor */
    panel_theme_t theme;
    bool animations;
    uint16_t sleep_after_s;   /* 0 disables display sleep */
    bool do_not_disturb;
    bool rotation_locked;     /* local UI state only */
    bool notification_preview;
} panel_settings_t;

typedef struct {
    uint16_t from_version;
    uint16_t to_version;
    uint32_t flags;
    bool animations_forced_on;
    bool changed;
} panel_settings_migration_t;

void panel_settings_set_defaults(panel_settings_t *settings);
bool panel_settings_validate(const panel_settings_t *settings);
bool panel_settings_sleep_allowed(uint16_t seconds);

/* Explicit one-time schema migration.  A v1 blob with animations disabled is
 * forced on exactly once; a v2 blob that says false is preserved.  The caller
 * persists the returned version/flags so the next boot does not force it. */
bool panel_settings_migrate(panel_settings_t *settings, uint16_t from_version, uint32_t *flags,
                            panel_settings_migration_t *result);

#ifdef __cplusplus
}
#endif
