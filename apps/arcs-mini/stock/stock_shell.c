#include "service_stock.h"
#include "service_stock_chart.h"
#include "stock_home.h"

#include <stdio.h>
#include <string.h>

#include "shell.h"
#include "lisa_mem.h"
#include "lisa_thread.h"
#include "mcp.h"
#include "voice_msg.h"

static void print_chart(void)
{
    stock_chart_snapshot_t *snapshot = lisa_mem_alloc(sizeof(*snapshot));
    if (!snapshot) return;
    service_stock_chart_snapshot(snapshot);
    char status[96];
    stock_home_status(status, sizeof(status));
    shellPrint(shellGetCurrent(), "%s\n", status);
    shellPrint(shellGetCurrent(), "chart=%s valid=%u date=%s points=%u seq=%lu\n",
               snapshot->symbol, snapshot->chart.valid, snapshot->chart.date,
               snapshot->chart.count, (unsigned long)snapshot->sequence);
    shellPrint(shellGetCurrent(), "chart loading=%u failed=%u error=%s\n",
               snapshot->loading, snapshot->failed, snapshot->error[0] ? snapshot->error : "none");
    lisa_mem_free(snapshot);
}

static void print_status(void)
{
    stock_snapshot_t snapshot;
    service_stock_snapshot(&snapshot);
    shellPrint(shellGetCurrent(), "state=%s network=%u clock=%u seq=%lu count=%lu revision=%lu next=%lus error=%s\n",
               snapshot.state == STOCK_STATE_READY ? "ready" :
               snapshot.state == STOCK_STATE_ERROR ? "error" : "loading",
               snapshot.network_connected, snapshot.clock_available,
               (unsigned long)snapshot.sequence, (unsigned long)snapshot.count, (unsigned long)snapshot.revision,
               (unsigned long)snapshot.next_poll_seconds, snapshot.error[0] ? snapshot.error : "none");
    for (unsigned i = 0; i < snapshot.count; ++i) {
        stock_quote_t *quote = &snapshot.quotes[i];
        shellPrint(shellGetCurrent(), "%s name=%s valid=%u price=%.2f change=%+.2f percent=%+.2f "
                   "source_time=%s %s stale=%u scheduled=%u\n",
                   snapshot.instruments[i].symbol, snapshot.instruments[i].name, quote->valid,
                   quote->price, quote->change, quote->change_pct, quote->valid ? quote->time_text : "unknown",
                   snapshot.instruments[i].market == STOCK_MARKET_CN ? "CST" : "ET", snapshot.stale[i], snapshot.scheduled[i]);
    }
}

typedef struct {
    Shell *shell;
    char action[8];
    char query[STOCK_QUERY_MAX + 1];
} stock_shell_job_t;

static void manage_worker(void *arg)
{
    stock_shell_job_t *job = arg;
    char *text = lisa_mem_alloc(2048);
    if (text) {
        int status = service_stock_manage(job->action, job->query, text, 2048);
        shellPrint(job->shell, "\nstock result=%d %s\n", status, text);
        lisa_mem_free(text);
    } else shellPrint(job->shell, "\nstock: no memory\n");
    lisa_mem_free(job);
}

static void print_tool(void)
{
    extern const struct mcp_tool __mcp_tool_start[], __mcp_tool_end[];
    for (const struct mcp_tool *tool = __mcp_tool_start; tool < __mcp_tool_end; ++tool) {
        if (strcmp(tool->name, "stock_watchlist") != 0) continue;
        cJSON *info = tool->list(tool->name);
        char *json = info ? cJSON_PrintUnformatted(info) : NULL;
        shellPrint(shellGetCurrent(), "%s\n", json ? json : "stock tool schema allocation failed");
        cJSON_free(json);
        cJSON_Delete(info);
        return;
    }
    shellPrint(shellGetCurrent(), "stock_watchlist not registered\n");
}

static int stock_command(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        print_status();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "tools") == 0) {
        print_tool();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "chart") == 0) {
        print_chart();
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "next") == 0 || strcmp(argv[1], "auto") == 0)) {
        voice_msg_button_evt_t event = {
            .button_id = 0,
            .action = strcmp(argv[1], "next") == 0 ? VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK :
                                                      VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK,
        };
        voice_msg_pub(VOICE_MSG_BUTTON_CHANGE, &event, sizeof(event));
        return 0;
    }
    if ((argc == 2 && strcmp(argv[1], "list") == 0) ||
        (argc == 3 && (strcmp(argv[1], "add") == 0 || strcmp(argv[1], "remove") == 0))) {
        if (argc == 3 && strlen(argv[2]) > STOCK_QUERY_MAX) return -1;
        stock_shell_job_t *job = lisa_mem_calloc(1, sizeof(*job));
        if (!job) return -1;
        job->shell = shellGetCurrent();
        snprintf(job->action, sizeof(job->action), "%s", argv[1]);
        if (argc == 3) snprintf(job->query, sizeof(job->query), "%s", argv[2]);
        const lisa_thread_attr_t attr = {.name = (uint8_t *)"stock.shell", .stack_size = 12 * 1024, .priority = 4};
        if (!lisa_thread_create(&attr, manage_worker, job)) {
            lisa_mem_free(job);
            return -1;
        }
        return 0;
    }
    shellPrint(shellGetCurrent(), "Usage: stock [status|chart|next|auto|list|tools|add <name/code>|remove <name/code>]\n");
    return -1;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 stock, stock_command, Stock watchlist status and diagnostics);
