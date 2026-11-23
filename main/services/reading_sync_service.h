/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_READING_SYNC_ACCESS_MAX 2048
#define APP_READING_SYNC_REFRESH_MAX 1024

typedef enum {
    APP_READING_SYNC_READEST = 1,
    APP_READING_SYNC_KOSYNC = 2,
    APP_READING_SYNC_BOTH = 3,
} app_reading_sync_provider_t;

typedef enum {
    APP_READING_SYNC_PUSH,
    APP_READING_SYNC_PULL,
    APP_READING_SYNC_AUTO,
} app_reading_sync_action_t;

typedef struct {
    char path[288];
    char title[192];
    uint32_t file_size;
    uint32_t chapter_byte_start;
    uint32_t chapter_byte_end;
    uint32_t byte_off;
    uint16_t chapter;
    uint16_t chapter_count;
    uint8_t percent;
} app_reading_sync_snapshot_t;

typedef struct {
    bool busy;
    bool pending;
    bool readest_logged_in;
    bool kosync_configured;
    bool remote_ready;
    bool remote_force;
    float remote_percent;
    char remote_path[288];
    char readest_state[64];
    char kosync_state[64];
    char message[128];
    char readest_user_code[24];
    char readest_verification_uri[128];
    char kosync_username[64];
    char kosync_server[256];
} app_reading_sync_status_t;

typedef struct {
    char readest_access[APP_READING_SYNC_ACCESS_MAX];
    char readest_refresh[APP_READING_SYNC_REFRESH_MAX];
    char readest_user[96];
    uint64_t readest_expires;
    char kosync_username[64];
    char kosync_key[33];
    char kosync_server[256];
    char kosync_device_id[33];
} app_reading_sync_backup_t;

void app_reading_sync_init(void);
void app_reading_sync_poll(void);
void app_reading_sync_get_status(app_reading_sync_status_t *out);

bool app_reading_sync_queue(const app_reading_sync_snapshot_t *snapshot,
                            app_reading_sync_provider_t provider,
                            app_reading_sync_action_t action);
bool app_reading_sync_request_readest_login(void);
bool app_reading_sync_configure_kosync(const char *username, const char *password,
                                       const char *server);
bool app_reading_sync_request_kosync_register(void);
bool app_reading_sync_take_remote(const char *path, float *percent, bool *force);
bool app_reading_sync_backup_export(app_reading_sync_backup_t *out);
bool app_reading_sync_backup_valid(const app_reading_sync_backup_t *backup);
bool app_reading_sync_backup_restore(const app_reading_sync_backup_t *backup);

