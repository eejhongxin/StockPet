#include "stock_quote.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(value) do { ++checks; assert(value); } while (0)

static void fixture(char *buffer, size_t capacity, const char *symbol, const char *code,
                    const char *price, const char *close, const char *change,
                    const char *pct, const char *date)
{
    const char *fields[33];
    for (unsigned i = 0; i < 33; ++i) fields[i] = "0";
    fields[1] = "\xc0\xd6\xf6\xce"; /* GBK 数据不应进入数字解析器。 */
    fields[2] = code;
    fields[3] = price;
    fields[4] = close;
    fields[30] = date;
    fields[31] = change;
    fields[32] = pct;
    int offset = snprintf(buffer, capacity, "v_%s=\"", symbol);
    assert(offset > 0 && (size_t)offset < capacity);
    for (unsigned i = 0; i < 33; ++i) {
        int used = snprintf(buffer + offset, capacity - (size_t)offset, "%s%s", i ? "~" : "", fields[i]);
        assert(used > 0 && (size_t)used < capacity - (size_t)offset);
        offset += used;
    }
    assert(snprintf(buffer + offset, capacity - (size_t)offset, "\";\n") == 3);
}

static stock_quote_t parse(stock_symbol_t symbol, const char *date)
{
    char data[1024];
    fixture(data, sizeof(data), symbol == STOCK_LEXIN ? "sh688018" : "usNVDA",
            symbol == STOCK_LEXIN ? "688018" : "NVDA.OQ", "110", "100", "10", "10", date);
    stock_quote_t out = {0};
    CHECK(stock_quote_parse(data, strlen(data), symbol, &out));
    CHECK(out.valid && out.price == 110 && out.previous_close == 100 && out.change_pct == 10);
    return out;
}

static void parser_cases(void)
{
    stock_quote_t china = parse(STOCK_LEXIN, "20261106100000");
    CHECK(strcmp(china.time_text, "2026-11-06 10:00:00") == 0);
    CHECK(china.epoch_utc == 1793930400LL);
    stock_quote_t america = parse(STOCK_NVDA, "2026-11-06 10:00:00");
    CHECK(america.epoch_utc - china.epoch_utc == 13 * 3600);
    stock_quote_t summer = parse(STOCK_NVDA, "2026-07-06 10:00:00");
    stock_quote_t summer_china = parse(STOCK_LEXIN, "20260706100000");
    CHECK(summer.epoch_utc - summer_china.epoch_utc == 12 * 3600);
    stock_quote_t sentinel = china, out;
    char data[1024], duplicate[2048];
    const char *bad_dates[] = {"20260229100000", "20260431100000", "20261301100000", "20260001100000",
        "20260100100000", "20260101240000", "20260101126000", "20260101125960", "2026110610000",
        "202611061000000", "2026xx06100000", "20060101100000", "21000101100000"};
    for (unsigned i = 0; i < sizeof(bad_dates) / sizeof(bad_dates[0]); ++i) {
        fixture(data, sizeof(data), "sh688018", "688018", "110", "100", "10", "10", bad_dates[i]);
        out = sentinel;
        CHECK(!stock_quote_parse(data, strlen(data), STOCK_LEXIN, &out));
        CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0);
    }
    const char *bad_numbers[] = {"nan", "inf", "1e9", "--1", "+-1", "1.2.3", " 110", "110 ", "", "-1", "0", "1.", "10000000000"};
    for (unsigned i = 0; i < sizeof(bad_numbers) / sizeof(bad_numbers[0]); ++i) {
        fixture(data, sizeof(data), "sh688018", "688018", bad_numbers[i], "100", "10", "10", "20261106100000");
        CHECK(!stock_quote_parse(data, strlen(data), STOCK_LEXIN, &out));
    }
    fixture(data, sizeof(data), "sh688018", "688019", "110", "100", "10", "10", "20261106100000");
    CHECK(!stock_quote_parse(data, strlen(data), STOCK_LEXIN, &out));
    fixture(data, sizeof(data), "sh688018", "688018", "110", "100", "9", "10", "20261106100000");
    CHECK(!stock_quote_parse(data, strlen(data), STOCK_LEXIN, &out));
    fixture(data, sizeof(data), "sh688018", "688018", "110", "100", "10", "9", "20261106100000");
    CHECK(!stock_quote_parse(data, strlen(data), STOCK_LEXIN, &out));
    fixture(data, sizeof(data), "sh688018", "688018", "110", "100", "10", "10", "20261106100000");
    CHECK(!stock_quote_parse(data, strlen(data) - 3, STOCK_LEXIN, &out));
    snprintf(duplicate, sizeof(duplicate), "%s%s", data, data);
    CHECK(!stock_quote_parse(duplicate, strlen(duplicate), STOCK_LEXIN, &out));
    CHECK(!stock_quote_parse(data, strlen(data), STOCK_NVDA, &out));
    CHECK(!stock_quote_parse(NULL, 10, STOCK_LEXIN, &out));
    CHECK(!stock_quote_parse(data, 0, STOCK_LEXIN, &out));
    CHECK(!stock_quote_parse(data, strlen(data), (stock_symbol_t)3, &out));
    size_t size = strlen(data);
    data[15] = 0;
    CHECK(!stock_quote_parse(data, size, STOCK_LEXIN, &out));
    fixture(data, sizeof(data), "sh688018", "688018", "110", "100", "10", "10", "20240229100000");
    CHECK(stock_quote_parse(data, strlen(data), STOCK_LEXIN, &out));
    fixture(data, sizeof(data), "usNVDA", "NVDA.OQ", "110", "100", "10", "10", "2026-03-08 02:30:00");
    CHECK(!stock_quote_parse(data, strlen(data), STOCK_NVDA, &out));
    fixture(data, sizeof(data), "usNVDA", "NVDA.OQ", "110", "100", "10", "10", "2026-11-01 01:30:00");
    CHECK(!stock_quote_parse(data, strlen(data), STOCK_NVDA, &out));
    CHECK(!stock_quote_is_stale(&china, china.epoch_utc + 180));
    CHECK(stock_quote_is_stale(&china, china.epoch_utc + 181));
    CHECK(stock_quote_is_stale(&china, china.epoch_utc - 61));
    CHECK(!stock_quote_is_stale(&china, china.epoch_utc - 60));
    CHECK(stock_quote_is_stale(&china, 0));
}

