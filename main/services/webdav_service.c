/* SPDX-License-Identifier: Apache-2.0 */
#include "webdav_service.h"

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include "memory_peak.h"
#include "network_http.h"
#include "network_service.h"
#include "network_policy.h"
#include "nvs.h"

#include <ctype.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEBDAV_USER_AGENT "Fan/1.1.8 (e-reader WebDAV client)"
#define WEBDAV_NVS_NAMESPACE "fan_webdav"
#define WEBDAV_KEY_SERVER "server"
#define WEBDAV_KEY_USER "user"
#define WEBDAV_KEY_PASSWORD "password"
#define WEBDAV_XML_LIMIT 32768u
#define WEBDAV_STACK 8192u
#define WEBDAV_JOB_LIMIT_US (180LL * 1000000)
#define WEBDAV_IO_MS 250

typedef enum { JOB_NONE, JOB_LIST, JOB_DOWNLOAD, JOB_UPLOAD, JOB_MOVE, JOB_DELETE } job_t;
typedef struct {
    job_t job;
    char remote[256];
    char destination[256];
    char local[288];
} job_args_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_webdav_status_t s_status;
static app_webdav_item_t s_items[APP_WEBDAV_ITEMS];
// WebDAV listing metadata is staged outside the 8 KiB worker stack.
static EXT_RAM_BSS_ATTR app_webdav_item_t s_parse_items[APP_WEBDAV_ITEMS];
static char s_server_url[APP_WEBDAV_SERVER_MAX], s_username[64], s_password[128];
static job_args_t s_job;
static network_http_session_t s_http;
static network_bearer_t s_bearer;
static TaskHandle_t s_task;
static bool s_credentials, s_config_loaded;
static atomic_bool s_cancel;
static int64_t s_job_deadline;
static void app_webdav_worker(void *arg);

static void notice(app_webdav_state_t state, const char *message) {
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    snprintf(s_status.notice, sizeof(s_status.notice), "%s", message);
    ++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
}

void app_webdav_status(app_webdav_status_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

bool app_webdav_item(unsigned index, app_webdav_item_t *out) {
    if (!out) return false;
    portENTER_CRITICAL(&s_lock);
    bool valid = index < s_status.count && index < APP_WEBDAV_ITEMS;
    if (valid) *out = s_items[index];
    portEXIT_CRITICAL(&s_lock);
    return valid;
}

static bool credential_text(const char *value, size_t max, bool username) {
    if (!value || !value[0] || strnlen(value, max + 1) > max) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 32 || *p == 127 || (username && *p == ':')) return false;
    return true;
}

static bool server_url_valid(const char *url) {
    if (!url || strnlen(url, APP_WEBDAV_SERVER_MAX) >= APP_WEBDAV_SERVER_MAX ||
        strncmp(url, "https://", 8) || !url[8] || strchr(url, '?') || strchr(url, '#') ||
        strchr(url, '\\') || strchr(url, '@') || strchr(url, ' ')) return false;
    const char *path = strchr(url + 8, '/');
    const char *authority_end = path ? path : url + strlen(url);
    if (authority_end == url + 8 || memchr(url + 8, '@', (size_t)(authority_end - (url + 8)))) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p < 33 || *p == 127) return false;
    if (strstr(url + 8, "//")) return false;
    return true;
}

static void load_config(void) {
    if (s_config_loaded) return;
    s_server_url[0] = s_username[0] = s_password[0] = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WEBDAV_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = sizeof(s_server_url);
        (void)nvs_get_str(handle, WEBDAV_KEY_SERVER, s_server_url, &size);
        size = sizeof(s_username);
        (void)nvs_get_str(handle, WEBDAV_KEY_USER, s_username, &size);
        size = sizeof(s_password);
        (void)nvs_get_str(handle, WEBDAV_KEY_PASSWORD, s_password, &size);
        nvs_close(handle);
    }
    if (!server_url_valid(s_server_url) || !credential_text(s_username, sizeof(s_username) - 1, true) ||
        !credential_text(s_password, sizeof(s_password) - 1, false)) {
        s_server_url[0] = s_username[0] = s_password[0] = 0;
        s_credentials = false;
    } else s_credentials = true;
    s_config_loaded = true;
}

