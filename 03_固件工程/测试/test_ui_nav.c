/*
 * Host tests for the LVGL-free navigation model.  These cover the semantics
 * that are easy to get wrong in UI callbacks: bounded history, the priority of
 * modal/control/settings back actions, home clearing history, and the
 * single-flight pending request policy used during rapid taps.
 */
#include <stdio.h>
#include <string.h>

#include "ui_nav.h"
#include "ui_types.h"

static int failures;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static void test_unlock_and_history(void)
{
    ui_nav_model_t nav;
    ui_nav_motion_t motion = UI_NAV_MOTION_NONE;
    ui_page_t target = UI_PAGE_NONE;

    ui_nav_model_init(&nav, UI_PAGE_LOCK);
    CHECK(nav.locked);
    CHECK(ui_nav_model_back_action(&nav, false, false) == UI_NAV_BACK_DISABLED);

    CHECK(ui_nav_model_unlock(&nav));
    CHECK(!nav.locked);
    CHECK(nav.current == UI_PAGE_HOME);
    CHECK(nav.depth == 0);
    CHECK(ui_nav_model_back_action(&nav, false, false) == UI_NAV_BACK_DISABLED);

    CHECK(ui_nav_model_enter(&nav, UI_PAGE_SETTINGS, &motion));
    CHECK(nav.current == UI_PAGE_SETTINGS);
    CHECK(nav.depth == 0);
    CHECK(motion == UI_NAV_MOTION_FORWARD);

    CHECK(ui_nav_model_enter(&nav, UI_PAGE_WEATHER, &motion));
    CHECK(nav.current == UI_PAGE_WEATHER);
    CHECK(nav.depth == 1);
    CHECK(nav.history[0] == UI_PAGE_SETTINGS);
    CHECK(motion == UI_NAV_MOTION_FORWARD);

    CHECK(ui_nav_model_back_action(&nav, false, false) == UI_NAV_BACK_PAGE);
    CHECK(ui_nav_model_back(&nav, &target, &motion));
    CHECK(target == UI_PAGE_SETTINGS);
    CHECK(nav.current == UI_PAGE_SETTINGS);
    CHECK(nav.depth == 0);
    CHECK(motion == UI_NAV_MOTION_BACK);

    CHECK(ui_nav_model_back(&nav, &target, &motion));
    CHECK(target == UI_PAGE_HOME);
    CHECK(nav.current == UI_PAGE_HOME);
    CHECK(nav.depth == 0);
    CHECK(!ui_nav_model_back(&nav, &target, &motion));
    CHECK(target == UI_PAGE_NONE);
}

static void test_history_is_bounded(void)
{
    static const ui_page_t pages[] = {
        UI_PAGE_SETTINGS, UI_PAGE_WEATHER, UI_PAGE_MONITOR, UI_PAGE_CALCULATOR,
        UI_PAGE_SETTINGS, UI_PAGE_WEATHER, UI_PAGE_MONITOR, UI_PAGE_CALCULATOR,
        UI_PAGE_SETTINGS, UI_PAGE_WEATHER, UI_PAGE_MONITOR,
    };
    ui_nav_model_t nav;
    ui_nav_motion_t motion = UI_NAV_MOTION_NONE;
    ui_nav_model_init(&nav, UI_PAGE_LOCK);
    CHECK(ui_nav_model_unlock(&nav));

    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        CHECK(ui_nav_model_enter(&nav, pages[i], &motion));
        CHECK(nav.depth <= UI_NAV_HISTORY_MAX);
    }
    CHECK(nav.depth == UI_NAV_HISTORY_MAX);

    /* The newest entry is always the page we just left. */
    CHECK(nav.history[UI_NAV_HISTORY_MAX - 1] == UI_PAGE_WEATHER);

    int pops = 0;
    ui_page_t target = UI_PAGE_NONE;
    while (ui_nav_model_back(&nav, &target, &motion)) {
        pops++;
        CHECK(pops <= UI_NAV_HISTORY_MAX + 1);
    }
    /* Eight retained previous pages plus the final return to Home. */
    CHECK(pops == UI_NAV_HISTORY_MAX + 1);
    CHECK(nav.current == UI_PAGE_HOME);
}

static void test_home_clears_history(void)
{
    ui_nav_model_t nav;
    ui_nav_motion_t motion = UI_NAV_MOTION_NONE;
    ui_nav_model_init(&nav, UI_PAGE_LOCK);
    CHECK(ui_nav_model_unlock(&nav));
    CHECK(ui_nav_model_enter(&nav, UI_PAGE_SETTINGS, &motion));
    CHECK(ui_nav_model_enter(&nav, UI_PAGE_CALCULATOR, &motion));
    CHECK(nav.depth == 1);

    CHECK(ui_nav_model_home(&nav, &motion));
    CHECK(nav.current == UI_PAGE_HOME);
    CHECK(nav.depth == 0);
    CHECK(motion == UI_NAV_MOTION_BACK);
    CHECK(!ui_nav_model_home(&nav, &motion));
    CHECK(!ui_nav_model_back(&nav, NULL, NULL));
}

