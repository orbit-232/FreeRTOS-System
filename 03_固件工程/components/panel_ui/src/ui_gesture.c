#include "ui_gesture.h"

ui_gesture_action_t ui_gesture_resolve(ui_page_t page, ui_swipe_dir_t dir, int press_y,
                                       int screen_h, bool modal_open)
{
    if (modal_open) {
        return UI_GESTURE_ACTION_NONE;
    }

    if (page == UI_PAGE_LOCK) {
        return dir == UI_SWIPE_TOP ? UI_GESTURE_ACTION_UNLOCK : UI_GESTURE_ACTION_NONE;
    }

    if (page == UI_PAGE_CONTROL) {
        if (dir == UI_SWIPE_TOP && screen_h > 0 && press_y > (screen_h * 2) / 3) {
            return UI_GESTURE_ACTION_CLOSE_CONTROL;
        }
        return UI_GESTURE_ACTION_NONE;
    }

    /* Pull down from the top edge to open the control centre on any page. */
    if (dir == UI_SWIPE_BOTTOM && press_y < UI_GESTURE_CONTROL_TOP_Y) {
        return UI_GESTURE_ACTION_OPEN_CONTROL;
    }

    if (page == UI_PAGE_HOME) {
        if (dir == UI_SWIPE_LEFT) {
            return UI_GESTURE_ACTION_HOME_NEXT;
        }
        if (dir == UI_SWIPE_RIGHT) {
            return UI_GESTURE_ACTION_HOME_PREV;
        }
    }

    return UI_GESTURE_ACTION_NONE;
}
