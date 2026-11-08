#ifndef ARCS_MINI_SERVICE_STOCK_CHART_H
#define ARCS_MINI_SERVICE_STOCK_CHART_H

#include "stock_chart.h"

typedef struct {
    stock_chart_t chart;
    char symbol[STOCK_SYMBOL_SIZE];
    bool loading;
    bool failed;
    uint32_t sequence;
    char error[48];
} stock_chart_snapshot_t;

int service_stock_chart_init(void);
/* Nonblocking. Empty symbol clears selection. Only watched symbols are fetched. */
void service_stock_chart_select(const char *symbol);
/* Bounded copy of selected stock only; safe from UI. */
void service_stock_chart_snapshot(stock_chart_snapshot_t *out);

#endif
