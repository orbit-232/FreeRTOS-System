#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware pages are full-screen children of one persistent root.  The host
 * renderer leaves this unset so page factories still return real LVGL screens. */
void ui_page_parent_set(lv_obj_t *parent);

/* Runtime suppresses per-page bars; static host page renders keep the icon
 * fallback so a standalone screenshot still has a navigation strip. */
void ui_page_nav_suppress(bool suppress);

/* The one fixed three-key Android-style system bar.  Created on lv_layer_sys()
 * by ui_start; pages never create a second runtime bar. */
#define UI_NAV_BAR_MAGIC 0x4E415642u
lv_obj_t *ui_nav_bar(lv_obj_t *parent, ui_page_t page);
lv_obj_t *ui_system_bar_create(void);
void ui_system_bar_rebuild(void);
void ui_system_bar_set_visible(bool visible);
bool ui_system_bar_is_visible(void);

/* Immediate pressed-state feedback for clickable cards, rows and buttons. */
void ui_pressable(lv_obj_t *obj);

typedef enum {
    UI_WEATHER_SUN = 0,
    UI_WEATHER_CLOUD,
    UI_WEATHER_RAIN,
    UI_WEATHER_PARTLY,
} ui_weather_kind_t;

lv_obj_t *ui_screen_create(void);
void ui_screen_gradient(lv_obj_t *screen, lv_color_t top, lv_color_t bottom);

lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
lv_obj_t *ui_label_at(lv_obj_t *parent, int x, int y, const char *text, const lv_font_t *font,
                      lv_color_t color);
void ui_label_set_text(lv_obj_t *label, const char *text);
lv_obj_t *ui_pill(lv_obj_t *parent, const char *text, lv_color_t fg, lv_color_t bg,
                  const lv_font_t *font);

lv_obj_t *ui_bar_create(lv_obj_t *parent, int x, int y, int w, int h);
void ui_bar_set(lv_obj_t *bar, int percent);

lv_obj_t *ui_header(lv_obj_t *parent, const char *title, const char *right_text);
lv_obj_t *ui_status_bar(lv_obj_t *parent, bool dark);
lv_obj_t *ui_grabber(lv_obj_t *parent);

lv_obj_t *ui_slider_create(lv_obj_t *parent, int x, int y, int w, int min, int max, int value);
lv_obj_t *ui_switch_create(lv_obj_t *parent, int x, int y, bool checked);

lv_obj_t *ui_weather_icon(lv_obj_t *parent, ui_weather_kind_t kind, int size);
lv_obj_t *ui_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                    const lv_font_t *font, lv_event_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
