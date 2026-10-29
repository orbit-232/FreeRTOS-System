#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "panel_calc.h"
#include "ui.h"
#include "ui_pages.h"
#include "ui_theme.h"
#include "ui_widgets.h"

typedef struct {
    calc_engine_t calc;
    lv_obj_t *history[3];
    lv_obj_t *expression;
    lv_obj_t *result;
    lv_obj_t *status;
} calc_ctx_t;

static calc_engine_t s_saved_calc;
static bool s_saved_calc_valid;
static calc_ctx_t *s_active_ctx;
static calc_engine_t s_bench_saved_calc;
static bool s_bench_saved_valid;
static bool s_bench_snapshot_taken;

static const char *op_glyph(char op)
{
    switch (op) {
    case '+':
        return "+";
    case '-':
        return "−";
    case '*':
        return "×";
    case '/':
        return "÷";
    default:
        return "";
    }
}

static void calc_refresh(calc_ctx_t *ctx)
{
    const ui_palette_t *p = ui_palette();
    for (int i = 0; i < 3; i++) {
        const char *text = i < ctx->calc.history_count ? ctx->calc.history[i] : "";
        lv_label_set_text(ctx->history[i], text);
    }

    char expression[64] = "";
    if (ctx->calc.pending_op != 0) {
        char acc[24];
        snprintf(acc, sizeof(acc), "%.10g", ctx->calc.accumulator);
        if (ctx->calc.entering) {
            snprintf(expression, sizeof(expression), "%s %s %s", acc,
                     op_glyph(ctx->calc.pending_op), ctx->calc.entry);
        } else {
            snprintf(expression, sizeof(expression), "%s %s", acc,
                     op_glyph(ctx->calc.pending_op));
        }
    } else if (ctx->calc.history_count > 0) {
        snprintf(expression, sizeof(expression), "%s",
                 ctx->calc.history[ctx->calc.history_count - 1]);
    }
    lv_label_set_text(ctx->expression, expression);

    size_t len = strlen(ctx->calc.display);
    if (ctx->calc.error) {
        lv_obj_set_style_text_font(ctx->result, &ui_font_32, 0);
        lv_obj_set_style_text_color(ctx->result, p->danger, 0);
    } else {
        lv_obj_set_style_text_font(ctx->result,
                                   len > 10 ? &ui_font_num_48 : &ui_font_num_80, 0);
        lv_obj_set_style_text_color(ctx->result, p->text, 0);
    }
    lv_label_set_text(ctx->result, ctx->calc.display);

    if (ctx->calc.error) {
        lv_label_set_text_fmt(ctx->status, "%s · 按 C 清除", calc_status_text(ctx->calc.status));
        lv_obj_set_style_text_color(ctx->status, p->danger, 0);
    } else {
        lv_label_set_text(ctx->status, "本地运算 · 最近 3 条历史 · 结果最多显示 14 位");
        lv_obj_set_style_text_color(ctx->status, p->muted, 0);
    }
}

static void key_cb(lv_event_t *event)
{
    calc_ctx_t *ctx = lv_event_get_user_data(event);
    const char *key = lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (ctx == NULL || key == NULL) {
        return;
    }

    if (strcmp(key, "C") == 0) {
        calc_clear(&ctx->calc);
    } else if (strcmp(key, "±") == 0) {
        calc_toggle_sign(&ctx->calc);
    } else if (strcmp(key, "%") == 0) {
        calc_percent(&ctx->calc);
    } else if (strcmp(key, "÷") == 0) {
        calc_set_operator(&ctx->calc, '/');
    } else if (strcmp(key, "×") == 0) {
        calc_set_operator(&ctx->calc, '*');
    } else if (strcmp(key, "−") == 0) {
        calc_set_operator(&ctx->calc, '-');
    } else if (strcmp(key, "+") == 0) {
        calc_set_operator(&ctx->calc, '+');
    } else if (strcmp(key, "=") == 0) {
        calc_equals(&ctx->calc);
    } else if (strcmp(key, "BS") == 0) {
        calc_backspace(&ctx->calc);
    } else if (strcmp(key, ".") == 0) {
        calc_input_dot(&ctx->calc);
    } else if (key[0] >= '0' && key[0] <= '9' && key[1] == '\0') {
        calc_input_digit(&ctx->calc, key[0]);
    }
    calc_refresh(ctx);
}

