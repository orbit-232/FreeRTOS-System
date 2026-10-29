#include "ui_network_panel.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "panel_network.h"
#include "ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"

typedef enum { NET_ROW_WIFI, NET_ROW_BLE } net_row_type_t;

typedef struct {
    net_row_type_t type;
    uint8_t authmode;
    uint8_t addr[6];
    uint8_t addr_type;
    char ssid[PANEL_WIFI_SSID_MAX + 1];
    char name[PANEL_BLE_NAME_MAX + 1];
} net_row_ref_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *wifi_status;
    lv_obj_t *wifi_list;
    lv_obj_t *wifi_switch;
    lv_obj_t *wifi_scan;
    lv_obj_t *wifi_disconnect;
    lv_obj_t *wifi_forget;
    lv_obj_t *wifi_detail;
    lv_obj_t *ble_status;
    lv_obj_t *ble_list;
    lv_obj_t *ble_switch;
    lv_obj_t *ble_scan;
    lv_obj_t *ble_stop;
    lv_obj_t *ble_disconnect;
    lv_obj_t *modal;
    lv_obj_t *modal_ssid;
    lv_obj_t *modal_error;
    lv_timer_t *timer;
    uint32_t wifi_seen;
    uint32_t ble_seen;
    uint8_t pending_authmode;
    char pending_ssid[PANEL_WIFI_SSID_MAX + 1];
} network_ctx_t;

static network_ctx_t *s_ctx;

static const char *wifi_state_text(panel_wifi_state_t state)
{
    switch (state) {
    case PANEL_WIFI_OFF: return "已关闭";
    case PANEL_WIFI_IDLE: return "未连接";
    case PANEL_WIFI_SCANNING: return "扫描中";
    case PANEL_WIFI_CONNECTING: return "连接中";
    case PANEL_WIFI_CONNECTED: return "已连接";
    case PANEL_WIFI_FAILED: return "未连接 · 可重试";
    case PANEL_WIFI_UNAVAILABLE: return "不可用";
    default: return "未知";
    }
}

static const char *ble_state_text(panel_ble_state_t state)
{
    switch (state) {
    case PANEL_BLE_OFF: return "已关闭";
    case PANEL_BLE_IDLE: return "未连接";
    case PANEL_BLE_SCANNING: return "扫描中";
    case PANEL_BLE_CONNECTING: return "连接中";
    case PANEL_BLE_CONNECTED: return "已连接";
    case PANEL_BLE_FAILED: return "未连接 · 可重试";
    case PANEL_BLE_UNAVAILABLE: return "不可用";
    default: return "未知";
    }
}

static void row_ref_delete_cb(lv_event_t *event)
{
    void *ref = lv_event_get_user_data(event);
    lv_free(ref);
}

static void modal_close(network_ctx_t *ctx)
{
    if (ctx != NULL && ctx->modal != NULL) {
        lv_obj_t *modal = ctx->modal;
        ctx->modal = NULL;
        ctx->modal_ssid = NULL;
        ctx->modal_error = NULL;
        ctx->pending_ssid[0] = '\0';
        lv_obj_delete(modal);
    }
}

static void set_modal_error(network_ctx_t *ctx, const char *text)
{
    if (ctx != NULL && ctx->modal_error != NULL) {
        lv_label_set_text(ctx->modal_error, text);
    }
}

static void modal_cancel_cb(lv_event_t *event)
{
    network_ctx_t *ctx = lv_event_get_user_data(event);
    modal_close(ctx);
}

static void modal_connect_cb(lv_event_t *event)
{
    network_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx == NULL || ctx->modal_ssid == NULL || ctx->pending_ssid[0] == '\0') {
        modal_close(ctx);
        return;
    }
    const char *password = lv_textarea_get_text(ctx->modal_ssid);
    char reason[64];
    if (!panel_wifi_credentials_valid(ctx->pending_ssid, password, ctx->pending_authmode,
                                      reason, sizeof(reason))) {
        set_modal_error(ctx, reason[0] ? reason : "凭据无效");
        return;
    }
    esp_err_t err = panel_network_wifi_connect(ctx->pending_ssid, password,
                                               ctx->pending_authmode, true);
    if (err != ESP_OK) {
        set_modal_error(ctx, err == ESP_ERR_TIMEOUT ? "命令队列忙，请重试" : "连接命令被拒绝");
        return;
    }
    modal_close(ctx);
}

