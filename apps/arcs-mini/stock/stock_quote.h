#ifndef ARCS_MINI_STOCK_QUOTE_H
#define ARCS_MINI_STOCK_QUOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STOCK_QUOTE_COUNT 2u /* Legacy two-stock API. */
#define STOCK_WATCH_MAX 6u
#define STOCK_SYMBOL_SIZE 16u
#define STOCK_NAME_SIZE 32u
#define STOCK_QUERY_MAX 80u
#define STOCK_RESPONSE_MAX 16384u

typedef enum {
    STOCK_MARKET_CN = 0,
    STOCK_MARKET_US = 1,
} stock_market_t;

typedef struct {
    stock_market_t market;
    char symbol[STOCK_SYMBOL_SIZE];
    char name[STOCK_NAME_SIZE];
} stock_instrument_t;

typedef enum {
    STOCK_LEXIN = 0,
    STOCK_NVDA = 1,
} stock_symbol_t;

typedef struct {
    bool valid;
    stock_symbol_t symbol; /* Legacy market tag; use snapshot instrument for identity. */
    char name[STOCK_NAME_SIZE];
    double price;
    double previous_close;
    double change;
    double change_pct;
    /* Source-local wall time: China Standard Time for CN, New York for US. */
    char time_text[20];
    int64_t epoch_utc;
} stock_quote_t;

/* Normalize a code, never a company name. Prefix us is lower-case to distinguish
 * actual tickers USB/USO; CN prefixes are case-insensitive. Failure leaves out unchanged. */
bool stock_instrument_from_code(const char *code, stock_instrument_t *out);
/* Match an exact canonical symbol and validate the provider's internal stock code. */
bool stock_quote_parse_instrument(const void *data, size_t size,
                                  const stock_instrument_t *instrument, stock_quote_t *out);
/* Regular-hours schedule only: exchange holidays are not inferred. */
bool stock_market_session_scheduled(stock_market_t market, int64_t now_utc);

/* Legacy fixed-stock wrappers; parse only the requested complete record. */
bool stock_quote_parse(const void *data, size_t size, stock_symbol_t symbol, stock_quote_t *out);
/* Regular-hours schedule only: exchange holidays are not inferred. */
bool stock_quote_session_scheduled(stock_symbol_t symbol, int64_t now_utc);
/* Valid quote older than 180 seconds (or over 60 seconds in future) is stale. */
bool stock_quote_is_stale(const stock_quote_t *quote, int64_t now_utc);

#endif