static void calc_delete_cb(lv_event_t *event)
{
    calc_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx != NULL) {
        /* During a same-page forced rebuild page_add() creates the new
         * context before LVGL delivers the old page's delete event.  Only the
         * owner may publish a snapshot; a detached old tree must not overwrite
         * the newer active state. */
        if (s_active_ctx == ctx) {
            s_saved_calc = ctx->calc;
            s_saved_calc_valid = true;
            s_active_ctx = NULL;
        } else if (s_active_ctx == NULL) {
            s_saved_calc = ctx->calc;
            s_saved_calc_valid = true;
        }
        lv_free(ctx);
    }
}

void ui_page_calculator_state_reset(void)
{
    s_saved_calc_valid = false;
    memset(&s_saved_calc, 0, sizeof(s_saved_calc));
    if (s_active_ctx != NULL) {
        calc_init(&s_active_ctx->calc);
        calc_refresh(s_active_ctx);
    }
}

void ui_page_calculator_state_summary(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    const calc_engine_t *calc = s_active_ctx != NULL ? &s_active_ctx->calc
                                                     : (s_saved_calc_valid ? &s_saved_calc : NULL);
    if (calc == NULL) {
        snprintf(buf, len, "未打开");
        return;
    }
    if (calc->error) {
        snprintf(buf, len, "%s", calc_status_text(calc->status));
    } else if (calc->history_count > 0) {
        snprintf(buf, len, "%s · 历史 %u", calc->display, (unsigned)calc->history_count);
    } else {
        snprintf(buf, len, "%s", calc->display);
    }
}

void ui_page_calculator_benchmark_snapshot(void)
{
    s_bench_snapshot_taken = true;
    if (s_active_ctx != NULL) {
        s_bench_saved_calc = s_active_ctx->calc;
        s_bench_saved_valid = true;
    } else if (s_saved_calc_valid) {
        s_bench_saved_calc = s_saved_calc;
        s_bench_saved_valid = true;
    } else {
        memset(&s_bench_saved_calc, 0, sizeof(s_bench_saved_calc));
        s_bench_saved_valid = false;
    }
}

void ui_page_calculator_benchmark_restore(void)
{
    if (!s_bench_snapshot_taken) {
        return;
    }
    if (s_bench_saved_valid) {
        s_saved_calc = s_bench_saved_calc;
        s_saved_calc_valid = true;
        if (s_active_ctx != NULL) {
            s_active_ctx->calc = s_bench_saved_calc;
            calc_refresh(s_active_ctx);
        }
    } else {
        s_saved_calc_valid = false;
        memset(&s_saved_calc, 0, sizeof(s_saved_calc));
        if (s_active_ctx != NULL) {
            calc_init(&s_active_ctx->calc);
            calc_refresh(s_active_ctx);
        }
    }
    s_bench_snapshot_taken = false;
}

static void clear_history_cb(lv_event_t *event)
{
    calc_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx != NULL) {
        calc_clear_history(&ctx->calc);
        calc_refresh(ctx);
    }
}

static void add_key(lv_obj_t *parent, int col, int row, const char *text, const char *key_id,
                    const lv_font_t *font, bool function, bool equals, calc_ctx_t *ctx)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, col * 186, row * 110);
    lv_obj_set_size(button, 172, 96);
    lv_obj_set_style_radius(button, 20, 0);
    lv_obj_set_style_border_color(button, p->border, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, function ? p->surface_alt : p->surface, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, p->border, LV_STATE_PRESSED);

    lv_color_t label_color = p->text;
    if (equals) {
        lv_obj_set_style_bg_color(button, p->accent, 0);
        lv_obj_set_style_bg_color(button, p->accent, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(button, p->accent, 0);
        label_color = lv_color_white();
    } else if (!function && (strcmp(key_id, "÷") == 0 || strcmp(key_id, "×") == 0 ||
                             strcmp(key_id, "−") == 0 || strcmp(key_id, "+") == 0)) {
        lv_obj_set_style_bg_color(button, p->accent_soft, 0);
        lv_obj_set_style_border_color(button, p->accent_soft, 0);
        label_color = p->accent;
    }

    lv_obj_t *label = ui_label(button, text, font, label_color);
    lv_obj_center(label);
    lv_obj_set_user_data(button, (void *)key_id);
    lv_obj_add_event_cb(button, key_cb, LV_EVENT_CLICKED, ctx);
}

