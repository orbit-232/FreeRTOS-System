/* 文件动态字体覆盖：文档中文预览 */
#include "ui_pages.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "lvgl.h"
#include "panel_services.h"
#include "ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *path_label;
    lv_obj_t *status_label;
    lv_obj_t *list;
    lv_obj_t *page_label;
    lv_obj_t *modal;
    lv_obj_t *modal_list;
    lv_timer_t *timer;
    panel_files_snapshot_t *snapshot;
    const lv_font_t *asset_font;
    panel_assets_state_t asset_state;
    uint32_t asset_retry_at;
    uint32_t modal_generation;
    uint32_t seen_generation;
} files_ctx_t;

static files_ctx_t *s_ctx;
static bool s_reset_root;

static void files_refresh_ui(files_ctx_t *ctx);
static void files_dismiss_preview(files_ctx_t *ctx);

static const lv_font_t *files_text_font(files_ctx_t *ctx)
{
    return (ctx != NULL && ctx->asset_font != NULL) ? ctx->asset_font : &ui_font_20;
}

static const char *state_text(panel_files_state_t state)
{
    switch (state) {
    case PANEL_FILES_STATE_MOUNTING: return "正在挂载TF卡…";
    case PANEL_FILES_STATE_READY: return "只读";
    case PANEL_FILES_STATE_NO_CARD: return "未检测到可用TF卡";
    case PANEL_FILES_STATE_ERROR: return "读取失败";
    default: return "初始化";
    }
}

static void files_close_modal(files_ctx_t *ctx)
{
    if (ctx->modal != NULL) {
        lv_obj_delete(ctx->modal);
        ctx->modal = NULL;
        ctx->modal_list = NULL;
        ctx->modal_generation = 0;
    }
}

static void files_modal_close_cb(lv_event_t *event)
{
    files_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx != NULL) files_dismiss_preview(ctx);
}