static void password_show_cb(lv_event_t *event)
{
    network_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx == NULL || ctx->modal_ssid == NULL) {
        return;
    }
    bool masked = lv_textarea_get_password_mode(ctx->modal_ssid);
    lv_textarea_set_password_mode(ctx->modal_ssid, !masked);
    lv_obj_t *label = lv_obj_get_child(lv_event_get_target_obj(event), 0);
    if (label != NULL) {
        lv_label_set_text(label, masked ? "隐藏" : "显示");
    }
}

static void keyboard_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    network_ctx_t *ctx = lv_event_get_user_data(event);
    if (code == LV_EVENT_CANCEL) {
        modal_close(ctx);
    }
    /* READY keeps the keyboard visible so Connect/Cancel stay reachable. */
}

static void open_password_modal(network_ctx_t *ctx, const char *ssid, uint8_t authmode)
{
    if (ctx == NULL || ctx->root == NULL || ssid == NULL) {
        return;
    }
    modal_close(ctx);
    snprintf(ctx->pending_ssid, sizeof(ctx->pending_ssid), "%s", ssid);
    ctx->pending_authmode = authmode;

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(overlay, 380, 78);
    lv_obj_set_size(overlay, 840, 660);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);

    lv_obj_t *panel = ui_card(overlay, 20, 20, 800, 600);
    ui_label_at(panel, 20, 18, "连接 Wi-Fi", &ui_font_32, ui_palette()->text);
    lv_obj_t *ssid_label = ui_label_at(panel, 20, 62, ssid, &ui_font_20,
                                       ui_palette()->muted);
    lv_obj_set_width(ssid_label, 750);
    lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_MODE_CLIP);

    lv_obj_t *ta = lv_textarea_create(panel);
    lv_obj_set_pos(ta, 20, 98);
    lv_obj_set_size(ta, 740, 50);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_max_length(ta, PANEL_WIFI_PASS_MAX);
    lv_textarea_set_placeholder_text(ta, "密码（开放网络留空）");
    lv_obj_set_style_text_font(ta, &ui_font_20, 0);

    lv_obj_t *error = ui_label_at(panel, 20, 156, "", &ui_font_16, ui_palette()->danger);
    lv_obj_set_width(error, 740);
    lv_obj_set_style_text_color(error, lv_color_hex(0xC0392B), 0);

    ui_button(panel, 20, 184, 140, 46, "显示", &ui_font_16, password_show_cb, ctx);
    ui_button(panel, 176, 184, 140, 46, "取消", &ui_font_16, modal_cancel_cb, ctx);
    lv_obj_t *connect = ui_button(panel, 620, 184, 140, 46, "连接", &ui_font_16,
                                  modal_connect_cb, ctx);
    lv_obj_set_style_bg_color(connect, ui_palette()->accent, 0);

    lv_obj_t *kb = lv_keyboard_create(panel);
    lv_obj_set_pos(kb, 20, 246);
    lv_obj_set_size(kb, 760, 320);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, keyboard_cb, LV_EVENT_ALL, ctx);

    ctx->modal = overlay;
    ctx->modal_ssid = ta;
    ctx->modal_error = error;
}

static void wifi_ap_clicked_cb(lv_event_t *event)
{
    network_ctx_t *ctx = lv_event_get_user_data(event);
    net_row_ref_t *ref = lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (ctx == NULL || ref == NULL || ref->type != NET_ROW_WIFI || ref->ssid[0] == '\0') {
        return;
    }
    open_password_modal(ctx, ref->ssid, ref->authmode);
}

static void ble_device_clicked_cb(lv_event_t *event)
{
    network_ctx_t *ctx = lv_event_get_user_data(event);
    net_row_ref_t *ref = lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (ctx == NULL || ref == NULL || ref->type != NET_ROW_BLE) {
        return;
    }
    esp_err_t err = panel_network_ble_connect(ref->addr, ref->addr_type);
    if (err != ESP_OK) {
        ui_toast(err == ESP_ERR_TIMEOUT ? "蓝牙命令队列忙" : "蓝牙未开启");
    }
}

