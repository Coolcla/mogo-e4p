/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "reading_sync_service.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "network_http.h"
#include "network_service.h"
#include "network_policy.h"
#include "book_metadata.h"
#include "device_power.h"

#define SYNC_HTTP_BODY_MAX 8192
#define SYNC_PENDING_MAX 6
#define SYNC_ACCESS_MAX APP_READING_SYNC_ACCESS_MAX
#define SYNC_REFRESH_MAX APP_READING_SYNC_REFRESH_MAX
#define SYNC_KOSYNC_DEFAULT "https://sync.koreader.rocks"
#define SYNC_READST_API "https://web.readest.com/api"
#define SYNC_SUPABASE_URL "https://vbsyxfusjjqdxkjqlysc.supabase.co"
#define SYNC_DEVICE_NAME "Fan"

static const char *TAG = "reading_sync";
static const char k_readest_anon_b64[] =
    "ZXlKaGJHY2lPaUpJVXpJMU5pSXNJblI1Y0NJNklrcFhWQ0o5LmV5SnBjM01pT2lKemRYQmhZbUZ6WlNJc0luSmxaaUk2SW5aaWMzbDRablZ6YW1weFpIaHJhbkZzZVhOaklpd2ljbTlzWlNJNkltRnViMjRpTENKcFlYUWlPakUzTXpReE1qTTJOekVzSW1WNGNDSTZNakEwT1RZNU9UWTNNWDAuM1U1VXFhb3VfMVNnclZlMWVvOXJBcGMwdUtqcWhwUWRVWGh2d1VIbVVmZw==";

typedef struct {
    char access[SYNC_ACCESS_MAX];
    char refresh[SYNC_REFRESH_MAX];
    char user[96];
    uint64_t expires;
    char ko_user[64];
    char ko_key[33];
    char ko_url[256];
    char ko_device[33];
} sync_config_t;

typedef enum { JOB_NONE, JOB_LOGIN, JOB_REGISTER, JOB_SYNC } sync_job_t;
typedef struct {
    sync_job_t job;
    app_reading_sync_provider_t provider;
    app_reading_sync_action_t action;
    app_reading_sync_snapshot_t snapshot;
    bool has_snapshot;
} queued_job_t;

typedef struct {
    char *body;
    size_t capacity, length;
    bool overflow;
} response_buffer_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static sync_config_t s_config;
static app_reading_sync_status_t s_status;
static queued_job_t s_pending_jobs[SYNC_PENDING_MAX];
static size_t s_pending_count;
static queued_job_t s_active_job;
static TaskHandle_t s_task;
static bool s_initialized;
static bool s_backup_restore_busy;
static int64_t s_retry_after_us;

static bool copy_cstr(char *dst, size_t capacity, const char *src) {
    if (!dst || !capacity || !src) return false;
    size_t n = strnlen(src, capacity);
    if (n >= capacity) return false;
    memcpy(dst, src, n + 1);
    return true;
}

static bool pending_insert_locked(const queued_job_t *job, bool front) {
    if (!job) return false;
    if (job->job == JOB_SYNC && job->has_snapshot) {
        for (size_t i = 0; i < s_pending_count; ++i) {
            queued_job_t *pending = &s_pending_jobs[i];
            if (pending->job != JOB_SYNC || !pending->has_snapshot ||
                strcmp(pending->snapshot.path, job->snapshot.path)) continue;
            bool replace = pending->action == job->action ||
                (job->action == APP_READING_SYNC_PUSH && pending->action == APP_READING_SYNC_AUTO);
            if (replace) { *pending = *job; return true; }
        }
    }
    if (s_pending_count == SYNC_PENDING_MAX) {
        size_t discard = SIZE_MAX;
        for (size_t i = 0; i < s_pending_count; ++i) {
            if (s_pending_jobs[i].job == JOB_SYNC &&
                s_pending_jobs[i].action == APP_READING_SYNC_AUTO) { discard = i; break; }
        }
        if (discard == SIZE_MAX) return false;
        memmove(&s_pending_jobs[discard], &s_pending_jobs[discard + 1],
                (s_pending_count - discard - 1) * sizeof(s_pending_jobs[0]));
        --s_pending_count;
    }
    if (front) {
        memmove(&s_pending_jobs[1], &s_pending_jobs[0], s_pending_count * sizeof(s_pending_jobs[0]));
        s_pending_jobs[0] = *job;
    } else {
        s_pending_jobs[s_pending_count] = *job;
    }
    ++s_pending_count;
    return true;
}

static queued_job_t pending_pop_locked(void) {
    queued_job_t job = {0};
    if (!s_pending_count) return job;
    job = s_pending_jobs[0];
    --s_pending_count;
    memmove(&s_pending_jobs[0], &s_pending_jobs[1], s_pending_count * sizeof(s_pending_jobs[0]));
    memset(&s_pending_jobs[s_pending_count], 0, sizeof(s_pending_jobs[0]));
    return job;
}

static bool nvs_read_string(nvs_handle_t handle, const char *key, char *out, size_t cap) {
    size_t n = cap;
    out[0] = 0;
    esp_err_t err = nvs_get_str(handle, key, out, &n);
    return err == ESP_OK && n > 0 && n <= cap;
}

