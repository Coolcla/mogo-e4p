/*
 * 中文：中国城市天气的有界解析与日期策略。未知天气代码不冒充晴天。
 * English: Bounded parsing and date policy for Chinese cities. Unknown codes never imply sunshine.
 */
#include "weather_model.h"
#include "../time/timezone.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool text_ok(const char *s, size_t capacity, bool empty) {
    if (!s || (!empty && !*s) || strlen(s) >= capacity) return false;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++, n = 0, v = c;
        if (c < 32 || c == 127) return false;
        if (c < 128) continue;
        if (c >= 194 && c <= 223) { n = 1; v = c & 31; }
        else if (c >= 224 && c <= 239) { n = 2; v = c & 15; }
        else if (c >= 240 && c <= 244) { n = 3; v = c & 7; }
        else return false;
        unsigned bytes = n;
        while (n--) { if ((*p & 192) != 128) return false; v = (v << 6) | (*p++ & 63); }
        if ((bytes == 2 && v < 2048) || (bytes == 3 && v < 65536) ||
            v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return false;
    }
    return true;
}

static cJSON *document(const char *json, size_t length) {
    if (!json || !length || length > WEATHER_BODY_LIMIT || memchr(json, 0, length)) return NULL;
    int depth = 0; bool quoted = false, escaped = false;
    for (size_t i = 0; i < length; ++i) {
        char c = json[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 12) return NULL; }
        else if (c == '}' || c == ']') { if (--depth < 0) return NULL; }
    }
    if (quoted || depth) return NULL;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return NULL;
    while (end < json + length && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
    if (end != json + length || !cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    return root;
}

static bool string_field(cJSON *root, const char *key, char *out, size_t cap, bool empty) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(v) || !text_ok(v->valuestring, cap, empty)) return false;
    memcpy(out, v->valuestring, strlen(v->valuestring) + 1);
    return true;
}
static bool number(cJSON *root, const char *key, double low, double high, double *out) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < low || v->valuedouble > high)
        return false;
    *out = v->valuedouble;
    return true;
}
static bool integer(cJSON *root, const char *key, int low, int high, int *out) {
    double n;
    if (!number(root, key, low, high, &n) || floor(n) != n) return false;
    *out = (int)n; return true;
}
static bool boolean(cJSON *root, const char *key, bool *out) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsBool(v)) return false;
    *out = cJSON_IsTrue(v); return true;
}
static int month_days(int year, int month) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}
static int32_t civil_day(int year, int month, int day) {
    year -= month <= 2;
    int era = year / 400, y = year - era * 400, m = month + (month > 2 ? -3 : 9);
    return era * 146097 + y * 365 + y / 4 - y / 100 + (153 * m + 2) / 5 + day - 1 - 719468;
}
static int32_t unix_day(int64_t seconds) {
    if (seconds < 0) return -1;
    time_t value = (time_t)seconds;
    struct tm utc = {0};
    return gmtime_r(&value, &utc) ? civil_day(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday) : -1;
}
static int digits(const char *s, unsigned n) {
    int v = 0;
    for (unsigned i = 0; i < n; ++i) { if (s[i] < '0' || s[i] > '9') return -1; v = v * 10 + s[i] - '0'; }
    return v;
}
static int32_t parse_date(const char *s) {
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return -1;
    int y = digits(s, 4), m = digits(s + 5, 2), d = digits(s + 8, 2);
    if (y < 2024 || y > 2099 || m < 1 || m > 12 || d < 1 || d > month_days(y, m)) return -1;
    return civil_day(y, m, d);
}
int64_t weather_timestamp(const char *s) {
    if (!s || strlen(s) < 20 || strlen(s) > 30 || s[10] != 'T' || s[13] != ':' || s[16] != ':') return 0;
    char date[11]; memcpy(date, s, 10); date[10] = 0;
    int32_t day = parse_date(date);
    int h = digits(s + 11, 2), m = digits(s + 14, 2), sec = digits(s + 17, 2);
    if (day < 0 || h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) return 0;
    size_t n = strlen(s);
    if (n != 20) {
        if (s[19] != '.' || n < 22) return 0;
        for (size_t i = 20; i + 1 < n; ++i) if (s[i] < '0' || s[i] > '9') return 0;
    }
    if (s[n - 1] != 'Z') return 0;
    return (int64_t)day * 86400 + h * 3600 + m * 60 + sec;
}
int64_t weather_http_date(const char *s) {
    if (!s || strnlen(s, 30) != 29 || s[3] != ',' || s[4] != ' ' || s[7] != ' ' ||
        s[11] != ' ' || s[16] != ' ' || s[19] != ':' || s[22] != ':' || strcmp(s + 25, " GMT")) return 0;
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const char weekdays[] = "SunMonTueWedThuFriSat";
    int month = 1, weekday = 0;
    while (month <= 12 && memcmp(s + 8, months + (month - 1) * 3, 3)) ++month;
    while (weekday < 7 && memcmp(s, weekdays + weekday * 3, 3)) ++weekday;
    int year = digits(s + 12, 4), day = digits(s + 5, 2);
    int hour = digits(s + 17, 2), minute = digits(s + 20, 2), second = digits(s + 23, 2);
    if (year < 2024 || year > 2099 || month > 12 || weekday == 7 || day < 1 ||
        day > month_days(year, month) || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) return 0;
    int32_t date = civil_day(year, month, day);
    if ((date + 4) % 7 != weekday) return 0;
    return (int64_t)date * 86400 + hour * 3600 + minute * 60 + second;
}
bool weather_clock_valid(int64_t now) { return now >= 1704067200 && now <= 4102444799LL; }
int32_t weather_local_day(int64_t now) {
    if (now < 0) return -1;
    time_t value = (time_t)now;
    struct tm local = {0};
    if (!localtime_r(&value, &local)) return -1;
    return civil_day(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}
void weather_date_text(int32_t day, char *out, size_t cap) {
    int y = 2024;
    while (y < 2100 && civil_day(y + 1, 1, 1) <= day) ++y;
    int m = 1; while (m < 12 && civil_day(y, m + 1, 1) <= day) ++m;
    static const char *week[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
    snprintf(out, cap, "%02d月%02d日  %s", m, (int)(day - civil_day(y, m, 1) + 1), week[(day + 4) % 7]);
}
void weather_time_text(int64_t utc, char *out, size_t cap) {
    if (!utc) { snprintf(out, cap, "--:--"); return; }
    time_t value = (time_t)utc;
    struct tm local = {0};
    if (!localtime_r(&value, &local)) { snprintf(out, cap, "--:--"); return; }
    snprintf(out, cap, "%02d:%02d", local.tm_hour, local.tm_min);
}
bool weather_location_valid(const weather_location_t *v) {
    return v && memchr(v->id, 0, sizeof(v->id)) && v->id[0] &&
        strspn(v->id, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") == strlen(v->id) &&
        memchr(v->admin2, 0, sizeof(v->admin2)) && text_ok(v->admin2, sizeof(v->admin2), true) &&
        memchr(v->name, 0, sizeof(v->name)) && memchr(v->admin1, 0, sizeof(v->admin1)) &&
        memchr(v->country, 0, sizeof(v->country)) && memchr(v->timezone, 0, sizeof(v->timezone)) &&
        text_ok(v->name, sizeof(v->name), false) && text_ok(v->admin1, sizeof(v->admin1), true) &&
        text_ok(v->country, sizeof(v->country), false) &&
        text_ok(v->timezone, sizeof(v->timezone), false) && app_timezone_posix_rule(v->timezone) &&
        isfinite(v->lat) && isfinite(v->lon) && v->lat >= -90 && v->lat <= 90 && v->lon >= -180 && v->lon <= 180;
}
static bool point(cJSON *root, weather_location_t *v) {
    return cJSON_IsObject(root) && number(root, "lat", -90, 90, &v->lat) &&
        number(root, "lon", -180, 180, &v->lon) && string_field(root, "timezone", v->timezone, sizeof(v->timezone), false) &&
        string_field(root, "locationId", v->id, sizeof(v->id), false);
}
static bool location(cJSON *root, weather_location_t *v) {
    memset(v, 0, sizeof(*v));
    return point(root, v) && string_field(root, "name", v->name, sizeof(v->name), false) &&
        string_field(root, "admin1", v->admin1, sizeof(v->admin1), true) &&
        string_field(root, "admin2", v->admin2, sizeof(v->admin2), true) &&
        string_field(root, "country", v->country, sizeof(v->country), false) && weather_location_valid(v);
}
bool weather_location_parse(const char *json, size_t length, weather_location_t *out) {
    cJSON *root = document(json, length); weather_location_t v;
    bool ok = root && location(root, &v);
    if (ok) *out = v;
    cJSON_Delete(root); return ok;
}

static void coordinate_id(double lat, double lon, char out[17]) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const double values[] = {lat, lon};
    for (unsigned i = 0; i < 2; ++i) {
        const unsigned char *bytes = (const unsigned char *)&values[i];
        for (size_t j = 0; j < sizeof(values[i]); ++j) hash = (hash ^ bytes[j]) * UINT64_C(1099511628211);
    }
    snprintf(out, 17, "%016llX", (unsigned long long)hash);
}

static bool optional_text(cJSON *root, const char *key, char *out, size_t capacity) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item || cJSON_IsNull(item)) { out[0] = 0; return true; }
    if (!cJSON_IsString(item) || !text_ok(item->valuestring, capacity, true)) return false;
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

static bool openmeteo_location(cJSON *item, weather_location_t *out) {
    weather_location_t value = {0};
    double id;
    bool has_id = number(item, "id", 0, 9007199254740991.0, &id) && floor(id) == id;
    if (!cJSON_IsObject(item) || !number(item, "latitude", -90, 90, &value.lat) ||
        !number(item, "longitude", -180, 180, &value.lon) ||
        !string_field(item, "name", value.name, sizeof(value.name), false) ||
        !optional_text(item, "admin1", value.admin1, sizeof(value.admin1)) ||
        !optional_text(item, "admin2", value.admin2, sizeof(value.admin2)) ||
        !string_field(item, "country", value.country, sizeof(value.country), false) ||
        !string_field(item, "timezone", value.timezone, sizeof(value.timezone), false)) return false;
    if (has_id) snprintf(value.id, sizeof(value.id), "%.0f", id);
    else coordinate_id(value.lat, value.lon, value.id);
    if (!weather_location_valid(&value)) return false;
    *out = value;
    return true;
}

bool weather_openmeteo_locations_parse(const char *json, size_t length, weather_location_t *out,
                                       unsigned page_size, unsigned *count) {
    if (!count) return false;
    *count = 0;
    cJSON *root = document(json, length);
    cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    int n = cJSON_GetArraySize(results);
    bool ok = root && out && page_size && page_size <= WEATHER_CITY_LIMIT && cJSON_IsArray(results) &&
              n >= 0 && (unsigned)n <= page_size;
    for (int i = 0; ok && i < n; ++i) ok = openmeteo_location(cJSON_GetArrayItem(results, i), &out[i]);
    if (ok) *count = (unsigned)n;
    cJSON_Delete(root);
    return ok;
}

bool weather_beacondb_location_parse(const char *json, size_t length, double *lat, double *lon) {
    if (!lat || !lon) return false;
    cJSON *root = document(json, length);
    cJSON *location = cJSON_GetObjectItemCaseSensitive(root, "location");
    double latitude, longitude;
    bool ok = root && cJSON_IsObject(location) && number(location, "lat", -90, 90, &latitude) &&
              number(location, "lng", -180, 180, &longitude);
    if (ok) { *lat = latitude; *lon = longitude; }
    cJSON_Delete(root);
    return ok;
}

static const char *first_address_text(cJSON *address, const char *const *keys) {
    for (size_t i = 0; keys[i]; ++i) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(address, keys[i]);
        if (cJSON_IsString(item) && text_ok(item->valuestring, 97, false)) return item->valuestring;
    }
    return NULL;
}

