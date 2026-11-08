#include "stock_lookup.h"
#include "stock_http.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static const char *http_response;
static char last_url[512];

/* Host substitutes for platform allocation and the separately verified HTTP I/O. */
void *lisa_mem_alloc(uint32_t size) { return malloc(size); }
void lisa_mem_free(void *pointer) { free(pointer); }
bool stock_http_get(const char *url, char *body, size_t capacity, size_t *body_size,
                    char *error, size_t error_size)
{
    snprintf(last_url, sizeof(last_url), "%s", url);
    if (!http_response) {
        snprintf(error, error_size, "test_network_unavailable");
        return false;
    }
    *body_size = strlen(http_response);
    assert(*body_size < capacity);
    memcpy(body, http_response, *body_size + 1);
    return true;
}

static const char apple[] = "v_hint=\"us~aapl.oq~\\u82f9\\u679c~pg~GP^us~aapy.am~\\u82f9\\u679cETF~pgetf~GP\"";
static const char lexin[] = "v_hint=\"sh~688018~\\u4e50\\u946b\\u79d1\\u6280~lxkj~GP-A-KCB^jj~006135~bond~cjlx~KJ-CX\"";
static const char ambiguous[] = "v_hint=\"sz~000001~平安银行~payh~GP-A^sz~001359~平安电工~padg~GP-A\"";
static const char pingan[] = "v_hint=\"sh~601318~中国平安~zgpa~GP-A^hk~02318~中国平安~zgpa~GP^us~pngay.ps~中国平安~zgpa~GP\"";
static const char google[] = "v_hint=\"us~goog.oq~google~google~GP^us~googl.oq~google~google~GP\"";

static void expect_result(const void *data, size_t size, const char *query, int expected, const char *symbol)
{
    stock_instrument_t result;
    memset(&result, 0x5a, sizeof(result));
    stock_instrument_t before = result;
    char message[512];
    int code = stock_lookup_parse(data, size, query, &result, message, sizeof(message));
    assert(code == expected);
    assert(message[0]);
    if (expected == 0) assert(strcmp(result.symbol, symbol) == 0);
    else assert(memcmp(&before, &result, sizeof(result)) == 0);
    ++checks;
}

static void expect_parse(const char *response, const char *query, int expected, const char *symbol)
{
    expect_result(response, strlen(response), query, expected, symbol);
}

