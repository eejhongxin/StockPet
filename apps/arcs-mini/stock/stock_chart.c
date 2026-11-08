#include "stock_chart.h"

#include <math.h>
#include <string.h>

#include "cJSON.h"

/* Validate the wire JSON before cJSON allocation. Apart from enforcing JSON's
 * number/string grammar, the depth cap bounds parser and destructor stacks.
 * NUL escapes are rejected because cJSON exposes strings as C strings. */
typedef struct {
    const unsigned char *data;
    size_t size;
    size_t pos;
} json_scan_t;

static bool json_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void json_skip_space(json_scan_t *scan)
{
    while (scan->pos < scan->size && json_space(scan->data[scan->pos])) ++scan->pos;
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool json_hex4(json_scan_t *scan, unsigned *out)
{
    if (scan->size - scan->pos < 4) return false;
    unsigned value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int digit = hex_digit(scan->data[scan->pos++]);
        if (digit < 0) return false;
        value = value * 16u + (unsigned)digit;
    }
    *out = value;
    return true;
}

static bool json_string(json_scan_t *scan)
{
    if (scan->pos >= scan->size || scan->data[scan->pos++] != '"') return false;
    while (scan->pos < scan->size) {
        unsigned char c = scan->data[scan->pos++];
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c == '\\') {
            if (scan->pos == scan->size) return false;
            c = scan->data[scan->pos++];
            if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' ||
                c == 'n' || c == 'r' || c == 't') continue;
            if (c != 'u') return false;
            unsigned code;
            if (!json_hex4(scan, &code) || !code) return false;
            if (code >= 0xd800 && code <= 0xdbff) {
                if (scan->size - scan->pos < 6 || scan->data[scan->pos++] != '\\' ||
                    scan->data[scan->pos++] != 'u' || !json_hex4(scan, &code) ||
                    code < 0xdc00 || code > 0xdfff) return false;
            } else if (code >= 0xdc00 && code <= 0xdfff) return false;
        } else if (c >= 0x80) {
            unsigned count, code, minimum;
            if (c >= 0xc2 && c <= 0xdf) { count = 1; code = c & 0x1fu; minimum = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { count = 2; code = c & 0x0fu; minimum = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { count = 3; code = c & 0x07u; minimum = 0x10000; }
            else return false;
            if (scan->size - scan->pos < count) return false;
            for (unsigned i = 0; i < count; ++i) {
                c = scan->data[scan->pos++];
                if ((c & 0xc0u) != 0x80u) return false;
                code = (code << 6) | (c & 0x3fu);
            }
            if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        }
    }
    return false;
}

static bool json_value(json_scan_t *scan, unsigned depth)
{
    json_skip_space(scan);
    if (scan->pos == scan->size || depth > 16) return false;
    unsigned char c = scan->data[scan->pos];
    if (c == '"') return json_string(scan);
    if (c == '{' || c == '[') {
        unsigned char close = c == '{' ? '}' : ']';
        ++scan->pos;
        json_skip_space(scan);
        if (scan->pos < scan->size && scan->data[scan->pos] == close) { ++scan->pos; return true; }
        for (;;) {
            if (c == '{') {
                if (!json_string(scan)) return false;
                json_skip_space(scan);
                if (scan->pos == scan->size || scan->data[scan->pos++] != ':') return false;
            }
            if (!json_value(scan, depth + 1)) return false;
            json_skip_space(scan);
            if (scan->pos == scan->size) return false;
            unsigned char separator = scan->data[scan->pos++];
            if (separator == close) return true;
            if (separator != ',') return false;
            json_skip_space(scan);
        }
    }
    const char *literal = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : NULL;
    if (literal) {
        size_t size = strlen(literal);
        if (scan->size - scan->pos < size || memcmp(scan->data + scan->pos, literal, size)) return false;
        scan->pos += size;
        return true;
    }
    if (c == '-') ++scan->pos;
    if (scan->pos == scan->size) return false;
    c = scan->data[scan->pos++];
    if (c >= '1' && c <= '9') {
        while (scan->pos < scan->size && scan->data[scan->pos] >= '0' && scan->data[scan->pos] <= '9') ++scan->pos;
    } else if (c != '0') return false;
    if (scan->pos < scan->size && scan->data[scan->pos] == '.') {
        size_t begin = ++scan->pos;
        while (scan->pos < scan->size && scan->data[scan->pos] >= '0' && scan->data[scan->pos] <= '9') ++scan->pos;
        if (scan->pos == begin) return false;
    }
    if (scan->pos < scan->size && (scan->data[scan->pos] == 'e' || scan->data[scan->pos] == 'E')) {
        ++scan->pos;
        if (scan->pos < scan->size && (scan->data[scan->pos] == '+' || scan->data[scan->pos] == '-')) ++scan->pos;
        size_t begin = scan->pos;
        while (scan->pos < scan->size && scan->data[scan->pos] >= '0' && scan->data[scan->pos] <= '9') ++scan->pos;
        if (scan->pos == begin) return false;
    }
    return true;
}

