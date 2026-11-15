/*
 * 中文：显示服务持有的堆缓冲盘点，不向页面暴露内部结构。
 * English: Inventory of display-owned heap buffers without exposing internal state to pages.
 */
#pragma once
#include "display_types.h"

/// UI 安全边界同步访问仍持有的分配基址；回调不得分配、阻塞或修改显示状态。
/// Visit live allocation bases at a UI safety boundary; the callback must not allocate, block or mutate display state.
void display_visit_allocations(const display_state_t *state,
    void (*visit)(void *context, const void *allocation), void *context);
