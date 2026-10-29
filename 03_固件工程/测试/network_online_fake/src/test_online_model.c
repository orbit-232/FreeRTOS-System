#include "panel_online_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static char *read_file(const char *path, size_t *len, int extra)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        printf("FAIL: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *data = (char *)malloc((size_t)size + (size_t)extra);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    for (int i = 0; i < extra; i++) {
        data[size + i] = '\0';
    }
    *len = (size_t)size;
    return data;
}

static const char *minimal_forecast =
    "{\"timezone\":\"Asia/Shanghai\","
    "\"current_units\":{\"temperature_2m\":\"°C\",\"relative_humidity_2m\":\"%\","
    "\"wind_speed_10m\":\"km/h\"},"
    "\"current\":{\"time\":\"2026-10-24T18:15\",\"temperature_2m\":29.1,"
    "\"relative_humidity_2m\":47,\"apparent_temperature\":30.1,"
    "\"weather_code\":2,\"wind_speed_10m\":9.4},"
    "\"hourly\":{\"time\":[\"2026-10-24T18:00\",\"2026-10-24T19:00\"],"
    "\"temperature_2m\":[29.0,28.0],\"weather_code\":[2,3]},"
    "\"daily\":{\"time\":[\"2026-10-24\",\"2026-10-25\"],"
    "\"temperature_2m_max\":[31.6,31.5],\"temperature_2m_min\":[21.8,22.1],"
    "\"weather_code\":[3,2]}}";