bool weather_nominatim_location_parse(const char *json, size_t length, double lat, double lon,
                                      weather_location_t *out) {
    if (!out || !isfinite(lat) || !isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180)
        return false;
    cJSON *root = document(json, length);
    cJSON *address = cJSON_GetObjectItemCaseSensitive(root, "address");
    static const char *const city_keys[] = {"city", "town", "village", "municipality", "hamlet", "suburb", "county", NULL};
    static const char *const admin_keys[] = {"state", "province", "region", "state_district", NULL};
    static const char *const admin2_keys[] = {"county", "state_district", "municipality", NULL};
    const char *city = first_address_text(address, city_keys);
    const char *admin1 = first_address_text(address, admin_keys);
    const char *admin2 = first_address_text(address, admin2_keys);
    cJSON *country_item = cJSON_GetObjectItemCaseSensitive(address, "country");
    weather_location_t value = {0};
    bool ok = root && cJSON_IsObject(address) && city && cJSON_IsString(country_item) &&
              text_ok(country_item->valuestring, sizeof(value.country), false);
    if (ok) {
        memcpy(value.name, city, strlen(city) + 1);
        if (admin1) memcpy(value.admin1, admin1, strlen(admin1) + 1);
        if (admin2) memcpy(value.admin2, admin2, strlen(admin2) + 1);
        memcpy(value.country, country_item->valuestring, strlen(country_item->valuestring) + 1);
        value.lat = lat; value.lon = lon;
        snprintf(value.timezone, sizeof(value.timezone), "America/Vancouver");
        coordinate_id(lat, lon, value.id);
        ok = weather_location_valid(&value);
    }
    if (ok) *out = value;
    cJSON_Delete(root);
    return ok;
}
bool weather_locations_parse(const char *json, size_t length, weather_location_t *out, unsigned page_size,
                             unsigned *count, unsigned *total, unsigned *page) {
    *count = *total = *page = 0; cJSON *root = document(json, length);
    cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "locations");
    int n = cJSON_GetArraySize(list);
    int size = 0, all = 0, current = 0;
    bool ok = root && out && page_size && page_size <= WEATHER_SEARCH_BATCH_SIZE &&
        cJSON_IsArray(list) && n <= (int)page_size &&
        integer(root, "pageSize", (int)page_size, (int)page_size, &size) &&
        integer(root, "total", 0, WEATHER_SEARCH_TOTAL_LIMIT, &all) &&
        integer(root, "page", 0, WEATHER_SEARCH_TOTAL_LIMIT - 1, &current) &&
        (all ? current * size < all && n == (all - current * size < size ? all - current * size : size) : !current && !n);
    for (int i = 0; ok && i < n; ++i) ok = location(cJSON_GetArrayItem(list, i), &out[i]);
    if (ok) { *count = (unsigned)n; *total = (unsigned)all; *page = (unsigned)current; }
    cJSON_Delete(root); return ok;
}
bool weather_catalog_parse(const char *json, size_t length, bool *enabled, bool *available) {
    *enabled = *available = false;
    cJSON *root = document(json, length); int schema;
    cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "components");
    bool ok = root && integer(root, "schemaVersion", 1, 1, &schema) && cJSON_IsArray(list) && cJSON_GetArraySize(list) <= 16;
    bool found = false; cJSON *item;
    cJSON_ArrayForEach(item, list) {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(item, "code");
        if (cJSON_IsString(code) && !strcmp(code->valuestring, "weather")) {
            if (found || !integer(item, "schemaVersion", 1, 1, &schema) ||
                !boolean(item, "enabled", enabled) || !boolean(item, "available", available)) ok = false;
            found = true;
        }
    }
    cJSON_Delete(root); return ok && found;
}
void weather_error_code(const char *json, size_t length, char *code, size_t capacity) {
    if (!capacity) return;
    code[0] = 0;
    cJSON *root = document(json, length);
    if (root) (void)string_field(root, "code", code, capacity, false);
    cJSON_Delete(root);
}
static bool timestamp_field(cJSON *root, const char *key, bool nullable, int64_t *out) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (nullable && cJSON_IsNull(v)) { *out = 0; return true; }
    if (!cJSON_IsString(v)) return false;
    *out = weather_timestamp(v->valuestring); return *out != 0;
}
bool weather_forecast_valid(const weather_forecast_t *f, int64_t now) {
    int32_t fetched_day = f && f->utc_offset_seconds == INT32_MIN
        ? weather_local_day(f->fetched)
        : f ? unix_day(f->fetched + f->utc_offset_seconds) : -1;
    if (!f || !weather_clock_valid(now) || !weather_location_valid(&f->location) ||
        f->stale > 1 ||
        f->fetched < 1704067200 || f->fetched > now + 120 || f->fresh <= f->fetched ||
        f->serve < f->fresh || f->serve - f->fetched > WEATHER_MAX_AGE_SECONDS || now >= f->serve ||
        (f->utc_offset_seconds != INT32_MIN &&
         (f->utc_offset_seconds < -43200 || f->utc_offset_seconds > 50400)) ||
        f->days[0].date != fetched_day ||
        f->days[1].date != f->days[0].date + 1) return false;
    for (unsigned i = 0; i < 2; ++i) {
        const weather_day_t *d = &f->days[i];
        if (!isfinite(d->min_c) || !isfinite(d->max_c) || d->min_c < -90 || d->max_c > 65 ||
            d->min_c > d->max_c || d->humidity < 0 || d->humidity > 100 || d->code < 0 || d->code > 999 ||
            !memchr(d->condition, 0, sizeof(d->condition)) || !text_ok(d->condition, sizeof(d->condition), false) ||
            (d->sunrise && !weather_clock_valid(d->sunrise)) ||
            (d->sunset && !weather_clock_valid(d->sunset)) ||
            (d->sunrise && d->sunset && d->sunrise >= d->sunset)) return false;
    }
    return true;
}

