#include "stock_chart.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static unsigned checks;
#define CHECK(value) do { ++checks; assert(value); } while (0)

static stock_instrument_t instrument(const char *symbol)
{
    stock_instrument_t result;
    CHECK(stock_instrument_from_code(symbol, &result));
    return result;
}

static void fixture(char *out, size_t capacity, const char *symbol, const char *code,
                    const char *previous_close, const char *date, const char *rows)
{
    int size = snprintf(out, capacity,
        "{\"code\":0,\"msg\":\"\",\"data\":{\"%s\":{"
        "\"data\":{\"data\":[%s],\"date\":\"%s\"},"
        "\"qt\":{\"%s\":[\"delay\",\"股票\",\"%s\",\"110\",\"%s\"]},"
        "\"pandata\":{\"last\":\"999\",\"time\":\"2026-11-06 20:00:00\"}}}}",
        symbol, rows, date, symbol, code, previous_close);
    CHECK(size > 0 && (size_t)size < capacity);
}

static void bad(const void *data, size_t size, const stock_instrument_t *stock)
{
    stock_chart_t out, sentinel;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    memcpy(&out, &sentinel, sizeof(out));
    CHECK(!stock_chart_parse(data, size, stock, &out));
    CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0);
}

static stock_chart_t good(const char *data, const stock_instrument_t *stock)
{
    stock_chart_t result;
    CHECK(stock_chart_parse(data, strlen(data), stock, &result));
    CHECK(result.valid && result.market == stock->market && strcmp(result.symbol, stock->symbol) == 0);
    CHECK(result.count > 0 && result.count <= STOCK_CHART_MAX_POINTS);
    for (unsigned i = 0; i < result.count; ++i) {
        CHECK(isfinite(result.points[i].price) && result.points[i].price > 0);
        CHECK(i == 0 || result.points[i].minute > result.points[i - 1].minute);
    }
    return result;
}

static void replace_once(char *out, size_t capacity, const char *data,
                         const char *search, const char *replacement)
{
    const char *found = strstr(data, search);
    CHECK(found != NULL);
    size_t prefix = (size_t)(found - data), size = strlen(replacement);
    CHECK(prefix + size + strlen(found + strlen(search)) < capacity);
    memcpy(out, data, prefix);
    memcpy(out + prefix, replacement, size);
    strcpy(out + prefix + size, found + strlen(search));
}

static void session_cases(void)
{
    stock_instrument_t cn = instrument("sh688018"), us = instrument("usNVDA");
    char data[2048];
    const char *rows = "\"0000 90 0\",\"0929 91 1\",\"0930 100 2\",\"1007 101 3\","
        "\"1130 102 4\",\"1131 103 5\",\"1259 104 6\",\"1300 105 7\","
        "\"1500 106 8\",\"1501 107 9\",\"1505 108 10\",\"1530 109 11\","
        "\"1600 110 12\",\"1601 111 13\"";
    fixture(data, sizeof(data), "sh688018", "688018", "99.25", "20261106", rows);
    stock_chart_t result = good(data, &cn);
    CHECK(strcmp(result.date, "2026-11-06") == 0 && result.previous_close == 99.25f);
    CHECK(result.count == 5 && result.points[0].minute == 570 && result.points[4].minute == 900);
    CHECK(result.points[1].minute == 607); /* No interpolation of absent minutes. */
    CHECK(result.points[2].minute == 690 && result.points[3].minute == 780); /* Lunch gap retained. */
    CHECK(result.points[4].price == 106); /* Ignore after-hours pandata and CN closing extensions. */
    fixture(data, sizeof(data), "usNVDA", "NVDA.OQ", "99.25", "20261106", rows);
    result = good(data, &us);
    CHECK(result.count == 11 && result.points[0].minute == 570 && result.points[10].minute == 960);
    fixture(data, sizeof(data), "usNVDA", "NVDA.OQ", "99.25", "20000101", "\"0937 100 0\"");
    result = good(data, &us);
    CHECK(result.count == 1 && result.points[0].minute == 577 && strcmp(result.date, "2000-01-01") == 0);
    fixture(data, sizeof(data), "usNVDA", "NVDA.OQ", "99.25", "20261106", "");
    bad(data, strlen(data), &us);
    fixture(data, sizeof(data), "sh688018", "688018", "99.25", "20261106", "\"1505 100 0\",\"1530 101 1\"");
    bad(data, strlen(data), &cn);
}