static char *replace_once(const char *text, const char *from, const char *to)
{
    char *pos = strstr(text, from);
    if (pos == NULL) {
        return NULL;
    }
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    size_t tail = strlen(pos + from_len) + 1;
    char *out = (char *)malloc(strlen(text) - from_len + to_len + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t head = (size_t)(pos - text);
    memcpy(out, text, head);
    memcpy(out + head, to, to_len);
    memcpy(out + head + to_len, pos + from_len, tail);
    return out;
}

static void test_real_fixture(const char *path)
{
    size_t len = 0;
    char *data = read_file(path, &len, 1);
    if (data == NULL) {
        failures++;
        return;
    }
    panel_online_forecast_t forecast;
    char reason[96];
    const int64_t now = 1792837200; /* 2026-10-24 18:20 CST */
    CHECK(panel_online_parse_forecast(data, len, now, &forecast, reason, sizeof(reason)) ==
          ESP_OK);
    CHECK(forecast.current_temp_c == 29);
    CHECK(forecast.apparent_temp_c == 30);
    CHECK(forecast.humidity_pct == 47);
    CHECK(forecast.weather_code == 2);
    CHECK(forecast.kind == PANEL_WEATHER_PARTLY);
    CHECK(forecast.current_epoch == 1792836900);
    CHECK(forecast.hour_count == 6);
    CHECK(forecast.hours[0].hour == 18);
    CHECK(forecast.hours[0].day_offset == 0);
    CHECK(forecast.days[0].day_offset == 0);
    CHECK(forecast.days[0].weekday == 6); /* 2026-10-24 is Saturday */
    CHECK(forecast.days[0].temp_max_c == 32);
    CHECK(forecast.days[0].temp_min_c == 22);
    CHECK(forecast.day_count == 5);

    /* Exact-length buffer without a trailing NUL: the parser must not read
     * the byte at json[len]. */
    char *exact = read_file(path, &len, 0);
    if (exact != NULL) {
        CHECK(panel_online_parse_forecast(exact, len, now, &forecast, reason,
                                          sizeof(reason)) == ESP_OK);
        free(exact);
    } else {
        failures++;
    }

    /* Truncated, garbage tail and embedded NUL must be rejected. */
    CHECK(panel_online_parse_forecast(data, len - 1, now, &forecast, reason,
                                      sizeof(reason)) != ESP_OK);
    char *garbage = (char *)malloc(len + 2);
    if (garbage != NULL) {
        memcpy(garbage, data, len);
        garbage[len] = 'x';
        garbage[len + 1] = '\0';
        CHECK(panel_online_parse_forecast(garbage, len + 1, now, &forecast, reason,
                                          sizeof(reason)) != ESP_OK);
        free(garbage);
    }
    char *embedded = (char *)malloc(len + 1);
    if (embedded != NULL) {
        memcpy(embedded, data, len + 1);
        embedded[len / 2] = '\0';
        CHECK(panel_online_parse_forecast(embedded, len, now, &forecast, reason,
                                          sizeof(reason)) != ESP_OK);
        free(embedded);
    }

    /* Stale and future response current.time are not accepted as fresh. */
    CHECK(panel_online_parse_forecast(data, len, now + 3600, &forecast, reason,
                                      sizeof(reason)) != ESP_OK);
    CHECK(panel_online_parse_forecast(data, len, now - 3600, &forecast, reason,
                                      sizeof(reason)) != ESP_OK);

    /* Wrong daily shape is rejected rather than normalized into a fake date. */
    char *daily_ts = replace_once(data, "\"2026-10-24\",", "\"2026-10-24T00:00\",");
    if (daily_ts != NULL) {
        CHECK(panel_online_parse_forecast(daily_ts, strlen(daily_ts), now, &forecast, reason,
                                          sizeof(reason)) != ESP_OK);
        free(daily_ts);
    } else {
        failures++;
    }
    free(data);
}

static void test_minimal_and_order(void)
{
    panel_online_forecast_t forecast;
    char reason[96];
    const int64_t now = 1792837200;
    size_t len = strlen(minimal_forecast);
    CHECK(panel_online_parse_forecast(minimal_forecast, len, now, &forecast, reason,
                                      sizeof(reason)) == ESP_OK);
    CHECK(forecast.hour_count == 2);
    CHECK(forecast.day_count == 2);

    char *gap = replace_once(minimal_forecast, "\"2026-10-24T19:00\"",
                             "\"2026-10-24T20:00\"");
    if (gap != NULL) {
        CHECK(panel_online_parse_forecast(gap, strlen(gap), now, &forecast, reason,
                                          sizeof(reason)) != ESP_OK);
        free(gap);
    } else {
        failures++;
    }

    char *day_gap = replace_once(minimal_forecast, "\"2026-10-25\"",
                                 "\"2026-10-26\"");
    if (day_gap != NULL) {
        CHECK(panel_online_parse_forecast(day_gap, strlen(day_gap), now, &forecast, reason,
                                          sizeof(reason)) != ESP_OK);
        free(day_gap);
    } else {
        failures++;
    }

    char *day_tomorrow = replace_once(minimal_forecast, "\"2026-10-24\"",
                                      "\"2026-10-25\"");
    if (day_tomorrow != NULL) {
        CHECK(panel_online_parse_forecast(day_tomorrow, strlen(day_tomorrow), now, &forecast,
                                          reason, sizeof(reason)) != ESP_OK);
        free(day_tomorrow);
    } else {
        failures++;
    }
}

static void test_depth_and_strings(void)
{
    char deep[1024];
    size_t used = 0;
    used += (size_t)snprintf(deep + used, sizeof(deep) - used, "%s", "{\"results\":");
    for (int i = 0; i < 20; i++) {
        used += (size_t)snprintf(deep + used, sizeof(deep) - used, "%s", "{\"a\":");
    }
    used += (size_t)snprintf(deep + used, sizeof(deep) - used, "%s", "1");
    for (int i = 0; i < 20; i++) {
        used += (size_t)snprintf(deep + used, sizeof(deep) - used, "%s", "}");
    }
    used += (size_t)snprintf(deep + used, sizeof(deep) - used, "%s", "}");
    panel_city_result_t results[6];
    unsigned count = 0;
    char reason[96];
    CHECK(panel_online_parse_geocode(deep, used, results, 6, &count, reason,
                                     sizeof(reason)) != ESP_OK);

    /* Brackets, braces and escaped quotes inside a string are data, not depth. */
    const char *escaped =
        "{\"results\":[],\"note\":\"[[{\\\"deep\\\":\\\"}]\\\"}]\"}";
    CHECK(panel_online_parse_geocode(escaped, strlen(escaped), results, 6, &count, reason,
                                     sizeof(reason)) == ESP_ERR_NOT_FOUND);
    CHECK(strstr(reason, "城市未找到") != NULL);
}

static void test_geocode_and_url(void)
{
    const char *json =
        "{\"results\":["
        "{\"name\":\"Guangzhou\",\"latitude\":23.11667,\"longitude\":113.25,"
        "\"country\":\"China\",\"admin1\":\"Guangdong\"},"
        "{\"name\":\"Shenzhen\",\"latitude\":22.54554,\"longitude\":114.0683,"
        "\"country\":\"China\",\"admin1\":\"Guangdong\"}]}";
    panel_city_result_t results[6];
    unsigned count = 0;
    char reason[96];
    CHECK(panel_online_parse_geocode(json, strlen(json), results, 6, &count, reason,
                                     sizeof(reason)) == ESP_OK);
    CHECK(count == 2);
    CHECK(strcmp(results[0].name, "Guangzhou") == 0);
    CHECK(strcmp(results[0].region, "Guangdong") == 0);
    CHECK(results[1].latitude > 22.5 && results[1].latitude < 22.6);

    const char *empty = "{\"results\":null}";
    CHECK(panel_online_parse_geocode(empty, strlen(empty), results, 6, &count, reason,
                                     sizeof(reason)) == ESP_ERR_NOT_FOUND);

    char url[512];
    CHECK(panel_online_build_forecast_url(23.11667, 113.25, url, sizeof(url)) == ESP_OK);
    CHECK(strstr(url, "api.open-meteo.com") != NULL);
    CHECK(strstr(url, "timezone=Asia%2FShanghai") != NULL);
    CHECK(panel_online_build_geocode_url("Guang zhou", url, sizeof(url)) == ESP_OK);
    CHECK(strstr(url, "Guang%20zhou") != NULL);
    CHECK(!panel_online_url_encode("Guang zhou", url, 4));
}

static void city_valid_base(panel_city_result_t *city)
{
    memset(city, 0, sizeof(*city));
    snprintf(city->name, sizeof(city->name), "Guangzhou");
    snprintf(city->region, sizeof(city->region), "Guangdong");
    snprintf(city->country, sizeof(city->country), "China");
    city->latitude = 23.11667;
    city->longitude = 113.25;
}

static void test_city_valid_unterminated_fields(void)
{
    panel_city_result_t city;
    city_valid_base(&city);
    CHECK(panel_online_city_valid(&city));

    /* Every fixed display field has no NUL at all.  This is the actual corrupt
     * NVS blob shape; an overlong C string is not sufficient coverage. */
    city_valid_base(&city);
    memset(city.name, 'A', sizeof(city.name));
    memset(city.region, 'B', sizeof(city.region));
    memset(city.country, 'C', sizeof(city.country));
    CHECK(!panel_online_city_valid(&city));

    city_valid_base(&city);
    memset(city.name, 'A', sizeof(city.name));
    CHECK(!panel_online_city_valid(&city));

    city_valid_base(&city);
    memset(city.region, 'B', sizeof(city.region));
    CHECK(!panel_online_city_valid(&city));

    city_valid_base(&city);
    memset(city.country, 'C', sizeof(city.country));
    CHECK(!panel_online_city_valid(&city));

    /* Control characters in each display field must be rejected. */
    city_valid_base(&city);
    city.name[1] = 0x1F;
    CHECK(!panel_online_city_valid(&city));
    city_valid_base(&city);
    city.region[0] = 0x7F;
    CHECK(!panel_online_city_valid(&city));
    city_valid_base(&city);
    city.country[0] = 0x01;
    CHECK(!panel_online_city_valid(&city));

    /* Valid full-length strings with a NUL in the final array byte pass. */
    city_valid_base(&city);
    memset(city.name, 'A', sizeof(city.name));
    city.name[sizeof(city.name) - 1] = '\0';
    memset(city.region, 'B', sizeof(city.region));
    city.region[sizeof(city.region) - 1] = '\0';
    memset(city.country, 'C', sizeof(city.country));
    city.country[sizeof(city.country) - 1] = '\0';
    CHECK(panel_online_city_valid(&city));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <openmeteo-reference.json>\n", argv[0]);
        return 2;
    }
    test_real_fixture(argv[1]);
    test_minimal_and_order();
    test_depth_and_strings();
    test_geocode_and_url();
    test_city_valid_unterminated_fields();
    printf("ONLINE_MODEL_TEST failures=%d\n", failures);
    return failures ? 1 : 0;
}
