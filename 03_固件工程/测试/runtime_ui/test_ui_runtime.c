/*
 * Dynamic host test for the real panel_ui router, fixed Android-style system
 * bar and Overview.  It drives actual page/overlay/event code with LVGL; it is
 * not a static screenshot test and does not touch hardware.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_diag.h"
#include "ui_nav.h"
#include "ui_network_panel.h"
#include "ui_pages.h"
#include "ui_perf.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#ifdef WAVESHARE_HOST_SIM
void panel_services_test_set_wifi_connected(bool connected);
void panel_services_test_set_time_ok(bool ok);
void panel_services_test_set_time_calibrated(bool calibrated);
void panel_services_test_set_weather(const panel_weather_snapshot_t *snapshot);
void panel_services_test_set_city_search(const panel_city_search_snapshot_t *snapshot);
void panel_services_test_reset_search(void);
const char *panel_services_test_last_search(void);
unsigned panel_services_test_last_city_index(void);
uint32_t panel_services_test_last_city_generation(void);
void panel_services_test_set_refresh_result(esp_err_t result);
unsigned panel_services_test_refresh_calls(void);
void panel_services_test_reset_refresh(void);
void panel_services_test_set_clock(const panel_clock_snapshot_t *snapshot);
void panel_services_test_set_clock_result(esp_err_t result);
unsigned panel_services_test_clock_calls(void);
void panel_services_test_reset_clock(void);
void panel_services_test_set_local_date(int year, int month, int day);
void panel_services_test_set_files(const panel_files_snapshot_t *snapshot);
void panel_bsp_test_set_boot_press(bool pressed);
void panel_services_test_set_assets(const panel_assets_status_t *status);
#endif

#define WIDTH 1280
#define HEIGHT 800

static uint16_t framebuffer[WIDTH * HEIGHT];
static uint8_t draw_buffer[WIDTH * HEIGHT * 2] __attribute__((aligned(4)));
static int failures;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    const uint16_t *src = (const uint16_t *)px_map;
    for (int32_t y = 0; y < h; y++) {
        int32_t dy = area->y1 + y;
        if (dy < 0 || dy >= HEIGHT) {
            continue;
        }
        for (int32_t x = 0; x < w; x++) {
            int32_t dx = area->x1 + x;
            if (dx >= 0 && dx < WIDTH) {
                framebuffer[dy * WIDTH + dx] = src[y * w + x];
            }
        }
    }
    lv_display_flush_ready(disp);
}

static void advance_ms(uint32_t ms)
{
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
}

static bool wait_idle(uint32_t max_ms)
{
    for (uint32_t elapsed = 0; elapsed < max_ms; elapsed += 10) {
        if (ui_nav_idle()) {
            return true;
        }
        lv_tick_inc(10);
        lv_timer_handler();
    }
    return ui_nav_idle();
}

static bool wait_page(ui_page_t page, uint32_t max_ms)
{
    for (uint32_t elapsed = 0; elapsed < max_ms; elapsed += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
        if (ui_current_page() == page && ui_nav_idle()) {
            return true;
        }
    }
    return ui_current_page() == page && ui_nav_idle();
}

static bool wait_overview_open(uint32_t max_ms)
{
    for (uint32_t elapsed = 0; elapsed < max_ms; elapsed += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
        if (ui_overview_is_open() && !ui_overview_is_animating()) {
            return true;
        }
    }
    return ui_overview_is_open() && !ui_overview_is_animating();
}

static bool wait_overview_closed(uint32_t max_ms)
{
    for (uint32_t elapsed = 0; elapsed < max_ms; elapsed += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
        if (!ui_overview_is_open() && !ui_overview_is_animating()) {
            return true;
        }
    }
    return !ui_overview_is_open() && !ui_overview_is_animating();
}

static bool wait_control_closed(uint32_t max_ms)
{
    for (uint32_t elapsed = 0; elapsed < max_ms; elapsed += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
        if (!ui_nav_control_open() && ui_nav_idle()) {
            return true;
        }
    }
    return !ui_nav_control_open() && ui_nav_idle();
}

static bool wait_control_open(uint32_t max_ms)
{
    for (uint32_t elapsed = 0; elapsed < max_ms; elapsed += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
        if (ui_nav_control_open() && ui_nav_idle()) {
            return true;
        }
    }
    return ui_nav_control_open() && ui_nav_idle();
}

static uint32_t page_count(void)
{
    lv_obj_t *root = lv_screen_active();
    return root != NULL ? lv_obj_get_child_count(root) : 0;
}

static lv_obj_t *active_page(void)
{
    return lv_obj_get_child(lv_screen_active(), 0);
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text)
{
    if (obj == NULL || text == NULL) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *value = lv_label_get_text(obj);
        if (value != NULL && strcmp(value, text) == 0) {
            return obj;
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static int count_label(lv_obj_t *obj, const char *text)
{
    if (obj == NULL) {
        return 0;
    }
    int total = 0;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *value = lv_label_get_text(obj);
        if (value != NULL && strcmp(value, text) == 0) {
            total++;
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        total += count_label(lv_obj_get_child(obj, i), text);
    }
    return total;
}

static lv_obj_t *find_label_contains(lv_obj_t *obj, const char *needle)
{
    if (obj == NULL || needle == NULL) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *value = lv_label_get_text(obj);
        if (value != NULL && strstr(value, needle) != NULL) {
            return obj;
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *found = find_label_contains(lv_obj_get_child(obj, i), needle);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static lv_point_t s_test_touch_point;
static lv_indev_state_t s_test_touch_state;

static void test_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = s_test_touch_point;
    data->state = s_test_touch_state;
}

static bool drag_notification_card(const char *title, int dx, int dy)
{
    lv_obj_t *label = find_label(active_page(), title);
    if (label == NULL) return false;
    lv_obj_t *card = lv_obj_get_parent(label);
    if (card == NULL) return false;
    lv_area_t area;
    lv_obj_get_coords(card, &area);
    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL) return false;
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, test_touch_read_cb);
    lv_indev_set_display(indev, lv_display_get_default());
    s_test_touch_point.x = (area.x1 + area.x2) / 2;
    s_test_touch_point.y = (area.y1 + area.y2) / 2;
    s_test_touch_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(indev);
    advance_ms(20);
    s_test_touch_point.x += dx;
    s_test_touch_point.y += dy;
    lv_indev_read(indev);
    advance_ms(20);
    s_test_touch_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);
    advance_ms(20);
    lv_indev_delete(indev);
    return find_label(active_page(), title) != NULL;
}

static lv_obj_t *find_obj_type(lv_obj_t *obj, const lv_obj_class_t *class_p)
{
    if (obj == NULL || class_p == NULL) {
        return NULL;
    }
    if (lv_obj_check_type(obj, class_p)) {
        return obj;
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *found = find_obj_type(lv_obj_get_child(obj, i), class_p);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static lv_obj_t *find_visible_label(lv_obj_t *obj, const char *text)
{
    if (obj == NULL || text == NULL || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *value = lv_label_get_text(obj);
        if (value != NULL && strcmp(value, text) == 0) {
            return obj;
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *found = find_visible_label(lv_obj_get_child(obj, i), text);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static bool click_visible_text(lv_obj_t *page, const char *text)
{
    lv_obj_t *label = find_visible_label(page, text);
    if (label == NULL) {
        return false;
    }
    lv_obj_t *parent = lv_obj_get_parent(label);
    if (parent == NULL) {
        return false;
    }
    lv_obj_send_event(parent, LV_EVENT_CLICKED, NULL);
    return true;
}

static bool rects_overlap(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static bool click_text(lv_obj_t *page, const char *text)
{
    lv_obj_t *label = find_label(page, text);
    if (label == NULL) {
        return false;
    }
    lv_obj_t *button = lv_obj_get_parent(label);
    if (button == NULL) {
        return false;
    }
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    return true;
}

static void set_animations(bool enabled)
{
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    settings.animations = enabled;
    CHECK(panel_services_update_settings(&settings));
}

static lv_obj_t *find_magic(lv_obj_t *obj)
{
    if (obj == NULL) {
        return NULL;
    }
    if ((uintptr_t)lv_obj_get_user_data(obj) == UI_NAV_BAR_MAGIC) {
        return obj;
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *found = find_magic(lv_obj_get_child(obj, i));
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static lv_obj_t *system_bar(void)
{
    return find_magic(lv_layer_sys());
}

static int count_system_bars(void)
{
    int count = 0;
    lv_obj_t *bar = find_magic(lv_layer_sys());
    if (bar != NULL) {
        count = 1;
    }
    return count;
}

static bool system_key_click(int index)
{
    lv_obj_t *bar = system_bar();
    if (bar == NULL || lv_obj_get_child_count(bar) < 3) {
        return false;
    }
    lv_obj_t *button = lv_obj_get_child(bar, index);
    if (button == NULL) {
        return false;
    }
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    return true;
}

static lv_obj_t *system_key(int index)
{
    lv_obj_t *bar = system_bar();
    if (bar == NULL || lv_obj_get_child_count(bar) < 3) {
        return NULL;
    }
    return lv_obj_get_child(bar, index);
}

static void check_system_bar_geometry(void)
{
    lv_obj_t *bar = system_bar();
    CHECK(bar != NULL);
    if (bar == NULL) {
        return;
    }
    lv_obj_update_layout(bar);
    CHECK(lv_obj_get_y(bar) == UI_NAV_Y);
    CHECK(lv_obj_get_height(bar) == UI_NAV_HEIGHT);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *button = system_key(i);
        CHECK(button != NULL);
        if (button != NULL) {
            CHECK(lv_obj_get_x(button) == 416 + i * 160);
            CHECK(lv_obj_get_y(button) == 8);
            CHECK(lv_obj_get_width(button) == 128);
            CHECK(lv_obj_get_height(button) == 48);
        }
    }
    CHECK(find_label(bar, "返回") == NULL);
    CHECK(find_label(bar, "主屏") == NULL);
}

static void check_system_bar_colors(void)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *bar = system_bar();
    CHECK(bar != NULL);
    if (bar == NULL) {
        return;
    }
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(bar, 0), p->surface));
    for (int i = 0; i < 3; i++) {
        lv_obj_t *button = system_key(i);
        CHECK(button != NULL);
        if (button == NULL) {
            continue;
        }
        CHECK(lv_color_eq(lv_obj_get_style_bg_color(button, LV_STATE_PRESSED),
                          p->surface_alt));
        lv_obj_t *icon = lv_obj_get_child(button, 0);
        CHECK(icon != NULL);
        if (icon == NULL) {
            continue;
        }
        if (i == 0) {
            CHECK(lv_color_eq(lv_obj_get_style_line_color(icon, 0), p->text));
        } else {
            CHECK(lv_color_eq(lv_obj_get_style_border_color(icon, 0), p->text));
        }
    }
}

static lv_obj_t *find_button_for_page(lv_obj_t *obj, ui_page_t page)
{
    if (obj == NULL) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_button_class) &&
        (ui_page_t)(uintptr_t)lv_obj_get_user_data(obj) == page) {
        return obj;
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *found = find_button_for_page(lv_obj_get_child(obj, i), page);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static lv_obj_t *overview_root(void)
{
    lv_obj_t *label = find_label(lv_layer_top(), "最近应用");
    if (label == NULL) {
        return NULL;
    }
    lv_obj_t *obj = label;
    while (lv_obj_get_parent(obj) != lv_layer_top()) {
        obj = lv_obj_get_parent(obj);
    }
    return obj;
}

static bool open_overview(void)
{
    if (!system_key_click(2)) {
        return false;
    }
    advance_ms(260);
    return ui_overview_is_open();
}

static void close_overview_back(void)
{
    system_key_click(0);
    advance_ms(260);
}

static void home_key(void)
{
    system_key_click(1);
    advance_ms(800);
}

static void back_key(void)
{
    system_key_click(0);
    advance_ms(400);
}

static bool click_overview_app(const char *name)
{
    lv_obj_t *root = overview_root();
    lv_obj_t *label = find_label(root, name);
    if (label == NULL) {
        return false;
    }
    lv_obj_t *card = lv_obj_get_parent(label);
    lv_obj_send_event(card, LV_EVENT_CLICKED, NULL);
    return true;
}

static void clear_overview_cards(void)
{
    lv_obj_t *root = overview_root();
    if (root == NULL) {
        return;
    }
    lv_obj_t *label = find_label(root, "全部清除");
    if (label != NULL) {
        lv_obj_t *button = lv_obj_get_parent(label);
        lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
        advance_ms(40);
    }
}

static bool dismiss_overview_page(ui_page_t page)
{
    lv_obj_t *root = overview_root();
    lv_obj_t *button = find_button_for_page(root, page);
    if (button == NULL) {
        return false;
    }
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    advance_ms(40);
    return true;
}

static bool inside_nav_bar(lv_obj_t *obj, lv_obj_t *page){
    while (obj != NULL && obj != page) {
        if ((uintptr_t)lv_obj_get_user_data(obj) == UI_NAV_BAR_MAGIC) {
            return true;
        }
        obj = lv_obj_get_parent(obj);
    }
    return false;
}

static int count_nav_bars(lv_obj_t *obj)
{
    if (obj == NULL) {
        return 0;
    }
    int count = (uintptr_t)lv_obj_get_user_data(obj) == UI_NAV_BAR_MAGIC ? 1 : 0;
    uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; i++) {
        count += count_nav_bars(lv_obj_get_child(obj, i));
    }
    return count;
}

static void check_layout_tree(lv_obj_t *obj, lv_obj_t *page, bool enforce_bottom)
{
    if (obj == NULL) {
        return;
    }
    if (obj != page && !inside_nav_bar(obj, page)) {
        lv_area_t coords;
        lv_obj_get_coords(obj, &coords);
        bool full_screen = lv_obj_get_width(obj) >= WIDTH && lv_obj_get_height(obj) >= HEIGHT;
        bool has_text = false;
        if (lv_obj_check_type(obj, &lv_label_class)) {
            const char *text = lv_label_get_text(obj);
            has_text = text != NULL && text[0] != '\0';
        }
        bool clickable = lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        if (enforce_bottom && !full_screen && (has_text || clickable) &&
            coords.y2 > UI_CONTENT_BOTTOM) {
            fprintf(stderr, "LAYOUT overflow: obj=%p text=%d clickable=%d y2=%d > %d\n",
                    (void *)obj, has_text ? 1 : 0, clickable ? 1 : 0, (int)coords.y2,
                    UI_CONTENT_BOTTOM);
            failures++;
        }
    }
    uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; i++) {
        check_layout_tree(lv_obj_get_child(obj, i), page, enforce_bottom);
    }
}

static void check_layout_obj(const char *name, lv_obj_t *page, bool enforce_bottom,
                             bool unlocked)
{
    CHECK(page != NULL);
    if (page == NULL) {
        return;
    }
    CHECK(count_nav_bars(page) == 0);
    CHECK(count_system_bars() == 1);
    CHECK(ui_system_bar_is_visible() == unlocked);
    if (unlocked) {
        check_system_bar_geometry();
    }
    check_layout_tree(page, page, enforce_bottom);
    printf("LAYOUT %s page_bars=0 system_bars=%d visible=%d\n", name,
           count_system_bars(), ui_system_bar_is_visible() ? 1 : 0);
}

static void check_page_layout(const char *name, bool enforce_bottom, bool unlocked)
{
    check_layout_obj(name, active_page(), enforce_bottom, unlocked);
}

static void check_overview_layout(void)
{
    lv_obj_t *root = overview_root();
    CHECK(root != NULL);
    if (root != NULL) {
        check_layout_obj("overview", root, true, true);
    }
}

static bool mru_contains(ui_page_t page)
{
    for (int i = 0; i < ui_overview_recent_count(); i++) {
        if (ui_overview_recent_at(i) == page) {
            return true;
        }
    }
    return false;
}

static bool open_app_from_home(const char *name)
{
    lv_obj_t *label = find_label(active_page(), name);
    if (label == NULL) {
        return false;
    }
    lv_obj_t *button = lv_obj_get_parent(label);
    if (button == NULL) {
        return false;
    }
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    return wait_idle(500);
}

static void calculator_summary(char *buf, size_t len)
{
    ui_page_calculator_state_summary(buf, len);
}

static void boot_ui(panel_theme_t theme)
{
    lv_init();
    lv_tick_inc(1);
    lv_display_t *disp = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_default(disp);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, draw_buffer, NULL, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    panel_settings_t settings;
    panel_services_get_settings(&settings);
    settings.animations = true;
    settings.theme = theme;
    CHECK(panel_services_update_settings(&settings));
    ui_start();
    advance_ms(20);
}

static void test_boot_lock_and_unlock(void)
{
    CHECK(ui_current_page() == UI_PAGE_LOCK);
    CHECK(page_count() == 1);
    CHECK(system_bar() != NULL);
    CHECK(!ui_system_bar_is_visible());
    CHECK(count_system_bars() == 1);
    check_page_layout("lock", false, false);
    panel_bsp_test_set_boot_press(true);
    advance_ms(60);
    CHECK(ui_current_page() == UI_PAGE_LOCK);
    CHECK(drag_notification_card("固件更新可用", 4, 80));
    CHECK(!drag_notification_card("固件更新可用", 110, 4));
    CHECK(find_label(active_page(), "定时任务完成") != NULL);

    ui_navigate(UI_PAGE_HOME);
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(page_count() == 2);
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *home_page = lv_obj_get_child(root, 0);
    lv_obj_t *lock_page = lv_obj_get_child(root, 1);
    CHECK(lv_obj_get_y(home_page) == 0);
    CHECK(lv_obj_get_y(lock_page) == 0);
    advance_ms(100);
    CHECK(lv_obj_get_y(lock_page) < 0);
    CHECK(lv_obj_get_y(home_page) == 0);
    CHECK(wait_idle(400));
    CHECK(page_count() == 1);
    CHECK(ui_system_bar_is_visible());
    check_page_layout("home", true, true);

    panel_bsp_test_set_boot_press(true);
    advance_ms(60);
    CHECK(wait_page(UI_PAGE_LOCK, 1600));
    ui_navigate(UI_PAGE_HOME);
    CHECK(wait_page(UI_PAGE_HOME, 1200));

    ui_navigate(UI_PAGE_SETTINGS);
    CHECK(wait_idle(400));
    panel_bsp_test_set_boot_press(true);
    advance_ms(60);
    CHECK(wait_page(UI_PAGE_LOCK, 1800));
    ui_navigate(UI_PAGE_HOME);
    CHECK(wait_page(UI_PAGE_HOME, 1200));

    ui_open_control();
    CHECK(wait_idle(500));
    panel_bsp_test_set_boot_press(true);
    advance_ms(60);
    CHECK(wait_page(UI_PAGE_LOCK, 1800));
    ui_navigate(UI_PAGE_HOME);
    CHECK(wait_page(UI_PAGE_HOME, 1200));

    ui_open_overview();
    CHECK(wait_idle(500));
    panel_bsp_test_set_boot_press(true);
    advance_ms(60);
    CHECK(wait_page(UI_PAGE_LOCK, 1800));
    ui_navigate(UI_PAGE_HOME);
    CHECK(wait_page(UI_PAGE_HOME, 1200));

    CHECK(ui_show_notice("Lock test", "", "Boot key closes this modal"));
    advance_ms(300);
    panel_bsp_test_set_boot_press(true);
    advance_ms(60);
    CHECK(wait_page(UI_PAGE_LOCK, 1800));
    ui_navigate(UI_PAGE_HOME);
    CHECK(wait_page(UI_PAGE_HOME, 1200));
}

static void test_fixed_bar_and_three_keys(void)
{
    lv_obj_t *bar = system_bar();
    CHECK(bar != NULL);
    int fixed_y = lv_obj_get_y(bar);
    ui_navigate(UI_PAGE_SETTINGS);
    advance_ms(100);
    CHECK(lv_obj_get_y(system_bar()) == fixed_y);
    CHECK(wait_idle(400));
    CHECK(lv_obj_get_y(system_bar()) == fixed_y);

    CHECK(system_key_click(0)); /* Back */
    CHECK(wait_idle(400));
    CHECK(ui_current_page() == UI_PAGE_HOME);

    ui_navigate(UI_PAGE_WEATHER);
    CHECK(wait_idle(400));
    CHECK(system_key_click(1)); /* Home */
    CHECK(wait_idle(400));
    CHECK(ui_current_page() == UI_PAGE_HOME);

    ui_navigate(UI_PAGE_MONITOR);
    CHECK(wait_idle(400));
    CHECK(open_overview());
    CHECK(ui_overview_is_open());
    CHECK(ui_current_page() == UI_PAGE_MONITOR);
    close_overview_back();
    CHECK(!ui_overview_is_open());
    CHECK(ui_current_page() == UI_PAGE_MONITOR);
    CHECK(system_key_click(1));
    CHECK(wait_idle(400));
    CHECK(ui_current_page() == UI_PAGE_HOME);
}

