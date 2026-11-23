/*
 * 中文：平台天气代理、独占短连和本地两日缓存；搜索批次仅暂存RAM，不保存密钥或完整城市字典。
 * English: Platform weather proxy, exclusive short connections and two-day cache; search batches stay in RAM, without keys or a full city dictionary.
 * 冻结：不抢占传书/升级；离页仅通知取消，防止弱网阻塞导航；休眠/关机前仍确认交还。按用户要求，非天气页空闲时可借用已就绪网络静默刷新。
 * Frozen: Never steal transfer/update sessions. Navigation signals cancellation without waiting on weak links; sleep/shutdown still require handoff. Per user request, idle non-weather pages may refresh quietly over an already-ready link.
 */
#include "weather_service.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls_errors.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "epdiy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "ota_online.h"
#include "device_power.h"
#include "ble_page_turner.h"
#include "memory_peak.h"
#include "memory_manager.h"
#include "time_service.h"
#include "wifi_transfer.h"
#include "network_service.h"
#include "network_http.h"
#include "../time/timezone.h"
#include "../diagnostics/network_diagnostics.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define WEATHER_OPENMETEO_ORIGIN "https://api.open-meteo.com/v1/forecast"
#define WEATHER_GEOCODING_ORIGIN "https://geocoding-api.open-meteo.com/v1/search"
#define WEATHER_BEACONDB_ORIGIN "https://api.beacondb.net/v1/geolocate"
#define WEATHER_NOMINATIM_ORIGIN "https://nominatim.openstreetmap.org/reverse"
#define WEATHER_USER_AGENT "Fan/1.1.8 (e-reader weather client)"
#define WEATHER_DEFER_US (5LL * 1000000)
#define WEATHER_RETRY_US (5LL * 60 * 1000000)
#define WEATHER_SESSION_US (45LL * 1000000)
#define WEATHER_CONNECT_US (6LL * 1000000)
#define WEATHER_RESPONSE_US (5LL * 1000000)
#define WEATHER_CELLULAR_CONNECT_US (20LL * 1000000)
#define WEATHER_CELLULAR_RESPONSE_US (10LL * 1000000)
#define WEATHER_IO_POLL_MS 200
#define WEATHER_TASK_STACK_BYTES 8192u
typedef struct weather_search_batch {
    struct weather_search_batch *next;
    unsigned page, count;
    weather_location_t locations[WEATHER_SEARCH_BATCH_SIZE];
} weather_search_batch_t;
typedef struct {
    uint32_t schema;
    uint8_t enabled, has_city, auto_refresh, auto_location;
    weather_location_t city;
} weather_preferences_t;
typedef struct {
    uint32_t schema;
    uint8_t enabled, has_city, auto_refresh;
    struct {
        char id[17], admin2[97];
        char name[97], admin1[97], country[49], timezone[33];
        double lat, lon;
    } city;
} weather_preferences_v3_t;
typedef struct {
    uint32_t schema;
    weather_forecast_t forecast;
} weather_cache_t;
typedef struct {
    int64_t utc, at_us;
} weather_clock_t;
typedef struct {
    weather_clock_t clock;
    bool seen;
} weather_date_header_t;
typedef struct {
    app_weather_state_t state;
    char notice[97];
    bool has_city, has_forecast, clear_cache, has_auto_location, auto_location;
    weather_location_t city;
    weather_forecast_t forecast;
    weather_clock_t clock;
    unsigned total;
    weather_search_batch_t *batches;
} weather_result_t;
static app_weather_view_t s_view;
static weather_result_t s_result;
static app_weather_request_t s_request;
static network_use_t s_network_use;
static char s_query[193];
static char s_cached_query[193];
static weather_search_batch_t *s_search_batches;
static bool s_search_total_known, s_requested_total_known;
static unsigned s_search_total, s_requested_total, s_requested_missing[2], s_requested_missing_count;
static weather_location_t s_requested_city;
static bool s_requested_has_city, s_requested_auto_location, s_background_cache, s_loaded, s_pending;
static bool s_auto_attempted;
static unsigned s_search_generation, s_requested_search_generation, s_requested_page;
static atomic_bool s_busy, s_cancel;
static bool s_fetch_after_cancel;
static SemaphoreHandle_t s_done;
static int64_t s_next_attempt, s_deadline;
static int32_t s_display_day;
static bool s_display_valid;
static weather_clock_t s_clock, s_requested_clock;
static network_http_session_t s_http_session;

static int64_t clock_now(const weather_clock_t *clock) {
    if (!clock->utc) return time(NULL);
    int64_t elapsed = esp_timer_get_time() - clock->at_us;
    return elapsed >= 0 ? clock->utc + elapsed / 1000000 : 0;
}
int64_t app_weather_now(void) { return clock_now(&s_clock); }

