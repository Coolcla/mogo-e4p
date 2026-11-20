/*
 * 中文：CloudZao 协议、TF 落盘和确认顺序；不依赖 UI 或 ESP HTTP。
 * English: CloudZao protocol, TF publication and acknowledgement order, independent of UI and ESP HTTP.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOUDZAO_JSON_MAX (128u * 1024u)
#define CLOUDZAO_CHUNK (8u * 1024u)
#define CLOUDZAO_URL_MAX 4096u
#define CLOUDZAO_KEY_MAX 128u
#define CLOUDZAO_NAME_MAX 120u
#define CLOUDZAO_LIST_ROWS 128u
#define CLOUDZAO_HISTORY_ROWS 32u

typedef enum { CZ_BIND, CZ_LIST, CZ_DOWNLOAD, CZ_ACK } cz_request_t;
typedef enum { CZ_JOB_BIND, CZ_JOB_PREVIEW, CZ_JOB_DOWNLOAD } cz_job_t;
typedef enum {
    CZ_OK, CZ_INVALID, CZ_AUTH, CZ_SERVER, CZ_TIMEOUT, CZ_WIFI,
    CZ_MEDIA, CZ_CANCELLED, CZ_RESOURCES, CZ_STORAGE, CZ_CONFLICT, CZ_DIGEST
} cz_error_t;

typedef enum {
    CZ_ROW_WAIT, CZ_ROW_CHECK, CZ_ROW_DOWNLOAD, CZ_ROW_VERIFY, CZ_ROW_CONFIRM,
    CZ_ROW_SAVED, CZ_ROW_PENDING, CZ_ROW_SKIPPED, CZ_ROW_FAILED, CZ_ROW_INTERRUPTED, CZ_ROW_UNAVAILABLE,
} cz_row_state_t;

typedef struct {
    uint32_t size;
    cz_row_state_t state;
    char name[CLOUDZAO_NAME_MAX + 1];
} cz_row_t;

typedef enum { CZ_KIND_NONE, CZ_KIND_EPUB, CZ_KIND_TXT, CZ_KIND_PNG, CZ_KIND_TTF } cz_kind_t;
typedef struct {
    cz_row_t file;
    cz_kind_t kind; ///< NONE 表示不可选择。/ NONE means not selectable.
    uint8_t fingerprint[32]; ///< 仅匹配身份，不含任务 ID 或下载地址。/ Match identity without retaining task IDs or links.
} cz_offer_t;
typedef struct { bool checked[CLOUDZAO_LIST_ROWS]; } cz_selection_t;
typedef struct {
    unsigned count;
    cz_selection_t selection;
    cz_offer_t offers[CLOUDZAO_LIST_ROWS];
} cz_queue_t;

typedef struct {
    unsigned revision, rows, current;
    bool active;
    cz_row_t file;
    uint32_t received, rate;
    int64_t last_data_us;
} cz_transfer_t;

typedef struct { cz_row_t file; uint32_t utc; } cz_history_item_t;
typedef struct { bool busy; unsigned count; cz_error_t error; } cz_history_status_t;

typedef struct {
    bool busy;
    cz_error_t error, item_error;
    cz_request_t item_request; ///< 最后一个单项错误所属操作。/ Operation of the last per-item error.
    unsigned total, processed, saved, pending, skipped, failed, unavailable;
    char code[7];
    bool binding_probed; ///< 无验证码时尝试验证；仅 error 为 CZ_OK 时确认绑定。/ Probed without a code; only CZ_OK confirms binding.
} cz_status_t;

typedef bool (*cz_sink_t)(void *context, const void *bytes, size_t length);
typedef struct {
    const char *url;          ///< 仅用于文件请求。/ Only supplied to a file request.
    const char *body;         ///< 绑定和确认的 JSON。/ JSON for binding and acknowledgement.
    const char *key, *mac;    ///< 文件请求始终传 NULL。/ Always NULL for a file request.
    size_t limit;            ///< 允许接收的最大字节数。/ Maximum allowed response bytes.
} cz_http_request_t;
typedef struct {
    void *context;
    const char *root;        ///< 当前 TF 挂载点。/ Current TF mount point.
    cz_error_t (*stop)(void *context);
    cz_error_t (*request)(void *context, cz_request_t operation, const cz_http_request_t *request,
                          cz_sink_t sink, void *sink_context);
    uint64_t (*free_bytes)(void *context);
    void (*book_added)(void *context);
    void (*progress)(void *context, const cz_status_t *status);
    /// 可选，仅 UI 元数据，不把云端任务 ID 或 URL 传出解析器。/ Optional UI metadata only, with no task IDs or URLs.
    void (*row)(void *context, unsigned index, const cz_row_t *file, bool active);
    void (*stage)(void *context, cz_row_state_t state, size_t received);
    void (*record)(void *context, const cz_row_t *file);
} cz_ops_t;

/// 预览填充队列，下载只匹配该队列里勾选的项目；不缓存网络响应。/ Preview fills the queue; download matches only its checked items. No response is retained.
cz_status_t cloudzao_run(const cz_ops_t *ops, cz_job_t job, cz_queue_t *queue, const char *key, const char *mac,
                         const char *version, char *json, unsigned char *chunk);
/// 检查 ASCII 请求头值，防止截断或注入。/ Validate an ASCII credential without truncation or header injection.
bool cloudzao_key_valid(const char *key);
/// 校验可显示、可保存的单个 UTF-8 文件名。/ Validate a single UTF-8 filename for display and storage.
bool cloudzao_name_valid(const char *name);

typedef struct {
    const char *host, *path;
    size_t host_len;
} cz_download_target_t;

/// 判断下载主机是否在 OSS 信任范围内；调用方仍需校验目标。/ Identify OSS trust scope; callers must still validate the target.
bool cloudzao_oss_host(const char *host, size_t length);

/// 解析仅以 TLS 请求的目标；路径和查询不改写。/ Parse a TLS-only target without rewriting its path or query.
bool cloudzao_download_target(const char *url, cz_download_target_t *target);