static void test_theme_live_cycle(void)
{
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(ui_theme_effective() == PANEL_THEME_LIGHT);
    check_system_bar_colors();

    ui_theme_set(PANEL_THEME_DARK);
    ui_rebuild_current_async();
    advance_ms(400);
    CHECK(wait_idle(600));
    CHECK(ui_theme_effective() == PANEL_THEME_DARK);
    check_system_bar_colors();

    ui_theme_set(PANEL_THEME_LIGHT);
    ui_rebuild_current_async();
    advance_ms(400);
    CHECK(wait_idle(600));
    CHECK(ui_theme_effective() == PANEL_THEME_LIGHT);
    check_system_bar_colors();
    CHECK(ui_current_page() == UI_PAGE_HOME);
}

static void test_dark_cold_boot(void)
{
    CHECK(ui_current_page() == UI_PAGE_LOCK);
    CHECK(!ui_system_bar_is_visible());
    ui_navigate(UI_PAGE_HOME);
    CHECK(wait_idle(500));
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(ui_theme_effective() == PANEL_THEME_DARK);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(lv_screen_active(), 0),
                      ui_palette()->bg));
    check_system_bar_colors();
    check_page_layout("home_dark_cold", true, true);
}

static void test_overview_from_every_page(void)
{
    static const ui_page_t pages[] = {
        UI_PAGE_SETTINGS, UI_PAGE_WEATHER, UI_PAGE_MONITOR, UI_PAGE_CALCULATOR,
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        ui_navigate(pages[i]);
        CHECK(wait_idle(400));
        lv_obj_t *source = active_page();
        CHECK(open_overview());
        CHECK(ui_overview_is_open());
        CHECK(active_page() == source);
        check_overview_layout();
        close_overview_back();
        CHECK(!ui_overview_is_open());
        CHECK(ui_current_page() == pages[i]);
        CHECK(active_page() == source);
        CHECK(system_key_click(1));
        CHECK(wait_idle(400));
        CHECK(ui_current_page() == UI_PAGE_HOME);
    }

    CHECK(open_overview());
    close_overview_back();
    CHECK(ui_current_page() == UI_PAGE_HOME);

    ui_navigate(UI_PAGE_WEATHER);
    CHECK(wait_idle(400));
    ui_open_control();
    CHECK(wait_idle(400));
    CHECK(open_overview());
    CHECK(ui_nav_control_open());
    close_overview_back();
    CHECK(!ui_overview_is_open());
    CHECK(ui_nav_control_open());
    back_key();
    CHECK(ui_current_page() == UI_PAGE_WEATHER);
    CHECK(!ui_nav_control_open());
    home_key();
    CHECK(ui_current_page() == UI_PAGE_HOME);
}

