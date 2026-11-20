/*
 * 中文：会话内的 HTTP 报文，不持久化凭据或 URL。
 * English: Session-local HTTP framing; no persistence of credentials or URLs.
 */
#pragma once
#include "cloudzao_receive.h"
#include "http_parser.h"

typedef struct {
    http_parser parser;
    cz_sink_t sink;
    void *context;
    size_t received, limit;
    cz_error_t error;
    bool done, download;
} cz_response_t;

/// 构造独立请求，返回字节数；失败返回零。/ Compose one isolated request, returning its byte length or zero.
size_t cloudzao_wire_request(cz_request_t operation, const cz_http_request_t *req, char *host, size_t host_cap,
                             char *output, size_t output_cap);
void cloudzao_wire_begin(cz_response_t *response, bool download, size_t limit, cz_sink_t sink, void *context);
/// 长度为零表示 EOF；解析或业务 HTTP 状态失败时不再调用 sink。/ Zero length is EOF; parser and HTTP status failures stop the sink.
bool cloudzao_wire_feed(cz_response_t *response, const void *data, size_t length);