static bool daily_number(cJSON *daily, const char *name, int index, double low, double high, double *out) {
    cJSON *array = cJSON_GetObjectItemCaseSensitive(daily, name);
    cJSON *item = cJSON_GetArrayItem(array, index);
    if (!cJSON_IsArray(array) || !cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < low || item->valuedouble > high) return false;
    *out = item->valuedouble;
    return true;
}

static bool daily_time(cJSON *daily, const char *name, int index, int64_t *out) {
    double value;
    if (!daily_number(daily, name, index, 1704067200, 4102444799.0, &value) || floor(value) != value)
        return false;
    *out = (int64_t)value;
    return true;
}

static const char *wmo_text(int code) {
    switch (code) {
        case 0: return "晴朗";
        case 1: return "大致晴朗";
        case 2: return "局部多雲";
        case 3: return "陰天";
        case 45: return "有霧";
        case 48: return "霧淞";
        case 51: return "毛毛雨";
        case 53: return "中度毛毛雨";
        case 55: return "強毛毛雨";
        case 56: return "輕度凍毛毛雨";
        case 57: return "強凍毛毛雨";
        case 61: return "小雨";
        case 63: return "中雨";
        case 65: return "大雨";
        case 66: return "輕度凍雨";
        case 67: return "強凍雨";
        case 71: return "小雪";
        case 73: return "中雪";
        case 75: return "大雪";
        case 77: return "雪粒";
        case 80: return "小陣雨";
        case 81: return "中陣雨";
        case 82: return "強陣雨";
        case 85: return "小陣雪";
        case 86: return "強陣雪";
        case 95: return "雷雨";
        case 96: return "雷雨伴小冰雹";
        case 97: return "強雷雨";
        case 99: return "雷雨伴大冰雹";
        default: return NULL;
    }
}

