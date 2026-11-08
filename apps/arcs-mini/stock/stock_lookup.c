#include "stock_lookup.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "lisa_mem.h"
#include "stock_http.h"

#define LOOKUP_BODY_LIMIT 8192u
#define LOOKUP_CANDIDATES 16u

typedef struct {
    stock_instrument_t instrument;
    bool exact_name;
    bool exact_code;
} candidate_t;

static bool whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Reject controls, invalid UTF-8, surrogate/overlong sequences and delimiters. */
static bool valid_text(const char *text)
{
    const unsigned char *bytes = (const unsigned char *)text;
    for (size_t i = 0; bytes[i];) {
        uint32_t point = bytes[i];
        unsigned extra = 0;
        if (point < 0x80) {
            if (point < 0x20 || point == 0x7f || point == '~' || point == '^') return false;
        } else if (point >= 0xc2 && point <= 0xdf) { point &= 0x1f; extra = 1; }
        else if (point >= 0xe0 && point <= 0xef) { point &= 0x0f; extra = 2; }
        else if (point >= 0xf0 && point <= 0xf4) { point &= 0x07; extra = 3; }
        else return false;
        for (unsigned j = 1; j <= extra; ++j) {
            unsigned char next = bytes[i + j];
            if (!next || (next & 0xc0) != 0x80) return false;
            point = (point << 6) | (next & 0x3f);
        }
        if ((extra == 1 && point < 0x80) || (extra == 2 && point < 0x800) ||
            (extra == 3 && point < 0x10000) || point > 0x10ffff ||
            (point >= 0xd800 && point <= 0xdfff)) return false;
        i += extra + 1;
    }
    return text[0] != '\0';
}

static bool normalize_query(const char *query, char normalized[STOCK_QUERY_MAX + 1])
{
    if (!query) return false;
    size_t size = 0;
    while (size <= STOCK_QUERY_MAX && query[size]) ++size;
    if (size > STOCK_QUERY_MAX) return false;
    size_t begin = 0;
    while (begin < size && whitespace(query[begin])) ++begin;
    while (size > begin && whitespace(query[size - 1])) --size;
    if (size == begin) return false;
    memcpy(normalized, query + begin, size - begin);
    normalized[size - begin] = '\0';
    return valid_text(normalized);
}

/* A lower-case ticker such as usb must not become the unrelated ticker B.
 * In lookup syntax, explicit US prefixes are unambiguous as usAAPL; entirely
 * lower-case "us..." queries are searched as bare tickers first. */
static bool query_code(const char *query, stock_instrument_t *out)
{
    char code[STOCK_QUERY_MAX + 1];
    snprintf(code, sizeof(code), "%s", query);
    if (code[0] == 'u' && code[1] == 's' && code[2] >= 'a' && code[2] <= 'z') {
        for (size_t i = 0; code[i]; ++i) {
            if (code[i] >= 'a' && code[i] <= 'z') code[i] -= 'a' - 'A';
        }
    }
    return stock_instrument_from_code(code, out);
}

