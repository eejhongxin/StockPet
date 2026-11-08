#include "service_stock_chart.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lisa_log.h"
#include "lisa_mem.h"
#include "lisa_thread.h"
#include "service_stock.h"
#include "stock_http.h"
#include "sys_network_manager.h"
#include "sysutils.h"

#define TAG "stock.chart"

typedef struct {
    char symbol[STOCK_SYMBOL_SIZE];
    stock_chart_t chart;
    bool failed;
    unsigned failures;
    TickType_t fetched_at;
    uint32_t retry_seconds;
    char error[48];
} chart_cache_t;

static bool s_initialized;
static chart_cache_t s_cache[STOCK_WATCH_MAX] __psram_bss__;
static stock_chart_snapshot_t s_view __psram_bss__;
static unsigned s_evict;

void service_stock_chart_select(const char *symbol)
{
    if (!symbol || strlen(symbol) >= STOCK_SYMBOL_SIZE) return;
    taskENTER_CRITICAL();
    if (strcmp(symbol, s_view.symbol) != 0) {
        uint32_t sequence = s_view.sequence + 1;
        memset(&s_view, 0, sizeof(s_view));
        s_view.sequence = sequence;
        snprintf(s_view.symbol, sizeof(s_view.symbol), "%s", symbol);
        s_view.loading = symbol[0] != '\0';
        for (unsigned i = 0; i < STOCK_WATCH_MAX; ++i) {
            if (symbol[0] && strcmp(s_cache[i].symbol, symbol) == 0) {
                s_view.chart = s_cache[i].chart;
                s_view.failed = s_cache[i].failed;
                s_view.loading = !s_view.chart.valid && !s_view.failed;
                memcpy(s_view.error, s_cache[i].error, sizeof(s_view.error));
                break;
            }
        }
    }
    taskEXIT_CRITICAL();
}

void service_stock_chart_snapshot(stock_chart_snapshot_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL();
    *out = s_view;
    taskEXIT_CRITICAL();
}

static int watched(const stock_snapshot_t *watch, const char *symbol)
{
    for (unsigned i = 0; i < watch->count; ++i) {
        if (strcmp(watch->instruments[i].symbol, symbol) == 0) return (int)i;
    }
    return -1;
}

static bool connected(void)
{
    sys_network_status_t network = {0};
    return sys_network_get_status(&network) == 0 && network.connected && !network.switching;
}

/* Only the chart worker mutates cache entries. Readers take bounded critical copies. */
static unsigned cache_slot(const char *symbol)
{
    for (unsigned i = 0; i < STOCK_WATCH_MAX; ++i) {
        if (strcmp(s_cache[i].symbol, symbol) == 0) return i;
    }
    unsigned slot = STOCK_WATCH_MAX;
    for (unsigned i = 0; i < STOCK_WATCH_MAX; ++i) {
        if (!s_cache[i].symbol[0]) { slot = i; break; }
    }
    if (slot == STOCK_WATCH_MAX) slot = s_evict++ % STOCK_WATCH_MAX;
    taskENTER_CRITICAL();
    memset(&s_cache[slot], 0, sizeof(s_cache[slot]));
    snprintf(s_cache[slot].symbol, sizeof(s_cache[slot].symbol), "%s", symbol);
    taskEXIT_CRITICAL();
    return slot;
}

static bool fetch(const stock_instrument_t *instrument, stock_chart_t *chart,
                  char *error, size_t error_size)
{
    char url[160];
    snprintf(url, sizeof(url), "https://web.ifzq.gtimg.cn/appstock/app/%s/query?code=%s",
             instrument->market == STOCK_MARKET_CN ? "minute" : "UsMinute", instrument->symbol);
    char *body = lisa_mem_alloc(STOCK_RESPONSE_MAX + 1);
    if (!body) {
        snprintf(error, error_size, "no_memory");
        return false;
    }
    size_t size = 0;
    bool good = stock_http_get(url, body, STOCK_RESPONSE_MAX + 1, &size, error, error_size);
    if (good && !stock_chart_parse(body, size, instrument, chart)) {
        snprintf(error, error_size, "invalid_chart");
        good = false;
    }
    lisa_mem_free(body);
    return good;
}

