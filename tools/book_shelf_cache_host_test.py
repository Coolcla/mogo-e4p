#!/usr/bin/env python3
"""Validate the persistent stable-entry shelf cache with real file IO."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CACHE_SOURCE = (ROOT / "main/book/book_shelf_cache.c").read_text(encoding="utf-8")

with tempfile.TemporaryDirectory(prefix="book-shelf-cache-") as directory:
    root = Path(directory)
    cache_dir = root / "cache"
    cache_dir.mkdir()
    cache_dir_c = str(cache_dir).replace('\\', '\\\\').replace('"', '\\"')
    source = root / "test.c"
    source.write_text(f'''#include <assert.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <sys/stat.h>\n#include <unistd.h>\n#define BOOK_SHELF_CACHE_DIR "{cache_dir_c}"\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\nstatic void *heap_caps_calloc(size_t count, size_t size, int caps) {{ (void)caps; return calloc(count, size); }}\n#include "{ROOT / 'main/book/book_shelf_model.h'}"\n#include "{ROOT / 'main/book/book_shelf_scan.h'}"\n{CACHE_SOURCE}\nstatic void write_file(const char *path, const char *value) {{ FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(value, 1, strlen(value), f) == strlen(value)); assert(!fclose(f)); }}\nint main(void) {{\n    char book[BOOK_STORE_PATH_MAX]; snprintf(book, sizeof(book), "%s/book.epub", "{root}"); write_file(book, "x");\n    book_shelf_entry_t entry = {{0}}; snprintf(entry.path, sizeof(entry.path), "%s", book);\n    snprintf(entry.name, sizeof(entry.name), "%s", "第一版"); snprintf(entry.author, sizeof(entry.author), "%s", "作者");\n    book_shelf_cache_t *cache = book_shelf_cache_open("card/one"); assert(cache);\n    assert(book_shelf_cache_save(cache, &entry, 1)); book_shelf_cache_close(cache);\n    cache = book_shelf_cache_open("card/one"); assert(cache); char name[256], author[128]; struct stat st; assert(!stat(book, &st));\n    assert(book_shelf_cache_lookup(cache, book, 1, st.st_mtime, name, sizeof(name), author, sizeof(author)));\n    assert(!strcmp(name, "第一版") && !strcmp(author, "作者")); assert(!book_shelf_cache_lookup(cache, book, 2, st.st_mtime, name, sizeof(name), author, sizeof(author)));\n    book_shelf_cache_close(cache); write_file(book, "xx"); cache = book_shelf_cache_open("card/one"); assert(cache); assert(!stat(book, &st));\n    assert(!book_shelf_cache_lookup(cache, book, 2, st.st_mtime, name, sizeof(name), author, sizeof(author)));\n    snprintf(entry.name, sizeof(entry.name), "%s", "第二版"); assert(book_shelf_cache_save(cache, &entry, 1)); book_shelf_cache_close(cache);\n    cache = book_shelf_cache_open("card/one"); assert(cache); assert(book_shelf_cache_lookup(cache, book, 2, st.st_mtime, name, sizeof(name), author, sizeof(author))); assert(!strcmp(name, "第二版")); book_shelf_cache_close(cache);\n    char cache_file[512]; snprintf(cache_file, sizeof(cache_file), "%s/.mogo-shelf-catalog-card_one.bin", "{cache_dir}"); FILE *broken = fopen(cache_file, "wb"); assert(broken); fputs("broken", broken); assert(!fclose(broken));\n    cache = book_shelf_cache_open("card/one"); assert(cache); assert(!book_shelf_cache_lookup(cache, book, 2, st.st_mtime, name, sizeof(name), author, sizeof(author))); book_shelf_cache_close(cache);\n    unlink(book); unlink(cache_file); puts("book_shelf_cache: PASS"); return 0;\n}}\n''', encoding="utf-8")
    heap_header = root / "esp_heap_caps.h"
    heap_header.write_text("#pragma once\n#include <stddef.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\nvoid *heap_caps_calloc(size_t,size_t,int);\n", encoding="utf-8")
    (root / "nvs.h").write_text("#pragma once\n#include <stdint.h>\ntypedef int nvs_handle_t;\n", encoding="utf-8")
    binary = root / "test"
    subprocess.run(["cc", "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", "-I" + str(root), "-I" + str(ROOT / "main/book"), "-I" + str(ROOT / "main"),
                    str(source), "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True)
