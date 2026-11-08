# 生生：像素宠物与语音助手

目标是“你好生生”唤醒、常驻宠物形象以及语音对话；查询资料与电脑 Codex 接入作为后续阶段。

新增的股票展示版本见 [生生股票盯盘](stock-watch.md)：待机显示乐鑫科技和 NVDA，
说话时回到语音界面。本轮仅看行情，不启用提醒。

## 当前实现

`apps/arcs-mini/shengsheng.conf` 启用 Mini3 专用的 `CONFIG_SHENGSHENG_AVATAR`。
原生语音桌面从固件内读取生生像素动画，替代道长形象，开机即可显示。
现有语音会话、字幕、相机展示、音乐、闹钟及系统电源策略继续由原 Home 页面管理。
形象响应待机、聆听、等待回复、说话、睡意及情绪表情。

这条路径不启动 Lua 运行器，语音会话不会添加 `mode=miniapp`，也不触发小应用的工具白名单。
功能键单击遵循原桌面规则开始／停止交互；暗屏后的第一击可能仅恢复亮度。

养成小应用 `miniapps/tamagotchi.lua` 的显示名字也统一为生生，但保留原应用 ID、
上传路径和存档格式。原生桌面目前只显示形象，不读取养成属性；从语音桌面进入养成
小应用后，仍是该小应用原有的按键和语音模式。二者的养成状态联动属于后续工作。

构建与部署命令见 [miniapps/README.md](../apps/arcs-mini/miniapps/README.md)。
不启用该配置即可构建原版桌面；SDK、AP、Boot 和资源分区不需要为换形象而修改。

## “你好生生”唤醒

目标唤醒词已确定为四字“你好生生”。当前实现没有宣称其已经可用。

设备的离线唤醒由 AP 声学算法和 `wake_word` 资源中的模型决定。
`CONFIG_APP_WAKE_WORD_DEFAULT`、KV 唤醒词或 UI 中显示的中文只影响提示文字，
不能把现有模型变成识别新短语的模型。为避免屏幕提示无法使用的唤醒词，
本次不将这些字段直接改成“你好生生”。当前仍可使用设备实际提示的唤醒词，
或单击功能键对话。

### 官方云端生成流程

当前仓库未提供从一句中文直接训练／导出该模型的本地工具。官方 `ling` 已提供
应用级唤醒词云端生成接口，可生成“你好生生”，随后分配给已绑定应用的角色。
这与只修改中文提示不同，生成的资源还需要通过设备模型兼容性与实机识别验证。