int main(int argc, char **argv)
{
    expect_parse(apple, "苹果", 0, "usAAPL");
    expect_parse(apple, "aapl", 0, "usAAPL");
    expect_parse(apple, "usAAPL", 0, "usAAPL");
    expect_parse(apple, "AAPY", 0, "usAAPY");
    expect_parse(lexin, "乐鑫", 0, "sh688018");
    expect_parse(lexin, "688018", 0, "sh688018");
    expect_parse(lexin, "  乐鑫科技  ", 0, "sh688018");
    expect_parse(ambiguous, "平安", -2, NULL);
    expect_parse(ambiguous, "平安银行", 0, "sz000001");
    expect_parse(pingan, "中国平安", -2, NULL);
    expect_parse(pingan, "sh601318", 0, "sh601318");
    expect_parse(google, "google", -2, NULL);
    expect_parse(google, "GOOG", 0, "usGOOG");
    expect_parse(google, "GOOGL", 0, "usGOOGL");
    expect_parse("v_hint=\"us~brk.b.n~伯克希尔b~bkxeb~GP\"", "BRK.B", 0, "usBRK.B");
    expect_parse("v_hint=\"us~usb.n~美国合众银行~mghzyh~GP\"", "usb", 0, "usUSB");
    expect_parse("v_hint=\"us~usb.n~美国合众银行~mghzyh~GP\"", "USB", 0, "usUSB");
    expect_parse("v_hint=\"N\";", "不存在", -1, NULL);
    expect_parse(" \n v_hint=\"N\"; \r\n", "unknown", -1, NULL);
    expect_parse("v_hint=\"\"", "unknown", -1, NULL);
    expect_parse("v_hint=\"hk~00700~腾讯控股~txkg~GP\"", "腾讯", -1, NULL);
    expect_parse("v_hint=\"sh~000001~上证指数~szzs~ZS\"", "上证", -1, NULL);
    expect_parse("v_hint=\"sh~510300~沪深300ETF~hs300~ETF\"", "沪深300", -1, NULL);
    expect_parse("v_hint=\"us~aapl.oq~苹果~pg~GP^us~aapl.oq~apple~pg~GP\"", "苹果", 0, "usAAPL");
    expect_parse("v_hint=\"us~aapl.oq~苹果~pg~GP^\"", "苹果", -2, NULL);
    expect_parse("v_hint=\"sh~600519~贵州茅台~gzmt~GP-AX\"", "600519", -1, NULL);
    expect_parse("v_hint=\"us~aa.unknown~a~a~GP\"", "AA", -1, NULL);
    expect_parse("v_hint=\"us~../AAPL.oq~a~a~GP\"", "AAPL", -1, NULL);
    expect_parse("v_hint=\"us~aapl.oq~a~a~GP~extra\"", "AAPL", -2, NULL);
    expect_parse("v_hint=\"us~aapl.oq~a\"", "AAPL", -2, NULL);
    expect_parse("v_hint=\"us~aapl.oq~\\u0000apple~a~GP\"", "AAPL", -2, NULL);
    expect_parse("v_hint=\"us~aapl.oq~\\uD800~a~GP\"", "AAPL", -2, NULL);
    expect_parse("v_hint=\"us~aapl.oq~\\n~a~GP\"", "AAPL", -1, NULL);
    expect_parse("v_hint=\"us~aapl.oq~\xC0\xAF~a~GP\"", "AAPL", -1, NULL);
    expect_parse("v_hint=\"N\";garbage", "AAPL", -2, NULL);
    expect_parse("<html>down</html>", "AAPL", -2, NULL);
    expect_parse("[]", "AAPL", -2, NULL);
    expect_parse("v_hint={}", "AAPL", -2, NULL);
    expect_parse(apple, "", -1, NULL);
    expect_parse(apple, " \t\n", -1, NULL);
    expect_parse(apple, "苹果\n", 0, "usAAPL");
    expect_parse(apple, "苹\n果", -1, NULL);
    char limit[81]; memset(limit, 'a', 80); limit[80] = '\0';
    char full_response[256];
    snprintf(full_response, sizeof(full_response), "v_hint=\"us~aapl.oq~%s~a~GP\"", limit);
    expect_parse(full_response, limit, 0, "usAAPL");
    char oversized[82]; memset(oversized, 'a', 81); oversized[81] = '\0';
    expect_parse(apple, oversized, -1, NULL);
    const char nul[] = "v_hint=\"N\"\0trailing";
    expect_result(nul, sizeof(nul) - 1, "AAPL", -1, NULL);
    expect_result(NULL, 10, "AAPL", -1, NULL);
    expect_result(apple, 8193, "AAPL", -1, NULL);
    char message[512];
    stock_instrument_t result;
    assert(stock_lookup_parse(apple, strlen(apple), "AAPL", NULL, message, sizeof(message)) == -1); ++checks;
    http_response = apple;
    assert(stock_lookup_resolve("苹果", &result, message, sizeof(message)) == 0);
    assert(strcmp(result.symbol, "usAAPL") == 0);
    assert(strcmp(result.name, "苹果") == 0);
    assert(strstr(last_url, "q=%E8%8B%B9%E6%9E%9C")); ++checks;
    assert(stock_lookup_resolve("usAAPL", &result, message, sizeof(message)) == 0);
    assert(strstr(last_url, "q=AAPL")); ++checks;
    http_response = "v_hint=\"us~usb.n~美国合众银行~a~GP\"";
    assert(stock_lookup_resolve("usb", &result, message, sizeof(message)) == 0);
    assert(strstr(last_url, "q=usb"));
    assert(strcmp(result.symbol, "usUSB") == 0); ++checks;
    http_response = lexin;
    assert(stock_lookup_resolve("sh688018", &result, message, sizeof(message)) == 0);
    assert(strstr(last_url, "q=688018")); ++checks;
    http_response = ambiguous;
    assert(stock_lookup_resolve("平安", &result, message, sizeof(message)) == -2); ++checks;
    http_response = google;
    assert(stock_lookup_resolve("google", &result, message, sizeof(message)) == -2); ++checks;
    http_response = NULL;
    assert(stock_lookup_resolve("usb", &result, message, sizeof(message)) == 0);
    assert(strcmp(result.symbol, "usUSB") == 0); ++checks;
    assert(stock_lookup_resolve("苹果", &result, message, sizeof(message)) == -2); ++checks;
    assert(stock_lookup_resolve("TSLA", &result, message, sizeof(message)) == 0);
    assert(strcmp(result.symbol, "usTSLA") == 0); ++checks;
    http_response = "v_hint=\"N\";";
    assert(stock_lookup_resolve("bj920047", &result, message, sizeof(message)) == 0);
    assert(strcmp(result.symbol, "bj920047") == 0); ++checks;
    /* Invalid and truncated data must never read beyond explicit bounds. */
    for (size_t length = 0; length < strlen(apple); ++length) {
        stock_lookup_parse(apple, length, "苹果", &result, message, sizeof(message));
    }
    char changed[sizeof(apple)];
    for (size_t index = 0; index < strlen(apple); ++index) {
        for (unsigned byte = 0; byte < 256; ++byte) {
            memcpy(changed, apple, sizeof(apple)); changed[index] = (char)byte;
            stock_lookup_parse(changed, strlen(apple), "苹果", &result, message, sizeof(message));
        }
    }
    if (argc == 4) {
        FILE *fixture = fopen(argv[1], "rb");
        assert(fixture);
        char body[8193];
        size_t size = fread(body, 1, sizeof(body), fixture);
        assert(!ferror(fixture) && size < sizeof(body));
        fclose(fixture);
        expect_result(body, size, argv[2], 0, argv[3]);
    }
    printf("Stock lookup checks passed: %u; byte mutation/truncation checks passed.\n", checks);
    return 0;
}
