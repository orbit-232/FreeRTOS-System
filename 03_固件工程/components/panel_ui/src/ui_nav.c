#include "ui_nav.h"

#include <string.h>

bool ui_nav_model_is_app(ui_page_t page)
{
    return page == UI_PAGE_SETTINGS || page == UI_PAGE_WEATHER || page == UI_PAGE_MONITOR ||
           page == UI_PAGE_CALCULATOR || page == UI_PAGE_CALENDAR || page == UI_PAGE_CLOCK ||
           page == UI_PAGE_FILES;
}

void ui_nav_model_init(ui_nav_model_t *model, ui_page_t current)
{
    if (model == NULL) {
        return;
    }
    memset(model, 0, sizeof(*model));
    model->current = current;
    model->control_source = UI_PAGE_HOME;
    model->locked = current == UI_PAGE_LOCK;
}

static void history_push(ui_nav_model_t *model, ui_page_t page)
{
    if (model->depth < UI_NAV_HISTORY_MAX) {
        model->history[model->depth++] = page;
        return;
    }

    /* Keep the newest eight entries.  Dropping the oldest entry is preferable
     * to unbounded growth or dropping the page the user just left. */
    memmove(&model->history[0], &model->history[1],
            sizeof(model->history[0]) * (UI_NAV_HISTORY_MAX - 1));
    model->history[UI_NAV_HISTORY_MAX - 1] = page;
}

bool ui_nav_model_unlock(ui_nav_model_t *model)
{
    if (model == NULL || !model->locked) {
        return false;
    }
    model->locked = false;
    model->current = UI_PAGE_HOME;
    model->depth = 0;
    return true;
}

bool ui_nav_model_enter(ui_nav_model_t *model, ui_page_t target, ui_nav_motion_t *motion)
{
    if (motion != NULL) {
        *motion = UI_NAV_MOTION_NONE;
    }
    if (model == NULL || model->locked || model->control_open) {
        return false;
    }
    if (target == UI_PAGE_NONE || target == UI_PAGE_LOCK || target == UI_PAGE_CONTROL ||
        target == model->current) {
        return false;
    }

    if (target == UI_PAGE_HOME) {
        bool changed = model->current != UI_PAGE_HOME || model->depth != 0;
        model->depth = 0;
        model->current = UI_PAGE_HOME;
        if (motion != NULL) {
            *motion = changed ? UI_NAV_MOTION_BACK : UI_NAV_MOTION_NONE;
        }
        return changed;
    }

    if (!ui_nav_model_is_app(target)) {
        return false;
    }

    if (model->current != UI_PAGE_HOME) {
        history_push(model, model->current);
    }
    model->current = target;
    if (motion != NULL) {
        *motion = UI_NAV_MOTION_FORWARD;
    }
    return true;
}

bool ui_nav_model_back(ui_nav_model_t *model, ui_page_t *target, ui_nav_motion_t *motion)
{
    if (target != NULL) {
        *target = UI_PAGE_NONE;
    }
    if (motion != NULL) {
        *motion = UI_NAV_MOTION_NONE;
    }
    if (model == NULL || model->locked || model->control_open ||
        model->current == UI_PAGE_HOME || model->current == UI_PAGE_LOCK) {
        return false;
    }

    ui_page_t next = UI_PAGE_HOME;
    if (model->depth > 0) {
        next = model->history[--model->depth];
    }
    model->current = next;
    if (next == UI_PAGE_HOME) {
        model->depth = 0;
    }
    if (target != NULL) {
        *target = next;
    }
    if (motion != NULL) {
        *motion = UI_NAV_MOTION_BACK;
    }
    return true;
}

bool ui_nav_model_home(ui_nav_model_t *model, ui_nav_motion_t *motion)
{
    if (motion != NULL) {
        *motion = UI_NAV_MOTION_NONE;
    }
    if (model == NULL || model->locked || model->control_open) {
        return false;
    }
    if (model->current == UI_PAGE_HOME && model->depth == 0) {
        return false;
    }
    model->current = UI_PAGE_HOME;
    model->depth = 0;
    if (motion != NULL) {
        *motion = UI_NAV_MOTION_BACK;
    }
    return true;
}

