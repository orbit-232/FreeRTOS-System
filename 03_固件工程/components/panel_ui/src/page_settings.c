#include "../../../config/version.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_pages.h"
#include "ui_network_panel.h"
#include "ui_theme.h"
#include "ui_widgets.h"

typedef enum {
    SECTION_NETWORK = 0,
    SECTION_DISPLAY,
    SECTION_SOUND,
    SECTION_NOTIFICATIONS,
    SECTION_SYSTEM,
    SECTION_ABOUT,
    SECTION_COUNT,
} settings_section_t;

static settings_section_t s_section;
static lv_obj_t *s_screen;
static lv_obj_t *s_content;
static lv_obj_t *s_nav_buttons[SECTION_COUNT];
static lv_obj_t *s_nav_labels[SECTION_COUNT];
static lv_obj_t *s_nav_status[SECTION_COUNT];
static lv_obj_t *s_brightness_value;
static lv_obj_t *s_sleep_value;
static lv_obj_t *s_volume_value;
static int s_demo_volume = 45;
static settings_section_t s_pending_section;
static uint32_t s_fade_generation;
static bool s_fade_active;

static void select_section(settings_section_t section);
static void select_section_force(settings_section_t section);

static const char *s_section_titles[SECTION_COUNT] = {
    "网络", "显示", "声音", "通知", "系统", "关于",
};
static const char *s_section_symbols[SECTION_COUNT] = {
    LV_SYMBOL_WIFI, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_VOLUME_MAX,
    LV_SYMBOL_BELL, LV_SYMBOL_SETTINGS, LV_SYMBOL_LIST,
};

static void format_bytes(uint32_t bytes, char *buf, size_t len)
{
    if (bytes >= 1024U * 1024U) {
        snprintf(buf, len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, len, "%u KB", (unsigned)(bytes / 1024U));
    }
}

static void format_uptime(uint64_t seconds, char *buf, size_t len)
{
    uint64_t days = seconds / 86400ULL;
    uint64_t hours = (seconds % 86400ULL) / 3600ULL;
    uint64_t mins = (seconds % 3600ULL) / 60ULL;
    snprintf(buf, len, "%" PRIu64 "天 %02" PRIu64 ":%02" PRIu64, days, hours, mins);
}

static void nav_refresh(void)
{
    const ui_palette_t *p = ui_palette();
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    panel_runtime_t runtime;
    panel_services_get_runtime(&runtime);

    for (int i = 0; i < SECTION_COUNT; i++) {
        bool on = i == (int)s_section;
        if (s_nav_buttons[i] != NULL) {
            lv_obj_set_style_bg_color(s_nav_buttons[i], on ? p->accent_soft : p->bg, 0);
            lv_obj_set_style_bg_opa(s_nav_buttons[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
        if (s_nav_labels[i] != NULL) {
            lv_obj_set_style_text_color(s_nav_labels[i], on ? p->accent : p->text, 0);
        }
        if (s_nav_status[i] != NULL) {
            lv_obj_set_style_text_color(s_nav_status[i], on ? p->accent : p->muted, 0);
        }
    }

    if (s_nav_status[SECTION_NETWORK] != NULL) {
        lv_label_set_text(s_nav_status[SECTION_NETWORK], "未接入");
    }
    if (s_nav_status[SECTION_DISPLAY] != NULL) {
        lv_label_set_text_fmt(s_nav_status[SECTION_DISPLAY], "亮度 %u%%", settings.brightness);
    }
    if (s_nav_status[SECTION_SOUND] != NULL) {
        lv_label_set_text(s_nav_status[SECTION_SOUND], "演示");
    }
    if (s_nav_status[SECTION_NOTIFICATIONS] != NULL) {
        lv_label_set_text(s_nav_status[SECTION_NOTIFICATIONS],
                          settings.do_not_disturb ? "免打扰 开" : "免打扰 关");
    }
    if (s_nav_status[SECTION_SYSTEM] != NULL) {
        lv_label_set_text_fmt(s_nav_status[SECTION_SYSTEM], "任务 %u",
                              (unsigned)runtime.task_count);
    }
    if (s_nav_status[SECTION_ABOUT] != NULL) {
        lv_label_set_text(s_nav_status[SECTION_ABOUT], "v" PANEL_FW_VERSION);
    }
}

static lv_obj_t *section_card(int y, int h)
{
    return ui_card(s_content, 0, y, 840, h);
}

static void section_header(const char *title, const char *subtitle)
{
    const ui_palette_t *p = ui_palette();
    ui_label_at(s_content, 0, 0, title, &ui_font_32, p->text);
    lv_obj_t *sub = ui_label_at(s_content, 0, 44, subtitle, &ui_font_16, p->muted);
    lv_obj_set_width(sub, 820);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_WRAP);
}

static void separator(lv_obj_t *parent, int y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(line, 24, y);
    lv_obj_set_size(line, 792, 1);
    lv_obj_set_style_bg_color(line, ui_palette()->border, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
}

static lv_obj_t *row_button(lv_obj_t *group, int y, const char *name, const char *value,
                            lv_event_cb_t cb, void *user_data)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *row = lv_button_create(group);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, 840, 64);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, p->surface_alt, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    ui_label_at(row, 24, 19, name, &ui_font_20, p->text);
    lv_obj_t *val = ui_label(row, value, &ui_font_16, p->muted);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -28, 0);
    if (cb != NULL) {
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
        ui_label_at(row, 792, 16, "›", &ui_font_24, p->muted);
    }
    separator(group, y + 63);
    return val;
}

