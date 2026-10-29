#include "panel_online.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "fake_online.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int failures;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static void wait_ms(unsigned ms) { Sleep(ms); }

static bool wait_http_calls(int min_calls, unsigned timeout_ms)
{
    for (unsigned i = 0; i < timeout_ms; i += 10) {
        if (fake_online_http_calls() >= min_calls) return true;
        Sleep(10);
    }
    return fake_online_http_calls() >= min_calls;
}

static bool wait_weather(bool has_data, unsigned timeout_ms)
{
    for (unsigned i = 0; i < timeout_ms; i += 10) {
        panel_weather_snapshot_t snapshot;
        panel_online_get_weather(&snapshot);
        if (snapshot.has_data == has_data) return true;
        Sleep(10);
    }
    panel_weather_snapshot_t snapshot;
    panel_online_get_weather(&snapshot);
    return snapshot.has_data == has_data;
}

static bool wait_search_count(unsigned count, unsigned timeout_ms)
{
    for (unsigned i = 0; i < timeout_ms; i += 10) {
        panel_city_search_snapshot_t snapshot;
        panel_online_get_city_search(&snapshot);
        if (snapshot.count == count) return true;
        Sleep(10);
    }
    return false;
}

static bool start_online(bool wifi)
{
    fake_online_reset();
    fake_online_set_wifi(wifi);
    if (panel_online_init() != ESP_OK) {
        printf("FAIL: panel_online_init\n");
        return false;
    }
    wait_ms(250);
    return true;
}

static bool sync_clock(void)
{
    if (fake_online_sntp_starts() == 0) {
        wait_ms(500);
    }
    if (fake_online_sntp_starts() == 0) {
        return false;
    }
    fake_online_fire_sntp_sync();
    wait_ms(100);
    return true;
}

static void build_forecast_now(char *buf, size_t cap)
{
    time_t now = time(NULL);
    time_t shifted = now + 8 * 3600;
    struct tm local = *gmtime(&shifted);
    char current[24];
    strftime(current, sizeof(current), "%Y-%m-%dT%H:%M", &local);

    char hours[6][24];
    int temps[6];
    int codes[6];
    time_t hour_base = shifted - (shifted % 3600);
    for (int i = 0; i < 6; i++) {
        time_t t = hour_base + i * 3600;
        struct tm tm_hour = *gmtime(&t);
        strftime(hours[i], sizeof(hours[i]), "%Y-%m-%dT%H:%M", &tm_hour);
        temps[i] = 20 + i;
        codes[i] = i == 0 ? 2 : 3;
    }
    char days[5][16];
    char day_max[5][12];
    char day_min[5][12];
    char day_code[5][12];
    for (int i = 0; i < 5; i++) {
        time_t t = shifted + i * 86400;
        struct tm tm_day = *gmtime(&t);
        strftime(days[i], sizeof(days[i]), "%Y-%m-%d", &tm_day);
        snprintf(day_max[i], sizeof(day_max[i]), "%d.2", 30 + i);
        snprintf(day_min[i], sizeof(day_min[i]), "%d.1", 20 + i);
        snprintf(day_code[i], sizeof(day_code[i]), "%d", 2 + (i % 3));
    }
    snprintf(buf, cap,
             "{\"timezone\":\"Asia/Shanghai\","
             "\"current_units\":{\"temperature_2m\":\"°C\",\"relative_humidity_2m\":\"%%\","
             "\"wind_speed_10m\":\"km/h\"},"
             "\"current\":{\"time\":\"%s\",\"temperature_2m\":29.1,"
             "\"relative_humidity_2m\":47,\"apparent_temperature\":30.1,"
             "\"weather_code\":2,\"wind_speed_10m\":9.4},"
             "\"hourly\":{\"time\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"],"
             "\"temperature_2m\":[%d,%d,%d,%d,%d,%d],"
             "\"weather_code\":[%d,%d,%d,%d,%d,%d]},"
             "\"daily\":{\"time\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"],"
             "\"temperature_2m_max\":[%s,%s,%s,%s,%s],"
             "\"temperature_2m_min\":[%s,%s,%s,%s,%s],"
             "\"weather_code\":[%s,%s,%s,%s,%s]}}",
             current, hours[0], hours[1], hours[2], hours[3], hours[4], hours[5],
             temps[0], temps[1], temps[2], temps[3], temps[4], temps[5],
             codes[0], codes[1], codes[2], codes[3], codes[4], codes[5],
             days[0], days[1], days[2], days[3], days[4],
             day_max[0], day_max[1], day_max[2], day_max[3], day_max[4],
             day_min[0], day_min[1], day_min[2], day_min[3], day_min[4],
             day_code[0], day_code[1], day_code[2], day_code[3], day_code[4]);
}

