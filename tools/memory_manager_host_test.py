#!/usr/bin/env python3
"""中文：验证真实内存服务的区域准入、所有权、并发和清理状态机。
English: Exercise the real memory service across pool admission, ownership, concurrency and cleanup states.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="memory-manager-") as directory:
    work = Path(directory)
    headers = {
        "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_STATE 0x103\n#define ESP_ERR_INVALID_ARG 0x102\n",
        "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
        "esp_log.h": "#pragma once\n#include <stdio.h>\n#define ESP_LOGI(tag, ...) do { (void)(tag); printf(__VA_ARGS__); putchar('\\n'); } while (0)\n",
        "esp_heap_caps.h": """#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
size_t heap_caps_get_total_size(uint32_t);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
size_t heap_caps_get_minimum_free_size(uint32_t);
void *heap_caps_malloc(size_t,uint32_t);
void *heap_caps_aligned_alloc(size_t,size_t,uint32_t);
void heap_caps_free(void *);
""",
        "freertos/FreeRTOS.h": "#pragma once\n",
        "freertos/portmacro.h": """#pragma once
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
""",
    }
    for name, contents in headers.items():
        path = work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
    binary = work / "test"
    subprocess.run([
        "cc", "-std=c11", "-D_POSIX_C_SOURCE=200112L", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-pthread", "-I" + str(work), "-Imain/services",
        "tools/memory_manager_host_test.c", "main/services/memory_manager.c", "-o", str(binary),
    ], cwd=ROOT, check=True, timeout=60)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30)
