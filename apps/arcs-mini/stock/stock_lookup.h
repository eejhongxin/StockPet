#ifndef ARCS_MINI_STOCK_LOOKUP_H
#define ARCS_MINI_STOCK_LOOKUP_H

#include "stock_quote.h"

/* Resolve one name or code. 0 means resolved, -1 invalid/no match, -2 ambiguous
 * or temporarily unavailable. The caller must verify a full quote before adding.
 * Does bounded network I/O: call only from the stock worker, never voice/UI. */
int stock_lookup_resolve(const char *query, stock_instrument_t *out,
                         char *message, size_t message_size);

/* Pure response parser used by resolve and host checks. Output is unchanged on
 * failure. Provider exact name/code takes precedence over partial ETF matches. */
int stock_lookup_parse(const void *data, size_t size, const char *query,
                       stock_instrument_t *out, char *message, size_t message_size);

#endif