static void test_forward_back_rapid_and_settings_back(void)
{
    lv_obj_t *root = lv_screen_active();
    ui_navigate(UI_PAGE_SETTINGS);
    CHECK(page_count() == 2);
    lv_obj_t *old = lv_obj_get_child(root, 0);
    lv_obj_t *incoming = lv_obj_get_child(root, 1);
    lv_obj_update_layout(root);
    CHECK(lv_obj_get_x(old) == 0);
    CHECK(lv_obj_get_x(incoming) == WIDTH);
    advance_ms(80);
    CHECK(lv_obj_get_x(old) < 0);
    CHECK(lv_obj_get_x(incoming) > 0);
    CHECK(wait_idle(400));
    CHECK(page_count() == 1);
    check_page_layout("settings_forward", true, true);

    lv_obj_t *settings = active_page();
    CHECK(click_text(settings, "显示"));
    advance_ms(220);
    CHECK(ui_current_page() == UI_PAGE_SETTINGS);
    back_key();
    CHECK(ui_current_page() == UI_PAGE_SETTINGS);
    CHECK(page_count() == 1);
    CHECK(find_label(active_page(), "亮度 · 真实背光") == NULL);
    back_key();
    CHECK(ui_current_page() == UI_PAGE_HOME);

    ui_navigate(UI_PAGE_WEATHER);
    ui_navigate(UI_PAGE_CALCULATOR);
    CHECK(wait_idle(900));
    CHECK(ui_current_page() == UI_PAGE_CALCULATOR);
    CHECK(page_count() == 1);
    ui_back();
    ui_back();
    CHECK(wait_idle(900));
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(page_count() == 1);

    ui_nav_stats_t stats;
    ui_nav_get_stats(&stats);
    CHECK(stats.coalesced >= 2);
    CHECK(stats.pending_dispatched >= 2);
}

static void test_control_source_and_calculator_state(void)
{
    ui_navigate(UI_PAGE_HOME);
    CHECK(wait_idle(400));
    ui_open_control();
    CHECK(wait_idle(400));
    CHECK(ui_nav_control_open());
    back_key();
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(!ui_nav_control_open());

    ui_navigate(UI_PAGE_CALCULATOR);
    CHECK(wait_idle(400));
    lv_obj_t *calc = active_page();
    CHECK(click_text(calc, "7"));
    advance_ms(20);
    CHECK(count_label(calc, "7") >= 2);

    ui_open_control();
    CHECK(wait_idle(400));
    CHECK(ui_current_page() == UI_PAGE_CONTROL);
    CHECK(ui_nav_control_open());
    back_key();
    CHECK(ui_current_page() == UI_PAGE_CALCULATOR);
    CHECK(!ui_nav_control_open());
    CHECK(active_page() == calc);
    CHECK(count_label(calc, "7") >= 2);
    home_key();
    CHECK(ui_current_page() == UI_PAGE_HOME);
}

static void test_all_page_layouts_and_zero_animation(void)
{
    static const ui_page_t pages[] = {
        UI_PAGE_SETTINGS, UI_PAGE_WEATHER, UI_PAGE_MONITOR, UI_PAGE_CALCULATOR,
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        ui_navigate(pages[i]);
        CHECK(wait_idle(400));
        check_page_layout(i == 0 ? "settings" : i == 1 ? "weather" :
                          i == 2 ? "monitor" : "calculator", true, true);
        home_key();
    }

    ui_navigate(UI_PAGE_SETTINGS);
    CHECK(wait_idle(400));
    static const char *const sections[] = {"显示", "声音", "通知", "系统", "关于", "网络"};
    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        CHECK(click_text(active_page(), sections[i]));
        advance_ms(220);
        check_page_layout(sections[i], true, true);
    }
    home_key();

    ui_page_home_swipe(1);
    CHECK(wait_idle(400));
    check_page_layout("home_page_2", true, true);
    CHECK(ui_page_home_current_page() == 1);
    CHECK(find_label(active_page(), "系统概览") != NULL);
    ui_page_home_swipe(1);
    CHECK(wait_idle(400));
    check_page_layout("home_page_1_wrap", true, true);
    CHECK(ui_page_home_current_page() == 0);
    ui_page_home_swipe(-1);
    CHECK(wait_idle(400));
    CHECK(ui_page_home_current_page() == 1);
    ui_page_home_swipe(-1);
    CHECK(wait_idle(400));
    CHECK(ui_page_home_current_page() == 0);

    ui_navigate(UI_PAGE_WEATHER);
    CHECK(wait_idle(400));
    ui_open_control();
    CHECK(wait_idle(400));
    lv_obj_t *control = lv_obj_get_child(lv_layer_top(), 0);
    check_layout_obj("control_overlay", control, true, true);
    CHECK(find_label(control, "After Hours") != NULL);
    ui_close_control();
    CHECK(wait_idle(500));
    home_key();

    set_animations(false);
    ui_navigate(UI_PAGE_SETTINGS);
    CHECK(ui_current_page() == UI_PAGE_SETTINGS);
    CHECK(!ui_nav_transition_active());
    CHECK(page_count() == 1);
    ui_back();
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(!ui_nav_transition_active());
    CHECK(page_count() == 1);
    set_animations(true);
}

static void test_overview_mru_cards_and_empty(void)
{
    CHECK(open_overview());
    clear_overview_cards();
    CHECK(ui_overview_recent_count() == 0);
    CHECK(find_label(overview_root(), "暂无最近应用") != NULL);
    close_overview_back();

    static const ui_page_t order[] = {
        UI_PAGE_SETTINGS, UI_PAGE_WEATHER, UI_PAGE_MONITOR, UI_PAGE_CALCULATOR,
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        ui_navigate(order[i]);
        CHECK(wait_idle(400));
        home_key();
        CHECK(ui_current_page() == UI_PAGE_HOME);
    }

    CHECK(open_overview());
    CHECK(ui_overview_recent_count() == 4);
    CHECK(ui_overview_recent_at(0) == UI_PAGE_CALCULATOR);
    CHECK(ui_overview_recent_at(1) == UI_PAGE_MONITOR);
    CHECK(ui_overview_recent_at(2) == UI_PAGE_WEATHER);
    CHECK(ui_overview_recent_at(3) == UI_PAGE_SETTINGS);
    CHECK(find_label(overview_root(), "计算器") != NULL);
    CHECK(find_label(overview_root(), "监视器") != NULL);

    CHECK(dismiss_overview_page(UI_PAGE_MONITOR));
    CHECK(ui_overview_recent_count() == 3);
    CHECK(ui_overview_recent_at(0) == UI_PAGE_CALCULATOR);
    CHECK(ui_overview_is_open());

    clear_overview_cards();
    CHECK(ui_overview_recent_count() == 0);
    CHECK(find_label(overview_root(), "暂无最近应用") != NULL);
    clear_overview_cards(); /* repeated clear on empty/disabled card */
    CHECK(ui_overview_recent_count() == 0);
    close_overview_back();
    CHECK(open_overview());
    CHECK(ui_overview_recent_count() == 0);
    CHECK(find_label(overview_root(), "暂无最近应用") != NULL);
    close_overview_back();
}

static void test_calculator_forced_rebuild_state(void)
{
    ui_page_calculator_state_reset();
    ui_navigate(UI_PAGE_CALCULATOR);
    CHECK(wait_idle(400));
    lv_obj_t *calc = active_page();
    CHECK(click_text(calc, "1"));
    CHECK(click_text(calc, "2"));
    CHECK(click_text(calc, "+"));
    CHECK(click_text(calc, "3"));
    CHECK(click_text(calc, "="));
    advance_ms(30);
    char summary[96];
    calculator_summary(summary, sizeof(summary));
    CHECK(strcmp(summary, "15 · 历史 1") == 0);

    ui_navigate_force(UI_PAGE_CALCULATOR);
    CHECK(wait_idle(500));
    calc = active_page();
    calculator_summary(summary, sizeof(summary));
    CHECK(strcmp(summary, "15 · 历史 1") == 0);
    CHECK(find_label(calc, "15") != NULL);

    home_key();
    CHECK(open_overview());
    CHECK(click_overview_app("计算器"));
    advance_ms(700);
    CHECK(ui_current_page() == UI_PAGE_CALCULATOR);
    calculator_summary(summary, sizeof(summary));
    CHECK(strcmp(summary, "15 · 历史 1") == 0);
    home_key();
    ui_page_calculator_state_reset();
}

static void test_app_state_restore_and_dismiss_reset(void)
{
    ui_navigate(UI_PAGE_CALCULATOR);
    CHECK(wait_idle(400));
    lv_obj_t *calc = active_page();
    CHECK(click_text(calc, "7"));
    advance_ms(20);
    CHECK(count_label(calc, "7") >= 2);
    home_key();
    CHECK(ui_current_page() == UI_PAGE_HOME);

    CHECK(open_overview());
    CHECK(click_overview_app("计算器"));
    advance_ms(600);
    CHECK(ui_current_page() == UI_PAGE_CALCULATOR);
    calc = active_page();
    CHECK(count_label(calc, "7") >= 2);

    /* Dismissing the current source is a close, not a resurrection. */
    CHECK(open_overview());
    CHECK(dismiss_overview_page(UI_PAGE_CALCULATOR));
    CHECK(!mru_contains(UI_PAGE_CALCULATOR));
    close_overview_back();
    CHECK(wait_idle(600));
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(!mru_contains(UI_PAGE_CALCULATOR));
    CHECK(open_app_from_home("计算器"));
    calc = active_page();
    CHECK(count_label(calc, "7") == 1); /* transient input was reset */
    home_key();

    ui_navigate(UI_PAGE_SETTINGS);
    CHECK(wait_idle(400));
    lv_obj_t *settings = active_page();
    CHECK(click_text(settings, "显示"));
    advance_ms(220);
    CHECK(find_label(settings, "亮度 · 真实背光") != NULL);
    home_key();
    CHECK(ui_current_page() == UI_PAGE_HOME);

    CHECK(open_overview());
    CHECK(click_overview_app("设置"));
    advance_ms(600);
    CHECK(ui_current_page() == UI_PAGE_SETTINGS);
    settings = active_page();
    CHECK(find_label(settings, "亮度 · 真实背光") != NULL);
    CHECK(open_overview());
    CHECK(dismiss_overview_page(UI_PAGE_SETTINGS));
    close_overview_back();
    CHECK(wait_idle(600));
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(!mru_contains(UI_PAGE_SETTINGS));
    CHECK(open_app_from_home("设置"));
    settings = active_page();
    CHECK(ui_page_settings_current_section() == 0);
    CHECK(find_label(settings, "亮度 · 真实背光") == NULL);
    home_key();

    /* Clear-all while an app is the source must also return Home. */
    ui_navigate(UI_PAGE_CALCULATOR);
    CHECK(wait_idle(400));
    CHECK(click_text(active_page(), "7"));
    CHECK(open_overview());
    clear_overview_cards();
    CHECK(ui_overview_recent_count() == 0);
    close_overview_back();
    CHECK(wait_idle(600));
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(ui_overview_recent_count() == 0);
    CHECK(open_app_from_home("计算器"));
    calc = active_page();
    CHECK(count_label(calc, "7") == 1);
    home_key();

    /* If the source was dismissed, selecting another card must not leave the
     * dismissed source in Back history. */
    ui_navigate(UI_PAGE_WEATHER);
    CHECK(wait_idle(400));
    home_key();
    ui_navigate(UI_PAGE_CALCULATOR);
    CHECK(wait_idle(400));
    CHECK(click_text(active_page(), "7"));
    CHECK(open_overview());
    CHECK(dismiss_overview_page(UI_PAGE_CALCULATOR));
    CHECK(click_overview_app("天气"));
    advance_ms(700);
    CHECK(ui_current_page() == UI_PAGE_WEATHER);
    back_key();
    CHECK(wait_idle(500));
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(!mru_contains(UI_PAGE_CALCULATOR));

    /* Dismissing a different recent app preserves the exact source. */
    ui_navigate(UI_PAGE_SETTINGS);
    CHECK(wait_idle(400));
    CHECK(open_overview());
    CHECK(dismiss_overview_page(UI_PAGE_WEATHER));
    close_overview_back();
    CHECK(wait_idle(500));
    CHECK(ui_current_page() == UI_PAGE_SETTINGS);
    home_key();

    ui_page_home_swipe(1);
    CHECK(wait_idle(400));
    CHECK(ui_page_home_current_page() == 1);
    ui_navigate(UI_PAGE_WEATHER);
    CHECK(wait_idle(400));
    home_key();
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(ui_page_home_current_page() == 1);
    CHECK(find_label(active_page(), "系统概览") != NULL);
    ui_page_home_swipe(-1);
    CHECK(wait_idle(400));
}