static bool persist_blob(const char *key, const void *value, size_t size, bool cancellable) {
    if (!epd_flash_begin()) return false;
    // 取得面板安全锁后再检查取消，避免旧城市请求晚于新设置写入。
    // Recheck cancellation under the panel guard so an old city's request cannot write after new preferences.
    if (cancellable && atomic_load(&s_cancel)) { epd_flash_end(); return false; }
    nvs_handle_t h;
    esp_err_t err = nvs_open("kiiko_weather", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = value ? nvs_set_blob(h, key, value, size) : nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND && !value) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    epd_flash_end();
    return err == ESP_OK;
}
static bool write_blob(const char *key, const void *value, size_t size) {
    return persist_blob(key, value, size, false);
}
static bool save_preferences(bool enabled, bool auto_refresh, bool auto_location,
                             bool has_city, const weather_location_t *city) {
    weather_preferences_t p = {.schema = 4, .enabled = enabled, .has_city = has_city,
        .auto_refresh = auto_refresh, .auto_location = auto_location};
    if (has_city) p.city = *city;
    return write_blob("prefs", &p, sizeof(p));
}
static weather_location_t fallback_location(void) {
    weather_location_t city = {0};
    snprintf(city.id, sizeof(city.id), "FALLBACK");
    snprintf(city.name, sizeof(city.name), "Vancouver");
    snprintf(city.admin1, sizeof(city.admin1), "British Columbia");
    snprintf(city.country, sizeof(city.country), "Canada");
    snprintf(city.timezone, sizeof(city.timezone), "America/Vancouver");
    city.lat = 49.2827; city.lon = -123.1207;
    return city;
}
void app_weather_init(void) {
    if (s_loaded) return;
    s_loaded = true;
    s_clock = (weather_clock_t){0};
    s_view.state = APP_WEATHER_IDLE;
    s_view.auto_refresh = true;
    s_view.auto_location = true;
    s_view.city = fallback_location();
    s_view.has_city = true;
    snprintf(s_view.notice, sizeof(s_view.notice), "自動定位尚未成功，暫用 Vancouver, BC");
    nvs_handle_t h;
    if (nvs_open("kiiko_weather", NVS_READONLY, &h) == ESP_OK) {
        weather_preferences_t p = {0}; size_t size = sizeof(p);
        bool loaded = nvs_get_blob(h, "prefs", &p, &size) == ESP_OK && size == sizeof(p) && p.schema == 4 &&
            p.enabled <= 1 && p.has_city <= 1 && p.auto_refresh <= 1 &&
            p.auto_location <= 1 && (!p.has_city || weather_location_valid(&p.city));
        if (loaded) {
            s_view.enabled = p.enabled; s_view.has_city = p.has_city;
            s_view.city = p.has_city ? p.city : fallback_location();
            s_view.auto_refresh = p.auto_refresh; s_view.auto_location = p.auto_location;
            s_view.notice[0] = 0;
        } else {
            weather_preferences_v3_t old = {0}; size = sizeof(old);
            if (nvs_get_blob(h, "prefs", &old, &size) == ESP_OK && size == sizeof(old) && old.schema == 3 &&
                old.enabled <= 1 && old.has_city <= 1 && old.auto_refresh <= 1) {
                s_view.enabled = old.enabled; s_view.has_city = old.has_city;
                s_view.city = fallback_location();
                bool text_valid = memchr(old.city.id, 0, sizeof(old.city.id)) &&
                    memchr(old.city.admin2, 0, sizeof(old.city.admin2)) &&
                    memchr(old.city.name, 0, sizeof(old.city.name)) &&
                    memchr(old.city.admin1, 0, sizeof(old.city.admin1)) &&
                    memchr(old.city.country, 0, sizeof(old.city.country)) &&
                    memchr(old.city.timezone, 0, sizeof(old.city.timezone));
                if (old.has_city && text_valid) {
                    snprintf(s_view.city.id, sizeof(s_view.city.id), "%s", old.city.id);
                    snprintf(s_view.city.admin2, sizeof(s_view.city.admin2), "%s", old.city.admin2);
                    snprintf(s_view.city.name, sizeof(s_view.city.name), "%s", old.city.name);
                    snprintf(s_view.city.admin1, sizeof(s_view.city.admin1), "%s", old.city.admin1);
                    snprintf(s_view.city.country, sizeof(s_view.city.country), "%s", old.city.country);
                    snprintf(s_view.city.timezone, sizeof(s_view.city.timezone), "%s", old.city.timezone);
                    s_view.city.lat = old.city.lat; s_view.city.lon = old.city.lon;
                }
                if (old.has_city && (!text_valid || !weather_location_valid(&s_view.city))) {
                    s_view.has_city = false; s_view.auto_location = true;
                    s_view.city = fallback_location();
                }
                s_view.auto_refresh = old.auto_refresh; s_view.auto_location = !old.has_city;
                s_view.notice[0] = 0;
            }
        }
        weather_cache_t cache = {0};size = sizeof(cache);
        if (s_view.has_city && nvs_get_blob(h, "cache", &cache, &size) == ESP_OK &&
            size == sizeof(cache) && cache.schema == 2 && weather_forecast_valid(&cache.forecast, time(NULL)) &&
            !strcmp(s_view.city.id, cache.forecast.location.id)) {
            s_view.forecast = cache.forecast;s_view.has_forecast = true;
        }
        nvs_close(h);
    }
    if (!app_timezone_set_iana(s_view.city.timezone)) {
        s_view.city = fallback_location();
        s_view.auto_location = true;
        snprintf(s_view.notice, sizeof(s_view.notice), "地點時區資料不支援，暫用 Vancouver, BC");
        (void)app_timezone_set_iana(s_view.city.timezone);
    }
    s_display_day = weather_local_day(time(NULL));
    s_display_valid = s_view.has_forecast;
}
const app_weather_view_t *app_weather_view(void) { return &s_view; }
bool app_weather_busy(void) { return atomic_load(&s_busy); }
static bool cancelled(void) {
    return atomic_load(&s_cancel) || esp_timer_get_time() >= s_deadline;
}
static void result_notice(app_weather_state_t state, const char *message) {
    s_result.state = state; snprintf(s_result.notice, sizeof(s_result.notice), "%s", message);
}
static void api_error(int status, const char *body) {
    char code[65];weather_error_code(body,strlen(body),code,sizeof(code));
    if (!strcmp(code, "COMPONENT_DISABLED")) {
        s_result.clear_cache = true; result_notice(APP_WEATHER_DISABLED, "天氣服務暫未開放");
    } else if (!strcmp(code, "CITY_SELECTION_REQUIRED")) {
        result_notice(APP_WEATHER_CITY_REQUIRED, "未能確定城市，請手動選擇");
    } else if (status == 429 || !strcmp(code, "WEATHER_COOLDOWN") ||
               !strcmp(code, "WEATHER_REFRESH_IN_PROGRESS")) {
        result_notice(APP_WEATHER_UNAVAILABLE, "請求較多，請稍後重試");
    } else result_notice(APP_WEATHER_UNAVAILABLE, "天氣服務暫不可用，請稍後重試");
}
static int http_wait_ms(int64_t until) {
    if (until > s_deadline) until = s_deadline;
    int64_t remaining = (until - esp_timer_get_time() + 999) / 1000;
    return remaining > WEATHER_IO_POLL_MS ? WEATHER_IO_POLL_MS : (int)remaining;
}
static esp_err_t weather_date_header(esp_http_client_event_t *event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data || !event->header_key ||
        strcasecmp(event->header_key, "Date")) return ESP_OK;
    weather_date_header_t *header = event->user_data;
    header->clock.utc = header->seen ? 0 : weather_http_date(event->header_value);
    header->clock.at_us = esp_timer_get_time();
    header->seen = true;
    return ESP_OK;
}
static bool http_request_once(const char *url, esp_http_client_method_t method,
                              const char *content_type, const char *request_body,
                              const char *user_agent, char *body, weather_clock_t *response_clock,
                              network_bearer_t bearer, bool *retryable) {
    *retryable = false;
    body[0] = 0;
    weather_date_header_t header = {0};
    if (response_clock) *response_clock = (weather_clock_t){0};
    if (!url) return false;
    size_t url_length = strnlen(url, 1024);
    if (url_length < 9 || url_length >= 1024 || strncmp(url, "https://", 8) ||
        strchr(url, '\r') || strchr(url, '\n') || strchr(url, ' ') || strchr(url, '#') || cancelled()) return false;
    size_t request_length = request_body ? strlen(request_body) : 0;
    const int64_t connect_us = bearer == NETWORK_BEARER_CELLULAR
        ? WEATHER_CELLULAR_CONNECT_US : WEATHER_CONNECT_US;
    const int64_t response_us = bearer == NETWORK_BEARER_CELLULAR
        ? WEATHER_CELLULAR_RESPONSE_US : WEATHER_RESPONSE_US;
    int64_t started = esp_timer_get_time();
    esp_http_client_config_t config = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = (int)(connect_us / 1000), .is_async = true,
        .disable_auto_redirect = true, .buffer_size = 1024, .buffer_size_tx = 1024,
        .event_handler = response_clock ? weather_date_header : NULL, .user_data = &header,
    };
    esp_err_t init_error;
    esp_http_client_handle_t client = network_http_client_init(&config, bearer, &s_http_session, &init_error);
    if (!client) {
        if (init_error == ESP_ERR_NO_MEM) result_notice(APP_WEATHER_FAILED, "記憶體不足，請稍後重試");
        *retryable = init_error == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME && !cancelled();
        network_diagnostics_http_failure("weather", NULL, "init", init_error, bearer);
        return false;
    }
    esp_http_client_set_method(client, method);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (content_type) esp_http_client_set_header(client, "Content-Type", content_type);
    if (user_agent) esp_http_client_set_header(client, "User-Agent", user_agent);
    bool ok = false;
    bool transport_error;
    const char *phase = "open";
    long result = ESP_FAIL;
    esp_err_t opened = ESP_FAIL;
    while (!cancelled() && esp_timer_get_time() - started < connect_us) {
        opened = esp_http_client_open(client, request_length);
        if (opened != ESP_ERR_HTTP_EAGAIN) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    transport_error = opened != ESP_OK;
    result = opened;
    if (!cancelled() && opened == ESP_OK) {
        size_t sent = 0;
        int64_t write_until = esp_timer_get_time() + response_us;
        while (!cancelled() && sent < request_length) {
            int wait = http_wait_ms(write_until);
            if (wait <= 0) { result = ESP_ERR_TIMEOUT; break; }
            esp_err_t timeout_result = esp_http_client_set_timeout_ms(client, wait);
            if (timeout_result != ESP_OK) { result = timeout_result; break; }
            int n = esp_http_client_write(client, request_body + sent, (int)(request_length - sent));
            result = n;
            if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            if (n <= 0) break;
            sent += (size_t)n;
            write_until = esp_timer_get_time() + response_us;
        }
        if (sent != request_length) {
            transport_error = true;
            phase = "request-body";
        }
        if (sent == request_length && !cancelled()) {
            phase = "headers";
            int64_t until = esp_timer_get_time() + response_us;
            int64_t length = -1;
            // Short I/O waits stay cancellable within the selected bearer's response window.
            while (!cancelled()) {
                int wait = http_wait_ms(until);
                if (wait <= 0) { result = ESP_ERR_TIMEOUT; break; }
                esp_err_t timeout_result = esp_http_client_set_timeout_ms(client, wait);
                if (timeout_result != ESP_OK) { result = timeout_result; break; }
                length = esp_http_client_fetch_headers(client);
                result = (long)length;
                if (length != -ESP_ERR_HTTP_EAGAIN) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            transport_error = length < 0 || esp_timer_get_time() >= until;
            if (!cancelled() && esp_timer_get_time() < until && length >= 0 && length <= WEATHER_BODY_LIMIT) {
                phase = "body";
                transport_error = true;
                size_t used = 0;
                until = esp_timer_get_time() + response_us;
                while (!cancelled() && used < WEATHER_BODY_LIMIT) {
                    int wait = http_wait_ms(until);
                    if (wait <= 0) { result = ESP_ERR_TIMEOUT; break; }
                    esp_err_t timeout_result = esp_http_client_set_timeout_ms(client, wait);
                    if (timeout_result != ESP_OK) { result = timeout_result; break; }
                    size_t chunk = WEATHER_BODY_LIMIT - used;
                    if (chunk > 512) chunk = 512;
                    int n = esp_http_client_read(client, body + used, (int)chunk);
                    result = n;
                    if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
                    if (n < 0) break;
                    if (!n) { ok = esp_http_client_is_complete_data_received(client); break; }
                    used += (size_t)n;
                    until = esp_timer_get_time() + response_us;
                }
                body[used] = 0;
                if (used == WEATHER_BODY_LIMIT) ok = esp_http_client_is_complete_data_received(client);
                if (ok) transport_error = false;
                if (ok && !cancelled() && !memchr(body, 0, used)) {
                    int status = esp_http_client_get_status_code(client);
                    if (status != 200) {
                        phase = "status"; result = status;
                        api_error(status, body); ok = false;
                    }
                } else ok = false;
            }
        }
    }
    ok = ok && !cancelled();
    *retryable = !ok && !cancelled() && transport_error;
    if (!cancelled()) network_http_note_result(&s_http_session, !transport_error);
    if (!ok && !atomic_load(&s_cancel))
        network_diagnostics_http_failure("weather", client, phase, result, bearer);
    if (ok) network_diagnostics_http_response("weather", client, strlen(body));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (ok && response_clock) *response_clock = header.clock;
    return ok;
}
static bool http_request(const char *url, esp_http_client_method_t method,
                         const char *content_type, const char *request_body,
                         const char *user_agent, char *body, weather_clock_t *response_clock,
                         network_bearer_t bearer) {
    const unsigned attempts = bearer == NETWORK_BEARER_CELLULAR ? 2 : 1;
    for (unsigned attempt = 0; attempt < attempts && !cancelled(); ++attempt) {
        bool retryable;
        if (http_request_once(url, method, content_type, request_body, user_agent,
                              body, response_clock, bearer, &retryable)) return true;
        if (!retryable || attempt + 1 == attempts || cancelled()) break;
        // 同一租约内仅重新建立一次连接；不重试业务错误，也不延长整个会话。
        // Retry one fresh connection on the same lease, within the original session deadline.
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}
static bool http_get(const char *url, char *body, weather_clock_t *response_clock,
                     network_bearer_t bearer) {
    return http_request(url, HTTP_METHOD_GET, NULL, NULL, NULL, body, response_clock, bearer);
}
static void search_free_batches(weather_search_batch_t **list) {
    while (*list) {
        weather_search_batch_t *next = (*list)->next;
        heap_caps_free(*list);
        *list = next;
    }
}
static weather_search_batch_t *search_batch(unsigned page) {
    for (weather_search_batch_t *batch = s_search_batches; batch; batch = batch->next)
        if (batch->page == page) return batch;
    return NULL;
}
static bool search_show_page(unsigned page) {
    unsigned offset = page * WEATHER_CITY_LIMIT;
    if (!s_search_total_known || (s_search_total ? offset >= s_search_total : page != 0)) return false;
    unsigned count = s_search_total ? s_search_total - offset : 0;
    if (count > WEATHER_CITY_LIMIT) count = WEATHER_CITY_LIMIT;
    for (unsigned i = 0; i < count; ++i) {
        weather_search_batch_t *batch = search_batch((offset + i) / WEATHER_SEARCH_BATCH_SIZE);
        if (!batch || (offset + i) % WEATHER_SEARCH_BATCH_SIZE >= batch->count) return false;
    }
    memset(s_view.results, 0, sizeof(s_view.results));
    for (unsigned i = 0; i < count; ++i) {
        weather_search_batch_t *batch = search_batch((offset + i) / WEATHER_SEARCH_BATCH_SIZE);
        s_view.results[i] = batch->locations[(offset + i) % WEATHER_SEARCH_BATCH_SIZE];
    }
    s_view.result_count = count; s_view.result_total = s_search_total; s_view.result_page = page;
    s_view.state = APP_WEATHER_READY;
    snprintf(s_view.notice, sizeof(s_view.notice), "%s", count ? "請選擇城市（自動識別僅作參考）" : "未找到城市，請換個名稱");
    ++s_view.revision;
    return true;
}

static bool wifi_scan_request_json(char *request, size_t capacity) {
    wifi_scan_config_t config = {.scan_type = WIFI_SCAN_TYPE_ACTIVE, .show_hidden = false};
    if (esp_wifi_scan_start(&config, true) != ESP_OK) return false;
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || !count) return false;
    if (count > 24) count = 24;
    wifi_ap_record_t *records = heap_caps_calloc(count, sizeof(*records), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!records) return false;
    uint16_t found = count;
    esp_err_t error = esp_wifi_scan_get_ap_records(&found, records);
    size_t used = 0;
    bool ok = error == ESP_OK && found > 0;
    if (ok) {
        int n = snprintf(request, capacity, "{\"wifiAccessPoints\":[");
        if (n < 0 || (size_t)n >= capacity) ok = false;
        else used = (size_t)n;
    }
    for (uint16_t i = 0; ok && i < found; ++i) {
        const uint8_t *mac = records[i].bssid;
        int n = snprintf(request + used, capacity - used,
            "%s{\"macAddress\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"signalStrength\":%d,\"age\":0}",
            i ? "," : "", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], records[i].rssi);
        if (n < 0 || (size_t)n >= capacity - used) ok = false;
        else used += (size_t)n;
    }
    if (ok) {
        int n = snprintf(request + used, capacity - used, "]}");
        if (n < 0 || (size_t)n >= capacity - used) ok = false;
    }
    /* The scan record contains SSIDs locally. No SSID is read or transmitted, and all scan data is erased. */
    memset(records, 0, (size_t)count * sizeof(*records));
    heap_caps_free(records);
    if (!ok) memset(request, 0, capacity);
    return ok;
}

