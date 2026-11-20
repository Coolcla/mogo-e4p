/*
 * 中文：真实协议和文件流程，模拟 HTTPS 响应，不使用账号或云端。
 * English: Exercise real framing and file workflows with synthetic HTTPS responses and no account or cloud.
 */
#include "cloudzao_receive.h"
#include "cloudzao_history.h"
#include <sys/stat.h>
#include "cloudzao_wire.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAC "aa:bb:cc:dd:ee:ff"
#ifdef MOGO_CLOUDZAO_LOCAL_RELAY
#include "cloudzao_local_relay.h"
#define KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define BIND_PATH "/api/v1/cloudzao/binding"
#define LIST_PATH "/api/v1/cloudzao/tasks/next"
#define ACK_PATH "/api/v1/cloudzao/tasks"
#else
#define KEY "host-only-placeholder"
#define BIND_PATH "/xiaozhi/api/firmware-resources/ota"
#define LIST_PATH "/xiaozhi/api/firmware-resources/push-resources/next"
#define ACK_PATH "/xiaozhi/api/firmware-resources/push-resources"
#endif
#define TARGET "/objects/a%2Fb?sig=x%2B1&expires=12"
#define URL "https://cdn.example.invalid" TARGET
#define OSS_HOST "bucket.oss-cn-hangzhou.aliyuncs.com"
#define OSS_URL "http://" OSS_HOST TARGET
#define DIGEST "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define ITEM_URL(kind,format_,mime,name,id,sha,url) \
    "{\"resourceType\":\"" kind "\",\"format\":\"" format_ "\",\"contentType\":\"" mime \
    "\",\"name\":\"" name "\",\"taskId\":\"" id "\",\"fileSize\":\"3\",\"sha256\":\"" sha "\",\"downloadUrl\":\"" url "\"}"
#define ITEM(kind,format_,mime,name,id,sha) ITEM_URL(kind,format_,mime,name,id,sha,URL)
#define TXT(name,id,sha) ITEM("BOOK", "TXT", "text/plain", name, id, sha)
#define LIST(items) "{\"code\":0,\"data\":[" items "]}"

typedef struct {
    const char *root, *json, *probe_json, *expected_ack;
    unsigned downloads, confirmations, books, lists, bindings;
    cz_error_t interrupt, ack_result, interrupt_during_download;
    uint64_t space;
    bool oversized, cancel_after_bind;
    unsigned queued, finished, stage_mask;
    size_t written;
    uint32_t history_next;
} test_t;

static bool present(const char *root, const char *file) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, file);
    return access(path, F_OK) == 0;
}
static cz_error_t stopped(void *context) { return ((test_t *)context)->interrupt; }
static uint64_t available(void *context) { return ((test_t *)context)->space; }
static void added(void *context) { ++((test_t *)context)->books; }
static void progress(void *context, const cz_status_t *status) {
    (void)context;
    assert(status->busy && status->processed <= status->total && status->pending <= status->saved);
}

static void observe_row(void *context, unsigned index, const cz_row_t *row, bool active) {
    test_t *test = context;
    assert(!row->name[0] || cloudzao_name_valid(row->name));
    if (row->state == CZ_ROW_WAIT) { assert(!active && index == test->queued); ++test->queued; }
    else if (active) { assert(row->state == CZ_ROW_CHECK); test->written = 0; }
    else { assert(index == test->finished); ++test->finished; }
}

static void observe_stage(void *context, cz_row_state_t state, size_t bytes) {
    test_t *test = context;
    test->stage_mask |= 1u << state;
    if (state == CZ_ROW_DOWNLOAD) { assert(bytes >= test->written && bytes <= 3); test->written = bytes; }
}

static void observe_result(void *context, const cz_row_t *row) {
    test_t *test = context;
    const cz_ops_t ops = {.context = test, .root = test->root, .stop = stopped};
    assert(cloudzao_history_record(&ops, &test->history_next, row, 1700000000));
}

static cz_error_t stop_history_clear_after_lookup(void *context) {
    unsigned *checks = context;
    return ++*checks == 1 ? CZ_OK : CZ_MEDIA;
}