static void test_modal_and_overview_priority(void)
{
    ui_navigate(UI_PAGE_CALCULATOR);
    CHECK(wait_idle(400));
    ui_show_notice("测试弹窗", "系统栏", "返回优先关闭弹窗，主页仍可回到启动器。");
    advance_ms(160);
    back_key();
    advance_ms(200);
    CHECK(ui_current_page() == UI_PAGE_CALCULATOR);
    CHECK(!ui_overview_is_open());

    ui_show_notice("测试弹窗", "系统栏", "再次测试 Home 的系统语义。");
    advance_ms(160);
    home_key();
    CHECK(ui_current_page() == UI_PAGE_HOME);

    ui_navigate(UI_PAGE_CALCULATOR);
    CHECK(wait_idle(400));
    ui_show_notice("测试弹窗", "系统栏", "Overview 应先关闭弹窗。");
    advance_ms(160);
    CHECK(open_overview());
    CHECK(ui_overview_is_open());
    close_overview_back();
    CHECK(ui_current_page() == UI_PAGE_CALCULATOR);
    home_key();
}

static void test_overview_home_and_card_selection(void)
{
    ui_navigate(UI_PAGE_SETTINGS);
    CHECK(wait_idle(400));
    home_key();
    CHECK(ui_current_page() == UI_PAGE_HOME);

    ui_navigate(UI_PAGE_WEATHER);
    CHECK(wait_idle(400));
    ui_open_control();
    CHECK(wait_idle(400));
    CHECK(open_overview());
    CHECK(ui_nav_control_open());
    home_key();
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(!ui_nav_control_open());
    CHECK(!ui_overview_is_open());

    ui_navigate(UI_PAGE_WEATHER);
    CHECK(wait_idle(400));
    ui_open_control();
    CHECK(wait_idle(400));
    CHECK(open_overview());
    CHECK(click_overview_app("设置"));
    advance_ms(900);
    CHECK(ui_current_page() == UI_PAGE_SETTINGS);
    CHECK(!ui_nav_control_open());
    CHECK(!ui_overview_is_open());
    home_key();
    CHECK(ui_current_page() == UI_PAGE_HOME);
}

static void test_repeated_overview_and_animation_off(void)
{
    for (int i = 0; i < 3; i++) {
        CHECK(open_overview());
        CHECK(ui_overview_is_open());
        system_key_click(2); /* second press during open animation or open state */
        advance_ms(260);
        if (ui_overview_is_open()) {
            close_overview_back();
        }
        CHECK(!ui_overview_is_open());
        CHECK(ui_current_page() == UI_PAGE_HOME);
    }

    set_animations(false);
    CHECK(system_key_click(2));
    advance_ms(30);
    CHECK(ui_overview_is_open());
    back_key();
    advance_ms(40);
    CHECK(!ui_overview_is_open());
    set_animations(true);

    ui_diag_set_active(true);
    ui_system_bar_refresh();
    CHECK(!ui_system_bar_is_visible());
    ui_diag_set_active(false);
    ui_system_bar_refresh();
    CHECK(ui_system_bar_is_visible());
}

static void test_demo_from_home(void)
{
    int mru_before = ui_overview_recent_count();
    ui_page_t mru_pages[4];
    for (int i = 0; i < mru_before && i < 4; i++) {
        mru_pages[i] = ui_overview_recent_at(i);
    }
    int section_before = ui_page_settings_current_section();
    char calc_before[96];
    calculator_summary(calc_before, sizeof(calc_before));

    CHECK(ui_nav_demo_start(1));
    advance_ms(50);
    for (int i = 0; i < 2500; i++) {
        advance_ms(10);
        if (!ui_nav_demo_active()) {
            break;
        }
    }
    CHECK(!ui_nav_demo_active());
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(page_count() == 1);
    CHECK(!ui_nav_control_open());
    CHECK(!ui_overview_is_open());
    CHECK(ui_overview_recent_count() == mru_before);
    for (int i = 0; i < mru_before && i < 4; i++) {
        CHECK(ui_overview_recent_at(i) == mru_pages[i]);
    }
    CHECK(ui_page_settings_current_section() == section_before);
    char calc_after[96];
    calculator_summary(calc_after, sizeof(calc_after));
    CHECK(strcmp(calc_before, calc_after) == 0);

    ui_nav_stats_t stats;
    ui_nav_get_stats(&stats);
    CHECK(stats.verified_app_enter >= 4);
    CHECK(stats.verified_app_back >= 4);
    CHECK(stats.verified_control_open >= 1);
    CHECK(stats.verified_control_close >= 1);
    CHECK(stats.verified_overview_open >= 1);
    CHECK(stats.verified_overview_close >= 1);
    CHECK(stats.verified_home_next >= 1);
    CHECK(stats.verified_home_prev >= 1);
    CHECK(stats.verify_failures == 0);
}

static void test_repeated_navigation_and_accounting(void)
{
    for (int i = 0; i < 8; i++) {
        ui_navigate(UI_PAGE_SETTINGS);
        CHECK(wait_idle(500));
        CHECK(ui_current_page() == UI_PAGE_SETTINGS);
        ui_back();
        CHECK(wait_idle(500));
        CHECK(ui_current_page() == UI_PAGE_HOME);
    }
    CHECK(page_count() == 1);
    CHECK(count_system_bars() == 1);
    CHECK(!ui_overview_is_open());
    CHECK(!ui_nav_control_open());
    check_page_layout("home_final", true, true);

    ui_nav_stats_t stats;
    ui_nav_get_stats(&stats);
    CHECK(stats.page_creates == stats.page_deletes + 1);
    CHECK(stats.max_history_depth <= UI_NAV_HISTORY_MAX);
    printf("STATS requests=%u accepted=%u coalesced=%u rejected=%u completed=%u "
           "creates=%u deletes=%u pending=%u max_depth=%u last_ms=%u max_ms=%u "
           "verified enter=%u back=%u control=%u/%u overview=%u/%u page=%u/%u failures=%u\n",
           (unsigned)stats.requests, (unsigned)stats.accepted, (unsigned)stats.coalesced,
           (unsigned)stats.rejected, (unsigned)stats.completed,
           (unsigned)stats.page_creates, (unsigned)stats.page_deletes,
           (unsigned)stats.pending_dispatched, (unsigned)stats.max_history_depth,
           (unsigned)stats.last_transition_ms, (unsigned)stats.max_transition_ms,
           (unsigned)stats.verified_app_enter, (unsigned)stats.verified_app_back,
           (unsigned)stats.verified_control_open, (unsigned)stats.verified_control_close,
           (unsigned)stats.verified_overview_open, (unsigned)stats.verified_overview_close,
           (unsigned)stats.verified_home_next, (unsigned)stats.verified_home_prev,
           (unsigned)stats.verify_failures);
}

static void test_bench_state_restore(void)
{
    set_animations(true);
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(ui_page_home_current_page() == 0);

    ui_page_calculator_state_reset();
    ui_navigate(UI_PAGE_CALCULATOR);
    CHECK(wait_idle(400));
    CHECK(click_text(active_page(), "4"));
    CHECK(click_text(active_page(), "2"));
    advance_ms(30);
    home_key();
    char calc_before[96];
    calculator_summary(calc_before, sizeof(calc_before));
    CHECK(strcmp(calc_before, "42") == 0);

    ui_navigate(UI_PAGE_SETTINGS);
    CHECK(wait_idle(400));
    CHECK(click_text(active_page(), "显示"));
    advance_ms(220);
    home_key();
    int section_before = ui_page_settings_current_section();
    CHECK(section_before == 1);

    int mru_before = ui_overview_recent_count();
    ui_page_t mru_pages[4];
    for (int i = 0; i < mru_before && i < 4; i++) {
        mru_pages[i] = ui_overview_recent_at(i);
    }

    CHECK(ui_nav_bench_start(1));
    advance_ms(50);
    for (int i = 0; i < 3000; i++) {
        advance_ms(10);
        if (!ui_nav_bench_active()) {
            break;
        }
    }
    CHECK(!ui_nav_bench_active());
    CHECK(ui_current_page() == UI_PAGE_HOME);
    char calc_after[96];
    calculator_summary(calc_after, sizeof(calc_after));
    CHECK(strcmp(calc_before, calc_after) == 0);
    CHECK(ui_page_settings_current_section() == section_before);
    CHECK(ui_overview_recent_count() == mru_before);
    for (int i = 0; i < mru_before && i < 4; i++) {
        CHECK(ui_overview_recent_at(i) == mru_pages[i]);
    }
    ui_nav_stats_t stats;
    ui_nav_get_stats(&stats);
    CHECK(stats.bench_state_restored >= 1);
}

static void test_bootlock_demo(void)
{
    CHECK(ui_current_page() == UI_PAGE_LOCK);
    CHECK(ui_nav_demo_start(1));
    advance_ms(50);
    for (int i = 0; i < 2500; i++) {
        advance_ms(10);
        if (!ui_nav_demo_active()) {
            break;
        }
    }
    CHECK(!ui_nav_demo_active());
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(page_count() == 1);
    CHECK(ui_system_bar_is_visible());
    ui_nav_stats_t stats;
    ui_nav_get_stats(&stats);
    CHECK(stats.verified_unlock == 1);
    CHECK(stats.verified_app_enter >= 4);
    CHECK(stats.verified_app_back >= 4);
    CHECK(stats.verified_control_open >= 1);
    CHECK(stats.verified_control_close >= 1);
    CHECK(stats.verified_overview_open >= 1);
    CHECK(stats.verified_overview_close >= 1);
    CHECK(stats.verified_home_next >= 1);
    CHECK(stats.verified_home_prev >= 1);
    CHECK(stats.verify_failures == 0);
    CHECK(ui_overview_recent_count() == 0);
    CHECK(ui_page_settings_current_section() == 0);
    char calc[96];
    calculator_summary(calc, sizeof(calc));
    CHECK(strcmp(calc, "未打开") == 0);
}