static bool equal_ascii_case(const char *left, const char *right)
{
    for (size_t i = 0;; ++i) {
        unsigned char a = (unsigned char)left[i], b = (unsigned char)right[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
        if (!a) return true;
    }
}

static void copy_display_name(char out[STOCK_NAME_SIZE], const char *name)
{
    size_t size = strlen(name);
    if (size >= STOCK_NAME_SIZE) {
        size = STOCK_NAME_SIZE - 1;
        while (size && ((unsigned char)name[size] & 0xc0) == 0x80) --size;
    }
    memcpy(out, name, size);
    out[size] = '\0';
}

static bool make_instrument(char *fields[5], stock_instrument_t *out)
{
    char symbol[STOCK_SYMBOL_SIZE];
    if (!valid_text(fields[2])) return false;
    if (strcmp(fields[0], "us") == 0 && strcmp(fields[4], "GP") == 0) {
        /* Tencent appends the listing exchange, e.g. aapl.oq, brk.b.n. */
        char *suffix = strrchr(fields[1], '.');
        if (suffix) {
            static const char *const exchanges[] = {".oq", ".n", ".am", ".ps"};
            bool known = false;
            for (unsigned i = 0; i < sizeof(exchanges) / sizeof(exchanges[0]); ++i) {
                if (equal_ascii_case(suffix, exchanges[i])) known = true;
            }
            if (!known) return false;
            *suffix = '\0';
        }
        if (snprintf(symbol, sizeof(symbol), "us%s", fields[1]) >= (int)sizeof(symbol)) return false;
    } else if ((strcmp(fields[0], "sh") == 0 || strcmp(fields[0], "sz") == 0 ||
                strcmp(fields[0], "bj") == 0) && strncmp(fields[4], "GP-A", 4) == 0 &&
               (fields[4][4] == '\0' || fields[4][4] == '-')) {
        if (snprintf(symbol, sizeof(symbol), "%s%s", fields[0], fields[1]) >= (int)sizeof(symbol)) return false;
    } else return false; /* No H shares, funds, indices or derivatives. */
    if (!stock_instrument_from_code(symbol, out)) return false;
    copy_display_name(out->name, fields[2]);
    return true;
}

static int fail(char *message, size_t size, int code, const char *text)
{
    if (message && size) snprintf(message, size, "%s", text);
    return code;
}

int stock_lookup_parse(const void *data, size_t size, const char *query,
                       stock_instrument_t *out, char *message, size_t message_size)
{
    char normalized[STOCK_QUERY_MAX + 1];
    if (!out || !normalize_query(query, normalized) || !data || !size || size > LOOKUP_BODY_LIMIT ||
        memchr(data, 0, size)) return fail(message, message_size, -1, "股票名称或查询结果无效。");
    const char *bytes = data;
    size_t begin = 0, end = size;
    while (begin < end && whitespace(bytes[begin])) ++begin;
    while (end > begin && whitespace(bytes[end - 1])) --end;
    if (end > begin && bytes[end - 1] == ';') {
        --end;
        while (end > begin && whitespace(bytes[end - 1])) --end;
    }
    static const char prefix[] = "v_hint=";
    if (end - begin < sizeof(prefix) + 1 || memcmp(bytes + begin, prefix, sizeof(prefix) - 1) != 0) {
        return fail(message, message_size, -2, "股票搜索结果格式异常，请稍后重试。");
    }
    begin += sizeof(prefix) - 1;
    if (bytes[begin] != '"' || bytes[end - 1] != '"') {
        return fail(message, message_size, -2, "股票搜索结果不完整，请稍后重试。");
    }
    /* Do not let an escaped NUL hide candidate fields from C string parsing. */
    for (size_t i = begin; i + 5 < end; ++i) {
        if (memcmp(bytes + i, "\\u0000", 6) == 0) {
            return fail(message, message_size, -2, "股票搜索结果无效。");
        }
    }
    const char *parsed_end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(bytes + begin, end - begin, &parsed_end, false);
    if (!json || !cJSON_IsString(json) || !json->valuestring || parsed_end != bytes + end) {
        cJSON_Delete(json);
        return fail(message, message_size, -2, "股票搜索结果无法解析，请稍后重试。");
    }
    if (strcmp(json->valuestring, "N") == 0 || !json->valuestring[0]) {
        cJSON_Delete(json);
        return fail(message, message_size, -1, "没找到这只股票，请提供A股六位代码或美股代码。");
    }
    stock_instrument_t code;
    bool code_query = query_code(normalized, &code);
    candidate_t candidates[LOOKUP_CANDIDATES];
    unsigned count = 0;
    bool invalid = false;
    char *row = json->valuestring;
    while (row && row[0]) {
        char *next = strchr(row, '^');
        if (next) {
            *next++ = '\0';
            if (!next[0]) { invalid = true; break; }
        }
        char *fields[5];
        fields[0] = row;
        bool complete = true;
        for (unsigned i = 1; i < 5; ++i) {
            char *delimiter = strchr(fields[i - 1], '~');
            if (!delimiter) { complete = false; break; }
            *delimiter = '\0';
            fields[i] = delimiter + 1;
        }
        if (!complete || strchr(fields[4], '~')) { invalid = true; break; }
        stock_instrument_t instrument;
        if (make_instrument(fields, &instrument)) {
            unsigned existing = count;
            for (unsigned i = 0; i < count; ++i) {
                if (strcmp(candidates[i].instrument.symbol, instrument.symbol) == 0) { existing = i; break; }
            }
            bool exact_name = equal_ascii_case(fields[2], normalized);
            bool exact_code = (code_query && strcmp(code.symbol, instrument.symbol) == 0) ||
                              equal_ascii_case(normalized, instrument.symbol + 2);
            if (existing < count) {
                candidates[existing].exact_name |= exact_name;
                candidates[existing].exact_code |= exact_code;
            } else if (count < LOOKUP_CANDIDATES) {
                candidates[count++] = (candidate_t){instrument, exact_name, exact_code};
            } else { invalid = true; break; }
        }
        row = next;
    }
    cJSON_Delete(json);
    if (invalid) return fail(message, message_size, -2, "股票搜索结果不完整，请直接提供股票代码。");
    if (!count) return fail(message, message_size, -1, "没有匹配的A股或美股股票，请提供股票代码。");
    int choice = -1;
    unsigned matches = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (candidates[i].exact_code) { choice = (int)i; ++matches; }
    }
    if (!matches) {
        for (unsigned i = 0; i < count; ++i) {
            if (candidates[i].exact_name) { choice = (int)i; ++matches; }
        }
    }
    if (!matches && count == 1) { choice = 0; matches = 1; }
    if (matches == 1) {
        *out = candidates[choice].instrument;
        if (message && message_size) snprintf(message, message_size, "已匹配%s（%s），需要核验报价。", out->name, out->symbol);
        return 0;
    }
    if (message && message_size) {
        snprintf(message, message_size, "找到多只股票：");
        for (unsigned i = 0; i < count && i < 4; ++i) {
            size_t used = strlen(message);
            if (used < message_size) snprintf(message + used, message_size - used, "%s%s（%s）",
                                              i ? "、" : "", candidates[i].instrument.name,
                                              candidates[i].instrument.symbol);
        }
        size_t used = strlen(message);
        if (used < message_size) snprintf(message + used, message_size - used, "。请说具体股票代码。");
    }
    return -2;
}