static void switch_row(lv_obj_t *group, int y, const char *name, bool checked,
                       lv_event_cb_t cb, void *user_data)
{
    ui_label_at(group, 24, y + 19, name, &ui_font_20, ui_palette()->text);
    lv_obj_t *sw = ui_switch_create(group, 756, y + 16, checked);
    if (cb != NULL) {
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user_data);
    }
    separator(group, y + 63);
}

static void theme_clicked_cb(lv_event_t *event)
{
    panel_theme_t theme = (panel_theme_t)(uintptr_t)lv_event_get_user_data(event);
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    settings.theme = theme;
    if (!panel_services_update_settings(&settings)) {
        ui_toast("设置保存失败");
        return;
    }
    ui_theme_set(theme);
    nav_refresh();
    ui_rebuild_current_async();
}

static void brightness_changed_cb(lv_event_t *event)
{
    int value = (int)lv_slider_get_value(lv_event_get_target_obj(event));
    if (s_brightness_value != NULL) {
        lv_label_set_text_fmt(s_brightness_value, "%d%%", value);
    }
    panel_services_set_brightness(value);
    nav_refresh();
}

static void brightness_released_cb(lv_event_t *event)
{
    (void)event;
    panel_services_schedule_save();
}

static void auto_brightness_cb(lv_event_t *event)
{
    (void)event;
    ui_show_notice("自动亮度不可用", "硬件说明",
                   "当前 10.1 寸套件没有环境光传感器，BSP 也未提供自动亮度接口。开关保持禁用，避免显示虚假的自动调节状态。");
}

static void sleep_clicked_cb(lv_event_t *event)
{
    (void)event;
    static const uint16_t options[] = {30, 60, 120, 300, 0};
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    int index = 0;
    for (int i = 0; i < 5; i++) {
        if (options[i] == settings.sleep_after_s) {
            index = i;
            break;
        }
    }
    index = (index + 1) % 5;
    settings.sleep_after_s = options[index];
    if (panel_services_update_settings(&settings) && s_sleep_value != NULL) {
        if (settings.sleep_after_s == 0) {
            lv_label_set_text(s_sleep_value, "关闭");
        } else {
            lv_label_set_text_fmt(s_sleep_value, "%u 秒", settings.sleep_after_s);
        }
    }
}

static void calibration_cb(lv_event_t *event)
{
    (void)event;
    ui_show_notice("触摸校准", "验收说明",
                   "BSP 当前通过固定 MIPI-DSI 90° 变换注册 GT9271 输入。四角与中心点、上滑/下滑/左右滑手势需要在实机上逐项验证；本固件不伪造校准结果。");
}

