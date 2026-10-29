#include "panel_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <mmsystem.h>

#include "fake_clock.h"
#include "nvs.h"

static int failures;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static int64_t make_epoch(int y, int mo, int d, int h, int mi, int s)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    t.tm_isdst = -1;
    return (int64_t)mktime(&t);
}

static void wait_ms(unsigned ms) { Sleep(ms); }

static panel_clock_snapshot_t snapshot(void)
{
    panel_clock_snapshot_t s;
    panel_clock_get_snapshot(&s);
    return s;
}

static bool wait_valid(bool valid, unsigned timeout_ms)
{
    for (unsigned i = 0; i < timeout_ms; i += 20) {
        if (snapshot().valid == valid) return true;
        wait_ms(20);
    }
    return snapshot().valid == valid;
}

static bool wait_alarm(bool pending, unsigned timeout_ms)
{
    for (unsigned i = 0; i < timeout_ms; i += 20) {
        if (snapshot().alarm_pending == pending) return true;
        wait_ms(20);
    }
    return snapshot().alarm_pending == pending;
}

static void test_basic(void)
{
    fake_clock_reset();
    fake_clock_set_calibrated(false);
    CHECK(panel_clock_init() == ESP_OK);
    wait_valid(false, 300);
    CHECK(!snapshot().valid);
    CHECK(!snapshot().alarm_enabled);

    fake_clock_set_epoch(make_epoch(2026, 10, 24, 6, 0, 0));
    fake_clock_set_calibrated(true);
    wait_valid(true, 500);
    CHECK(snapshot().year == 2026 && snapshot().month == 10 && snapshot().day == 24);

    CHECK(panel_clock_alarm_set(true, 7, 30) == ESP_OK);
    wait_ms(200);
    CHECK(snapshot().alarm_enabled);
    CHECK(fake_clock_nvs_saved());
    CHECK(fake_clock_nvs_writes() == 1);

    fake_clock_set_epoch(make_epoch(2026, 10, 24, 7, 29, 50));
    wait_ms(200);
    fake_clock_set_epoch(make_epoch(2026, 10, 24, 7, 30, 0));
    CHECK(wait_alarm(true, 700));
    uint32_t gen = snapshot().alarm_generation;
    CHECK(gen > 0);
    CHECK(panel_clock_alarm_ack(gen) == ESP_OK);
    CHECK(wait_alarm(false, 600));

    fake_clock_set_epoch(make_epoch(2026, 10, 24, 7, 30, 30));
    wait_ms(300);
    CHECK(!snapshot().alarm_pending);
    fake_clock_set_epoch(make_epoch(2026, 10, 24, 8, 0, 0));
    wait_ms(250);
    fake_clock_set_epoch(make_epoch(2026, 10, 24, 7, 30, 0));
    wait_ms(300);
    CHECK(!snapshot().alarm_pending); /* same local date */

    fake_clock_set_epoch(make_epoch(2026, 10, 25, 7, 29, 50));
    wait_ms(250);
    fake_clock_set_epoch(make_epoch(2026, 10, 25, 7, 30, 0));
    CHECK(wait_alarm(true, 700));
    gen = snapshot().alarm_generation;
    CHECK(panel_clock_alarm_ack(gen) == ESP_OK);
    CHECK(wait_alarm(false, 600));

    /* First calibration exactly on the target minute must not synthesize a
     * boundary; only a real previous minute may trigger. */
    CHECK(panel_clock_alarm_set(true, 9, 0) == ESP_OK);
    fake_clock_set_calibrated(false);
    fake_clock_set_epoch(make_epoch(2026, 10, 25, 9, 0, 0));
    wait_ms(250);
    fake_clock_set_calibrated(true);
    wait_ms(400);
    CHECK(!snapshot().alarm_pending);

    fake_clock_set_calibrated(false);
    fake_clock_set_epoch(make_epoch(2026, 10, 26, 7, 30, 0));
    wait_ms(400);
    CHECK(!snapshot().alarm_pending);
    CHECK(!snapshot().valid);

    fake_clock_nvs_set_fail(true);
    CHECK(panel_clock_alarm_set(false, 8, 0) == ESP_OK);
    wait_ms(250);
    CHECK(snapshot().alarm_persist_failed);
    unsigned writes_before = fake_clock_nvs_writes();
    fake_clock_nvs_set_fail(false);
    CHECK(panel_clock_alarm_set(false, 8, 0) == ESP_OK); /* same config retries */
    wait_ms(250);
    CHECK(!snapshot().alarm_persist_failed);
    CHECK(fake_clock_nvs_writes() > writes_before);
}

