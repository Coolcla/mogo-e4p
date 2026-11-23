/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_WEBDAV_ITEMS 24
#define APP_WEBDAV_SERVER_MAX 192
typedef enum {
    APP_WEBDAV_IDLE,
    APP_WEBDAV_BUSY,
    APP_WEBDAV_READY,
    APP_WEBDAV_FAILED,
} app_webdav_state_t;
typedef struct {
    char name[128];
    char path[256]; /* URL-encoded path relative to /dav */
    uint32_t size;
    bool is_dir;
} app_webdav_item_t;
typedef struct {
    app_webdav_state_t state;
    unsigned count;
    unsigned revision;
    char notice[97];
} app_webdav_status_t;

bool app_webdav_set_credentials(const char *username, const char *password);
bool app_webdav_configure(const char *server_url, const char *username, const char *password);
bool app_webdav_config_valid(const char *server_url, const char *username, const char *password);
bool app_webdav_get_config(char *server_url, size_t server_capacity,
                           char *username, size_t username_capacity,
                           char *password, size_t password_capacity);
bool app_webdav_restore_config(bool configured, const char *server_url,
                               const char *username, const char *password);
bool app_webdav_configured(void);
void app_webdav_clear_credentials(void);
bool app_webdav_has_credentials(void);
bool app_webdav_list(const char *path);
bool app_webdav_download(const char *remote_path, const char *local_path);
bool app_webdav_upload(const char *local_path, const char *remote_path);
bool app_webdav_move(const char *remote_path, const char *destination_path);
bool app_webdav_delete(const char *remote_path);
void app_webdav_cancel(void);
void app_webdav_status(app_webdav_status_t *out);
bool app_webdav_item(unsigned index, app_webdav_item_t *out);