weather_parse_result_t weather_openmeteo_forecast_parse(const char *json, size_t length, int64_t now,
                           const weather_location_t *selected, weather_forecast_t *out) {
    cJSON *root = document(json, length);
    weather_forecast_t forecast = {0};
    weather_parse_result_t result = WEATHER_PARSE_DOCUMENT;
    cJSON *daily = root ? cJSON_GetObjectItemCaseSensitive(root, "daily") : NULL;
    double offset;
    if (!root) goto done;
    result = WEATHER_PARSE_LOCATION;
    if (!selected || !isfinite(selected->lat) || !isfinite(selected->lon) ||
        fabs(selected->lat) > 90 || fabs(selected->lon) > 180 ||
        !number(root, "latitude", -90, 90, &forecast.location.lat) ||
        !number(root, "longitude", -180, 180, &forecast.location.lon) ||
        fabs(forecast.location.lat - selected->lat) > 0.15 || fabs(forecast.location.lon - selected->lon) > 0.15 ||
        !string_field(root, "timezone", forecast.location.timezone, sizeof(forecast.location.timezone), false) ||
        !app_timezone_posix_rule(forecast.location.timezone)) goto done;
    forecast.location = *selected;
    if (!string_field(root, "timezone", forecast.location.timezone, sizeof(forecast.location.timezone), false)) goto done;
    result = WEATHER_PARSE_DAYS;
    /* utc_offset_seconds is a top-level integer; daily arrays use Unix seconds. */
    cJSON *offset_value = cJSON_GetObjectItemCaseSensitive(root, "utc_offset_seconds");
    cJSON *daily_dates = cJSON_GetObjectItemCaseSensitive(daily, "time");
    if (!cJSON_IsObject(daily) || !cJSON_IsArray(daily_dates) || cJSON_GetArraySize(daily_dates) != 2 ||
        !cJSON_IsNumber(offset_value) || !isfinite(offset_value->valuedouble) ||
        floor(offset_value->valuedouble) != offset_value->valuedouble ||
        offset_value->valuedouble < -50400 || offset_value->valuedouble > 50400) goto done;
    offset = offset_value->valuedouble;
    forecast.utc_offset_seconds = (int32_t)offset;
    cJSON *codes = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
    if (!cJSON_IsArray(codes) || cJSON_GetArraySize(codes) != 2) goto done;
    for (int i = 0; i < 2; ++i) {
        weather_day_t *day = &forecast.days[i];
        int code;
        double humidity, daily_epoch;
        if (!daily_number(daily, "time", i, 1704067200, 4102444799.0, &daily_epoch) ||
            floor(daily_epoch) != daily_epoch ||
            (day->date = unix_day((int64_t)daily_epoch + forecast.utc_offset_seconds)) < 0 ||
            !daily_number(daily, "temperature_2m_min", i, -90, 65, &day->min_c) ||
            !daily_number(daily, "temperature_2m_max", i, -90, 65, &day->max_c) ||
            !daily_number(daily, "relative_humidity_2m_mean", i, 0, 100, &humidity) ||
            floor(humidity) != humidity || !daily_number(daily, "weather_code", i, 0, 99, &daily_epoch) ||
            floor(daily_epoch) != daily_epoch) goto done;
        cJSON *sunrise = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "sunrise"), i);
        cJSON *sunset = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "sunset"), i);
        if ((cJSON_IsNumber(sunrise) && !daily_time(daily, "sunrise", i, &day->sunrise)) ||
            (!cJSON_IsNumber(sunrise) && !cJSON_IsNull(sunrise)) ||
            (cJSON_IsNumber(sunset) && !daily_time(daily, "sunset", i, &day->sunset)) ||
            (!cJSON_IsNumber(sunset) && !cJSON_IsNull(sunset))) goto done;
        code = (int)daily_epoch;
        day->humidity = (int)humidity;
        day->code = code;
        const char *condition = wmo_text(code);
        if (!condition) goto done;
        snprintf(day->condition, sizeof(day->condition), "%s", condition);
    }
    result = WEATHER_PARSE_CLOCK;
    if (!weather_clock_valid(now)) goto done;
    forecast.fetched = now;
    forecast.fresh = now + 1800;
    forecast.serve = now + WEATHER_MAX_AGE_SECONDS;
    result = WEATHER_PARSE_VALIDITY;
    if (!weather_forecast_valid(&forecast, now)) goto done;
    *out = forecast;
    result = WEATHER_PARSE_OK;
