#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel_calc.h"
#include "panel_settings.h"

static int failures;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

#define CHECK_NEAR(actual, expected, eps)                                                 \
    do {                                                                                  \
        double _a = (actual);                                                             \
        double _e = (expected);                                                           \
        if (fabs(_a - _e) > (eps)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %f != %f\n", __FILE__, __LINE__, _a, _e);       \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static void enter_digits(calc_engine_t *calc, const char *digits)
{
    for (const char *p = digits; *p; p++) {
        if (*p == '.') {
            calc_input_dot(calc);
        } else {
            calc_input_digit(calc, *p);
        }
    }
}

static void test_settings(void)
{
    panel_settings_t settings;
    panel_settings_set_defaults(&settings);
    CHECK(panel_settings_validate(&settings));
    CHECK(settings.brightness == 70);
    CHECK(settings.theme == PANEL_THEME_LIGHT);
    CHECK(settings.sleep_after_s == 60);
    CHECK(!settings.auto_brightness);
    CHECK(settings.animations);

    settings.brightness = 0;
    CHECK(!panel_settings_validate(&settings));
    settings.brightness = 101;
    CHECK(!panel_settings_validate(&settings));
    settings.brightness = 12;
    CHECK(panel_settings_validate(&settings));

    settings.auto_brightness = true;
    CHECK(!panel_settings_validate(&settings));
    settings.auto_brightness = false;

    settings.sleep_after_s = 45;
    CHECK(!panel_settings_validate(&settings));
    settings.sleep_after_s = 0;
    CHECK(panel_settings_validate(&settings));
    settings.sleep_after_s = 300;
    CHECK(panel_settings_validate(&settings));

    settings.theme = (panel_theme_t)99;
    CHECK(!panel_settings_validate(&settings));
}

static void test_settings_animation_migration(void)
{
    panel_settings_t settings;
    panel_settings_migration_t result;
    uint32_t flags = 0;

    panel_settings_set_defaults(&settings);
    settings.animations = false;
    CHECK(panel_settings_migrate(&settings, PANEL_SETTINGS_SCHEMA_V1, &flags, &result));
    CHECK(settings.animations);
    CHECK(result.changed);
    CHECK(result.animations_forced_on);
    CHECK(result.from_version == PANEL_SETTINGS_SCHEMA_V1);
    CHECK(result.to_version == PANEL_SETTINGS_SCHEMA_V2);
    CHECK((flags & PANEL_SETTINGS_FLAG_ANIMATIONS_MIGRATED) != 0);

    /* A v2 preference of "off" is a real user choice and must stay off. */
    settings.animations = false;
    flags = PANEL_SETTINGS_FLAG_ANIMATIONS_MIGRATED;
    CHECK(panel_settings_migrate(&settings, PANEL_SETTINGS_SCHEMA_V2, &flags, &result));
    CHECK(!settings.animations);
    CHECK(!result.changed);
    CHECK(!result.animations_forced_on);
    CHECK(result.to_version == PANEL_SETTINGS_SCHEMA_V2);

    /* A v1 preference of "on" is upgraded once without an artificial force. */
    panel_settings_set_defaults(&settings);
    flags = 0;
    CHECK(panel_settings_migrate(&settings, PANEL_SETTINGS_SCHEMA_V1, &flags, &result));
    CHECK(settings.animations);
    CHECK(result.changed);
    CHECK(!result.animations_forced_on);

    flags = 0;
    CHECK(!panel_settings_migrate(&settings, 99, &flags, &result));
}

static void test_continuous_calculation(void)
{
    calc_engine_t calc;
    calc_init(&calc);
    enter_digits(&calc, "128");
    calc_set_operator(&calc, '*');
    enter_digits(&calc, "12");
    calc_set_operator(&calc, '/');
    CHECK(!calc.error);
    CHECK_NEAR(calc.current, 1536.0, 1e-9);
    CHECK(calc.history_count == 1);
    CHECK(strcmp(calc.history[0], "128 × 12 = 1536") == 0);

    enter_digits(&calc, "4");
    calc_set_operator(&calc, '*');
    CHECK_NEAR(calc.current, 384.0, 1e-9);
    CHECK(calc.history_count == 2);

    enter_digits(&calc, "3.5");
    calc_equals(&calc);
    CHECK(!calc.error);
    CHECK_NEAR(calc.current, 1344.0, 1e-9);
    CHECK(strcmp(calc.display, "1344") == 0);
    CHECK(calc.history_count == 3);
    CHECK(strcmp(calc.history[2], "384 × 3.5 = 1344") == 0);

    calc_clear_history(&calc);
    CHECK(calc.history_count == 0);
}

static void test_decimal_and_sign(void)
{
    calc_engine_t calc;
    calc_init(&calc);
    enter_digits(&calc, "0.1");
    calc_set_operator(&calc, '+');
    enter_digits(&calc, "0.2");
    calc_equals(&calc);
    CHECK(!calc.error);
    CHECK_NEAR(calc.current, 0.3, 1e-9);

    calc_clear(&calc);
    enter_digits(&calc, "42");
    calc_toggle_sign(&calc);
    CHECK_NEAR(calc.current, -42.0, 1e-9);
    calc_toggle_sign(&calc);
    CHECK_NEAR(calc.current, 42.0, 1e-9);

    calc_clear(&calc);
    enter_digits(&calc, "50");
    calc_percent(&calc);
    CHECK_NEAR(calc.current, 0.5, 1e-9);

    calc_clear(&calc);
    enter_digits(&calc, "123");
    calc_backspace(&calc);
    CHECK_NEAR(calc.current, 12.0, 1e-9);
    calc_backspace(&calc);
    calc_backspace(&calc);
    CHECK(strcmp(calc.display, "0") == 0);
}

static void test_errors(void)
{
    calc_engine_t calc;
    calc_init(&calc);
    enter_digits(&calc, "8");
    calc_set_operator(&calc, '/');
    enter_digits(&calc, "0");
    calc_equals(&calc);
    CHECK(calc.error);
    CHECK(calc.status == CALC_ERR_DIV_ZERO);
    CHECK(strcmp(calc_status_text(calc.status), "除零错误") == 0);
    CHECK(strcmp(calc.display, "除零错误") == 0);

    /* Error state is stable until C is pressed. */
    calc_input_digit(&calc, '7');
    calc_equals(&calc);
    CHECK(calc.error);
    calc_clear(&calc);
    CHECK(!calc.error);
    CHECK(strcmp(calc.display, "0") == 0);
    CHECK(calc.status == CALC_OK);

    calc_init(&calc);
    enter_digits(&calc, "99999999999999");
    calc_input_digit(&calc, '9');
    CHECK(calc.error);
    CHECK(calc.status == CALC_ERR_INPUT_LONG);
    calc_clear(&calc);
    CHECK(!calc.error);

    calc_init(&calc);
    enter_digits(&calc, "999999999999");
    calc_set_operator(&calc, '*');
    enter_digits(&calc, "999999999999");
    calc_equals(&calc);
    CHECK(calc.error);
    CHECK(calc.status == CALC_ERR_OVERFLOW);
    calc_clear(&calc);
    CHECK(!calc.error);
}

static void test_operator_without_rhs(void)
{
    calc_engine_t calc;
    calc_init(&calc);
    enter_digits(&calc, "5");
    calc_set_operator(&calc, '+');
    calc_equals(&calc);
    CHECK(!calc.error);
    CHECK_NEAR(calc.current, 10.0, 1e-9);
    CHECK(calc.history_count == 1);
}

int main(void)
{
    test_settings();
    test_settings_animation_migration();
    test_continuous_calculation();
    test_decimal_and_sign();
    test_errors();
    test_operator_without_rhs();

    if (failures == 0) {
        printf("PASS: all panel_model tests passed\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", failures);
    return 1;
}
