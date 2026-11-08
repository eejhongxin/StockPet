#ifndef ARCS_MINI_SERVICE_STOCK_H
#define ARCS_MINI_SERVICE_STOCK_H

#include "stock_quote.h"

typedef enum {
    STOCK_STATE_LOADING = 0,
    STOCK_STATE_READY,
    STOCK_STATE_ERROR,
} stock_state_t;

typedef struct {
    stock_state_t state;
    uint32_t count;
    uint32_t revision;
    stock_instrument_t instruments[STOCK_WATCH_MAX];
    stock_quote_t quotes[STOCK_WATCH_MAX];
    bool network_connected;
    bool clock_available;
    bool scheduled[STOCK_WATCH_MAX];
    bool stale[STOCK_WATCH_MAX];
    int64_t now_utc;
    int64_t last_success_utc;
    uint32_t next_poll_seconds;
    uint32_t sequence;
    char error[48];
} stock_snapshot_t;

int service_stock_init(void);
/* Bounded critical-section copy only; safe from the UI task. */
void service_stock_snapshot(stock_snapshot_t *out);
/* add/remove/list. May perform network or storage I/O: call from a worker, never UI/voice.ebus. */
int service_stock_manage(const char *action, const char *query, char *text, size_t text_size);

#endif