bool ui_nav_model_control_open(ui_nav_model_t *model)
{
    if (model == NULL || model->locked || model->control_open) {
        return false;
    }
    model->control_source = model->current;
    model->control_open = true;
    return true;
}

bool ui_nav_model_control_close(ui_nav_model_t *model, ui_page_t *source)
{
    if (source != NULL) {
        *source = UI_PAGE_NONE;
    }
    if (model == NULL || !model->control_open) {
        return false;
    }
    model->control_open = false;
    if (source != NULL) {
        *source = model->control_source;
    }
    return true;
}

ui_nav_back_action_t ui_nav_model_back_action(const ui_nav_model_t *model, bool modal_open,
                                             bool settings_subsection)
{
    if (model == NULL) {
        return UI_NAV_BACK_DISABLED;
    }
    if (modal_open) {
        return UI_NAV_BACK_MODAL;
    }
    if (model->control_open) {
        return UI_NAV_BACK_CONTROL;
    }
    if (model->current == UI_PAGE_SETTINGS && settings_subsection) {
        return UI_NAV_BACK_SETTINGS;
    }
    if (model->locked || model->current == UI_PAGE_HOME || model->current == UI_PAGE_NONE) {
        return UI_NAV_BACK_DISABLED;
    }
    return UI_NAV_BACK_PAGE;
}

uint32_t ui_nav_motion_duration(ui_nav_motion_t motion, bool animations_enabled)
{
    if (!animations_enabled) {
        return 0;
    }
    switch (motion) {
    case UI_NAV_MOTION_FORWARD:
        return 220;
    case UI_NAV_MOTION_BACK:
        return 200;
    case UI_NAV_MOTION_UNLOCK:
        return 260;
    case UI_NAV_MOTION_CONTROL_OPEN:
    case UI_NAV_MOTION_CONTROL_CLOSE:
        return 220;
    case UI_NAV_MOTION_HOME_NEXT:
    case UI_NAV_MOTION_HOME_PREV:
        return 200;
    case UI_NAV_MOTION_FADE:
        return 140;
    default:
        return 0;
    }
}

const char *ui_nav_motion_name(ui_nav_motion_t motion)
{
    switch (motion) {
    case UI_NAV_MOTION_FORWARD:
        return "forward-right";
    case UI_NAV_MOTION_BACK:
        return "back-left";
    case UI_NAV_MOTION_UNLOCK:
        return "unlock-up";
    case UI_NAV_MOTION_CONTROL_OPEN:
        return "control-down";
    case UI_NAV_MOTION_CONTROL_CLOSE:
        return "control-up";
    case UI_NAV_MOTION_HOME_NEXT:
        return "home-next";
    case UI_NAV_MOTION_HOME_PREV:
        return "home-prev";
    case UI_NAV_MOTION_FADE:
        return "fade";
    default:
        return "none";
    }
}

bool ui_nav_pending_page(ui_nav_pending_t *pending, ui_page_t page)
{
    if (pending == NULL || page == UI_PAGE_NONE) {
        return false;
    }
    if (pending->kind == UI_NAV_PENDING_PAGE && pending->page == page) {
        return false;
    }
    pending->kind = UI_NAV_PENDING_PAGE;
    pending->page = page;
    return true;
}

bool ui_nav_pending_back(ui_nav_pending_t *pending)
{
    if (pending == NULL || pending->kind == UI_NAV_PENDING_BACK) {
        return false;
    }
    pending->kind = UI_NAV_PENDING_BACK;
    pending->page = UI_PAGE_NONE;
    return true;
}

bool ui_nav_pending_home(ui_nav_pending_t *pending)
{
    if (pending == NULL || pending->kind == UI_NAV_PENDING_HOME) {
        return false;
    }
    pending->kind = UI_NAV_PENDING_HOME;
    pending->page = UI_PAGE_NONE;
    return true;
}

bool ui_nav_pending_take(ui_nav_pending_t *pending, ui_nav_pending_t *out)
{
    if (pending == NULL || out == NULL || pending->kind == UI_NAV_PENDING_NONE) {
        return false;
    }
    *out = *pending;
    pending->kind = UI_NAV_PENDING_NONE;
    pending->page = UI_PAGE_NONE;
    return true;
}
