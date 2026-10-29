#include "panel_online_model.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static void set_reason(char *reason, size_t cap, const char *text)
{
    if (reason == NULL || cap == 0) {
        return;
    }
    snprintf(reason, cap, "%s", text != NULL ? text : "");
}

static bool copy_display(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0 || src == NULL) {
        return false;
    }
    size_t len = strlen(src);
    if (len == 0 || len >= cap) {
        return false;
    }
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        /* Dynamic names are shown with the existing raw fonts, which cover
         * ASCII and the static Chinese UI strings.  Non-ASCII search results
         * are normalized to '?' rather than risking mosaic glyphs. */
        dst[out++] = (c < 0x80) ? (char)c : '?';
    }
    dst[out] = '\0';
    return true;
}

static bool ascii_display(const char *text)
{
    if (text == NULL) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != 0; p++) {
        if (*p < 0x20 || *p >= 0x7F) {
            return false;
        }
    }
    return true;
}

bool panel_online_is_ascii_display(const char *text)
{
    return ascii_display(text);
}

static const char *json_string(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool json_number(const cJSON *object, const char *key, double min, double max,
                        double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    double value = item->valuedouble;
    if (!isfinite(value) || value < min || value > max) {
        return false;
    }
    *out = value;
    return true;
}

static bool json_int(const cJSON *object, const char *key, int min, int max, int *out)
{
    double value = 0;
    if (!json_number(object, key, (double)min, (double)max, &value)) {
        return false;
    }
    int rounded = (int)floor(value + 0.5);
    if (fabs(value - (double)rounded) > 0.0001) {
        return false;
    }
    *out = rounded;
    return true;
}

static bool round_temp(double value, int min, int max, int *out)
{
    if (!isfinite(value) || value < (double)min || value > (double)max) {
        return false;
    }
    *out = (int)floor(value + 0.5);
    return true;
}

static int days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    return days[month - 1];
}

/* Howard Hinnant's civil date algorithm; dates are local CST-8 wall time. */
static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    int adjusted_month = (int)month + ((month > 2) ? -3 : 9);
    const unsigned doy =
        (153 * (unsigned)adjusted_month + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool parse_iso_local(const char *text, int64_t *epoch, int *year, int *month, int *day,
                            int *hour, int *minute)
{
    if (text == NULL) {
        return false;
    }
    int y = 0;
    int mo = 0;
    int d = 0;
    int h = 0;
    int mi = 0;
    char tail = 0;
    int fields = sscanf(text, "%4d-%2d-%2dT%2d:%2d%c", &y, &mo, &d, &h, &mi, &tail);
    if (fields != 5) {
        return false;
    }
    (void)tail;
    if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 ||
        d > days_in_month(y, mo) || h < 0 || h > 23 || mi < 0 || mi > 59) {
        return false;
    }
    int64_t days = days_from_civil(y, (unsigned)mo, (unsigned)d);
    *epoch = days * 86400 + h * 3600 + mi * 60 - 8 * 3600;
    if (year) *year = y;
    if (month) *month = mo;
    if (day) *day = d;
    if (hour) *hour = h;
    if (minute) *minute = mi;
    return true;
}

/* Open-Meteo daily.time is YYYY-MM-DD, not an ISO timestamp. */
static bool parse_iso_date(const char *text, int64_t *epoch, int *year, int *month, int *day)
{
    if (text == NULL) {
        return false;
    }
    int y = 0;
    int mo = 0;
    int d = 0;
    char tail = 0;
    int fields = sscanf(text, "%4d-%2d-%2d%c", &y, &mo, &d, &tail);
    if (fields != 3) {
        return false;
    }
    (void)tail;
    if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 ||
        d > days_in_month(y, mo)) {
        return false;
    }
    *epoch = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400;
    if (year) *year = y;
    if (month) *month = mo;
    if (day) *day = d;
    return true;
}

