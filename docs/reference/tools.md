# 工具参考

[文档首页](../README.md) · [工具调用流程](../architecture/tool-calling-flow.md) · [Hooks 流程](../architecture/hooks-flow.md) · [功能全览](feature-index.md) · [命令参考](commands.md) · [扩展指南](../features/extensions/README.md) · [安全模型](../development/security.md)

本页按当前主线源码整理。工具由名称、说明、JSON Schema、确认属性和执行函数组成。模型只能调用当前注册且已挂载的工具；`/tools` 可查三态。

## 工具怎样进入请求

1. 核心工具启动即注册。
2. `search`、`lsp`、MCP 等按配置决定是否注册。
3. Agent、Todo、交互提问和项目记忆按运行模式再补。
4. Lua、DLL 与 MCP 工具动态挂载。
5. 总数超过 `tool_search_threshold` 时，延迟工具只留下索引；模型先调 `tool_search`，命中后下一轮请求才带完整 schema。

主代理与子代理各有一张 registry。MCP、LSP、Skill 和读写搜索等可进两边；`agent`、`todo_write`、`ask_user` 只属于主代理，免得递归失控或子代理抢终端。

## 可用性总表

| 工具 | 条件 | 改外部状态 | 默认确认 |
| --- | --- | --- | --- |
| `read_file` | 恒在 | 否 | 否 |
| `search` | 恒在 | 否 | 否 |
| `write_file` | 恒在 | 写盘 | 是 |
| `edit_file` | 恒在 | 写盘 | 是 |
| `run_command` | 恒在 | 可任意执行 | 是 |
| `background_output` | 恒在 | 读后台任务台账与日志 | 否 |
| `stop_background` | 恒在 | 终止后台任务 | 否 |
| `skill` | 恒在；无 Skill 时会回空清单错误 | 读取 Skill 资源 | 否 |
| `web_fetch` | 恒在 | 发 HTTP GET | 否 |
| `web_search` | 配置 `search` | 发搜索请求 | 否 |
| `lsp` | 配置 `lsp` | 启动语言服务器 | 否 |
| `agent` | 主代理 | 调模型与子工具 | 跟随子工具 |
| `todo_write` | 主代理 | 改会话内清单 | 否 |
| `ask_user` | 真交互入口 | 等用户选择 | 否 |
| `memory_save` | 项目记忆 generate 开启 | 排后台写入 | 否 |
| `tool_search` | 存在延迟工具 | 改本场挂载集合 | 否 |
| `mcp__*` | MCP 握手成功 | 由服务决定 | 是 |
| `plugin__*` | Lua/DLL 加载成功 | 由插件决定 | 是 |

