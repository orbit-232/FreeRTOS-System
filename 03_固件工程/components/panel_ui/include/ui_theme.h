#pragma once

#include "lvgl.h"
#include "panel_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Layout constants shared by all pages.  The reference canvas is 800x480;
 * 1280x800 is the 1.6x landscape target with 16 px safety bands. */
#define UI_WIDTH 1280
#define UI_HEIGHT 800
#define UI_SAFE 16
#define UI_MARGIN 40
#define UI_STATUS_HEIGHT 58
#define UI_HEADER_HEIGHT 84
#define UI_NAV_HEIGHT 64
#define UI_NAV_Y (UI_HEIGHT - UI_NAV_HEIGHT)
#define UI_CONTENT_BOTTOM (UI_NAV_Y - 8)
#define UI_CONTENT_TOP (UI_STATUS_HEIGHT + 20)
#define UI_SCALE(v) ((int)(((v) * 8) / 5))

LV_FONT_DECLARE(ui_font_16);
LV_FONT_DECLARE(ui_font_20);
LV_FONT_DECLARE(ui_font_24);
LV_FONT_DECLARE(ui_font_32);
LV_FONT_DECLARE(ui_font_num_48);
LV_FONT_DECLARE(ui_font_num_80);
LV_FONT_DECLARE(ui_font_num_128);

typedef struct {
    lv_color_t bg;
    lv_color_t surface;
    lv_color_t surface_alt;
    lv_color_t text;
    lv_color_t muted;
    lv_color_t accent;
    lv_color_t accent_soft;
    lv_color_t border;
    lv_color_t success;
    lv_color_t warning;
    lv_color_t danger;
    lv_color_t shadow;
} ui_palette_t;

const ui_palette_t *ui_palette(void);
void ui_theme_init(panel_theme_t setting);
void ui_theme_set(panel_theme_t setting);
panel_theme_t ui_theme_effective(void);
uint32_t ui_anim_time(uint32_t base_ms);

#ifdef __cplusplus
}
#endif
