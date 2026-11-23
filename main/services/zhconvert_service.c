/* SPDX-License-Identifier: Apache-2.0
 * Staged TXT/EPUB conversion using the Tianhuo Cdict exact converter.
 * Source files are read-only. Output is written beside the source on the SD card.
 */
#include "zhconvert_service.h"

#include "../book/zip_reader.h"
#include "../book/gbk.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "memory_peak.h"
#include "network_http.h"
#include "network_service.h"
#include "network_policy.h"
#include "mbedtls/platform_util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZH_API_CONVERT "https://convert.cdict.info/api/convert.php"
#define ZH_USER_AGENT "Fan/1.1.8 (e-reader book conversion)"
#define ZH_WORKER_STACK 12288
#define ZH_IO_TIMEOUT_MS 25000
#define ZH_MAX_CHUNK 131072u
#define ZH_MAX_XML_EVENTS 8192u
#define ZH_MAX_MANIFEST_ITEMS 2048u
#define ZH_ZIP_ENTRY_LIMIT ZIP_OUTPUT_MAX
#define ZH_ZIP_TOTAL_LIMIT (256u * 1024u * 1024u)
#define ZH_MARKER "ZZFANPROTECTMARKERXZQ"
#define ZH_MARKER_LEN (sizeof(ZH_MARKER) - 1)
#define ZH_STACK_MEM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    uint32_t source, length;
    bool markup, cdata;
} xml_event_t;
typedef struct {
    uint8_t *data;
    size_t length, capacity, limit;
} buffer_t;
static bool file_write(FILE *f, const void *data, size_t length);
typedef struct {
    const char *name;
    uint16_t name_len;
    uint16_t flags;
    uint32_t crc, size, offset;
} zip_out_entry_t;
typedef struct {
    char href[1025];
    char media[96];
    char id[128];
} manifest_item_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_zhconvert_status_t s_status;
static TaskHandle_t s_task;
static atomic_bool s_cancel;
static network_http_session_t s_http;
static network_lease_t *s_lease;

static bool cancelled(void) { return atomic_load(&s_cancel); }
static void progress(unsigned done, unsigned total) {
    portENTER_CRITICAL(&s_lock);
    s_status.completed = done; s_status.total = total; ++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
}
void app_zhconvert_status(app_zhconvert_status_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock); *out = s_status; portEXIT_CRITICAL(&s_lock);
}
void app_zhconvert_cancel(void) { atomic_store(&s_cancel, true); }

static bool buffer_reserve(buffer_t *b, size_t extra) {
    if (extra > b->limit - b->length) return false;
    size_t need = b->length + extra;
    if (need <= b->capacity) return true;
    size_t cap = b->capacity ? b->capacity : 4096;
    while (cap < need) {
        if (cap > b->limit / 2) { cap = b->limit; break; }
        cap *= 2;
    }
    uint8_t *p = heap_caps_realloc(b->data, cap, ZH_STACK_MEM);
    if (!p) return false;
    b->data = p; b->capacity = cap;
    return true;
}
static bool buffer_add(buffer_t *b, const void *data, size_t length) {
    if (!buffer_reserve(b, length)) return false;
    if (length) memcpy(b->data + b->length, data, length);
    b->length += length; return true;
}
static void buffer_free(buffer_t *b) {
    if (b->data) { mbedtls_platform_zeroize(b->data, b->capacity); heap_caps_free(b->data); }
    memset(b, 0, sizeof(*b));
}

static int http_timeout(void) {
    return cancelled() ? 1 : ZH_IO_TIMEOUT_MS;
}
static bool http_read_response(esp_http_client_handle_t client, char *dst, size_t capacity,
                               size_t *length) {
    size_t used = 0;
    while (!cancelled() && used + 1 < capacity) {
        if (esp_http_client_set_timeout_ms(client, http_timeout()) != ESP_OK) return false;
        int n = esp_http_client_read(client, dst + used, (int)(capacity - used - 1));
        if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (n < 0) return false;
        if (!n) {
            if (!esp_http_client_is_complete_data_received(client)) return false;
            dst[used] = 0; *length = used; return true;
        }
        used += (size_t)n;
    }
    return false;
}
static bool http_json(const char *url, esp_http_client_method_t method,
                      const char *body, char **response, size_t *response_len) {
    esp_http_client_config_t config = {
        .url = url, .method = method, .timeout_ms = ZH_IO_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
        .keep_alive_enable = false,
    };
    esp_err_t err = ESP_OK;
    esp_http_client_handle_t client = network_http_client_init(&config, s_lease->bearer, &s_http, &err);
    if (!client) return false;
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "User-Agent", ZH_USER_AGENT);
    size_t request_len = body ? strlen(body) : 0;
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
        err = esp_http_client_open(client, request_len);
        size_t sent = 0;
        while (err == ESP_OK && sent < request_len && !cancelled()) {
            esp_http_client_set_timeout_ms(client, http_timeout());
            int n = esp_http_client_write(client, body + sent, (int)(request_len - sent));
            if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            if (n <= 0) { err = ESP_FAIL; break; }
            sent += (size_t)n;
        }
        if (sent != request_len) err = ESP_FAIL;
    } else err = esp_http_client_open(client, 0);
    int64_t content_length = err == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
    bool ok = false;
    if (err == ESP_OK && content_length >= 0 && content_length <= 2 * 1024 * 1024 &&
        esp_http_client_get_status_code(client) == 200) {
        size_t cap = (size_t)content_length + 2;
        if (cap < 4096) cap = 4096;
        if (cap <= 2 * 1024 * 1024 + 1) {
            char *buf = heap_caps_malloc(cap, ZH_STACK_MEM);
            if (buf && http_read_response(client, buf, cap, response_len)) {
                *response = buf; ok = true;
            } else heap_caps_free(buf);
        }
    }
    network_http_note_result(&s_http, ok || esp_http_client_get_status_code(client) == 200);
    esp_http_client_close(client); esp_http_client_cleanup(client);
    return ok;
}
static cJSON *parse_json(char *json) {
    return cJSON_ParseWithLength(json, strlen(json));
}
static bool api_convert(const char *text, size_t text_len, char **converted, size_t *converted_len) {
    if (!text || !text_len || text_len > ZH_MAX_CHUNK || text_len == SIZE_MAX) return false;
    char *input = heap_caps_malloc(text_len + 1, ZH_STACK_MEM);
    if (!input) return false;
    memcpy(input, text, text_len); input[text_len] = 0;
    cJSON *request = cJSON_CreateObject();
    bool request_ok = request && cJSON_AddStringToObject(request, "text", input) &&
                      cJSON_AddStringToObject(request, "dir", "s2t");
    char *body = request_ok ? cJSON_PrintUnformatted(request) : NULL;
    cJSON_Delete(request);
    mbedtls_platform_zeroize(input, text_len + 1); heap_caps_free(input);
    if (!body) return false;
    char *json = NULL; size_t json_len = 0;
    bool ok = http_json(ZH_API_CONVERT, HTTP_METHOD_POST, body, &json, &json_len);
    mbedtls_platform_zeroize(body, strlen(body)); free(body);
    if (!ok) return false;
    cJSON *root = parse_json(json);
    cJSON *success = root ? cJSON_GetObjectItemCaseSensitive(root, "ok") : NULL;
    cJSON *out = root ? cJSON_GetObjectItemCaseSensitive(root, "result") : NULL;
    const char *value = cJSON_IsString(out) ? out->valuestring : NULL;
    size_t out_len = value ? strlen(value) : 0;
    if (cJSON_IsTrue(success) && value && out_len <= 2 * ZH_MAX_CHUNK && !memchr(value, 0, out_len)) {
        char *copy = heap_caps_malloc(out_len + 1, ZH_STACK_MEM);
        if (copy) { memcpy(copy, value, out_len + 1); *converted = copy; *converted_len = out_len; }
        else value = NULL;
    } else value = NULL;
    ok = value != NULL;
    cJSON_Delete(root); mbedtls_platform_zeroize(json, json_len); heap_caps_free(json);
    return ok;
}