static void chart_worker(void *unused)
{
    (void)unused;
    for (;;) {
        char symbol[STOCK_SYMBOL_SIZE];
        taskENTER_CRITICAL();
        memcpy(symbol, s_view.symbol, sizeof(symbol));
        taskEXIT_CRITICAL();
        stock_snapshot_t watch;
        service_stock_snapshot(&watch);
        int selected = watched(&watch, symbol);
        if (symbol[0] && selected >= 0 && connected()) {
            unsigned slot = cache_slot(symbol);
            chart_cache_t *cache = &s_cache[slot];
            uint32_t elapsed = (xTaskGetTickCount() - cache->fetched_at) / configTICK_RATE_HZ;
            if ((!cache->chart.valid && !cache->failed) || elapsed >= cache->retry_seconds) {
                taskENTER_CRITICAL();
                if (strcmp(s_view.symbol, symbol) == 0 && !s_view.loading) {
                    s_view.loading = true;
                    ++s_view.sequence;
                }
                taskEXIT_CRITICAL();
                /* Allocate the parser result outside the task stack (about 3.4 KiB). */
                stock_chart_t *parsed = lisa_mem_alloc(sizeof(*parsed));
                char error[48] = "no_memory";
                bool good = parsed && fetch(&watch.instruments[selected], parsed, error, sizeof(error));
                int date_order = good && cache->chart.valid ? strcmp(parsed->date, cache->chart.date) : 1;
                if (good && cache->chart.valid && (date_order < 0 ||
                    (date_order == 0 && parsed->points[parsed->count - 1].minute <
                     cache->chart.points[cache->chart.count - 1].minute))) {
                    snprintf(error, sizeof(error), "older_chart");
                    good = false;
                }
                stock_snapshot_t current;
                service_stock_snapshot(&current);
                /* Removed stocks may finish an HTTP request but can never repopulate the UI. */
                bool still_watched = watched(&current, symbol) >= 0;
                int64_t now = (int64_t)time(NULL);
                bool scheduled = now >= 1704067200LL &&
                    stock_market_session_scheduled(watch.instruments[selected].market, now);
                taskENTER_CRITICAL();
                cache->fetched_at = xTaskGetTickCount();
                cache->failed = !good;
                if (good) {
                    cache->chart = *parsed;
                    cache->failures = 0;
                    cache->error[0] = '\0';
                    cache->retry_seconds = scheduled ? 15 : 60;
                } else {
                    if (cache->failures < 4) ++cache->failures;
                    cache->retry_seconds = 15u << cache->failures;
                    snprintf(cache->error, sizeof(cache->error), "%s", error);
                }
                if (still_watched && strcmp(s_view.symbol, symbol) == 0) {
                    s_view.chart = cache->chart;
                    s_view.loading = false;
                    s_view.failed = cache->failed;
                    memcpy(s_view.error, cache->error, sizeof(s_view.error));
                    ++s_view.sequence;
                }
                taskEXIT_CRITICAL();
                if (good) LOGI("%s date=%s points=%u", symbol, parsed->date, parsed->count);
                else LOGW("%s failed: %s", symbol, error);
                lisa_mem_free(parsed);
            }
        }
        lisa_thread_mdelay(500);
    }
}

int service_stock_chart_init(void)
{
    if (s_initialized) return 0;
    const lisa_thread_attr_t attr = {.name = (uint8_t *)"stock.chart", .stack_size = 12 * 1024, .priority = 4};
    if (!lisa_thread_create(&attr, chart_worker, NULL)) return -1;
    s_initialized = true;
    LOGI("intraday chart worker enabled");
    return 0;
}