static bool locate_wifi_city(char *request, size_t request_capacity, char *body,
                             network_bearer_t bearer, weather_location_t *city) {
    if (!wifi_scan_request_json(request, request_capacity)) return false;
    bool located = http_request(WEATHER_BEACONDB_ORIGIN, HTTP_METHOD_POST, "application/json",
        request, WEATHER_USER_AGENT, body, NULL, bearer);
    memset(request, 0, request_capacity);
    if (!located) return false;
    double latitude, longitude;
    if (!weather_beacondb_location_parse(body, strlen(body), &latitude, &longitude)) return false;
    memset(body, 0, WEATHER_BODY_LIMIT + 1);
    static int64_t last_nominatim_us;
    int64_t now_us = esp_timer_get_time();
    int64_t wait_us = 1000000 - (now_us - last_nominatim_us);
    if (last_nominatim_us && wait_us > 0) vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
    char url[256];
    if (snprintf(url, sizeof(url), "%s?format=jsonv2&lat=%.6f&lon=%.6f&zoom=10&accept-language=zh-TW",
                 WEATHER_NOMINATIM_ORIGIN, latitude, longitude) >= (int)sizeof(url)) return false;
    last_nominatim_us = esp_timer_get_time();
    if (!http_request(url, HTTP_METHOD_GET, NULL, NULL, WEATHER_USER_AGENT, body, NULL, bearer)) return false;
    return weather_nominatim_location_parse(body, strlen(body), latitude, longitude, city);
}

