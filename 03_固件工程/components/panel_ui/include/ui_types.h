#pragma once

/* Page identifiers shared by UI code, the gesture router and host tests.
 * Deliberately free of LVGL types so the router can be unit tested on the host. */
typedef enum {
    UI_PAGE_LOCK = 0,
    UI_PAGE_HOME,
    UI_PAGE_CONTROL,
    UI_PAGE_SETTINGS,
    UI_PAGE_WEATHER,
    UI_PAGE_MONITOR,
    UI_PAGE_CALCULATOR,
    UI_PAGE_CALENDAR,
    UI_PAGE_CLOCK,
    UI_PAGE_FILES,
    UI_PAGE_NONE = 0xFF,
} ui_page_t;
