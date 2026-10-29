#include "panel_calc.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CALC_ENTRY_DIGIT_LIMIT 14
#define CALC_VALUE_LIMIT 1.0e15

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    size_t length = strlen(src);
    if (length >= dst_len) {
        length = dst_len - 1;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static const char *operator_text(char op)
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

static void format_value(double value, char *out, size_t out_len)
{
    if (!isfinite(value) || fabs(value) >= CALC_VALUE_LIMIT) {
        copy_text(out, out_len, "0");
        return;
    }
    if (value == 0.0) {
        copy_text(out, out_len, "0");
        return;
    }
    if (fabs(value) < 1.0e12 && value == trunc(value)) {
        snprintf(out, out_len, "%.0f", value);
    } else {
        snprintf(out, out_len, "%.10g", value);
    }
}

static void set_error(calc_engine_t *calc, calc_status_t status)
{
    calc->error = true;
    calc->status = status;
    calc->pending_op = 0;
    calc->entering = false;
    copy_text(calc->display, sizeof(calc->display), calc_status_text(status));
}

static void refresh_display(calc_engine_t *calc)
{
    if (calc->error) {
        return;
    }
    copy_text(calc->display, sizeof(calc->display), calc->entry);
}

static double entry_value(const calc_engine_t *calc)
{
    return strtod(calc->entry, NULL);
}

static bool apply_operator(double lhs, double rhs, char op, double *result)
{
    switch (op) {
    case '+':
        *result = lhs + rhs;
        break;
    case '-':
        *result = lhs - rhs;
        break;
    case '*':
        *result = lhs * rhs;
        break;
    case '/':
        if (fabs(rhs) < 1.0e-15) {
            return false;
        }
        *result = lhs / rhs;
        break;
    default:
        return false;
    }
    return isfinite(*result) && fabs(*result) < CALC_VALUE_LIMIT;
}

static void push_history(calc_engine_t *calc, double lhs, char op, double rhs, double result)
{
    char line[CALC_HISTORY_LEN];
    char lhs_text[CALC_DISPLAY_MAX];
    char rhs_text[CALC_DISPLAY_MAX];
    char result_text[CALC_DISPLAY_MAX];

    format_value(lhs, lhs_text, sizeof(lhs_text));
    format_value(rhs, rhs_text, sizeof(rhs_text));
    format_value(result, result_text, sizeof(result_text));
    snprintf(line, sizeof(line), "%s %s %s = %s", lhs_text, operator_text(op), rhs_text,
             result_text);

    if (calc->history_count < CALC_HISTORY_MAX) {
        copy_text(calc->history[calc->history_count], CALC_HISTORY_LEN, line);
        calc->history_count++;
    } else {
        for (uint8_t i = 1; i < CALC_HISTORY_MAX; i++) {
            copy_text(calc->history[i - 1], CALC_HISTORY_LEN, calc->history[i]);
        }
        copy_text(calc->history[CALC_HISTORY_MAX - 1], CALC_HISTORY_LEN, line);
    }
}

void calc_init(calc_engine_t *calc)
{
    if (calc == NULL) {
        return;
    }
    memset(calc, 0, sizeof(*calc));
    calc->status = CALC_OK;
    copy_text(calc->entry, sizeof(calc->entry), "0");
    copy_text(calc->display, sizeof(calc->display), "0");
}

void calc_clear(calc_engine_t *calc)
{
    if (calc == NULL) {
        return;
    }
    calc->accumulator = 0.0;
    calc->current = 0.0;
    calc->pending_op = 0;
    calc->entering = false;
    calc->error = false;
    calc->status = CALC_OK;
    copy_text(calc->entry, sizeof(calc->entry), "0");
    copy_text(calc->display, sizeof(calc->display), "0");
}

void calc_clear_history(calc_engine_t *calc)
{
    if (calc == NULL) {
        return;
    }
    memset(calc->history, 0, sizeof(calc->history));
    calc->history_count = 0;
}

void calc_input_digit(calc_engine_t *calc, char digit)
{
    if (calc == NULL || calc->error || digit < '0' || digit > '9') {
        return;
    }

    size_t digits = 0;
    for (const char *p = calc->entry; *p != '\0'; p++) {
        if (isdigit((unsigned char)*p)) {
            digits++;
        }
    }
    if (!calc->entering) {
        copy_text(calc->entry, sizeof(calc->entry), "0");
        calc->entering = true;
        digits = 0;
    }
    if (digits >= CALC_ENTRY_DIGIT_LIMIT) {
        set_error(calc, CALC_ERR_INPUT_LONG);
        return;
    }

    if (strcmp(calc->entry, "0") == 0) {
        calc->entry[0] = digit;
        calc->entry[1] = '\0';
    } else if (strcmp(calc->entry, "-0") == 0) {
        calc->entry[1] = digit;
        calc->entry[2] = '\0';
    } else {
        size_t len = strlen(calc->entry);
        if (len + 1 < sizeof(calc->entry)) {
            calc->entry[len] = digit;
            calc->entry[len + 1] = '\0';
        }
    }
    calc->current = entry_value(calc);
    refresh_display(calc);
}

void calc_input_dot(calc_engine_t *calc)
{
    if (calc == NULL || calc->error) {
        return;
    }
    if (!calc->entering) {
        copy_text(calc->entry, sizeof(calc->entry), "0.");
        calc->entering = true;
    } else if (strchr(calc->entry, '.') == NULL) {
        size_t len = strlen(calc->entry);
        if (len + 1 < sizeof(calc->entry)) {
            calc->entry[len] = '.';
            calc->entry[len + 1] = '\0';
        }
    }
    calc->current = entry_value(calc);
    refresh_display(calc);
}

void calc_backspace(calc_engine_t *calc)
{
    if (calc == NULL || calc->error) {
        return;
    }
    if (!calc->entering) {
        copy_text(calc->entry, sizeof(calc->entry), "0");
        calc->entering = true;
    } else {
        size_t len = strlen(calc->entry);
        if (len > 0) {
            calc->entry[len - 1] = '\0';
        }
        if (calc->entry[0] == '\0' || strcmp(calc->entry, "-") == 0) {
            copy_text(calc->entry, sizeof(calc->entry), "0");
        }
    }
    calc->current = entry_value(calc);
    refresh_display(calc);
}

void calc_toggle_sign(calc_engine_t *calc)
{
    if (calc == NULL || calc->error) {
        return;
    }
    if (!calc->entering) {
        format_value(calc->current, calc->entry, sizeof(calc->entry));
        calc->entering = true;
    }
    if (calc->entry[0] == '-') {
        memmove(calc->entry, calc->entry + 1, strlen(calc->entry));
    } else if (strcmp(calc->entry, "0") != 0) {
        size_t len = strlen(calc->entry);
        if (len + 2 < sizeof(calc->entry)) {
            memmove(calc->entry + 1, calc->entry, len + 1);
            calc->entry[0] = '-';
        }
    }
    calc->current = entry_value(calc);
    refresh_display(calc);
}

void calc_percent(calc_engine_t *calc)
{
    if (calc == NULL || calc->error) {
        return;
    }
    calc->current = calc->current / 100.0;
    format_value(calc->current, calc->entry, sizeof(calc->entry));
    calc->entering = true;
    refresh_display(calc);
}

void calc_set_operator(calc_engine_t *calc, char op)
{
    if (calc == NULL || calc->error) {
        return;
    }
    if (op != '+' && op != '-' && op != '*' && op != '/') {
        return;
    }

    if (calc->pending_op != 0 && calc->entering) {
        double result = 0.0;
        double lhs = calc->accumulator;
        double rhs = calc->current;
        if (!apply_operator(lhs, rhs, calc->pending_op, &result)) {
            set_error(calc, calc->pending_op == '/' && fabs(rhs) < 1.0e-15
                                ? CALC_ERR_DIV_ZERO
                                : CALC_ERR_OVERFLOW);
            return;
        }
        push_history(calc, lhs, calc->pending_op, rhs, result);
        calc->accumulator = result;
        calc->current = result;
        format_value(result, calc->entry, sizeof(calc->entry));
    } else {
        calc->accumulator = calc->current;
    }

    calc->pending_op = op;
    calc->entering = false;
    refresh_display(calc);
}

void calc_equals(calc_engine_t *calc)
{
    if (calc == NULL || calc->error) {
        return;
    }
    if (calc->pending_op == 0) {
        format_value(calc->current, calc->entry, sizeof(calc->entry));
        calc->entering = false;
        refresh_display(calc);
        return;
    }

    double lhs = calc->accumulator;
    double rhs = calc->entering ? calc->current : calc->accumulator;
    double result = 0.0;
    char op = calc->pending_op;
    if (!apply_operator(lhs, rhs, op, &result)) {
        set_error(calc, op == '/' && fabs(rhs) < 1.0e-15 ? CALC_ERR_DIV_ZERO
                                                         : CALC_ERR_OVERFLOW);
        return;
    }

    push_history(calc, lhs, op, rhs, result);
    calc->accumulator = result;
    calc->current = result;
    calc->pending_op = 0;
    calc->entering = false;
    format_value(result, calc->entry, sizeof(calc->entry));
    refresh_display(calc);
}

const char *calc_status_text(calc_status_t status)
{
    switch (status) {
    case CALC_OK:
        return "正常";
    case CALC_ERR_DIV_ZERO:
        return "除零错误";
    case CALC_ERR_OVERFLOW:
        return "数值溢出";
    case CALC_ERR_INPUT_LONG:
        return "输入过长";
    default:
        return "错误";
    }
}

bool calc_is_error(const calc_engine_t *calc)
{
    return calc != NULL && calc->error;
}