static void history_regression(test_t *test) {
    const cz_ops_t ops = {.context = test, .root = test->root, .stop = stopped};
    cz_history_item_t items[CLOUDZAO_HISTORY_ROWS];
    unsigned count = 0;
    assert(!cloudzao_history_read(&ops, items, &count) && count > 0);
    char path[512]; snprintf(path, sizeof(path), "%s/.readpico/cloudzao/history.v1", test->root);
    cz_row_t row = {.size = 3, .state = CZ_ROW_SAVED};
    uint32_t next = 0;
    for (unsigned i = 0; i < 40; ++i) {
        snprintf(row.name, sizeof(row.name), "file-%02u.txt", i);
        assert(cloudzao_history_record(&ops, &next, &row, 1700000000 + i));
    }
    assert(!cloudzao_history_read(&ops, items, &count) && count == CLOUDZAO_HISTORY_ROWS);
    assert(!strcmp(items[0].file.name, "file-39.txt") && items[0].utc == 1700000039);
    assert(!strcmp(items[CLOUDZAO_HISTORY_ROWS-1].file.name, "file-08.txt"));
    struct stat file;
    assert(!stat(path, &file) && file.st_size == 144 * CLOUDZAO_HISTORY_ROWS);
    FILE *f = fopen(path, "r+b"); assert(f);
    assert(!fseek(f, (long)((next-1) % CLOUDZAO_HISTORY_ROWS) * 144, SEEK_SET));
    assert(fputc(0, f) == 0 && !fclose(f));
    assert(!cloudzao_history_read(&ops, items, &count) && count == CLOUDZAO_HISTORY_ROWS - 1);
    assert(!strcmp(items[0].file.name, "file-38.txt"));
    test->interrupt = CZ_CANCELLED;
    assert(!cloudzao_history_record(&ops, &next, &row, 0));
    assert(cloudzao_history_read(&ops, items, &count) == CZ_CANCELLED && !count);
    assert(cloudzao_history_clear(&ops) == CZ_CANCELLED && !stat(path, &file));
    test->interrupt = CZ_OK;
    char sibling[512]; snprintf(sibling, sizeof(sibling), "%s/.readpico/cloudzao/keep.txt", test->root);
    f = fopen(sibling, "wb"); assert(f && fputc('a', f) == 'a' && !fclose(f));
    unsigned checks = 0;
    const cz_ops_t removed = {.root = test->root, .context = &checks, .stop = stop_history_clear_after_lookup};
    assert(cloudzao_history_clear(&removed) == CZ_MEDIA && checks == 2 && !stat(path, &file));
    assert(present(test->root, "books/中文.txt") && present(test->root, ".readpico/cloudzao/keep.txt"));
    assert(cloudzao_history_clear(&ops) == CZ_OK);
    assert(stat(path, &file) && errno == ENOENT);
    assert(cloudzao_history_clear(&ops) == CZ_OK);
    assert(cloudzao_history_read(&ops, items, &count) == CZ_OK && count == 0);
    assert(present(test->root, "books/中文.txt") && present(test->root, ".readpico/cloudzao/keep.txt"));
    assert(!mkdir(path, 0700));
    assert(cloudzao_history_clear(&ops) == CZ_STORAGE);
    assert(!rmdir(path));
    next = 0;
    assert(cloudzao_history_record(&ops, &next, &row, 1700000040));
    assert(cloudzao_history_read(&ops, items, &count) == CZ_OK && count == 1);
    assert(!strcmp(items[0].file.name, row.name) && items[0].utc == 1700000040);
    puts("CloudZao history clear is local, cancellable and idempotent: PASS");
}