static void test_stopwatch_countdown(void)
{
    fake_clock_reset();
    fake_clock_set_calibrated(true);
    fake_clock_set_epoch(make_epoch(2026, 10, 24, 12, 0, 0));
    CHECK(panel_clock_init() == ESP_OK);
    wait_ms(200);

    CHECK(panel_clock_stopwatch_start() == ESP_OK);
    wait_ms(150);
    fake_clock_advance_monotonic_us(1500000);
    wait_ms(200);
    panel_clock_snapshot_t s = snapshot();
    CHECK(s.stopwatch_running);
    CHECK(s.stopwatch_ms >= 1500);
    CHECK(panel_clock_stopwatch_pause() == ESP_OK);
    wait_ms(200);
    uint32_t held = snapshot().stopwatch_ms;
    fake_clock_advance_monotonic_us(1000000);
    wait_ms(200);
    CHECK(snapshot().stopwatch_ms == held);
    CHECK(panel_clock_stopwatch_start() == ESP_OK);
    wait_ms(100);
    fake_clock_advance_monotonic_us(5000000000000LL);
    wait_ms(250);
    CHECK(snapshot().stopwatch_ms == UINT32_MAX);
    CHECK(panel_clock_stopwatch_pause() == ESP_OK);
    wait_ms(200);
    CHECK(snapshot().stopwatch_ms == UINT32_MAX);
    CHECK(panel_clock_stopwatch_reset() == ESP_OK);
    wait_ms(150);
    CHECK(snapshot().stopwatch_ms == 0);

    CHECK(panel_clock_countdown_start(2) == ESP_OK);
    wait_ms(150);
    fake_clock_advance_monotonic_us(1000000);
    wait_ms(200);
    s = snapshot();
    CHECK(s.countdown_running);
    CHECK(s.countdown_remaining_ms > 0 && s.countdown_remaining_ms <= 1000);
    CHECK(panel_clock_countdown_pause() == ESP_OK);
    wait_ms(150);
    uint32_t paused = snapshot().countdown_remaining_ms;
    fake_clock_advance_monotonic_us(500000);
    wait_ms(200);
    CHECK(snapshot().countdown_remaining_ms == paused);
    CHECK(panel_clock_countdown_resume() == ESP_OK);
    wait_ms(150);
    fake_clock_advance_monotonic_us(1500000);
    wait_ms(250);
    s = snapshot();
    CHECK(!s.countdown_running);
    CHECK(s.countdown_remaining_ms == 0);
    CHECK(s.countdown_done);
    uint32_t done_gen = s.countdown_done_generation;
    CHECK(panel_clock_countdown_ack(done_gen) == ESP_OK);
    wait_ms(200);
    CHECK(!snapshot().countdown_done);
    CHECK(panel_clock_countdown_start(360000) == ESP_ERR_INVALID_ARG);

    int rejected = 0;
    for (int i = 0; i < 100; i++) {
        if (panel_clock_stopwatch_pause() == ESP_ERR_TIMEOUT) {
            rejected++;
        }
    }
    CHECK(rejected > 0);
}

static void test_persisted_alarm(void)
{
    struct {
        uint32_t magic;
        uint16_t version;
        uint8_t enabled;
        uint8_t hour;
        uint8_t minute;
        uint8_t reserved[3];
    } blob = {0x50434C4BUL, 1, 1, 6, 15, {0}};
    fake_clock_reset();
    fake_clock_nvs_set_raw(&blob, sizeof(blob));
    fake_clock_set_calibrated(true);
    fake_clock_set_epoch(make_epoch(2026, 10, 24, 6, 0, 0));
    CHECK(panel_clock_init() == ESP_OK);
    wait_ms(200);
    panel_clock_snapshot_t s = snapshot();
    CHECK(s.alarm_enabled);
    CHECK(s.alarm_hour == 6 && s.alarm_minute == 15);
}

static void test_bad_alarm(void)
{
    struct {
        uint32_t magic;
        uint16_t version;
        uint8_t enabled;
        uint8_t hour;
        uint8_t minute;
        uint8_t reserved[3];
    } blob = {0x50434C4BUL, 1, 1, 99, 99, {0}};
    fake_clock_reset();
    fake_clock_nvs_set_raw(&blob, sizeof(blob));
    fake_clock_set_calibrated(true);
    fake_clock_set_epoch(make_epoch(2026, 10, 24, 6, 0, 0));
    CHECK(panel_clock_init() == ESP_OK);
    wait_ms(200);
    CHECK(!snapshot().alarm_enabled);
}

int main(int argc, char **argv)
{
    _putenv("TZ=CST-8");
    _tzset();
    timeBeginPeriod(1);
    if (argc > 1 && strcmp(argv[1], "persist") == 0) {
        test_persisted_alarm();
    } else if (argc > 1 && strcmp(argv[1], "bad") == 0) {
        test_bad_alarm();
    } else {
        test_basic();
        test_stopwatch_countdown();
    }
    printf("B2_CLOCK_TEST failures=%d\n", failures);
    return failures ? 1 : 0;
}
