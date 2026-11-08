#include "stock_quote.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 仅解析分隔符和 ASCII 数值；行情源的 GBK 名称不送进 UTF-8 界面。 */
static const char *const instrument_codes[] = {"sh688018", "usNVDA"};
static const char *const instrument_names[] = {"乐鑫科技", "NVDA"};

typedef struct {
    const char *data;
    size_t size;
} span_t;

static bool span_equal(span_t span, const char *value)
{
    size_t size = strlen(value);
    return span.size == size && memcmp(span.data, value, size) == 0;
}

static bool ascii_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char ascii_upper(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

static char ascii_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

bool stock_instrument_from_code(const char *code, stock_instrument_t *out)
{
    if (!code || !out) return false;
    size_t size = 0;
    while (size < STOCK_SYMBOL_SIZE && code[size]) ++size;
    if (!size || size >= STOCK_SYMBOL_SIZE) return false;
    stock_instrument_t candidate = {0};
    const char *digits = code;
    const char *prefix = NULL;
    char first = ascii_lower(code[0]), second = size > 1 ? ascii_lower(code[1]) : 0;
    if (size > 2 && code[2] >= '0' && code[2] <= '9' &&
        ((first == 's' && (second == 'h' || second == 'z')) || (first == 'b' && second == 'j'))) {
        if (size != 8) return false;
        digits += 2;
        prefix = second == 'h' ? "sh" : second == 'z' ? "sz" : "bj";
    }
    bool numeric_code = true;
    for (const char *p = digits; *p; ++p) {
        if (*p < '0' || *p > '9') numeric_code = false;
    }
    if (prefix || numeric_code) {
        if (strlen(digits) != 6 || !numeric_code) return false;
        if (!prefix) {
            switch (digits[0]) {
            case '6': prefix = "sh"; break;
            case '0': case '3': prefix = "sz"; break;
            case '4': case '8': case '9': prefix = "bj"; break;
            default: return false;
            }
        }
        candidate.market = STOCK_MARKET_CN;
        snprintf(candidate.symbol, sizeof(candidate.symbol), "%s%s", prefix, digits);
        memcpy(candidate.name, digits, 7);
    } else {
        /* Only the explicit provider prefix is lower-case. An uppercase USB is
         * the US Bancorp ticker, not provider prefix us followed by ticker B. */
        const char *ticker = code[0] == 'u' && code[1] == 's' ? code + 2 : code;
        size_t ticker_size = strlen(ticker);
        if (!ticker_size || ticker_size > 10 || !ascii_alpha(ticker[0])) return false;
        unsigned dots = 0;
        candidate.market = STOCK_MARKET_US;
        memcpy(candidate.symbol, "us", 2);
        for (size_t i = 0; i < ticker_size; ++i) {
            char c = ticker[i];
            if (c == '.') {
                if (!i || i + 1 == ticker_size || ++dots > 1) return false;
            } else if (!ascii_alpha(c) && (c < '0' || c > '9')) return false;
            candidate.symbol[i + 2] = ascii_upper(c);
        }
        memcpy(candidate.name, candidate.symbol + 2, ticker_size + 1);
    }
    *out = candidate;
    return true;
}

static bool numeric(span_t span, bool positive, double *out)
{
    char text[32];
    if (!span.size || span.size >= sizeof(text)) return false;
    size_t digits = 0, dots = 0;
    for (size_t i = 0; i < span.size; ++i) {
        char c = span.data[i];
        if (c >= '0' && c <= '9') ++digits;
        else if (c == '.' && ++dots <= 1) continue;
        else if ((c == '+' || c == '-') && i == 0) continue;
        else return false;
    }
    if (!digits || span.data[span.size - 1] == '.') return false;
    memcpy(text, span.data, span.size);
    text[span.size] = '\0';
    double value = strtod(text, NULL);
    if (!isfinite(value) || fabs(value) > 1000000000.0 || (positive && value <= 0)) return false;
    *out = value;
    return true;
}

/* 公历算法不依赖 C 库本地时区和 32 位 time_t。 */
static int64_t civil_days(int year, int month, int day)
{
    year -= month <= 2;
    int era = year / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned mp = (unsigned)(month + (month > 2 ? -3 : 9));
    unsigned doy = (153 * mp + 2) / 5 + (unsigned)day - 1;
    return (int64_t)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

static int weekday(int year, int month, int day)
{
    return (int)((civil_days(year, month, day) + 4) % 7);
}

static int first_sunday(int year, int month)
{
    return 1 + (7 - weekday(year, month, 1)) % 7;
}

static void epoch_parts(int64_t epoch, int *year, int *month, int *day, int *minute, int *wday)
{
    int64_t days = epoch / 86400;
    int seconds = (int)(epoch % 86400);
    int64_t z = days + 719468;
    int era = (int)(z / 146097);
    unsigned doe = (unsigned)(z - (int64_t)era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)mp + (mp < 10 ? 3 : -9);
    *year = y + (*month <= 2);
    *minute = seconds / 60;
    *wday = (int)((days + 4) % 7);
}

static int ny_utc_offset(int64_t epoch)
{
    int year, month, day, minute, wday;
    epoch_parts(epoch, &year, &month, &day, &minute, &wday);
    int64_t begin = civil_days(year, 3, first_sunday(year, 3) + 7) * 86400 + 7 * 3600;
    int64_t end = civil_days(year, 11, first_sunday(year, 11)) * 86400 + 6 * 3600;
    return epoch >= begin && epoch < end ? -4 * 3600 : -5 * 3600;
}

static bool timestamp(span_t span, stock_market_t market, stock_quote_t *quote)
{
    char digits[14];
    size_t count = 0;
    if (market == STOCK_MARKET_CN) {
        if (span.size != 14) return false;
        memcpy(digits, span.data, 14);
    } else {
        if (span.size != 19 || span.data[4] != '-' || span.data[7] != '-' || span.data[10] != ' ' ||
            span.data[13] != ':' || span.data[16] != ':') return false;
        for (size_t i = 0; i < span.size; ++i) {
            if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
            digits[count++] = span.data[i];
        }
    }
    for (size_t i = 0; i < sizeof(digits); ++i) {
        if (digits[i] < '0' || digits[i] > '9') return false;
    }
    int values[6] = {0};
    const unsigned widths[] = {4, 2, 2, 2, 2, 2};
    count = 0;
    for (size_t i = 0; i < 6; ++i) {
        for (unsigned j = 0; j < widths[i]; ++j) values[i] = values[i] * 10 + digits[count++] - '0';
    }
    int year = values[0], month = values[1], day = values[2], hour = values[3];
    int min = values[4], sec = values[5];
    static const unsigned month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 2007 || year > 2099 || month < 1 || month > 12 || day < 1 || hour > 23 || min > 59 || sec > 59) return false;
    unsigned max_day = month_days[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ++max_day;
    if ((unsigned)day > max_day) return false;
    int offset = 8 * 3600;
    if (market == STOCK_MARKET_US) {
        int begin = first_sunday(year, 3) + 7, end = first_sunday(year, 11);
        /* 不猜测夏令时跳过或重复的本地小时。常规盘中时间不落在这些窗口。 */
        if ((month == 3 && day == begin && hour == 2) || (month == 11 && day == end && hour == 1)) return false;
        bool dst = (month > 3 && month < 11) || (month == 3 && (day > begin || (day == begin && hour >= 3))) ||
                   (month == 11 && (day < end || (day == end && hour < 1)));
        offset = dst ? -4 * 3600 : -5 * 3600;
    }
    quote->epoch_utc = civil_days(year, month, day) * 86400 + hour * 3600 + min * 60 + sec - offset;
    /* 统一显示日期，不丢掉休市时的旧行情时间。 */
    const unsigned digit_positions[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    memcpy(quote->time_text, "0000-00-00 00:00:00", 20);
    for (size_t i = 0; i < sizeof(digits); ++i) quote->time_text[digit_positions[i]] = digits[i];
    return true;
}

static bool provider_code(span_t code, const stock_instrument_t *instrument)
{
    const char *ticker = instrument->symbol + 2;
    if (span_equal(code, ticker)) return true;
    if (instrument->market != STOCK_MARKET_US) return false;
    size_t size = strlen(ticker);
    if (code.size <= size || memcmp(code.data, ticker, size) != 0) return false;
    static const char *const venues[] = {".OQ", ".N", ".AM", ".O", ".A", ".P", ".NQ"};
    span_t suffix = {code.data + size, code.size - size};
    for (unsigned i = 0; i < sizeof(venues) / sizeof(venues[0]); ++i) {
        if (span_equal(suffix, venues[i])) return true;
    }
    return false;
}

static bool record(span_t body, const stock_instrument_t *instrument, stock_quote_t *out)
{
    span_t fields[33];
    size_t begin = 0, count = 0;
    for (size_t i = 0; i <= body.size; ++i) {
        if (i == body.size || body.data[i] == '~') {
            if (count < 33) fields[count] = (span_t){body.data + begin, i - begin};
            ++count;
            begin = i + 1;
        }
    }
    if (count <= 32 || !provider_code(fields[2], instrument)) return false;
    stock_quote_t quote = {.symbol = instrument->market == STOCK_MARKET_CN ? STOCK_LEXIN : STOCK_NVDA};
    if (!numeric(fields[3], true, &quote.price) || !numeric(fields[4], true, &quote.previous_close) ||
        !numeric(fields[31], false, &quote.change) || !numeric(fields[32], false, &quote.change_pct) ||
        !timestamp(fields[30], instrument->market, &quote)) return false;
    double change = quote.price - quote.previous_close;
    if (fabs(change - quote.change) > 0.031 || fabs(change / quote.previous_close * 100 - quote.change_pct) > 0.031) return false;
    memcpy(quote.name, instrument->name, strlen(instrument->name) + 1);
    quote.valid = true;
    *out = quote;
    return true;
}

bool stock_quote_parse_instrument(const void *data, size_t size,
                                  const stock_instrument_t *instrument, stock_quote_t *out)
{
    if (!data || !out || !instrument || !size || size > STOCK_RESPONSE_MAX || memchr(data, 0, size)) return false;
    if (!memchr(instrument->symbol, 0, sizeof(instrument->symbol)) ||
        !memchr(instrument->name, 0, sizeof(instrument->name)) || !instrument->name[0]) return false;
    stock_instrument_t checked;
    if (!stock_instrument_from_code(instrument->symbol, &checked) ||
        checked.market != instrument->market || strcmp(checked.symbol, instrument->symbol) != 0) return false;
    const char *bytes = data;
    char name[STOCK_SYMBOL_SIZE + 2];
    snprintf(name, sizeof(name), "v_%s", instrument->symbol);
    size_t name_size = strlen(name);
    bool found = false;
    stock_quote_t quote;
    /* 完整赋值语句且标识符精确匹配：A 与 AAPL、BRK 与 BRK.B 不能互相混淆。 */
    for (size_t begin = 0; begin < size;) {
        size_t end = begin;
        while (end < size && bytes[end] != '\n' && bytes[end] != '\r') ++end;
        size_t cursor = begin;
        while (cursor < end && (bytes[cursor] == ' ' || bytes[cursor] == '\t')) ++cursor;
        if (end - cursor > name_size && memcmp(bytes + cursor, name, name_size) == 0 &&
            bytes[cursor + name_size] == '=') {
            cursor += name_size;
            if (end - cursor < 4 || bytes[cursor + 1] != '"' || found) return false;
            cursor += 2;
            size_t body_begin = cursor;
            while (cursor < end && bytes[cursor] != '"') ++cursor;
            if (cursor + 1 >= end || bytes[cursor + 1] != ';') return false;
            size_t finish = cursor + 2;
            while (finish < end && (bytes[finish] == ' ' || bytes[finish] == '\t')) ++finish;
            if (finish != end || !record((span_t){bytes + body_begin, cursor - body_begin}, instrument, &quote)) return false;
            found = true;
        }
        begin = end;
        while (begin < size && (bytes[begin] == '\r' || bytes[begin] == '\n')) ++begin;
    }
    if (found) *out = quote;
    return found;
}

bool stock_quote_parse(const void *data, size_t size, stock_symbol_t symbol, stock_quote_t *out)
{
    if (symbol < STOCK_LEXIN || symbol > STOCK_NVDA) return false;
    stock_instrument_t instrument;
    if (!stock_instrument_from_code(instrument_codes[symbol], &instrument)) return false;
    memcpy(instrument.name, instrument_names[symbol], strlen(instrument_names[symbol]) + 1);
    return stock_quote_parse_instrument(data, size, &instrument, out);
}

bool stock_market_session_scheduled(stock_market_t market, int64_t now_utc)
{
    if (now_utc < 1167609600LL || now_utc > 4102444799LL || market < STOCK_MARKET_CN || market > STOCK_MARKET_US) return false;
    int offset = market == STOCK_MARKET_CN ? 8 * 3600 : ny_utc_offset(now_utc);
    int year, month, day, minute, wday;
    epoch_parts(now_utc + offset, &year, &month, &day, &minute, &wday);
    if (wday == 0 || wday == 6) return false;
    if (market == STOCK_MARKET_CN) return (minute >= 570 && minute < 690) || (minute >= 780 && minute < 900);
    return minute >= 570 && minute < 960;
}

bool stock_quote_session_scheduled(stock_symbol_t symbol, int64_t now_utc)
{
    if (symbol < STOCK_LEXIN || symbol > STOCK_NVDA) return false;
    return stock_market_session_scheduled(symbol == STOCK_LEXIN ? STOCK_MARKET_CN : STOCK_MARKET_US, now_utc);
}

bool stock_quote_is_stale(const stock_quote_t *quote, int64_t now_utc)
{
    return !quote || !quote->valid || now_utc < 1167609600LL ||
           quote->epoch_utc > now_utc + 60 || now_utc - quote->epoch_utc > 180;
}