static bool encode_query(const char *query, char *url, size_t capacity)
{
    static const char prefix[] = "https://smartbox.gtimg.cn/s3/?v=2&t=all&q=";
    static const char hex[] = "0123456789ABCDEF";
    size_t used = sizeof(prefix) - 1;
    if (capacity <= used) return false;
    memcpy(url, prefix, used);
    for (const unsigned char *p = (const unsigned char *)query; *p; ++p) {
        bool safe = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                    (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-';
        size_t bytes = safe ? 1 : 3;
        if (used + bytes >= capacity) return false;
        if (safe) url[used++] = (char)*p;
        else { url[used++] = '%'; url[used++] = hex[*p >> 4]; url[used++] = hex[*p & 15]; }
    }
    url[used] = '\0';
    return true;
}

int stock_lookup_resolve(const char *query, stock_instrument_t *out,
                         char *message, size_t message_size)
{
    char normalized[STOCK_QUERY_MAX + 1];
    if (!out || !normalize_query(query, normalized)) return fail(message, message_size, -1, "股票名称或代码无效，请简短说明或直接提供代码。");
    stock_instrument_t code;
    bool code_query = query_code(normalized, &code);
    /* Canonical symbols are not always understood by search; search the code. */
    const char *search = normalized;
    if (code_query && (code.market == STOCK_MARKET_CN ||
        (normalized[0] == 'u' && normalized[1] == 's' && normalized[2] >= 'A' && normalized[2] <= 'Z'))) {
        search = code.symbol + 2;
    }
    char url[320], error[48];
    if (!encode_query(search, url, sizeof(url))) return fail(message, message_size, -1, "股票名称过长。");
    char *body = lisa_mem_alloc(LOOKUP_BODY_LIMIT + 1);
    if (!body) return fail(message, message_size, -2, "内存不足，请稍后重试。");
    size_t size = 0;
    bool fetched = stock_http_get(url, body, LOOKUP_BODY_LIMIT + 1, &size, error, sizeof(error));
    int result = fetched ? stock_lookup_parse(body, size, normalized, out, message, message_size) :
                          fail(message, message_size, -2, "股票搜索暂时不可用，请稍后重试或提供股票代码。");
    lisa_mem_free(body);
    /* A syntactically valid explicit code can be checked directly against quotes.
     * Never use this fallback for ambiguous search results. */
    if (code_query && ((!fetched) || result == -1)) {
        *out = code;
        if (message && message_size) snprintf(message, message_size, "正在核验股票代码%s。", code.symbol);
        return 0;
    }
    return result;
}