static bool array_number(const cJSON *array, int index, double min, double max, double *out)
{
    const cJSON *item = cJSON_GetArrayItem(array, index);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    double value = item->valuedouble;
    if (!isfinite(value) || value < min || value > max) {
        return false;
    }
    *out = value;
    return true;
}

static bool array_int(const cJSON *array, int index, int min, int max, int *out)
{
    double value = 0;
    if (!array_number(array, index, (double)min, (double)max, &value)) {
        return false;
    }
    int rounded = (int)floor(value + 0.5);
    if (fabs(value - (double)rounded) > 0.0001) {
        return false;
    }
    *out = rounded;
    return true;
}

static bool array_string(const cJSON *array, int index, const char **out)
{
    const cJSON *item = cJSON_GetArrayItem(array, index);
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
        return false;
    }
    *out = item->valuestring;
    return true;
}

panel_weather_kind_t panel_online_kind_from_code(int code)
{
    if (code == 0) return PANEL_WEATHER_CLEAR;
    if (code == 1 || code == 2) return PANEL_WEATHER_PARTLY;
    if (code == 3) return PANEL_WEATHER_CLOUDY;
    if (code == 45 || code == 48) return PANEL_WEATHER_FOG;
    if (code >= 51 && code <= 57) return PANEL_WEATHER_DRIZZLE;
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) {
        return PANEL_WEATHER_RAIN;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return PANEL_WEATHER_SNOW;
    }
    if (code >= 95 && code <= 99) return PANEL_WEATHER_THUNDER;
    return PANEL_WEATHER_UNKNOWN;
}

#define ONLINE_JSON_MAX_DEPTH 16
#define ONLINE_MAX_DATA_SKEW_SEC 1800

/* Bounded structural scan before cJSON.  Braces/brackets inside strings are
 * ignored, including escaped quotes/backslashes. */
static bool json_depth_ok(const char *json, size_t len)
{
    unsigned depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)json[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            depth++;
            if (depth > ONLINE_JSON_MAX_DEPTH) {
                return false;
            }
        } else if (c == '}' || c == ']') {
            if (depth == 0) {
                return false;
            }
            depth--;
        }
    }
    return !in_string && !escaped && depth == 0;
}

