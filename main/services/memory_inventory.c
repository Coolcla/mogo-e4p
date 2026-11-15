/*
 * 中文：盘点仍存活的资源，不改变其所有权；未覆盖的驱动、任务栈及临时分配保留为未追踪。
 * English: Inventory live resources without taking ownership; uncovered drivers, task stacks and temporary allocations stay untracked.
 */
#include "memory_inventory.h"
#include "display_memory.h"
#include "ttf_font.h"
#include "ui_font.h"
#include "reader_prefetch.h"
#include "book_layout.h"
#include "ui_reader_background.h"
#include "ui_avatar.h"
#include "book_cover_jobs.h"
#include "lock_style_images.h"
#include "wifi_transfer.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"

static void count_allocation(void *context, const void *allocation) {
    if (!allocation) return;
    mm_owner_snapshot_t *owner = context;
    // 访问者只交付仍持有的堆分配基址；静态数据和借用别名由资源所有者排除。
    // Visitors supply only live heap allocation bases, excluding static data and borrowed aliases at the owner.
    if (esp_ptr_external_ram(allocation))
        owner->psram_bytes += heap_caps_get_allocated_size((void *)allocation);
    else if (esp_ptr_internal(allocation))
        owner->internal_bytes += heap_caps_get_allocated_size((void *)allocation);
}

void memory_inventory_snapshot(const display_state_t *display, mm_snapshot_t *out) {
    if (!out) return;
    mm_get_snapshot(out);
    display_visit_allocations(display, count_allocation, &out->owner[MM_OWNER_SYSTEM]);
    ui_font_visit_allocations(count_allocation, &out->owner[MM_OWNER_UI_CACHE]);
    ui_avatar_visit_allocations(count_allocation, &out->owner[MM_OWNER_UI_CACHE]);
    if (reader_prefetch_try_lock()) {
        ttf_font_visit_allocations(count_allocation, &out->owner[MM_OWNER_READER]);
        book_layout_visit_allocations(count_allocation, &out->owner[MM_OWNER_READER]);
        ui_reader_background_visit_allocations(count_allocation, &out->owner[MM_OWNER_READER]);
        reader_prefetch_unlock();
    } else {
        out->inventory_partial = true;
    }
    // 预绘帧已在 mm 账本中，不再次遍历；文件结果的访问由各自发布锁保护。
    // Prefetch frames are already in the mm ledger; file-result visitors hold their publication locks.
    book_cover_jobs_visit_allocations(count_allocation, &out->owner[MM_OWNER_FILE_JOB]);
    lock_images_visit_allocations(count_allocation, &out->owner[MM_OWNER_FILE_JOB]);
    size_t internal = 0, psram = 0;
    wifi_transfer_memory_usage(&internal, &psram);
    out->owner[MM_OWNER_NETWORK].internal_bytes += internal;
    out->owner[MM_OWNER_NETWORK].psram_bytes += psram;
    internal = psram = 0;
    for (unsigned i = 0; i < MM_OWNER_UNTRACKED; ++i) {
        internal += out->owner[i].internal_bytes;
        psram += out->owner[i].psram_bytes;
    }
    out->owner[MM_OWNER_UNTRACKED].internal_bytes = out->pool[MM_POOL_INTERNAL].used_bytes > internal
        ? out->pool[MM_POOL_INTERNAL].used_bytes - internal : 0;
    out->owner[MM_OWNER_UNTRACKED].psram_bytes = out->pool[MM_POOL_PSRAM].used_bytes > psram
        ? out->pool[MM_POOL_PSRAM].used_bytes - psram : 0;
    if (internal > out->pool[MM_POOL_INTERNAL].used_bytes || psram > out->pool[MM_POOL_PSRAM].used_bytes)
        out->inventory_partial = true;

    static int64_t next_log_ms;
    if (out->updated_ms >= next_log_ms) {
        next_log_ms = out->updated_ms + 5000;
        ESP_LOGI("memory", "MEMORY_USAGE v=1 internal_used=%u psram_used=%u internal_free=%u psram_free=%u "
                 "system=%u reader=%u ui=%u network=%u file=%u other=%u partial=%u",
                 (unsigned)out->pool[MM_POOL_INTERNAL].used_bytes, (unsigned)out->pool[MM_POOL_PSRAM].used_bytes,
                 (unsigned)out->pool[MM_POOL_INTERNAL].free_bytes, (unsigned)out->pool[MM_POOL_PSRAM].free_bytes,
                 (unsigned)(out->owner[MM_OWNER_SYSTEM].internal_bytes + out->owner[MM_OWNER_SYSTEM].psram_bytes),
                 (unsigned)(out->owner[MM_OWNER_READER].internal_bytes + out->owner[MM_OWNER_READER].psram_bytes),
                 (unsigned)(out->owner[MM_OWNER_UI_CACHE].internal_bytes + out->owner[MM_OWNER_UI_CACHE].psram_bytes),
                 (unsigned)(out->owner[MM_OWNER_NETWORK].internal_bytes + out->owner[MM_OWNER_NETWORK].psram_bytes),
                 (unsigned)(out->owner[MM_OWNER_FILE_JOB].internal_bytes + out->owner[MM_OWNER_FILE_JOB].psram_bytes),
                 (unsigned)(out->owner[MM_OWNER_UNTRACKED].internal_bytes + out->owner[MM_OWNER_UNTRACKED].psram_bytes),
                 (unsigned)out->inventory_partial);
    }
}
