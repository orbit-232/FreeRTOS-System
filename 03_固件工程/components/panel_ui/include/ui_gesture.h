#pragma once

#include <stdbool.h>

#include "ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-finger swipe direction as seen on the rotated 1280x800 canvas. */
typedef enum {
    UI_SWIPE_NONE = 0,
    UI_SWIPE_LEFT,
    UI_SWIPE_RIGHT,
    UI_SWIPE_TOP,
    UI_SWIPE_BOTTOM,
} ui_swipe_dir_t;

typedef enum {
    UI_GESTURE_ACTION_NONE = 0,
    UI_GESTURE_ACTION_UNLOCK,
    UI_GESTURE_ACTION_HOME_PREV,
    UI_GESTURE_ACTION_HOME_NEXT,
    UI_GESTURE_ACTION_OPEN_CONTROL,
    UI_GESTURE_ACTION_CLOSE_CONTROL,
} ui_gesture_action_t;

/* Top band (below the 58 px status bar) that opens the control centre. */
#define UI_GESTURE_CONTROL_TOP_Y 140

/* Pure routing decision, kept free of LVGL so host tests can cover every page,
 * direction and the modal case.  press_y is the press start in logical canvas
 * coordinates; screen_h is 800 on the target panel. */
ui_gesture_action_t ui_gesture_resolve(ui_page_t page, ui_swipe_dir_t dir, int press_y,
                                       int screen_h, bool modal_open);

#ifdef __cplusplus
}
#endif