static void animations_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    settings.animations = (lv_obj_get_state(sw) & LV_STATE_CHECKED) != 0;
    panel_services_update_settings(&settings);
}

static void dnd_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    settings.do_not_disturb = (lv_obj_get_state(sw) & LV_STATE_CHECKED) != 0;
    panel_services_update_settings(&settings);
    nav_refresh();
}

static void preview_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    settings.notification_preview = (lv_obj_get_state(sw) & LV_STATE_CHECKED) != 0;
    panel_services_update_settings(&settings);
}

static void clear_notifications_cb(lv_event_t *event)
{
    (void)event;
    ui_toast("示例通知已清空（仅界面状态）");
}

static void refresh_system_cb(lv_event_t *event)
{
    (void)event;
    select_section_force(SECTION_SYSTEM);
}

static void sound_volume_cb(lv_event_t *event)
{
    s_demo_volume = (int)lv_slider_get_value(lv_event_get_target_obj(event));
    if (s_volume_value != NULL) {
        lv_label_set_text_fmt(s_volume_value, "%d%%", s_demo_volume);
    }
}

static void build_network(void)
{
    section_header("网络", "真实 C6 Hosted Wi-Fi/蓝牙状态；扫描、连接和重试均在后台服务执行。");
    ui_network_panel_create(s_content);
}

