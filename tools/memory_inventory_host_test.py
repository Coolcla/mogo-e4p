#!/usr/bin/env python3
"""中文：验证真实盘点服务与资源访问者的分池、释放交还及并发所有权。
English: Check the real inventory service and resource visitors across pools, release/handoff and concurrent ownership.
"""
from pathlib import Path
import subprocess
import tempfile

from home_cover_cache_host_test import function

ROOT = Path(__file__).resolve().parents[1]

UNIT = r'''
#include "memory_inventory.h"
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) assert(!pthread_mutex_lock(lock))
#define portEXIT_CRITICAL(lock) assert(!pthread_mutex_unlock(lock))
static pthread_mutex_t allocations_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {void *ptr;size_t bytes;mm_pool_t pool;} allocations[512];
static size_t live[MM_POOL_COUNT];
static unsigned logs;
static int64_t now_ms;
static void *own(size_t bytes,mm_pool_t pool){
 void *p=calloc(1,bytes);assert(p);portENTER_CRITICAL(&allocations_lock);
 for(unsigned i=0;i<512;++i)if(!allocations[i].ptr){
  allocations[i].ptr=p;allocations[i].bytes=bytes;allocations[i].pool=pool;live[pool]+=bytes;
  portEXIT_CRITICAL(&allocations_lock);return p;
 }abort();
}
static void release(void **p){
 if(!*p)return;portENTER_CRITICAL(&allocations_lock);
 for(unsigned i=0;i<512;++i)if(allocations[i].ptr==*p){
  live[allocations[i].pool]-=allocations[i].bytes;allocations[i].ptr=NULL;
  free(*p);*p=NULL;portEXIT_CRITICAL(&allocations_lock);return;
 }abort();
}
static bool is_pool(const void *p,mm_pool_t pool){
 bool found=false;portENTER_CRITICAL(&allocations_lock);
 for(unsigned i=0;i<512;++i)if(allocations[i].ptr==p){found=allocations[i].pool==pool;break;}
 portEXIT_CRITICAL(&allocations_lock);return found;
}
bool esp_ptr_external_ram(const void *p){return is_pool(p,MM_POOL_PSRAM);}
bool esp_ptr_internal(const void *p){return is_pool(p,MM_POOL_INTERNAL);}
size_t heap_caps_get_allocated_size(void *p){
 portENTER_CRITICAL(&allocations_lock);
 for(unsigned i=0;i<512;++i)if(allocations[i].ptr==p){size_t n=allocations[i].bytes;portEXIT_CRITICAL(&allocations_lock);return n;}
 abort();
}
void inventory_test_log(const char *tag,const char *format,...){assert(!strcmp(tag,"memory")&&strstr(format,"MEMORY_USAGE"));++logs;}
static bool reader_busy;
static unsigned reader_locks,reader_unlocks,font_visits;
static void *font_source,*tracked_frame;
bool reader_prefetch_try_lock(void){if(reader_busy)return false;++reader_locks;return true;}
void reader_prefetch_unlock(void){++reader_unlocks;assert(reader_unlocks==reader_locks);}
void ttf_font_visit_allocations(void (*visit)(void *,const void *),void *context){
 assert(!reader_busy&&reader_locks==reader_unlocks+1);++font_visits;visit(context,font_source);
}
static size_t network_internal,network_psram;
void wifi_transfer_memory_usage(size_t *internal,size_t *psram){*internal=network_internal;*psram=network_psram;}
void mm_get_snapshot(mm_snapshot_t *out){
 memset(out,0,sizeof(*out));out->updated_ms=now_ms;
 portENTER_CRITICAL(&allocations_lock);
 for(unsigned i=0;i<MM_POOL_COUNT;++i){out->pool[i].total=1024*1024;out->pool[i].used_bytes=live[i]+4096;
  out->pool[i].free_bytes=out->pool[i].total-out->pool[i].used_bytes;}
 portEXIT_CRITICAL(&allocations_lock);
 if(tracked_frame)out->owner[MM_OWNER_READER].psram_bytes=heap_caps_get_allocated_size(tracked_frame);
}
struct EpdiyHighlevelState {void *front_fb,*back_fb,*difference_fb,*dirty_lines,*dirty_columns;};
DISPLAY_VISITOR
#define UI_GLYPH_SLOTS 256
static struct {void *bitmap;} *s_glyphs;
UI_FONT_VISITOR
static void *s_pages,*s_line,*s_images,*s_page_img_start,*s_gray;
static struct {void *glyphs;}s_glyph_maps[2];
LAYOUT_VISITOR
BACKGROUND_VISITOR
static void *s_pixels[2];
static portMUX_TYPE s_avatar_lock=portMUX_INITIALIZER_UNLOCKED;
AVATAR_VISITOR
#define BOOK_COVER_JOB_SLOTS 9
static struct {struct {void *pixels;}result;void *automatic;} *s_jobs;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
COVER_VISITOR
#define RAM_SLOTS 12
#define s_lock image_lock
static portMUX_TYPE image_lock=portMUX_INITIALIZER_UNLOCKED;
static struct {void *pixels;}s_slots[RAM_SLOTS];
static void *s_catalogue;
LOCK_VISITOR
#undef s_lock
static void conserved(const mm_snapshot_t *snapshot){
 size_t internal=0,psram=0;
 for(unsigned i=0;i<MM_OWNER_COUNT;++i){internal+=snapshot->owner[i].internal_bytes;psram+=snapshot->owner[i].psram_bytes;}
 assert(internal==snapshot->pool[MM_POOL_INTERNAL].used_bytes&&psram==snapshot->pool[MM_POOL_PSRAM].used_bytes);
}
static atomic_bool swapping;
static void *swap_avatar(void *unused){
 (void)unused;
 for(unsigned n=0;n<5000;++n){
  void *next=own(32+n%11,MM_POOL_PSRAM);
  portENTER_CRITICAL(&s_avatar_lock);void *old=s_pixels[0];s_pixels[0]=next;portEXIT_CRITICAL(&s_avatar_lock);
  release(&old);
 }
 atomic_store(&swapping,false);return NULL;
}
int main(void){
 mm_snapshot_t snapshot;display_state_t display={0};
 memory_inventory_snapshot(NULL,NULL);
 memory_inventory_snapshot(NULL,&snapshot);assert(logs==1&&!snapshot.inventory_partial);conserved(&snapshot);
 for(unsigned i=0;i<MM_OWNER_UNTRACKED;++i)assert(!snapshot.owner[i].psram_bytes&&!snapshot.owner[i].internal_bytes);
 display=(display_state_t){.front_fb=own(200,MM_POOL_PSRAM),.back_fb=own(201,MM_POOL_PSRAM),
  .difference_fb=own(400,MM_POOL_PSRAM),.dirty_lines=own(21,MM_POOL_INTERNAL),.dirty_columns=own(22,MM_POOL_INTERNAL)};
 font_source=own(300,MM_POOL_PSRAM);tracked_frame=own(64,MM_POOL_PSRAM);
 s_glyphs=own(UI_GLYPH_SLOTS*sizeof(*s_glyphs),MM_POOL_PSRAM);s_glyphs[17].bitmap=own(80,MM_POOL_PSRAM);
 s_pixels[0]=own(60,MM_POOL_PSRAM);s_pixels[1]=own(82,MM_POOL_PSRAM);
 s_pages=own(12,MM_POOL_PSRAM);s_glyph_maps[0].glyphs=own(11,MM_POOL_INTERNAL);s_gray=own(70,MM_POOL_PSRAM);
 s_jobs=own(BOOK_COVER_JOB_SLOTS*sizeof(*s_jobs),MM_POOL_PSRAM);
 s_jobs[2].result.pixels=own(150,MM_POOL_PSRAM);s_jobs[8].automatic=own(160,MM_POOL_PSRAM);
 s_catalogue=own(170,MM_POOL_PSRAM);s_slots[4].pixels=own(180,MM_POOL_PSRAM);
 now_ms=500;memory_inventory_snapshot(&display,&snapshot);assert(logs==1&&!snapshot.inventory_partial);conserved(&snapshot);
 assert(snapshot.owner[MM_OWNER_SYSTEM].psram_bytes==801&&snapshot.owner[MM_OWNER_SYSTEM].internal_bytes==43);
 assert(snapshot.owner[MM_OWNER_READER].psram_bytes==446&&snapshot.owner[MM_OWNER_READER].internal_bytes==11);
 assert(snapshot.owner[MM_OWNER_UI_CACHE].psram_bytes==UI_GLYPH_SLOTS*sizeof(*s_glyphs)+222);
 assert(snapshot.owner[MM_OWNER_FILE_JOB].psram_bytes==BOOK_COVER_JOB_SLOTS*sizeof(*s_jobs)+660);
 assert(snapshot.owner[MM_OWNER_UNTRACKED].internal_bytes==4096&&snapshot.owner[MM_OWNER_UNTRACKED].psram_bytes==4096);
 // 已转交与已释放资源不再归属原模块，存活的借用方仍计入未知。
 // Transferred/released buffers leave their former owner's total; live borrowers remain untracked.
 void *transferred=s_jobs[2].result.pixels;s_jobs[2].result.pixels=NULL;
 release(&s_jobs[8].automatic);release(&s_slots[4].pixels);release(&s_catalogue);
 release(&s_glyphs[17].bitmap);release((void **)&s_glyphs);
 now_ms=1000;memory_inventory_snapshot(&display,&snapshot);conserved(&snapshot);
 assert(snapshot.owner[MM_OWNER_FILE_JOB].psram_bytes==BOOK_COVER_JOB_SLOTS*sizeof(*s_jobs));
 assert(snapshot.owner[MM_OWNER_UI_CACHE].psram_bytes==142);
 assert(snapshot.owner[MM_OWNER_UNTRACKED].psram_bytes==4246);
 unsigned before=font_visits;reader_busy=true;memory_inventory_snapshot(&display,&snapshot);
 assert(snapshot.inventory_partial&&font_visits==before&&reader_locks==reader_unlocks);
 assert(snapshot.owner[MM_OWNER_READER].psram_bytes==64&&!snapshot.owner[MM_OWNER_READER].internal_bytes);conserved(&snapshot);
 reader_busy=false;
 void *network_buffer=own(128,MM_POOL_PSRAM),*network_stack=own(256,MM_POOL_INTERNAL);
 network_internal=256;network_psram=128;now_ms=5000;memory_inventory_snapshot(&display,&snapshot);
 assert(snapshot.owner[MM_OWNER_NETWORK].internal_bytes==256&&snapshot.owner[MM_OWNER_NETWORK].psram_bytes==128&&logs==2);conserved(&snapshot);
 network_internal=network_psram=0;release(&network_buffer);release(&network_stack);
 // 真实头像访问者与并发发布/释放竞争时保持指针存活，不停工作任务。
 // The real avatar visitor preserves pointer lifetime during concurrent publication/free without stopping workers.
 pthread_t worker;atomic_store(&swapping,true);assert(!pthread_create(&worker,NULL,swap_avatar,NULL));
 do {memory_inventory_snapshot(&display,&snapshot);}while(atomic_load(&swapping));
 assert(!pthread_join(worker,NULL));memory_inventory_snapshot(&display,&snapshot);conserved(&snapshot);
 assert(!snapshot.inventory_partial&&!snapshot.owner[MM_OWNER_NETWORK].psram_bytes&&!snapshot.owner[MM_OWNER_NETWORK].internal_bytes);
 release(&transferred);release(&s_pixels[0]);release(&s_pixels[1]);
 for(unsigned i=0;i<512;++i)if(allocations[i].ptr){free(allocations[i].ptr);allocations[i].ptr=NULL;}
 puts("PASS: real display/UI/layout/file visitors, live pool attribution, tracked-frame exclusion, handoff/release, busy readers, network lifecycle and 5000 concurrent avatar swaps");
}
'''


