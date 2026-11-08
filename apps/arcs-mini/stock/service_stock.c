#include "service_stock.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "easyflash.h"
#include "lisa_kv.h"
#include "lisa_log.h"
#include "lisa_mem.h"
#include "lisa_mutex.h"
#include "lisa_thread.h"
#include "stock_http.h"
#include "stock_lookup.h"
#include "sys_network_manager.h"

#define TAG "stock.watch"
#define STOCK_CLOCK_MIN 1704067200LL
#define STOCK_STORAGE_KEY "stock.watch.v1"
#define STOCK_STORAGE_MAGIC 0x53544b31u

typedef struct {
    uint32_t magic;
    uint32_t count;
    stock_instrument_t instruments[STOCK_WATCH_MAX];
    uint32_t checksum;
} stock_saved_t;

static bool s_initialized;
static stock_snapshot_t s_snapshot;
static lisa_mutex_t *s_manage_mutex;

static bool clock_now(int64_t *now)
{
    *now = (int64_t)time(NULL);
    return *now >= STOCK_CLOCK_MIN;
}

static bool connected_now(void)
{
    sys_network_status_t network = {0};
    return sys_network_get_status(&network) == 0 && network.connected && !network.switching;
}

static uint32_t checksum(const stock_saved_t *saved)
{
    const unsigned char *bytes = (const unsigned char *)saved;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(stock_saved_t, checksum); ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static bool instrument_valid(const stock_instrument_t *instrument)
{
    if (!memchr(instrument->symbol, 0, sizeof(instrument->symbol)) ||
        !memchr(instrument->name, 0, sizeof(instrument->name)) || !instrument->name[0]) return false;
    stock_instrument_t normalized;
    if (!stock_instrument_from_code(instrument->symbol, &normalized) ||
        strcmp(normalized.symbol, instrument->symbol) != 0 || normalized.market != instrument->market) return false;
    /* Names originate from the bounded UTF-8 lookup decoder; disallow control bytes in persisted data. */
    for (const unsigned char *p = (const unsigned char *)instrument->name; *p; ++p) {
        if (*p < 32 || *p == 127) return false;
    }
    return true;
}

static bool save_watchlist(const stock_snapshot_t *snapshot)
{
    stock_saved_t saved = {.magic = STOCK_STORAGE_MAGIC, .count = snapshot->count};
    for (unsigned i = 0; i < saved.count; ++i) saved.instruments[i] = snapshot->instruments[i];
    saved.checksum = checksum(&saved);
    return lisa_kv_set_blob(STOCK_STORAGE_KEY, (uint8_t *)&saved, sizeof(saved)) == 0;
}

static void load_watchlist(stock_snapshot_t *snapshot)
{
    size_t size = 0;
    ef_get_env_blob(STOCK_STORAGE_KEY, NULL, 0, &size);
    if (size) {
        stock_saved_t saved;
        bool good = size == sizeof(saved) &&
                    ef_get_env_blob(STOCK_STORAGE_KEY, &saved, sizeof(saved), NULL) == sizeof(saved) &&
                    saved.magic == STOCK_STORAGE_MAGIC && saved.count <= STOCK_WATCH_MAX &&
                    saved.checksum == checksum(&saved);
        if (good) {
            for (unsigned i = 0; i < saved.count; ++i) {
                if (!instrument_valid(&saved.instruments[i])) { good = false; break; }
                for (unsigned j = 0; j < i; ++j) {
                    if (strcmp(saved.instruments[i].symbol, saved.instruments[j].symbol) == 0) good = false;
                }
            }
        }
        if (good) {
            snapshot->count = saved.count;
            memcpy(snapshot->instruments, saved.instruments, sizeof(snapshot->instruments));
            LOGI("loaded %lu saved stocks", (unsigned long)snapshot->count);
            return;
        }
        LOGW("invalid saved watchlist; using defaults");
    }
    snapshot->count = 2;
    stock_instrument_from_code("sh688018", &snapshot->instruments[0]);
    stock_instrument_from_code("usNVDA", &snapshot->instruments[1]);
    snprintf(snapshot->instruments[0].name, STOCK_NAME_SIZE, "乐鑫科技");
    snprintf(snapshot->instruments[1].name, STOCK_NAME_SIZE, "英伟达");
}

static void publish(const stock_snapshot_t *snapshot)
{
    taskENTER_CRITICAL();
    s_snapshot = *snapshot;
    taskEXIT_CRITICAL();
}

/* A request started before a voice edit must never restore removed rows. */
static bool publish_poll(const stock_snapshot_t *snapshot)
{
    taskENTER_CRITICAL();
    bool current = s_snapshot.revision == snapshot->revision;
    if (current) s_snapshot = *snapshot;
    taskEXIT_CRITICAL();
    return current;
}

void service_stock_snapshot(stock_snapshot_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL();
    *out = s_snapshot;
    taskEXIT_CRITICAL();
    out->clock_available = clock_now(&out->now_utc);
    for (unsigned i = 0; i < out->count; ++i) {
        out->scheduled[i] = out->clock_available && stock_market_session_scheduled(out->instruments[i].market, out->now_utc);
        out->stale[i] = !out->clock_available || stock_quote_is_stale(&out->quotes[i], out->now_utc);
    }
}

static bool quote_request(const stock_instrument_t *instruments, unsigned count, char *body,
                          size_t *size, char *error, size_t error_size)
{
    char url[192] = "https://qt.gtimg.cn/q=";
    size_t used = strlen(url);
    for (unsigned i = 0; i < count; ++i) {
        int n = snprintf(url + used, sizeof(url) - used, "%s%s", i ? "," : "", instruments[i].symbol);
        if (n < 0 || (size_t)n >= sizeof(url) - used) return false;
        used += (size_t)n;
    }
    return stock_http_get(url, body, STOCK_RESPONSE_MAX + 1, size, error, error_size);
}

static void stock_worker(void *unused)
{
    (void)unused;
    uint32_t wait_seconds = 0, failures = 0, revision = 0;
    bool connected_before = false;
    for (;;) {
        stock_snapshot_t next;
        service_stock_snapshot(&next);
        if (revision != next.revision) {
            revision = next.revision;
            wait_seconds = 0;
            failures = 0;
        }
        bool connected = connected_now();
        next.network_connected = connected;
        bool reconnect = connected && !connected_before;
        connected_before = connected;
        if (!next.count) {
            next.state = STOCK_STATE_READY;
            next.error[0] = '\0';
            next.next_poll_seconds = 0;
            publish_poll(&next);
        } else if (!connected) {
            next.state = STOCK_STATE_ERROR;
            snprintf(next.error, sizeof(next.error), "network_unavailable");
            next.next_poll_seconds = 5;
            publish_poll(&next);
            wait_seconds = 0;
        } else if (wait_seconds && !reconnect) {
            --wait_seconds;
            next.next_poll_seconds = wait_seconds;
            publish_poll(&next);
        } else {
            char *body = lisa_mem_alloc(STOCK_RESPONSE_MAX + 1);
            size_t size = 0;
            bool complete = body && quote_request(next.instruments, next.count, body, &size, next.error, sizeof(next.error));
            if (!body) snprintf(next.error, sizeof(next.error), "no_memory");
            if (complete) {
                for (unsigned i = 0; i < next.count; ++i) {
                    stock_quote_t parsed;
                    if (stock_quote_parse_instrument(body, size, &next.instruments[i], &parsed) &&
                        (!next.quotes[i].valid || parsed.epoch_utc >= next.quotes[i].epoch_utc)) {
                        next.quotes[i] = parsed;
                    } else {
                        complete = false;
                    }
                }
                if (!complete) snprintf(next.error, sizeof(next.error), "invalid_quote");
            }
            lisa_mem_free(body);
            if (complete) {
                next.state = STOCK_STATE_READY;
                next.error[0] = '\0';
                next.clock_available = clock_now(&next.now_utc);
                if (next.clock_available) next.last_success_utc = next.now_utc;
                failures = 0;
                bool active = false;
                for (unsigned i = 0; i < next.count; ++i) {
                    active |= next.clock_available && stock_market_session_scheduled(next.instruments[i].market, next.now_utc);
                }
                wait_seconds = active ? 15 : 60;
            } else {
                next.state = STOCK_STATE_ERROR;
                if (failures < 4) ++failures;
                wait_seconds = 15u << failures;
            }
            next.next_poll_seconds = wait_seconds;
            ++next.sequence;
            if (publish_poll(&next)) {
                if (complete) LOGI("quotes updated: %lu stocks, seq=%lu", (unsigned long)next.count, (unsigned long)next.sequence);
                else LOGW("quote update failed: %s; retry in %lu s", next.error, (unsigned long)wait_seconds);
            }
        }
        lisa_thread_mdelay(1000);
    }
}

static bool equal_fold(const char *left, const char *right)
{
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++, b = (unsigned char)*right++;
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
        if (a != b) return false;
    }
    return *left == *right;
}

