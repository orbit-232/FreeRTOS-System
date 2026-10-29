#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PANEL_ONLINE_HTTP_TIMEOUT_MS
#define PANEL_ONLINE_HTTP_TIMEOUT_MS 12000
#endif

/* Bounded HTTPS GET helper.  On success, body is NUL-terminated and *len
 * excludes the terminator.  The caller still checks *status/content_type. */
esp_err_t panel_online_http_get(const char *url, char *body, size_t cap, size_t *len,
                                int *status, char *content_type, size_t content_type_cap);

#ifdef __cplusplus
}
#endif