static void config_load(void) {
    sync_config_t config = {0};
    snprintf(config.ko_url, sizeof(config.ko_url), "%s", SYNC_KOSYNC_DEFAULT);
    nvs_handle_t h;
    if (nvs_open("rd_sync", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_read_string(h, "rd_access", config.access, sizeof(config.access));
        (void)nvs_read_string(h, "rd_refresh", config.refresh, sizeof(config.refresh));
        (void)nvs_read_string(h, "rd_user", config.user, sizeof(config.user));
        (void)nvs_get_u64(h, "rd_expiry", &config.expires);
        (void)nvs_read_string(h, "ko_user", config.ko_user, sizeof(config.ko_user));
        (void)nvs_read_string(h, "ko_key", config.ko_key, sizeof(config.ko_key));
        (void)nvs_read_string(h, "ko_url", config.ko_url, sizeof(config.ko_url));
        (void)nvs_read_string(h, "ko_device", config.ko_device, sizeof(config.ko_device));
        nvs_close(h);
    }
    if (!config.ko_url[0]) snprintf(config.ko_url, sizeof(config.ko_url), "%s", SYNC_KOSYNC_DEFAULT);
    if (!config.ko_device[0]) {
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        for (size_t i = 0; i < sizeof(random); ++i)
            snprintf(config.ko_device + i * 2, sizeof(config.ko_device) - i * 2, "%02x", random[i]);
        if (nvs_open("rd_sync", NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_set_str(h, "ko_device", config.ko_device);
            (void)nvs_commit(h);
            nvs_close(h);
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_config = config;
    s_status.readest_logged_in = config.access[0] != 0 && config.refresh[0] != 0;
    s_status.kosync_configured = config.ko_user[0] != 0 && strlen(config.ko_key) == 32;
    snprintf(s_status.kosync_username, sizeof(s_status.kosync_username), "%s", config.ko_user);
    snprintf(s_status.kosync_server, sizeof(s_status.kosync_server), "%s", config.ko_url);
    snprintf(s_status.readest_state, sizeof(s_status.readest_state), "%s", s_status.readest_logged_in ? "已登入" : "尚未登入");
    snprintf(s_status.kosync_state, sizeof(s_status.kosync_state), "%s", s_status.kosync_configured ? "已設定" : "尚未設定");
    s_initialized = true;
    portEXIT_CRITICAL(&s_lock);
}

void app_reading_sync_init(void) {
    if (!s_initialized) config_load();
}

static void status_text(char *field, size_t cap, const char *text) {
    portENTER_CRITICAL(&s_lock);
    snprintf(field, cap, "%s", text ? text : "");
    portEXIT_CRITICAL(&s_lock);
}

void app_reading_sync_get_status(app_reading_sync_status_t *out) {
    if (!out) return;
    app_reading_sync_init();
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    out->busy = s_task != NULL;
    out->pending = s_pending_count != 0;
    out->readest_logged_in = s_config.access[0] != 0 && s_config.refresh[0] != 0;
    out->kosync_configured = s_config.ko_user[0] != 0 && strlen(s_config.ko_key) == 32;
    snprintf(out->kosync_username, sizeof(out->kosync_username), "%s", s_config.ko_user);
    snprintf(out->kosync_server, sizeof(out->kosync_server), "%s", s_config.ko_url);
    portEXIT_CRITICAL(&s_lock);
}

static bool md5_hex(const void *data, size_t len, char out[33]) {
    unsigned char digest[16];
    mbedtls_md_context_t md5;
    mbedtls_md_init(&md5);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    bool ok = info && mbedtls_md_setup(&md5, info, 0) == 0 &&
        mbedtls_md_starts(&md5) == 0 && mbedtls_md_update(&md5, data, len) == 0 &&
        mbedtls_md_finish(&md5, digest) == 0;
    mbedtls_md_free(&md5);
    if (!ok) { mbedtls_platform_zeroize(digest, sizeof(digest)); return false; }
    for (size_t i = 0; i < sizeof(digest); ++i)
        snprintf(out + i * 2, 33 - i * 2, "%02x", digest[i]);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return true;
}

static void copy_utf8_prefix(char *dst, size_t capacity, const char *src) {
    if (!dst || capacity == 0) return;
    dst[0] = 0;
    if (!src) return;
    size_t len = strlen(src);
    if (len >= capacity) {
        len = capacity - 1;
        while (len > 0 && (((const unsigned char *)src)[len] & 0xC0u) == 0x80u) --len;
    }
    memcpy(dst, src, len);
    dst[len] = 0;
}

static void snapshot_xpointer(const app_reading_sync_snapshot_t *snapshot,
                              char *out, size_t capacity) {
    if (!out || !capacity) return;
    unsigned fragment = snapshot ? (unsigned)snapshot->chapter + 1u : 1u;
    (void)snprintf(out, capacity, "/body/DocFragment[%u]", fragment);
}

static bool save_readest_tokens(const char *access, const char *refresh, const char *user,
                                uint64_t expires) {
    if (!access || !refresh || !access[0] || !refresh[0] ||
        strnlen(access, SYNC_ACCESS_MAX) >= SYNC_ACCESS_MAX ||
        strnlen(refresh, SYNC_REFRESH_MAX) >= SYNC_REFRESH_MAX) return false;
    nvs_handle_t h;
    if (nvs_open("rd_sync", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, "rd_access", access);
    if (err == ESP_OK) err = nvs_set_str(h, "rd_refresh", refresh);
    if (err == ESP_OK && user && strnlen(user, 96) < 96) err = nvs_set_str(h, "rd_user", user);
    if (err == ESP_OK) err = nvs_set_u64(h, "rd_expiry", expires);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_config.access, sizeof(s_config.access), "%s", access);
    snprintf(s_config.refresh, sizeof(s_config.refresh), "%s", refresh);
    if (user) snprintf(s_config.user, sizeof(s_config.user), "%s", user);
    s_config.expires = expires;
    s_status.readest_logged_in = true;
    snprintf(s_status.readest_state, sizeof(s_status.readest_state), "已登入");
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static esp_err_t response_event(esp_http_client_event_t *event) {
    response_buffer_t *r = event ? event->user_data : NULL;
    if (!r || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    size_t add = (size_t)event->data_len;
    if (add >= r->capacity || r->length > r->capacity - add - 1) {
        r->overflow = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(r->body + r->length, event->data, add);
    r->length += add;
    r->body[r->length] = 0;
    return ESP_OK;
}

static cJSON *http_json(network_bearer_t bearer, const char *url, esp_http_client_method_t method,
                        const char *body, const char *access, const char *user,
                        const char *key, const char *api_key, int *http_status) {
    if (http_status) *http_status = 0;
    char *buffer = calloc(1, SYNC_HTTP_BODY_MAX + 1);
    if (!buffer) return NULL;
    response_buffer_t response = {.body = buffer, .capacity = SYNC_HTTP_BODY_MAX + 1};
    esp_http_client_config_t cfg = {
        .url = url, .method = method, .timeout_ms = 15000,
        .buffer_size = 2048, .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = response_event, .user_data = &response,
    };
    esp_err_t init_error = ESP_OK;
    network_http_session_t session = {0};
    esp_http_client_handle_t client = network_http_client_init(&cfg, bearer, &session, &init_error);
    esp_err_t err = client ? ESP_OK : init_error;
    if (client) {
        if (esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK ||
            esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK)
            err = ESP_FAIL;
        if (err == ESP_OK && (user || key)) {
            if (user && esp_http_client_set_header(client, "x-auth-user", user) != ESP_OK) err = ESP_FAIL;
            if (key && esp_http_client_set_header(client, "x-auth-key", key) != ESP_OK) err = ESP_FAIL;
            if (err == ESP_OK && esp_http_client_set_header(client, "Accept", "application/vnd.koreader.v1+json") != ESP_OK)
                err = ESP_FAIL;
        }
        if (err == ESP_OK && api_key && esp_http_client_set_header(client, "apikey", api_key) != ESP_OK) err = ESP_FAIL;
        if (err == ESP_OK && access) {
            char auth[SYNC_ACCESS_MAX + 16];
            int n = snprintf(auth, sizeof(auth), "Bearer %s", access);
            if (n <= 0 || (size_t)n >= sizeof(auth) ||
                esp_http_client_set_header(client, "Authorization", auth) != ESP_OK) err = ESP_FAIL;
            mbedtls_platform_zeroize(auth, sizeof(auth));
        }
        if (err == ESP_OK && body && esp_http_client_set_post_field(client, body, (int)strlen(body)) != ESP_OK)
            err = ESP_FAIL;
        if (err == ESP_OK) err = esp_http_client_perform(client);
        if (http_status) *http_status = esp_http_client_get_status_code(client);
        network_http_note_result(&session, err == ESP_OK || esp_http_client_get_status_code(client) != 0);
        esp_http_client_cleanup(client);
    }
    cJSON *json = NULL;
    if (err == ESP_OK && !response.overflow && response.length) json = cJSON_ParseWithLength(buffer, response.length);
    if (response.overflow) ESP_LOGW(TAG, "sync response exceeded %u bytes", (unsigned)SYNC_HTTP_BODY_MAX);
    mbedtls_platform_zeroize(buffer, SYNC_HTTP_BODY_MAX + 1);
    free(buffer);
    return json;
}

static cJSON *json_post_body(const char *key, const char *value) {
    cJSON *body = cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body, key, value)) { cJSON_Delete(body); return NULL; }
    return body;
}

static char *json_print(cJSON *item) {
    return item ? cJSON_PrintUnformatted(item) : NULL;
}

static bool snapshot_identity(const app_reading_sync_snapshot_t *snapshot,
                              char doc_hash[33], char meta_hash[33], char author[192]) {
    if (!snapshot || !snapshot->path[0]) return false;
    FILE *file = fopen(snapshot->path, "rb");
    if (!file) return false;
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || st.st_size <= 0) { fclose(file); return false; }
    mbedtls_md_context_t md5;
    mbedtls_md_init(&md5);
    const mbedtls_md_info_t *md5_info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    bool ok = md5_info && mbedtls_md_setup(&md5, md5_info, 0) == 0 && mbedtls_md_starts(&md5) == 0;
    uint8_t block[1024];
    for (int i = -1; ok && i <= 10; ++i) {
        uint64_t offset = i < 0 ? 256u : ((uint64_t)1024 << (2 * i));
        if (offset >= (uint64_t)st.st_size || fseek(file, (long)offset, SEEK_SET) != 0) break;
        size_t want = (uint64_t)st.st_size - offset < sizeof(block)
            ? (size_t)((uint64_t)st.st_size - offset) : sizeof(block);
        size_t n = fread(block, 1, want, file);
        if (n && mbedtls_md_update(&md5, block, n) != 0) ok = false;
        if (n != want) break;
    }
    unsigned char digest[16];
    if (ok) ok = mbedtls_md_finish(&md5, digest) == 0;
    mbedtls_md_free(&md5);
    fclose(file);
    mbedtls_platform_zeroize(block, sizeof(block));
    if (!ok) { mbedtls_platform_zeroize(digest, sizeof(digest)); return false; }
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(doc_hash + i * 2, 33 - i * 2, "%02x", digest[i]);
    mbedtls_platform_zeroize(digest, sizeof(digest));

    char title[192] = {0}, cached_title[192] = {0}, cached_author[192] = {0};
    if (snapshot->title[0]) snprintf(title, sizeof(title), "%s", snapshot->title);
    author[0] = 0;
    if (book_metadata_cached(snapshot->path, cached_title, sizeof(cached_title),
                             cached_author, sizeof(cached_author)) == ESP_OK) {
        if (cached_title[0]) snprintf(title, sizeof(title), "%s", cached_title);
        if (cached_author[0]) snprintf(author, 192, "%s", cached_author);
    }
    if (!title[0]) {
        const char *base = strrchr(snapshot->path, '/');
        const char *backslash = strrchr(snapshot->path, '\\');
        if (backslash && (!base || backslash > base)) base = backslash;
        copy_utf8_prefix(title, sizeof(title), base ? base + 1 : snapshot->path);
    }

    // Readest hashes title|authors|identifiers after stripping the scheme
    // prefix from identifiers. EPUBs have a stable package identifier that
    // distinguishes editions that share a title and author.
    char identifier_raw[384] = {0}, identifier[256] = {0};
    if (book_metadata_identifier(snapshot->path, identifier_raw, sizeof(identifier_raw)) == ESP_OK) {
        char *start = identifier_raw;
        while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') ++start;
        char *end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) --end;
        *end = 0;
        const char *colon = strchr(start, ':');
        const char *normalized = colon ? colon + 1 : start;
        if (!strncmp(start, "urn:", 4)) {
            const char *last = strrchr(start, ':');
            if (last) normalized = last + 1;
        }
        snprintf(identifier, sizeof(identifier), "%s", normalized);
    }

    // The Readest plugin trims surrounding whitespace from each author.
    char *author_start = author;
    while (*author_start == ' ' || *author_start == '\t' || *author_start == '\r' || *author_start == '\n') ++author_start;
    if (author_start != author) memmove(author, author_start, strlen(author_start) + 1);
    size_t author_len = strlen(author);
    while (author_len && (author[author_len - 1] == ' ' || author[author_len - 1] == '\t' ||
                          author[author_len - 1] == '\r' || author[author_len - 1] == '\n'))
        author[--author_len] = 0;

    char identity[768];
    int n = snprintf(identity, sizeof(identity), "%s|%s|%s", title, author, identifier);
    if (n <= 0 || (size_t)n >= sizeof(identity)) return false;
    ok = md5_hex(identity, (size_t)n, meta_hash);
    mbedtls_platform_zeroize(identity, sizeof(identity));
    mbedtls_platform_zeroize(identifier_raw, sizeof(identifier_raw));
    mbedtls_platform_zeroize(identifier, sizeof(identifier));
    mbedtls_platform_zeroize(cached_title, sizeof(cached_title));
    mbedtls_platform_zeroize(cached_author, sizeof(cached_author));
    return ok;
}

// KOReader's partial MD5 samples 1024 bytes at offsets 0, 1024, 4096, ... .
// Keep this independent from Readest's existing book hash, whose identity is already in use.
static bool kosync_document_hash(const char *path, char out[33]) {
    if (!path || !path[0] || !out) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || st.st_size <= 0) {
        fclose(file);
        return false;
    }

    mbedtls_md_context_t md5;
    mbedtls_md_init(&md5);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    bool ok = info && mbedtls_md_setup(&md5, info, 0) == 0 && mbedtls_md_starts(&md5) == 0;
    uint8_t block[1024];
    for (int i = -1; ok && i <= 10; ++i) {
        // KOReader's LuaJIT bit.lshift(1024, -2) wraps the shift count and yields offset 0.
        const uint64_t offset = i < 0 ? 0u : ((uint64_t)1024 << (2 * i));
        if (offset >= (uint64_t)st.st_size) break;
        if (fseek(file, (long)offset, SEEK_SET) != 0) { ok = false; break; }
        const size_t want = (uint64_t)st.st_size - offset < sizeof(block)
            ? (size_t)((uint64_t)st.st_size - offset) : sizeof(block);
        const size_t n = fread(block, 1, want, file);
        if (n && mbedtls_md_update(&md5, block, n) != 0) ok = false;
        if (n != want) {
            if (ferror(file)) ok = false;
            break;
        }
    }
    unsigned char digest[16] = {0};
    if (ok) ok = mbedtls_md_finish(&md5, digest) == 0;
    mbedtls_md_free(&md5);
    fclose(file);
    mbedtls_platform_zeroize(block, sizeof(block));
    if (!ok) {
        mbedtls_platform_zeroize(digest, sizeof(digest));
        return false;
    }
    for (size_t i = 0; i < sizeof(digest); ++i)
        snprintf(out + i * 2, 33 - i * 2, "%02x", digest[i]);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return true;
}

static bool readest_anon_key(char out[512]) {
    size_t decoded = 0;
    if (mbedtls_base64_decode((unsigned char *)out, 512, &decoded,
            (const unsigned char *)k_readest_anon_b64, strlen(k_readest_anon_b64)) != 0 || decoded >= 512)
        return false;
    out[decoded] = 0;
    return true;
}

static bool readest_refresh_token(network_bearer_t bearer, sync_config_t *cfg) {
    time_t now = time(NULL);
    if (cfg->access[0] && cfg->refresh[0] && cfg->expires > (uint64_t)now + 90) return true;
    if (!cfg->refresh[0]) return false;
    cJSON *payload = json_post_body("refresh_token", cfg->refresh);
    char *body = json_print(payload);
    cJSON_Delete(payload);
    char url[160], api_key[512] = {0};
    snprintf(url, sizeof(url), "%s/auth/v1/token?grant_type=refresh_token", SYNC_SUPABASE_URL);
    if (!body || !readest_anon_key(api_key)) { free(body); mbedtls_platform_zeroize(api_key, sizeof(api_key)); return false; }
    int status = 0;
    cJSON *json = http_json(bearer, url, HTTP_METHOD_POST, body, NULL, NULL, NULL, api_key, &status);
    free(body);
    mbedtls_platform_zeroize(api_key, sizeof(api_key));
    const cJSON *access = json ? cJSON_GetObjectItemCaseSensitive(json, "access_token") : NULL;
    const cJSON *refresh = json ? cJSON_GetObjectItemCaseSensitive(json, "refresh_token") : NULL;
    const cJSON *expires = json ? cJSON_GetObjectItemCaseSensitive(json, "expires_in") : NULL;
    const cJSON *user = json ? cJSON_GetObjectItemCaseSensitive(json, "user") : NULL;
    const cJSON *id = user ? cJSON_GetObjectItemCaseSensitive(user, "id") : NULL;
    const char *username = cJSON_IsString(id) ? id->valuestring : cfg->user;
    bool ok = status == 200 && cJSON_IsString(access) && cJSON_IsString(refresh) && cJSON_IsNumber(expires);
    if (ok) {
        uint64_t expiry = (uint64_t)time(NULL) + (uint64_t)expires->valuedouble;
        ok = save_readest_tokens(access->valuestring, refresh->valuestring, username, expiry);
        if (ok) {
            snprintf(cfg->access, sizeof(cfg->access), "%s", access->valuestring);
            snprintf(cfg->refresh, sizeof(cfg->refresh), "%s", refresh->valuestring);
            cfg->expires = expiry;
            snprintf(cfg->user, sizeof(cfg->user), "%s", username);
        }
    }
    cJSON_Delete(json);
    return ok;
}

static bool readest_login(network_bearer_t bearer) {
    int status = 0;
    cJSON *request = cJSON_CreateObject();
    char *request_body = json_print(request);
    cJSON_Delete(request);
    cJSON *code = http_json(bearer, SYNC_READST_API "/crosspoint/device/code", HTTP_METHOD_POST,
                            request_body ? request_body : "{}", NULL, NULL, NULL, NULL, &status);
    free(request_body);
    const cJSON *device = code ? cJSON_GetObjectItemCaseSensitive(code, "device_code") : NULL;
    const cJSON *user = code ? cJSON_GetObjectItemCaseSensitive(code, "user_code") : NULL;
    const cJSON *uri = code ? cJSON_GetObjectItemCaseSensitive(code, "verification_uri") : NULL;
    const cJSON *interval = code ? cJSON_GetObjectItemCaseSensitive(code, "interval") : NULL;
    const cJSON *expires = code ? cJSON_GetObjectItemCaseSensitive(code, "expires_in") : NULL;
    if (status != 200 || !cJSON_IsString(device) || !cJSON_IsString(user) || !cJSON_IsString(uri)) {
        cJSON_Delete(code);
        status_text(s_status.readest_state, sizeof(s_status.readest_state), "無法取得登入代碼");
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.readest_user_code, sizeof(s_status.readest_user_code), "%s", user->valuestring);
    snprintf(s_status.readest_verification_uri, sizeof(s_status.readest_verification_uri), "%s", uri->valuestring);
    snprintf(s_status.readest_state, sizeof(s_status.readest_state), "請至 Readest 網頁授權");
    portEXIT_CRITICAL(&s_lock);
    char device_code[256];
    if (!copy_cstr(device_code, sizeof(device_code), device->valuestring)) { cJSON_Delete(code); return false; }
    unsigned poll_s = cJSON_IsNumber(interval) && interval->valueint > 0 ? (unsigned)interval->valueint : 5;
    unsigned lifetime_s = cJSON_IsNumber(expires) && expires->valueint > 0 ? (unsigned)expires->valueint : 600;
    cJSON_Delete(code);
    uint64_t deadline = (uint64_t)time(NULL) + lifetime_s;
    while ((uint64_t)time(NULL) < deadline) {
        vTaskDelay(pdMS_TO_TICKS(poll_s * 1000));
        cJSON *body_json = cJSON_CreateObject();
        if (!body_json || !cJSON_AddStringToObject(body_json, "device_code", device_code) ||
            !cJSON_AddStringToObject(body_json, "grant", "session")) {
            cJSON_Delete(body_json); break;
        }
        char *body = json_print(body_json);
        cJSON_Delete(body_json);
        int poll_status = 0;
        cJSON *result = body ? http_json(bearer, SYNC_READST_API "/crosspoint/device/token",
            HTTP_METHOD_POST, body, NULL, NULL, NULL, NULL, &poll_status) : NULL;
        free(body);
        const cJSON *access = result ? cJSON_GetObjectItemCaseSensitive(result, "access_token") : NULL;
        const cJSON *refresh = result ? cJSON_GetObjectItemCaseSensitive(result, "refresh_token") : NULL;
        const cJSON *account = result ? cJSON_GetObjectItemCaseSensitive(result, "user") : NULL;
        const cJSON *id = account ? cJSON_GetObjectItemCaseSensitive(account, "id") : NULL;
        const cJSON *email = account ? cJSON_GetObjectItemCaseSensitive(account, "email") : NULL;
        if (poll_status == 200 && cJSON_IsString(access) && cJSON_IsString(refresh) && cJSON_IsObject(account)) {
            const char *who = cJSON_IsString(email) ? email->valuestring : cJSON_IsString(id) ? id->valuestring : "Readest";
            bool saved = save_readest_tokens(access->valuestring, refresh->valuestring, who,
                (uint64_t)time(NULL) + 3600);
            cJSON_Delete(result);
            mbedtls_platform_zeroize(device_code, sizeof(device_code));
            status_text(s_status.readest_state, sizeof(s_status.readest_state), saved ? "登入成功" : "登入資訊儲存失敗");
            return saved;
        }
        const cJSON *error = result ? cJSON_GetObjectItemCaseSensitive(result, "error") : NULL;
        bool waiting = cJSON_IsString(error) &&
            (!strcmp(error->valuestring, "authorization_pending") || !strcmp(error->valuestring, "slow_down"));
        if (cJSON_IsString(error) && !strcmp(error->valuestring, "slow_down")) poll_s += 5;
        cJSON_Delete(result);
        if (!waiting && poll_status != 0 && poll_status != 400) break;
    }
    mbedtls_platform_zeroize(device_code, sizeof(device_code));
    status_text(s_status.readest_state, sizeof(s_status.readest_state), "登入逾時，請重試");
    return false;
}

static bool readest_sync(network_bearer_t bearer, sync_config_t *cfg,
                         const app_reading_sync_snapshot_t *snapshot,
                         app_reading_sync_action_t action, float *remote_percent) {
    if (!cfg->access[0] || !cfg->refresh[0] || !readest_refresh_token(bearer, cfg)) {
        status_text(s_status.readest_state, sizeof(s_status.readest_state), "請重新登入 Readest");
        return false;
    }
    char doc_hash[33], meta_hash[33], author[192];
    if (!snapshot_identity(snapshot, doc_hash, meta_hash, author)) {
        status_text(s_status.readest_state, sizeof(s_status.readest_state), "無法讀取書籍識別資料");
        return false;
    }
    bool ok = false;
    if (action == APP_READING_SYNC_PULL || action == APP_READING_SYNC_AUTO) {
        char url[512];
        snprintf(url, sizeof(url), SYNC_READST_API "/sync?since=0&type=configs&book=%s&meta_hash=%s", doc_hash, meta_hash);
        int status = 0;
        cJSON *json = http_json(bearer, url, HTTP_METHOD_GET, NULL, cfg->access, NULL, NULL, NULL, &status);
        const cJSON *configs = json ? cJSON_GetObjectItemCaseSensitive(json, "configs") : NULL;
        if (status == 200 && cJSON_IsArray(configs) && cJSON_GetArraySize(configs)) {
            const cJSON *entry = cJSON_GetArrayItem(configs, cJSON_GetArraySize(configs) - 1);
            const cJSON *progress = cJSON_GetObjectItemCaseSensitive(entry, "progress");
            const cJSON *pointer = cJSON_GetObjectItemCaseSensitive(entry, "xpointer");
            float fraction = -1.0f;
            if (cJSON_IsArray(progress) && cJSON_GetArraySize(progress) >= 2) {
                const cJSON *current = cJSON_GetArrayItem(progress, 0);
                const cJSON *total = cJSON_GetArrayItem(progress, 1);
                if (cJSON_IsNumber(current) && cJSON_IsNumber(total) && total->valuedouble > 1)
                    fraction = (float)(current->valuedouble - 1.0) / (float)(total->valuedouble - 1.0);
            }
            if (fraction < 0 && cJSON_IsString(pointer) && !strncmp(pointer->valuestring, "fan:v1:", 7)) {
                unsigned pct = 0;
                if (sscanf(pointer->valuestring, "fan:v1:%*u:%*u:%u", &pct) == 1 && pct <= 100)
                    fraction = (float)pct / 100.0f;
            }
            if (fraction >= 0.0f && fraction <= 1.0f && remote_percent && fraction > *remote_percent)
                *remote_percent = fraction;
            ok = fraction >= 0.0f;
        } else if (status == 200) ok = true;
        cJSON_Delete(json);
        if (status == 401 || status == 403) {
            status_text(s_status.readest_state, sizeof(s_status.readest_state), "Readest 權杖失效");
            return false;
        }
    }
    if (action == APP_READING_SYNC_PUSH || action == APP_READING_SYNC_AUTO) {
        if (action == APP_READING_SYNC_AUTO && remote_percent && *remote_percent > (float)snapshot->percent / 100.0f + 0.005f)
            return true;
        cJSON *root = cJSON_CreateObject();
        cJSON *books = cJSON_CreateArray(), *notes = cJSON_CreateArray(), *configs = cJSON_CreateArray();
        cJSON *config = cJSON_CreateObject(), *progress = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "books", books);
        cJSON_AddItemToObject(root, "notes", notes);
        cJSON_AddItemToObject(root, "configs", configs);
        cJSON_AddItemToArray(configs, config);
        cJSON_AddStringToObject(config, "bookHash", doc_hash);
        cJSON_AddStringToObject(config, "metaHash", meta_hash);
        cJSON_AddItemToObject(config, "progress", progress);
        cJSON_AddItemToArray(progress, cJSON_CreateNumber(snapshot->percent + 1));
        cJSON_AddItemToArray(progress, cJSON_CreateNumber(101));
        char xpointer[48];
        snapshot_xpointer(snapshot, xpointer, sizeof(xpointer));
        cJSON_AddStringToObject(config, "xpointer", xpointer);
        cJSON_AddNumberToObject(config, "updatedAt", (double)time(NULL) * 1000.0);
        char *body = json_print(root);
        cJSON_Delete(root);
        int status = 0;
        cJSON *response = body ? http_json(bearer, SYNC_READST_API "/sync", HTTP_METHOD_POST,
            body, cfg->access, NULL, NULL, NULL, &status) : NULL;
        free(body);
        cJSON_Delete(response);
        ok = status == 200 || status == 201;
    }
    status_text(s_status.readest_state, sizeof(s_status.readest_state), ok ? "同步完成" : "同步失敗");
    mbedtls_platform_zeroize(doc_hash, sizeof(doc_hash));
    mbedtls_platform_zeroize(meta_hash, sizeof(meta_hash));
    mbedtls_platform_zeroize(author, sizeof(author));
    return ok;
}

static bool kosync_request(network_bearer_t bearer, sync_config_t *cfg,
                           const app_reading_sync_snapshot_t *snapshot,
                           app_reading_sync_action_t action, float *remote_percent) {
    if (!cfg->ko_user[0] || strlen(cfg->ko_key) != 32) {
        status_text(s_status.kosync_state, sizeof(s_status.kosync_state), "請先設定 KOReader 帳號");
        return false;
    }
    char doc_hash[33], ko_doc_hash[33], meta_hash[33], author[192];
    if (!snapshot_identity(snapshot, doc_hash, meta_hash, author) ||
        !kosync_document_hash(snapshot->path, ko_doc_hash)) {
        status_text(s_status.kosync_state, sizeof(s_status.kosync_state), "無法讀取書籍識別資料");
        return false;
    }
    size_t url_len = strlen(cfg->ko_url);
    while (url_len && cfg->ko_url[url_len - 1] == '/') cfg->ko_url[--url_len] = 0;
    bool ok = false;
    if (action == APP_READING_SYNC_PULL || action == APP_READING_SYNC_AUTO) {
        char url[512];
        snprintf(url, sizeof(url), "%s/syncs/progress/%s", cfg->ko_url, ko_doc_hash);
        int status = 0;
        cJSON *json = http_json(bearer, url, HTTP_METHOD_GET, NULL, NULL, cfg->ko_user, cfg->ko_key, NULL, &status);
        const cJSON *percentage = json ? cJSON_GetObjectItemCaseSensitive(json, "percentage") : NULL;
        if (status == 200 && cJSON_IsNumber(percentage) && percentage->valuedouble >= 0 && percentage->valuedouble <= 1) {
            if (remote_percent && percentage->valuedouble > *remote_percent)
                *remote_percent = (float)percentage->valuedouble;
            ok = true;
        } else if (status == 404 || (status == 200 && !percentage)) ok = true;
        if (status == 401) status_text(s_status.kosync_state, sizeof(s_status.kosync_state), "KOReader 帳號驗證失敗");
        cJSON_Delete(json);
    }
    if (action == APP_READING_SYNC_PUSH || action == APP_READING_SYNC_AUTO) {
        if (action == APP_READING_SYNC_AUTO && remote_percent && *remote_percent > (float)snapshot->percent / 100.0f + 0.005f)
            goto done;
        char url[512], progress[96];
        snprintf(url, sizeof(url), "%s/syncs/progress", cfg->ko_url);
        // KOReader expects a document-engine XPointer here. Fan can provide an
        // exact chapter anchor, while percentage remains the cross-format fallback.
        snapshot_xpointer(snapshot, progress, sizeof(progress));
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "document", ko_doc_hash);
        cJSON_AddStringToObject(payload, "progress", progress);
        cJSON_AddNumberToObject(payload, "percentage", (double)snapshot->percent / 100.0);
        cJSON_AddStringToObject(payload, "device", SYNC_DEVICE_NAME);
        cJSON_AddStringToObject(payload, "device_id", cfg->ko_device);
        char *body = json_print(payload);
        cJSON_Delete(payload);
        int status = 0;
        cJSON *response = body ? http_json(bearer, url, HTTP_METHOD_PUT, body, NULL,
            cfg->ko_user, cfg->ko_key, NULL, &status) : NULL;
        free(body);
        cJSON_Delete(response);
        ok = status == 200 || status == 202;
    }
done:
    status_text(s_status.kosync_state, sizeof(s_status.kosync_state), ok ? "同步完成" : "同步失敗");
    mbedtls_platform_zeroize(doc_hash, sizeof(doc_hash));
    mbedtls_platform_zeroize(ko_doc_hash, sizeof(ko_doc_hash));
    mbedtls_platform_zeroize(meta_hash, sizeof(meta_hash));
    mbedtls_platform_zeroize(author, sizeof(author));
    return ok;
}

