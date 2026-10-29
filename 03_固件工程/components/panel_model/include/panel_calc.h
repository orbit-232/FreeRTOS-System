#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CALC_DISPLAY_MAX 24
#define CALC_HISTORY_MAX 3
#define CALC_HISTORY_LEN 96

typedef enum {
    CALC_OK = 0,
    CALC_ERR_DIV_ZERO,
    CALC_ERR_OVERFLOW,
    CALC_ERR_INPUT_LONG,
} calc_status_t;

typedef struct {
    double accumulator;
    double current;
    char pending_op; /* '+', '-', '*', '/', or 0 */
    bool entering;   /* current is being typed */
    bool error;
    calc_status_t status;
    char entry[CALC_DISPLAY_MAX];
    char display[CALC_DISPLAY_MAX];
    char history[CALC_HISTORY_MAX][CALC_HISTORY_LEN];
    uint8_t history_count;
} calc_engine_t;

void calc_init(calc_engine_t *calc);
void calc_clear(calc_engine_t *calc);
void calc_clear_history(calc_engine_t *calc);
void calc_input_digit(calc_engine_t *calc, char digit);
void calc_input_dot(calc_engine_t *calc);
void calc_backspace(calc_engine_t *calc);
void calc_toggle_sign(calc_engine_t *calc);
void calc_percent(calc_engine_t *calc);
void calc_set_operator(calc_engine_t *calc, char op);
void calc_equals(calc_engine_t *calc);

const char *calc_status_text(calc_status_t status);
bool calc_is_error(const calc_engine_t *calc);

#ifdef __cplusplus
}
#endif
