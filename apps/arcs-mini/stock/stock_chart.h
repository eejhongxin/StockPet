#ifndef ARCS_MINI_STOCK_CHART_H
#define ARCS_MINI_STOCK_CHART_H

#include "stock_quote.h"

#define STOCK_CHART_MAX_POINTS 420u

typedef struct {
    uint16_t minute; /* Minute of the source-local day, including session gaps. */
    float price;
} stock_chart_point_t;

typedef struct {
    bool valid;
    stock_market_t market;
    char symbol[STOCK_SYMBOL_SIZE];
    char date[11]; /* Source trading date, YYYY-MM-DD. */
    float previous_close;
    uint16_t count;
    stock_chart_point_t points[STOCK_CHART_MAX_POINTS];
} stock_chart_t;

/* Parse Tencent minute/query (CN) or UsMinute/query (US) UTF-8 JSON.
 * Retains only real, ordered regular-session points; does not fill gaps.
 * Input is bounded by STOCK_RESPONSE_MAX. Failure leaves out unchanged. */
bool stock_chart_parse(const void *data, size_t size,
                       const stock_instrument_t *instrument, stock_chart_t *out);

#endif
