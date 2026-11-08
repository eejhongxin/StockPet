# 生生股票行情后台服务

首次启动默认乐鑫科技（`sh688018`，人民币）和英伟达（`usNVDA`，美元）。
自选股支持语音添加／删除／查看，上限 6 只，保存在 `stock.watch.v1` KV。
服务只显示行情，不交易、不发提醒；自选管理结果由原语音会话回复，不需要电脑中转或行情 API Key。

## 接口与线程

- `service_stock_init()`：从应用主任务初始化一次，重复调用返回成功。
- `service_stock_snapshot()`：复制小型快照，不做网络 I/O，不等待互斥锁；可从 UI 任务调用。
- `service_stock_manage()`：同步的 add/remove/list 服务接口，只能从工作线程调用。新增先搜索再验证报价，保存成功才发布；MCP `stock_watchlist` 在独立线程异步调用它。
- `stock_quote_parse_instrument()`：通用、纯 C、长度受限的 Tencent 文本解析器；拒绝截断、无穷值、
  非法日期、错误股票 ID 和数值不一致，失败不修改目标。
- 行情网络请求独占低优先级任务，12 KiB 栈从 LISA PSRAM 分配。网络请求在临界区外执行。
- 使用当前 HTTP SDK 的底层会话 API 有界流式读取，而非按服务器声明长度分配整包。
  HTTP 状态必须 200，拒绝重定向，报价响应上限 16 KiB，搜索 8 KiB，整轮操作约 12 秒预算，
  HTTP timeout 单位实际为秒；不记录原始响应。

## 行情来源与新鲜度

报价来自 `https://qt.gtimg.cn/q=<自选代码>`；名称查询来自腾讯 smartbox，UTF-8 URL 编码，响应 Unicode 名称经 JSON 解码。这是公开行情端点，并非承诺可用性或
延迟的交易所实时数据订阅。接口可能变更、限流或停止服务。美股应始终显示“可能延时”。
返回内容含 GBK 股票名称，本模块忽略名称字段，使用搜索返回的 UTF-8 显示名（截断到完整 Unicode 字符，屏幕必要时省略）。

A 股行情时间按北京时间转换为 UTC；美股字段按纽约本地时间转换，按 2007 年起的
美国夏令时规则处理。屏幕应保留行情源的日期和时间，不能将 HTTP 抓取成功时间冒充行情时间。

按周一至周五的常规交易时段决定请求频率：A 股 09:30–11:30、13:00–15:00（北京时间），
美股 09:30–16:00（纽约时间）；任一市场在常规时段时约 15 秒一轮，否则约 60 秒。
请求耗时另计。不包含交易所节假日、半日市、临时停牌或美股盘前／盘后状态推断。
常规时段只是调度依据，不能据此断言市场当时正在交易。

行情比系统当前时间旧 180 秒，或比系统时间领先超过 60 秒时，快照标记 `stale`。
休市看到旧行情是正常行为，UI 仍应保留其日期和时间；常规时段旧行情需要明确提示。
失败保留旧的有效价格并标记错误，退避间隔为 30、60、120、240 秒；网络恢复时立即重试。
系统未校时时 `clock_available=false`，不会伪造刷新时间或推断交易状态。

本模块没有付费行情接入和服务器证书校验策略覆盖；HTTPS 的具体行为沿用现有 SDK。

## 主机验证

从仓库根目录执行：

```sh
gcc -std=c11 -Wall -Wextra -Werror -pedantic -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I apps/arcs-mini/stock \
  apps/arcs-mini/stock/stock_quote.c \
  apps/arcs-mini/stock/tests/test_stock_quote.c -lm -o tmp/stock-quote-test
./tmp/stock-quote-test tmp/stock-probe.bin
```

最后一个参数为可选的真实 GBK 响应文件。测试包含非法数值和公历日期、闰年、数值一致性、
不完整／重复记录、夏令时、交易时段边界、周末、旧行情边界，并对真实响应做逐字节变异
和逐长度截断，在 ASan／UBSan 下验证解析边界。

## 自选股一致性

- 持久化记录有版本魔数、数量、固定结构和校验和；装载时验证代码、名称边界和去重，损坏记录回退默认列表。
- 写 KV 成功后才更新运行列表；保存失败会返回错误，不宣称添加或删除成功。
- 每次编辑增加 revision，旧网络请求回调仅在 revision 仍相同时发布，避免删除后被在途请求恢复。
- 删除匹配优先准确代码，多个同名时要求代码；新增重复项不写 Flash。
- 名称匹配失败或歧义均不改列表，新增还必须通过真实行情解析，不能只凭语音模型给的代码认定有效。
- UI 每页一只分时图、默认手动；双击下一只、三击开关 8 秒自动轮换。语音／TTS 时让出原 Home；空列表显示添加提示。
- `stock_lookup.c`、`stock_http.c` 分别负责搜索解析和限定腾讯域名的有界请求。

通用报价回归含两组真实响应；名称搜索测试位于 `tests/test_stock_lookup.c`。


## 分时曲线与缓存

`stock_chart_parse()` 解析腾讯 `/appstock/app/minute/query`（A股）及大小写敏感的
`/appstock/app/UsMinute/query`（美股）。长度限 16 KiB，输出至多 420 个含真实分钟的点。
输入必须是完整 JSON、匹配准确股票身份、真实日期和严格递增的分钟，价格有限且为正。
仅保留常规交易时间；UI 不跨午休或缺失分钟连线，不外推单点。

`service_stock_chart_init()` 启动独立 12 KiB 栈工作线程；`select(symbol)` 与 `snapshot(out)`
是非阻塞快照接口。6 个缓存仅存源数据，后台只请求当前选股；15/60 秒周期和失败退避与报价相似。
成功数据不可倒退到更早日期／分钟，错误保留原图。选择改变会立即替换为匹配的缓存或空白等待态，
在途请求完成只在所选代码仍匹配且股票仍在自选列表时发布，防止串股。

`stock_home_handle_button()` 从按钮线程仅提交临界区标志；LVGL 操作在原 UI timer 内完成。
行情面板退场、Home 退出和语音开始时不接管按钮。图形按数据序号更新，生生眨眼不重建图形。

解析测试：

```sh
gcc -std=c11 -Wall -Wextra -Werror -pedantic -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I apps/arcs-mini/stock -I arcs-sdk/modules/cjson \
  apps/arcs-mini/stock/stock_chart.c apps/arcs-mini/stock/stock_quote.c \
  arcs-sdk/modules/cjson/cJSON.c apps/arcs-mini/stock/tests/test_stock_chart.c \
  -lm -o tmp/stock-chart-test
./tmp/stock-chart-test tmp
```

最后参数是可选真实响应目录，包含 `stock-chart-cn.json`、`stock-chart-nvda.json` 等。
无真实响应仍测试合成边界、逐次分配失败、JSON 深度／语法／UTF-8、重复键和非法分钟／价格／日期。