static void date_cases(void)
{
    stock_instrument_t cn = instrument("sh688018");
    char data[2048];
    const char *invalid[] = {"", "2026110", "202611060", "2026-11-06", "2026xx06", "00000101",
        "20260001", "20261301", "20260100", "20260431", "20260229", "19000229", "21000229", "20260132"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        fixture(data, sizeof(data), "sh688018", "688018", "100", invalid[i], "\"0930 110 1\"");
        bad(data, strlen(data), &cn);
    }
    const char *valid[] = {"20000229", "20240229", "19000301", "00010101", "99991231"};
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        fixture(data, sizeof(data), "sh688018", "688018", "100", valid[i], "\"0930 110 1\"");
        (void)good(data, &cn);
    }
}

static void point_cases(void)
{
    stock_instrument_t cn = instrument("sh688018");
    char data[2048], rows[256];
    const char *numbers[] = {"0", "-1", "nan", "NaN", "inf", "Infinity", "1e2", "1e309", "1000000001",
        "1.", ".5", "1.2.3", "--1", "1x", "", "999999999999999999999999999999999"};
    for (unsigned i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
        snprintf(rows, sizeof(rows), "\"0930 %s 1\"", numbers[i]);
        fixture(data, sizeof(data), "sh688018", "688018", "100", "20261106", rows);
        bad(data, strlen(data), &cn);
        fixture(data, sizeof(data), "sh688018", "688018", numbers[i], "20261106", "\"0930 110 1\"");
        bad(data, strlen(data), &cn);
    }
    const char *bad_rows[] = {"\"093 100 1\"", "\"09300 100 1\"", "\"0960 100 1\"", "\"2400 100 1\"",
        "\"-930 100 1\"", "\"09a0 100 1\"", "\"0930 100\"", "\"0930 100 NaN\"", "\"0930 100 -1\"",
        "\"0930 100 1 2 3\"", "null", "123", "{}", "[]", "\"0930 100 1\",\"0930 101 2\"",
        "\"0931 100 1\",\"0930 101 2\"", "\"0930 100 1\",\"0929 101 2\"",
        "\"0930 100 1\",\"1530 101 2\",\"1530 102 3\""};
    for (unsigned i = 0; i < sizeof(bad_rows) / sizeof(bad_rows[0]); ++i) {
        fixture(data, sizeof(data), "sh688018", "688018", "100", "20261106", bad_rows[i]);
        bad(data, strlen(data), &cn);
    }
    fixture(data, sizeof(data), "sh688018", "688018", "1000000000", "20261106", "\"0930 0.000001 0 0.00\"");
    stock_chart_t result = good(data, &cn);
    CHECK(result.previous_close == 1000000000.0f && result.points[0].price > 0);
    fixture(data, sizeof(data), "sh688018", "688018", "100", "20261106", "\"0930 1000000000 0\"");
    result = good(data, &cn);
    CHECK(result.points[0].price == 1000000000.0f);
    fixture(data, sizeof(data), "sh688018", "688018", "100", "20261106", "\"0930\\t100.5  1  2.25  \"");
    (void)good(data, &cn);
}

