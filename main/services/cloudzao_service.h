/*
 * 中文：CloudZao 本机联调会话与卸载前的文件交还。
 * English: Local CloudZao test sessions and file handoff before unmount.
 */
#pragma once
#include <stdbool.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifdef CONFIG_MOGO_CLOUDZAO_TRANSFER
#include "cloudzao_receive.h"
/// 启动单次操作；下载必须提供勾选，参数只在启动时复制。/ Start one job; download requires a selection, copied at start.
cz_error_t cloudzao_start(cz_job_t job, const char *key, const cz_selection_t *selection);
/// 只暴露预览元数据，不暴露用于匹配的指纹。/ Expose preview metadata only, not matching fingerprints.
bool cloudzao_offer(unsigned index, cz_row_t *out, cz_kind_t *kind);
void cloudzao_discard_queue(void);
cz_status_t cloudzao_status(void);
cz_transfer_t cloudzao_transfer_status(void);
bool cloudzao_transfer_row(unsigned index, cz_row_t *out);
/// 离线读取当前 TF 卡的历史，由同一工作门控管理。/ Read current-card history offline under the same worker gate.
cz_error_t cloudzao_start_history(void);
/// 离线清除当前 TF 卡的历史，沿用工作门控和卡交还。/ Clear current-card history offline under the worker and media-handoff gates.
cz_error_t cloudzao_start_history_clear(void);
cz_history_status_t cloudzao_history_status(void);
bool cloudzao_history_item(unsigned index, cz_history_item_t *out);
void cloudzao_cancel(void);
void cloudzao_media_lost(void);
/// 仅主循环消费一次，合并同批书架失效通知。/ Main loop consumes coalesced bookshelf invalidation once.
bool cloudzao_take_books_changed(void);
/// 请求取消并等待句柄及网络归还，不强制终止工作任务。/ Cancel and wait for handles and lease without killing the worker.
bool cloudzao_quiesce(unsigned timeout_ms);
#else
static inline bool cloudzao_quiesce(unsigned timeout_ms) { (void)timeout_ms; return true; }
#endif