static void scheduled(stock_symbol_t symbol, const char *date, bool expected)
{
    char expanded[20];
    if (symbol == STOCK_NVDA && strlen(date) == 14) {
        snprintf(expanded, sizeof(expanded), "%.4s-%.2s-%.2s %.2s:%.2s:%.2s", date, date + 4,
                 date + 6, date + 8, date + 10, date + 12);
        date = expanded;
    }
    stock_quote_t quote = parse(symbol, date);
    CHECK(stock_quote_session_scheduled(symbol, quote.epoch_utc) == expected);
}

static void session_cases(void)
{
    scheduled(STOCK_LEXIN, "20261106092959", false);
    scheduled(STOCK_LEXIN, "20261106093000", true);
    scheduled(STOCK_LEXIN, "20261106112959", true);
    scheduled(STOCK_LEXIN, "20261106113000", false);
    scheduled(STOCK_LEXIN, "20261106125959", false);
    scheduled(STOCK_LEXIN, "20261106130000", true);
    scheduled(STOCK_LEXIN, "20261106145959", true);
    scheduled(STOCK_LEXIN, "20261106150000", false);
    scheduled(STOCK_LEXIN, "20261107100000", false);
    scheduled(STOCK_LEXIN, "20261108100000", false);
    scheduled(STOCK_NVDA, "20261106092959", false);
    scheduled(STOCK_NVDA, "20261106093000", true);
    scheduled(STOCK_NVDA, "20261106155959", true);
    scheduled(STOCK_NVDA, "20261106160000", false);
    scheduled(STOCK_NVDA, "20260706100000", true);
    scheduled(STOCK_NVDA, "20261107100000", false);
    scheduled(STOCK_NVDA, "20261108100000", false);
    scheduled(STOCK_NVDA, "20260306100000", true);
    scheduled(STOCK_NVDA, "20260309100000", true);
    scheduled(STOCK_NVDA, "20261030100000", true);
    scheduled(STOCK_NVDA, "20261102100000", true);
    CHECK(!stock_quote_session_scheduled(STOCK_NVDA, 0));
}