static void files_modal_open(files_ctx_t *ctx)
{
    if (ctx->modal != NULL) {
        if (ctx->modal_generation == ctx->snapshot->generation) return;
        files_close_modal(ctx);
    }

    const ui_palette_t *p = ui_palette();
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_t *card = ui_card(overlay, 80, 40, 1120, 660);
    ui_label_at(card, 28, 18, "文件预览", &ui_font_32, p->text);
    lv_obj_t *name = ui_label_at(card, 28, 66, "", files_text_font(ctx), p->text);
    lv_obj_set_width(name, 1040);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(name, ctx->snapshot->preview_name);
    char info[192];
    snprintf(info, sizeof(info), "%llu 字节 · %s · %s%s",
             (unsigned long long)ctx->snapshot->preview_size,
             ctx->snapshot->preview_type[0] ? ctx->snapshot->preview_type : "文件",
             (ctx->snapshot->preview_mtime_valid && ctx->snapshot->preview_mtime_text[0])
                 ? ctx->snapshot->preview_mtime_text : "时间未知",
             ctx->snapshot->preview_truncated ? " · 已截断" : "");
    ui_label_at(card, 28, 104, info, &ui_font_16, p->muted);

    lv_obj_t *viewport = lv_obj_create(card);
    lv_obj_set_pos(viewport, 28, 144);
    lv_obj_set_size(viewport, 1060, 420);
    lv_obj_set_style_bg_opa(viewport, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(viewport, 0, 0);
    lv_obj_set_style_pad_all(viewport, 0, 0);
    lv_obj_set_style_pad_right(viewport, 10, 0);
    lv_obj_set_scroll_dir(viewport, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t *body = lv_label_create(viewport);
    lv_obj_set_pos(body, 0, 0);
    lv_obj_set_width(body, 1020);
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(body, files_text_font(ctx), 0);
    lv_obj_set_style_text_color(body, p->text, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    if (ctx->snapshot->preview_error) {
        lv_label_set_text(body, ctx->snapshot->preview_error_text);
    } else {
        lv_label_set_text(body, ctx->snapshot->preview);
    }
    ui_button(card, 28, 586, 300, 52, "关闭", &ui_font_20, files_modal_close_cb, ctx);
    ctx->modal = overlay;
    ctx->modal_generation = ctx->snapshot->generation;
}

static void files_entry_cb(lv_event_t *event)
{
    files_ctx_t *ctx = lv_event_get_user_data(event);
    unsigned index = (unsigned)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(event));
    if (ctx == NULL || ctx->snapshot == NULL || index >= ctx->snapshot->count) return;
    panel_files_entry_t *e = &ctx->snapshot->entries[index];
    if (!e->valid_name || !e->valid_utf8) {
        ui_toast(e->truncated_name ? "文件名过长，已禁用" : "文件名编码不受支持");
        return;
    }
    esp_err_t err = e->is_dir ? panel_services_files_enter(index, ctx->snapshot->generation)
                              : panel_services_files_preview(index, ctx->snapshot->generation);
    if (err != ESP_OK) ui_toast(err == ESP_ERR_TIMEOUT ? "命令队列忙" : "操作失败");
}

static void files_rebuild_list(files_ctx_t *ctx)
{
    if (ctx->list == NULL || ctx->snapshot == NULL) return;
    lv_obj_clean(ctx->list);
    if (ctx->snapshot->state != PANEL_FILES_STATE_READY) {
        return;
    }
    for (unsigned i = 0; i < ctx->snapshot->count; i++) {
        panel_files_entry_t *e = &ctx->snapshot->entries[i];
        lv_obj_t *row = lv_button_create(ctx->list);
        lv_obj_set_size(row, 1160, 38);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_user_data(row, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(row, files_entry_cb, LV_EVENT_CLICKED, ctx);
        char text[160];
        snprintf(text, sizeof(text), "%s %s", e->is_dir ? "[D]" : "[F]", e->name);
        lv_obj_t *label = ui_label_at(row, 8, 6, text, files_text_font(ctx),
                                      e->valid_name && e->valid_utf8 ? ui_palette()->text
                                                                     : ui_palette()->danger);
        lv_obj_set_width(label, 1120);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    }
}

static void files_apply_text_font(files_ctx_t *ctx)
{
    if (ctx == NULL) return;
    const lv_font_t *font = files_text_font(ctx);
    if (ctx->path_label != NULL) lv_obj_set_style_text_font(ctx->path_label, font, 0);
    if (ctx->list != NULL) lv_obj_set_style_text_font(ctx->list, font, 0);
}

static void files_dismiss_preview(files_ctx_t *ctx)
{
    if (ctx == NULL) return;
    if (ctx->snapshot != NULL) {
        ctx->snapshot->preview_open = false;
        ctx->snapshot->preview_error = false;
        ctx->snapshot->preview_error_text[0] = '\0';
    }
    files_close_modal(ctx);
    (void)panel_services_files_close_preview();
}

static void files_update_asset(files_ctx_t *ctx)
{
    if (ctx == NULL) return;
    panel_assets_status_t asset;
    panel_services_get_assets(&asset);
    ctx->asset_state = asset.state;
    if (ctx->asset_font == NULL && asset.state == PANEL_ASSETS_STATE_READY) {
        const lv_font_t *font = panel_services_assets_acquire_font();
        if (font != NULL) {
            ctx->asset_font = font;
            files_apply_text_font(ctx);
            if (ctx->modal != NULL) files_close_modal(ctx);
            files_refresh_ui(ctx);
        }
        return;
    }
    if (ctx->asset_font == NULL &&
        (asset.state == PANEL_ASSETS_STATE_IDLE ||
         asset.state == PANEL_ASSETS_STATE_NO_CARD ||
         asset.state == PANEL_ASSETS_STATE_IO_ERROR)) {
        uint32_t now = lv_tick_get();
        if ((int32_t)(now - ctx->asset_retry_at) >= 0) {
            ctx->asset_retry_at = now + (asset.state == PANEL_ASSETS_STATE_NO_CARD ? 2000U : 10000U);
            (void)panel_services_assets_request_font();
        }
    }
}

static void files_refresh_ui(files_ctx_t *ctx)
{
    if (ctx->snapshot == NULL) return;
    lv_label_set_text(ctx->path_label, ctx->snapshot->path[0] ? ctx->snapshot->path : "/");
    const char *status = ctx->snapshot->error[0] ? ctx->snapshot->error
                                                 : state_text(ctx->snapshot->state);
    char status_buf[128];
    if (ctx->snapshot->state == PANEL_FILES_STATE_READY && ctx->snapshot->error[0] == '\0') {
        const char *suffix = ctx->asset_font != NULL ? "外置中文字体" :
                             ctx->asset_state == PANEL_ASSETS_STATE_LOADING ? "加载外置字体" :
                             "内置字体";
        snprintf(status_buf, sizeof(status_buf), "%s · %s", status, suffix);
        status = status_buf;
    }
    lv_label_set_text(ctx->status_label, status);
    if (ctx->snapshot->page_count > 0) {
        lv_label_set_text_fmt(ctx->page_label, "%u / %u", ctx->snapshot->page + 1,
                              ctx->snapshot->page_count);
    } else {
        lv_label_set_text(ctx->page_label, "- / -");
    }
    files_rebuild_list(ctx);
    if (ctx->snapshot->preview_open || ctx->snapshot->preview_error) {
        files_modal_open(ctx);
    } else {
        files_close_modal(ctx);
    }
}

static void files_timer_cb(lv_timer_t *timer)
{
    files_ctx_t *ctx = lv_timer_get_user_data(timer);
    if (ctx == NULL || ctx->snapshot == NULL) return;
    files_update_asset(ctx);
    panel_services_get_files(ctx->snapshot);
    if (ctx->snapshot->generation != ctx->seen_generation) {
        ctx->seen_generation = ctx->snapshot->generation;
        files_refresh_ui(ctx);
    }
}

static void files_action_cb(lv_event_t *event)
{
    files_ctx_t *ctx = lv_event_get_user_data(event);
    int action = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(event));
    if (ctx == NULL || ctx->snapshot == NULL) return;
    esp_err_t err = ESP_OK;
    switch (action) {
    case 0: err = panel_services_files_root(ctx->snapshot->generation); break;
    case 1: err = panel_services_files_up(ctx->snapshot->generation); break;
    case 2: err = panel_services_files_refresh(ctx->snapshot->generation); break;
    case 3: err = panel_services_files_retry(); break;
    case 4: err = panel_services_files_page(-1, ctx->snapshot->generation); break;
    case 5: err = panel_services_files_page(1, ctx->snapshot->generation); break;
    default: break;
    }
    if (err != ESP_OK) ui_toast(err == ESP_ERR_TIMEOUT ? "命令队列忙" : "操作失败");
    if (action == 3 && err == ESP_OK) {
        ctx->asset_retry_at = 0;
        (void)panel_services_assets_request_font();
    }
}

static void files_delete_cb(lv_event_t *event)
{
    files_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx == NULL) return;
    if (ctx->timer != NULL) lv_timer_delete(ctx->timer);
    files_close_modal(ctx);
    if (ctx->list != NULL) lv_obj_clean(ctx->list);
    if (ctx->path_label != NULL) lv_obj_set_style_text_font(ctx->path_label, &ui_font_20, 0);
    if (ctx->status_label != NULL) lv_obj_set_style_text_font(ctx->status_label, &ui_font_16, 0);
    if (ctx->asset_font != NULL) {
        panel_services_assets_release_font();
        ctx->asset_font = NULL;
    } else {
        panel_services_assets_cancel_font();
    }
    if (s_ctx == ctx) s_ctx = NULL;
    heap_caps_free(ctx->snapshot);
    lv_free(ctx);
}

bool ui_page_files_back(void)
{
    if (s_ctx == NULL) return false;
    if (s_ctx->modal != NULL) {
        files_dismiss_preview(s_ctx);
        return true;
    }
    if (s_ctx->snapshot != NULL && strcmp(s_ctx->snapshot->path, "/") != 0) {
        if (panel_services_files_up(s_ctx->snapshot->generation) == ESP_OK) return true;
    }
    return false;
}

void ui_page_files_ui_state_reset(void) { s_reset_root = true; }

void ui_page_files_state_summary(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return;
    panel_files_snapshot_t *snap = heap_caps_malloc(sizeof(*snap),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (snap == NULL) { snprintf(buf, len, "文件"); return; }
    panel_services_get_files(snap);
    snprintf(buf, len, "%s · %s", snap->path[0] ? snap->path : "/",
             snap->state == PANEL_FILES_STATE_READY ? "只读" : state_text(snap->state));
    heap_caps_free(snap);
}

lv_obj_t *ui_page_files_create(void)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *screen = ui_screen_create();
    if (ui_theme_effective() == PANEL_THEME_DARK) ui_screen_gradient(screen, p->accent_soft, p->bg);
    else ui_screen_gradient(screen, lv_color_hex(0xEAF5EE), lv_color_hex(0xF9F8F7));
    ui_status_bar(screen, false);
    lv_obj_t *header = ui_header(screen, "文件", NULL);

    files_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    panel_files_snapshot_t *snap = heap_caps_malloc(sizeof(*snap),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx == NULL || snap == NULL) {
        heap_caps_free(snap);
        lv_free(ctx);
        return screen;
    }
    memset(ctx, 0, sizeof(*ctx));
    memset(snap, 0, sizeof(*snap));
    ctx->screen = screen;
    ctx->snapshot = snap;
    s_ctx = ctx;

    lv_obj_t *b0 = ui_button(header, 360, 8, 100, 40, "根目录", &ui_font_16, files_action_cb, ctx);
    lv_obj_t *b1 = ui_button(header, 470, 8, 100, 40, "上级", &ui_font_16, files_action_cb, ctx);
    lv_obj_t *b2 = ui_button(header, 580, 8, 100, 40, "刷新", &ui_font_16, files_action_cb, ctx);
    lv_obj_t *b3 = ui_button(header, 690, 8, 100, 40, "重试", &ui_font_16, files_action_cb, ctx);
    lv_obj_set_user_data(b0, (void *)(uintptr_t)0);
    lv_obj_set_user_data(b1, (void *)(uintptr_t)1);
    lv_obj_set_user_data(b2, (void *)(uintptr_t)2);
    lv_obj_set_user_data(b3, (void *)(uintptr_t)3);

    ctx->path_label = ui_label_at(screen, 40, 150, "/", &ui_font_20, p->text);
    lv_obj_set_width(ctx->path_label, 800);
    lv_label_set_long_mode(ctx->path_label, LV_LABEL_LONG_MODE_DOTS);
    ctx->status_label = ui_label_at(screen, 40, 180, "正在挂载…", &ui_font_16, p->muted);
    lv_obj_set_width(ctx->status_label, 800);

    ctx->list = lv_obj_create(screen);
    lv_obj_remove_flag(ctx->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(ctx->list, 40, 210);
    lv_obj_set_size(ctx->list, 1160, 456);
    lv_obj_set_style_bg_opa(ctx->list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->list, 0, 0);
    lv_obj_set_style_pad_all(ctx->list, 0, 0);
    lv_obj_set_style_pad_row(ctx->list, 0, 0);
    lv_obj_set_flex_flow(ctx->list, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *prev = ui_button(screen, 40, 682, 110, 38, "上一页", &ui_font_16,
                               files_action_cb, ctx);
    ctx->page_label = ui_label_at(screen, 180, 690, "- / -", &ui_font_16, p->muted);
    lv_obj_t *next = ui_button(screen, 300, 682, 110, 38, "下一页", &ui_font_16,
                               files_action_cb, ctx);
    lv_obj_set_user_data(prev, (void *)(uintptr_t)4);
    lv_obj_set_user_data(next, (void *)(uintptr_t)5);

    ui_nav_bar(screen, UI_PAGE_FILES);
    ctx->timer = lv_timer_create(files_timer_cb, 200, ctx);
    lv_obj_add_event_cb(screen, files_delete_cb, LV_EVENT_DELETE, ctx);
    ctx->asset_retry_at = lv_tick_get();
    (void)panel_services_assets_request_font();
    if (s_reset_root) {
        s_reset_root = false;
        panel_services_files_root(0);
    }
    files_timer_cb(ctx->timer);
    return screen;
}
