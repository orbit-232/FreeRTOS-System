#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded application history.  The model is deliberately independent of
 * LVGL so navigation semantics and rapid-request policy can be tested on the
 * host without a display. */
#define UI_NAV_HISTORY_MAX 8

typedef enum {
    UI_NAV_MOTION_NONE = 0,
    UI_NAV_MOTION_FORWARD,
    UI_NAV_MOTION_BACK,
    UI_NAV_MOTION_UNLOCK,
    UI_NAV_MOTION_CONTROL_OPEN,
    UI_NAV_MOTION_CONTROL_CLOSE,
    UI_NAV_MOTION_HOME_NEXT,
    UI_NAV_MOTION_HOME_PREV,
    UI_NAV_MOTION_FADE,
} ui_nav_motion_t;

typedef enum {
    UI_NAV_BACK_DISABLED = 0,
    UI_NAV_BACK_MODAL,
    UI_NAV_BACK_CONTROL,
    UI_NAV_BACK_SETTINGS,
    UI_NAV_BACK_PAGE,
} ui_nav_back_action_t;

typedef enum {
    UI_NAV_PENDING_NONE = 0,
    UI_NAV_PENDING_PAGE,
    UI_NAV_PENDING_BACK,
    UI_NAV_PENDING_HOME,
} ui_nav_pending_kind_t;

typedef struct {
    ui_nav_pending_kind_t kind;
    ui_page_t page;
} ui_nav_pending_t;

typedef struct {
    ui_page_t current;
    ui_page_t history[UI_NAV_HISTORY_MAX];
    uint8_t depth;
    ui_page_t control_source;
    bool control_open;
    bool locked;
} ui_nav_model_t;

void ui_nav_model_init(ui_nav_model_t *model, ui_page_t current);
bool ui_nav_model_is_app(ui_page_t page);
bool ui_nav_model_unlock(ui_nav_model_t *model);
bool ui_nav_model_enter(ui_nav_model_t *model, ui_page_t target, ui_nav_motion_t *motion);
bool ui_nav_model_back(ui_nav_model_t *model, ui_page_t *target, ui_nav_motion_t *motion);
bool ui_nav_model_home(ui_nav_model_t *model, ui_nav_motion_t *motion);
bool ui_nav_model_control_open(ui_nav_model_t *model);
bool ui_nav_model_control_close(ui_nav_model_t *model, ui_page_t *source);
ui_nav_back_action_t ui_nav_model_back_action(const ui_nav_model_t *model, bool modal_open,
                                             bool settings_subsection);
uint32_t ui_nav_motion_duration(ui_nav_motion_t motion, bool animations_enabled);
const char *ui_nav_motion_name(ui_nav_motion_t motion);

/* Single-flight pending policy.  At most one request is retained; the latest
 * intent wins and exact duplicates are ignored. */
bool ui_nav_pending_page(ui_nav_pending_t *pending, ui_page_t page);
bool ui_nav_pending_back(ui_nav_pending_t *pending);
bool ui_nav_pending_home(ui_nav_pending_t *pending);
bool ui_nav_pending_take(ui_nav_pending_t *pending, ui_nav_pending_t *out);

#ifdef __cplusplus
}
#endif
