#include "ui_theme.h"

#include "panel_services.h"

static const ui_palette_t s_light = {
    .bg = {.blue = 0xF7, .green = 0xF8, .red = 0xF9},
    .surface = {.blue = 0xFF, .green = 0xFF, .red = 0xFF},
    .surface_alt = {.blue = 0xED, .green = 0xEF, .red = 0xF0},
    .text = {.blue = 0x2B, .green = 0x2C, .red = 0x2C},
    .muted = {.blue = 0x75, .green = 0x7A, .red = 0x7D},
    .accent = {.blue = 0xDE, .green = 0x83, .red = 0x27},
    .accent_soft = {.blue = 0xFC, .green = 0xF2, .red = 0xE5},
    .border = {.blue = 0xE3, .green = 0xE5, .red = 0xE6},
    .success = {.blue = 0x68, .green = 0x9E, .red = 0x2E},
    .warning = {.blue = 0x3C, .green = 0x9A, .red = 0xD1},
    .danger = {.blue = 0x4C, .green = 0x4C, .red = 0xD9},
    .shadow = {.blue = 0x00, .green = 0x00, .red = 0x00},
};

static const ui_palette_t s_dark = {
    .bg = {.blue = 0x1B, .green = 0x16, .red = 0x12},
    .surface = {.blue = 0x29, .green = 0x21, .red = 0x1B},
    .surface_alt = {.blue = 0x33, .green = 0x2A, .red = 0x23},
    .text = {.blue = 0xF8, .green = 0xF6, .red = 0xF4},
    .muted = {.blue = 0xBA, .green = 0xB0, .red = 0xA7},
    .accent = {.blue = 0xEE, .green = 0xA8, .red = 0x5E},
    .accent_soft = {.blue = 0x55, .green = 0x3A, .red = 0x20},
    .border = {.blue = 0x45, .green = 0x39, .red = 0x30},
    .success = {.blue = 0x93, .green = 0xCB, .red = 0x5D},
    .warning = {.blue = 0x5E, .green = 0xB6, .red = 0xE6},
    .danger = {.blue = 0x7A, .green = 0x7A, .red = 0xF0},
    .shadow = {.blue = 0x00, .green = 0x00, .red = 0x00},
};

static panel_theme_t s_effective = PANEL_THEME_LIGHT;

const ui_palette_t *ui_palette(void)
{
    return s_effective == PANEL_THEME_DARK ? &s_dark : &s_light;
}

void ui_theme_set(panel_theme_t setting)
{
    if (setting == PANEL_THEME_AUTO) {
        /* The board has no ambient-light sensor, so auto resolves to light. */
        s_effective = PANEL_THEME_LIGHT;
    } else {
        s_effective = setting;
    }
}

void ui_theme_init(panel_theme_t setting)
{
    ui_theme_set(setting);
}

panel_theme_t ui_theme_effective(void)
{
    return s_effective;
}

uint32_t ui_anim_time(uint32_t base_ms)
{
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    return settings.animations ? base_ms : 0;
}