static void test_bootlock_bench(unsigned cycles)
{
    CHECK(ui_current_page() == UI_PAGE_LOCK);
    CHECK(ui_nav_bench_start(cycles));
    advance_ms(50);
    for (int i = 0; i < 3500; i++) {
        advance_ms(10);
        if (!ui_nav_bench_active()) {
            break;
        }
    }
    CHECK(!ui_nav_bench_active());
    CHECK(ui_current_page() == UI_PAGE_HOME);
    CHECK(page_count() == 1);
    CHECK(ui_system_bar_is_visible());
    ui_nav_stats_t stats;
    ui_nav_get_stats(&stats);
    CHECK(stats.verified_unlock == 1);
    CHECK(stats.verified_app_enter >= 4 * cycles);
    CHECK(stats.verified_app_back >= 4 * cycles);
    CHECK(stats.verified_control_open >= cycles);
    CHECK(stats.verified_control_close >= cycles);
    CHECK(stats.verified_overview_open >= cycles);
    CHECK(stats.verified_overview_close >= cycles);
    CHECK(stats.verified_home_next >= cycles);
    CHECK(stats.verified_home_prev >= cycles);
    CHECK(stats.verify_failures == 0);
    CHECK(stats.bench_state_restored >= 1);
    CHECK(ui_overview_recent_count() == 0);

    ui_perf_snapshot_t perf;
    ui_perf_get_snapshot(&perf);
    CHECK(perf.actions >= 1 + 14 * cycles);
    CHECK(perf.frames > 0);
    CHECK(perf.frame_samples > 0);
    CHECK(perf.render_samples > 0);
    CHECK(perf.flush_cycles > 0);
    CHECK(perf.wait_cycles > 0);
    CHECK(perf.flush_calls >= perf.flush_cycles);
    CHECK(perf.wait_calls >= perf.wait_cycles);
    CHECK(perf.action_overflow == 0);
    CHECK(perf.frame_overflow == 0);
    CHECK(perf.render_overflow == 0);
    CHECK(perf.flush_overflow == 0);
    CHECK(perf.wait_overflow == 0);
    printf("BENCH cycles=%u actions=%u frames=%u frame_samples=%u render_samples=%u "
           "flush_cycles=%u wait_cycles=%u flush_calls=%u wait_calls=%u "
           "final_missing=%u request_first_p50=%u request_complete_p50=%u "
           "frame_p50=%u flush_cycle_p50=%u wait_cycle_p50=%u\n",
           cycles, (unsigned)perf.actions, (unsigned)perf.frames,
           (unsigned)perf.frame_samples, (unsigned)perf.render_samples,
           (unsigned)perf.flush_cycles, (unsigned)perf.wait_cycles,
           (unsigned)perf.flush_calls, (unsigned)perf.wait_calls,
           (unsigned)perf.final_missing, (unsigned)perf.request_first_p50_us,
           (unsigned)perf.request_complete_p50_us, (unsigned)perf.frame_p50_us,
           (unsigned)perf.flush_cycle_p50_us, (unsigned)perf.wait_cycle_p50_us);
}

static void test_removed_apps_absent(void)
{
    ui_navigate(UI_PAGE_HOME);
    CHECK(wait_idle(600));
    lv_obj_t *root = active_page();
    CHECK(root != NULL);
    CHECK(find_label(root, "相机") == NULL);
    CHECK(find_label(root, "相册") == NULL);
    CHECK(find_label(root, "家居") == NULL);
    CHECK(find_label(root, "文件") != NULL ||
          find_label(root, "音乐") != NULL ||
          find_label(root, "天气") != NULL);
}

static void fill_weather(panel_weather_snapshot_t *weather, const char *city, int temp,
                         uint32_t generation)
{
    memset(weather, 0, sizeof(*weather));
    weather->has_data = true;
    weather->calibrated = true;
    weather->wifi_connected = true;
    weather->updated_epoch = 1792837200;
    weather->data_epoch = 1792836900;
    weather->current_temp_c = temp;
    weather->apparent_temp_c = temp + 1;
    weather->humidity_pct = 62;
    weather->wind_speed_kmh = 12.3;
    weather->weather_code = 0;
    weather->kind = PANEL_WEATHER_CLEAR;
    weather->hour_count = 6;
    weather->day_count = 5;
    weather->generation = generation;
    snprintf(weather->city, sizeof(weather->city), "%s", city);
    snprintf(weather->region, sizeof(weather->region), "广东");
    snprintf(weather->country, sizeof(weather->country), "中国");
    weather->latitude = 23.11667;
    weather->longitude = 113.25;
    for (int i = 0; i < 6; i++) {
        weather->hours[i].hour = 18 + i;
        weather->hours[i].day_offset = 0;
        weather->hours[i].temp_c = temp + i;
        weather->hours[i].kind = PANEL_WEATHER_CLEAR;
    }
    for (int i = 0; i < 5; i++) {
        weather->days[i].day_offset = i;
        weather->days[i].weekday = (6 + i) % 7;
        weather->days[i].temp_max_c = temp + 3 + i;
        weather->days[i].temp_min_c = temp - 3 + i;
        weather->days[i].kind = PANEL_WEATHER_CLEAR;
    }
}

static void fill_city(panel_city_result_t *city, const char *name, const char *region)
{
    memset(city, 0, sizeof(*city));
    snprintf(city->name, sizeof(city->name), "%s", name);
    snprintf(city->region, sizeof(city->region), "%s", region);
    snprintf(city->country, sizeof(city->country), "China");
    city->latitude = 23.0;
    city->longitude = 113.0;
}

static void test_lock_calibration_label(void)
{
    panel_weather_snapshot_t weather;
    fill_weather(&weather, "广州", 26, 10);
    panel_services_test_set_time_calibrated(true);
    panel_services_test_set_weather(&weather);
    ui_rebuild_current_async();
    advance_ms(1100);
    CHECK(ui_current_page() == UI_PAGE_LOCK);
    lv_obj_t *root = active_page();
    CHECK(find_label_contains(root, "已校时") != NULL);
    CHECK(find_label_contains(root, "演示天气") == NULL);
    panel_services_test_set_time_calibrated(false);
}

static void test_lock_reminder_wake(void)
{
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    int old_sleep = settings.sleep_after_s;
    settings.sleep_after_s = 30;
    panel_services_update_settings(&settings);
    advance_ms(31500);
    CHECK(ui_test_is_sleeping());

    panel_clock_snapshot_t clock;
    memset(&clock, 0, sizeof(clock));
    clock.valid = true;
    clock.calibrated = true;
    clock.year = 2026;
    clock.month = 10;
    clock.day = 24;
    clock.hour = 7;
    clock.minute = 30;
    clock.weekday = 5;
    clock.alarm_enabled = true;
    clock.alarm_hour = 7;
    clock.alarm_minute = 30;
    clock.alarm_pending = true;
    clock.alarm_generation = 42;
    panel_services_test_set_clock(&clock);

    advance_ms(450);
    CHECK(!ui_test_is_sleeping());
    CHECK(ui_current_page() == UI_PAGE_LOCK);
    CHECK(find_label(lv_layer_top(), "屏幕提醒") != NULL);
    ui_back();
    CHECK(wait_idle(400));
    CHECK(find_label(lv_layer_top(), "屏幕提醒") == NULL);

    settings.sleep_after_s = old_sleep;
    panel_services_update_settings(&settings);
}