static lv_obj_t *make_row(lv_obj_t *list, const char *text, net_row_ref_t *ref,
                          lv_event_cb_t cb, network_ctx_t *ctx)
{
    lv_obj_t *row = lv_button_create(list);
    lv_obj_set_size(row, 760, 46);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_user_data(row, ref);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, row_ref_delete_cb, LV_EVENT_DELETE, ref);
    ui_label_at(row, 8, 12, text, &ui_font_16, ui_palette()->text);
    return row;
}

static void rebuild_wifi_list(network_ctx_t *ctx, const panel_wifi_snapshot_t *snapshot)
{
    lv_obj_clean(ctx->wifi_list);
    for (unsigned i = 0; i < snapshot->ap_count; i++) {
        const panel_wifi_ap_t *ap = &snapshot->aps[i];
        net_row_ref_t *ref = lv_malloc(sizeof(*ref));
        if (ref == NULL) continue;
        memset(ref, 0, sizeof(*ref));
        ref->type = NET_ROW_WIFI;
        ref->authmode = ap->authmode;
        snprintf(ref->ssid, sizeof(ref->ssid), "%s", ap->ssid);
        char text[96];
        snprintf(text, sizeof(text), "%s  %s  %d dBm",
                 ap->ssid[0] ? ap->ssid : "<隐藏网络>",
                 ap->authmode == 0 ? "开放" : "加密", ap->rssi);
        make_row(ctx->wifi_list, text, ref, wifi_ap_clicked_cb, ctx);
    }
    if (snapshot->ap_count == 0) {
        ui_label_at(ctx->wifi_list, 8, 12,
                    snapshot->scanning ? "扫描中…" : "未发现网络",
                    &ui_font_16, ui_palette()->muted);
    }
}

static void rebuild_ble_list(network_ctx_t *ctx, const panel_ble_snapshot_t *snapshot)
{
    lv_obj_clean(ctx->ble_list);
    for (unsigned i = 0; i < snapshot->device_count; i++) {
        const panel_ble_device_t *dev = &snapshot->devices[i];
        net_row_ref_t *ref = lv_malloc(sizeof(*ref));
        if (ref == NULL) continue;
        memset(ref, 0, sizeof(*ref));
        ref->type = NET_ROW_BLE;
        ref->addr_type = dev->addr_type;
        memcpy(ref->addr, dev->addr, 6);
        snprintf(ref->name, sizeof(ref->name), "%s", dev->name);
        char text[128];
        snprintf(text, sizeof(text), "%s  %02X:%02X:%02X:%02X:%02X:%02X  t=%u  %d dBm",
                 dev->name[0] ? dev->name : "(匿名)",
                 dev->addr[5], dev->addr[4], dev->addr[3], dev->addr[2],
                 dev->addr[1], dev->addr[0], dev->addr_type, dev->rssi);
        make_row(ctx->ble_list, text, ref, ble_device_clicked_cb, ctx);
    }
    if (snapshot->device_count == 0) {
        ui_label_at(ctx->ble_list, 8, 12,
                    snapshot->scanning ? "扫描中…" : "未发现设备",
                    &ui_font_16, ui_palette()->muted);
    }
}