static void identity_cases(void)
{
    char data[2048], changed[4096];
    stock_instrument_t us = instrument("usBRK.B");
    const char *valid_codes[] = {"BRK.B", "BRK.B.OQ", "BRK.B.N", "BRK.B.AM", "BRK.B.O", "BRK.B.A", "BRK.B.P", "BRK.B.NQ"};
    for (unsigned i = 0; i < sizeof(valid_codes) / sizeof(valid_codes[0]); ++i) {
        fixture(data, sizeof(data), "usBRK.B", valid_codes[i], "100", "20261106", "\"0930 110 1\"");
        (void)good(data, &us);
    }
    const char *invalid_codes[] = {"BRK", "BRK.BX", "BRK.B.NQX", "BRK.B.X", "BRK.B.N.X", "brk.b.N"};
    for (unsigned i = 0; i < sizeof(invalid_codes) / sizeof(invalid_codes[0]); ++i) {
        fixture(data, sizeof(data), "usBRK.B", invalid_codes[i], "100", "20261106", "\"0930 110 1\"");
        bad(data, strlen(data), &us);
    }
    fixture(data, sizeof(data), "usBRK.B", "BRK.B.N", "100", "20261106", "\"0930 110 1\"");
    stock_instrument_t wrong = instrument("usBRK");
    bad(data, strlen(data), &wrong);
    wrong = us; wrong.market = STOCK_MARKET_CN;
    bad(data, strlen(data), &wrong);
    wrong = us; memset(wrong.symbol, 'A', sizeof(wrong.symbol));
    bad(data, strlen(data), &wrong);
    wrong = us; strcpy(wrong.symbol, "usbrk.b");
    bad(data, strlen(data), &wrong);
    stock_instrument_t cn = instrument("sh688018");
    fixture(data, sizeof(data), "sh688018", "688019", "100", "20261106", "\"0930 110 1\"");
    bad(data, strlen(data), &cn);
    fixture(data, sizeof(data), "sh688018", "688018", "100", "20261106", "\"0930 110 1\"");
    const char *search[] = {"\"code\":0", "\"code\":0", "\"code\":0", "\"code\":0", "\"data\":{\"sh688018\"",
        "\"sh688018\":", "\"qt\":", "\"date\":", "\"data\":[", "\"qt\":{\"sh688018\":",
        "\"date\":\"20261106\"", "\"qt\":{", "\"data\":[", "\"688018\"", "\"100\""};
    const char *replacement[] = {"\"code\":1", "\"code\":\"0\"", "\"code\":false", "\"code\":0,\"code\":0",
        "\"data\":{},\"data\":{\"sh688018\"", "\"sh688018\":{},\"sh688018\":", "\"qt\":{},\"qt\":",
        "\"date\":\"20261106\",\"date\":", "\"data\":[],\"data\":[", "\"qt\":{\"sh688018\":[],\"sh688018\":",
        "\"date\":20261106", "\"qt\":null,\"ignored\":{", "\"data\":null,\"ignored\":[", "688018", "100"};
    for (unsigned i = 0; i < sizeof(search) / sizeof(search[0]); ++i) {
        replace_once(changed, sizeof(changed), data, search[i], replacement[i]);
        bad(changed, strlen(changed), &cn);
    }
    /* Exact symbol lookup survives an unrelated, longer prefix match. */
    replace_once(changed, sizeof(changed), data, "\"data\":{\"sh688018\"", "\"data\":{\"sh6880180\":{},\"sh688018\"");
    (void)good(changed, &cn);
    /* Unicode-escaped keys are decoded before identity/duplicate checks. */
    replace_once(changed, sizeof(changed), data, "\"code\":0", "\"code\":0,\"co\\u0064e\":0");
    bad(changed, strlen(changed), &cn);
}