static const char *geocode_two =
    "{\"results\":["
    "{\"name\":\"Guangzhou\",\"latitude\":23.11667,\"longitude\":113.25,"
    "\"country\":\"China\",\"admin1\":\"Guangdong\"},"
    "{\"name\":\"Shenzhen\",\"latitude\":22.54554,\"longitude\":114.0683,"
    "\"country\":\"China\",\"admin1\":\"Guangdong\"}]}";

static bool start_with_forecast(void)
{
    if (!start_online(true)) return false;
    if (!sync_clock()) return false;
    char forecast[4096];
    build_forecast_now(forecast, sizeof(forecast));
    fake_online_http_set_forecast(200, "application/json; charset=utf-8", forecast,
                                  strlen(forecast));
    CHECK(panel_online_refresh() == ESP_OK);
    return wait_weather(true, 2500);
}

static int scenario_offline(void)
{
    if (!start_online(false)) return 1;
    wait_ms(1200);
    panel_weather_snapshot_t weather;
    panel_online_get_weather(&weather);
    CHECK(!weather.wifi_connected);
    CHECK(!weather.has_data);
    CHECK(strstr(weather.reason, "等待网络") != NULL);
    CHECK(fake_online_http_calls() == 0);
    CHECK(fake_online_sntp_starts() == 0);
    return 0;
}

static int scenario_offline_search_refresh(void)
{
    if (!start_online(false)) return 1;
    CHECK(panel_online_search_city("Shenzhen") == ESP_ERR_INVALID_STATE);
    CHECK(panel_online_refresh() == ESP_OK);
    wait_ms(300);
    CHECK(fake_online_http_calls() == 0);
    panel_city_search_snapshot_t search;
    panel_online_get_city_search(&search);
    CHECK(!search.searching);
    CHECK(strstr(search.error, "等待网络") != NULL);
    return 0;
}

static int scenario_sync_success(void)
{
    CHECK(start_with_forecast());
    panel_weather_snapshot_t weather;
    panel_online_get_weather(&weather);
    CHECK(weather.has_data);
    CHECK(weather.current_temp_c == 29);
    CHECK(weather.humidity_pct == 47);
    CHECK(weather.hour_count == 6);
    CHECK(weather.day_count == 5);
    CHECK(strcmp(weather.city, "广州") == 0);
    CHECK(weather.updated_epoch > 0);
    CHECK(weather.data_epoch > 0);
    CHECK(fake_online_sntp_starts() == 1);
    CHECK(strstr(fake_online_last_url(), "api.open-meteo.com") != NULL);
    CHECK(strstr(fake_online_last_url(), "timezone=Asia%2FShanghai") != NULL);
    return 0;
}

static int scenario_failure_cache(void)
{
    CHECK(start_with_forecast());
    panel_weather_snapshot_t before;
    panel_online_get_weather(&before);
    int calls = fake_online_http_calls();
    fake_online_http_set_error(ESP_FAIL);
    fake_advance_ms(31000);
    CHECK(panel_online_refresh() == ESP_OK);
    wait_ms(500);
    panel_weather_snapshot_t after;
    panel_online_get_weather(&after);
    CHECK(after.has_data);
    CHECK(after.stale);
    CHECK(after.updated_epoch == before.updated_epoch);
    CHECK(after.current_temp_c == before.current_temp_c);
    CHECK(after.reason[0] != '\0');
    CHECK(fake_online_http_calls() > calls);
    return 0;
}