static cz_error_t request(void *context, cz_request_t operation, const cz_http_request_t *req,
                          cz_sink_t sink, void *sink_context) {
    test_t *test = context;
    char wire[CLOUDZAO_CHUNK], host[254];
    assert(cloudzao_wire_request(operation, req, host, sizeof(host), wire, sizeof(wire)));
    if (operation != CZ_DOWNLOAD) {
#ifdef MOGO_CLOUDZAO_LOCAL_RELAY
        assert(!strcmp(host, CZ_RELAY_ADDRESS) && strstr(wire, "Host: " CZ_RELAY_AUTHORITY "\r\n"));
        assert(strstr(wire, "Authorization: Bearer " KEY "\r\n"));
        assert(!strstr(wire, "X-Resource-Key") && !strstr(wire, "Device-Id") && !strstr(wire, MAC));
#else
        assert(!strcmp(host, "admin.metalio.world"));
        assert(strstr(wire, "X-Resource-Key: " KEY) && strstr(wire, "Device-Id: " MAC));
#endif
        assert(!req->url);
    }
    if (operation == CZ_DOWNLOAD) {
        assert(!req->key && !req->mac && !req->body);
        if (!strcmp(req->url, OSS_URL)) assert(!strcmp(host, OSS_HOST));
        else assert(!strcmp(req->url, URL) && !strcmp(host, "cdn.example.invalid"));
        assert(!strstr(wire, KEY) && !strstr(wire, MAC) && !strstr(wire, "Authorization") && !strstr(wire, "Device-Id") && !strstr(wire, "X-Resource-Key"));
        assert(strstr(wire, "GET /objects/a%2Fb?sig=x%2B1&expires=12 HTTP/1.1"));
        assert(req->limit == 3);
        ++test->downloads;
        assert(sink(sink_context, "a", 1));
        test->interrupt = test->interrupt_during_download;
        bool accepted = sink(sink_context, "bc", 2);
        return accepted ? CZ_OK : CZ_INVALID;
    }
    if (operation == CZ_ACK) {
        ++test->confirmations;
        assert(strstr(wire, "DELETE " ACK_PATH " HTTP/1.1"));
        assert(req->body && strstr(req->body, "\"taskId\":\""));
        if (test->expected_ack) assert(strstr(req->body, test->expected_ack));
        if (strstr(req->body, "book-1\"")) assert(present(test->root, "books/中文.txt"));
        const char *ok = "{\"code\":0,\"data\":{\"removedTasks\":0}}";
        if (test->ack_result != CZ_OK) return test->ack_result;
        return sink(sink_context, ok, strlen(ok)) ? CZ_OK : CZ_INVALID;
    }
    if (operation == CZ_BIND) {
        ++test->bindings;
        assert(strstr(wire, "POST " BIND_PATH " HTTP/1.1"));
        assert(req->body && strstr(req->body, "\"application\":{\"version\":\"1.1.6\"}"));
    } else {
        ++test->lists;
        assert(strstr(wire, "POST " LIST_PATH " HTTP/1.1"));
        assert(!req->body);
    }
    if (test->oversized) {
        static char big[CLOUDZAO_JSON_MAX + 1];
        return sink(sink_context, big, sizeof(big)) ? CZ_OK : CZ_INVALID;
    }
    const char *body = operation == CZ_LIST && test->probe_json ? test->probe_json : test->json;
    const bool accepted = sink(sink_context, body, strlen(body));
    if (operation == CZ_BIND && test->cancel_after_bind) test->interrupt = CZ_CANCELLED;
    return accepted ? CZ_OK : CZ_INVALID;
}

static cz_queue_t test_queue;
static cz_status_t run_job(test_t *test, cz_job_t job, const char *json) {
    test->json = json;
    test->queued = test->finished = test->stage_mask = 0;
    static char buffer[CLOUDZAO_JSON_MAX + 1];
    static unsigned char chunk[CLOUDZAO_CHUNK];
    cz_ops_t ops = {.context = test, .root = test->root, .stop = stopped, .request = request,
                    .free_bytes = available, .book_added = added, .progress = progress,
                    .row = observe_row, .stage = observe_stage, .record = observe_result};
    return cloudzao_run(&ops, job, &test_queue, KEY, MAC, "1.1.6", buffer, chunk);
}