lv_obj_t *ui_page_calculator_create(void)
{
    const ui_palette_t *p = ui_palette();
    lv_obj_t *screen = ui_screen_create();
    ui_status_bar(screen, false);
    ui_header(screen, "计算器", "本地运算");

    calc_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return screen;
    }
    memset(ctx, 0, sizeof(*ctx));
    if (s_active_ctx != NULL) {
        /* Same-page force rebuild: the old context still owns the live input,
         * pending operator and history until the old page is deleted. */
        ctx->calc = s_active_ctx->calc;
    } else if (s_saved_calc_valid) {
        ctx->calc = s_saved_calc;
    } else {
        calc_init(&ctx->calc);
    }
    s_active_ctx = ctx;

    lv_obj_t *display = ui_card(screen, 40, 150, 440, 570);
    ui_label_at(display, 24, 20, "历史", &ui_font_16, p->muted);
    lv_obj_t *clear_history = ui_label_at(display, 320, 20, "清除历史", &ui_font_16, p->muted);
    lv_obj_add_flag(clear_history, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clear_history, clear_history_cb, LV_EVENT_CLICKED, ctx);

    for (int i = 0; i < 3; i++) {
        ctx->history[i] = ui_label_at(display, 24, 54 + i * 30, "", &ui_font_16, p->muted);
        lv_obj_set_width(ctx->history[i], 392);
        lv_obj_set_style_text_align(ctx->history[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    ctx->expression = ui_label_at(display, 24, 340, "", &ui_font_20, p->muted);
    lv_obj_set_width(ctx->expression, 392);
    lv_obj_set_style_text_align(ctx->expression, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(ctx->expression, LV_LABEL_LONG_MODE_CLIP);

    ctx->result = ui_label_at(display, 24, 382, "0", &ui_font_num_80, p->text);
    lv_obj_set_width(ctx->result, 392);
    lv_obj_set_style_text_align(ctx->result, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(ctx->result, LV_LABEL_LONG_MODE_CLIP);

    ctx->status = ui_label_at(display, 24, 520, "", &ui_font_16, p->muted);
    lv_obj_set_width(ctx->status, 392);
    lv_obj_set_style_text_align(ctx->status, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *keys = lv_obj_create(screen);
    lv_obj_remove_flag(keys, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(keys, 510, 150);
    lv_obj_set_size(keys, 730, 560);
    lv_obj_set_style_bg_opa(keys, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keys, 0, 0);
    lv_obj_set_style_pad_all(keys, 0, 0);

    add_key(keys, 0, 0, "C", "C", &ui_font_24, true, false, ctx);
    add_key(keys, 1, 0, "±", "±", &ui_font_24, true, false, ctx);
    add_key(keys, 2, 0, "%", "%", &ui_font_24, true, false, ctx);
    add_key(keys, 3, 0, "÷", "÷", &ui_font_32, false, false, ctx);

    add_key(keys, 0, 1, "7", "7", &ui_font_32, false, false, ctx);
    add_key(keys, 1, 1, "8", "8", &ui_font_32, false, false, ctx);
    add_key(keys, 2, 1, "9", "9", &ui_font_32, false, false, ctx);
    add_key(keys, 3, 1, "×", "×", &ui_font_32, false, false, ctx);

    add_key(keys, 0, 2, "4", "4", &ui_font_32, false, false, ctx);
    add_key(keys, 1, 2, "5", "5", &ui_font_32, false, false, ctx);
    add_key(keys, 2, 2, "6", "6", &ui_font_32, false, false, ctx);
    add_key(keys, 3, 2, "−", "−", &ui_font_32, false, false, ctx);

    add_key(keys, 0, 3, "1", "1", &ui_font_32, false, false, ctx);
    add_key(keys, 1, 3, "2", "2", &ui_font_32, false, false, ctx);
    add_key(keys, 2, 3, "3", "3", &ui_font_32, false, false, ctx);
    add_key(keys, 3, 3, "+", "+", &ui_font_32, false, false, ctx);

    add_key(keys, 0, 4, "0", "0", &ui_font_32, false, false, ctx);
    add_key(keys, 1, 4, ".", ".", &ui_font_32, false, false, ctx);
    add_key(keys, 2, 4, LV_SYMBOL_BACKSPACE, "BS", &lv_font_montserrat_24, true, false, ctx);
    add_key(keys, 3, 4, "=", "=", &ui_font_32, false, true, ctx);

    calc_refresh(ctx);
    ui_nav_bar(screen, UI_PAGE_CALCULATOR);
    lv_obj_add_event_cb(screen, calc_delete_cb, LV_EVENT_DELETE, ctx);
    return screen;
}
