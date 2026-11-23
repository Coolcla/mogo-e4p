/* SPDX-License-Identifier: Apache-2.0 */
#include "timezone.h"
#include "timezone_posix.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static char s_applied_iana[64];

const char *app_timezone_posix_rule(const char *iana_name) {
    if (!iana_name || !iana_name[0]) return NULL;
    size_t low = 0, high = sizeof(s_timezone_posix) / sizeof(s_timezone_posix[0]);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(s_timezone_posix[middle].iana, iana_name);
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return low < sizeof(s_timezone_posix) / sizeof(s_timezone_posix[0]) &&
           !strcmp(s_timezone_posix[low].iana, iana_name)
        ? s_timezone_posix[low].posix : NULL;
}

bool app_timezone_set_iana(const char *iana_name) {
    if (!iana_name || !iana_name[0] || strlen(iana_name) >= sizeof(s_applied_iana)) return false;
    if (!strcmp(s_applied_iana, iana_name)) return true;
    const char *rule = app_timezone_posix_rule(iana_name);
    if (!rule || setenv("TZ", rule, 1) != 0) return false;
    tzset();
    memcpy(s_applied_iana, iana_name, strlen(iana_name) + 1);
    return true;
}