static cz_status_t run(test_t *test, bool bind, const char *json) {
    if (bind) return run_job(test, CZ_JOB_BIND, json);
    cz_status_t preview = run_job(test, CZ_JOB_PREVIEW, json);
    if (preview.error) return preview;
    bool any = false;
    for (unsigned i = 0; i < test_queue.count; ++i) {
        test_queue.selection.checked[i] = test_queue.offers[i].kind != CZ_KIND_NONE;
        any |= test_queue.selection.checked[i];
    }
    return any ? run_job(test, CZ_JOB_DOWNLOAD, json) : preview;
}

static bool count(void *context, const void *data, size_t length) {
    (void)data;
    *(size_t *)context += length;
    return true;
}
static void framing(void) {
    assert(cloudzao_oss_host(OSS_HOST, sizeof(OSS_HOST) - 1));
    assert(cloudzao_oss_host("bucket.ALIYUNCS.COM", sizeof("bucket.ALIYUNCS.COM") - 1));
    assert(!cloudzao_oss_host("fakealiyuncs.com", sizeof("fakealiyuncs.com") - 1));
    assert(!cloudzao_oss_host("bucket.aliyuncs.com.example.invalid", sizeof("bucket.aliyuncs.com.example.invalid") - 1));
    const char *content = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    const char *chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n2\r\nbc\r\n0\r\n\r\n";
    const char *redirect = "HTTP/1.1 302 Found\r\nLocation: https://other.invalid/\r\nContent-Length: 3\r\n\r\nabc";
    const char *unauthorized = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n";
    cz_response_t response;
    size_t received = 0;
    cloudzao_wire_begin(&response, true, 3, count, &received);
    for (size_t i = 0; i < strlen(content); ++i) assert(cloudzao_wire_feed(&response, content+i, 1));
    assert(response.done && received == 3);
    received = 0;
    cloudzao_wire_begin(&response, true, 3, count, &received);
    assert(cloudzao_wire_feed(&response, chunked, strlen(chunked)) && response.done && received == 3);
    received = 0;
    cloudzao_wire_begin(&response, true, 2, count, &received);
    assert(!cloudzao_wire_feed(&response, content, strlen(content)) && !received && response.error == CZ_INVALID);
    cloudzao_wire_begin(&response, true, 2, count, &received);
    assert(!cloudzao_wire_feed(&response, chunked, strlen(chunked)) && received == 1 && response.error == CZ_INVALID);
    received = 0;
    cloudzao_wire_begin(&response, true, 3, count, &received);
    assert(!cloudzao_wire_feed(&response, redirect, strlen(redirect)) && !received && response.error == CZ_SERVER);
    cloudzao_wire_begin(&response, false, 3, count, &received);
    assert(!cloudzao_wire_feed(&response, unauthorized, strlen(unauthorized)) && response.error == CZ_AUTH);
    cloudzao_wire_begin(&response, true, 3, count, &received);
    assert(cloudzao_wire_feed(&response, content, strlen(content) - 1) && !response.done);
    assert(!cloudzao_wire_feed(&response, "", 0));
    char host[254], wire[CLOUDZAO_CHUNK];
    cz_http_request_t req = {.url = "https://admin.metalio.world@other.invalid/x"};
    assert(!cloudzao_wire_request(CZ_DOWNLOAD, &req, host, sizeof(host), wire, sizeof(wire)));
    req.url = OSS_URL; req.key = KEY;
    assert(!cloudzao_wire_request(CZ_DOWNLOAD, &req, host, sizeof(host), wire, sizeof(wire)));
#ifdef MOGO_CLOUDZAO_LOCAL_RELAY
    const cz_http_request_t invalid_token = {.key = "not-a-device-bearer", .mac = MAC};
    assert(!cloudzao_wire_request(CZ_LIST, &invalid_token, host, sizeof(host), wire, sizeof(wire)));
#endif
    req.key = NULL;
    const char *rejected[] = {
        "http://cdn.example.invalid" TARGET, "http://bucket.aliyuncs.com.example.invalid" TARGET,
        "http://bucket.aliyuncs.com@other.aliyuncs.com" TARGET, "http://bucket.aliyuncs.com:80" TARGET,
        OSS_URL "#fragment", "https://host.example.invalid:80" TARGET,
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        req.url = rejected[i];
        assert(!cloudzao_wire_request(CZ_DOWNLOAD, &req, host, sizeof(host), wire, sizeof(wire)));
    }
}