static bool fetch_forecast_url(const weather_location_t *city, char *url, size_t capacity) {
    return snprintf(url, capacity,
        WEATHER_OPENMETEO_ORIGIN "?latitude=%.5f&longitude=%.5f&daily=temperature_2m_min,temperature_2m_max,relative_humidity_2m_mean,weather_code,sunrise,sunset&timezone=auto&timeformat=unixtime&forecast_days=2&temperature_unit=celsius",
        city->lat, city->lon) < (int)capacity;
}

static void weather_worker(void *arg) {
    network_lease_t lease = {0};
    char *body = cancelled() ? NULL : mm_alloc(MM_OWNER_NETWORK, MM_POOL_PSRAM, WEATHER_BODY_LIMIT + 1, 0, 0);
    // 已知分配完成后再交还闸门，随后才能申请无线，不能持有名额等待网络。
    // Finish known allocations before acquiring the radio; never hold the startup slot while waiting for the network.
    memory_peak_finish((memory_peak_token_t)(uintptr_t)arg);
    result_notice(APP_WEATHER_FAILED, "天氣更新失敗，可稍後重試");
    if (cancelled()) goto done;
    if (!body) {
        result_notice(APP_WEATHER_FAILED, "記憶體不足，請稍後重試"); goto done;
    }
    if (ble_pt_connected()) {
        result_notice(APP_WEATHER_UNAVAILABLE, "翻頁器正在使用，請先關閉藍芽");
        goto done;
    }
    const esp_err_t acquire = network_service_acquire(s_network_use, &lease);
    if (acquire != ESP_OK || !network_service_wait_ready_cancelable(&lease, 30000, cancelled)) {
        const char *hint = memory_peak_hint(acquire);
        result_notice(APP_WEATHER_UNAVAILABLE, hint ? hint : "當前聯網方式不可用，請檢查網路設定");
        goto done;
    }
    network_diagnostics_snapshot("weather", lease.bearer);
    if (s_request == APP_WEATHER_SEARCH) {
        char escaped[577], url[768];
        if (!weather_url_encode(s_query, escaped, sizeof(escaped))) goto done;
        if (snprintf(url, sizeof(url), "%s?name=%s&count=%u&language=en&format=json",
                     WEATHER_GEOCODING_ORIGIN, escaped, WEATHER_CITY_LIMIT) >= (int)sizeof(url)) goto done;
        weather_search_batch_t *batch = heap_caps_calloc(1, sizeof(*batch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!batch) goto done;
        unsigned count = 0;
        bool ok = http_get(url, body, NULL, lease.bearer) && weather_openmeteo_locations_parse(
            body, strlen(body), batch->locations, WEATHER_CITY_LIMIT, &count);
        if (!ok) { heap_caps_free(batch); goto done; }
        batch->page = 0; batch->count = count;
        s_result.total = count; s_result.batches = batch;
        result_notice(APP_WEATHER_READY, ""); goto done;
    }
    weather_location_t city = s_requested_has_city ? s_requested_city : fallback_location();
    char location_notice[97] = "";
    char *request = NULL;
    bool publish_city = false;
    bool publish_auto_location = s_request == APP_WEATHER_LOCATE || s_requested_auto_location;
    if (s_request == APP_WEATHER_LOCATE || s_requested_auto_location || !s_requested_has_city) {
        request = mm_alloc(MM_OWNER_NETWORK, MM_POOL_PSRAM, 4096, 0, 0);
        if (request) memset(request, 0, 4096);
        weather_location_t located = {0};
        bool success = request && locate_wifi_city(request, 4096, body, lease.bearer, &located);
        if (request) { (void)mm_free(request); request = NULL; }
        if (success) {
            city = located;
            publish_city = true;
        } else if (!s_requested_has_city || !strcmp(city.id, "FALLBACK")) {
            city = fallback_location();
            publish_city = true;
            snprintf(location_notice, sizeof(location_notice), "Wi-Fi 定位失敗，暫用 Vancouver, BC");
        } else {
            snprintf(location_notice, sizeof(location_notice), "Wi-Fi 定位失敗，沿用上次地點");
        }
    }
    char url[768];
    if (!fetch_forecast_url(&city, url, sizeof(url))) goto done;
    weather_clock_t clock = s_requested_clock, response_clock;
    if (!http_get(url, body, &response_clock, lease.bearer)) {
        if (location_notice[0]) result_notice(APP_WEATHER_UNAVAILABLE, location_notice);
        goto done;
    }
    if (response_clock.utc && (!clock.utc || clock_now(&response_clock) > clock_now(&clock))) clock = response_clock;
    weather_parse_result_t parsed = weather_openmeteo_forecast_parse(body, strlen(body), clock_now(&clock),
                                                                      &city, &s_result.forecast);
    if (parsed != WEATHER_PARSE_OK) {
        network_diagnostics_data_failure("weather", "forecast-parse", weather_parse_result_name(parsed),
                                        strlen(body), clock_now(&clock), lease.bearer);
        goto done;
    }
    snprintf(city.timezone, sizeof(city.timezone), "%s", s_result.forecast.location.timezone);
    if (publish_city) { s_result.has_city = true; s_result.city = city; }
    if (publish_auto_location) {
        s_result.has_auto_location = true;
        s_result.auto_location = true;
    }
    s_result.clock = clock;
    s_result.has_forecast = true; result_notice(APP_WEATHER_READY, location_notice);
done:
    network_service_release(&lease);
    if (body) (void)mm_free(body);
    if (cancelled() && s_result.state == APP_WEATHER_READY) {
        s_result.has_forecast = false;
        result_notice(APP_WEATHER_FAILED, "天氣更新失敗，可稍後重試");
    }
    if (s_background_cache && !atomic_load(&s_cancel)) {
        if (s_result.has_forecast) {
            weather_cache_t cache = {.schema=2,.forecast=s_result.forecast};
            (void)persist_blob("cache", &cache, sizeof(cache), true);
        } else if (s_result.clear_cache) (void)persist_blob("cache", NULL, 0, true);
    }
    if (s_result.state != APP_WEATHER_READY) search_free_batches(&s_result.batches);
    atomic_store(&s_busy, false);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}
static bool collect_result(void) {
    if (!s_pending || app_weather_busy() || xSemaphoreTake(s_done, 0) != pdTRUE) return false;
    s_pending = false;
    if (atomic_load(&s_cancel)) {
        search_free_batches(&s_result.batches);
        return false;
    }
    if (s_request == APP_WEATHER_SEARCH && s_requested_search_generation != s_search_generation) {
        search_free_batches(&s_result.batches);
        s_view.state = APP_WEATHER_IDLE; s_view.notice[0] = 0; ++s_view.revision; return true;
    }
    s_view.state = s_result.state; snprintf(s_view.notice, sizeof(s_view.notice), "%s", s_result.notice);
    if (s_result.has_city) {
        const bool auto_location = s_result.has_auto_location ? s_result.auto_location : s_view.auto_location;
        if (save_preferences(s_view.enabled, s_view.auto_refresh, auto_location, true, &s_result.city)) {
            s_view.city = s_result.city; s_view.has_city = true; s_view.auto_location = auto_location;
            s_view.has_forecast = false;
            (void)app_timezone_set_iana(s_view.city.timezone);
        } else {
            s_result.has_forecast = false;
            snprintf(s_view.notice, sizeof(s_view.notice), "城市儲存失敗，請檢查設定儲存");
        }
    }
    if (s_result.clear_cache) {
        s_view.has_forecast = false;
        if (!s_background_cache) (void)write_blob("cache", NULL, 0);
    }
    else if (s_result.has_forecast) {
        s_clock = s_result.clock;
        s_view.forecast = s_result.forecast; s_view.has_forecast = true;
        (void)app_timezone_set_iana(s_view.forecast.location.timezone);
        if (s_view.has_city && !strcmp(s_view.city.id, s_view.forecast.location.id) &&
            strcmp(s_view.city.timezone, s_view.forecast.location.timezone)) {
            snprintf(s_view.city.timezone, sizeof(s_view.city.timezone), "%s",
                     s_view.forecast.location.timezone);
            (void)save_preferences(s_view.enabled, s_view.auto_refresh, s_view.auto_location,
                                   true, &s_view.city);
        }
        if (!s_background_cache) {
            weather_cache_t cache = {.schema=2,.forecast=s_view.forecast};
            (void)write_blob("cache", &cache, sizeof(cache));
        }
    }
    if (s_request == APP_WEATHER_SEARCH) {
        if (s_result.state == APP_WEATHER_READY) {
            while (s_result.batches) {
                weather_search_batch_t *batch = s_result.batches;
                s_result.batches = batch->next; batch->next = s_search_batches; s_search_batches = batch;
            }
            s_search_total = s_result.total; s_search_total_known = true;
            (void)search_show_page(s_requested_page);
        }
    } else {
        s_view.result_count = s_view.result_total = s_view.result_page = 0;
        memset(s_view.results, 0, sizeof(s_view.results));
    }
    ++s_view.revision; return true;
}
void app_weather_cancel(void) {
    s_fetch_after_cancel = false;
    if (!s_pending) return;
    if (!atomic_exchange(&s_cancel, true)) {
        s_view.state = APP_WEATHER_IDLE;
        s_view.notice[0] = 0;
        ++s_view.revision;
    }
    (void)collect_result();
}
void app_weather_reap_cancelled(void) {
    if (s_pending && atomic_load(&s_cancel)) (void)collect_result();
}
void app_weather_cancel_join(void) {
    app_weather_cancel();
    if (!s_pending) return;
    xSemaphoreTake(s_done, portMAX_DELAY);
    s_pending = false;
    search_free_batches(&s_result.batches);
}
bool app_weather_set_enabled(bool enabled) {
    app_weather_init(); app_weather_cancel();
    if (!save_preferences(enabled, s_view.auto_refresh, s_view.auto_location, s_view.has_city, &s_view.city)) return false;
    s_view.enabled = enabled; s_next_attempt = 0; ++s_view.revision; return true;
}
bool app_weather_set_auto_refresh(bool enabled) {
    app_weather_init(); app_weather_cancel();
    if (!save_preferences(s_view.enabled, enabled, s_view.auto_location, s_view.has_city, &s_view.city)) return false;
    s_view.auto_refresh = enabled; s_next_attempt = 0; ++s_view.revision; return true;
}
bool app_weather_select(const weather_location_t *city) {
    if (!weather_location_valid(city)) return false;
    app_weather_init(); app_weather_cancel();
    if (!save_preferences(s_view.enabled, s_view.auto_refresh, false, true, city)) return false;
    s_view.city = *city; s_view.has_city = true; s_view.has_forecast = false;
    s_view.auto_location = false;
    s_auto_attempted = true;
    (void)app_timezone_set_iana(city->timezone);
    app_weather_clear_search();
    s_view.state = APP_WEATHER_IDLE; s_view.notice[0] = 0;
    (void)write_blob("cache", NULL, 0);
    s_next_attempt = 0; ++s_view.revision; return true;
}
bool app_weather_restore_preferences(bool auto_refresh, bool has_city, const weather_location_t *city) {
    if (has_city && !weather_location_valid(city)) return false;
    app_weather_init();
    app_weather_cancel();
    if (!save_preferences(s_view.enabled, auto_refresh, !has_city, has_city, city)) return false;
    s_view.auto_refresh = auto_refresh;
    s_view.has_city = has_city;
    s_view.city = has_city ? *city : fallback_location();
    s_view.auto_location = !has_city;
    s_auto_attempted = has_city;
    if (has_city) (void)app_timezone_set_iana(city->timezone);
    s_view.has_forecast = false;
    s_display_valid = false;
    s_view.state = APP_WEATHER_IDLE;
    app_weather_clear_search();
    s_next_attempt = 0;
    return true;
}
void app_weather_clear_search(void) {
    ++s_search_generation;
    search_free_batches(&s_search_batches);
    s_cached_query[0] = 0; s_search_total_known = false; s_search_total = 0;
    if(s_view.state!=APP_WEATHER_LOADING)s_view.notice[0]=0;
    s_view.result_count = s_view.result_total = s_view.result_page = 0;
    memset(s_view.results, 0, sizeof(s_view.results)); ++s_view.revision;
}
static bool start_request(app_weather_request_t request, const char *query, unsigned page, bool manual, bool quiet) {
    app_weather_init(); (void)collect_result();
    if (s_pending || pico_online_busy() || app_time_service_busy()) {
        if (!manual) s_next_attempt = esp_timer_get_time() + WEATHER_DEFER_US;
        return false;
    }
    if (!weather_clock_valid(time(NULL))) {
        s_view.state = APP_WEATHER_UNAVAILABLE;
        snprintf(s_view.notice, sizeof(s_view.notice), "請先校準系統日期與時間"); ++s_view.revision; return false;
    }
    if (ble_pt_connected()) {
        if (manual) {
            s_view.state = APP_WEATHER_UNAVAILABLE;
            snprintf(s_view.notice, sizeof(s_view.notice), "翻頁器正在使用，請先關閉藍芽");
            ++s_view.revision;
        }
        return false;
    }
    const pmu_snapshot_t *pmu = device_power_get();
    int percent = pmu_battery_percent(pmu);
    if (percent >= 0 && percent < 10 && !pmu_battery_charging(pmu)) {
        s_view.state = APP_WEATHER_UNAVAILABLE;
        snprintf(s_view.notice, sizeof(s_view.notice), "電量較低，請充電後更新天氣"); ++s_view.revision; return false;
    }
    if (request == APP_WEATHER_SEARCH) {
        if (!weather_query_valid(query) || page >= (WEATHER_SEARCH_TOTAL_LIMIT + WEATHER_CITY_LIMIT - 1) / WEATHER_CITY_LIMIT) return false;
        if (!page || strcmp(s_cached_query, query)) {
            app_weather_clear_search(); snprintf(s_cached_query, sizeof(s_cached_query), "%s", query);
        }
        if (s_search_total_known && page * WEATHER_CITY_LIMIT >= s_search_total) return false;
        snprintf(s_query, sizeof(s_query), "%s", query);
    }
    s_requested_page = page;
    memory_peak_token_t peak;
    if (memory_peak_try_begin(MEMORY_PEAK_WEATHER, &peak) != ESP_OK) {
        if (!manual) s_next_attempt = esp_timer_get_time() + WEATHER_DEFER_US;
        return false;
    }
    const esp_err_t capacity = memory_peak_check((memory_peak_heap_t){
        .internal_free = WEATHER_TASK_STACK_BYTES, .internal_block = WEATHER_TASK_STACK_BYTES,
        .psram_free = WEATHER_BODY_LIMIT + 1 + 4096 + 24 * sizeof(wifi_ap_record_t),
        .psram_block = WEATHER_BODY_LIMIT + 1 + 4096 + 24 * sizeof(wifi_ap_record_t),
    });
    if (capacity != ESP_OK) {
        memory_peak_finish(peak);
        if (manual) {
            s_view.state = APP_WEATHER_FAILED;
            snprintf(s_view.notice, sizeof(s_view.notice), "記憶體不足，請稍後重試");
            ++s_view.revision;
        }
        return false;
    }
    if (!s_done) s_done = xSemaphoreCreateBinary();
    if (!s_done) { memory_peak_finish(peak); return false; }
    s_request = request; s_requested_city = s_view.city; s_requested_has_city = s_view.has_city;
    s_requested_auto_location = s_view.auto_location;
    s_background_cache = !manual && request == APP_WEATHER_FETCH && s_requested_has_city;
    s_network_use = quiet ? NETWORK_USE_WEATHER_BACKGROUND :
        manual && request == APP_WEATHER_FETCH ? NETWORK_USE_WEATHER_MANUAL : NETWORK_USE_WEATHER;
    s_requested_clock = s_clock;
    if(request==APP_WEATHER_LOCATE||(request==APP_WEATHER_FETCH&&!s_view.has_city))s_auto_attempted=true;
    memset(&s_result, 0, sizeof(s_result));
    s_requested_total_known = s_search_total_known; s_requested_total = s_search_total;
    s_requested_missing_count = 0;
    if (request == APP_WEATHER_SEARCH) {
        unsigned first = page * WEATHER_CITY_LIMIT / WEATHER_SEARCH_BATCH_SIZE;
        unsigned last = (page * WEATHER_CITY_LIMIT + WEATHER_CITY_LIMIT - 1) / WEATHER_SEARCH_BATCH_SIZE;
        for (unsigned batch = first; batch <= last; ++batch)
            if (!search_batch(batch)) s_requested_missing[s_requested_missing_count++] = batch;
    }
    atomic_store(&s_cancel, false); atomic_store(&s_busy, true);
    s_next_attempt = esp_timer_get_time() + WEATHER_RETRY_US;
    s_deadline = esp_timer_get_time() + WEATHER_SESSION_US;
    // TLS 的软件大整数运算可在单次调用内持续占用 CPU；与 Idle 同级确保它仍可喂看门狗。
    // TLS software bignum work may run for a long time in one call; share idle priority so Idle still runs.
    if (xTaskCreate(weather_worker, "weather", WEATHER_TASK_STACK_BYTES, (void *)(uintptr_t)peak,
                    tskIDLE_PRIORITY, NULL) != pdPASS) {
        memory_peak_finish(peak);
        atomic_store(&s_busy, false);
        s_view.state = APP_WEATHER_FAILED;
        snprintf(s_view.notice, sizeof(s_view.notice), "記憶體不足，請稍後重試"); ++s_view.revision; return false;
    }
    s_pending = true;
    s_requested_search_generation = s_search_generation;
    if (!quiet && (manual || !s_view.has_forecast || !weather_forecast_valid(&s_view.forecast, app_weather_now()))) {
        s_view.state = APP_WEATHER_LOADING; s_view.notice[0] = 0; ++s_view.revision;
    }
    return true;
}
static bool request_page(app_weather_request_t request, const char *query, unsigned page, bool manual) {
    return start_request(request, query, page, manual, false);
}
bool app_weather_request(app_weather_request_t request, const char *query) {
    if (request_page(request, query, 0, true)) return true;
    // 选城后的显式更新不能被旧请求取消吞掉；只保留一个待更新标志，不分配任务。
    // Preserve one explicit refresh after city selection without allocating a queued task.
    if (request == APP_WEATHER_FETCH && s_pending && atomic_load(&s_cancel)) {
        s_fetch_after_cancel = true;
        return true;
    }
    return false;
}
bool app_weather_search(const char *query, unsigned page) {
    app_weather_init(); (void)collect_result();
    if (s_pending || !weather_query_valid(query) || page >= (WEATHER_SEARCH_TOTAL_LIMIT + WEATHER_CITY_LIMIT - 1) / WEATHER_CITY_LIMIT) return false;
    if (!strcmp(s_cached_query, query) && search_show_page(page)) return true;
    return request_page(APP_WEATHER_SEARCH, query, page, true);
}
static bool auto_due(int64_t now, bool valid) {
    return s_view.auto_refresh && ((s_view.auto_location && !s_auto_attempted) || s_view.has_city) && !s_pending &&
        esp_timer_get_time() >= s_next_attempt && s_view.state != APP_WEATHER_CITY_REQUIRED &&
        ((s_view.auto_location && !s_auto_attempted) || !valid || now >= s_view.forecast.fetched + WEATHER_MAX_AGE_SECONDS ||
         !weather_day_for(&s_view.forecast, now, true));
}
void app_weather_background_pause(void) {
    if (s_pending && s_network_use == NETWORK_USE_WEATHER_BACKGROUND && !collect_result()) app_weather_cancel();
}
void app_weather_background_poll(void) {
    app_weather_init();
    if (s_pending && s_network_use == NETWORK_USE_WEATHER_BACKGROUND) (void)collect_result();
    if (!s_view.has_city) return;
    const int64_t now = app_weather_now();
    if (!auto_due(now, s_view.has_forecast && weather_forecast_valid(&s_view.forecast, now))) return;
    network_status_t status;
    network_service_get_status(&status);
    if (status.active_bearer == NETWORK_BEARER_NONE) return;
    s_next_attempt = esp_timer_get_time() + WEATHER_RETRY_US;
    (void)start_request(s_view.auto_location && !s_auto_attempted ? APP_WEATHER_LOCATE : APP_WEATHER_FETCH,
                        NULL, 0, false, true);
}
bool app_weather_poll(bool automatic) {
    app_weather_init(); unsigned revision = s_view.revision;
    (void)collect_result();
    if (s_fetch_after_cancel && !s_pending) {
        s_fetch_after_cancel = false;
        (void)request_page(APP_WEATHER_FETCH, NULL, 0, true);
    }
    int64_t now = app_weather_now();
    bool valid = s_view.has_forecast && weather_forecast_valid(&s_view.forecast, now);
    int32_t day = weather_local_day(now);
    if (day != s_display_day || valid != s_display_valid) {
        s_display_day = day; s_display_valid = valid; ++s_view.revision;
    }
    if (automatic && auto_due(now, valid)) {
        s_next_attempt = esp_timer_get_time() + WEATHER_RETRY_US;
        (void)request_page(s_view.auto_location && !s_auto_attempted ? APP_WEATHER_LOCATE : APP_WEATHER_FETCH,
                           NULL, 0, false);
    }
    return revision != s_view.revision;
}