static const cJSON *unique_field(const cJSON *object, const char *name)
{
    if (!cJSON_IsObject(object)) return NULL;
    const cJSON *found = NULL;
    for (const cJSON *item = object->child; item; item = item->next) {
        if (item->string && strcmp(item->string, name) == 0) {
            if (found) return NULL;
            found = item;
        }
    }
    return found;
}

static bool decimal(const char *text, size_t size, double *out)
{
    if (!size || size > 31 || text[0] < '0' || text[0] > '9') return false;
    double value = 0, fraction = 0.1;
    bool dot = false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '.' && !dot && i + 1 < size) { dot = true; continue; }
        if (c < '0' || c > '9') return false;
        if (dot) { value += (c - '0') * fraction; fraction *= 0.1; }
        else value = value * 10 + c - '0';
    }
    if (!isfinite(value)) return false;
    *out = value;
    return true;
}

static bool positive_price(const char *text, size_t size, float *out)
{
    double value;
    if (!decimal(text, size, &value) || value <= 0 || value > 1000000000.0) return false;
    float price = (float)value;
    if (!isfinite(price) || price <= 0) return false;
    *out = price;
    return true;
}

static bool source_date(const cJSON *item, char out[11])
{
    if (!cJSON_IsString(item) || strlen(item->valuestring) != 8) return false;
    const char *text = item->valuestring;
    for (unsigned i = 0; i < 8; ++i) if (text[i] < '0' || text[i] > '9') return false;
    unsigned year = (unsigned)((text[0] - '0') * 1000 + (text[1] - '0') * 100 +
                               (text[2] - '0') * 10 + text[3] - '0');
    unsigned month = (unsigned)((text[4] - '0') * 10 + text[5] - '0');
    unsigned day = (unsigned)((text[6] - '0') * 10 + text[7] - '0');
    static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (!year || month < 1 || month > 12 || !day) return false;
    unsigned max_day = days[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ++max_day;
    if (day > max_day) return false;
    memcpy(out, text, 4); out[4] = '-';
    memcpy(out + 5, text + 4, 2); out[7] = '-';
    memcpy(out + 8, text + 6, 2); out[10] = '\0';
    return true;
}

static bool provider_code(const cJSON *item, const stock_instrument_t *instrument)
{
    if (!cJSON_IsString(item)) return false;
    const char *code = item->valuestring, *ticker = instrument->symbol + 2;
    size_t size = strlen(ticker);
    if (strcmp(code, ticker) == 0) return true;
    if (instrument->market != STOCK_MARKET_US || strlen(code) <= size || memcmp(code, ticker, size)) return false;
    static const char *const venues[] = {".OQ", ".N", ".AM", ".O", ".A", ".P", ".NQ"};
    for (unsigned i = 0; i < sizeof(venues) / sizeof(venues[0]); ++i) {
        if (strcmp(code + size, venues[i]) == 0) return true;
    }
    return false;
}

static bool row_space(char c)
{
    return c == ' ' || c == '\t';
}

static bool source_point(const cJSON *item, stock_chart_point_t *out)
{
    if (!cJSON_IsString(item)) return false;
    const char *text = item->valuestring;
    size_t size = strlen(text);
    if (size < 8 || size > 127 || !row_space(text[4])) return false;
    for (unsigned i = 0; i < 4; ++i) if (text[i] < '0' || text[i] > '9') return false;
    unsigned hour = (unsigned)((text[0] - '0') * 10 + text[1] - '0');
    unsigned minute = (unsigned)((text[2] - '0') * 10 + text[3] - '0');
    if (hour > 23 || minute > 59) return false;
    out->minute = (uint16_t)(hour * 60 + minute);
    size_t pos = 4;
    unsigned fields = 0;
    while (pos < size) {
        while (pos < size && row_space(text[pos])) ++pos;
        if (pos == size) break;
        size_t begin = pos;
        while (pos < size && !row_space(text[pos])) ++pos;
        if (!fields) {
            if (!positive_price(text + begin, pos - begin, &out->price)) return false;
        } else {
            double ignored;
            if (!decimal(text + begin, pos - begin, &ignored)) return false;
        }
        if (++fields > 3) return false;
    }
    return fields >= 2;
}

static bool regular_session(stock_market_t market, uint16_t minute)
{
    if (market == STOCK_MARKET_US) return minute >= 570 && minute <= 960;
    return (minute >= 570 && minute <= 690) || (minute >= 780 && minute <= 900);
}

bool stock_chart_parse(const void *data, size_t size,
                       const stock_instrument_t *instrument, stock_chart_t *out)
{
    if (!data || !out || !instrument || !size || size > STOCK_RESPONSE_MAX || memchr(data, 0, size) ||
        !memchr(instrument->symbol, 0, sizeof(instrument->symbol))) return false;
    stock_instrument_t normalized;
    if (!stock_instrument_from_code(instrument->symbol, &normalized) ||
        normalized.market != instrument->market || strcmp(normalized.symbol, instrument->symbol)) return false;
    json_scan_t scan = {.data = data, .size = size};
    if (!json_value(&scan, 0)) return false;
    json_skip_space(&scan);
    if (scan.pos != size) return false;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(data, size, &end, 0);
    if (!root) return false;
    const cJSON *code = unique_field(root, "code");
    const cJSON *node = unique_field(unique_field(root, "data"), instrument->symbol);
    const cJSON *series = unique_field(node, "data");
    const cJSON *rows = unique_field(series, "data");
    const cJSON *quote = unique_field(unique_field(node, "qt"), instrument->symbol);
    const cJSON *previous_close = cJSON_GetArrayItem(quote, 4);
    stock_chart_t chart = {0};
    bool valid = cJSON_IsNumber(code) && code->valuedouble == 0 && cJSON_IsArray(quote) &&
                 provider_code(cJSON_GetArrayItem(quote, 2), instrument) &&
                 cJSON_IsString(previous_close) &&
                 positive_price(previous_close->valuestring, strlen(previous_close->valuestring), &chart.previous_close) &&
                 source_date(unique_field(series, "date"), chart.date) && cJSON_IsArray(rows);
    if (valid) {
        int count = cJSON_GetArraySize(rows);
        valid = count > 0 && (unsigned)count <= STOCK_CHART_MAX_POINTS;
        int previous_minute = -1;
        for (const cJSON *item = rows->child; valid && item; item = item->next) {
            stock_chart_point_t point;
            valid = source_point(item, &point) && point.minute > previous_minute;
            if (!valid) break;
            previous_minute = point.minute;
            if (regular_session(instrument->market, point.minute)) chart.points[chart.count++] = point;
        }
        valid = valid && chart.count > 0;
    }
    cJSON_Delete(root);
    if (!valid) return false;
    chart.market = instrument->market;
    memcpy(chart.symbol, instrument->symbol, strlen(instrument->symbol) + 1);
    chart.valid = true;
    *out = chart;
    return true;
}