static void binding_regression(const char *root) {
    test_t test = {.root = root, .space = 1ULL << 30, .probe_json = LIST("")};
    cz_status_t status = run(&test, true, "{\"activation\":{\"code\":\"012345\"},\"firmware\":{\"url\":\"ignored\"}}");
    assert(!status.error && !status.binding_probed && !strcmp(status.code, "012345") && !test.lists);
    assert(run(&test, true, "{\"error\":\"upstream\",\"activation\":{\"code\":\"012345\"}}").error == CZ_SERVER);
    assert(run(&test, true, "{\"activation\":{\"code\":12345}}").error == CZ_INVALID && !test.lists);

    status = run(&test, true, "{\"firmware\":{}}");
    assert(!status.error && status.binding_probed && !status.code[0] && test.lists == 1);
    test.probe_json = LIST(TXT("probe-only.txt", "book-probe", DIGEST));
    status = run(&test, true, "{\"activation\":null}");
    assert(!status.error && status.binding_probed && !status.total && test.lists == 2);
    assert(!test.queued && !test.stage_mask && !test.downloads && !test.confirmations && !test.books);
    assert(!present(root, "books/probe-only.txt") && !present(root, ".readpico"));

    test.probe_json = "{\"code\":500,\"data\":[]}";
    status = run(&test, true, "{}");
    assert(status.binding_probed && status.error == CZ_SERVER && test.lists == 3);
    test.probe_json = "{\"code\":401,\"data\":[]}";
    status = run(&test, true, "{}");
    assert(status.binding_probed && status.error == CZ_AUTH && test.lists == 4);
    test.probe_json = "{\"code\":0,\"data\":null}";
    status = run(&test, true, "{}");
    assert(status.binding_probed && status.error == CZ_INVALID && test.lists == 5);
    test.cancel_after_bind = true;
    status = run(&test, true, "{}");
    assert(status.error == CZ_CANCELLED && test.lists == 5);
}