static void json_cases(void)
{
    stock_instrument_t cn = instrument("sh688018");
    char data[2048], changed[4096];
    fixture(data, sizeof(data), "sh688018", "688018", "100", "20261106", "\"0930 110 1\"");
    size_t size = strlen(data);
    bad(NULL, size, &cn);
    bad(data, 0, &cn);
    bad(data, size, NULL);
    CHECK(!stock_chart_parse(data, size, &cn, NULL));
    char *oversized = malloc(STOCK_RESPONSE_MAX + 1);
    CHECK(oversized != NULL);
    memset(oversized, ' ', STOCK_RESPONSE_MAX + 1);
    bad(oversized, STOCK_RESPONSE_MAX + 1, &cn);
    memcpy(oversized, data, size);
    CHECK(stock_chart_parse(oversized, STOCK_RESPONSE_MAX, &cn, &(stock_chart_t){0}));
    free(oversized);
    for (size_t i = 0; i < size; ++i) bad(data, i, &cn);
    snprintf(changed, sizeof(changed), " \t\r\n%s \r\n", data);
    (void)good(changed, &cn);
    const char *search[] = {"\"code\":0", "\"code\":0", "\"code\":0", "\"code\":0", "\"code\":0",
        "\"code\":0", "\"code\":0", "\"股票\"", "\"股票\"", "\"股票\"", "\"股票\"", "\"股票\"",
        "\"股票\"", "\"股票\"", "\"股票\"", "\"股票\"", "\"股票\"", "\"股票\""};
    const char *replacement[] = {"\"code\":00", "\"code\":+0", "\"code\":.0", "\"code\":0.", "\"code\":0e",
        "\"code\":NaN", "\"code\":0,", "\"a\n\"", "\"\\x20\"", "\"\\u0000\"", "\"\\ud800\"", "\"\\udc00\"",
        "\"\\ud800\\ud800\"", "\"\xc0\xaf\"", "\"\xed\xa0\x80\"", "\"\xf4\x90\x80\x80\"", "\"\x80\"", "\"\xe4\xb8\""};
    for (unsigned i = 0; i < sizeof(search) / sizeof(search[0]); ++i) {
        replace_once(changed, sizeof(changed), data, search[i], replacement[i]);
        bad(changed, strlen(changed), &cn);
    }
    replace_once(changed, sizeof(changed), data, "\"股票\"", "\"\\ud83d\\ude00\"");
    (void)good(changed, &cn);
    snprintf(changed, sizeof(changed), "%s null", data);
    bad(changed, strlen(changed), &cn);
    snprintf(changed, sizeof(changed), "%s%s", data, data);
    bad(changed, strlen(changed), &cn);
    memcpy(changed, data, size); changed[size / 2] = 0;
    bad(changed, size, &cn);
    /* cJSON's 1000-level default is too deep for an RTOS task stack. */
    char nested[256];
    memset(nested, '[', 80); memset(nested + 80, ']', 80); nested[160] = 0;
    replace_once(changed, sizeof(changed), data, "\"msg\":\"\"", "\"msg\":null");
    char expression[300]; snprintf(expression, sizeof(expression), "\"msg\":%s", nested);
    replace_once(changed, sizeof(changed), data, "\"msg\":\"\"", expression);
    bad(changed, strlen(changed), &cn);
}

static void bounds_cases(void)
{
    stock_instrument_t us = instrument("usNVDA");
    char data[STOCK_RESPONSE_MAX], rows[12000];
    for (unsigned limit = STOCK_CHART_MAX_POINTS; limit <= STOCK_CHART_MAX_POINTS + 1; ++limit) {
        size_t pos = 0;
        for (unsigned i = 0; i < limit; ++i) {
            unsigned minute = 550 + i;
            int used = snprintf(rows + pos, sizeof(rows) - pos, "%s\"%02u%02u 100 1\"",
                i ? "," : "", minute / 60, minute % 60);
            CHECK(used > 0 && (size_t)used < sizeof(rows) - pos);
            pos += (size_t)used;
        }
        fixture(data, sizeof(data), "usNVDA", "NVDA.OQ", "100", "20261106", rows);
        if (limit == STOCK_CHART_MAX_POINTS) {
            stock_chart_t result = good(data, &us);
            CHECK(result.count == 391 && result.points[0].minute == 570 && result.points[390].minute == 960);
        } else bad(data, strlen(data), &us);
    }
}

static size_t allocation_index, fail_allocation, live_allocations;

static void *test_malloc(size_t size)
{
    if (++allocation_index == fail_allocation) return NULL;
    void *result = malloc(size);
    if (result) ++live_allocations;
    return result;
}

static void test_free(void *ptr)
{
    if (ptr) { CHECK(live_allocations > 0); --live_allocations; free(ptr); }
}

