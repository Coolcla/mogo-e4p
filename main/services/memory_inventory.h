/*
 * 中文：在 UI 安全边界合并分配账本与模块实际持有资源。
 * English: Merge the allocation ledger and live module-owned resources at a UI safety boundary.
 */
#pragma once
#include "memory_manager.h"
#include "../display/display_types.h"

/// 只在 UI 任务调用；不申请内存、不停止任务，忙碌阅读资源留在未追踪项并标记 partial。
/// UI-task only; no allocations or worker stops. Busy reader resources remain untracked and mark the inventory partial.
void memory_inventory_snapshot(const display_state_t *display, mm_snapshot_t *out);