static void instrument_cases(void)
{
    static const struct {const char *input; const char *symbol; stock_market_t market;} good[] = {
        {"688018", "sh688018", STOCK_MARKET_CN}, {"600519", "sh600519", STOCK_MARKET_CN},
        {"SH600519", "sh600519", STOCK_MARKET_CN}, {"sZ000001", "sz000001", STOCK_MARKET_CN},
        {"300750", "sz300750", STOCK_MARKET_CN}, {"430047", "bj430047", STOCK_MARKET_CN},
        {"830799", "bj830799", STOCK_MARKET_CN}, {"920002", "bj920002", STOCK_MARKET_CN},
        {"bj920002", "bj920002", STOCK_MARKET_CN}, {"aapl", "usAAPL", STOCK_MARKET_US},
        {"usmsft", "usMSFT", STOCK_MARKET_US}, {"BRK.B", "usBRK.B", STOCK_MARKET_US},
        {"usBRK.B", "usBRK.B", STOCK_MARKET_US}, {"USB", "usUSB", STOCK_MARKET_US},
        {"USO", "usUSO", STOCK_MARKET_US}, {"USFD", "usUSFD", STOCK_MARKET_US},
        {"SHOP", "usSHOP", STOCK_MARKET_US}, {"SHW", "usSHW", STOCK_MARKET_US},
        {"SH", "usSH", STOCK_MARKET_US}, {"usV", "usV", STOCK_MARKET_US},
        {"usA1", "usA1", STOCK_MARKET_US},
    };
    stock_instrument_t instrument;
    memset(&instrument, 0x5a, sizeof(instrument));
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        CHECK(stock_instrument_from_code(good[i].input, &instrument));
        CHECK(strcmp(instrument.symbol, good[i].symbol) == 0 && instrument.market == good[i].market);
    }
    stock_instrument_t sentinel = instrument;
    const char *bad[] = {"", " ", "sh60051", "sh6005199", "sz00000X", "200001", "100001", "99999",
        "us", "us.A", "usA.", "usA..B", "usBRK-B", "usA/B", "A;reboot", "a,b", "A B", "A\tB", "A\nB",
        "https://x", "../../", "usABCDEFGHIJK", "123456789012345", "1234567890123456", "英伟达"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        instrument = sentinel;
        CHECK(!stock_instrument_from_code(bad[i], &instrument));
        CHECK(memcmp(&instrument, &sentinel, sizeof(instrument)) == 0);
    }
    CHECK(!stock_instrument_from_code(NULL, &instrument));
    CHECK(!stock_instrument_from_code("AAPL", NULL));
}

static void generic_cases(void)
{
    static const char *const symbols[] = {"sh600519", "sz000001", "bj920002", "usAAPL", "usV", "usBRK.B"};
    static const char *const codes[] = {"600519", "000001", "920002", "AAPL.OQ", "V.N", "BRK.B.N"};
    char body[8192] = "", row[1024];
    stock_instrument_t instruments[STOCK_WATCH_MAX];
    for (unsigned i = 0; i < STOCK_WATCH_MAX; ++i) {
        CHECK(stock_instrument_from_code(symbols[i], &instruments[i]));
        snprintf(instruments[i].name, sizeof(instruments[i].name), "Test %u", i);
        fixture(row, sizeof(row), symbols[i], codes[i], "110", "100", "10", "10",
                instruments[i].market == STOCK_MARKET_CN ? "20261106100000" : "2026-11-06 10:00:00");
        strcat(body, row);
    }
    stock_quote_t quote = {0};
    for (unsigned i = 0; i < STOCK_WATCH_MAX; ++i) {
        CHECK(stock_quote_parse_instrument(body, strlen(body), &instruments[i], &quote));
        CHECK(quote.valid && quote.price == 110 && strcmp(quote.name, instruments[i].name) == 0);
        CHECK(stock_market_session_scheduled(instruments[i].market, quote.epoch_utc));
    }
    stock_instrument_t apple = instruments[3];
    stock_quote_t sentinel = quote;
    const char *bad_codes[] = {"AAPLX.OQ", "AAPL.X", "AAPL.OQ.X", "APPLE", "aapl.OQ", "AAPL.NQX"};
    for (unsigned i = 0; i < sizeof(bad_codes) / sizeof(bad_codes[0]); ++i) {
        fixture(row, sizeof(row), "usAAPL", bad_codes[i], "110", "100", "10", "10", "2026-11-06 10:00:00");
        quote = sentinel;
        CHECK(!stock_quote_parse_instrument(row, strlen(row), &apple, &quote));
        CHECK(memcmp(&quote, &sentinel, sizeof(quote)) == 0);
    }
    const char *venue_codes[] = {"AAPL", "AAPL.OQ", "AAPL.N", "AAPL.AM", "AAPL.O", "AAPL.A", "AAPL.P", "AAPL.NQ"};
    for (unsigned i = 0; i < sizeof(venue_codes) / sizeof(venue_codes[0]); ++i) {
        fixture(row, sizeof(row), "usAAPL", venue_codes[i], "110", "100", "10", "10", "2026-11-06 10:00:00");
        CHECK(stock_quote_parse_instrument(row, strlen(row), &apple, &quote));
    }
    /* Other identifiers sharing a prefix must not shadow a legitimate target. */
    fixture(body, sizeof(body), "usAAPLX", "AAPLX.OQ", "110", "100", "10", "10", "2026-11-06 10:00:00");
    fixture(row, sizeof(row), "usAAPL", "AAPL.OQ", "110", "100", "10", "10", "2026-11-06 10:00:00");
    strcat(body, row);
    CHECK(stock_quote_parse_instrument(body, strlen(body), &apple, &quote));
    strcat(body, row);
    CHECK(!stock_quote_parse_instrument(body, strlen(body), &apple, &quote));
    CHECK(!stock_quote_parse_instrument(row, strlen(row) - 3, &apple, &quote));
    stock_instrument_t broken = apple;
    broken.market = STOCK_MARKET_CN;
    CHECK(!stock_quote_parse_instrument(row, strlen(row), &broken, &quote));
    broken = apple;
    memset(broken.symbol, 'A', sizeof(broken.symbol));
    CHECK(!stock_quote_parse_instrument(row, strlen(row), &broken, &quote));
    broken = apple;
    memset(broken.name, 'A', sizeof(broken.name));
    CHECK(!stock_quote_parse_instrument(row, strlen(row), &broken, &quote));
    CHECK(!stock_quote_parse_instrument(row, strlen(row), NULL, &quote));
    CHECK(!stock_market_session_scheduled((stock_market_t)3, 1793930400LL));
    /* Response limit expanded for six rows; over-limit input is rejected first. */
    char *large = malloc(STOCK_RESPONSE_MAX + 1);
    CHECK(large != NULL);
    memset(large, ' ', STOCK_RESPONSE_MAX + 1);
    memcpy(large, row, strlen(row));
    CHECK(stock_quote_parse_instrument(large, STOCK_RESPONSE_MAX, &apple, &quote));
    CHECK(!stock_quote_parse_instrument(large, STOCK_RESPONSE_MAX + 1, &apple, &quote));
    free(large);
}