流程依据已核对的官方源码版本
[`f0263424cf116730a44118059d4860cd4f239b99`](https://github.com/LISTENAI/ling/tree/f0263424cf116730a44118059d4860cd4f239b99)：

- [安装与登录](https://github.com/LISTENAI/ling/blob/f0263424cf116730a44118059d4860cd4f239b99/README.md#L19)。
- [唤醒词命令和测试配置范围](https://github.com/LISTENAI/ling/blob/f0263424cf116730a44118059d4860cd4f239b99/README.md#L97)。
- [参数、异步状态及可能收费说明](https://github.com/LISTENAI/ling/blob/f0263424cf116730a44118059d4860cd4f239b99/skills/ling/references/commands.md#L162)。

需要 ListenAI 平台账号的应用管理 API Key。设备 SID、电脑登录密码与该 API Key
不是同一种凭据；当前准备环境尚未配置此平台 API Key。用户应在本机终端运行
`ling login` 并输入来自 <https://platform.listenai.com/keys> 的密钥，不应将其写入本文件、
聊天或共享日志。登录后先查询账号、应用与现有唤醒词，避免重复生成：

```sh
ling --version
ling login
ling account
ling app list
ling app --product-id <PRODUCT_ID> role list
ling app --product-id <PRODUCT_ID> wakeword list
ling app --product-id <PRODUCT_ID> role wakeword show <ROLE_ID>
```

确认目标是设备当前绑定的应用及使用中的角色后，才准备生成任务：

```sh
ling app --product-id <PRODUCT_ID> wakeword generate '你好生生' \
  --sensitivity medium --response '我在，我是生生'
```

**生成可能收费，尚未提交生成任务。** CLI 会先询问
“生成唤醒词「你好生生」可能产生费用。确认提交生成任务？”，公开命令与源码未提供
具体金额报价。当前改名需求未包含不明费用授权；应先向用户说明实际可获得的费用信息，
取得相应授权后再提交。非交互环境需要 `--yes`，不能将该参数当作已有费用授权。
唤醒词最多 12 字符，灵敏度可选 `low`／`medium`／`high`，应答语最多 5 条、每条最多
12 字符；本示例均符合 CLI 参数约束。

生成任务返回 ID 后，用下列只读命令查询状态：

```sh
ling app --product-id <PRODUCT_ID> wakeword show <WAKEWORD_ID>
```

状态依次可能为等待生成、生成中、可用或生成失败；失败为终态，不应持续轮询。
仅在状态为可用（`ready`）时，才能切换角色使用的新唤醒词：

```sh
ling app --product-id <PRODUCT_ID> role wakeword set <ROLE_ID> <WAKEWORD_ID>
```

该命令只更新应用的**测试配置**，生产配置需要通过平台正常发布流程同步。
云端人设、声音和应答称呼也需要在该应用中统一为生生；桌面皮肤不改变云端提示词。
本轮未修改云端人设、角色默认值或唤醒词配置。

### 设备资源更新与验收

设备已有唤醒资源 OTA 链路：联网后检查云端设备配置，读取 `wakeup_word.resources`
中 `wake_word.bin` 的地址、大小、MD5 与文本，下载并写入唤醒词分区，同步
`sys.wakeword`，随后重启加载模型。当前 Mini3 构建已启用 `CONFIG_OTA`，未在编译期
禁止唤醒词更新；实际生效仍取决于角色／测试设备配置、平台发布范围、联网状态以及
运行时 `user.disable_wakeword_update` 是否禁用该更新。

生成完成、配置切换并确认资源可下发后，保持设备 USB 供电与联网，再重启触发检查。
应答语与角色唤醒词修改同样需要设备重启。抓取日志确认新资源下载、校验、写入和重启
成功后，在常规启动下测试“你好生生”唤醒、误唤醒、远近距离识别、字幕及语音回复；
不能仅以 UI 出现新文字作为完成依据。

相关代码：`src/system/platform.c` 加载 `cae_esr.bin` 和 `wrap.json`，
`src/middleware/ota/ota_api.c` 解析资源，`src/middleware/ota/ota_manager.c` 下载并写入。
如果改用离线资源部署，需要获得适配当前 AP 引擎的模型和配置，再按现有 `wake_word`
ROMFS 结构打包，验证资源尺寸与模型兼容性；`tools/romfs.py` 仅负责打包已有文件。

## 以后让生生查资料

推荐将电脑作为中转：

```mermaid
flowchart LR
    A[生生：录音与唤醒] --> B[现有语音识别与意图路由]
    B --> C[电脑上的查询服务]
    C --> D[Codex SDK 或 codex exec]
    D --> E[回答文本及来源]
    E --> F[生生：字幕和语音播报]
```

电脑运行 Codex 和查询服务，设备通过 HTTP 提交查询。SSH 用于部署和维护电脑服务。
这种方式复用电脑上已配置的 Codex，端侧无需保存 SSH 密码或搬运 Codex 登录凭据。
Codex CLI 及 SDK 的运行环境不适合这块 MCU；若选择端侧直接访问模型 API，
也仍需处理认证、流式响应、语音识别／合成和查询工具，而不是将 CLI 放到板子上执行。

初版建议仅接“帮我查……”意图，采用任务 ID、轮询和取消，容纳几十秒以上的查询时间；
长结果分段播报，并保留来源链接。语音文本如何送入电脑服务，需要在现有云端意图链路
或固件的识别结果处理处增加明确路由。仅启动电脑服务不能视为语音查询已打通。
搜索能力还取决于电脑实际的账号、模型／provider 与搜索配置，本轮未调用用户的 Codex。

OpenAI Docs 已核对的官方资料：

- [Non-interactive mode](https://learn.chatgpt.com/docs/non-interactive-mode)：用 `codex exec` 从程序调用，支持 JSON 输出。
- [Codex SDK](https://learn.chatgpt.com/docs/codex-sdk)：电脑应用集成与会话管理。
- [Web search](https://learn.chatgpt.com/docs/web-search)：查询搜索配置与使用方式。

本轮没有实现或启动电脑查询服务，没有接入新的模型 API。