static int scenario_search_choose(void)
{
    CHECK(start_with_forecast());
    fake_online_http_set_geocode(200, "application/json", geocode_two, strlen(geocode_two));
    CHECK(panel_online_search_city("Shenzhen") == ESP_OK);
    CHECK(wait_search_count(2, 2000));
    panel_city_search_snapshot_t search;
    panel_online_get_city_search(&search);
    uint32_t generation = search.generation;
    panel_weather_snapshot_t before;
    panel_online_get_weather(&before);

    CHECK(panel_online_choose_city(0, generation - 1) == ESP_ERR_INVALID_STATE);
    panel_online_get_weather(&before);
    CHECK(strcmp(before.city, "广州") == 0);

    CHECK(panel_online_choose_city(1, generation) == ESP_OK);
    wait_ms(300);
    panel_online_get_weather(&before);
    CHECK(strcmp(before.city, "Shenzhen") == 0);
    CHECK(fake_online_nvs_has_city());
    return 0;
}

static int scenario_queue_full_transaction(void)
{
    CHECK(start_with_forecast());
    fake_online_http_set_geocode(200, "application/json", geocode_two, strlen(geocode_two));
    CHECK(panel_online_search_city("Shenzhen") == ESP_OK);
    CHECK(wait_search_count(2, 2000));
    panel_city_search_snapshot_t search;
    panel_online_get_city_search(&search);
    panel_weather_snapshot_t before;
    panel_online_get_weather(&before);

    fake_online_http_block_next();
    int calls = fake_online_http_calls();
    fake_advance_ms(31000);
    CHECK(panel_online_refresh() == ESP_OK);
    CHECK(wait_http_calls(calls + 1, 1500));
    for (int i = 0; i < 4; i++) {
        fake_advance_ms(31000);
        CHECK(panel_online_refresh() == ESP_OK);
    }
    CHECK(panel_online_choose_city(1, search.generation) == ESP_ERR_TIMEOUT);
    panel_online_get_weather(&before);
    CHECK(strcmp(before.city, "广州") == 0);
    CHECK(before.has_data);
    fake_online_http_release();
    wait_ms(800);
    return 0;
}

static int scenario_rapid_queries(void)
{
    CHECK(start_with_forecast());
    const char *first =
        "{\"results\":[{\"name\":\"Guangzhou\",\"latitude\":23.1,\"longitude\":113.2}]}";
    const char *second =
        "{\"results\":[{\"name\":\"Shenzhen\",\"latitude\":22.5,\"longitude\":114.0}]}";
    fake_online_http_set_geocode(200, "application/json", first, strlen(first));
    fake_online_http_block_next();
    int calls = fake_online_http_calls();
    CHECK(panel_online_search_city("Guangzhou") == ESP_OK);
    CHECK(wait_http_calls(calls + 1, 1500));
    fake_online_http_set_geocode(200, "application/json", second, strlen(second));
    CHECK(panel_online_search_city("Shenzhen") == ESP_OK);
    fake_online_http_release();
    wait_ms(800);
    panel_city_search_snapshot_t search;
    panel_online_get_city_search(&search);
    CHECK(search.count == 1);
    CHECK(strcmp(search.results[0].name, "Shenzhen") == 0);
    return 0;
}

static int scenario_city_change_during_http(void)
{
    CHECK(start_with_forecast());
    fake_online_http_set_geocode(200, "application/json", geocode_two, strlen(geocode_two));
    CHECK(panel_online_search_city("Shenzhen") == ESP_OK);
    CHECK(wait_search_count(2, 2000));
    panel_city_search_snapshot_t search;
    panel_online_get_city_search(&search);

    fake_online_http_block_next();
    int calls = fake_online_http_calls();
    fake_advance_ms(31000);
    CHECK(panel_online_refresh() == ESP_OK);
    CHECK(wait_http_calls(calls + 1, 1500));
    fake_online_http_set_error(ESP_FAIL);
    CHECK(panel_online_choose_city(1, search.generation) == ESP_OK);
    panel_weather_snapshot_t weather;
    panel_online_get_weather(&weather);
    CHECK(strcmp(weather.city, "Shenzhen") == 0);
    CHECK(!weather.has_data);
    fake_online_http_release();
    wait_ms(1000);
    panel_online_get_weather(&weather);
    CHECK(strcmp(weather.city, "Shenzhen") == 0);
    CHECK(!weather.has_data);
    return 0;
}

