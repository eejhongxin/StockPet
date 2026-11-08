#ifndef ARCS_MINI_STOCK_HTTP_H
#define ARCS_MINI_STOCK_HTTP_H

#include <stdbool.h>
#include <stddef.h>

/* Fixed Tencent HTTPS hosts only. capacity includes a trailing NUL; size excludes it.
 * Reject redirects/non-200, oversized/incomplete responses, and requests over ~12s.
 * Network I/O must run outside voice/UI tasks. */
bool stock_http_get(const char *url, char *body, size_t capacity, size_t *body_size,
                    char *error, size_t error_size);

#endif