static void test_weather_live_ui(void)
{
    panel_weather_snapshot_t weather;
    fill_weather(&weather, "广州", 26, 20);
    panel_services_test_set_time_ok(true);
    panel_services_test_set_time_calibrated(true);
    panel_services_test_set_wifi_connected(true);
    panel_services_test_set_weather(&weather);

    ui_navigate(UI_PAGE_WEATHER);
    CHECK(wait_page(UI_PAGE_WEATHER, 1200));
    lv_obj_t *root = active_page();
    CHECK(find_label(root, "广州") != NULL);
    CHECK(find_label(root, "26°") != NULL);
    CHECK(find_label(root, "62%") != NULL);
    CHECK(find_label(root, "12.3 km/h") != NULL);
    CHECK(find_label(root, "已校时") != NULL);
    CHECK(find_label(root, "演示天气") == NULL);
    CHECK(find_label(root, "21°") == NULL);

    lv_obj_t *city_button_label = find_label(root, "切换城市");
    CHECK(city_button_label != NULL);
    lv_obj_t *city_button = city_button_label != NULL ? lv_obj_get_parent(city_button_label)
                                                       : NULL;
    CHECK(city_button != NULL);
    if (city_button != NULL) {
        lv_obj_update_layout(root);
        lv_area_t button_area;
        lv_obj_get_coords(city_button, &button_area);
        CHECK(button_area.y2 < 150); /* hourly card starts at y=150 */
        CHECK(button_area.x1 >= 900);
        lv_point_t center = {
            .x = (button_area.x1 + button_area.x2) / 2,
            .y = (button_area.y1 + button_area.y2) / 2,
        };
        CHECK(lv_obj_hit_test(city_button, &center));
        lv_obj_send_event(city_button, LV_EVENT_CLICKED, NULL);
    }
    CHECK(ui_weather_panel_test_modal_open());
    lv_obj_t *keyboard = find_obj_type(lv_layer_top(), &lv_keyboard_class);
    CHECK(keyboard != NULL);
    if (keyboard != NULL) {
        lv_obj_update_layout(lv_layer_top());
        lv_area_t kb_area;
        lv_obj_get_coords(keyboard, &kb_area);
        CHECK(kb_area.y1 >= 400);
        CHECK(kb_area.y2 < 728);
        CHECK(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    }
    CHECK(find_label(lv_layer_top(), "选择城市") != NULL);
    ui_back();
    CHECK(!ui_weather_panel_test_modal_open());
    CHECK(ui_current_page() == UI_PAGE_WEATHER);

    lv_obj_t *refresh_label = find_label(root, "刷新");
    CHECK(refresh_label != NULL);
    lv_obj_t *refresh_button = refresh_label != NULL ? lv_obj_get_parent(refresh_label) : NULL;
    CHECK(refresh_button != NULL);
    if (refresh_button != NULL && city_button != NULL) {
        lv_obj_update_layout(root);
        lv_area_t refresh_area;
        lv_area_t city_area;
        lv_obj_get_coords(refresh_button, &refresh_area);
        lv_obj_get_coords(city_button, &city_area);
        CHECK(refresh_area.y2 < 150);
        CHECK(refresh_area.x1 >= 840);
        CHECK(refresh_area.x2 < city_area.x1); /* 140 px button, 20 px gap */
        lv_point_t refresh_center = {
            .x = (refresh_area.x1 + refresh_area.x2) / 2,
            .y = (refresh_area.y1 + refresh_area.y2) / 2,
        };
        CHECK(lv_obj_hit_test(refresh_button, &refresh_center));

        panel_services_test_reset_refresh();
        panel_services_test_set_refresh_result(ESP_OK);
        lv_obj_send_event(refresh_button, LV_EVENT_CLICKED, NULL);
        CHECK(panel_services_test_refresh_calls() == 1);
        CHECK(find_label(root, "已请求刷新") != NULL);
        CHECK(find_label(root, "26°") != NULL); /* no fabricated immediate data */

        panel_services_test_reset_refresh();
        panel_services_test_set_refresh_result(ESP_ERR_NOT_FINISHED);
        lv_obj_send_event(refresh_button, LV_EVENT_CLICKED, NULL);
        CHECK(panel_services_test_refresh_calls() == 1);
        CHECK(find_label(root, "稍后再试") != NULL);

        panel_services_test_reset_refresh();
        panel_services_test_set_refresh_result(ESP_ERR_TIMEOUT);
        lv_obj_send_event(refresh_button, LV_EVENT_CLICKED, NULL);
        CHECK(panel_services_test_refresh_calls() == 1);
        CHECK(find_label(root, "命令队列忙") != NULL);

        panel_weather_snapshot_t offline = weather;
        offline.wifi_connected = false;
        offline.calibrated = true;
        panel_services_test_set_weather(&offline);
        panel_services_test_reset_refresh();
        panel_services_test_set_refresh_result(ESP_ERR_INVALID_STATE);
        lv_obj_send_event(refresh_button, LV_EVENT_CLICKED, NULL);
        CHECK(panel_services_test_refresh_calls() == 1);
        CHECK(find_label(root, "等待网络") != NULL);

        offline.wifi_connected = true;
        offline.calibrated = false;
        panel_services_test_set_weather(&offline);
        panel_services_test_reset_refresh();
        panel_services_test_set_refresh_result(ESP_ERR_INVALID_STATE);
        lv_obj_send_event(refresh_button, LV_EVENT_CLICKED, NULL);
        CHECK(panel_services_test_refresh_calls() == 1);
        CHECK(find_label(root, "等待校时") != NULL);

        panel_services_test_set_weather(&weather);
        panel_services_test_reset_refresh();
        advance_ms(4100);
        CHECK(find_label(root, "等待校时") == NULL);
    }

    ui_weather_panel_test_open();
    ui_home();
    CHECK(wait_page(UI_PAGE_HOME, 1200));
    CHECK(!ui_weather_panel_test_modal_open());
    advance_ms(1100);
    root = active_page();
    CHECK(find_label(root, "26°") != NULL);
    CHECK(find_label_contains(root, "已校时") != NULL);
    CHECK(find_label(root, "演示天气") == NULL);

    ui_navigate(UI_PAGE_WEATHER);
    CHECK(wait_page(UI_PAGE_WEATHER, 1200));
    ui_open_overview();
    CHECK(wait_overview_open(1200));
    CHECK(find_label_contains(lv_layer_top(), "广州 26°") != NULL);
    ui_close_overview();
    CHECK(wait_overview_closed(1200));
    CHECK(ui_current_page() == UI_PAGE_WEATHER);

    panel_weather_snapshot_t next;
    fill_weather(&next, "Shenzhen", 30, weather.generation + 1);
    panel_services_test_set_weather(&next);
    advance_ms(1100);
    root = active_page();
    CHECK(find_label(root, "Shenzhen") != NULL);
    CHECK(find_label(root, "30°") != NULL);

    next.stale = true;
    next.wifi_connected = false;
    snprintf(next.reason, sizeof(next.reason), "离线，显示缓存");
    next.generation++;
    panel_services_test_set_weather(&next);
    advance_ms(1100);
    CHECK(find_label(root, "离线") != NULL);

    panel_services_test_set_time_calibrated(false);
    next.calibrated = false;
    next.wifi_connected = true;
    next.stale = false;
    snprintf(next.reason, sizeof(next.reason), "等待校时");
    next.generation++;
    panel_services_test_set_weather(&next);
    advance_ms(1100);
    CHECK(find_label(root, "未校时") != NULL);

    panel_services_test_set_time_calibrated(true);
    next.calibrated = true;
    next.generation++;
    panel_services_test_set_weather(&next);
    advance_ms(1100);

    ui_weather_panel_test_open();
    ui_open_control();
    CHECK(wait_control_open(1200));
    CHECK(!ui_weather_panel_test_modal_open());
    ui_close_control();
    CHECK(wait_control_closed(1200));

    ui_weather_panel_test_open();
    ui_navigate(UI_PAGE_HOME);
    CHECK(wait_page(UI_PAGE_HOME, 1200));
    CHECK(!ui_weather_panel_test_modal_open());

    ui_navigate(UI_PAGE_WEATHER);
    CHECK(wait_page(UI_PAGE_WEATHER, 1200));
    root = active_page();
    CHECK(find_label(root, "Shenzhen") != NULL);

    panel_services_test_reset_search();
    panel_city_search_snapshot_t search;
    memset(&search, 0, sizeof(search));
    search.count = 1;
    search.generation = 7;
    fill_city(&search.results[0], "Shenzhen", "Guangdong");
    panel_services_test_set_city_search(&search);
    ui_weather_panel_test_open();
    ui_weather_panel_test_set_query("Shenzhen");
    CHECK(ui_weather_panel_test_search());
    CHECK(strcmp(panel_services_test_last_search(), "Shenzhen") == 0);
    advance_ms(1100);
    CHECK(find_label(lv_layer_top(), "Shenzhen · Guangdong · China") != NULL);

    memset(&search, 0, sizeof(search));
    search.count = 1;
    search.generation = 8;
    fill_city(&search.results[0], "Beijing", "Beijing");
    panel_services_test_set_city_search(&search);
    advance_ms(1100);
    CHECK(find_label(lv_layer_top(), "Shenzhen · Guangdong · China") == NULL);
    CHECK(find_label(lv_layer_top(), "Beijing · Beijing · China") != NULL);
    CHECK(click_text(lv_layer_top(), "Beijing · Beijing · China"));
    CHECK(!ui_weather_panel_test_modal_open());
    CHECK(panel_services_test_last_city_index() == 0);
    CHECK(panel_services_test_last_city_generation() == 8);
    ui_home();
    CHECK(wait_page(UI_PAGE_HOME, 1200));
}

static void test_calendar_clock_and_reminders(void)
{
    ui_home();
    CHECK(wait_page(UI_PAGE_HOME, 1200));
    for (int i = 0; i < UI_HOME_PAGE_COUNT && ui_page_home_current_page() != 0; i++) {
        ui_page_home_swipe(1);
        CHECK(wait_idle(600));
    }
    CHECK(ui_page_home_current_page() == 0);
    CHECK(click_text(active_page(), "日历"));
    CHECK(wait_page(UI_PAGE_CALENDAR, 1200));
    lv_obj_t *cal = active_page();
    CHECK(find_label(cal, "2026年10月") != NULL);
    char selected_before[40];
    char selected_after[40];
    ui_page_calendar_state_summary(selected_before, sizeof(selected_before));
    ui_page_calendar_test_set_month(1970, 1);
    ui_page_calendar_test_click_outside();
    ui_page_calendar_state_summary(selected_after, sizeof(selected_after));
    CHECK(strcmp(selected_before, selected_after) == 0);
    ui_page_calendar_test_set_month(2099, 12);
    ui_page_calendar_test_click_outside();
    ui_page_calendar_state_summary(selected_after, sizeof(selected_after));
    CHECK(strcmp(selected_before, selected_after) == 0);
    ui_page_calendar_test_set_month(2026, 10);
    lv_obj_update_layout(cal);
    lv_obj_t *cal_title = find_label(cal, "日历");
    lv_obj_t *cal_month = find_label(cal, "2026年10月");
    lv_obj_t *cal_week = find_label(cal, "周一");
    lv_obj_t *cal_back_icon = find_label(cal, LV_SYMBOL_LEFT);
    lv_obj_t *cal_back = cal_back_icon != NULL ? lv_obj_get_parent(cal_back_icon) : NULL;
    lv_obj_t *cal_first = find_label(cal, "28");
    lv_obj_t *cal_footer = find_label_contains(cal, "选中");
    CHECK(cal_title != NULL && cal_month != NULL && cal_week != NULL && cal_first != NULL);
    CHECK(cal_back != NULL && cal_footer != NULL);
    if (cal_title != NULL && cal_month != NULL && cal_week != NULL && cal_first != NULL &&
        cal_back != NULL && cal_footer != NULL) {
        lv_area_t title_area, month_area, week_area, first_area, footer_area, back_area;
        lv_obj_get_coords(cal_title, &title_area);
        lv_obj_get_coords(cal_month, &month_area);
        lv_obj_get_coords(cal_week, &week_area);
        lv_obj_get_coords(cal_first, &first_area);
        lv_obj_get_coords(cal_footer, &footer_area);
        lv_obj_get_coords(cal_back, &back_area);
        CHECK(month_area.y1 >= title_area.y2);
        CHECK(!rects_overlap(&month_area, &title_area));
        CHECK(!rects_overlap(&month_area, &back_area));
        CHECK(week_area.y1 >= month_area.y2);
        CHECK(first_area.y1 >= week_area.y2);
        CHECK(footer_area.y2 < 736);
        lv_point_t back_center = {
            .x = (back_area.x1 + back_area.x2) / 2,
            .y = (back_area.y1 + back_area.y2) / 2,
        };
        CHECK(lv_obj_hit_test(cal_back, &back_center));
    }
    CHECK(click_text(cal, "下个月"));
    CHECK(find_label(cal, "2026年11月") != NULL);
    CHECK(click_text(cal, "今天"));
    CHECK(find_label(cal, "2026年10月") != NULL);
    CHECK(click_text(cal, "15"));
    CHECK(find_label_contains(cal, "2026年10月15日") != NULL);
    panel_services_test_set_time_calibrated(true);
    panel_services_test_set_local_date(2026, 10, 25);
    advance_ms(1100);
    CHECK(find_label_contains(cal, "今天 2026年10月25日") != NULL);
    CHECK(find_label_contains(cal, "选中 2026年10月15日") != NULL);
    panel_services_test_set_time_calibrated(false);
    advance_ms(1100);
    CHECK(find_label_contains(cal, "等待校时") != NULL);
    CHECK(find_label_contains(cal, "选中 2026年10月15日") != NULL);
    panel_services_test_set_time_calibrated(true);
    panel_services_test_set_local_date(2026, 10, 24);
    advance_ms(1100);
    ui_home();
    CHECK(wait_page(UI_PAGE_HOME, 1200));
    for (int i = 0; i < UI_HOME_PAGE_COUNT && ui_page_home_current_page() != 0; i++) {
        ui_page_home_swipe(1);
        CHECK(wait_idle(600));
    }
    CHECK(click_text(active_page(), "日历"));
    CHECK(wait_page(UI_PAGE_CALENDAR, 1200));
    cal = active_page();
    CHECK(find_label_contains(cal, "2026年10月15日") != NULL);
    lv_obj_t *cal_back_icon2 = find_label(cal, LV_SYMBOL_LEFT);
    lv_obj_t *cal_back2 = cal_back_icon2 != NULL ? lv_obj_get_parent(cal_back_icon2) : NULL;
    CHECK(cal_back2 != NULL);
    if (cal_back2 != NULL) {
        lv_obj_update_layout(cal);
        lv_area_t back_area;
        lv_obj_get_coords(cal_back2, &back_area);
        lv_point_t center = {
            .x = (back_area.x1 + back_area.x2) / 2,
            .y = (back_area.y1 + back_area.y2) / 2,
        };
        CHECK(lv_obj_hit_test(cal_back2, &center));
        lv_obj_send_event(cal_back2, LV_EVENT_CLICKED, NULL);
    }
    CHECK(wait_page(UI_PAGE_HOME, 1200));

    panel_clock_snapshot_t clock;
    memset(&clock, 0, sizeof(clock));
    clock.valid = true;
    clock.calibrated = true;
    clock.year = 2026;
    clock.month = 10;
    clock.day = 24;
    clock.hour = 10;
    clock.minute = 24;
    clock.second = 5;
    clock.weekday = 5;
    clock.alarm_enabled = true;
    clock.alarm_hour = 7;
    clock.alarm_minute = 30;
    clock.stopwatch_ms = 1500;
    clock.countdown_remaining_ms = 300000;
    panel_services_test_set_clock(&clock);
    ui_home();
    CHECK(wait_page(UI_PAGE_HOME, 1200));
    for (int i = 0; i < UI_HOME_PAGE_COUNT && ui_page_home_current_page() != 0; i++) {
        ui_page_home_swipe(1);
        CHECK(wait_idle(600));
    }
    CHECK(click_text(active_page(), "时钟"));
    CHECK(wait_page(UI_PAGE_CLOCK, 1200));
    lv_obj_t *clock_page = active_page();
    CHECK(find_label(clock_page, "10:24:05") != NULL);
    CHECK(find_label_contains(clock_page, "2026年10月24日") != NULL);
    lv_obj_update_layout(clock_page);
    lv_obj_t *clock_title = find_label(clock_page, "时钟");
    lv_obj_t *clock_tab_alarm = find_label(clock_page, "闹钟");
    lv_obj_t *clock_time = find_label(clock_page, "10:24:05");
    lv_obj_t *clock_back_icon = find_label(clock_page, LV_SYMBOL_LEFT);
    lv_obj_t *clock_back = clock_back_icon != NULL ? lv_obj_get_parent(clock_back_icon) : NULL;
    CHECK(clock_title != NULL && clock_tab_alarm != NULL && clock_time != NULL);
    CHECK(clock_back != NULL);
    if (clock_title != NULL && clock_tab_alarm != NULL && clock_time != NULL &&
        clock_back != NULL) {
        lv_area_t title_area, tab_area, time_area, back_area;
        lv_obj_get_coords(clock_title, &title_area);
        lv_obj_get_coords(clock_tab_alarm, &tab_area);
        lv_obj_get_coords(clock_time, &time_area);
        lv_obj_get_coords(clock_back, &back_area);
        CHECK(tab_area.y1 >= title_area.y2);
        CHECK(!rects_overlap(&tab_area, &title_area));
        CHECK(tab_area.y2 < 192);
        CHECK(time_area.y1 >= 192);
        CHECK(time_area.y2 < 736);
        lv_point_t center = {
            .x = (back_area.x1 + back_area.x2) / 2,
            .y = (back_area.y1 + back_area.y2) / 2,
        };
        CHECK(lv_obj_hit_test(clock_back, &center));
    }

    CHECK(click_visible_text(clock_page, "秒表"));
    advance_ms(150);
    CHECK(find_label(clock_page, "00:01.5") != NULL);
    panel_services_test_reset_clock();
    CHECK(click_visible_text(clock_page, "开始"));
    CHECK(panel_services_test_clock_calls() == 1);

    CHECK(click_visible_text(clock_page, "倒计时"));
    advance_ms(150);
    CHECK(find_label(clock_page, "00:05:00") != NULL);
    panel_services_test_reset_clock();
    CHECK(click_visible_text(clock_page, "开始"));
    CHECK(panel_services_test_clock_calls() == 1);

    CHECK(click_visible_text(clock_page, "闹钟"));
    advance_ms(150);
    CHECK(find_label(clock_page, "每日闹钟") != NULL);
    panel_services_test_reset_clock();
    CHECK(click_visible_text(clock_page, "保存"));
    CHECK(panel_services_test_clock_calls() == 1);
    CHECK(find_label(clock_page, "设置已提交") != NULL);

    ui_home();
    CHECK(wait_page(UI_PAGE_HOME, 1200));
    for (int i = 0; i < UI_HOME_PAGE_COUNT && ui_page_home_current_page() != 0; i++) {
        ui_page_home_swipe(1);
        CHECK(wait_idle(600));
    }
    CHECK(click_text(active_page(), "时钟"));
    CHECK(wait_page(UI_PAGE_CLOCK, 1200));
    CHECK(find_label(active_page(), "每日闹钟") != NULL); /* tab state preserved */

    memset(&clock, 0, sizeof(clock));
    clock.valid = true;
    clock.calibrated = true;
    clock.alarm_enabled = true;
    clock.alarm_hour = 7;
    clock.alarm_minute = 30;
    clock.alarm_pending = true;
    clock.alarm_generation = 50;
    panel_services_test_set_clock(&clock);
    advance_ms(400);
    CHECK(find_label(lv_layer_top(), "屏幕提醒") != NULL);
    CHECK(ui_current_page() == UI_PAGE_CLOCK);
    ui_back();
    CHECK(wait_idle(400));
    CHECK(find_label(lv_layer_top(), "屏幕提醒") == NULL);

    CHECK(ui_show_notice("占用测试", "测试", "提醒应等待此弹层关闭。"));
    clock.alarm_generation = 51;
    clock.alarm_pending = true;
    panel_services_test_set_clock(&clock);
    advance_ms(400);
    CHECK(find_label(lv_layer_top(), "屏幕提醒") == NULL);
    ui_back();
    CHECK(wait_idle(400));
    advance_ms(400);
    CHECK(find_label(lv_layer_top(), "屏幕提醒") != NULL);
    ui_back();
    CHECK(wait_idle(400));

    clock.alarm_generation = 52;
    clock.alarm_pending = true;
    clock.countdown_done = true;
    clock.countdown_done_generation = 5;
    panel_services_test_set_clock(&clock);
    advance_ms(400);
    CHECK(find_label(lv_layer_top(), "屏幕提醒") != NULL);
    CHECK(find_label_contains(lv_layer_top(), "倒计时") != NULL);
    ui_back();
    CHECK(wait_idle(400));

    clock.alarm_generation = 60;
    clock.alarm_pending = true;
    clock.countdown_done = false;
    panel_services_test_set_clock(&clock);
    CHECK(panel_services_clock_alarm_ack(59) == ESP_OK);
    panel_clock_snapshot_t check;
    panel_services_get_clock(&check);
    CHECK(check.alarm_pending);

    lv_obj_t *clock_back_icon2 = find_label(active_page(), LV_SYMBOL_LEFT);
    lv_obj_t *clock_back2 = clock_back_icon2 != NULL ? lv_obj_get_parent(clock_back_icon2) : NULL;
    CHECK(clock_back2 != NULL);
    if (clock_back2 != NULL) {
        lv_obj_send_event(clock_back2, LV_EVENT_CLICKED, NULL);
    }
    CHECK(wait_page(UI_PAGE_HOME, 1200));
}

static void test_home_music_entry(void)
{
    ui_home();
    CHECK(wait_page(UI_PAGE_HOME, 1200));
    for (int i = 0; i < UI_HOME_PAGE_COUNT && ui_page_home_current_page() != 0; i++) {
        ui_page_home_swipe(1);
        CHECK(wait_idle(600));
    }
    CHECK(click_text(active_page(), "音乐"));
    CHECK(wait_control_open(1200));
    CHECK(ui_nav_control_open());
    lv_obj_t *control = lv_obj_get_child(lv_layer_top(), 0);
    CHECK(control != NULL);
    CHECK(find_label(control, "After Hours") != NULL);
    lv_obj_t *next_label = find_label(control, "下一个");
    CHECK(next_label != NULL);
    lv_obj_t *next_button = lv_obj_get_parent(next_label);
    lv_area_t next_area;
    lv_obj_get_coords(next_button, &next_area);
    lv_point_t next_point = {
        .x = (next_area.x1 + next_area.x2) / 2,
        .y = (next_area.y1 + next_area.y2) / 2,
    };
    lv_obj_t *hit = lv_indev_search_obj(lv_layer_top(), &next_point);
    CHECK(hit == next_button || lv_obj_get_parent(hit) == next_button);
    lv_obj_send_event(next_button, LV_EVENT_CLICKED, NULL);
    advance_ms(300);
    CHECK(find_label(control, "Starboy") != NULL);
    CHECK(find_label(control, "After Hours") == NULL);
    back_key();
    CHECK(wait_page(UI_PAGE_HOME, 1200));
    CHECK(!ui_nav_control_open());
}

static void test_files_ui(void)
{
    panel_files_snapshot_t files;
    panel_assets_status_t assets;
    memset(&assets, 0, sizeof(assets));
    assets.state = PANEL_ASSETS_STATE_NO_CARD;
    snprintf(assets.detail, sizeof(assets.detail), "TF卡不可用，使用内置字体");
    panel_services_test_set_assets(&assets);
    memset(&files, 0, sizeof(files));
    files.state = PANEL_FILES_STATE_NO_CARD;
    snprintf(files.error, sizeof(files.error), "未检测到可用TF卡");
    files.generation = 1;
    panel_services_test_set_files(&files);
    ui_home();
    CHECK(wait_page(UI_PAGE_HOME, 1200));
    for (int i = 0; i < UI_HOME_PAGE_COUNT && ui_page_home_current_page() != 0; i++) {
        ui_page_home_swipe(1);
        CHECK(wait_idle(600));
    }
    CHECK(click_text(active_page(), "文件"));
    CHECK(wait_page(UI_PAGE_FILES, 1200));
    lv_obj_t *page = active_page();
    CHECK(find_label(page, "未检测到可用TF卡") != NULL);
    CHECK(find_label(page, "重试") != NULL);

    memset(&files, 0, sizeof(files));
    files.state = PANEL_FILES_STATE_READY;
    snprintf(files.path, sizeof(files.path), "/");
    files.count = 12;
    files.total_entries = 12;
    files.page_count = 1;
    files.generation = 2;
    snprintf(files.entries[0].name, sizeof(files.entries[0].name), "Docs");
    files.entries[0].is_dir = true;
    files.entries[0].valid_name = true;
    files.entries[0].valid_utf8 = true;
    snprintf(files.entries[1].name, sizeof(files.entries[1].name), "readme.txt");
    files.entries[1].valid_name = true;
    files.entries[1].valid_utf8 = true;
    files.entries[1].size = 42;
    for (unsigned i = 2; i < 12; i++) {
        snprintf(files.entries[i].name, sizeof(files.entries[i].name), "f%02u.txt", i);
        files.entries[i].valid_name = true;
        files.entries[i].valid_utf8 = true;
        files.entries[i].size = i;
    }
    panel_services_test_set_files(&files);
    advance_ms(300);
    CHECK(find_label_contains(page, "Docs") != NULL);
    CHECK(find_label_contains(page, "readme.txt") != NULL);
    CHECK(find_label_contains(page, "内置字体") != NULL);
    lv_obj_t *last_label = find_label_contains(page, "f11.txt");
    CHECK(last_label != NULL);
    lv_obj_t *last_row = lv_obj_get_parent(last_label);
    lv_obj_t *list = lv_obj_get_parent(last_row);
    lv_area_t row_area;
    lv_area_t list_area;
    lv_obj_get_coords(last_row, &row_area);
    lv_obj_get_coords(list, &list_area);
    CHECK(row_area.y1 >= list_area.y1);
    CHECK(row_area.y2 <= list_area.y2);
    lv_obj_send_event(last_row, LV_EVENT_CLICKED, NULL);

    /* Corrupt/missing asset pack: the page must keep working on the firmware
     * font and must not claim the external font was loaded. */
    assets.state = PANEL_ASSETS_STATE_CORRUPT;
    snprintf(assets.detail, sizeof(assets.detail), "字体SHA-256校验失败");
    panel_services_test_set_assets(&assets);
    advance_ms(500);
    CHECK(find_label_contains(page, "readme.txt") != NULL);
    CHECK(find_label_contains(page, "内置字体") != NULL);

    files.preview_open = true;
    snprintf(files.preview_name, sizeof(files.preview_name), "readme.txt");
    snprintf(files.preview_type, sizeof(files.preview_type), "UTF-8 文本");
    snprintf(files.preview, sizeof(files.preview), "hello\nworld\n");
    files.preview_len = strlen(files.preview);
    files.preview_size = 12;
    files.generation = 3;
    panel_services_test_set_files(&files);
    advance_ms(300);
    CHECK(find_label(lv_layer_top(), "文件预览") != NULL);
    CHECK(find_label_contains(lv_layer_top(), "hello") != NULL);
    ui_back();
    CHECK(wait_idle(400));

    /* Long preview must have an independent scroll viewport; the end marker
     * must be reachable by scrolling to the bottom. */
    files.preview_open = true;
    files.preview_error = false;
    files.preview_size = 4096;
    files.preview_mtime_valid = true;
    snprintf(files.preview_mtime_text, sizeof(files.preview_mtime_text),
             "2026-10-26 22:15");
    size_t pos = 0;
    for (int i = 0; i < 40; i++) {
        pos += (size_t)snprintf(files.preview + pos, sizeof(files.preview) - pos,
                                "line %02d: scrollable preview body\n", i);
    }
    snprintf(files.preview + pos, sizeof(files.preview) - pos, "END_MARKER\n");
    files.preview_len = strlen(files.preview);
    files.generation = 4;
    panel_services_test_set_files(&files);
    advance_ms(300);
    lv_obj_t *end_label = find_label_contains(lv_layer_top(), "END_MARKER");
    CHECK(end_label != NULL);
    lv_obj_t *viewport = lv_obj_get_parent(end_label);
    CHECK(lv_obj_get_scroll_bottom(viewport) > 0);
    int32_t bottom = lv_obj_get_scroll_bottom(viewport);
    lv_obj_scroll_to_y(viewport, bottom, LV_ANIM_OFF);
    lv_obj_update_layout(viewport);
    CHECK(lv_obj_get_scroll_y(viewport) == bottom);
    CHECK(lv_obj_get_scroll_bottom(viewport) == 0);
    lv_area_t body_area;
    lv_area_t viewport_area;
    lv_obj_get_coords(end_label, &body_area);
    lv_obj_get_coords(viewport, &viewport_area);
    CHECK(body_area.y2 <= viewport_area.y2 + 2);
    ui_back();
    CHECK(wait_idle(400));

    /* Non-text file details: type/size/time remain visible with the error. */
    files.preview_open = false;
    files.preview_error = true;
    snprintf(files.preview_error_text, sizeof(files.preview_error_text),
             "该文件类型不支持预览");
    snprintf(files.preview_type, sizeof(files.preview_type), "其他文件");
    files.preview_size = 1234;
    files.generation = 5;
    panel_services_test_set_files(&files);
    advance_ms(300);
    CHECK(find_label(lv_layer_top(), "文件预览") != NULL);
    CHECK(find_label_contains(lv_layer_top(), "其他文件") != NULL);
    CHECK(find_label_contains(lv_layer_top(), "2026-10-26 22:15") != NULL);
    CHECK(find_label_contains(lv_layer_top(), "1234 字节") != NULL);
    ui_back();
    CHECK(wait_idle(400));

    /* A failed text preview must surface the worker's error modal. */
    snprintf(files.preview_type, sizeof(files.preview_type), "非 UTF-8 文本");
    snprintf(files.preview_error_text, sizeof(files.preview_error_text),
             "文件不是有效的UTF-8文本");
    files.generation = 6;
    panel_services_test_set_files(&files);
    advance_ms(300);
    CHECK(find_label(lv_layer_top(), "文件预览") != NULL);
    CHECK(find_label_contains(lv_layer_top(), "UTF-8") != NULL);
    ui_back();
    CHECK(wait_idle(400));

    ui_navigate(UI_PAGE_HOME);
    CHECK(wait_page(UI_PAGE_HOME, 1200));
}

static void test_status_bar_live_network(void)
{
    panel_services_test_set_time_ok(true);
    panel_services_test_set_wifi_connected(true);
    panel_services_test_set_time_calibrated(true);

    lv_obj_t *light_parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(light_parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(light_parent, 0, 0);
    lv_obj_set_size(light_parent, UI_WIDTH, UI_HEIGHT);
    lv_obj_t *light = ui_status_bar(light_parent, false);
    CHECK(light != NULL);
    CHECK(find_label(light, "已连接") != NULL);
    CHECK(find_label(light, "10:24") != NULL);
    CHECK(find_label(light, "已校时") != NULL);

    panel_services_test_set_time_calibrated(false);
    advance_ms(1100);
    CHECK(find_label(light, "未校时") != NULL);
    panel_services_test_set_time_calibrated(true);
    advance_ms(1100);
    CHECK(find_label(light, "已校时") != NULL);

    panel_services_test_set_wifi_connected(false);
    advance_ms(1100);
    CHECK(find_label(light, "未联网") != NULL);
    CHECK(find_label(light, "已连接") == NULL);

    panel_services_test_set_wifi_connected(true);
    advance_ms(1100);
    CHECK(find_label(light, "已连接") != NULL);

    panel_services_test_set_wifi_connected(false);
    advance_ms(1100);
    CHECK(find_label(light, "未联网") != NULL);

    panel_services_test_set_wifi_connected(true);
    advance_ms(1100);
    CHECK(find_label(light, "已连接") != NULL);

    panel_services_test_set_time_ok(false);
    advance_ms(1100);
    CHECK(find_label(light, "已连接") != NULL);
    CHECK(find_label(light, "10:24") != NULL);
    panel_services_test_set_time_ok(true);

    lv_obj_t *dark_parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(dark_parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(dark_parent, 0, 0);
    lv_obj_set_size(dark_parent, UI_WIDTH, UI_HEIGHT);
    lv_obj_t *dark = ui_status_bar(dark_parent, true);
    CHECK(dark != NULL);
    CHECK(find_label(dark, "已连接") != NULL);

    panel_services_test_set_wifi_connected(false);
    advance_ms(1100);
    CHECK(find_label(dark, "未联网") != NULL);
    CHECK(find_label(light, "未联网") != NULL);

    lv_obj_delete(light);
    panel_services_test_set_wifi_connected(true);
    advance_ms(1100);
    CHECK(find_label(dark, "已连接") != NULL);

    lv_obj_t *rebuilt = ui_status_bar(dark_parent, false);
    CHECK(rebuilt != NULL);
    CHECK(find_label(rebuilt, "已连接") != NULL);
    lv_obj_delete(dark);
    advance_ms(1100);
    CHECK(find_label(rebuilt, "已连接") != NULL);

    lv_obj_delete(rebuilt);
    lv_obj_delete(dark_parent);
    lv_obj_delete(light_parent);
    advance_ms(1100);
    panel_services_test_set_wifi_connected(false);
    panel_services_test_set_time_ok(true);
}

static void test_network_modal_back_and_page_delete(void)
{
    ui_page_settings_restore_section(0);
    ui_navigate(UI_PAGE_SETTINGS);
    CHECK(wait_page(UI_PAGE_SETTINGS, 1000));

    ui_network_panel_test_open("TestAP", 3);
    CHECK(ui_network_panel_test_modal_open());
    ui_back();
    CHECK(wait_idle(400));
    CHECK(!ui_network_panel_test_modal_open());
    CHECK(ui_current_page() == UI_PAGE_SETTINGS);

    ui_network_panel_test_open("TestAP2", 0);
    CHECK(ui_network_panel_test_modal_open());
    ui_open_overview();
    CHECK(wait_overview_open(1000));
    CHECK(!ui_network_panel_test_modal_open());
    CHECK(ui_overview_is_open());
    ui_close_overview();
    CHECK(wait_overview_closed(1000));
    CHECK(!ui_overview_is_open());
    CHECK(wait_page(UI_PAGE_SETTINGS, 1000));

    ui_network_panel_test_open("TestAP3", 3);
    CHECK(ui_network_panel_test_modal_open());
    ui_open_control();
    CHECK(wait_control_open(1000));
    CHECK(!ui_network_panel_test_modal_open());
    CHECK(ui_nav_control_open());
    ui_close_control();
    CHECK(wait_control_closed(1200));
    CHECK(!ui_nav_control_open());
    ui_page_settings_restore_section(0);
    ui_navigate(UI_PAGE_SETTINGS);
    CHECK(wait_page(UI_PAGE_SETTINGS, 1000));

    for (int i = 0; i < 3; i++) {
        ui_network_panel_test_open("LoopAP", 3);
        CHECK(ui_network_panel_test_modal_open());
        ui_back();
        CHECK(wait_idle(400));
        CHECK(!ui_network_panel_test_modal_open());
        CHECK(ui_current_page() == UI_PAGE_SETTINGS);
    }

    ui_network_panel_test_open("SectionAP", 3);
    CHECK(ui_network_panel_test_modal_open());
    ui_page_settings_restore_section(1);
    CHECK(!ui_network_panel_test_modal_open());
    CHECK(ui_current_page() == UI_PAGE_SETTINGS);
    ui_page_settings_restore_section(0);
    CHECK(wait_page(UI_PAGE_SETTINGS, 1000));

    ui_network_panel_test_open("TestAP4", 0);
    CHECK(ui_network_panel_test_modal_open());
    ui_home();
    CHECK(wait_page(UI_PAGE_HOME, 1200));
    CHECK(!ui_network_panel_test_modal_open());
    CHECK(ui_current_page() == UI_PAGE_HOME);
}

int main(int argc, char **argv)
{
    bool dark_cold = argc > 1 && strcmp(argv[1], "dark-cold") == 0;
    boot_ui(dark_cold ? PANEL_THEME_DARK : PANEL_THEME_LIGHT);
    if (dark_cold) {
        test_dark_cold_boot();
    } else if (argc > 1 && strcmp(argv[1], "bootlock-demo") == 0) {
        test_bootlock_demo();
    } else if (argc > 1 && strcmp(argv[1], "bootlock-bench") == 0) {
        test_bootlock_bench(1);
    } else if (argc > 1 && strcmp(argv[1], "bootlock-bench3") == 0) {
        test_bootlock_bench(3);
    } else {
        test_lock_calibration_label();
        test_lock_reminder_wake();
        test_boot_lock_and_unlock();
        test_fixed_bar_and_three_keys();
        test_theme_live_cycle();
        test_forward_back_rapid_and_settings_back();
        test_control_source_and_calculator_state();
        test_all_page_layouts_and_zero_animation();
        test_overview_from_every_page();
        test_overview_mru_cards_and_empty();
        test_calculator_forced_rebuild_state();
        test_app_state_restore_and_dismiss_reset();
        test_modal_and_overview_priority();
        test_overview_home_and_card_selection();
        test_repeated_overview_and_animation_off();
        test_demo_from_home();
        test_removed_apps_absent();
        test_repeated_navigation_and_accounting();
        test_bench_state_restore();
        test_status_bar_live_network();
        test_weather_live_ui();
        test_calendar_clock_and_reminders();
        test_home_music_entry();
        test_files_ui();
        test_network_modal_back_and_page_delete();
    }

    if (failures != 0) {
        fprintf(stderr, "FAIL: runtime UI failures=%d\n", failures);
        return 1;
    }
    printf("PASS: runtime three-key/overview/system-bar tests passed\n");
    return 0;
}