`settings.local.json`、会话“总是允许”和确认档可改变实际确认行为。详见[配置手册](configuration.md#七settingslocaljson项目级本地权限)。

后台代理的写操作须预放行：后台子代理没有可弹确认的终端，`needs_confirm` 的工具（`write_file`、`edit_file`、`run_command` 等）一律当场拒绝——主会话导航坞会立刻弹一条 toast 并在 transcript 记事件（含 `/permissions` 预放行的出路），但重试仍会被拒。要让后台代理干活，先在 `/permissions`（写进 `settings.local.json` 的 allow）预放行对应工具，或让它前台跑。

## 文件工具

### `read_file`

参数：

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `path` | 是 | 相对或绝对文件路径。 |
| `offset` | 否 | 起始行，1 基；小于 1 按 1。 |
| `limit` | 否 | 最多行数；默认 2000。 |

输出每行带行号。单次最多约 1 MiB；行数或字节到顶后会写明最后一行与下一次 `offset`。空文件、目录、无权限、offset 越界都有明确结果。

### `write_file`

参数：`path`、`content` 都必填。内容按 UTF-8 字节整体写入。父目录不存在会创建；目标文件存在便覆盖。适合新文件或整篇重写，小改动应交给 `edit_file`。

确认前终端会生成 diff。工具成功后报告字节数，并标明是否覆盖旧文件。

### `edit_file`

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `path` | 是 | 已有文件。 |
| `old_string` | 是 | 要替换的原文，不能为空。 |
| `new_string` | 是 | 新内容，可为空。 |
| `replace_all` | 否 | 默认 false，要求唯一命中；true 替换全部。 |

匹配按三层走：

1. 字节级精确匹配。
2. 统一 CRLF/LF/CR 后匹配。
3. 忽略整块统一缩进与行尾空白。

任一层出现多个候选，且未设 `replace_all=true`，便拒绝猜测并报告位置。三层都找不到时，提示先读最新文件片段。写入前同样画 diff。

## 搜索工具

### `search`

有两种模式：

```jsonl
{"mode":"grep","pattern":"AgentLoop","path":"src","glob":"*.cpp"}
{"mode":"glob","pattern":"docs/**/*.md","path":"."}
```

| 参数 | 说明 |
| --- | --- |
| `mode` | `grep` 或 `glob`。 |
| `pattern` | grep 用 ECMAScript 正则；glob 支持 `*`、`?`、`**`。 |
| `path` | 根目录，默认 cwd。 |
| `glob` | 仅 grep：先按文件名/相对路径筛文件。 |

不带 `/` 的 glob 按文件名递归匹配；带 `/` 的按相对路径匹配。自动跳过 `.git/`、`build/`、`node_modules/` 和二进制文件。最多返回 100 条，超出会标注。

### `lsp`

| mode | 参数 | 结果 |
| --- | --- | --- |
| `definition` | `file`、`line`、`character` | 定义位置与目标行。 |
| `references` | 同上 | 引用位置列表。 |
| `symbols` | `file` | 文件符号树。 |
| `diagnostics` | `file` | 错误、警告与提示；最多等服务器 2 秒。 |

行列都是 1 基。文件扩展名先路由到配置的语言；工具读盘后发 `didOpen`。服务懒启动，闲置后关闭，下次再起。

## 命令工具

### `run_command`

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `command` | 必填 | 一整条 shell 命令。 |
| `timeout_ms` | 120000 | 前台超时；后台模式忽略。 |
| `shell` | Windows `powershell`；POSIX `sh` | Windows 也可选 `cmd`。 |
| `run_in_background` | false | true 时 spawn 后立即返回 PID 与日志路径。 |

前台模式合并 stdout/stderr，附退出码。输出达到 2 MiB 会终止进程树并标注截断；超时也收整棵树。Windows PowerShell 命令用 UTF-16LE `-EncodedCommand`，减少引号和中文转义故障。

后台模式把输出写进系统临时目录的日志文件。会话退出时，Windows Job Object 或 POSIX 进程注册表负责收尾。查看与停止仍走普通 tool-use 往返，不另开一套 wire 协议。

后台任务已经有两只普通工具接进同一套 tool-use 协议：

- `background_output`：不传 `task_id` 列任务；传 id 时查状态，并读日志尾部，默认 50 行、最多取末尾 64 KiB。
- `stop_background`：按 `task_id` 收掉任务。Windows 当前终止根进程；POSIX 向独立进程组发信号。

后台 watcher 会在任务转入终态后排一条完成通知。Windows 能取得准确退出码；POSIX 脱离会话后通常只能认出进程已死，退出码记作 `-1`。更深的 shell、超时、杀树与平台差异见[文件读取与命令执行深挖](../architecture/tools/file-commands.md)。

## 网络工具

### `web_fetch`

参数：`url` 必须是 HTTP(S)，`max_bytes` 默认 102400。跟随重定向；HTML 剥标签，文本原样返回；NUL 二进制内容拒收。返回头含最终 URL、HTTP 状态、Content-Type 与截断状态。超时 30 秒。

它不是浏览器：不执行 JavaScript，不保登录态，不点按钮。动态站点应换 API、MCP 浏览器服务或人工提供内容。

### `web_search`

参数：`query` 必填，`count` 默认 5、上限 10。当前适配 Tavily、Brave、Serper。工具返回编号、标题、URL 与摘要；深读正文再交给 `web_fetch`。

只有配置 `search.provider` 与 `search.api_key` 才注册。它和模型服务端原生搜索互不替代：前者是本地工具调用，后者是协议请求里的厂商能力。

## 工作流工具

### `agent`

`prompt` 必填，必须自包含；子代理看不见主对话。

步数预算不在工具入参里，模型定不了——要限步走配置 `subagent.max_steps_per_turn`（未设则继承 `max_steps_per_turn`，默认 `0` = 不限步）。这道口子早先是敞开的，实测模型见字段就填，一趟深挖的活被自己掐到十来步就 `budget_exhausted` 收场，等于把配置里"不限步"的默认悄悄夺了。默认不限步是有意为之：防跑飞靠 ESC/Ctrl+C，不靠模型自己拍一个数。

解析层仍收 `max_steps_per_turn`（旧名 `max_turns` 兼容，两者同现取新名）：手写 JSON、老脚本、测试照旧能用，`0` 表示不设上限，负数拒绝。

子代理有独立历史、独立工具表与同一模型后端。其流式碎念不回主屏，只显示子工具状态，末尾把结论交回。主 Esc/Ctrl+C 会透传取消；pre/post hooks 与工具确认照常生效。

适合：大目录检索、通读多份文件、调研后只需短结论。不要把一件本可直接读两页文件的小事绕给子代理。

### `todo_write`

参数 `items` 是完整清单，不是增量。每项包含 `content` 和 `pending / in_progress / completed`。空数组清空。所有项先校验，全部合法才替换，免得留下半张表。

终端首次显示 `todo_write(N 项)`；后续显示 `todo_update(N 项)`。项数不变时原位点亮变化项，增删项时另起新块。

### `ask_user`

一次 1 到 4 题，每题 2 到 4 个选项，可设 `multi_select`。界面自动补“自己填写”。只用来问会改变实现方向、又查不明的选择；不该拿它问可从仓库读出的事实。

管道和单发模式不挂此工具。Esc 取消后，工具返回取消结果，不在输入行里死等。

## Skill 与延迟工具

### `skill`

按技能名读取 `SKILL.md`，把正文与资源基准目录交给模型。扫描兼容 Agent Skills 的用户级、项目级 `.agents/skills`，也保留 `.lubancode/skills`。同名优先级是项目 > 用户 > 官方；同层里 `.lubancode` > `.agents`。管理命令由 `/skill` 负责，模型工具只负责使用，不负责暗中安装。

### `tool_search`

参数 `query` 是空格分隔关键词，`limit` 默认 5。它在延迟工具的名字和说明里评分，命中后把这些工具加入本场 loaded 集合；下一轮请求便带完整 schema。没命中时会给名字前缀建议。

### `programmatic_tool_calling`

只在 `tool_calling=programmatic` 且 Python、平台围栏与工具白名单满足条件时注册。参数是一段 Python 脚本与用途说明。脚本从 `luban_tools` 导入 typed stub，用 `emit()` 交回一份摘要；每枚 stub 调用经 framed RPC 回宿主，再走与普通 JSON 工具相同的 schema、Hook、确认、取消与审计链。

首版入选集只收 `read_file`、`search` 等配置允许的只读工具。POSIX 没有可靠文件系统/网络隔离时默认回落 JSON。完整配置、runner 上限与手测见 [PTC 手册](../features/tools/ptc.md)。

## 动态工具

### MCP

命名为 `mcp__<server>__<tool>`。说明与 schema 来自服务器 `tools/list`。调用通过长命 stdio JSON-RPC 进程完成。默认要求确认，实际副作用由服务定义。

### Lua / DLL

命名为 `plugin__<stem>__<tool>`。schema 与执行函数由插件提供。默认要求确认。Lua 与 DLL 都在宿主进程内运行；崩溃、死循环和内存破坏会直接拖住主程序。

### `memory_save`

只在项目记忆写入开启时出现。参数含 `kind`、可选稳定 `id`、`title`、`summary`、`content`、`keywords`、`paths`。它只排后台任务，不在工具回调里直接改 Markdown。

不得保存当前进度、未经核验的猜测、网页/MCP 原文、日志、密钥或个人数据。详见[项目记忆](../architecture/memory/design.md)。

## 工具结果怎样进入会话

- 运行态、终态、参数、摘要与截断后的全文先进入 transcript。
- 工具结果作为中立 `ToolResultBlock` 写回 Agent 历史，再由各协议编码。
- 会话 JSONL 保留工具事件；Markdown 导出保留可读摘要。
- 终端紧凑档可隐藏子工具，但数据仍在 transcript；`Ctrl+O` 或 `Ctrl+E` 可重看。
- 输出进 JSON 前会清洗非法 UTF-8，避免 Windows 代码页或外部程序字节把请求序列化打坏。

## 排错

| 现象 | 先查什么 |
| --- | --- |
| 模型说没有某工具 | `/tools` 看是否未注册或仍在 deferred；必要时让模型用 `tool_search`。 |
| MCP 工具没出现 | `/mcp` 看进程、握手和 `tools/list`；服务器 stdout 不得写日志。 |
| LSP 报扩展名无配置 | 检查 `lsp.<language>.extensions` 与目标文件后缀。 |
| 命令中文乱码 | Windows 优先用 PowerShell；外部程序若硬吐本地代码页，检查它自身编码。 |
| edit 多处匹配 | 扩大 `old_string` 上下文，或确认确实要全改后设 `replace_all=true`。 |
| 工具每次都问 | 看确认档、`needs_confirmation`、项目 settings 与会话“总是允许”。 |
| 网页只有空壳 | 页面依赖 JavaScript；换 API、浏览器 MCP 或让用户提供静态正文。 |
