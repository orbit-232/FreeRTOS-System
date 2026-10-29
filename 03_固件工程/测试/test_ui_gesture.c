/*
 * Host test for the pure gesture router in ui_gesture.c.  It covers the lock
 * screen, control centre, home paging, the top pull-down band and the modal
 * guard, i.e. the decisions that used to be buried in an LVGL callback.
 */
#include <stdio.h>

#include "ui_gesture.h"
#include "ui_types.h"

static int failures;

static void expect(const char *name, ui_page_t page, ui_swipe_dir_t dir, int press_y,
                   int screen_h, bool modal, ui_gesture_action_t want)
{
    ui_gesture_action_t got = ui_gesture_resolve(page, dir, press_y, screen_h, modal);
    printf("%-38s page=%3d dir=%d y=%4d h=%3d modal=%d -> %d (want %d)\n", name,
           (int)page, (int)dir, press_y, screen_h, modal, (int)got, (int)want);
    if (got != want) {
        printf("  FAIL\n");
        failures++;
    }
}

int main(void)
{
    expect("lock swipe up unlocks", UI_PAGE_LOCK, UI_SWIPE_TOP, 600, 800, false,
           UI_GESTURE_ACTION_UNLOCK);
    expect("lock swipe down ignored", UI_PAGE_LOCK, UI_SWIPE_BOTTOM, 100, 800, false,
           UI_GESTURE_ACTION_NONE);
    expect("lock modal ignored", UI_PAGE_LOCK, UI_SWIPE_TOP, 600, 800, true,
           UI_GESTURE_ACTION_NONE);

    expect("control bottom-edge swipe up closes", UI_PAGE_CONTROL, UI_SWIPE_TOP, 700, 800,
           false, UI_GESTURE_ACTION_CLOSE_CONTROL);
    expect("control top swipe up ignored", UI_PAGE_CONTROL, UI_SWIPE_TOP, 300, 800, false,
           UI_GESTURE_ACTION_NONE);
    expect("control horizontal ignored", UI_PAGE_CONTROL, UI_SWIPE_LEFT, 700, 800, false,
           UI_GESTURE_ACTION_NONE);

    expect("home top-edge pull down opens", UI_PAGE_HOME, UI_SWIPE_BOTTOM, 100, 800, false,
           UI_GESTURE_ACTION_OPEN_CONTROL);
    expect("home pull down below band ignored", UI_PAGE_HOME, UI_SWIPE_BOTTOM, 200, 800,
           false, UI_GESTURE_ACTION_NONE);
    expect("home swipe left next page", UI_PAGE_HOME, UI_SWIPE_LEFT, 400, 800, false,
           UI_GESTURE_ACTION_HOME_NEXT);
    expect("home swipe right previous page", UI_PAGE_HOME, UI_SWIPE_RIGHT, 400, 800, false,
           UI_GESTURE_ACTION_HOME_PREV);
    expect("home swipe up ignored", UI_PAGE_HOME, UI_SWIPE_TOP, 100, 800, false,
           UI_GESTURE_ACTION_NONE);

    expect("settings top pull down opens", UI_PAGE_SETTINGS, UI_SWIPE_BOTTOM, 50, 800, false,
           UI_GESTURE_ACTION_OPEN_CONTROL);
    expect("settings horizontal ignored", UI_PAGE_SETTINGS, UI_SWIPE_LEFT, 400, 800, false,
           UI_GESTURE_ACTION_NONE);
    expect("settings modal guard", UI_PAGE_SETTINGS, UI_SWIPE_BOTTOM, 50, 800, true,
           UI_GESTURE_ACTION_NONE);

    expect("weather top pull down opens", UI_PAGE_WEATHER, UI_SWIPE_BOTTOM, 139, 800, false,
           UI_GESTURE_ACTION_OPEN_CONTROL);
    expect("weather at threshold ignored", UI_PAGE_WEATHER, UI_SWIPE_BOTTOM, 140, 800, false,
           UI_GESTURE_ACTION_NONE);
    expect("zero screen height does not crash", UI_PAGE_CONTROL, UI_SWIPE_TOP, 700, 0, false,
           UI_GESTURE_ACTION_NONE);

    if (failures != 0) {
        printf("FAIL: gesture routing failures=%d\n", failures);
        return 1;
    }
    printf("PASS: gesture routing covered for lock/home/control/settings/weather + modal\n");
    return 0;
}
