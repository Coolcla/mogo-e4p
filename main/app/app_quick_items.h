/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：快捷栏项目与排列契约，不访问设置或硬件。
 * English: Quick-bar item and ordering contract, independent of storage and hardware.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// 编号写入设置和备份，已有编号不得重排。/ Persisted IDs; never reorder existing values.
typedef enum {
    APP_QUICK_WIFI, APP_QUICK_BLUETOOTH, APP_QUICK_FULL, APP_QUICK_FRONTLIGHT,
    APP_QUICK_CELLULAR, APP_QUICK_ROTATION, APP_QUICK_HAPTICS, APP_QUICK_MEMORY,
    APP_QUICK_LOCK, APP_QUICK_ITEM_COUNT,
} app_quick_item_t;

#define APP_QUICK_MAX_VISIBLE 8
#define APP_QUICK_LAYOUT_DEFAULT {8, {0, 1, 2, 3, 5, 6, 7, 8, 4}}

typedef struct {
    uint8_t count; ///< 前 count 项显示，其余隐藏。/ Show the first count items; hide the rest.
    uint8_t order[APP_QUICK_ITEM_COUNT]; ///< 全部项目的唯一排列。/ A permutation of every item.
} app_quick_layout_t;

static inline bool app_quick_layout_valid(const app_quick_layout_t *layout) {
    if (!layout || layout->count > APP_QUICK_MAX_VISIBLE) return false;
    unsigned seen = 0;
    for (unsigned i = 0; i < APP_QUICK_ITEM_COUNT; ++i) {
        const unsigned item = layout->order[i];
        if (item >= APP_QUICK_ITEM_COUNT || (seen & (1u << item))) return false;
        seen |= 1u << item;
    }
    return true;
}
