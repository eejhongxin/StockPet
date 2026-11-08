#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cJSON.h"
#include "lisa_log.h"
#include "lisa_mem.h"
#include "lisa_thread.h"
#include "mcp.h"
#include "service_stock.h"

#define STOCK_TOOL_NAME "stock_watchlist"
#define STOCK_QUERY_BYTES_MAX 80u
#define STOCK_CALL_ID_BYTES_MAX 63u
#define STOCK_RESULT_BYTES 2048u

typedef struct {
    char id[STOCK_CALL_ID_BYTES_MAX + 1];
    char action[8];
    char query[STOCK_QUERY_BYTES_MAX + 1];
    char text[STOCK_RESULT_BYTES];
    cJSON *result;
    cJSON *error;
} stock_mcp_job_t;

/* One bounded operation at a time. Network lookup and KV writes must not block
 * voice.ebus, which dispatches MCP callbacks and all voice state changes. */
static bool s_stock_call_active;

static cJSON *stock_result_text(const char *name, const char *text, bool error, bool borrowed)
{
    cJSON *result = mcp_tool_call_result_create(name);
    const cJSON *tool = cJSON_GetObjectItemCaseSensitive(result, "tool");
    if (!cJSON_IsString(tool) || !tool->valuestring || strcmp(tool->valuestring, name) != 0) {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *item = content ? cJSON_CreateObject() : NULL;
    if (!item || !cJSON_AddItemToArray(content, item)) {
        cJSON_Delete(item);
        cJSON_Delete(result);
        return NULL;
    }
    if (!cJSON_AddStringToObject(item, "type", "text") || !cJSON_AddBoolToObject(result, "isError", error)) {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON *value = borrowed ? cJSON_CreateStringReference(text) : cJSON_CreateString(text);
    if (!value || !cJSON_AddItemToObject(item, "text", value)) {
        cJSON_Delete(value);
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static bool required_string(cJSON *required, const char *name)
{
    cJSON *entry = cJSON_CreateString(name);
    if (!entry || !cJSON_AddItemToArray(required, entry)) {
        cJSON_Delete(entry);
        return false;
    }
    return true;
}

static cJSON *stock_watchlist_list(const char *name)
{
    cJSON *tool = mcp_tool_list_info_create_default(name,
        "管理生生行情桌面的自选股票。用户说添加苹果股票、帮我盯一下某股票时用add；"
        "删除英伟达、不看某股票时用remove；看看自选股、现在盯哪些股票时用list。"
        "支持A股和美股，query传用户说的股票名称或代码，名称不明确时必须根据返回结果追问，禁止猜股票代码。"
        "这是行情显示与自选股管理，不交易、不设置价格提醒；美股公开行情可能延时。"
        "首次响应先说一句非空自然语言（如我来更新自选股），随后再调用工具；"
        "必须收到工具成功结果后才能说已添加或已删除，失败时说明原因，不得编造成功。"
        "list返回设备当前保存的自选与最新可用行情，按来源时间及延时说明回答，不能称为保证实时。"
        "每次只添加或删除一只股票。");
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
    if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(tool, "name")) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(tool, "description")) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(schema, "type")) ||
        !cJSON_IsObject(properties) || !cJSON_IsArray(required)) {
        goto failed;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(schema, "additionalProperties");
    if (!cJSON_AddBoolToObject(schema, "additionalProperties", false)) goto failed;
    cJSON *action = cJSON_AddObjectToObject(properties, "action");
    if (!action || !cJSON_AddStringToObject(action, "type", "string") ||
        !cJSON_AddStringToObject(action, "description", "add添加一只，remove删除一只，list查看设备当前自选及行情")) goto failed;
    cJSON *values = cJSON_AddArrayToObject(action, "enum");
    if (!values || !required_string(values, "add") || !required_string(values, "remove") ||
        !required_string(values, "list") || !required_string(required, "action")) goto failed;
    cJSON *query = cJSON_AddObjectToObject(properties, "query");
    if (!query || !cJSON_AddStringToObject(query, "type", "string") ||
        !cJSON_AddNumberToObject(query, "maxLength", STOCK_QUERY_BYTES_MAX) ||
        !cJSON_AddStringToObject(query, "description",
            "add/remove必填非空；用户说出的股票中文名、英文名或代码，例如苹果、英伟达、600519、AAPL。"
            "最多80个UTF-8字节，不要猜测、改写或附加无关句子；list省略此参数。")) goto failed;
    return tool;
failed:
    cJSON_Delete(tool);
    return NULL;
}

static size_t bounded_length(const char *text, size_t limit)
{
    size_t length = 0;
    if (text) while (length <= limit && text[length]) ++length;
    return length;
}

static bool trim_query(const char *input, char *output)
{
    size_t length = bounded_length(input, STOCK_QUERY_BYTES_MAX);
    if (!input || length > STOCK_QUERY_BYTES_MAX) return false;
    size_t begin = 0;
    while (begin < length && input[begin] == ' ') ++begin;
    while (length > begin && input[length - 1] == ' ') --length;
    for (size_t i = begin; i < length; ++i) {
        if ((unsigned char)input[i] < 0x20 || (unsigned char)input[i] == 0x7f) return false;
    }
    memcpy(output, input + begin, length - begin);
    output[length - begin] = '\0';
    return true;
}

static void stock_call_release(void)
{
    taskENTER_CRITICAL();
    s_stock_call_active = false;
    taskEXIT_CRITICAL();
}

static void stock_watchlist_worker(void *argument)
{
    stock_mcp_job_t *job = argument;
    int status = service_stock_manage(job->action, job->query, job->text, sizeof(job->text));
    job->text[sizeof(job->text) - 1] = '\0';
    if (!job->text[0]) {
        const char *fallback = status == 0 ? "自选股操作成功。" : "自选股操作失败，请稍后再试。";
        memcpy(job->text, fallback, strlen(fallback) + 1);
    }
    cJSON_SetBoolValue(job->error, status != 0);
    /* The content string borrows job->text; the response API duplicates it
     * synchronously before publishing, so retain the job until that returns. */
    if (mcp_tool_call_result_response(job->id, job->result) != 0) {
        LOGE("stock MCP async response failed");
    }
    cJSON_Delete(job->result);
    lisa_mem_free(job);
    stock_call_release();
    /* lisa_thread_create's entry wrapper deletes the task and PSRAM stack. */
}

static cJSON *stock_watchlist_call(const char *id, const char *name, cJSON *args)
{
    const cJSON *action = mcp_tool_call_args_get(args, "action");
    const cJSON *query = mcp_tool_call_args_get(args, "query");
    if (!cJSON_IsObject(args) || !cJSON_IsString(action) || !action->valuestring ||
        (strcmp(action->valuestring, "add") != 0 && strcmp(action->valuestring, "remove") != 0 &&
         strcmp(action->valuestring, "list") != 0)) {
        return stock_result_text(name, "action必须是add、remove或list。", true, false);
    }
    unsigned action_count = 0, query_count = 0;
    for (const cJSON *entry = args->child; entry; entry = entry->next) {
        if (entry->string && strcmp(entry->string, "action") == 0) ++action_count;
        else if (entry->string && strcmp(entry->string, "query") == 0) ++query_count;
        else return stock_result_text(name, "只接受action和query参数。", true, false);
    }
    if (action_count != 1 || query_count > 1) {
        return stock_result_text(name, "不允许重复的action或query参数。", true, false);
    }
    bool list = strcmp(action->valuestring, "list") == 0;
    char parsed_query[STOCK_QUERY_BYTES_MAX + 1] = {0};
    if ((query && (!cJSON_IsString(query) || !trim_query(query->valuestring, parsed_query))) ||
        (!list && !parsed_query[0]) || (list && parsed_query[0])) {
        return stock_result_text(name, "添加或删除时请给出一只股票的名称或代码（最多80字节）；查看自选时请省略query。", true, false);
    }
    size_t id_length = bounded_length(id, STOCK_CALL_ID_BYTES_MAX);
    if (!id || !id_length || id_length > STOCK_CALL_ID_BYTES_MAX) {
        return stock_result_text(name, "MCP调用标识无效，请重试。", true, false);
    }
    taskENTER_CRITICAL();
    bool busy = s_stock_call_active;
    if (!busy) s_stock_call_active = true;
    taskEXIT_CRITICAL();
    if (busy) return stock_result_text(name, "正在处理上一条自选股请求，请稍后再试。", true, false);
    stock_mcp_job_t *job = lisa_mem_calloc(1, sizeof(*job));
    if (!job) {
        stock_call_release();
        return stock_result_text(name, "内存不足，未执行自选股操作。", true, false);
    }
    memcpy(job->id, id, id_length + 1);
    memcpy(job->action, action->valuestring, strlen(action->valuestring) + 1);
    memcpy(job->query, parsed_query, strlen(parsed_query) + 1);
    /* Build the complete result before mutating persisted user selections. */
    job->result = stock_result_text(STOCK_TOOL_NAME, job->text, true, true);
    if (!job->result) {
        lisa_mem_free(job);
        stock_call_release();
        return stock_result_text(name, "内存不足，未执行自选股操作。", true, false);
    }
    job->error = cJSON_GetObjectItemCaseSensitive(job->result, "isError");
    const lisa_thread_attr_t attributes = {
        .name = (uint8_t *)"stock.mcp",
        .stack_size = 12 * 1024,
        .priority = 4,
    };
    if (!lisa_thread_create(&attributes, stock_watchlist_worker, job)) {
        cJSON_Delete(job->result);
        lisa_mem_free(job);
        stock_call_release();
        return stock_result_text(name, "自选股任务启动失败，未执行操作。", true, false);
    }
    return NULL;
}

MCP_TOOL_DEFINE(stock_watchlist, stock_watchlist_list, stock_watchlist_call);