done:
    cJSON_Delete(root);
    return result;
}
weather_parse_result_t weather_forecast_parse_result(const char *json, size_t length, int64_t now,
                           const weather_location_t *selected, weather_forecast_t *out) {
    cJSON *root = document(json, length); weather_forecast_t f = {0}; int schema; bool stale = false;
    cJSON *days = cJSON_GetObjectItemCaseSensitive(root, "days");
    char source[16];
    weather_parse_result_t result = WEATHER_PARSE_DOCUMENT;
    if (!root) goto done;
    result = WEATHER_PARSE_LOCATION;
    if (!weather_location_valid(selected)) goto done;
    result = WEATHER_PARSE_SCHEMA;
    if (!integer(root, "schemaVersion", 1, 1, &schema) ||
        !string_field(root, "source", source, sizeof(source), false) || strcmp(source, "QWeather")) goto done;
    result = WEATHER_PARSE_LOCATION;
    if (!point(cJSON_GetObjectItemCaseSensitive(root, "location"), &f.location) ||
        strcmp(f.location.id, selected->id) || strcmp(f.location.timezone, selected->timezone) ||
        fabs(f.location.lat - selected->lat) > .011 || fabs(f.location.lon - selected->lon) > .011) goto done;
    result = WEATHER_PARSE_DAYS;
    if (!cJSON_IsArray(days) || cJSON_GetArraySize(days) != 2) goto done;
    result = WEATHER_PARSE_TIMESTAMPS;
    if (!timestamp_field(root, "fetchedAt", false, &f.fetched) ||
        !timestamp_field(root, "freshUntil", false, &f.fresh) ||
        !timestamp_field(root, "serveUntil", false, &f.serve) || !boolean(root, "stale", &stale)) goto done;
    f.location = *selected; f.stale = stale; f.utc_offset_seconds = INT32_MIN;
    result = WEATHER_PARSE_DAYS;
    for (int i = 0; i < 2; ++i) {
        cJSON *item = cJSON_GetArrayItem(days, i); char date[11], code[4]; weather_day_t *d = &f.days[i];
        bool ok = string_field(item, "date", date, sizeof(date), false) && (d->date = parse_date(date)) >= 0 &&
            number(item, "minC", -90, 65, &d->min_c) && number(item, "maxC", -90, 65, &d->max_c) &&
            integer(item, "dayHumidityPct", 0, 100, &d->humidity) &&
            string_field(item, "conditionCode", code, sizeof(code), false) && strlen(code) == 3 &&
            (d->code = digits(code, 3)) >= 100 && string_field(item, "conditionText", d->condition, sizeof(d->condition), false) &&
            timestamp_field(item, "sunrise", true, &d->sunrise) && timestamp_field(item, "sunset", true, &d->sunset);
        if (!ok) goto done;
    }
    result = WEATHER_PARSE_CLOCK;
    if (!weather_clock_valid(now) || f.fetched > now + 120) goto done;
    result = WEATHER_PARSE_EXPIRED;
    if (now >= f.serve) goto done;
    result = WEATHER_PARSE_VALIDITY;
    if (!weather_forecast_valid(&f, now)) goto done;
    *out = f;
    result = WEATHER_PARSE_OK;
done:
    cJSON_Delete(root); return result;
}
bool weather_forecast_parse(const char *json, size_t length, int64_t now,
                           const weather_location_t *selected, weather_forecast_t *out) {
    return weather_forecast_parse_result(json, length, now, selected, out) == WEATHER_PARSE_OK;
}
const char *weather_parse_result_name(weather_parse_result_t result) {
    static const char *const names[] = {"ok", "document", "schema", "location", "timestamps",
        "days", "clock", "expired", "validity"};
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "unknown";
}
const weather_day_t *weather_day_for(const weather_forecast_t *f, int64_t now, bool tomorrow) {
    if (!weather_forecast_valid(f, now)) return NULL;
    int32_t date = weather_local_day(now) + tomorrow;
    for (unsigned i = 0; i < 2; ++i) if (f->days[i].date == date) return &f->days[i];
    return NULL;
}
weather_scene_t weather_scene(int code) {
    /* Keep the legacy cached QWeather scene IDs readable while new fetches use WMO codes. */
    if (code == 100 || code == 150) return WEATHER_SCENE_SUN;
    if (code == 104) return WEATHER_SCENE_OVERCAST;
    if ((code >= 101 && code <= 103) || (code >= 151 && code <= 153)) return WEATHER_SCENE_CLOUD;
    if (code >= 302 && code <= 304) return WEATHER_SCENE_STORM;
    if (code >= 300 && code <= 399) return WEATHER_SCENE_RAIN;
    if (code >= 404 && code <= 406) return WEATHER_SCENE_SLEET;
    if (code >= 400 && code <= 499) return WEATHER_SCENE_SNOW;
    if (code == 503 || code == 504 || code == 507 || code == 508) return WEATHER_SCENE_DUST;
    if (code == 500 || code == 501 || code == 502 || code == 509 || code == 510 ||
        (code >= 511 && code <= 515)) return WEATHER_SCENE_FOG;
    if (code == 0 || code == 1) return WEATHER_SCENE_SUN;
    if (code == 3) return WEATHER_SCENE_OVERCAST;
    if (code == 2) return WEATHER_SCENE_CLOUD;
    if (code == 95 || code == 96 || code == 97 || code == 99) return WEATHER_SCENE_STORM;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return WEATHER_SCENE_RAIN;
    if (code == 77 || (code >= 71 && code <= 75) || code == 85 || code == 86) return WEATHER_SCENE_SNOW;
    if (code == 45 || code == 48) return WEATHER_SCENE_FOG;
    return WEATHER_SCENE_NEUTRAL;
}
bool weather_url_encode(const char *text, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF"; size_t pos = 0;
    if (!cap || !text) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (pos + (plain ? 1 : 3) >= cap) { out[0] = 0; return false; }
        if (plain) out[pos++] = (char)*p;
        else { out[pos++] = '%'; out[pos++] = hex[*p >> 4]; out[pos++] = hex[*p & 15]; }
    }
    out[pos] = 0; return true;
}
bool weather_query_valid(const char *text) {
    if (!text_ok(text, 193, false)) return false;
    unsigned count = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if ((*p & 192) != 128) ++count;
    return count >= 2 && count <= 64;
}