static void worker(void *arg) {
    (void)arg;
    queued_job_t job;
    sync_config_t cfg;
    portENTER_CRITICAL(&s_lock);
    job = s_active_job;
    cfg = s_config;
    portEXIT_CRITICAL(&s_lock);
    network_lease_t lease = {0};
    const esp_err_t acquired = network_service_acquire(NETWORK_USE_WEATHER, &lease);
    if (acquired != ESP_OK || !network_service_wait_ready_cancelable(&lease, 30000, NULL)) {
        network_service_release(&lease);
        mbedtls_platform_zeroize(&cfg, sizeof(cfg));
        portENTER_CRITICAL(&s_lock);
        (void)pending_insert_locked(&job, true);
        s_retry_after_us = esp_timer_get_time() + 30LL * 1000000;
        s_active_job = (queued_job_t){0};
        s_task = NULL;
        s_status.busy = false;
        s_status.pending = s_pending_count != 0;
        snprintf(s_status.message, sizeof(s_status.message), "網路連線失敗，稍後自動重試");
        if (job.job == JOB_LOGIN) snprintf(s_status.readest_state, sizeof(s_status.readest_state), "網路連線失敗，稍後重試");
        if (job.job == JOB_REGISTER) snprintf(s_status.kosync_state, sizeof(s_status.kosync_state), "網路連線失敗，稍後重試");
        portEXIT_CRITICAL(&s_lock);
        vTaskDelete(NULL);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_retry_after_us = 0;
    portEXIT_CRITICAL(&s_lock);
    bool ok = false;
    if (job.job == JOB_LOGIN) {
        ok = readest_login(lease.bearer);
    } else if (job.job == JOB_REGISTER) {
        char url[512];
        snprintf(url, sizeof(url), "%s/users/create", cfg.ko_url);
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "username", cfg.ko_user);
        cJSON_AddStringToObject(payload, "password", cfg.ko_key);
        char *body = json_print(payload);
        cJSON_Delete(payload);
        int status = 0;
        cJSON *response = body ? http_json(lease.bearer, url, HTTP_METHOD_POST, body,
            NULL, NULL, NULL, NULL, &status) : NULL;
        free(body);
        ok = status == 201 || status == 200;
        cJSON_Delete(response);
        status_text(s_status.kosync_state, sizeof(s_status.kosync_state), ok ? "註冊完成" : status == 402 ? "帳號已存在" : "註冊失敗");
    } else if (job.job == JOB_SYNC && job.has_snapshot) {
        bool did = false, success = false;
        float remote = -1.0f;
        if (job.action == APP_READING_SYNC_AUTO) {
            // Compare both remote positions before uploading so a newer location
            // from either service is never overwritten by the local copy.
            if (job.provider & APP_READING_SYNC_READEST) {
                did |= cfg.access[0] != 0;
                if (cfg.access[0]) success |= readest_sync(lease.bearer, &cfg, &job.snapshot, APP_READING_SYNC_PULL, &remote);
            }
            if (job.provider & APP_READING_SYNC_KOSYNC) {
                did |= cfg.ko_user[0] != 0;
                if (cfg.ko_user[0]) success |= kosync_request(lease.bearer, &cfg, &job.snapshot, APP_READING_SYNC_PULL, &remote);
            }
            if (remote > (float)job.snapshot.percent / 100.0f + 0.005f) {
                portENTER_CRITICAL(&s_lock);
                s_status.remote_ready = true;
                s_status.remote_force = false;
                s_status.remote_percent = remote;
                snprintf(s_status.remote_path, sizeof(s_status.remote_path), "%s", job.snapshot.path);
                snprintf(s_status.message, sizeof(s_status.message), "發現雲端位置 %.0f%%", remote * 100.0f);
                portEXIT_CRITICAL(&s_lock);
                success = true;
            } else {
                if ((job.provider & APP_READING_SYNC_READEST) && cfg.access[0])
                    success |= readest_sync(lease.bearer, &cfg, &job.snapshot, APP_READING_SYNC_PUSH, NULL);
                if ((job.provider & APP_READING_SYNC_KOSYNC) && cfg.ko_user[0])
                    success |= kosync_request(lease.bearer, &cfg, &job.snapshot, APP_READING_SYNC_PUSH, NULL);
            }
        } else {
            if (job.provider & APP_READING_SYNC_READEST) {
                did |= cfg.access[0] != 0;
                success |= readest_sync(lease.bearer, &cfg, &job.snapshot, job.action, &remote);
            }
            if (job.provider & APP_READING_SYNC_KOSYNC) {
                did |= cfg.ko_user[0] != 0;
                success |= kosync_request(lease.bearer, &cfg, &job.snapshot, job.action, &remote);
            }
            if (job.action == APP_READING_SYNC_PULL && remote >= 0.0f) {
                portENTER_CRITICAL(&s_lock);
                s_status.remote_ready = true;
                s_status.remote_force = true;
                s_status.remote_percent = remote;
                snprintf(s_status.remote_path, sizeof(s_status.remote_path), "%s", job.snapshot.path);
                snprintf(s_status.message, sizeof(s_status.message), "雲端閱讀位置 %.0f%%", remote * 100.0f);
                portEXIT_CRITICAL(&s_lock);
            }
        }
        ok = did && success;
        status_text(s_status.message, sizeof(s_status.message), ok ? "閱讀進度同步完成" : "無可用的同步帳號或同步失敗");
    }
    network_service_release(&lease);
    mbedtls_platform_zeroize(&cfg, sizeof(cfg));
    portENTER_CRITICAL(&s_lock);
    s_status.busy = false;
    s_task = NULL;
    s_active_job = (queued_job_t){0};
    s_status.pending = s_pending_count != 0;
    if (job.job == JOB_LOGIN) snprintf(s_status.message, sizeof(s_status.message), "%s", ok ? "Readest 已登入" : "Readest 登入失敗");
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

static bool queue_job(const queued_job_t *job) {
    if (!job) return false;
    app_reading_sync_init();
    portENTER_CRITICAL(&s_lock);
    bool queued = !s_backup_restore_busy && pending_insert_locked(job, false);
    s_status.pending = s_pending_count != 0;
    if (!queued) {
        snprintf(s_status.message, sizeof(s_status.message), "同步佇列已滿，請稍後重試");
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    if (job->job == JOB_LOGIN) snprintf(s_status.readest_state, sizeof(s_status.readest_state), "等待網路登入");
    else if (job->job == JOB_REGISTER) snprintf(s_status.kosync_state, sizeof(s_status.kosync_state), "等待網路註冊");
    else snprintf(s_status.message, sizeof(s_status.message), "等待網路和時間校準");
    portEXIT_CRITICAL(&s_lock);
    return queued;
}

void app_reading_sync_poll(void) {
    app_reading_sync_init();
    const pmu_snapshot_t *pmu = device_power_get();
    if (!pmu || !pmu->time_synced || pmu->unix_sec < 1704067200) return;
    portENTER_CRITICAL(&s_lock);
    if (s_task || !s_pending_count || esp_timer_get_time() < s_retry_after_us) { portEXIT_CRITICAL(&s_lock); return; }
    queued_job_t job = pending_pop_locked();
    s_active_job = job;
    s_status.busy = true;
    s_status.pending = false;
    if (job.job == JOB_SYNC) {
        if ((job.provider & APP_READING_SYNC_READEST) && s_config.access[0])
            snprintf(s_status.readest_state, sizeof(s_status.readest_state), "同步中");
        if ((job.provider & APP_READING_SYNC_KOSYNC) && s_config.ko_user[0])
            snprintf(s_status.kosync_state, sizeof(s_status.kosync_state), "同步中");
    }
    BaseType_t created = xTaskCreate(worker, "reading_sync", 10240, NULL, 3, &s_task);
    if (created != pdPASS) {
        s_task = NULL;
        s_status.busy = false;
        (void)pending_insert_locked(&job, true);
        s_active_job = (queued_job_t){0};
        s_status.pending = s_pending_count != 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

bool app_reading_sync_queue(const app_reading_sync_snapshot_t *snapshot,
                            app_reading_sync_provider_t provider,
                            app_reading_sync_action_t action) {
    if (!snapshot || !snapshot->path[0] || provider < APP_READING_SYNC_READEST || provider > APP_READING_SYNC_BOTH)
        return false;
    queued_job_t job = {.job = JOB_SYNC, .provider = provider, .action = action,
        .snapshot = *snapshot, .has_snapshot = true};
    return queue_job(&job);
}

bool app_reading_sync_request_readest_login(void) {
    return queue_job(&(queued_job_t){.job = JOB_LOGIN});
}

static bool url_valid_https(const char *url) {
    if (!url || strnlen(url, 256) >= 256 || strncmp(url, "https://", 8)) return false;
    const char *host = url + 8;
    if (!*host || strchr(host, ' ') || strchr(host, '\\') || strchr(host, '?') || strchr(host, '#')) return false;
    return strchr(host, '.') != NULL || !strncmp(host, "localhost", 9);
}

static bool sync_backup_string_valid(const char *value, size_t capacity) {
    return value && memchr(value, 0, capacity) != NULL;
}

static bool sync_backup_hex_valid(const char *value, size_t expected) {
    if (!value || strnlen(value, expected + 1) != expected) return false;
    for (size_t i = 0; i < expected; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool app_reading_sync_backup_valid(const app_reading_sync_backup_t *backup) {
    if (!backup ||
        !sync_backup_string_valid(backup->readest_access, sizeof(backup->readest_access)) ||
        !sync_backup_string_valid(backup->readest_refresh, sizeof(backup->readest_refresh)) ||
        !sync_backup_string_valid(backup->readest_user, sizeof(backup->readest_user)) ||
        !sync_backup_string_valid(backup->kosync_username, sizeof(backup->kosync_username)) ||
        !sync_backup_string_valid(backup->kosync_key, sizeof(backup->kosync_key)) ||
        !sync_backup_string_valid(backup->kosync_server, sizeof(backup->kosync_server)) ||
        !sync_backup_string_valid(backup->kosync_device_id, sizeof(backup->kosync_device_id)) ||
        !url_valid_https(backup->kosync_server)) return false;

    const bool access = backup->readest_access[0] != 0;
    const bool refresh = backup->readest_refresh[0] != 0;
    if (access != refresh) return false;
    if (access) {
        if (!backup->readest_expires) return false;
    } else if (backup->readest_user[0] || backup->readest_expires) return false;

    const bool username = backup->kosync_username[0] != 0;
    const size_t key_len = strnlen(backup->kosync_key, sizeof(backup->kosync_key));
    if (username != (key_len == 32)) return false;
    if (key_len && !sync_backup_hex_valid(backup->kosync_key, 32)) return false;
    const size_t device_len = strnlen(backup->kosync_device_id, sizeof(backup->kosync_device_id));
    if (device_len != 32 || !sync_backup_hex_valid(backup->kosync_device_id, 32)) return false;
    return true;
}

bool app_reading_sync_backup_export(app_reading_sync_backup_t *out) {
    if (!out) return false;
    app_reading_sync_init();
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_lock);
    memcpy(out->readest_access, s_config.access, sizeof(out->readest_access));
    memcpy(out->readest_refresh, s_config.refresh, sizeof(out->readest_refresh));
    memcpy(out->readest_user, s_config.user, sizeof(out->readest_user));
    out->readest_expires = s_config.expires;
    memcpy(out->kosync_username, s_config.ko_user, sizeof(out->kosync_username));
    memcpy(out->kosync_key, s_config.ko_key, sizeof(out->kosync_key));
    memcpy(out->kosync_server, s_config.ko_url, sizeof(out->kosync_server));
    memcpy(out->kosync_device_id, s_config.ko_device, sizeof(out->kosync_device_id));
    portEXIT_CRITICAL(&s_lock);
    return app_reading_sync_backup_valid(out);
}

static esp_err_t sync_backup_set_or_erase_string(nvs_handle_t handle, const char *key, const char *value) {
    if (value[0]) return nvs_set_str(handle, key, value);
    esp_err_t err = nvs_erase_key(handle, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

bool app_reading_sync_backup_restore(const app_reading_sync_backup_t *backup) {
    app_reading_sync_init();
    if (!app_reading_sync_backup_valid(backup)) return false;
    portENTER_CRITICAL(&s_lock);
    const bool idle = s_task == NULL && s_pending_count == 0 && !s_backup_restore_busy;
    if (idle) s_backup_restore_busy = true;
    portEXIT_CRITICAL(&s_lock);
    if (!idle) return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("rd_sync", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = sync_backup_set_or_erase_string(handle, "rd_access", backup->readest_access);
        if (err == ESP_OK) err = sync_backup_set_or_erase_string(handle, "rd_refresh", backup->readest_refresh);
        if (err == ESP_OK) err = sync_backup_set_or_erase_string(handle, "rd_user", backup->readest_user);
        if (err == ESP_OK && backup->readest_access[0]) err = nvs_set_u64(handle, "rd_expiry", backup->readest_expires);
        else if (err == ESP_OK) {
            err = nvs_erase_key(handle, "rd_expiry");
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        }
        if (err == ESP_OK) err = sync_backup_set_or_erase_string(handle, "ko_user", backup->kosync_username);
        if (err == ESP_OK) err = sync_backup_set_or_erase_string(handle, "ko_key", backup->kosync_key);
        if (err == ESP_OK) err = nvs_set_str(handle, "ko_url", backup->kosync_server);
        if (err == ESP_OK) err = sync_backup_set_or_erase_string(handle, "ko_device", backup->kosync_device_id);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }

    portENTER_CRITICAL(&s_lock);
    if (err == ESP_OK) {
        mbedtls_platform_zeroize(&s_config, sizeof(s_config));
        memcpy(s_config.access, backup->readest_access, sizeof(s_config.access));
        memcpy(s_config.refresh, backup->readest_refresh, sizeof(s_config.refresh));
        memcpy(s_config.user, backup->readest_user, sizeof(s_config.user));
        s_config.expires = backup->readest_expires;
        memcpy(s_config.ko_user, backup->kosync_username, sizeof(s_config.ko_user));
        memcpy(s_config.ko_key, backup->kosync_key, sizeof(s_config.ko_key));
        memcpy(s_config.ko_url, backup->kosync_server, sizeof(s_config.ko_url));
        memcpy(s_config.ko_device, backup->kosync_device_id, sizeof(s_config.ko_device));
        s_status.readest_logged_in = s_config.access[0] && s_config.refresh[0];
        s_status.kosync_configured = s_config.ko_user[0] && strlen(s_config.ko_key) == 32;
        snprintf(s_status.readest_state, sizeof(s_status.readest_state), "%s",
                 s_status.readest_logged_in ? "已登入" : "尚未登入");
        snprintf(s_status.kosync_state, sizeof(s_status.kosync_state), "%s",
                 s_status.kosync_configured ? "已設定" : "尚未設定");
        snprintf(s_status.kosync_username, sizeof(s_status.kosync_username), "%s", s_config.ko_user);
        snprintf(s_status.kosync_server, sizeof(s_status.kosync_server), "%s", s_config.ko_url);
        s_status.remote_ready = false;
        s_status.remote_path[0] = 0;
        s_initialized = true;
    }
    s_backup_restore_busy = false;
    portEXIT_CRITICAL(&s_lock);
    return err == ESP_OK;
}

bool app_reading_sync_configure_kosync(const char *username, const char *password,
                                       const char *server) {
    app_reading_sync_init();
    if (!username || strnlen(username, 64) >= 64 || !username[0] ||
        !server || !url_valid_https(server) || !password || strnlen(password, 128) >= 128)
        return false;
    sync_config_t config;
    portENTER_CRITICAL(&s_lock);
    const bool restore_busy = s_backup_restore_busy;
    config = s_config;
    portEXIT_CRITICAL(&s_lock);
    if (restore_busy) return false;
    if (password[0] && !md5_hex(password, strlen(password), config.ko_key)) return false;
    if (strlen(config.ko_key) != 32 || !copy_cstr(config.ko_user, sizeof(config.ko_user), username) ||
        !copy_cstr(config.ko_url, sizeof(config.ko_url), server)) return false;
    size_t url_len = strlen(config.ko_url);
    while (url_len > 8 && config.ko_url[url_len - 1] == '/') config.ko_url[--url_len] = 0;
    nvs_handle_t h;
    if (nvs_open("rd_sync", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, "ko_user", config.ko_user);
    if (err == ESP_OK) err = nvs_set_str(h, "ko_key", config.ko_key);
    if (err == ESP_OK) err = nvs_set_str(h, "ko_url", config.ko_url);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_config.ko_user, sizeof(s_config.ko_user), "%s", config.ko_user);
    snprintf(s_config.ko_key, sizeof(s_config.ko_key), "%s", config.ko_key);
    snprintf(s_config.ko_url, sizeof(s_config.ko_url), "%s", config.ko_url);
    s_status.kosync_configured = true;
    snprintf(s_status.kosync_username, sizeof(s_status.kosync_username), "%s", config.ko_user);
    snprintf(s_status.kosync_server, sizeof(s_status.kosync_server), "%s", config.ko_url);
    snprintf(s_status.kosync_state, sizeof(s_status.kosync_state), "已設定");
    portEXIT_CRITICAL(&s_lock);
    mbedtls_platform_zeroize(&config, sizeof(config));
    return true;
}

bool app_reading_sync_request_kosync_register(void) {
    app_reading_sync_init();
    portENTER_CRITICAL(&s_lock);
    bool configured = s_config.ko_user[0] && strlen(s_config.ko_key) == 32;
    portEXIT_CRITICAL(&s_lock);
    if (!configured) return false;
    return queue_job(&(queued_job_t){.job = JOB_REGISTER});
}

bool app_reading_sync_take_remote(const char *path, float *percent, bool *force) {
    if (!path || !percent) return false;
    bool ready = false;
    portENTER_CRITICAL(&s_lock);
    if (s_status.remote_ready && !strcmp(path, s_status.remote_path)) {
        *percent = s_status.remote_percent;
        if (force) *force = s_status.remote_force;
        s_status.remote_ready = false;
        s_status.remote_force = false;
        ready = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ready;
}