static int scenario_wifi_no_sync(void)
{
    if (!start_online(true)) return 1;
    wait_ms(350);
    CHECK(fake_online_sntp_starts() == 1);
    CHECK(panel_online_refresh() == ESP_OK);
    CHECK(panel_online_search_city("Shenzhen") == ESP_ERR_INVALID_STATE);
    wait_ms(300);
    CHECK(fake_online_http_calls() == 0);
    panel_weather_snapshot_t weather;
    panel_online_get_weather(&weather);
    CHECK(!weather.calibrated);
    CHECK(strstr(weather.reason, "等待校时") != NULL);
    panel_city_search_snapshot_t search;
    panel_online_get_city_search(&search);
    CHECK(strstr(search.error, "等待校时") != NULL);
    return 0;
}

static int scenario_search_queue_full(void)
{
    CHECK(start_with_forecast());
    fake_online_http_set_geocode(200, "application/json", geocode_two, strlen(geocode_two));
    CHECK(panel_online_search_city("Shenzhen") == ESP_OK);
    CHECK(wait_search_count(2, 2000));
    panel_city_search_snapshot_t before;
    panel_online_get_city_search(&before);

    fake_online_http_block_next();
    int calls = fake_online_http_calls();
    fake_advance_ms(31000);
    CHECK(panel_online_refresh() == ESP_OK);
    CHECK(wait_http_calls(calls + 1, 1500));
    for (int i = 0; i < 4; i++) {
        fake_advance_ms(31000);
        CHECK(panel_online_refresh() == ESP_OK);
    }
    CHECK(panel_online_search_city("Beijing") == ESP_ERR_TIMEOUT);
    panel_city_search_snapshot_t after;
    panel_online_get_city_search(&after);
    CHECK(!after.searching);
    CHECK(after.count == before.count);
    CHECK(after.generation == before.generation);
    fake_online_http_release();
    wait_ms(800);
    return 0;
}

static int scenario_sntp_retry_server(void)
{
    if (!start_online(true)) return 1;
    wait_ms(300);
    CHECK(fake_online_sntp_starts() == 1);
    CHECK(strcmp(fake_online_last_ntp_server(), "ntp.aliyun.com") == 0);
    fake_advance_ms(31000);
    (void)panel_online_refresh();
    wait_ms(1500);
    CHECK(fake_online_sntp_restarts() >= 1);
    CHECK(strcmp(fake_online_last_ntp_server(), "cn.pool.ntp.org") == 0);
    return 0;
}

typedef struct {
    const char *name;
    int (*run)(void);
} scenario_t;

int main(int argc, char **argv)
{
    static const scenario_t scenarios[] = {
        {"offline", scenario_offline},
        {"offline_search_refresh", scenario_offline_search_refresh},
        {"wifi_no_sync", scenario_wifi_no_sync},
        {"sync_success", scenario_sync_success},
        {"failure_cache", scenario_failure_cache},
        {"search_choose", scenario_search_choose},
        {"queue_full_transaction", scenario_queue_full_transaction},
        {"search_queue_full", scenario_search_queue_full},
        {"rapid_queries", scenario_rapid_queries},
        {"city_change_during_http", scenario_city_change_during_http},
        {"sntp_retry_server", scenario_sntp_retry_server},
    };
    if (argc < 2) {
        for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
            printf("%s\n", scenarios[i].name);
        }
        return 2;
    }
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (strcmp(argv[1], scenarios[i].name) == 0) {
            int rc = scenarios[i].run();
            printf("ONLINE_SERVICE_TEST scenario=%s failures=%d\n", argv[1], failures);
            return rc != 0 || failures != 0;
        }
    }
    return 2;
}