static bool parse_json_root(const char *json, size_t len, size_t max_len,
                            char *reason, size_t reason_len, cJSON **out)
{
    *out = NULL;
    if (json == NULL || len == 0 || len > max_len) {
        set_reason(reason, reason_len, "响应超长");
        return false;
    }
    if (memchr(json, '\0', len) != NULL) {
        set_reason(reason, reason_len, "JSON 无效");
        return false;
    }
    if (!json_depth_ok(json, len)) {
        set_reason(reason, reason_len, "JSON 无效");
        return false;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    if (root == NULL || end == NULL || end < json || end > json + len) {
        cJSON_Delete(root);
        set_reason(reason, reason_len, "JSON 无效");
        return false;
    }
    const char *p = end;
    while (p < json + len && isspace((unsigned char)*p)) {
        p++;
    }
    if (p != json + len) {
        cJSON_Delete(root);
        set_reason(reason, reason_len, "JSON 无效");
        return false;
    }
    *out = root;
    return true;
}

esp_err_t panel_online_parse_forecast(const char *json, size_t len, int64_t now_epoch,
                                      panel_online_forecast_t *out,
                                      char *reason, size_t reason_len)
{
    if (out == NULL) {
        set_reason(reason, reason_len, "天气输出为空");
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    set_reason(reason, reason_len, "");

    cJSON *root = NULL;
    if (!parse_json_root(json, len, PANEL_ONLINE_FORECAST_MAX, reason, reason_len, &root)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    const char *failure = "天气数据无效";
    const char *timezone = json_string(root, "timezone");
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *current_units = cJSON_GetObjectItemCaseSensitive(root, "current_units");
    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");

    if (!cJSON_IsObject(current) || !cJSON_IsObject(current_units) ||
        !cJSON_IsObject(hourly) || !cJSON_IsObject(daily) ||
        timezone == NULL || strcmp(timezone, "Asia/Shanghai") != 0) {
        goto done;
    }
    if (json_string(current_units, "temperature_2m") == NULL ||
        strcmp(json_string(current_units, "temperature_2m"), "°C") != 0 ||
        json_string(current_units, "relative_humidity_2m") == NULL ||
        strcmp(json_string(current_units, "relative_humidity_2m"), "%") != 0 ||
        json_string(current_units, "wind_speed_10m") == NULL ||
        strcmp(json_string(current_units, "wind_speed_10m"), "km/h") != 0) {
        failure = "天气单位不匹配";
        goto done;
    }

    const char *current_time = json_string(current, "time");
    int current_year = 0;
    int current_month = 0;
    int current_day = 0;
    if (!parse_iso_local(current_time, &out->current_epoch, &current_year, &current_month,
                         &current_day, NULL, NULL)) {
        failure = "当前时间无效";
        goto done;
    }
    if (now_epoch <= 0 ||
        llabs((long long)(out->current_epoch - now_epoch)) > ONLINE_MAX_DATA_SKEW_SEC) {
        failure = "当前时间异常";
        goto done;
    }
    double temp = 0;
    double apparent = 0;
    double humidity = 0;
    double wind = 0;
    int code = 0;
    if (!json_number(current, "temperature_2m", -60, 60, &temp) ||
        !json_number(current, "apparent_temperature", -70, 70, &apparent) ||
        !json_number(current, "relative_humidity_2m", 0, 100, &humidity) ||
        !json_number(current, "wind_speed_10m", 0, 300, &wind) ||
        !json_int(current, "weather_code", 0, 99, &code)) {
        goto done;
    }
    if (!round_temp(temp, -60, 60, &out->current_temp_c) ||
        !round_temp(apparent, -70, 70, &out->apparent_temp_c)) {
        goto done;
    }
    out->humidity_pct = (int)floor(humidity + 0.5);
    out->wind_speed_kmh = wind;
    out->weather_code = code;
    out->kind = panel_online_kind_from_code(code);

    const cJSON *hour_time = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    const cJSON *hour_temp = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
    const cJSON *hour_code = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");
    int hour_count = 0;
    if (!cJSON_IsArray(hour_time) || !cJSON_IsArray(hour_temp) || !cJSON_IsArray(hour_code)) {
        goto done;
    }
    hour_count = cJSON_GetArraySize(hour_time);
    if (hour_count < 1 || hour_count > 168 ||
        cJSON_GetArraySize(hour_temp) != hour_count ||
        cJSON_GetArraySize(hour_code) != hour_count) {
        goto done;
    }
    int64_t current_day_epoch =
        days_from_civil(current_year, (unsigned)current_month, (unsigned)current_day) * 86400;
    int64_t current_hour_epoch = out->current_epoch - (out->current_epoch % 3600);
    int64_t previous_hour_epoch = 0;
    int64_t first_selected_epoch = 0;
    for (int i = 0; i < hour_count && out->hour_count < PANEL_ONLINE_HOURS; i++) {
        const char *time_text = NULL;
        int64_t hour_epoch = 0;
        double hour_temp_value = 0;
        int hour_code_value = 0;
        if (!array_string(hour_time, i, &time_text) ||
            !parse_iso_local(time_text, &hour_epoch, NULL, NULL, NULL, NULL, NULL) ||
            !array_number(hour_temp, i, -60, 60, &hour_temp_value) ||
            !array_int(hour_code, i, 0, 99, &hour_code_value)) {
            goto done;
        }
        if (i > 0 && hour_epoch != previous_hour_epoch + 3600) {
            failure = "小时数据顺序错误";
            goto done;
        }
        previous_hour_epoch = hour_epoch;
        if (hour_epoch < current_hour_epoch) {
            continue;
        }
        if (out->hour_count == 0) {
            first_selected_epoch = hour_epoch;
        }
        int64_t local_seconds = hour_epoch + 8 * 3600;
        int64_t local_day = local_seconds / 86400;
        panel_online_hour_t *entry = &out->hours[out->hour_count++];
        entry->day_offset = (int)(local_day - current_day_epoch / 86400);
        entry->hour = (int)((local_seconds % 86400) / 3600);
        entry->temp_c = (int)floor(hour_temp_value + 0.5);
        entry->kind = panel_online_kind_from_code(hour_code_value);
    }
    if (out->hour_count == 0) {
        failure = "无未来小时数据";
        goto done;
    }
    if (first_selected_epoch - current_hour_epoch > 3600) {
        failure = "小时数据缺口";
        goto done;
    }

    const cJSON *day_time = cJSON_GetObjectItemCaseSensitive(daily, "time");
    const cJSON *day_max = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    const cJSON *day_min = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    const cJSON *day_code = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
    int day_count = 0;
    if (!cJSON_IsArray(day_time) || !cJSON_IsArray(day_max) ||
        !cJSON_IsArray(day_min) || !cJSON_IsArray(day_code)) {
        goto done;
    }
    day_count = cJSON_GetArraySize(day_time);
    if (day_count < 1 || day_count > 7 ||
        cJSON_GetArraySize(day_max) != day_count ||
        cJSON_GetArraySize(day_min) != day_count ||
        cJSON_GetArraySize(day_code) != day_count) {
        goto done;
    }
    int64_t previous_day_epoch = 0;
    for (int i = 0; i < day_count; i++) {
        const char *time_text = NULL;
        int64_t day_epoch = 0;
        double max_value = 0;
        double min_value = 0;
        int code_value = 0;
        if (!array_string(day_time, i, &time_text) ||
            !parse_iso_date(time_text, &day_epoch, NULL, NULL, NULL) ||
            !array_number(day_max, i, -60, 60, &max_value) ||
            !array_number(day_min, i, -60, 60, &min_value) ||
            !array_int(day_code, i, 0, 99, &code_value)) {
            goto done;
        }
        if (i > 0 && day_epoch != previous_day_epoch + 86400) {
            failure = "日数据顺序错误";
            goto done;
        }
        previous_day_epoch = day_epoch;
        int max_round = (int)floor(max_value + 0.5);
        int min_round = (int)floor(min_value + 0.5);
        if (max_round < min_round) {
            goto done;
        }
        int64_t local_seconds = day_epoch + 8 * 3600;
        int64_t local_day = local_seconds / 86400;
        int day_offset = (int)(local_day - current_day_epoch / 86400);
        if (i == 0 && day_offset != 0) {
            failure = "日数据起始错误";
            goto done;
        }
        if (out->day_count < PANEL_ONLINE_DAYS) {
            panel_online_day_t *entry = &out->days[out->day_count++];
            entry->day_offset = day_offset;
            int weekday = (int)((local_day + 4) % 7);
            if (weekday < 0) weekday += 7;
            entry->weekday = weekday;
            entry->temp_max_c = max_round;
            entry->temp_min_c = min_round;
            entry->kind = panel_online_kind_from_code(code_value);
        }
    }
    if (out->day_count == 0) {
        failure = "无日数据";
        goto done;
    }

    err = ESP_OK;
    failure = "";
done:
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        set_reason(reason, reason_len, failure);
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t panel_online_parse_geocode(const char *json, size_t len,
                                     panel_city_result_t *results, unsigned max_results,
                                     unsigned *count, char *reason, size_t reason_len)
{
    if (results == NULL || count == NULL || max_results == 0) {
        set_reason(reason, reason_len, "城市输出为空");
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    set_reason(reason, reason_len, "");

    cJSON *root = NULL;
    if (!parse_json_root(json, len, PANEL_ONLINE_GEOCODE_MAX, reason, reason_len, &root)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_reason(reason, reason_len, "JSON 无效");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (items == NULL || cJSON_IsNull(items)) {
        cJSON_Delete(root);
        set_reason(reason, reason_len, "城市未找到");
        return ESP_ERR_NOT_FOUND;
    }
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        set_reason(reason, reason_len, "城市数据无效");
        return ESP_ERR_INVALID_RESPONSE;
    }

    int size = cJSON_GetArraySize(items);
    for (int i = 0; i < size && *count < max_results; i++) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }
        const char *name = json_string(item, "name");
        double latitude = 0;
        double longitude = 0;
        if (!json_number(item, "latitude", -90, 90, &latitude) ||
            !json_number(item, "longitude", -180, 180, &longitude) ||
            name == NULL) {
            continue;
        }
        panel_city_result_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        if (!copy_display(candidate.name, sizeof(candidate.name), name)) {
            continue;
        }
        const char *region = json_string(item, "admin1");
        const char *country = json_string(item, "country");
        if (region != NULL) {
            (void)copy_display(candidate.region, sizeof(candidate.region), region);
        }
        if (country != NULL) {
            (void)copy_display(candidate.country, sizeof(candidate.country), country);
        }
        candidate.latitude = latitude;
        candidate.longitude = longitude;
        if (!panel_online_city_valid(&candidate)) {
            continue;
        }
        results[(*count)++] = candidate;
    }
    cJSON_Delete(root);
    if (*count == 0) {
        set_reason(reason, reason_len, "城市未找到");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static bool fixed_display_text_valid(const char *text, size_t cap, bool require_nonempty)
{
    if (text == NULL || cap == 0) {
        return false;
    }
    const char *nul = (const char *)memchr(text, '\0', cap);
    if (nul == NULL) {
        return false;
    }
    size_t len = (size_t)(nul - text);
    if (require_nonempty && len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

bool panel_online_city_valid(const panel_city_result_t *city)
{
    if (city == NULL || !isfinite(city->latitude) || !isfinite(city->longitude) ||
        city->latitude < -90 || city->latitude > 90 ||
        city->longitude < -180 || city->longitude > 180) {
        return false;
    }
    /* NVS blobs may be corrupt or unterminated.  Bound every fixed array
     * before any string operation and validate all display fields. */
    if (!fixed_display_text_valid(city->name, sizeof(city->name), true) ||
        !fixed_display_text_valid(city->region, sizeof(city->region), false) ||
        !fixed_display_text_valid(city->country, sizeof(city->country), false)) {
        return false;
    }
    return true;
}

bool panel_online_url_encode(const char *in, char *out, size_t out_len)
{
    if (in == NULL || out == NULL || out_len == 0) {
        return false;
    }
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != 0; p++) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (used + 1 >= out_len) return false;
            out[used++] = (char)c;
        } else {
            if (used + 3 >= out_len) return false;
            static const char hex[] = "0123456789ABCDEF";
            out[used++] = '%';
            out[used++] = hex[(c >> 4) & 0xF];
            out[used++] = hex[c & 0xF];
        }
    }
    out[used] = '\0';
    return true;
}

esp_err_t panel_online_build_forecast_url(double latitude, double longitude,
                                          char *out, size_t out_len)
{
    if (out == NULL || out_len == 0 || !isfinite(latitude) || !isfinite(longitude) ||
        latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(
        out, out_len,
        "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
        "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
        "&hourly=temperature_2m,weather_code"
        "&daily=temperature_2m_max,temperature_2m_min,weather_code"
        "&timezone=Asia%%2FShanghai&forecast_days=5",
        latitude, longitude);
    if (written <= 0 || (size_t)written >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t panel_online_build_geocode_url(const char *query, char *out, size_t out_len)
{
    if (query == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t query_len = strlen(query);
    if (query_len == 0 || query_len > PANEL_ONLINE_CITY_QUERY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const unsigned char *p = (const unsigned char *)query; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    char encoded[PANEL_ONLINE_CITY_QUERY_MAX * 3 + 1];
    if (!panel_online_url_encode(query, encoded, sizeof(encoded))) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(
        out, out_len,
        "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=6&language=en&format=json",
        encoded);
    if (written <= 0 || (size_t)written >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
