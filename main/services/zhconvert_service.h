/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

typedef enum {
    APP_ZHCONVERT_IDLE,
    APP_ZHCONVERT_BUSY,
    APP_ZHCONVERT_DONE,
    APP_ZHCONVERT_FAILED,
} app_zhconvert_state_t;

typedef struct {
    app_zhconvert_state_t state;
    unsigned revision;
    unsigned completed, total;
    char output[288];
    char notice[97];
} app_zhconvert_status_t;

bool app_zhconvert_start(const char *input_path);
void app_zhconvert_cancel(void);
void app_zhconvert_status(app_zhconvert_status_t *out);