static bool utf8_valid(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = s[i]; if (c < 0x80) { ++i; continue; }
        unsigned k = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!k || k > n - i) return false;
        uint32_t cp = c & (0x7f >> k);
        for (unsigned j = 1; j < k; ++j) { if ((s[i+j] & 0xc0) != 0x80) return false; cp = cp << 6 | (s[i+j] & 63); }
        if ((k == 2 && cp < 0x80) || (k == 3 && cp < 0x800) || (k == 4 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return false;
        i += k;
    }
    return true;
}
typedef struct {
    uint32_t codepoint;
    uint16_t encoded;
} gbk_reverse_t;
static int gbk_reverse_compare(const void *left, const void *right) {
    const gbk_reverse_t *a = left, *b = right;
    if (a->codepoint != b->codepoint) return a->codepoint < b->codepoint ? -1 : 1;
    return a->encoded < b->encoded ? -1 : a->encoded > b->encoded;
}
static gbk_reverse_t *gbk_reverse_create(void) {
    const size_t capacity = 126u * 190u;
    gbk_reverse_t *map = heap_caps_malloc(capacity * sizeof(*map), ZH_STACK_MEM);
    if (!map) return NULL;
    size_t used = 0;
    for (unsigned lead = 0x81; lead <= 0xfe; ++lead) {
        for (unsigned trail = 0x40; trail <= 0xfe; ++trail) {
            if (trail == 0x7f) continue;
            uint32_t cp = gbk_codepoint((unsigned char)lead, (unsigned char)trail);
            if (cp == 0xfffd || cp < 0x80) continue;
            map[used++] = (gbk_reverse_t){.codepoint = cp,
                .encoded = (uint16_t)((lead << 8) | trail)};
        }
    }
    qsort(map, used, sizeof(*map), gbk_reverse_compare);
    return map;
}
static bool gbk_encode_utf8(const char *src, size_t length, const gbk_reverse_t *map,
                            size_t map_count, FILE *out) {
    size_t at = 0;
    while (at < length) {
        const uint8_t *s = (const uint8_t *)src + at;
        uint8_t first = s[0]; uint32_t cp; size_t width;
        if (first < 0x80) { cp = first; width = 1; }
        else {
            width = first >= 0xc2 && first <= 0xdf ? 2 :
                first >= 0xe0 && first <= 0xef ? 3 :
                first >= 0xf0 && first <= 0xf4 ? 4 : 0;
            if (!width || width > length - at) return false;
            cp = first & (0x7f >> width);
            for (size_t i = 1; i < width; ++i) {
                if ((s[i] & 0xc0) != 0x80) return false;
                cp = (cp << 6) | (s[i] & 0x3f);
            }
            if ((width == 2 && cp < 0x80) || (width == 3 && cp < 0x800) ||
                (width == 4 && cp < 0x10000) || cp > 0x10ffff ||
                (cp >= 0xd800 && cp <= 0xdfff)) return false;
        }
        if (cp < 0x80) {
            if (fputc((int)cp, out) == EOF) return false;
        } else {
            size_t low = 0, high = map_count;
            while (low < high) {
                size_t middle = low + (high - low) / 2;
                if (map[middle].codepoint < cp) low = middle + 1;
                else high = middle;
            }
            if (low == map_count || map[low].codepoint != cp) return false;
            uint16_t code = map[low].encoded;
            uint8_t bytes[2] = {(uint8_t)(code >> 8), (uint8_t)code};
            if (!file_write(out, bytes, sizeof(bytes))) return false;
        }
        at += width;
    }
    return true;
}
typedef struct {
    bool valid;
    unsigned remaining;
    uint32_t codepoint, minimum;
} utf8_scan_t;
static void utf8_scan_byte(utf8_scan_t *scan, uint8_t byte) {
    if (!scan->valid || byte == 0) { scan->valid = false; return; }
    if (!scan->remaining) {
        if (byte < 0x80) return;
        if (byte >= 0xc2 && byte <= 0xdf) {
            scan->remaining = 1; scan->codepoint = byte & 0x1f; scan->minimum = 0x80;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            scan->remaining = 2; scan->codepoint = byte & 0x0f; scan->minimum = 0x800;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            scan->remaining = 3; scan->codepoint = byte & 0x07; scan->minimum = 0x10000;
        } else scan->valid = false;
        return;
    }
    if ((byte & 0xc0) != 0x80) { scan->valid = false; return; }
    scan->codepoint = (scan->codepoint << 6) | (byte & 0x3f);
    if (--scan->remaining == 0 && (scan->codepoint < scan->minimum ||
        (scan->codepoint >= 0xd800 && scan->codepoint <= 0xdfff) ||
        scan->codepoint > 0x10ffff)) scan->valid = false;
}
static bool txt_detect_encoding(FILE *in, size_t offset, bool *is_gbk) {
    uint8_t block[4096];
    utf8_scan_t utf8 = {.valid = true};
    bool gbk = true, gbk_lead_pending = false;
    uint8_t gbk_lead = 0;
    if (fseek(in, (long)offset, SEEK_SET)) return false;
    for (;;) {
        size_t n = fread(block, 1, sizeof(block), in);
        if (!n) { if (ferror(in)) return false; break; }
        for (size_t i = 0; i < n; ++i) {
            uint8_t byte = block[i];
            utf8_scan_byte(&utf8, byte);
            if (gbk_lead_pending) {
                uint32_t cp = gbk_codepoint(gbk_lead, byte);
                if (byte < 0x40 || byte > 0xfe || byte == 0x7f || cp == 0xfffd) gbk = false;
                gbk_lead_pending = false;
            } else if (byte < 0x80) {
                /* ASCII is valid in both encodings. */
            } else if (byte >= 0x81 && byte <= 0xfe) {
                gbk_lead = byte; gbk_lead_pending = true;
            } else gbk = false;
        }
    }
    if (utf8.remaining) utf8.valid = false;
    if (gbk_lead_pending) gbk = false;
    if (utf8.valid) *is_gbk = false;
    else if (gbk) *is_gbk = true;
    else return false;
    clearerr(in);
    return fseek(in, (long)offset, SEEK_SET) == 0;
}
static bool make_output_paths(const char *src, bool epub, char *tmp, size_t tmp_cap,
                              char *dst, size_t dst_cap) {
    if (!src || strncmp(src, "/sdcard/", 8)) return false;
    size_t length = strlen(src);
    const char *ext = epub ? ".epub" : ".txt";
    size_t ext_len = strlen(ext);
    if (length <= ext_len || strcasecmp(src + length - ext_len, ext)) return false;
    size_t stem = length - ext_len;
    if (snprintf(dst, dst_cap, "%.*s.zh-TW%s", (int)stem, src, ext) >= (int)dst_cap) return false;
    struct stat st;
    if (stat(dst, &st) == 0 || errno != ENOENT) return false;
    if (snprintf(tmp, tmp_cap, "%s.part", dst) >= (int)tmp_cap) return false;
    (void)unlink(tmp);
    return true;
}
static bool file_write(FILE *f, const void *data, size_t length) {
    return !length || fwrite(data, 1, length, f) == length;
}
static bool convert_txt(const char *src, const char *tmp, const char **failure) {
    FILE *in = fopen(src, "rb");
    if (!in) { *failure = "無法讀取 TXT；原檔保留"; return false; }
    uint8_t probe[4] = {0}; size_t got = fread(probe, 1, sizeof(probe), in);
    if (ferror(in)) { fclose(in); *failure = "TXT 讀取失敗；原檔保留"; return false; }
    size_t bom = got >= 3 && !memcmp(probe, "\xef\xbb\xbf", 3) ? 3 : 0;
    if (!bom && got >= 2 && ((probe[0] == 0xff && probe[1] == 0xfe) || (probe[0] == 0xfe && probe[1] == 0xff))) {
        fclose(in); *failure = "TXT 編碼不支援；請使用 UTF-8 或 GBK，原檔保留"; return false;
    }
    bool is_gbk = false;
    if (!txt_detect_encoding(in, bom, &is_gbk)) {
        fclose(in); *failure = "TXT 編碼不支援；請使用 UTF-8 或 GBK，原檔保留"; return false;
    }
    gbk_reverse_t *reverse_map = is_gbk ? gbk_reverse_create() : NULL;
    if (is_gbk && !reverse_map) { fclose(in); *failure = "記憶體不足；原檔保留"; return false; }
    size_t reverse_count = 0;
    if (reverse_map) {
        for (unsigned lead = 0x81; lead <= 0xfe; ++lead) {
            for (unsigned trail = 0x40; trail <= 0xfe; ++trail) {
                if (trail != 0x7f && gbk_codepoint((unsigned char)lead, (unsigned char)trail) != 0xfffd &&
                    gbk_codepoint((unsigned char)lead, (unsigned char)trail) >= 0x80) ++reverse_count;
            }
        }
    }
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        if (reverse_map) { mbedtls_platform_zeroize(reverse_map, 126u * 190u * sizeof(*reverse_map)); heap_caps_free(reverse_map); }
        fclose(in);
        *failure = "無法建立暫存檔；原檔保留";
        return false;
    }
    bool ok = !bom || file_write(out, probe, bom);
    size_t chunk_cap = ZH_MAX_CHUNK;
    uint8_t *input = heap_caps_malloc(chunk_cap + 4, ZH_STACK_MEM);
    char *utf8_input = is_gbk ? heap_caps_malloc(chunk_cap * 2 + 4, ZH_STACK_MEM) : NULL;
    if (!input || (is_gbk && !utf8_input)) ok = false;
    unsigned chunks = 0;
    while (ok && !cancelled()) {
        size_t n = fread(input, 1, chunk_cap, in);
        if (!n) { if (ferror(in)) ok = false; break; }
        bool eof = feof(in);
        size_t use = n;
        if (!eof && use && is_gbk && (input[use - 1] >= 0x81 && input[use - 1] <= 0xfe)) {
            --use;
        } else if (!eof && use && !is_gbk && !utf8_valid(input, use)) {
            while (use && !utf8_valid(input, use)) --use;
        }
        if (!use) { ok = false; break; }
        /* Prefer a paragraph boundary, while keeping at least half of each chunk. */
        if (!eof && use > 1024) {
            size_t boundary = use;
            while (boundary > use / 2 && input[boundary - 1] != '\n') --boundary;
            if (boundary > use / 2 && boundary < use) use = boundary;
        }
        if (use < n && fseek(in, -(long)(n - use), SEEK_CUR)) { ok = false; break; }
        const char *api_input = (const char *)input; size_t api_len = use;
        if (is_gbk) {
            api_len = gbk_to_utf8((const char *)input, use, utf8_input, chunk_cap * 2 + 4);
            if (!api_len || !utf8_valid((const uint8_t *)utf8_input, api_len)) { ok = false; break; }
            api_input = utf8_input;
        } else if (!utf8_valid(input, use)) { ok = false; break; }
        char *converted = NULL; size_t converted_len = 0;
        ok = api_convert(api_input, api_len, &converted, &converted_len);
        if (!ok) *failure = "天火轉碼轉換失敗；原檔保留";
        if (ok && is_gbk && !gbk_encode_utf8(converted, converted_len, reverse_map, reverse_count, out)) {
            ok = false; *failure = "轉換結果無法以原 GBK 編碼表示；原檔保留";
        }
        if (ok && !is_gbk && !file_write(out, converted, converted_len)) {
            ok = false; *failure = "儲存 TXT 結果失敗；原檔保留";
        }
        if (converted) { mbedtls_platform_zeroize(converted, converted_len); heap_caps_free(converted); }
        progress(++chunks, 0);
    }
    if (cancelled()) ok = false;
    if (input) { mbedtls_platform_zeroize(input, chunk_cap + 4); heap_caps_free(input); }
    if (utf8_input) { mbedtls_platform_zeroize(utf8_input, chunk_cap * 2 + 4); heap_caps_free(utf8_input); }
    if (reverse_map) { mbedtls_platform_zeroize(reverse_map, 126u * 190u * sizeof(*reverse_map)); heap_caps_free(reverse_map); }
    if (fclose(in)) { ok = false; *failure = "TXT 讀取失敗；原檔保留"; }
    if (fflush(out) || ferror(out)) { ok = false; *failure = "儲存 TXT 結果失敗；原檔保留"; }
    if (fclose(out)) { ok = false; *failure = "儲存 TXT 結果失敗；原檔保留"; }
    if (!ok && !*failure) *failure = "TXT 轉換失敗；原檔保留";
    return ok;
}