static void build_display(void)
{
    const ui_palette_t *p = ui_palette();
    section_header("显示", "亮度、主题、动画与休眠会立即作用到界面或背光。");
    panel_settings_t settings;
    panel_services_get_settings(&settings);

    lv_obj_t *bright = section_card(104, 112);
    ui_label_at(bright, 24, 18, "亮度 · 真实背光", &ui_font_20, p->text);
    s_brightness_value = ui_label_at(bright, 720, 14, "70%", &ui_font_24, p->text);
    lv_obj_set_width(s_brightness_value, 90);
    lv_obj_set_style_text_align(s_brightness_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *slider = ui_slider_create(bright, 24, 62, 792, PANEL_BRIGHTNESS_MIN,
                                        PANEL_BRIGHTNESS_MAX, settings.brightness);
    lv_obj_add_event_cb(slider, brightness_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, brightness_released_cb, LV_EVENT_RELEASED, NULL);
    lv_label_set_text_fmt(s_brightness_value, "%u%%", settings.brightness);

    lv_obj_t *auto_card = section_card(232, 64);
    ui_label_at(auto_card, 24, 18, "自动亮度", &ui_font_20, p->muted);
    ui_label_at(auto_card, 220, 20, "无环境光传感器，已禁用", &ui_font_16, p->muted);
    lv_obj_t *auto_sw = ui_switch_create(auto_card, 756, 16, false);
    lv_obj_add_state(auto_sw, LV_STATE_DISABLED);
    lv_obj_add_flag(auto_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(auto_card, auto_brightness_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *appearance = section_card(312, 88);
    ui_label_at(appearance, 24, 18, "外观", &ui_font_20, p->text);
    lv_obj_t *seg = lv_obj_create(appearance);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(seg, 540, 16);
    lv_obj_set_size(seg, 276, 48);
    lv_obj_set_style_bg_color(seg, p->surface_alt, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(seg, 12, 0);
    lv_obj_set_style_border_width(seg, 0, 0);
    lv_obj_set_style_pad_all(seg, 4, 0);
    const char *themes[] = {"浅色", "深色", "自动"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *button = lv_button_create(seg);
        lv_obj_set_pos(button, i * 89, 0);
        lv_obj_set_size(button, 86, 40);
        lv_obj_set_style_radius(button, 9, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        bool on = (int)settings.theme == i;
        lv_obj_set_style_bg_color(button, on ? p->surface : p->surface_alt, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_t *label = ui_label(button, themes[i], &ui_font_16,
                                   on ? (i == 1 ? p->accent : p->text) : p->muted);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, theme_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    lv_obj_t *group = section_card(416, 208);
    s_sleep_value = row_button(group, 10, "息屏时间",
                               settings.sleep_after_s == 0 ? "关闭" : "读取中", sleep_clicked_cb,
                               NULL);
    if (settings.sleep_after_s != 0) {
        lv_label_set_text_fmt(s_sleep_value, "%u 秒", settings.sleep_after_s);
    }
    row_button(group, 74, "触摸校准", "实机待验收", calibration_cb, NULL);
    ui_label_at(group, 24, 147, "动画效果", &ui_font_20, p->text);
    lv_obj_t *anim_sw = ui_switch_create(group, 756, 142, settings.animations);
    lv_obj_add_event_cb(anim_sw, animations_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (settings.theme == PANEL_THEME_AUTO) {
        ui_label_at(s_content, 0, 628, "自动主题：无光传感器，等同浅色。", &ui_font_16, p->muted);
    }
}

static void build_sound(void)
{
    const ui_palette_t *p = ui_palette();
    section_header("声音", "音频服务未接入，以下为明确标记的界面演示值。");
    lv_obj_t *group = section_card(110, 180);
    ui_label_at(group, 24, 18, "音量 · 演示", &ui_font_20, p->text);
    lv_obj_t *value = ui_label_at(group, 742, 14, "45%", &ui_font_24, p->text);
    s_volume_value = value;
    lv_obj_set_width(value, 70);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    ui_label_at(group, 24, 62, LV_SYMBOL_VOLUME_MID, &lv_font_montserrat_24, p->muted);
    lv_obj_t *slider = ui_slider_create(group, 68, 58, 730, 0, 100, s_demo_volume);
    lv_obj_add_event_cb(slider, sound_volume_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_label_set_text_fmt(value, "%d%%", s_demo_volume);
    ui_label_at(group, 24, 126, "音量滑块仅更新界面演示值，不会调用 I2S/ES8311。",
                &ui_font_16, p->muted);
    lv_obj_t *note = ui_card(s_content, 0, 316, 840, 180);
    ui_label_at(note, 24, 22, "未接入项", &ui_font_24, p->text);
    lv_obj_t *body = ui_label_at(note, 24, 64,
                                 "本工程不复制官方音频示例，也不初始化 ES8311 音频链路。请勿把本页数值当作真实扬声器音量或已连接设备状态。",
                                 &ui_font_20, p->muted);
    lv_obj_set_width(body, 790);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
}

static void build_notifications(void)
{
    const ui_palette_t *p = ui_palette();
    section_header("通知", "勿扰和预览为本地设置；系统没有通知服务。");
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    lv_obj_t *group = section_card(110, 224);
    switch_row(group, 10, "免打扰", settings.do_not_disturb, dnd_cb, NULL);
    switch_row(group, 74, "通知预览", settings.notification_preview, preview_cb, NULL);
    lv_obj_t *clear = ui_button(group, 24, 146, 180, 52, "清空示例通知", &ui_font_16,
                                clear_notifications_cb, NULL);
    lv_obj_set_style_bg_color(clear, p->surface_alt, 0);
    ui_label_at(group, 224, 160, "只清除界面提示，不涉及系统通知", &ui_font_16, p->muted);

    lv_obj_t *note = ui_card(s_content, 0, 356, 840, 180);
    ui_label_at(note, 24, 22, "示例数据", &ui_font_24, p->text);
    lv_obj_t *body = ui_label_at(note, 24, 64,
                                 "锁屏上的两条通知是视觉示例。勿扰开启后，锁屏会显示“勿扰已开启”；没有真实消息中心或推送通道。",
                                 &ui_font_20, p->muted);
    lv_obj_set_width(body, 790);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
}

static void build_system(void)
{
    const ui_palette_t *p = ui_palette();
    section_header("系统", "运行时间、堆和 PSRAM 为运行时实测值。");
    panel_runtime_t runtime;
    panel_services_get_runtime(&runtime);
    char uptime[32];
    char heap[24];
    char psram[24];
    format_uptime(runtime.uptime_s, uptime, sizeof(uptime));
    format_bytes(runtime.heap_free, heap, sizeof(heap));
    format_bytes(runtime.psram_free, psram, sizeof(psram));

    lv_obj_t *stats = section_card(110, 276);
    row_button(stats, 10, "运行时间", uptime, NULL, NULL);
    row_button(stats, 74, "内部 RAM 可用", heap, NULL, NULL);
    row_button(stats, 138, "PSRAM 可用", psram, NULL, NULL);
    char tasks[16];
    snprintf(tasks, sizeof(tasks), "%u 个", (unsigned)runtime.task_count);
    row_button(stats, 202, "FreeRTOS 任务数", tasks, NULL, NULL);

    lv_obj_t *refresh = ui_button(s_content, 0, 398, 180, 52, "刷新实测值", &ui_font_16,
                                  refresh_system_cb, NULL);
    lv_obj_set_style_bg_color(refresh, p->surface_alt, 0);

    lv_obj_t *group = section_card(466, 152);
    ui_label_at(group, 24, 18, "Flash", &ui_font_16, p->muted);
    lv_obj_t *flash = ui_label_at(group, 700, 12, "32 MB", &ui_font_20, p->text);
    lv_obj_set_width(flash, 112);
    lv_obj_set_style_text_align(flash, LV_TEXT_ALIGN_RIGHT, 0);
    separator(group, 51);
    ui_label_at(group, 24, 64, "ESP-IDF", &ui_font_16, p->muted);
    lv_obj_t *idf = ui_label_at(group, 700, 58, "v6.0.2", &ui_font_20, p->text);
    lv_obj_set_width(idf, 112);
    lv_obj_set_style_text_align(idf, LV_TEXT_ALIGN_RIGHT, 0);
    separator(group, 97);
    ui_label_at(group, 24, 110, "LVGL", &ui_font_16, p->muted);
    lv_obj_t *lvgl = ui_label_at(group, 700, 104, "9.5.0", &ui_font_20, p->text);
    lv_obj_set_width(lvgl, 112);
    lv_obj_set_style_text_align(lvgl, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_about(void)
{
    const ui_palette_t *p = ui_palette();
    section_header("关于", "WavesharePanel 原生 LVGL 界面");
    lv_obj_t *card = section_card(110, 480);
    ui_label_at(card, 24, 22, "WavesharePanel " PANEL_FW_VERSION, &ui_font_32, p->text);
    ui_label_at(card, 24, 74, "ESP32-P4-WIFI6-Touch-LCD-10.1", &ui_font_20, p->muted);

    const char *rows[][2] = {
        {"屏幕", "10.1 寸 800×1280 · 270° 横屏 1280×800"},
        {"触摸", "GT9271 十点电容 · 固定 BSP 变换"},
        {"显示驱动", "JD9365 · MIPI-DSI · RGB565"},
        {"BSP", "waveshare/esp32_p4_wifi6_touch_lcd_x 2.0.3"},
        {"图形", "LVGL 9.5.0 · esp_lvgl_adapter 0.6.4"},
        {"构建", "ESP-IDF 6.0.2 · GCC 15.2.0"},
        {"字体", "Noto Sans SC 子集 · SIL OFL 1.1"},
        {"说明", "视觉与触摸仍需实机验收"},
    };
    for (int i = 0; i < 8; i++) {
        ui_label_at(card, 24, 126 + i * 40, rows[i][0], &ui_font_16, p->muted);
        lv_obj_t *value = ui_label_at(card, 210, 122 + i * 40, rows[i][1], &ui_font_20,
                                      i == 7 ? p->warning : p->text);
        lv_obj_set_width(value, 600);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_CLIP);
    }
    ui_label_at(s_content, 0, 600,
                "本页是工程信息，不代表已完成实机视觉/触摸验收。", &ui_font_16, p->muted);
}

static void build_section(void)
{
    if (s_content == NULL) {
        return;
    }
    lv_obj_clean(s_content);
    s_brightness_value = NULL;
    s_sleep_value = NULL;
    s_volume_value = NULL;
    switch (s_section) {
    case SECTION_NETWORK:
        build_network();
        break;
    case SECTION_DISPLAY:
        build_display();
        break;
    case SECTION_SOUND:
        build_sound();
        break;
    case SECTION_NOTIFICATIONS:
        build_notifications();
        break;
    case SECTION_SYSTEM:
        build_system();
        break;
    case SECTION_ABOUT:
        build_about();
        break;
    default:
        break;
    }
    nav_refresh();
}

static void apply_section(settings_section_t section)
{
    s_section = section;
    build_section();
    if (s_content != NULL) {
        lv_obj_set_style_opa(s_content, LV_OPA_COVER, 0);
    }
}

static void section_opa_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void fade_in_done_cb(lv_anim_t *anim)
{
    if ((uint32_t)(uintptr_t)lv_anim_get_user_data(anim) != s_fade_generation) {
        return;
    }
    s_fade_active = false;
    if (s_content != NULL) {
        lv_obj_set_style_opa(s_content, LV_OPA_COVER, 0);
    }
    if (s_pending_section != s_section) {
        select_section(s_pending_section);
    }
}

static void begin_fade_in(void)
{
    if (s_content == NULL) {
        s_fade_active = false;
        return;
    }
    lv_obj_set_style_opa(s_content, LV_OPA_TRANSP, 0);
    s_fade_generation++;
    uint32_t generation = s_fade_generation;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_content);
    lv_anim_set_exec_cb(&anim, section_opa_cb);
    lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&anim, 70);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&anim, fade_in_done_cb);
    lv_anim_set_user_data(&anim, (void *)(uintptr_t)generation);
    lv_anim_start(&anim);
}

static void fade_out_done_cb(lv_anim_t *anim)
{
    if ((uint32_t)(uintptr_t)lv_anim_get_user_data(anim) != s_fade_generation ||
        s_content == NULL) {
        return;
    }
    s_section = s_pending_section;
    build_section();
    begin_fade_in();
}

static void begin_fade_out(void)
{
    if (s_content == NULL) {
        apply_section(s_pending_section);
        return;
    }
    s_fade_active = true;
    s_fade_generation++;
    uint32_t generation = s_fade_generation;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_content);
    lv_anim_set_exec_cb(&anim, section_opa_cb);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&anim, 70);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&anim, fade_out_done_cb);
    lv_anim_set_user_data(&anim, (void *)(uintptr_t)generation);
    lv_anim_start(&anim);
}

static void select_section(settings_section_t section)
{
    if (ui_network_panel_back()) {
        return;
    }
    if ((int)section < 0 || section >= SECTION_COUNT) {
        return;
    }
    if (section == s_section && !s_fade_active) {
        return;
    }
    s_pending_section = section;
    if (s_fade_active) {
        return;
    }

    panel_settings_t settings;
    panel_services_get_settings(&settings);
    if (!settings.animations || s_content == NULL) {
        apply_section(section);
        return;
    }
    begin_fade_out();
}

static void select_section_force(settings_section_t section)
{
    if (ui_network_panel_back()) {
        return;
    }
    if (s_content != NULL) {
        lv_anim_delete(s_content, NULL);
    }
    s_fade_active = false;
    s_fade_generation++;
    s_pending_section = section;
    apply_section(section);
}

static void nav_clicked_cb(lv_event_t *event)
{
    settings_section_t section = (settings_section_t)(uintptr_t)lv_event_get_user_data(event);
    select_section(section);
}

bool ui_page_settings_back(void)
{
    if (ui_network_panel_back()) {
        return true;
    }
    if (s_screen == NULL || s_section == SECTION_NETWORK) {
        return false;
    }
    select_section(SECTION_NETWORK);
    return true;
}

void ui_page_settings_reset_for_benchmark(void)
{
    if (s_screen == NULL) {
        s_section = SECTION_NETWORK;
        s_pending_section = SECTION_NETWORK;
        s_fade_active = false;
        return;
    }
    if (s_section != SECTION_NETWORK) {
        select_section_force(SECTION_NETWORK);
    }
}

void ui_page_settings_state_reset(void)
{
    ui_page_settings_reset_for_benchmark();
}

void ui_page_settings_state_summary(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    if ((int)s_section < 0 || s_section >= SECTION_COUNT) {
        snprintf(buf, len, "设置");
        return;
    }
    snprintf(buf, len, "%s", s_section_titles[s_section]);
}

int ui_page_settings_current_section(void)
{
    return (int)s_section;
}

void ui_page_settings_restore_section(int section)
{
    if (section < 0 || section >= SECTION_COUNT) {
        return;
    }
    if (s_screen == NULL) {
        s_section = (settings_section_t)section;
        s_pending_section = s_section;
        s_fade_active = false;
        return;
    }
    if (s_section != (settings_section_t)section) {
        select_section_force((settings_section_t)section);
    }
}

static void settings_delete_cb(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) == s_screen) {
        if (s_content != NULL) {
            lv_anim_delete(s_content, NULL);
        }
        s_fade_active = false;
        s_fade_generation++;
        s_screen = NULL;
        s_content = NULL;
        for (int i = 0; i < SECTION_COUNT; i++) {
            s_nav_buttons[i] = NULL;
            s_nav_labels[i] = NULL;
            s_nav_status[i] = NULL;
        }
        s_brightness_value = NULL;
        s_sleep_value = NULL;
        s_volume_value = NULL;
    }
}

#ifdef WAVESHARE_HOST_SIM
void ui_page_settings_host_set_section(int section)
{
    if (section >= 0 && section < SECTION_COUNT) {
        s_section = (settings_section_t)section;
    }
}
#endif

static void settings_back_cb(lv_event_t *event)
{
    (void)event;
    ui_back();
}

lv_obj_t *ui_page_settings_create(void)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *screen = ui_screen_create();
    s_screen = screen;
    s_fade_active = false;
    s_fade_generation++;
    s_pending_section = s_section;
    ui_status_bar(screen, false);

    lv_obj_t *side = lv_obj_create(screen);
    lv_obj_remove_flag(side, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(side, 0, UI_STATUS_HEIGHT);
    lv_obj_set_size(side, 340, UI_CONTENT_BOTTOM - UI_STATUS_HEIGHT);
    lv_obj_set_style_bg_color(side, p->surface, 0);
    lv_obj_set_style_bg_opa(side, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(side, p->border, 0);
    lv_obj_set_style_border_width(side, 1, 0);
    lv_obj_set_style_border_side(side, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_radius(side, 0, 0);
    lv_obj_set_style_pad_all(side, 0, 0);
    ui_label_at(side, 24, 12, "设置", &ui_font_32, p->text);

    lv_obj_t *back = lv_button_create(side);
    lv_obj_set_pos(back, 272, 10);
    lv_obj_set_size(back, 48, 48);
    lv_obj_set_style_radius(back, 24, 0);
    lv_obj_set_style_bg_color(back, p->surface_alt, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, settings_back_cb, LV_EVENT_CLICKED, NULL);
    ui_pressable(back);
    lv_obj_t *back_icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_24, p->text);
    lv_obj_center(back_icon);

    for (int i = 0; i < SECTION_COUNT; i++) {
        lv_obj_t *button = lv_button_create(side);
        lv_obj_set_pos(button, 20, 78 + i * 64);
        lv_obj_set_size(button, 300, 56);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_radius(button, 14, 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_add_event_cb(button, nav_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        ui_pressable(button);
        lv_obj_t *icon = ui_label_at(button, 16, 14, s_section_symbols[i],
                                     &lv_font_montserrat_24, p->muted);
        (void)icon;
        s_nav_buttons[i] = button;
        s_nav_labels[i] = ui_label_at(button, 54, 8, s_section_titles[i], &ui_font_20,
                                      p->text);
        s_nav_status[i] = ui_label_at(button, 54, 34, "", &ui_font_16, p->muted);
    }

    s_content = lv_obj_create(screen);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_content, 380, 78);
    lv_obj_set_size(s_content, 840, UI_CONTENT_BOTTOM - 78);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);

    build_section();
    ui_nav_bar(screen, UI_PAGE_SETTINGS);
    lv_obj_add_event_cb(screen, settings_delete_cb, LV_EVENT_DELETE, NULL);
    (void)settings_delete_cb;
    return screen;
}