static void selection_regression(const char *root) {
#define SELECT_BOOK TXT("choice.txt", "choice-book", DIGEST)
#define SELECT_IMAGE ITEM("BADGE", "", "image/png", "choice.png", "choice-image", DIGEST)
#define SELECT_FONT ITEM("FONT", "", "font/ttf", "choice.ttf", "choice-font", DIGEST)
#define UNSUPPORTED_ITEM ITEM("FONT", "", "application/octet-stream", "choice.ef", "choice-unsupported", DIGEST)
    const char *queue = LIST(SELECT_BOOK "," SELECT_IMAGE "," UNSUPPORTED_ITEM "," SELECT_FONT);
    test_t test = {.root = root, .space = 1ULL << 30, .expected_ack = "choice-image\""};
    cz_status_t status = run_job(&test, CZ_JOB_PREVIEW, queue);
    assert(!status.error && status.total == 4 && test_queue.count == 4 && test.lists == 1);
    assert(test_queue.offers[0].kind == CZ_KIND_TXT && test_queue.offers[1].kind == CZ_KIND_PNG &&
           test_queue.offers[2].kind == CZ_KIND_NONE && test_queue.offers[3].kind == CZ_KIND_TTF);
    assert(!test.downloads && !test.confirmations && !test.queued && !test.finished && !test.books);
    assert(!present(root, "books") && !present(root, "pictures") && !present(root, "fonts") && !present(root, ".readpico"));
    test_queue.selection = (cz_selection_t){0};
    test_queue.selection.checked[2] = true;
    assert(run_job(&test, CZ_JOB_DOWNLOAD, queue).error == CZ_INVALID && test.lists == 1);
    test_queue.selection.checked[1] = test_queue.selection.checked[3] = true;
    // 云端重排、换签、删除与新推送均不能扩大用户选择。
    // Reordering, URL refresh, removal and newly pushed files never expand selection.
    status = run_job(&test, CZ_JOB_DOWNLOAD, LIST(SELECT_BOOK "," TXT("new.txt", "new-book", DIGEST) ","
        ITEM_URL("BADGE", "", "image/png", "choice.png", "choice-image", DIGEST, OSS_URL) "," SELECT_IMAGE));
    assert(!status.error && status.total == 2 && status.saved == 1 && !status.pending && status.unavailable == 1);
    assert(test.lists == 2 && test.downloads == 1 && test.confirmations == 1 && test.finished == 2);
    assert(present(root, "pictures/choice.png") && !present(root, "books") && !present(root, "fonts"));
    assert(!run_job(&test, CZ_JOB_PREVIEW, queue).error);
    test_queue.selection = (cz_selection_t){.checked = {true}};
    status = run_job(&test, CZ_JOB_DOWNLOAD, LIST(TXT("choice.txt", "choice-book",
        "aa7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ","
        TXT("choice.txt", "different-task", DIGEST)));
    assert(!status.error && status.unavailable == 1 && !status.saved && !present(root, "books"));
    assert(test.downloads == 1 && test.confirmations == 1);
    test.interrupt = CZ_CANCELLED;
    assert(run_job(&test, CZ_JOB_DOWNLOAD, queue).error == CZ_CANCELLED && test.downloads == 1 && test.confirmations == 1);
#undef SELECT_BOOK
#undef SELECT_IMAGE
#undef SELECT_FONT
#undef UNSUPPORTED_ITEM
}

int main(int argc, char **argv) {
    assert(argc == 2);
    framing();
    test_t test = {.root = argv[1], .space = 1ULL << 30};
    binding_regression(argv[1]);
    char selection_root[512];
    assert(snprintf(selection_root, sizeof(selection_root), "%s/selection", argv[1]) < (int)sizeof(selection_root));
    assert(!mkdir(selection_root, 0775));
    selection_regression(selection_root);
    cz_status_t status;
    assert(!run(&test, false, LIST("")).total && test.lists == 1);
    assert(run(&test, false, "{\"code\":401,\"data\":[]}").error == CZ_AUTH);
    assert(run(&test, false, "{\"code\":500,\"data\":[]}").error == CZ_SERVER);
    assert(run(&test, false, "{\"code\":0,\"data\":[]}junk").error == CZ_INVALID);
    assert(run(&test, false, "{\"code\":0,\"data\":[[[[[[[[[[[[[[[[[[0]]]]]]]]]]]]]]]]]]}").error == CZ_INVALID);
    test.oversized = true;
    assert(run(&test, false, "").error == CZ_INVALID);
    test.oversized = false;
    status = run(&test, false, LIST(TXT("../escape.txt", "book-0", DIGEST) "," TXT("a\\u0000.txt", "book-0", DIGEST)));
    assert(status.error == CZ_INVALID && !test.downloads && !test.confirmations);
    status = run(&test, false, LIST(TXT("../escape.txt", "book-0", DIGEST) "," TXT(".hidden.txt", "book-0", DIGEST)
        "," ITEM("FONT", "", "application/octet-stream", "unsupported.ef", "font-1", DIGEST)));
    assert(!status.error && status.total == 3 && !test.downloads && !test.confirmations);
    for (unsigned i = 0; i < 3; ++i) assert(test_queue.offers[i].kind == CZ_KIND_NONE);
    assert(!test_queue.offers[0].file.name[0] && !test_queue.offers[1].file.name[0]);
    status = run(&test, false, LIST(TXT("bad.txt", "book-0", "aa7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
    assert(!status.error && status.failed == 1 && !test.confirmations && !present(test.root, "books/bad.txt"));
    test.downloads = 0;
    test.ack_result = CZ_TIMEOUT;
    status = run(&test, false, LIST(TXT("中文.txt", "book-1", DIGEST)));
    assert(!status.error && status.saved == 1 && status.pending == 1 && status.item_request == CZ_ACK && test.confirmations == 1 && test.downloads == 1 && test.books == 1);
    assert(test.queued == 1 && test.finished == 1 && test.written == 3);
    assert(test.stage_mask == ((1u << CZ_ROW_DOWNLOAD) | (1u << CZ_ROW_VERIFY) | (1u << CZ_ROW_CONFIRM)));
    test.ack_result = CZ_OK;
    assert(!run(&test, false, LIST("")).total && test.confirmations == 1 && test.downloads == 1);
    status = run(&test, false, LIST(TXT("中文.txt", "book-1", DIGEST)));
    assert(!status.error && status.saved == 1 && !status.pending && test.confirmations == 2 && test.downloads == 1 && test.books == 1);
    status = run(&test, false, LIST(TXT("中文.txt", "book-2", "aa7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
    assert(status.item_error == CZ_CONFLICT && test.confirmations == 2 && test.downloads == 1);
    status = run(&test, false, LIST(ITEM("BOOK", "EPUB", "application/epub+zip", "book.epub", "book-3", DIGEST) ","
        ITEM("BADGE", "", "image/png", "wall.png", "badge-3", DIGEST) "," ITEM("FONT", "", "font/ttf", "font.ttf", "font-3", DIGEST)));
    assert(!status.error && status.saved == 3 && !status.pending && test.books == 2);
    assert(present(test.root, "books/book.epub") && present(test.root, "pictures/wall.png") && present(test.root, "fonts/font.ttf"));
    status = run(&test, false, LIST(ITEM_URL("BOOK", "TXT", "text/plain", "oss.txt", "book-4", DIGEST, OSS_URL)));
    assert(!status.error && status.saved == 1 && !status.pending && present(test.root, "books/oss.txt"));
    unsigned confirmations = test.confirmations, downloads = test.downloads;
    status = run(&test, false, LIST(ITEM_URL("BOOK", "TXT", "text/plain", "insecure.txt", "book-5", DIGEST,
        "http://cdn.example.invalid" TARGET)));
    assert(!status.error && !status.saved && test_queue.offers[0].kind == CZ_KIND_NONE);
    assert(test.confirmations == confirmations && test.downloads == downloads && !present(test.root, "books/insecure.txt"));
    test.interrupt_during_download = CZ_CANCELLED;
    status = run(&test, false, LIST(TXT("cancel.txt", "book-5", DIGEST)));
    assert(status.error == CZ_CANCELLED && test.confirmations == confirmations && !present(test.root, "books/cancel.txt"));
    assert(!present(test.root, ".readpico/cloudzao/incoming.part"));
    test.interrupt = CZ_OK;
    test.interrupt_during_download = CZ_MEDIA;
    status = run(&test, false, LIST(TXT("pull.txt", "book-5", DIGEST)));
    assert(status.error == CZ_MEDIA && test.confirmations == confirmations && !present(test.root, "books/pull.txt"));
    assert(present(test.root, ".readpico/cloudzao/incoming.part"));
    test.interrupt = test.interrupt_during_download = CZ_OK;
    assert(!run(&test, false, LIST("")).error && present(test.root, ".readpico/cloudzao/incoming.part"));
    test.space = 0;
    status = run(&test, false, LIST(TXT("full.txt", "book-5", DIGEST)));
    assert(status.item_error == CZ_STORAGE && test.confirmations == confirmations);
    assert(!present(test.root, ".readpico/cloudzao/incoming.part"));
    char many[1024] = "{\"code\":0,\"data\":[";
    for (unsigned i = 0; i < CLOUDZAO_LIST_ROWS + 3; ++i) strcat(many, i ? ",{}" : "{}");
    strcat(many, "]}");
    status = run(&test, false, many);
    assert(!status.error && status.total == CLOUDZAO_LIST_ROWS + 3 && test_queue.count == CLOUDZAO_LIST_ROWS);
    assert(!test.queued && !test.finished);
    history_regression(&test);
    puts("CloudZao framing, storage, acknowledgement and cancellation passed");
    return 0;
}