static void put16(uint8_t *p, uint16_t x) { p[0]=(uint8_t)x; p[1]=(uint8_t)(x>>8); }
static void put32(uint8_t *p, uint32_t x) { put16(p,(uint16_t)x); put16(p+2,(uint16_t)(x>>16)); }
static uint32_t crc32_data(const uint8_t *p, size_t n) {
    uint32_t crc=UINT32_MAX;
    while(n--){crc^=*p++;for(unsigned i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
    return ~crc;
}

static bool append_utf8(buffer_t *out, uint32_t cp) {
    char b[4]; size_t n;
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    if (cp < 0x80) { b[0]=(char)cp; n=1; }
    else if (cp < 0x800) { b[0]=(char)(0xc0|(cp>>6)); b[1]=(char)(0x80|(cp&63)); n=2; }
    else if (cp < 0x10000) { b[0]=(char)(0xe0|(cp>>12)); b[1]=(char)(0x80|((cp>>6)&63)); b[2]=(char)(0x80|(cp&63)); n=3; }
    else { b[0]=(char)(0xf0|(cp>>18)); b[1]=(char)(0x80|((cp>>12)&63)); b[2]=(char)(0x80|((cp>>6)&63)); b[3]=(char)(0x80|(cp&63)); n=4; }
    return buffer_add(out,b,n);
}
static bool xml_decode_text(const uint8_t *src, size_t len, buffer_t *out) {
    for (size_t i=0;i<len;) {
        if (src[i]!='&') {
            size_t n=1; while(i+n<len&&(src[i+n]&0xc0)==0x80)++n;
            if (!utf8_valid(src+i,n)||!buffer_add(out,src+i,n)) return false;
            i+=n; continue;
        }
        size_t end=i+1; while(end<len&&end-i<=12&&src[end]!=';')++end;
        if(end>=len||src[end]!=';')return false;
        uint32_t cp=0;
        if(end-i==4&&!memcmp(src+i,"&lt;",4))cp='<';
        else if(end-i==4&&!memcmp(src+i,"&gt;",4))cp='>';
        else if(end-i==5&&!memcmp(src+i,"&amp;",5))cp='&';
        else if(end-i==6&&!memcmp(src+i,"&quot;",6))cp='"';
        else if(end-i==6&&!memcmp(src+i,"&apos;",6))cp='\'';
        else if(end-i==6&&!memcmp(src+i,"&nbsp;",6))cp=0xa0;
        else if(end-i==7&&!memcmp(src+i,"&mdash;",7))cp=0x2014;
        else if(end-i==8&&!memcmp(src+i,"&hellip;",8))cp=0x2026;
        else if(end-i==7&&!memcmp(src+i,"&ndash;",7))cp=0x2013;
        else if(src[i+1]=='#'){
            unsigned base=10;size_t p=i+2;if(p<end&&(src[p]=='x'||src[p]=='X')){base=16;++p;}
            if(p==end)return false;
            for(;p<end;++p){unsigned char c=src[p];unsigned d=c>='0'&&c<='9'?c-'0':base==16&&c>='a'&&c<='f'?c-'a'+10:base==16&&c>='A'&&c<='F'?c-'A'+10:99;if(d>=base||cp>(0x10ffff-d)/base)return false;cp=cp*base+d;}
        } else return false;
        if(!append_utf8(out,cp))return false;
        i=end+1;
    }
    return true;
}
static bool xml_escape_add(buffer_t *out, const char *src, size_t len, bool cdata) {
    if(cdata){if(memmem(src,len,"]]>",3))return false;return buffer_add(out,src,len);}
    for(size_t i=0;i<len;++i){const char *rep=NULL;size_t n=0;switch(src[i]){case '&':rep="&amp;";n=5;break;case '<':rep="&lt;";n=4;break;case '>':rep="&gt;";n=4;break;case '\"':rep="&quot;";n=6;break;case '\'':rep="&apos;";n=6;break;default:break;}if(rep){if(!buffer_add(out,rep,n))return false;}else if(!buffer_add(out,src+i,1))return false;}
    return true;
}
static bool xml_tag_end(const uint8_t *src,size_t length,size_t start,size_t *end,bool *is_cdata_start,bool *is_cdata_end){
    *is_cdata_start=false;*is_cdata_end=false;
    if(start+9<=length&&!memcmp(src+start,"<![CDATA[",9)){*end=start+9;*is_cdata_start=true;return true;}
    if(start+3<=length&&!memcmp(src+start,"]]>",3)){*end=start+3;*is_cdata_end=true;return true;}
    if(start+4<=length&&!memcmp(src+start,"<!--",4)){const uint8_t *close=(const uint8_t*)memmem(src+start+4,length-start-4,"-->",3);if(!close)return false;*end=(size_t)(close-src)+3;return true;}
    bool quote=false;uint8_t q=0;unsigned bracket=0;
    for(size_t i=start+1;i<length;++i){uint8_t c=src[i];if(quote){if(c==q)quote=false;continue;}if(c=='\''||c=='\"'){quote=true;q=c;continue;}if(c=='[')++bracket;else if(c==']'&&bracket)--bracket;else if(c=='>'&&!bracket){*end=i+1;return true;}}
    return false;
}

static bool api_convert_xml_chunk(const uint8_t *source,buffer_t *plain,xml_event_t *events,size_t event_count,buffer_t *dest){
    char *converted=NULL;size_t converted_len=0;
    if(plain->length&&!api_convert((const char*)plain->data,plain->length,&converted,&converted_len))return false;
    size_t pos=0;bool ok=true,cdata=false;
    for(size_t i=0;i<event_count&&ok;++i){xml_event_t *e=&events[i];
        if(!e->markup){cdata=e->cdata;continue;}
        const char *mark=converted?memmem(converted+pos,converted_len-pos,ZH_MARKER,ZH_MARKER_LEN):NULL;
        if(!mark){ok=false;break;}
        size_t n=(size_t)(mark-(converted+pos));
        ok=xml_escape_add(dest,converted+pos,n,cdata)&&buffer_add(dest,source+e->source,e->length);
        pos+=n+ZH_MARKER_LEN;
        if(e->length==9&&!memcmp(source+e->source,"<![CDATA[",9))cdata=true;
        else if(e->length==3&&!memcmp(source+e->source,"]]>",3))cdata=false;
    }
    if(ok){size_t tail=converted_len-pos;const char *extra=converted?memmem(converted+pos,tail,ZH_MARKER,ZH_MARKER_LEN):NULL;if(extra)ok=false;else ok=xml_escape_add(dest,converted?converted+pos:"",tail,cdata);}
    if(converted){mbedtls_platform_zeroize(converted,converted_len);heap_caps_free(converted);}
    return ok;
}

static bool xml_add_markup(buffer_t *plain,xml_event_t *events,size_t *count,uint32_t off,uint32_t len){
    if(*count>=ZH_MAX_XML_EVENTS)return false;
    events[(*count)++]=(xml_event_t){.source=off,.length=len,.markup=true};
    return buffer_add(plain,ZH_MARKER,ZH_MARKER_LEN);
}
static bool xml_add_text(buffer_t *plain,xml_event_t *events,size_t *count,const uint8_t *src,size_t len,bool cdata){
    buffer_t decoded={.limit=ZH_MAX_CHUNK};
    if(cdata){if(!utf8_valid(src,len)||!buffer_add(&decoded,src,len)){buffer_free(&decoded);return false;}}
    else if(!xml_decode_text(src,len,&decoded)){buffer_free(&decoded);return false;}
    if(decoded.length>UINT32_MAX||*count>=ZH_MAX_XML_EVENTS){buffer_free(&decoded);return false;}
    events[(*count)++]=(xml_event_t){.length=(uint32_t)decoded.length,.markup=false,.cdata=cdata};
    bool ok=buffer_add(plain,decoded.data,decoded.length);buffer_free(&decoded);return ok;
}
static bool xml_flush(const uint8_t *source,buffer_t *plain,xml_event_t *events,size_t *count,buffer_t *dest){
    if(!*count)return true;
    bool ok=api_convert_xml_chunk(source,plain,events,*count,dest);
    plain->length=0;*count=0;return ok;
}
static bool xml_convert_document(const uint8_t *src,size_t length,bool metadata,buffer_t *out){
    if(length>ZIP_OUTPUT_MAX||!utf8_valid(src,length)||memmem(src,length,ZH_MARKER,ZH_MARKER_LEN))return false;
    buffer_t plain={.limit=ZH_MAX_CHUNK};xml_event_t *events=heap_caps_calloc(ZH_MAX_XML_EVENTS,sizeof(*events),ZH_STACK_MEM);
    if(!events)return false;
    size_t count=0,pos=0,chunk_limit=ZH_MAX_CHUNK;
    bool cdata=false, allow_text=true, ignored[64]={0}, selected[64]={0};size_t depth=0;
    while(pos<length&&!cancelled()){
        if(src[pos]=='<'){
            size_t end;bool cstart,cend;if(!xml_tag_end(src,length,pos,&end,&cstart,&cend))goto fail;
            if(count&&plain.length+ZH_MARKER_LEN>chunk_limit&&!xml_flush(src,&plain,events,&count,out))goto fail;
            if(!xml_add_markup(&plain,events,&count,(uint32_t)pos,(uint32_t)(end-pos)))goto fail;
            if(cstart)cdata=true;
            if(cend)cdata=false;
            bool declaration=src[pos+1]=='!'||src[pos+1]=='?';
            if(!declaration&&!cstart&&!cend){
                bool closing=src[pos+1]=='/';size_t name_at=pos+(closing?2:1);char name[48]={0};size_t name_len=0;
                while(name_at<end&&name_len+1<sizeof(name)&&(isalnum(src[name_at])||src[name_at]==':'||src[name_at]=='_'||src[name_at]=='-'))name[name_len++]=(char)src[name_at++];
                char *local=strrchr(name,':');local=local?local+1:name;
                if(closing){if(depth) --depth;}
                else{
                    bool parent_ignore=depth?ignored[depth-1]:false;
                    bool parent_select=depth?selected[depth-1]:false;
                    bool self=end>pos&&src[end-2]=='/';
                    bool this_ignore=parent_ignore||(!metadata&&(!strcmp(local,"script")||!strcmp(local,"style")));
                    static const char *const fields[]={"title","creator","description","subject","publisher"};
                    bool this_select=metadata&&(parent_select);
                    if(metadata)for(size_t k=0;k<sizeof(fields)/sizeof(fields[0]);++k)if(!strcmp(local,fields[k]))this_select=true;
                    if(!self&&depth<64){ignored[depth]=this_ignore;selected[depth]=this_select;++depth;}
                }
            }
            allow_text=metadata?(depth?selected[depth-1]:false):!(depth&&ignored[depth-1]);
            if(cstart)cdata=true;
            if(cend)cdata=false;
            pos=end;continue;
        }
        size_t end=pos;while(end<length&&src[end]!='<')++end;
        size_t n=end-pos;
        if(!allow_text){if(!xml_flush(src,&plain,events,&count,out)||!buffer_add(out,src+pos,n))goto fail;pos=end;continue;}
        size_t take=n;
        if(take>chunk_limit){take=chunk_limit;while(take&&pos+take<end&&(src[pos+take]&0xc0)==0x80)--take;if(!take)goto fail;for(size_t k=take;k>take/2;--k)if(src[pos+k-1]=='\n'){take=k;break;}}
        if(plain.length+take>chunk_limit&&count){if(!xml_flush(src,&plain,events,&count,out))goto fail;}
        if(!xml_add_text(&plain,events,&count,src+pos,take,cdata))goto fail;
        pos+=take;
    }
    if(cancelled()||!xml_flush(src,&plain,events,&count,out))goto fail;
    buffer_free(&plain);heap_caps_free(events);return true;
fail:
    buffer_free(&plain);heap_caps_free(events);return false;
}

static bool attr_value(const uint8_t *tag,size_t length,const char *name,char *out,size_t cap){
    size_t n=strlen(name);for(size_t i=1;i+n+1<length;++i){if(memcmp(tag+i,name,n)||!(isspace(tag[i-1])||tag[i-1]=='<'))continue;size_t p=i+n;while(p<length&&isspace(tag[p]))++p;if(p>=length||tag[p++]!='=')continue;while(p<length&&isspace(tag[p]))++p;if(p>=length||(tag[p]!='\''&&tag[p]!='\"'))continue;uint8_t q=tag[p++];size_t start=p;while(p<length&&tag[p]!=q)++p;if(p>=length||p-start>=cap)return false;memcpy(out,tag+start,p-start);out[p-start]=0;return true;}return false;
}
static bool container_opf(const uint8_t *xml,size_t len,char *out,size_t cap){
    const uint8_t *p=xml,*end=xml+len;while(p<end){const uint8_t *tag=memchr(p,'<',(size_t)(end-p));if(!tag)break;size_t te;bool cs,ce;if(!xml_tag_end(xml,len,(size_t)(tag-xml),&te,&cs,&ce))return false;if(te>(size_t)(end-xml))return false;if(te-(size_t)(tag-xml)>10&&!memcmp(tag,"<rootfile",9)&&attr_value(tag,te-(size_t)(tag-xml),"full-path",out,cap))return true;p=xml+te;}return false;
}
static bool resolve_href(const char *opf,const char *href,char *out,size_t cap){
    if(!href[0]||href[0]=='/'||strstr(href,"://"))return false;
    char clean[1025];size_t n=0;for(size_t i=0;href[i]&&href[i]!='#'&&href[i]!='?';++i){if(href[i]=='%'){if(!isxdigit((unsigned char)href[i+1])||!isxdigit((unsigned char)href[i+2]))return false;unsigned a=isdigit((unsigned char)href[i+1])?href[i+1]-'0':tolower((unsigned char)href[i+1])-'a'+10;unsigned b=isdigit((unsigned char)href[i+2])?href[i+2]-'0':tolower((unsigned char)href[i+2])-'a'+10;clean[n++]=(char)(a*16+b);i+=2;}else clean[n++]=href[i];if(n+1>=sizeof(clean))return false;}clean[n]=0;
    char joined[2048];const char *slash=strrchr(opf,'/');size_t dir=slash?(size_t)(slash-opf+1):0;if(dir+n>=sizeof(joined))return false;memcpy(joined,opf,dir);memcpy(joined+dir,clean,n+1);
    char result[1025];size_t used=0;size_t i=0;while(joined[i]){while(joined[i]=='/')++i;size_t start=i;while(joined[i]&&joined[i]!='/')++i;size_t part=i-start;if(!part|| (part==1&&joined[start]=='.'))continue;if(part==2&&joined[start]=='.'&&joined[start+1]=='.'){if(!used)return false;while(used&&result[used-1]!='/')--used;if(used)--used;continue;}if(used&&result[used-1]!='/')result[used++]='/';if(used+part>=sizeof(result))return false;memcpy(result+used,joined+start,part);used+=part;}result[used]=0;if(!used||used>=cap)return false;memcpy(out,result,used+1);return true;
}
static bool opf_manifest(const uint8_t *opf,size_t len,manifest_item_t *items,size_t capacity,size_t *count,bool *spine_used,size_t spine_cap){
    *count=0;memset(spine_used,0,spine_cap*sizeof(*spine_used));
    const uint8_t *p=opf,*end=opf+len;while(p<end){const uint8_t *tag=memchr(p,'<',(size_t)(end-p));if(!tag)break;size_t te;bool cs,ce;if(!xml_tag_end(opf,len,(size_t)(tag-opf),&te,&cs,&ce))return false;size_t n=te-(size_t)(tag-opf);
        if(n>5&&!memcmp(tag,"<item",5)&&(isspace(tag[5])||tag[5]=='/')){if(*count>=capacity)return false;manifest_item_t *m=&items[*count];if(!attr_value(tag,n,"id",m->id,sizeof(m->id))||!attr_value(tag,n,"href",m->href,sizeof(m->href))||!attr_value(tag,n,"media-type",m->media,sizeof(m->media)))return false;++*count;}
        if(n>8&&!memcmp(tag,"<itemref",8)){char idref[128];if(attr_value(tag,n,"idref",idref,sizeof(idref))){for(size_t i=0;i<*count;++i)if(!strcmp(items[i].id,idref))spine_used[i]=true;}}
        p=opf+te;
    }return true;
}
static bool epub_entry_text(const char *name,const char *opf_path,const manifest_item_t *items,size_t count,const bool *spine){
    if(!strcmp(name,opf_path))return true;
    for(size_t i=0;i<count;++i)if(spine[i]&&(!strcmp(items[i].media,"application/xhtml+xml")||!strcmp(items[i].media,"text/html"))){char resolved[1025];if(resolve_href(opf_path,items[i].href,resolved,sizeof(resolved))&&!strcmp(name,resolved))return true;}
    return false;
}

static bool epub_rewrite(const char *src,const char *tmp,const char **failure){
    zip_reader_t *zip=NULL;
    if(zip_open(src,&zip)!=ESP_OK){*failure="EPUB 格式不支援或檔案損壞；原檔保留";return false;}
    size_t count=zip_entry_count(zip);
    if(!count||count>ZIP_ENTRY_MAX){zip_close(zip);*failure="EPUB 條目數超出安全上限；原檔保留";return false;}
    int container=zip_find(zip,"META-INF/container.xml"),mimetype=zip_find(zip,"mimetype");
    if(container<0||mimetype<0){zip_close(zip);*failure="EPUB 缺少必要結構；原檔保留";return false;}
    if(zip_entry_size(zip,container)>ZH_ZIP_ENTRY_LIMIT||zip_entry_size(zip,mimetype)!=20){zip_close(zip);*failure="EPUB 結構超出安全處理限制；原檔保留";return false;}
    uint8_t mime[20];
    if(zip_extract(zip,mimetype,mime,sizeof(mime))!=ESP_OK||memcmp(mime,"application/epub+zip",sizeof(mime))){zip_close(zip);*failure="EPUB mimetype 無效；原檔保留";return false;}
    for(size_t i=0;i<count;++i){const char *name=zip_entry_name(zip,(int)i);if(!strcmp(name,"META-INF/encryption.xml")){zip_close(zip);*failure="EPUB 含 DRM／加密資源；原檔保留";return false;}}
    uint8_t *data=heap_caps_malloc(zip_entry_size(zip,container)+1,ZH_STACK_MEM);if(!data){zip_close(zip);return false;}
    bool ok=zip_extract(zip,container,data,zip_entry_size(zip,container))==ESP_OK;char opf_path[1025];if(ok)ok=container_opf(data,zip_entry_size(zip,container),opf_path,sizeof(opf_path));heap_caps_free(data);
    int opf_index=ok?zip_find(zip,opf_path):-1;if(opf_index<0||zip_entry_size(zip,opf_index)>ZH_ZIP_ENTRY_LIMIT){zip_close(zip);return false;}
    data=heap_caps_malloc(zip_entry_size(zip,opf_index)+1,ZH_STACK_MEM);if(!data){zip_close(zip);return false;}
    ok=zip_extract(zip,opf_index,data,zip_entry_size(zip,opf_index))==ESP_OK;
    manifest_item_t *items=heap_caps_calloc(ZH_MAX_MANIFEST_ITEMS,sizeof(*items),ZH_STACK_MEM);bool *spine=heap_caps_calloc(ZH_MAX_MANIFEST_ITEMS,sizeof(*spine),ZH_STACK_MEM);size_t item_count=0;
    if(!items||!spine)ok=false;
    if(ok)ok=opf_manifest(data,zip_entry_size(zip,opf_index),items,ZH_MAX_MANIFEST_ITEMS,&item_count,spine,ZH_MAX_MANIFEST_ITEMS);
    heap_caps_free(data);
    FILE *out=ok?fopen(tmp,"wb"):NULL;if(!out)ok=false;
    zip_out_entry_t *out_entries=heap_caps_calloc(count,sizeof(*out_entries),ZH_STACK_MEM);if(!out_entries)ok=false;
    uint32_t out_index=0;uint64_t total_input=0;
    for(size_t order=0;ok&&order<count;++order){size_t i=order==0?(size_t)mimetype:order<= (size_t)mimetype?order-1:order; if(i>=count)continue;size_t raw_size=zip_entry_size(zip,(int)i);const char *name=zip_entry_name(zip,(int)i);total_input+=raw_size;if(total_input>ZH_ZIP_TOTAL_LIMIT||raw_size>ZH_ZIP_ENTRY_LIMIT){ok=false;break;}
        uint8_t *raw=heap_caps_malloc(raw_size?raw_size:1,ZH_STACK_MEM);if(!raw){ok=false;break;}ok=zip_extract(zip,(int)i,raw,raw_size)==ESP_OK;
        buffer_t converted={.limit=ZH_ZIP_ENTRY_LIMIT};bool is_xml=ok&&epub_entry_text(name,opf_path,items,item_count,spine);
        if(ok&&is_xml)ok=xml_convert_document(raw,raw_size,!strcmp(name,opf_path),&converted);
        const uint8_t *payload=is_xml?converted.data:raw;size_t payload_len=is_xml?converted.length:raw_size;
        size_t name_len = strlen(name);
        if(ok&&(name_len>UINT16_MAX||payload_len>UINT32_MAX||ftell(out)<0||
            (uint64_t)ftell(out)+payload_len+46+name_len>UINT32_MAX||out_index>=count))ok=false;
        if(ok){zip_out_entry_t *e=&out_entries[out_index++];e->name=name;e->name_len=(uint16_t)strlen(name);e->size=(uint32_t)payload_len;e->crc=crc32_data(payload,payload_len);e->offset=(uint32_t)ftell(out);e->flags=0;
            uint8_t h[30]={0};put32(h,0x04034b50);put16(h+4,20);put16(h+6,0x0800);put16(h+8,0);put32(h+14,e->crc);put32(h+18,e->size);put32(h+22,e->size);put16(h+26,e->name_len);
            ok=file_write(out,h,sizeof(h))&&file_write(out,name,e->name_len)&&file_write(out,payload,payload_len);}
        buffer_free(&converted);mbedtls_platform_zeroize(raw,raw_size);heap_caps_free(raw);progress((unsigned)order+1,(unsigned)count);
    }
    uint32_t central=ok?(uint32_t)ftell(out):0;for(uint32_t i=0;ok&&i<out_index;++i){zip_out_entry_t *e=&out_entries[i];uint8_t h[46]={0};put32(h,0x02014b50);put16(h+4,20);put16(h+6,20);put16(h+8,0x0800);put16(h+10,0);put32(h+16,e->crc);put32(h+20,e->size);put32(h+24,e->size);put16(h+28,e->name_len);put32(h+42,e->offset);ok=file_write(out,h,sizeof(h))&&file_write(out,e->name,e->name_len);}
    uint32_t end=ok?(uint32_t)ftell(out):0;uint32_t central_size=end-central;if(out_index>UINT16_MAX||end>UINT32_MAX-22u)ok=false;uint8_t eocd[22]={0};put32(eocd,0x06054b50);put16(eocd+8,(uint16_t)out_index);put16(eocd+10,(uint16_t)out_index);put32(eocd+12,central_size);put32(eocd+16,central);if(ok)ok=file_write(out,eocd,sizeof(eocd));
    if(out_entries)heap_caps_free(out_entries);
    if(items)heap_caps_free(items);
    if(spine)heap_caps_free(spine);
    zip_close(zip);
    if(out){if(fflush(out)||ferror(out))ok=false;if(fclose(out))ok=false;}
    if(!ok)(void)unlink(tmp);
    if(!ok&&!*failure)*failure="EPUB 無法安全重建或空間不足；原檔保留";
    return ok;
}

static bool convert_file(const char *source,char *output,size_t output_cap,const char **failure){
    bool epub=false;size_t n=strlen(source);if(n>=5&&!strcasecmp(source+n-5,".epub"))epub=true;else if(n<4||strcasecmp(source+n-4,".txt")){*failure="只支援 TXT 與 EPUB；原檔保留";return false;}
    char tmp[320];
    if(!make_output_paths(source,epub,tmp,sizeof(tmp),output,output_cap)){
        *failure="結果檔已存在或檔名過長；原檔保留";return false;
    }
    FILE *stage=fopen(tmp,"wb");if(!stage){*failure="無法建立暫存檔；原檔保留";return false;}if(fclose(stage)){(void)unlink(tmp);*failure="無法建立暫存檔；原檔保留";return false;}
    bool ok=false;
    if(epub)ok=epub_rewrite(source,tmp,failure);
    else ok=convert_txt(source,tmp,failure);
    if(ok){struct stat st;if(stat(output,&st)==0||errno!=ENOENT||rename(tmp,output)){ok=false;*failure="無法完成結果檔存檔；原檔保留";}}
    if(!ok)(void)unlink(tmp);
    return ok;
}
static void worker(void *arg){
    char path[288];snprintf(path,sizeof(path),"%s",(const char*)arg);free(arg);
    const char *failure=NULL;
    memory_peak_token_t peak=0;bool memory_ok=memory_peak_try_begin(MEMORY_PEAK_WEATHER,&peak)==ESP_OK;bool ok=memory_ok;
    network_lease_t lease={0};s_lease=&lease;
    bool network_ready=false;
    if(!memory_ok)failure="系統忙碌，請稍後重試；原檔保留";
    if(ok){
        const esp_err_t acquired=network_service_acquire(NETWORK_USE_WEATHER,&lease);
        network_ready=acquired==ESP_OK && network_service_wait_ready_cancelable(&lease,30000,cancelled);
        ok=network_ready;
        if(!ok)failure="無法自動連線，請檢查已儲存的網路設定；原檔保留";
    }
    if(ok && cancelled()) { ok=false; failure="已取消，原檔保留"; }
    char output[288]={0};if(ok&&!cancelled())ok=convert_file(path,output,sizeof(output),&failure);
    network_service_release(&lease);s_lease=NULL;
    if(cancelled())ok=false;
    portENTER_CRITICAL(&s_lock);
    if(ok){s_status.state=APP_ZHCONVERT_DONE;snprintf(s_status.output,sizeof(s_status.output),"%s",output);snprintf(s_status.notice,sizeof(s_status.notice),"轉換完成，可在書籍列表預覽新檔");}
    else {
        s_status.state=APP_ZHCONVERT_FAILED;
        const char *notice=cancelled()?"已取消，原檔保留":
            (!memory_ok?"系統忙碌，請稍後重試；原檔保留":
             (!network_ready?"無法自動連線，請檢查已儲存的網路設定":
              (failure?failure:"轉換失敗，原檔保留；請檢查格式或空間")));
        snprintf(s_status.notice,sizeof(s_status.notice),"%s",notice);
    }
    ++s_status.revision;s_task=NULL;portEXIT_CRITICAL(&s_lock);
    memory_peak_finish(peak);vTaskDelete(NULL);
}
bool app_zhconvert_start(const char *path){
    if(!path||strncmp(path,"/sdcard/",8)||strnlen(path,288)>=288)return false;
    const char *ext=strrchr(path,'.');if(!ext||(strcasecmp(ext,".txt")&&strcasecmp(ext,".epub")))return false;
    struct stat st;if(stat(path,&st)||!S_ISREG(st.st_mode))return false;
    char *copy=strdup(path);if(!copy)return false;
    portENTER_CRITICAL(&s_lock);if(s_task){portEXIT_CRITICAL(&s_lock);free(copy);return false;}
    atomic_store(&s_cancel,false);s_status=(app_zhconvert_status_t){.state=APP_ZHCONVERT_BUSY,.revision=s_status.revision+1};
    BaseType_t created=xTaskCreate(worker,"zhconvert",ZH_WORKER_STACK,copy,4,&s_task);
    if(created!=pdPASS){s_task=NULL;portEXIT_CRITICAL(&s_lock);free(copy);return false;}
    portEXIT_CRITICAL(&s_lock);return true;
}