static void allocation_cases(void)
{
    stock_instrument_t cn = instrument("sh688018");
    char data[2048];
    fixture(data, sizeof(data), "sh688018", "688018", "100", "20261106", "\"0930 110 1\"");
    cJSON_Hooks hooks = {.malloc_fn = test_malloc, .free_fn = test_free};
    cJSON_InitHooks(&hooks);
    allocation_index = 0; fail_allocation = (size_t)-1;
    (void)good(data, &cn);
    size_t total = allocation_index;
    CHECK(total > 0 && live_allocations == 0);
    for (fail_allocation = 1; fail_allocation <= total; ++fail_allocation) {
        allocation_index = 0;
        bad(data, strlen(data), &cn);
        CHECK(live_allocations == 0);
    }
    cJSON_InitHooks(NULL);
}

static void mutation_cases(void)
{
    stock_instrument_t cn = instrument("sh688018");
    char original[2048], changed[2048];
    fixture(original, sizeof(original), "sh688018", "688018", "100", "20261106",
        "\"0930 110 1\",\"1130 111 2\",\"1300 112 3\",\"1500 113 4\"");
    size_t size = strlen(original);
    unsigned random = 0x12345678u;
    for (unsigned i = 0; i < 3000; ++i) {
        memcpy(changed, original, size);
        random = random * 1664525u + 1013904223u;
        size_t pos = random % size;
        random = random * 1664525u + 1013904223u;
        changed[pos] = (char)(random >> 24);
        stock_chart_t result, sentinel;
        memset(&sentinel, 0xa5, sizeof(sentinel)); memcpy(&result, &sentinel, sizeof(result));
        if (stock_chart_parse(changed, size, &cn, &result)) {
            CHECK(result.valid && result.count > 0 && result.count <= 4 && strcmp(result.symbol, cn.symbol) == 0);
            for (unsigned j = 0; j < result.count; ++j) {
                CHECK(isfinite(result.points[j].price) && result.points[j].price > 0);
                CHECK(j == 0 || result.points[j].minute > result.points[j - 1].minute);
            }
        } else CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
    }
}

static void real_cases(const char *directory)
{
    static const struct {
        const char *file, *symbol;
        unsigned count;
        float previous_close;
    } cases[] = {
        {"stock-chart-cn.json", "sh688018", 242, 164.47f},
        {"stock-chart-nvda.json", "usNVDA", 391, 181.60f},
        {"stock-chart-aapl.json", "usAAPL", 391, 344.17f},
        {"stock-chart-sz000001.json", "sz000001", 242, 11.64f},
        {"stock-chart-bj920002.json", "bj920002", 242, 49.30f},
        {"stock-chart-sh600519.json", "sh600519", 242, 1452.00f},
        {"stock-chart-usBRK.B.json", "usBRK.B", 391, 572.33f},
        {"stock-chart-us-minute.json", "usNVDA", 0, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char path[512], data[STOCK_RESPONSE_MAX + 1];
        int used = snprintf(path, sizeof(path), "%s/%s", directory, cases[i].file);
        CHECK(used > 0 && (size_t)used < sizeof(path));
        FILE *file = fopen(path, "rb");
        CHECK(file != NULL);
        size_t size = fread(data, 1, sizeof(data) - 1, file);
        CHECK(size > 0 && feof(file));
        fclose(file); data[size] = 0;
        stock_instrument_t stock = instrument(cases[i].symbol);
        if (!cases[i].count) { bad(data, size, &stock); continue; }
        stock_chart_t result = good(data, &stock);
        CHECK(result.count == cases[i].count && strcmp(result.date, "2026-11-06") == 0);
        CHECK(fabsf(result.previous_close - cases[i].previous_close) < 0.001f);
        CHECK(result.points[0].minute == 570 && result.points[result.count - 1].minute ==
            (stock.market == STOCK_MARKET_CN ? 900 : 960));
        for (size_t truncate = 0; truncate < size; truncate += 83) bad(data, truncate, &stock);
    }
}

int main(int argc, char **argv)
{
    session_cases(); date_cases(); point_cases(); identity_cases(); json_cases();
    bounds_cases(); allocation_cases(); mutation_cases();
    if (argc > 1) real_cases(argv[1]);
    printf("stock_chart: %u checks passed%s\n", checks, argc > 1 ? " (including captured source responses)" : "");
    return 0;
}
