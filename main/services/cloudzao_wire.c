/*
 * 中文：CloudZao 使用一次请求、一次响应的 HTTPS 连接；无跳转、认证重试或跨主机复用。
 * English: One request/response per HTTPS connection; no redirects, auth retry, or cross-host reuse.
 */
#include "cloudzao_wire.h"
#include <stdio.h>
#include <string.h>

#ifdef MOGO_CLOUDZAO_LOCAL_RELAY
#include "cloudzao_local_relay.h"
#define CZ_HOST CZ_RELAY_ADDRESS
#define CZ_AUTHORITY CZ_RELAY_AUTHORITY
#define CZ_ROOT "/api/v1/cloudzao"
#define CZ_BIND_PATH CZ_ROOT "/binding"
#define CZ_LIST_PATH CZ_ROOT "/tasks/next"
#define CZ_ACK_PATH CZ_ROOT "/tasks"
#else
#define CZ_HOST "admin.metalio.world"
#define CZ_AUTHORITY CZ_HOST
#define CZ_ROOT "/xiaozhi/api/firmware-resources"
#define CZ_BIND_PATH CZ_ROOT "/ota"
#define CZ_LIST_PATH CZ_ROOT "/push-resources/next"
#define CZ_ACK_PATH CZ_ROOT "/push-resources"
#endif

size_t cloudzao_wire_request(cz_request_t operation, const cz_http_request_t *req, char *host, size_t host_cap,
                             char *output, size_t output_cap) {
    bool download = operation == CZ_DOWNLOAD;
    const char *path = operation == CZ_BIND ? CZ_BIND_PATH : operation == CZ_ACK ? CZ_ACK_PATH : CZ_LIST_PATH;
    if (download) {
        if (!req->url || req->key || req->mac || req->body) return 0;
        cz_download_target_t target;
        if (!cloudzao_download_target(req->url, &target) || target.host_len >= host_cap) return 0;
        memcpy(host, target.host, target.host_len);
        host[target.host_len] = 0;
        path = target.path;
    } else {
        if (host_cap < sizeof(CZ_HOST) || req->url || !cloudzao_key_valid(req->key) || !req->mac ||
            strlen(req->mac) != 17 || strspn(req->mac, "0123456789abcdefABCDEF:") != 17 ||
            ((operation == CZ_BIND || operation == CZ_ACK) != (req->body != NULL))) return 0;
#ifdef MOGO_CLOUDZAO_LOCAL_RELAY
        if (strlen(req->key) != 64 || strspn(req->key, "0123456789abcdef") != 64) return 0;
#endif
        memcpy(host, CZ_HOST, sizeof(CZ_HOST));
    }
    int count = snprintf(output, output_cap, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n",
                         download ? "GET" : operation == CZ_ACK ? "DELETE" : "POST", path, download ? host : CZ_AUTHORITY);
    if (count < 0 || (size_t)count >= output_cap) return 0;
    size_t used = (size_t)count;
    if (!download) {
#ifdef MOGO_CLOUDZAO_LOCAL_RELAY
        count = snprintf(output + used, output_cap - used,
                         "Authorization: Bearer %s\r\nAccept: application/json\r\nContent-Length: %zu\r\n",
                         req->key, req->body ? strlen(req->body) : 0);
#else
        count = snprintf(output + used, output_cap - used,
                         "X-Resource-Key: %s\r\nDevice-Id: %s\r\nAccept: application/json\r\nContent-Length: %zu\r\n",
                         req->key, req->mac, req->body ? strlen(req->body) : 0);
#endif
        if (count < 0 || (size_t)count >= output_cap - used) return 0;
        used += (size_t)count;
    }
    count = req->body ? snprintf(output + used, output_cap - used, "Content-Type: application/json\r\n\r\n%s", req->body) :
        snprintf(output + used, output_cap - used, "\r\n");
    return count >= 0 && (size_t)count < output_cap - used ? used + (size_t)count : 0;
}

static int headers(http_parser *parser) {
    cz_response_t *response = parser->data;
    if (parser->status_code != 200)
        response->error = !response->download && parser->status_code == 401 ? CZ_AUTH : CZ_SERVER;
    else if (parser->content_length != UINT64_MAX && parser->content_length > response->limit)
        response->error = CZ_INVALID;
    return response->error == CZ_OK ? 0 : -1;
}

static int body(http_parser *parser, const char *data, size_t length) {
    cz_response_t *response = parser->data;
    if (length > response->limit - response->received || !response->sink(response->context, data, length)) {
        response->error = CZ_INVALID;
        return -1;
    }
    response->received += length;
    return 0;
}

static int complete(http_parser *parser) {
    cz_response_t *response = parser->data;
    response->done = true;
    http_parser_pause(parser, 1);
    return 0;
}

void cloudzao_wire_begin(cz_response_t *response, bool download, size_t limit, cz_sink_t sink, void *context) {
    *response = (cz_response_t){.context = context, .download = download, .limit = limit, .sink = sink};
    http_parser_init(&response->parser, HTTP_RESPONSE);
    response->parser.data = response;
}

bool cloudzao_wire_feed(cz_response_t *response, const void *data, size_t length) {
    static const http_parser_settings callbacks = {
        .on_headers_complete = headers, .on_body = body, .on_message_complete = complete,
    };
    size_t used = http_parser_execute(&response->parser, &callbacks, data, length);
    enum http_errno error = HTTP_PARSER_ERRNO(&response->parser);
    if ((error == HPE_OK || (error == HPE_PAUSED && response->done)) && used == length &&
        !response->parser.upgrade && (length || response->done)) return true;
    if (response->error == CZ_OK) response->error = CZ_INVALID;
    return false;
}
