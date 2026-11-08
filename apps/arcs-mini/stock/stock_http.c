#include "stock_http.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "HTTPCUsr_api.h"

#define STOCK_HTTP_LIMIT 16384u

static UINT32 request_timeout(TickType_t started)
{
    uint32_t elapsed = (xTaskGetTickCount() - started) * portTICK_PERIOD_MS;
    return elapsed < 12000u ? (12000u - elapsed + 999u) / 1000u : 0;
}

static bool allowed_url(const char *url)
{
    static const char quote_host[] = "https://qt.gtimg.cn/";
    static const char search_host[] = "https://smartbox.gtimg.cn/";
    static const char chart_host[] = "https://web.ifzq.gtimg.cn/";
    if (!url || strlen(url) >= 512) return false;
    return strncmp(url, quote_host, sizeof(quote_host) - 1) == 0 ||
           strncmp(url, search_host, sizeof(search_host) - 1) == 0 ||
           strncmp(url, chart_host, sizeof(chart_host) - 1) == 0;
}

bool stock_http_get(const char *url, char *body, size_t capacity, size_t *body_size,
                    char *error, size_t error_size)
{
    if (!body_size || !error || !error_size) return false;
    *body_size = 0;
    if (!allowed_url(url) || !body || capacity < 2 || capacity > STOCK_HTTP_LIMIT + 1) {
        snprintf(error, error_size, "http_argument");
        return false;
    }
    body[0] = '\0';
    bool good = false;
    const size_t limit = capacity - 1;
    TickType_t started = xTaskGetTickCount();
    HTTP_SESSION_HANDLE handle = HTTPClientOpenRequest(0);
    if (!handle) {
        snprintf(error, error_size, "http_open");
        return false;
    }
    if (HTTPClientSetVerb(handle, VerbGet) != HTTP_CLIENT_SUCCESS) {
        snprintf(error, error_size, "http_config");
        goto done;
    }
    /* The SDK takes a mutable URL; never let it alter caller-owned storage. */
    char mutable_url[512];
    snprintf(mutable_url, sizeof(mutable_url), "%s", url);
    UINT32 timeout = request_timeout(started);
    if (!timeout) {
        snprintf(error, error_size, "http_timeout");
        goto done;
    }
    int result = HTTPClientSendRequest(handle, mutable_url, NULL, 0, FALSE, timeout, 0, NULL, NULL);
    if (result != HTTP_CLIENT_SUCCESS) {
        snprintf(error, error_size, "http_request");
        goto done;
    }
    timeout = request_timeout(started);
    if (!timeout) {
        snprintf(error, error_size, "http_timeout");
        goto done;
    }
    if (HTTPClientRecvResponse(handle, timeout) != HTTP_CLIENT_SUCCESS) {
        snprintf(error, error_size, "http_response");
        goto done;
    }
    HTTP_CLIENT info = {0};
    if (HTTPClientGetInfo(handle, &info) != HTTP_CLIENT_SUCCESS || info.HTTPStatusCode != 200) {
        snprintf(error, error_size, "http_status");
        goto done;
    }
    if (info.TotalResponseBodyLength > limit) {
        snprintf(error, error_size, "response_too_large");
        goto done;
    }
    for (;;) {
        UINT32 received = 0;
        size_t remaining = capacity - *body_size;
        UINT32 request_size = remaining > 1024 ? 1024 : (UINT32)remaining;
        timeout = request_timeout(started);
        if (!timeout) {
            snprintf(error, error_size, "http_timeout");
            goto done;
        }
        /* One spare byte lets unknown-length bodies prove they exceed the limit. */
        result = HTTPClientReadData(handle, body + *body_size, request_size, timeout, &received);
        if (received > request_size || *body_size + received > limit) {
            snprintf(error, error_size, "response_too_large");
            goto done;
        }
        *body_size += received;
        if (result != HTTP_CLIENT_SUCCESS && result != HTTP_CLIENT_EOS) {
            snprintf(error, error_size, "http_read");
            goto done;
        }
        if (info.TotalResponseBodyLength && *body_size >= info.TotalResponseBodyLength) {
            good = *body_size == info.TotalResponseBodyLength;
            break;
        }
        if (result == HTTP_CLIENT_EOS) {
            good = !info.TotalResponseBodyLength;
            break;
        }
        if (received == 0) break;
    }
    if (!good || !*body_size) {
        snprintf(error, error_size, "http_read");
        good = false;
    }
    if (!request_timeout(started)) {
        snprintf(error, error_size, "http_timeout");
        good = false;
    }
done:
    if (good) {
        body[*body_size] = '\0';
        error[0] = '\0';
    }
    HTTPClientCloseRequest(&handle);
    return good;
}