bool app_webdav_set_credentials(const char *username, const char *password) {
    load_config();
    if (!server_url_valid(s_server_url)) return false;
    return app_webdav_configure(s_server_url, username, password);
}

bool app_webdav_configure(const char *server_url, const char *username, const char *password) {
    load_config();
    if (!server_url_valid(server_url) ||
        !credential_text(username, sizeof(s_username) - 1, true) ||
        !credential_text(password, sizeof(s_password) - 1, false)) return false;
    portENTER_CRITICAL(&s_lock);
    if (s_task) { portEXIT_CRITICAL(&s_lock); return false; }
    portEXIT_CRITICAL(&s_lock);
    nvs_handle_t handle;
    if (nvs_open(WEBDAV_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(handle, WEBDAV_KEY_SERVER, server_url);
    if (err == ESP_OK) err = nvs_set_str(handle, WEBDAV_KEY_USER, username);
    if (err == ESP_OK) err = nvs_set_str(handle, WEBDAV_KEY_PASSWORD, password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return false;
    portENTER_CRITICAL(&s_lock);
    if (s_task) { portEXIT_CRITICAL(&s_lock); return false; }
    memset(s_username, 0, sizeof(s_username));
    memset(s_password, 0, sizeof(s_password));
    memset(s_server_url, 0, sizeof(s_server_url));
    snprintf(s_server_url, sizeof(s_server_url), "%s", server_url);
    snprintf(s_username, sizeof(s_username), "%s", username);
    snprintf(s_password, sizeof(s_password), "%s", password);
    s_credentials = s_config_loaded = true;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool app_webdav_get_config(char *server_url, size_t server_capacity,
                           char *username, size_t username_capacity,
                           char *password, size_t password_capacity) {
    load_config();
    portENTER_CRITICAL(&s_lock);
    bool fits = server_url && username && password && server_capacity > strlen(s_server_url) &&
        username_capacity > strlen(s_username) && password_capacity > strlen(s_password);
    if (fits) {
        snprintf(server_url, server_capacity, "%s", s_server_url);
        snprintf(username, username_capacity, "%s", s_username);
        snprintf(password, password_capacity, "%s", s_password);
    }
    portEXIT_CRITICAL(&s_lock);
    return fits && s_credentials;
}

bool app_webdav_configured(void) {
    load_config();
    portENTER_CRITICAL(&s_lock);
    bool configured = s_credentials && s_server_url[0] && s_username[0] && s_password[0];
    portEXIT_CRITICAL(&s_lock);
    return configured;
}

void app_webdav_clear_credentials(void) {
    portENTER_CRITICAL(&s_lock);
    mbedtls_platform_zeroize(s_server_url, sizeof(s_server_url));
    mbedtls_platform_zeroize(s_username, sizeof(s_username));
    mbedtls_platform_zeroize(s_password, sizeof(s_password));
    s_credentials = false;
    s_config_loaded = false;
    portEXIT_CRITICAL(&s_lock);
}

bool app_webdav_has_credentials(void) {
    load_config();
    bool ready;
    portENTER_CRITICAL(&s_lock);
    ready = s_credentials;
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

static bool remote_path_valid(const char *path) {
    if (!path || path[0] != '/' || strnlen(path, 256) >= 256 ||
        strchr(path, '\\') || strchr(path, '?') || strchr(path, '#') ||
        strstr(path, "//")) return false;
    const char *segment = path + 1;
    while (*segment) {
        const char *end = strchr(segment, '/');
        size_t length = end ? (size_t)(end - segment) : strlen(segment);
        if ((length == 1 && segment[0] == '.') || (length == 2 && segment[0] == '.' && segment[1] == '.'))
            return false;
        if (!end) break;
        segment = end + 1;
    }
    return true;
}

static bool local_path_valid(const char *path) {
    if (!path || strncmp(path, "/sdcard/", 8) || strnlen(path, 288) >= 288 || strchr(path, '\\')) return false;
    const char *segment = path + 8;
    while (*segment) {
        const char *slash = strchr(segment, '/');
        size_t length = slash ? (size_t)(slash - segment) : strlen(segment);
        if (length == 2 && segment[0] == '.' && segment[1] == '.') return false;
        if (!slash) break;
        segment = slash + 1;
    }
    return true;
}

static bool start_job(job_t job, const char *remote, const char *destination, const char *local) {
    if (!app_webdav_configured() || !remote_path_valid(remote) ||
        ((job == JOB_MOVE) && !remote_path_valid(destination)) ||
        ((job == JOB_DOWNLOAD || job == JOB_UPLOAD) && !local_path_valid(local))) return false;
    memory_peak_token_t peak = 0;
    if (memory_peak_try_begin(MEMORY_PEAK_CLOUDZAO, &peak) != ESP_OK) {
        notice(APP_WEBDAV_FAILED, "目前有其他網路或傳輸工作，請稍後重試");
        return false;
    }
    const esp_err_t memory = memory_peak_check((memory_peak_heap_t){
        .internal_free = WEBDAV_STACK, .internal_block = WEBDAV_STACK,
        .psram_free = WEBDAV_XML_LIMIT + 1, .psram_block = WEBDAV_XML_LIMIT + 1,
    });
    if (memory != ESP_OK) {
        memory_peak_finish(peak);
        notice(APP_WEBDAV_FAILED, "記憶體不足，無法連線 WebDAV");
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_task) {
        portEXIT_CRITICAL(&s_lock);
        memory_peak_finish(peak);
        return false;
    }
    s_job = (job_args_t){.job = job};
    snprintf(s_job.remote, sizeof(s_job.remote), "%s", remote);
    if (destination) snprintf(s_job.destination, sizeof(s_job.destination), "%s", destination);
    if (local) snprintf(s_job.local, sizeof(s_job.local), "%s", local);
    s_status.state = APP_WEBDAV_BUSY;
    s_status.notice[0] = 0;
    s_status.count = 0;
    ++s_status.revision;
    atomic_store(&s_cancel, false);
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(app_webdav_worker, "webdav", WEBDAV_STACK,
                    (void *)(uintptr_t)peak, tskIDLE_PRIORITY, &s_task) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_task = NULL;
        portEXIT_CRITICAL(&s_lock);
        memory_peak_finish(peak);
        notice(APP_WEBDAV_FAILED, "無法啟動 WebDAV 工作");
        return false;
    }
    return true;
}

bool app_webdav_list(const char *path) { return start_job(JOB_LIST, path, NULL, NULL); }
bool app_webdav_download(const char *remote_path, const char *local_path) {
    return start_job(JOB_DOWNLOAD, remote_path, NULL, local_path);
}
bool app_webdav_upload(const char *local_path, const char *remote_path) {
    return start_job(JOB_UPLOAD, remote_path, NULL, local_path);
}
bool app_webdav_move(const char *remote_path, const char *destination_path) {
    return start_job(JOB_MOVE, remote_path, destination_path, NULL);
}
bool app_webdav_delete(const char *remote_path) {
    return start_job(JOB_DELETE, remote_path, NULL, NULL);
}
void app_webdav_cancel(void) { atomic_store(&s_cancel, true); }

static bool cancelled(void) {
    return atomic_load(&s_cancel) || esp_timer_get_time() >= s_job_deadline;
}

static void set_auth(char *out, size_t capacity, char *user, size_t user_cap,
                     char *password, size_t password_cap) {
    portENTER_CRITICAL(&s_lock);
    snprintf(user, user_cap, "%s", s_username);
    snprintf(password, password_cap, "%s", s_password);
    portEXIT_CRITICAL(&s_lock);
    char plain[sizeof(s_username) + sizeof(s_password) + 2];
    size_t length = snprintf(plain, sizeof(plain), "%s:%s", user, password);
    size_t encoded = 0;
    if ((size_t)length < sizeof(plain) && capacity > 7 &&
        mbedtls_base64_encode((unsigned char *)out + 6, capacity - 6, &encoded,
                              (const unsigned char *)plain, (size_t)length) == 0) {
        memcpy(out, "Basic ", 6);
        out[6 + encoded] = 0;
    } else out[0] = 0;
    mbedtls_platform_zeroize(plain, sizeof(plain));
}

static int io_timeout(void) {
    return cancelled() ? 0 : WEBDAV_IO_MS;
}

static esp_err_t open_client(esp_http_client_handle_t client, int64_t length) {
    while (!cancelled()) {
        esp_http_client_set_timeout_ms(client, io_timeout());
        esp_err_t err = esp_http_client_open(client, length);
        if (err != ESP_ERR_HTTP_EAGAIN) return err;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_ERR_TIMEOUT;
}

static int64_t fetch_headers(esp_http_client_handle_t client) {
    while (!cancelled()) {
        esp_http_client_set_timeout_ms(client, io_timeout());
        int64_t length = esp_http_client_fetch_headers(client);
        if (length != -ESP_ERR_HTTP_EAGAIN) return length;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return -ESP_ERR_TIMEOUT;
}

static bool response_headers_ready(esp_http_client_handle_t client) {
    int64_t length = fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    return length >= 0 || (length == -1 && status >= 100);
}

static bool write_all(esp_http_client_handle_t client, const void *data, size_t length) {
    const unsigned char *p = data;
    while (length && !cancelled()) {
        esp_http_client_set_timeout_ms(client, io_timeout());
        int n = esp_http_client_write(client, (const char *)p, length > 2048 ? 2048 : (int)length);
        if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (n <= 0) return false;
        p += n;
        length -= (size_t)n;
    }
    return !length && !cancelled();
}

static esp_http_client_handle_t make_client(const char *url, esp_http_client_method_t method,
                                            const char *auth, const char *content_type,
                                            const char *depth, esp_err_t *error) {
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = WEBDAV_IO_MS,
        .is_async = true,
        .disable_auto_redirect = true,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = network_http_client_init(
        &config, s_bearer, &s_http, error);
    if (!client) return NULL;
    esp_http_client_set_method(client, method);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "User-Agent", WEBDAV_USER_AGENT);
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (content_type) esp_http_client_set_header(client, "Content-Type", content_type);
    if (depth) esp_http_client_set_header(client, "Depth", depth);
    if (method == HTTP_METHOD_PUT) esp_http_client_set_header(client, "If-None-Match", "*");
    return client;
}

/* Small namespace-independent XML reader, sufficient for WebDAV multistatus responses. */
typedef struct {
    const char *start;
    const char *after;
    const char *text;
    const char *text_end;
    bool closing;
    char local[32];
} xml_tag_t;
static bool xml_next_tag(const char **cursor, const char *end, xml_tag_t *tag) {
    const char *p = *cursor;
    while (p < end && *p != '<') ++p;
    if (p >= end) return false;
    const char *start = p++;
    memset(tag, 0, sizeof(*tag));
    tag->start = start;
    if (p < end && *p == '/') { tag->closing = true; ++p; }
    if (p >= end || *p == '!' || *p == '?') { *cursor = p; return xml_next_tag(cursor, end, tag); }
    const char *name = p;
    while (p < end && (isalnum((unsigned char)*p) || *p == ':' || *p == '_' || *p == '-')) ++p;
    if (p == name) { *cursor = p; return xml_next_tag(cursor, end, tag); }
    const char *local = name;
    for (const char *q = name; q < p; ++q) if (*q == ':') local = q + 1;
    size_t local_len = (size_t)(p - local);
    if (local_len >= sizeof(tag->local)) local_len = sizeof(tag->local) - 1;
    memcpy(tag->local, local, local_len);
    while (p < end && *p != '>') ++p;
    if (p >= end) return false;
    tag->after = p + 1;
    tag->text = tag->after;
    *cursor = tag->after;
    return true;
}

static bool xml_text(const char *start, const char *end, const char *wanted, char *out, size_t capacity) {
    const char *p = start;
    while (p < end) {
        xml_tag_t tag;
        if (!xml_next_tag(&p, end, &tag)) break;
        if (!tag.closing && !strcasecmp(tag.local, wanted)) {
            const char *text_start = tag.after;
            const char *scan = tag.after;
            while (scan < end) {
                xml_tag_t close;
                if (!xml_next_tag(&scan, end, &close)) break;
                if (close.closing && !strcasecmp(close.local, wanted)) {
                    size_t n = (size_t)(close.start - text_start);
                    if (n >= capacity) return false;
                    memcpy(out, text_start, n);
                    out[n] = 0;
                    return true;
                }
            }
        }
    }
    return false;
}

static bool href_path(const char *href, char *path, size_t capacity) {
    char server[APP_WEBDAV_SERVER_MAX];
    portENTER_CRITICAL(&s_lock);
    snprintf(server, sizeof(server), "%s", s_server_url);
    portEXIT_CRITICAL(&s_lock);
    size_t server_length = strlen(server);
    while (server_length > 8 && server[server_length - 1] == '/') server[--server_length] = 0;
    const char *server_path = strchr(server + 8, '/');
    size_t base_length = server_path ? strlen(server_path) : 0;
    const char *p = href;
    if (strlen(p) >= server_length && !strncmp(p, server, server_length) &&
        (!p[server_length] || p[server_length] == '/')) p += server_length;
    else if (!strncmp(p, "https://", 8)) {
        const char *configured_host_end = strchr(server + 8, '/');
        const char *href_host_end = strchr(p + 8, '/');
        size_t configured_host_length = configured_host_end ? (size_t)(configured_host_end - server) : strlen(server);
        size_t href_host_length = href_host_end ? (size_t)(href_host_end - p) : strlen(p);
        if (configured_host_length != href_host_length || strncasecmp(server, p, configured_host_length)) return false;
        p = href_host_end ? href_host_end : "/";
        if (base_length) {
            if (strlen(p) < base_length || strncmp(p, server_path, base_length) ||
                (p[base_length] && p[base_length] != '/')) return false;
            p += base_length;
        }
    } else if (*p == '/') {
        if (base_length) {
            if (strlen(p) < base_length || strncmp(p, server_path, base_length) ||
                (p[base_length] && p[base_length] != '/')) return false;
            p += base_length;
        }
    } else return false;
    if (!*p) p = "/";
    if (*p != '/' || strnlen(p, capacity) >= capacity || strchr(p, '?') || strchr(p, '#') ||
        strchr(p, '\\') || strstr(p, "//")) return false;
    const char *segment = p + 1;
    while (*segment) {
        const char *slash = strchr(segment, '/');
        size_t n = slash ? (size_t)(slash - segment) : strlen(segment);
        if (n == 6 && !strncasecmp(segment, "%2e%2e", 6)) return false;
        if (n == 2 && !strncmp(segment, "..", 2)) return false;
        if (!slash) break;
        segment = slash + 1;
    }
    snprintf(path, capacity, "%s", p);
    return true;
}

static void display_name(const char *path, char *name, size_t capacity) {
    size_t length = strlen(path);
    if (length > 1 && path[length - 1] == '/') --length;
    size_t start = length;
    while (start && path[start - 1] != '/') --start;
    size_t used = 0;
    for (size_t i = start; i < length && used + 1 < capacity; ++i) {
        if (path[i] == '%' && i + 2 < length && isxdigit((unsigned char)path[i + 1]) &&
            isxdigit((unsigned char)path[i + 2])) {
            char hex[3] = {path[i + 1], path[i + 2], 0};
            name[used++] = (char)strtoul(hex, NULL, 16);
            i += 2;
        } else name[used++] = path[i];
    }
    name[used] = 0;
    if (!used) snprintf(name, capacity, "/");
}

static bool parse_listing(const char *xml, size_t length, const char *requested) {
    memset(s_parse_items, 0, sizeof(s_parse_items));
    app_webdav_item_t *found = s_parse_items;
    unsigned count = 0;
    const char *end = xml + length;
    const char *cursor = xml;
    while (cursor < end) {
        xml_tag_t tag;
        if (!xml_next_tag(&cursor, end, &tag)) break;
        if (tag.closing || strcasecmp(tag.local, "response")) continue;
        const char *segment_start = tag.after;
        const char *scan = segment_start;
        const char *segment_end = NULL;
        while (scan < end) {
            xml_tag_t close;
            if (!xml_next_tag(&scan, end, &close)) break;
            if (close.closing && !strcasecmp(close.local, "response")) {
                segment_end = close.start;
                break;
            }
        }
        if (!segment_end) return false;
        char href[512], path[256];
        if (xml_text(segment_start, segment_end, "href", href, sizeof(href)) && href_path(href, path, sizeof(path)) &&
            strcmp(path, requested) && count < APP_WEBDAV_ITEMS) {
            bool is_dir = false;
            uint32_t size = 0;
            const char *p = segment_start;
            while (p < segment_end) {
                xml_tag_t nested;
                if (!xml_next_tag(&p, segment_end, &nested)) break;
                if (!nested.closing && !strcasecmp(nested.local, "collection")) is_dir = true;
            }
            char size_text[24];
            if (xml_text(segment_start, segment_end, "getcontentlength", size_text, sizeof(size_text))) {
                char *tail = NULL;
                unsigned long value = strtoul(size_text, &tail, 10);
                if (!*size_text || !tail || *tail || value > UINT32_MAX) return false;
                size = (uint32_t)value;
            }
            app_webdav_item_t *item = &found[count];
            snprintf(item->path, sizeof(item->path), "%s", path);
            display_name(path, item->name, sizeof(item->name));
            item->size = size;
            item->is_dir = is_dir || path[strlen(path) - 1] == '/';
            ++count;
        }
        cursor = segment_end;
    }
    portENTER_CRITICAL(&s_lock);
    memset(s_items, 0, sizeof(s_items));
    memcpy(s_items, found, count * sizeof(found[0]));
    s_status.count = count;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static bool make_url(const char *remote, char *url, size_t capacity) {
    if (!remote_path_valid(remote)) return false;
    char server[APP_WEBDAV_SERVER_MAX];
    portENTER_CRITICAL(&s_lock);
    snprintf(server, sizeof(server), "%s", s_server_url);
    portEXIT_CRITICAL(&s_lock);
    size_t length = strlen(server);
    while (length > 8 && server[length - 1] == '/') server[--length] = 0;
    return server_url_valid(server) && snprintf(url, capacity, "%s%s", server, remote) < (int)capacity;
}

static bool read_small_response(esp_http_client_handle_t client, char *body, size_t capacity) {
    size_t used = 0;
    while (!cancelled() && used + 1 < capacity) {
        esp_http_client_set_timeout_ms(client, io_timeout());
        int n = esp_http_client_read(client, body + used, capacity - used - 1);
        if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (n < 0) return false;
        if (!n) {
            body[used] = 0;
            return esp_http_client_is_complete_data_received(client);
        }
        used += (size_t)n;
    }
    return false;
}

static bool request_listing(const char *auth, const char *remote, char *body, size_t capacity) {
    static const char propfind[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/></d:prop></d:propfind>";
    char url[512];
    if (!make_url(remote, url, sizeof(url))) return false;
    esp_err_t err;
    esp_http_client_handle_t client = make_client(url, HTTP_METHOD_PROPFIND, auth,
        "application/xml; charset=utf-8", "1", &err);
    if (!client) return false;
    bool ok = open_client(client, sizeof(propfind) - 1) == ESP_OK &&
              write_all(client, propfind, sizeof(propfind) - 1) && response_headers_ready(client) &&
              esp_http_client_get_status_code(client) == 207 &&
              read_small_response(client, body, capacity);
    if (ok) ok = parse_listing(body, strlen(body), remote);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static bool remote_request(const char *auth, job_t job, const char *remote, const char *destination) {
    char url[512], target[512];
    if (!make_url(remote, url, sizeof(url)) ||
        (job == JOB_MOVE && !make_url(destination, target, sizeof(target)))) return false;
    const esp_http_client_method_t method = job == JOB_MOVE ? HTTP_METHOD_MOVE : HTTP_METHOD_DELETE;
    esp_err_t err;
    esp_http_client_handle_t client = make_client(url, method, auth, NULL, NULL, &err);
    if (!client) return false;
    if (job == JOB_MOVE) {
        esp_http_client_set_header(client, "Destination", target);
        esp_http_client_set_header(client, "Overwrite", "F");
    }
    bool ok = open_client(client, 0) == ESP_OK && response_headers_ready(client);
    const int status = esp_http_client_get_status_code(client);
    if (job == JOB_MOVE) ok = ok && (status == 200 || status == 201 || status == 204);
    else ok = ok && (status == 200 || status == 202 || status == 204);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok && status == 412) notice(APP_WEBDAV_FAILED, "遠端已有同名檔案，未覆蓋");
    else if (!ok && status == 404) notice(APP_WEBDAV_FAILED, "找不到遠端檔案或資料夾");
    return ok;
}

static bool download_file(const char *auth, const char *remote, const char *local) {
    char url[512], temp[320];
    if (!make_url(remote, url, sizeof(url)) || snprintf(temp, sizeof(temp), "%s.part", local) >= (int)sizeof(temp))
        return false;
    struct stat st;
    if (!stat(local, &st)) { notice(APP_WEBDAV_FAILED, "本機已有同名檔案，未覆蓋"); return false; }
    FILE *file = fopen(temp, "wb");
    if (!file) { notice(APP_WEBDAV_FAILED, "無法建立暫存檔，請檢查 TF 卡空間"); return false; }
    esp_err_t err;
    esp_http_client_handle_t client = make_client(url, HTTP_METHOD_GET, auth, NULL, NULL, &err);
    if (!client) { fclose(file); unlink(temp); return false; }
    bool ok = open_client(client, 0) == ESP_OK && response_headers_ready(client) &&
              esp_http_client_get_status_code(client) == 200;
    unsigned char chunk[2048];
    while (ok && !cancelled()) {
        esp_http_client_set_timeout_ms(client, io_timeout());
        int n = esp_http_client_read(client, (char *)chunk, sizeof(chunk));
        if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (n < 0) { ok = false; break; }
        if (!n) { ok = esp_http_client_is_complete_data_received(client); break; }
        if (fwrite(chunk, 1, (size_t)n, file) != (size_t)n) { ok = false; break; }
    }
    mbedtls_platform_zeroize(chunk, sizeof(chunk));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (fflush(file) || fsync(fileno(file))) ok = false;
    if (fclose(file)) ok = false;
    if (ok && !cancelled() && stat(local, &st) != 0 && rename(temp, local) == 0) return true;
    unlink(temp);
    return false;
}

static bool upload_file(const char *auth, const char *local, const char *remote) {
    char url[512];
    if (!make_url(remote, url, sizeof(url))) return false;
    struct stat st;
    if (stat(local, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) {
        notice(APP_WEBDAV_FAILED, "找不到本機檔案"); return false;
    }
    FILE *file = fopen(local, "rb");
    if (!file) { notice(APP_WEBDAV_FAILED, "無法讀取本機檔案"); return false; }
    esp_err_t err;
    esp_http_client_handle_t client = make_client(url, HTTP_METHOD_PUT, auth,
        "application/octet-stream", NULL, &err);
    if (!client) { fclose(file); return false; }
    bool ok = open_client(client, st.st_size) == ESP_OK;
    unsigned char chunk[2048];
    int64_t remaining = st.st_size;
    while (ok && remaining > 0 && !cancelled()) {
        size_t want = remaining > (int64_t)sizeof(chunk) ? sizeof(chunk) : (size_t)remaining;
        size_t n = fread(chunk, 1, want, file);
        if (n != want || !write_all(client, chunk, n)) { ok = false; break; }
        remaining -= (int64_t)n;
    }
    mbedtls_platform_zeroize(chunk, sizeof(chunk));
    if (remaining != 0 || ferror(file)) ok = false;
    fclose(file);
    if (ok) ok = response_headers_ready(client);
    int status = esp_http_client_get_status_code(client);
    ok = ok && (status == 200 || status == 201 || status == 204);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (status == 412) notice(APP_WEBDAV_FAILED, "遠端已有同名檔案，未覆蓋");
    return ok;
}

static void app_webdav_worker(void *arg) {
    memory_peak_token_t peak = (memory_peak_token_t)(uintptr_t)arg;
    s_job_deadline = esp_timer_get_time() + WEBDAV_JOB_LIMIT_US;
    job_args_t job;
    char user[sizeof(s_username)] = {0}, password[sizeof(s_password)] = {0};
    char auth[280] = {0};
    portENTER_CRITICAL(&s_lock);
    job = s_job;
    portEXIT_CRITICAL(&s_lock);
    set_auth(auth, sizeof(auth), user, sizeof(user), password, sizeof(password));
    mbedtls_platform_zeroize(user, sizeof(user));
    mbedtls_platform_zeroize(password, sizeof(password));
    network_lease_t lease = {0};
    bool ok = false;
    char *body = NULL;
    esp_err_t acquired = cancelled() ? ESP_ERR_INVALID_STATE
        : network_service_acquire(NETWORK_USE_WEATHER_MANUAL, &lease);
    if (acquired == ESP_OK && network_service_wait_ready_cancelable(&lease, 30000, cancelled)) {
        s_bearer = lease.bearer;
        if (job.job == JOB_LIST) {
            body = heap_caps_malloc(WEBDAV_XML_LIMIT + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (body) ok = request_listing(auth, job.remote, body, WEBDAV_XML_LIMIT + 1);
        } else if (job.job == JOB_DOWNLOAD) ok = download_file(auth, job.remote, job.local);
        else if (job.job == JOB_UPLOAD) ok = upload_file(auth, job.local, job.remote);
        else if (job.job == JOB_MOVE || job.job == JOB_DELETE)
            ok = remote_request(auth, job.job, job.remote, job.destination);
    }
    network_service_release(&lease);
    if (body) { memset(body, 0, WEBDAV_XML_LIMIT + 1); heap_caps_free(body); }
    mbedtls_platform_zeroize(auth, sizeof(auth));
    if (cancelled() && !ok) notice(APP_WEBDAV_FAILED, "WebDAV 工作已取消");
    if (!ok) {
        app_webdav_status_t status;
        app_webdav_status(&status);
        if (status.state == APP_WEBDAV_BUSY)
            notice(APP_WEBDAV_FAILED, "WebDAV 連線失敗，請檢查帳密、網路或伺服器路徑");
    } else {
        if (job.job == JOB_LIST) notice(APP_WEBDAV_READY, "");
        else if (job.job == JOB_DOWNLOAD) notice(APP_WEBDAV_READY, "下載完成，已存入 TF 卡 WebDAV 資料夾");
        else if (job.job == JOB_UPLOAD) notice(APP_WEBDAV_READY, "上傳完成");
        else if (job.job == JOB_MOVE) notice(APP_WEBDAV_READY, "遠端重新命名完成");
        else notice(APP_WEBDAV_READY, "遠端檔案已刪除");
    }
    portENTER_CRITICAL(&s_lock);
    s_task = NULL;
    portEXIT_CRITICAL(&s_lock);
    memory_peak_finish(peak);
    vTaskDelete(NULL);
}