static int find_stock(const stock_snapshot_t *snapshot, const char *query, bool partial)
{
    stock_instrument_t code;
    char normalized[STOCK_QUERY_MAX + 1];
    snprintf(normalized, sizeof(normalized), "%s", query);
    if (normalized[0] == 'u' && normalized[1] == 's' && normalized[2] >= 'a' && normalized[2] <= 'z') {
        for (char *p = normalized; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
    }
    bool by_code = stock_instrument_from_code(normalized, &code);
    int match = -1;
    for (unsigned i = 0; i < snapshot->count; ++i) {
        const stock_instrument_t *instrument = &snapshot->instruments[i];
        if ((by_code && strcmp(code.symbol, instrument->symbol) == 0) ||
            equal_fold(query, instrument->symbol + 2)) return (int)i;
    }
    for (unsigned i = 0; i < snapshot->count; ++i) {
        if (equal_fold(query, snapshot->instruments[i].name)) {
            if (match >= 0) return -2;
            match = (int)i;
        }
    }
    if (match >= 0) return match;
    /* Short Chinese company names such as 乐鑫 can match a unique saved full name. */
    if (partial && strlen(query) >= 3) {
        for (unsigned i = 0; i < snapshot->count; ++i) {
            if (strstr(snapshot->instruments[i].name, query)) {
                if (match >= 0) return -2;
                match = (int)i;
            }
        }
    }
    return match;
}

static int list_stocks(const stock_snapshot_t *snapshot, char *text, size_t size)
{
    int written = snprintf(text, size, "当前自选股 %lu/%u 只%s", (unsigned long)snapshot->count,
                           (unsigned)STOCK_WATCH_MAX, snapshot->count ? "：\n" : "，可以说添加苹果股票。");
    if (written < 0 || (size_t)written >= size) return -1;
    size_t used = (size_t)written;
    for (unsigned i = 0; i < snapshot->count; ++i) {
        const stock_instrument_t *instrument = &snapshot->instruments[i];
        const stock_quote_t *quote = &snapshot->quotes[i];
        char price[192];
        if (quote->valid) snprintf(price, sizeof(price), "%.2f %s，%+.2f%%，行情时间 %s %s%s",
            quote->price, instrument->market == STOCK_MARKET_CN ? "元" : "美元", quote->change_pct,
            quote->time_text, instrument->market == STOCK_MARKET_CN ? "北京时间" : "纽约时间",
            snapshot->stale[i] ? "（旧报价或休市报价）" : "");
        else snprintf(price, sizeof(price), "等待行情更新");
        written = snprintf(text + used, size - used, "%u. %s %s：%s\n", i + 1,
                           instrument->name, instrument->symbol, price);
        if (written < 0 || (size_t)written >= size - used) return -1;
        used += (size_t)written;
    }
    written = snprintf(text + used, size - used, "腾讯公开行情，美股可能延时；不提供交易或提醒。");
    return written >= 0 && (size_t)written < size - used ? 0 : -1;
}

int service_stock_manage(const char *action, const char *query, char *text, size_t text_size)
{
    if (!text || !text_size) return -1;
    text[0] = '\0';
    if (!s_initialized || !action) {
        snprintf(text, text_size, "行情服务尚未就绪。");
        return -1;
    }
    if (strcmp(action, "list") == 0) {
        stock_snapshot_t snapshot;
        service_stock_snapshot(&snapshot);
        return list_stocks(&snapshot, text, text_size);
    }
    bool add = strcmp(action, "add") == 0;
    if ((!add && strcmp(action, "remove") != 0) || !query || !query[0] || strlen(query) > STOCK_QUERY_MAX) {
        snprintf(text, text_size, "请提供要添加或删除的股票名称或代码。");
        return -1;
    }
    if (lisa_mutex_lock(s_manage_mutex, 0) != 0) {
        snprintf(text, text_size, "正在处理上一只股票，请稍后再试。");
        return -1;
    }
    int result = -1;
    stock_snapshot_t snapshot;
    service_stock_snapshot(&snapshot);
    int found = find_stock(&snapshot, query, !add);
    if (add && found >= 0) {
        snprintf(text, text_size, "%s %s 已在自选股中，无需重复添加。", snapshot.instruments[found].name, snapshot.instruments[found].symbol);
        result = 0;
        goto done;
    }
    if (!add) {
        if (found < 0) {
            snprintf(text, text_size, "%s", found == -2 ? "匹配到多只自选股，请提供准确股票代码。" : "自选股中未找到这只股票，请核对名称或代码。");
            goto done;
        }
        stock_instrument_t removed = snapshot.instruments[found];
        for (unsigned i = (unsigned)found; i + 1 < snapshot.count; ++i) {
            snapshot.instruments[i] = snapshot.instruments[i + 1];
            snapshot.quotes[i] = snapshot.quotes[i + 1];
        }
        --snapshot.count;
        memset(&snapshot.instruments[snapshot.count], 0, sizeof(snapshot.instruments[0]));
        memset(&snapshot.quotes[snapshot.count], 0, sizeof(snapshot.quotes[0]));
        if (!save_watchlist(&snapshot)) {
            snprintf(text, text_size, "保存失败，本次没有删除，请稍后重试。");
            goto done;
        }
        ++snapshot.revision;
        snapshot.state = snapshot.count ? STOCK_STATE_LOADING : STOCK_STATE_READY;
        snapshot.error[0] = '\0';
        snapshot.next_poll_seconds = 0;
        publish(&snapshot);
        snprintf(text, text_size, "已删除 %s %s，剩余 %lu 只；重启后保留。", removed.name, removed.symbol, (unsigned long)snapshot.count);
        result = 0;
        goto done;
    }
    if (snapshot.count >= STOCK_WATCH_MAX) {
        snprintf(text, text_size, "自选股最多 %u 只，请先删除一只再添加。", (unsigned)STOCK_WATCH_MAX);
        goto done;
    }
    if (!connected_now()) {
        snprintf(text, text_size, "网络未连接，无法验证新股票，请联网后再试。");
        goto done;
    }
    stock_instrument_t instrument;
    if (stock_lookup_resolve(query, &instrument, text, text_size) != 0) goto done;
    if (!instrument_valid(&instrument)) {
        snprintf(text, text_size, "股票查询结果无效，请改用准确股票代码。");
        goto done;
    }
    service_stock_snapshot(&snapshot);
    found = find_stock(&snapshot, instrument.symbol, false);
    if (found >= 0) {
        snprintf(text, text_size, "%s %s 已在自选股中，无需重复添加。", snapshot.instruments[found].name, instrument.symbol);
        result = 0;
        goto done;
    }
    char *body = lisa_mem_alloc(STOCK_RESPONSE_MAX + 1);
    stock_quote_t quote;
    size_t size = 0;
    char error[48] = "no_memory";
    bool good = body && quote_request(&instrument, 1, body, &size, error, sizeof(error)) &&
                stock_quote_parse_instrument(body, size, &instrument, &quote);
    lisa_mem_free(body);
    if (!good) {
        snprintf(text, text_size, "没有取得 %s %s 的有效行情，未添加；请核对代码或稍后重试。", instrument.name, instrument.symbol);
        goto done;
    }
    service_stock_snapshot(&snapshot);
    snapshot.instruments[snapshot.count] = instrument;
    snapshot.quotes[snapshot.count] = quote;
    ++snapshot.count;
    if (!save_watchlist(&snapshot)) {
        snprintf(text, text_size, "保存失败，本次没有添加，请稍后重试。");
        goto done;
    }
    ++snapshot.revision;
    snapshot.state = STOCK_STATE_LOADING;
    snapshot.error[0] = '\0';
    snapshot.next_poll_seconds = 0;
    publish(&snapshot);
    snprintf(text, text_size, "已添加 %s %s，共 %lu 只，待机屏双击切换查看分时图；重启后保留。腾讯公开行情，美股可能延时。",
             instrument.name, instrument.symbol, (unsigned long)snapshot.count);
    result = 0;
done:
    lisa_mutex_unlock(s_manage_mutex);
    return result;
}

int service_stock_init(void)
{
    if (s_initialized) return 0;
    s_manage_mutex = lisa_mutex_create();
    if (!s_manage_mutex) return -1;
    stock_snapshot_t initial = {.state = STOCK_STATE_LOADING, .revision = 1};
    load_watchlist(&initial);
    publish(&initial);
    lisa_thread_attr_t attr = {.name = (uint8_t *)"stock.watch", .stack_size = 12 * 1024, .priority = 4};
    if (!lisa_thread_create(&attr, stock_worker, NULL)) {
        initial.state = STOCK_STATE_ERROR;
        snprintf(initial.error, sizeof(initial.error), "worker_unavailable");
        publish(&initial);
        lisa_mutex_delete(s_manage_mutex);
        s_manage_mutex = NULL;
        return -1;
    }
    s_initialized = true;
    LOGI("stock display enabled: %lu stocks; voice watchlist; alerts disabled", (unsigned long)initial.count);
    return 0;
}
