/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

/// Apply the complete POSIX transition rule for an IANA timezone name.
bool app_timezone_set_iana(const char *iana_name);

/// Return the embedded POSIX rule for an IANA timezone name, or NULL.
const char *app_timezone_posix_rule(const char *iana_name);