static void live_fixture(const char *path)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    char data[8193];
    size_t size = fread(data, 1, sizeof(data), file);
    CHECK(fclose(file) == 0);
    CHECK(size > 0 && size <= 8192);
    stock_quote_t quote;
    CHECK(stock_quote_parse(data, size, STOCK_LEXIN, &quote));
    CHECK(strcmp(quote.name, "乐鑫科技") == 0);
    CHECK(stock_quote_parse(data, size, STOCK_NVDA, &quote));
    CHECK(strcmp(quote.name, "NVDA") == 0);
    /* 对真实响应逐字节变异，配合 ASan/UBSan 检查所有边界路径。 */
    for (size_t i = 0; i < size; ++i) {
        char old = data[i];
        for (unsigned j = 0; j < 8; ++j) {
            data[i] = (char)(1u << j);
            (void)stock_quote_parse(data, size, STOCK_LEXIN, &quote);
            (void)stock_quote_parse(data, size, STOCK_NVDA, &quote);
        }
        data[i] = old;
    }
    for (size_t i = 0; i < size; ++i) {
        (void)stock_quote_parse(data, i, STOCK_LEXIN, &quote);
        (void)stock_quote_parse(data, i, STOCK_NVDA, &quote);
    }
}

static void generic_live_fixture(const char *path)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    char *data = malloc(STOCK_RESPONSE_MAX + 1);
    CHECK(data != NULL);
    size_t size = fread(data, 1, STOCK_RESPONSE_MAX + 1, file);
    CHECK(fclose(file) == 0);
    CHECK(size > 0 && size <= STOCK_RESPONSE_MAX);
    const char *symbols[] = {"sh600519", "sz000001", "bj920002", "usAAPL", "usV", "usBRK.B", "usSPY"};
    for (unsigned k = 0; k < sizeof(symbols) / sizeof(symbols[0]); ++k) {
        stock_instrument_t instrument;
        stock_quote_t quote;
        CHECK(stock_instrument_from_code(symbols[k], &instrument));
        CHECK(stock_quote_parse_instrument(data, size, &instrument, &quote));
        CHECK(quote.valid && quote.price > 0);
        for (size_t i = 0; i < size; ++i) {
            char old = data[i];
            data[i] = (char)0xff;
            (void)stock_quote_parse_instrument(data, size, &instrument, &quote);
            data[i] = old;
            (void)stock_quote_parse_instrument(data, i, &instrument, &quote);
        }
    }
    free(data);
}

int main(int argc, char **argv)
{
    parser_cases();
    session_cases();
    instrument_cases();
    generic_cases();
    if (argc > 1) live_fixture(argv[1]);
    if (argc > 2) generic_live_fixture(argv[2]);
    printf("Stock parser/session checks passed: %u; live fixture mutation checks: %s\n", checks, argc > 1 ? "passed" : "not requested");
    return 0;
}