def main():
    visitors = {
        "DISPLAY_VISITOR": ("main/display/display.c", "display_visit_allocations"),
        "UI_FONT_VISITOR": ("main/font/ui_font.c", "ui_font_visit_allocations"),
        "LAYOUT_VISITOR": ("main/book/book_layout.c", "book_layout_visit_allocations"),
        "BACKGROUND_VISITOR": ("main/ui/ui_reader_background.c", "ui_reader_background_visit_allocations"),
        "AVATAR_VISITOR": ("main/ui/ui_avatar.c", "ui_avatar_visit_allocations"),
        "COVER_VISITOR": ("main/book/book_cover_jobs.c", "book_cover_jobs_visit_allocations"),
        "LOCK_VISITOR": ("main/services/lock_style_images.c", "lock_images_visit_allocations"),
    }
    unit = UNIT
    for marker, (path, name) in visitors.items():
        unit = unit.replace(marker, function(name, ROOT / path))
    with tempfile.TemporaryDirectory(prefix="memory-inventory-") as directory:
        work = Path(directory)
        headers = {
            "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n",
            "esp_heap_caps.h": "#pragma once\n#include <stddef.h>\nsize_t heap_caps_get_allocated_size(void *p);\n",
            "esp_memory_utils.h": "#pragma once\n#include <stdbool.h>\nbool esp_ptr_external_ram(const void *p);\nbool esp_ptr_internal(const void *p);\n",
            "esp_log.h": "#pragma once\nvoid inventory_test_log(const char *,const char *,...);\n#define ESP_LOGI(...) inventory_test_log(__VA_ARGS__)\n",
            "reader_prefetch.h": "#pragma once\n#include <stdbool.h>\nbool reader_prefetch_try_lock(void);\nvoid reader_prefetch_unlock(void);\n",
            "wifi_transfer.h": "#pragma once\n#include <stddef.h>\nvoid wifi_transfer_memory_usage(size_t *,size_t *);\n",
            "display_memory.h": "#pragma once\n#include \"display_types.h\"\nvoid display_visit_allocations(const display_state_t *,void (*)(void *,const void *),void *);\n",
        }
        for header, name in (
            ("ttf_font.h", "ttf_font_visit_allocations"),
            ("ui_font.h", "ui_font_visit_allocations"),
            ("book_layout.h", "book_layout_visit_allocations"),
            ("ui_reader_background.h", "ui_reader_background_visit_allocations"),
            ("ui_avatar.h", "ui_avatar_visit_allocations"),
            ("book_cover_jobs.h", "book_cover_jobs_visit_allocations"),
            ("lock_style_images.h", "lock_images_visit_allocations"),
        ):
            headers[header] = f"#pragma once\nvoid {name}(void (*)(void *,const void *),void *);\n"
        for name, contents in headers.items():
            (work / name).write_text(contents)
        (work / "test.c").write_text(unit)
        binary = work / "test"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g",
            "-fsanitize=address,undefined", "-pthread", "-I" + str(work),
            "-I" + str(ROOT / "main/services"), "-I" + str(ROOT / "main/display"),
            str(work / "test.c"), str(ROOT / "main/services/memory_inventory.c"), "-o", str(binary),
        ], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