static void test_control_source_is_preserved(void)
{
    ui_nav_model_t nav;
    ui_nav_motion_t motion = UI_NAV_MOTION_NONE;
    ui_page_t source = UI_PAGE_NONE;
    ui_nav_model_init(&nav, UI_PAGE_LOCK);
    CHECK(ui_nav_model_unlock(&nav));
    CHECK(ui_nav_model_enter(&nav, UI_PAGE_CALCULATOR, &motion));
    CHECK(nav.depth == 0);

    CHECK(ui_nav_model_control_open(&nav));
    CHECK(nav.control_open);
    CHECK(nav.current == UI_PAGE_CALCULATOR);
    CHECK(nav.depth == 0);
    CHECK(nav.control_source == UI_PAGE_CALCULATOR);
    CHECK(ui_nav_model_back_action(&nav, false, false) == UI_NAV_BACK_CONTROL);
    CHECK(!ui_nav_model_control_open(&nav));

    CHECK(ui_nav_model_control_close(&nav, &source));
    CHECK(!nav.control_open);
    CHECK(source == UI_PAGE_CALCULATOR);
    CHECK(nav.current == UI_PAGE_CALCULATOR);
    CHECK(nav.depth == 0);

    /* A calculator back still returns to its real source. */
    ui_page_t target = UI_PAGE_NONE;
    CHECK(ui_nav_model_back(&nav, &target, &motion));
    CHECK(target == UI_PAGE_HOME);
}

static void test_back_priority(void)
{
    ui_nav_model_t nav;
    ui_nav_motion_t motion = UI_NAV_MOTION_NONE;
    ui_nav_model_init(&nav, UI_PAGE_LOCK);
    CHECK(ui_nav_model_unlock(&nav));
    CHECK(ui_nav_model_enter(&nav, UI_PAGE_SETTINGS, &motion));

    CHECK(ui_nav_model_back_action(&nav, true, true) == UI_NAV_BACK_MODAL);
    CHECK(ui_nav_model_back_action(&nav, false, true) == UI_NAV_BACK_SETTINGS);
    CHECK(ui_nav_model_back_action(&nav, false, false) == UI_NAV_BACK_PAGE);

    CHECK(ui_nav_model_enter(&nav, UI_PAGE_CALCULATOR, &motion));
    CHECK(ui_nav_model_back_action(&nav, false, true) == UI_NAV_BACK_PAGE);

    CHECK(ui_nav_model_control_open(&nav));
    CHECK(ui_nav_model_back_action(&nav, false, true) == UI_NAV_BACK_CONTROL);
    CHECK(ui_nav_model_back_action(&nav, true, true) == UI_NAV_BACK_MODAL);
}

static void test_motion_durations(void)
{
    CHECK(ui_nav_motion_duration(UI_NAV_MOTION_FORWARD, true) == 220);
    CHECK(ui_nav_motion_duration(UI_NAV_MOTION_BACK, true) == 200);
    CHECK(ui_nav_motion_duration(UI_NAV_MOTION_UNLOCK, true) == 260);
    CHECK(ui_nav_motion_duration(UI_NAV_MOTION_CONTROL_OPEN, true) == 220);
    CHECK(ui_nav_motion_duration(UI_NAV_MOTION_CONTROL_CLOSE, true) == 220);
    CHECK(ui_nav_motion_duration(UI_NAV_MOTION_HOME_NEXT, true) == 200);
    CHECK(ui_nav_motion_duration(UI_NAV_MOTION_HOME_PREV, true) == 200);
    CHECK(ui_nav_motion_duration(UI_NAV_MOTION_FADE, true) == 140);
    CHECK(ui_nav_motion_duration(UI_NAV_MOTION_FORWARD, false) == 0);
    CHECK(ui_nav_motion_duration(UI_NAV_MOTION_UNLOCK, false) == 0);
    CHECK(strcmp(ui_nav_motion_name(UI_NAV_MOTION_FORWARD), "forward-right") == 0);
}

static void test_single_pending_policy(void)
{
    ui_nav_pending_t pending = {0};
    ui_nav_pending_t out = {0};

    CHECK(ui_nav_pending_page(&pending, UI_PAGE_SETTINGS));
    CHECK(pending.kind == UI_NAV_PENDING_PAGE);
    CHECK(!ui_nav_pending_page(&pending, UI_PAGE_SETTINGS));
    CHECK(ui_nav_pending_page(&pending, UI_PAGE_WEATHER));
    CHECK(pending.page == UI_PAGE_WEATHER);
    CHECK(ui_nav_pending_back(&pending));
    CHECK(pending.kind == UI_NAV_PENDING_BACK);
    CHECK(!ui_nav_pending_back(&pending));
    CHECK(ui_nav_pending_home(&pending));
    CHECK(pending.kind == UI_NAV_PENDING_HOME);
    CHECK(!ui_nav_pending_home(&pending));

    CHECK(ui_nav_pending_take(&pending, &out));
    CHECK(out.kind == UI_NAV_PENDING_HOME);
    CHECK(pending.kind == UI_NAV_PENDING_NONE);
    CHECK(!ui_nav_pending_take(&pending, &out));
}

int main(void)
{
    test_unlock_and_history();
    test_history_is_bounded();
    test_home_clears_history();
    test_control_source_is_preserved();
    test_back_priority();
    test_motion_durations();
    test_single_pending_policy();

    if (failures != 0) {
        fprintf(stderr, "FAIL: navigation model failures=%d\n", failures);
        return 1;
    }
    printf("PASS: navigation model history/back/control/bounded-pending tests passed\n");
    return 0;
}