static void network_timer_cb(lv_timer_t *timer)
{
    network_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (ctx == NULL) return;
    panel_wifi_snapshot_t wifi;
    panel_ble_snapshot_t ble;
    panel_network_wifi_get(&wifi);
    panel_network_ble_get(&ble);

    if (ctx->wifi_status != NULL) {
        char text[180];
        if (wifi.connected) {
            snprintf(text, sizeof(text), "已连接 %s · %s%s", wifi.ssid, wifi.ip,
                     wifi.scanning ? " · 扫描中" : "");
        } else {
            snprintf(text, sizeof(text), "%s%s%s", wifi_state_text(wifi.state),
                     wifi.reason[0] ? " · " : "", wifi.reason);
        }
        lv_label_set_text(ctx->wifi_status, text);
    }
    if (ctx->wifi_detail != NULL) {
        lv_label_set_text_fmt(ctx->wifi_detail, "%u 个网络 · %s", wifi.ap_count,
                              wifi.scanning ? "扫描中" : "空闲");
    }
    if (ctx->ble_status != NULL) {
        char text[180];
        if (ble.state == PANEL_BLE_CONNECTED) {
            snprintf(text, sizeof(text), "已连接 %s · 服务 %u",
                     ble.connected_name[0] ? ble.connected_name : "(匿名)",
                     ble.service_count);
        } else {
            snprintf(text, sizeof(text), "%s%s%s", ble_state_text(ble.state),
                     ble.reason[0] ? " · " : "", ble.reason);
        }
        lv_label_set_text(ctx->ble_status, text);
    }
    if (ctx->wifi_switch != NULL) {
        if (wifi.enabled) lv_obj_add_state(ctx->wifi_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(ctx->wifi_switch, LV_STATE_CHECKED);
    }
    if (ctx->ble_switch != NULL) {
        if (ble.enabled) lv_obj_add_state(ctx->ble_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(ctx->ble_switch, LV_STATE_CHECKED);
    }
    if (wifi.generation != ctx->wifi_seen) {
        ctx->wifi_seen = wifi.generation;
        rebuild_wifi_list(ctx, &wifi);
    }
    if (ble.generation != ctx->ble_seen) {
        ctx->ble_seen = ble.generation;
        rebuild_ble_list(ctx, &ble);
    }
}

static void wifi_switch_cb(lv_event_t *event)
{
    network_ctx_t *ctx = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_target_obj(event);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    esp_err_t err = panel_network_wifi_set_enabled(enabled);
    if (err != ESP_OK) {
        if (enabled) lv_obj_remove_state(sw, LV_STATE_CHECKED);
        else lv_obj_add_state(sw, LV_STATE_CHECKED);
        ui_toast(err == ESP_ERR_TIMEOUT ? "Wi-Fi 命令队列忙" : "Wi-Fi 命令被拒绝");
        return;
    }
    if (!enabled && ctx != NULL && ctx->wifi_detail != NULL) {
        lv_label_set_text(ctx->wifi_detail, "Wi-Fi 已关闭");
    }
}

static void wifi_scan_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = panel_network_wifi_scan();
    if (err != ESP_OK) ui_toast("Wi-Fi 未开启或命令队列忙");
}

static void wifi_forget_cb(lv_event_t *event)
{
    (void)event;
    if (panel_network_wifi_disconnect(true) != ESP_OK) ui_toast("忘记网络命令队列忙");
}

static void wifi_disconnect_cb(lv_event_t *event)
{
    (void)event;
    if (panel_network_wifi_disconnect(false) != ESP_OK) ui_toast("断开命令队列忙");
}

static void ble_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    esp_err_t err = panel_network_ble_set_enabled(enabled);
    if (err != ESP_OK) {
        if (enabled) lv_obj_remove_state(sw, LV_STATE_CHECKED);
        else lv_obj_add_state(sw, LV_STATE_CHECKED);
        ui_toast("蓝牙命令队列忙");
    }
}

static void ble_scan_cb(lv_event_t *event)
{
    (void)event;
    if (panel_network_ble_scan() != ESP_OK) ui_toast("蓝牙未开启或命令队列忙");
}

static void ble_stop_cb(lv_event_t *event)
{
    (void)event;
    if (panel_network_ble_stop() != ESP_OK) ui_toast("蓝牙命令队列忙");
}

static void ble_disconnect_cb(lv_event_t *event)
{
    (void)event;
    if (panel_network_ble_disconnect() != ESP_OK) ui_toast("蓝牙命令队列忙");
}

static void network_delete_cb(lv_event_t *event)
{
    network_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx == NULL) return;
    if (ctx->timer != NULL) {
        lv_timer_delete(ctx->timer);
        ctx->timer = NULL;
    }
    modal_close(ctx);
    if (s_ctx == ctx) s_ctx = NULL;
    lv_free(ctx);
}

bool ui_network_panel_back(void)
{
    if (s_ctx == NULL || s_ctx->modal == NULL) return false;
    modal_close(s_ctx);
    return true;
}

#ifdef WAVESHARE_HOST_SIM
void ui_network_panel_test_open(const char *ssid, uint8_t authmode)
{
    if (s_ctx != NULL) open_password_modal(s_ctx, ssid, authmode);
}

bool ui_network_panel_test_modal_open(void)
{
    return s_ctx != NULL && s_ctx->modal != NULL;
}
#endif

lv_obj_t *ui_network_panel_create(lv_obj_t *parent)
{
    if (parent == NULL) return NULL;
    network_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx == NULL) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->wifi_seen = UINT32_MAX;
    ctx->ble_seen = UINT32_MAX;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, 840, 626);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    ctx->root = root;

    lv_obj_t *wifi = ui_card(root, 0, 100, 840, 270);
    ui_label_at(wifi, 24, 18, "Wi-Fi", &ui_font_32, ui_palette()->text);
    ctx->wifi_status = ui_label_at(wifi, 132, 24, "", &ui_font_16, ui_palette()->muted);
    lv_obj_set_width(ctx->wifi_status, 560);
    ctx->wifi_switch = ui_switch_create(wifi, 756, 16, false);
    lv_obj_add_event_cb(ctx->wifi_switch, wifi_switch_cb, LV_EVENT_VALUE_CHANGED, ctx);
    ctx->wifi_detail = ui_label_at(wifi, 24, 66, "", &ui_font_16, ui_palette()->muted);
    ctx->wifi_scan = ui_button(wifi, 520, 58, 90, 40, "扫描", &ui_font_16,
                               wifi_scan_cb, NULL);
    ctx->wifi_disconnect = ui_button(wifi, 620, 58, 90, 40, "断开", &ui_font_16,
                                     wifi_disconnect_cb, NULL);
    lv_obj_set_style_bg_color(ctx->wifi_disconnect, ui_palette()->surface_alt, 0);
    ctx->wifi_forget = ui_button(wifi, 720, 58, 90, 40, "忘记", &ui_font_16,
                                 wifi_forget_cb, NULL);
    lv_obj_set_style_bg_color(ctx->wifi_forget, ui_palette()->surface_alt, 0);
    ctx->wifi_list = lv_obj_create(wifi);
    lv_obj_set_pos(ctx->wifi_list, 24, 108);
    lv_obj_set_size(ctx->wifi_list, 792, 142);
    lv_obj_set_style_bg_opa(ctx->wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->wifi_list, 0, 0);
    lv_obj_set_style_pad_all(ctx->wifi_list, 0, 0);
    lv_obj_set_flex_flow(ctx->wifi_list, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *ble = ui_card(root, 0, 382, 840, 234);
    ui_label_at(ble, 24, 18, "蓝牙", &ui_font_32, ui_palette()->text);
    ctx->ble_status = ui_label_at(ble, 132, 24, "", &ui_font_16, ui_palette()->muted);
    lv_obj_set_width(ctx->ble_status, 560);
    ctx->ble_switch = ui_switch_create(ble, 756, 16, false);
    lv_obj_add_event_cb(ctx->ble_switch, ble_switch_cb, LV_EVENT_VALUE_CHANGED, ctx);
    ctx->ble_scan = ui_button(ble, 520, 58, 90, 40, "扫描", &ui_font_16,
                              ble_scan_cb, NULL);
    ctx->ble_stop = ui_button(ble, 620, 58, 90, 40, "停止", &ui_font_16,
                              ble_stop_cb, NULL);
    lv_obj_set_style_bg_color(ctx->ble_stop, ui_palette()->surface_alt, 0);
    ctx->ble_disconnect = ui_button(ble, 720, 58, 90, 40, "断开", &ui_font_16,
                                    ble_disconnect_cb, NULL);
    lv_obj_set_style_bg_color(ctx->ble_disconnect, ui_palette()->surface_alt, 0);
    ctx->ble_list = lv_obj_create(ble);
    lv_obj_set_pos(ctx->ble_list, 24, 108);
    lv_obj_set_size(ctx->ble_list, 792, 112);
    lv_obj_set_style_bg_opa(ctx->ble_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->ble_list, 0, 0);
    lv_obj_set_style_pad_all(ctx->ble_list, 0, 0);
    lv_obj_set_flex_flow(ctx->ble_list, LV_FLEX_FLOW_COLUMN);

    s_ctx = ctx;
    ctx->timer = lv_timer_create(network_timer_cb, 500, ctx);
    lv_obj_add_event_cb(root, network_delete_cb, LV_EVENT_DELETE, ctx);
    network_timer_cb(ctx->timer);
    return root;
}
