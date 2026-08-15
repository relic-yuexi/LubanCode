# LubanCode 配置

[文档首页](README.md) · [功能总览](feature-reference.md) · [命令手册](commands.md) · [Provider 目录](provider-catalog.md) · [扩展指南](extensions.md) · [架构说明](architecture.md)

lubancode 要跟大模型对话,得知道 `wire`(协议)、`base_url`、`api_key`、`model` 这几件事。本文档核实自 `src/config/config.hpp`、发行包 `skills/lubancode-config/SKILL.md` 与 `lubancode --help` 的真实输出,字段名与语义以代码为准。

## 先跑起来

最省事的路，是直接运行 `lubancode`，跟着初次向导走。若要手写一份最小配置，可放在 `~/.lubancode/config.json`：

```json
{
  "wire": "responses",
  "base_url": "https://your-provider.example/v1",
  "api_key": "sk-...",
  "model": "your-model"
}
```

不想让密钥落盘，就把 `api_key` 换成环境变量 `LUBANCODE_API_KEY`。要管多家服务，优先用下文 `providers` 数组与 `key_env`。

### 一份较完整的例子

下面这份把常用块摆在一处。它不是必抄模板。用到哪段，留哪段。

```json
{
  "active_provider": "openai",
  "theme": "dark",
  "language": "zh-CN",
  "think": "medium",
  "context_window": "256k",
  "compact_model": "",
  "max_context_chars": 600000,
  "max_turns": 0,
  "tool_search_threshold": 20,
  "connect_timeout_ms": 15000,
  "stream_idle_timeout_secs": 60,
  "request_timeout_secs": 30,
  "status_panel": {
    "items": ["permission_mode", "provider", "model", "cwd", "git_branch", "context", "tokens"],
    "separator": " · "
  },
  "providers": [
    {
      "name": "openai",
      "base_url": "https://api.openai.com/v1",
      "wire": "responses",
      "key_env": "OPENAI_API_KEY",
      "model": "gpt-5.4",
      "context_window": "256k"
    }
  ],
  "memory": {
    "enabled": false,
    "use": true,
    "generate": true,
    "max_index_bytes": 16384,
    "max_retrieval_bytes": 24576,
    "max_results": 4
  },
  "hooks": {
    "pre_tool": [],
    "post_tool": [],
    "session_start": [],
    "session_end": []
  },
  "mcpServers": {},
  "lsp": {}
}
```

密钥由 `OPENAI_API_KEY` 提供，不写进 JSON。`memory.enabled` 此处故意保持 `false`；看清数据边界后再开。

## 一、配置分层与优先级

配置文件按**两级目录**各放一份,两级都读、**按字段**逐个合并(不是整份谁盖谁):

- **项目级**:当前目录 `<cwd>/.lubancode/config.json`。
- **全局**:用户主目录 `~/.lubancode/config.json`(Windows 是 `%USERPROFILE%`)。

每一级各自还认旧位置 `<目录>/.lubancode.json`——读到旧位置、且对应新位置尚无文件时,lubancode 启动时会自动把它迁移到新位置,并打一行"配置已迁移到 ..."通知;搬家失败也不中断程序,照旧用回旧文件。

**只在主目录里跑**(`cwd` 就是主目录)时,只当项目级一份读,不重复。

字段按**五级**逐个决,高到低:

1. **`LUBANCODE_*` 专属环境变量**。
2. **项目级** `config.json`。
3. **全局** `config.json`。
4. **通用环境变量**(向后兼容,跟 Claude Code、Codex 等工具共用同名变量容易撞车,建议改用第 1 级):`wire=anthropic` 时读 `ANTHROPIC_BASE_URL`/`ANTHROPIC_AUTH_TOKEN`/`ANTHROPIC_MODEL`;`wire=responses` 或 `chat_completions` 时读 `OPENAI_BASE_URL`/`OPENAI_API_KEY`/`OPENAI_MODEL`。
5. **内置默认值**。

逐字段合并:项目级写了某字段就用项目级那一份,项目级缺的字段回退全局,全局也缺再往下一级找。`hooks`、`mcpServers`、`search`、`lsp`、`status_panel` 这几段是**整段回退**(不做键级混合)——项目级写了 `hooks` 就用项目级那一整段 `hooks`,否则用全局那一整段。`tool_search_threshold`、`connect_timeout_ms`、`stream_idle_timeout_secs`、`request_timeout_secs` 只从配置文件(项目级 > 全局)或内置默认值来,没有环境变量这一级。

`/config`(或 `lubancode --config`)会打出每个字段最终来自哪一级,排查配置问题用。

### 为什么要有专属环境变量

不少人机器上已经装了 Claude Code、Codex 之类的工具,全局环境变量里早设好了 `ANTHROPIC_BASE_URL`、`ANTHROPIC_AUTH_TOKEN`——那是给那些工具专用的中转服务配的,lubancode 要是也去读,轻则连错服务,重则被中转拒之门外。推荐直接用 `LUBANCODE_*` 专属变量,或放一份配置文件,不跟别的工具打架。

## 二、config.json 字段表

`config.json` 顶层须是 JSON object;字段可只写一部分,缺的往下一级找。

| 字段 | 类型与取值 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `wire` | `anthropic` / `responses` / `chat_completions` | `anthropic` | 选择 Anthropic Messages、OpenAI Responses 或 OpenAI Chat Completions 兼容接口。`chat` 也认，写回时统一成 `chat_completions`。 |
| `base_url` | 字符串 | 无内置默认值 | 模型服务根地址。 |
| `api_key` | 字符串 | 无内置默认值 | 模型服务认证值,别提交进仓库。 |
| `model` | 字符串 | 无内置默认值 | 发请求所用模型名。 |
| `active_provider` | 字符串 | 空 | 上次选中的 provider 名；启动时从 `providers` 展开连接、密钥来源、模型与私有参数。项目级可钉住选择，否则回退全局。 |
| `theme` | `dark` / `light` / `plain` | `dark` | 终端配色;管道或重定向到文件时自动降为 `plain`。 |
| `status_panel` | JSON object | 见下文 | 定制输入框下方常驻状态栏的字段、顺序与分隔符；项目级整段压过全局。 |
| `think` | `none`/`low`/`medium`/`high`,可留空 | 空串 | 推理强度;空串时不往请求里带推理参数(跟无此功能的旧版本行为一致)。 |
| `soul` | 空串 / `default` / `off` / `souls/` 下文件名(不带 `.md`) | 空串 | 风格叠加层。空串和 `default` 读 `SOUL.md`;`off` 不叠加。 |
| `context_window` | 字符串或整数,支持 `256k`/`512k`/`1m` 或裸数字 | `256000` | 会话上下文窗口(token),`k=1000`、`m=1000000`(十进制)。 |
| `compact_model` | 字符串,可留空 | 空串 | `/compact` 专用模型;空串就沿用会话模型。模型在目录里带 `context_window` 时,压缩输入按它单独算预算(窗口 − 输出预留 − 协议余量),装不下明确拒绝、不截史。 |
| `max_context_chars` | 正整数 | `600000` | 旧的按字节硬切安全网,跟 `context_window` 不是一回事,两条防线互不依赖;真触发时终端打有损裁剪告警。 |
| `max_turns` | 非负整数 | `0`(无上限) | agent 主循环一次来回的轮数上限。不配或配 `0` = 不设上限,防跑飞靠 ESC/Ctrl+C;配正整数才是硬上限,超过就报错停止。负数或非法值静默忽略。 |
| `system_prompt_file` | 字符串,UTF-8 文本路径 | 无 | 人格段文件路径;没配就用内置人格,`--system-prompt` 命令行参数会压过它。 |
| `tool_search_threshold` | 非负整数 | `20` | 注册工具总数超过此数才启用延迟挂载(工具搜索);`0` 永不延迟。 |
| `memory` | JSON object | `enabled=false` | 项目记忆开关、读写子开关与召回预算，见下节。只能由全局配置打开。 |
| `language` | `zh-CN` / `en` / 语言包语言码 | 空 = 跟系统 | 界面语言。 |
| `hooks` | JSON object | 四类数组都空 | 外部命令钩子,详见下节。 |
| `mcpServers` | JSON object | 空 object | MCP stdio 服务器表,详见下节。 |
| `search` | JSON object | 未配置 | 搜索服务;不写时 `web_search` 工具不注册。 |
| `lsp` | JSON object | 空 object | 语言服务器表;不写时 `lsp` 工具不注册。 |
| `extra_body` | JSON object | 空 object | 顶层单 provider 写法下,每次请求浅合并进请求体顶层的额外字段,详见下节。 |
| `extra_headers` | JSON object(字符串到字符串) | 空 | 每次请求附带的额外 HTTP 头,详见下节。 |
| `providers` | 数组,每项见下方 provider 字段表 | 空 | 多端模型配置,详见"provider 实战"一节。 |
| `connect_timeout_ms` | 正整数(毫秒) | `15000` | TCP+TLS 握手阶段超时上限。 |
| `stream_idle_timeout_secs` | 正整数(秒) | `60` | 流式(SSE)读空闲超时——连续这么多秒没收到字节就判定假死。 |
| `request_timeout_secs` | 正整数(秒) | `30` | 非流式请求(如拉模型列表)的整体超时。 |

`base_url`/`api_key`/`model` 没有内置默认值——lubancode 不绑死哪一家模型服务,三项都没配到:交互模式会自动走一遍初次配置向导;单发模式/管道模式会直接报错,提示三条配置途径。

### status_panel

输入框下方的状态栏默认依次显示确认档、模型、工作目录、Git 分支、context 百分比与 token 用量。`status_panel.items` 可重排或隐藏字段，`separator` 可换分隔符：

```json
{
  "status_panel": {
    "items": ["model", "cwd", "git_branch", "context", "effort"],
    "separator": " · "
  }
}
```

认得的字段如下：

| 字段 | 内容 |
| --- | --- |
| `permission_mode` | 当前 confirm / auto / yolo 档及 Shift+Tab 提示。 |
| `model` | 当前会话模型。 |
| `cwd` | 当前工作目录；太长时从左侧收起，保住末级目录。 |
| `git_branch` | 当前 Git 分支；不在仓库时自动跳过，游离 HEAD 显示短哈希。 |
| `context` | 当前上下文占用百分比。 |
| `tokens` | 已用 token / 窗口 token；尚无实测用量时自动跳过。 |
| `provider` | 当前 provider；没有名字时自动跳过。 |
| `effort` | 当前推理档位；未设置时自动跳过。 |

数组顺序就是展示顺序。写空数组会留下空状态行；想保住 Shift+Tab 的可发现性，别删 `permission_mode`。这套字段在程序内拼装，不起 shell，不会因脚本迟滞拖住输入框。

### 项目记忆

项目记忆默认关闭。要用，须在全局 `~/.lubancode/config.json` 明写：

```json
{
  "memory": {
    "enabled": true,
    "use": true,
    "generate": true,
    "max_index_bytes": 16384,
    "max_retrieval_bytes": 24576,
    "max_results": 4
  }
}
```

- `enabled` 管总开关。
- `use` 管同步召回。每轮只读本地索引，最多取 `max_results` 份正文。
- `generate` 挂出 `memory_save` 工具。模型只把小而稳定的事实或用户明说的项目偏好排进队列；后台进程落盘、更新同 id 主题并重建索引。
- `max_index_bytes` 限制本轮索引字节数；`max_retrieval_bytes` 限制命中正文总字节数；三项预算都须是正整数。

项目级 `.lubancode/config.json` 不能自行把记忆从关改开。全局开过后，项目配置可以关闭，或收紧 `use`、`generate` 与预算。陌生仓库便不能替用户开启聊天提取。

Git 主工作树与 linked worktree 按 common git dir 共用一份记忆。正文放在 `~/.lubancode/projects/<项目key>/memory/`，分 `facts/` 与 `preferences/`；`index.md` 和 `.state/catalog.json` 都可从主题文件重建。记忆不写进会话 history，也不随 `/export` 导出。

交互会话可用 `/memory` 看状态。`/memory use on|off` 与 `/memory learn on|off` 只改本场；`/memory remember fact|preference 标题 [:: 正文]` 可显式排一条；`list`、`forget <id>`、`rebuild` 分别用来查看、归档与重建。

### providers 数组字段

`providers` 数组每一项对应一个模型服务端,`/provider` 命令族管理的就是这个数组(始终写进**全局**配置,不碰项目配置)。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `name` | 字符串,必填 | provider 名字,`/provider switch <名字>` 用。 |
| `base_url` | 字符串,必填 | 服务根地址。 |
| `wire` | `anthropic` / `responses` / `chat_completions` | 协议。 |
| `key_env` | 字符串 | 密钥所在环境变量名(默认只记名字,不落明文密钥)。 |
| `api_key` | 字符串,可选 | 非空时优先于 `key_env`(`/provider add` 向导贴明文密钥走这条,展示/日志一律打码)。 |
| `model` | 字符串,可选 | 默认模型,留空仍可 `/model` 选。 |
| `model_reasoning_effort` | 字符串,可选 | 切到该 provider 时按 `/think` 同一套机制应用的推理档位。 |
| `context_window` | 字符串或整数 | 上下文窗口,默认 `256000`。 |
| `native_web_search` | 布尔 | 是否声明服务端原生联网搜索,默认 `false`。 |
| `extra_body` | JSON object | 该端每次请求浅合并的额外顶层字段。 |
| `extra_headers` | JSON object | 该端每次请求追加/覆盖的 HTTP 头。 |

### `/provider` 命令用法

```
/provider list
/provider refresh                       从 GitHub 更新常见厂家目录
/provider add                          从常见厂家目录选择；末项为全手填
/provider add <名字>                    同上，预填本地 provider 名
/provider add <名字> <base_url> <anthropic|responses|chat_completions> [--key-env 变量名] [--key 明文key] [--model 模型] [--effort 档位] [--window 大小]
/provider switch <名字> [模型]
/provider remove <名字>
/provider set <名字> native_web_search on|off
/provider set <名字> extra_body <JSON object>
/provider set <名字> extra_header <头名> <值>
```

`/provider switch` 校验成功便记住选择。项目配置已写 `active_provider` 时继续写回项目；其余场景写入全局 `~/.lubancode/config.json`。这里只存名字，密钥仍留在 provider 的 `api_key` 或 `key_env`。`LUBANCODE_*` 专属环境变量照旧压在最上。

### 界面语言与语言包

`language` 字段(及 `LUBANCODE_LANG` 环境变量)取 `zh-CN` / `en` / 语言包语言码,空 = 跟系统。内置中英文编译进程序;其余语言靠 `~/.lubancode/languages/*.json` 外部语言包扩展——文件名即语言码(`ja.json` → `ja`),内容是平面键值对,没翻到的键自动回退中文。坏语言包警告跳过,不阻断启动。

```json
// ~/.lubancode/languages/ja.json
{ "language.name": "日本語 (ja)", "cmd.clear.done": "会話履歴をクリアしました" }
```

会话内 `/language` 即时切换。机制、回退链、键名规矩详见[界面多语言](i18n.md)。

## 三、LUBANCODE_* 环境变量表

| 环境变量 | 对应字段 | 取值 |
| --- | --- | --- |
| `LUBANCODE_WIRE` | `wire` | `anthropic`、`responses` 或 `chat_completions`。 |
| `LUBANCODE_BASE_URL` | `base_url` | 模型服务根地址。 |
| `LUBANCODE_API_KEY` | `api_key` | 模型服务认证值。 |
| `LUBANCODE_MODEL` | `model` | 模型名。 |
| `LUBANCODE_MAX_CONTEXT` | `max_context_chars` | 正整数;无效或不大于零时当没设。 |
| `LUBANCODE_MAX_TURNS` | `max_turns` | 非负整数;`0` = 不设上限,负数或无效值当没设。 |
| `LUBANCODE_THEME` | `theme` | `dark`、`light` 或 `plain`。 |
| `LUBANCODE_LANG` | `language` | `zh-CN`/`en`/语言包语言码;空 = 跟系统。 |
| `LUBANCODE_SYSTEM_PROMPT_FILE` | `system_prompt_file` | UTF-8 人格文件路径。 |
| `LUBANCODE_CONTEXT_WINDOW` | `context_window` | `256k`/`512k`/`1m` 或正整数。 |
| `LUBANCODE_COMPACT_MODEL` | `compact_model` | 压缩模型名;空值当没设。 |
| `LUBANCODE_THINK` | `think` | `none`/`low`/`medium`/`high`。 |
| `LUBANCODE_SOUL` | `soul` | `default`、`off` 或 `souls/` 下魂名。 |
| `LUBANCODE_FORCE_COLOR` | 终端颜色开关 | 设为 `1` 时,管道/重定向也强制尝试输出颜色;不写入 `config.json`。 |
| `LUBANCODE_CONFIRM_MODE` | 会话起手确认档 | `confirm`、`auto` 或 `yolo`；不写入 `config.json`，优先级低于 `--yes`，高于 `settings.local.json`。 |

环境变量设为空串,按没设处理。`hooks`、`mcpServers`、`search`、`lsp`、`tool_search_threshold`、`connect_timeout_ms`、`stream_idle_timeout_secs`、`request_timeout_secs` 没有对应的 `LUBANCODE_*` 变量,只能写配置文件。

交互命令 `/update` 使用当前配置的 `connect_timeout_ms` 与 `request_timeout_secs`。启动参数 `--check-update` 在加载配置前执行，故用内置默认超时。两者都只访问 GitHub Release API，不带模型密钥。

## 四、hooks / mcpServers / search / lsp

这四段只从配置文件读,没有环境变量、也没有内置默认值这两级——不写就是空。

### hooks

`hooks` 可有 `pre_tool`、`post_tool`、`session_start`、`session_end` 四个数组,每项须有字符串 `command`。命令交给平台默认 shell：Windows 用 `cmd.exe`，POSIX 用 `/bin/sh`。`pre_tool`/`post_tool` 可再写字符串 `matcher`:精确工具名,或 `"*"` 匹配全部工具;省略/空串也当 `"*"`。`session_start`/`session_end` 不看 `matcher`。

```json
{
  "hooks": {
    "pre_tool": [
      { "matcher": "write_file", "command": "echo about to write" }
    ],
    "session_start": [
      { "command": "echo session started" }
    ]
  }
}
```

### mcpServers

键是服务器名,值是 `{command(必填), args(可选,字符串数组), env(可选,字符串到字符串 object)}`。

```json
{
  "mcpServers": {
    "local-tools": {
      "command": "node",
      "args": ["C:/tools/mcp-server.js"],
      "env": { "MCP_MODE": "stdio" }
    }
  }
}
```

起进程握手成功后,工具以 `mcp__服务器名__工具名` 挂进工具表,`/mcp` 看状态。

### search

必须同时有 `provider`(只认 `tavily`/`brave`/`serper`)与非空字符串 `api_key`。配了这段才会注册 `web_search` 工具;`web_fetch` 工具无须配置,始终可用。

```json
{
  "search": {
    "provider": "tavily",
    "api_key": "<在本机填入搜索服务密钥>"
  }
}
```

### lsp

键是语言名(同时作 LSP 的 `languageId`),值是 `{command(必填), args(可选), extensions(必填,非空字符串数组), idle_minutes(可选,正整数,缺省 10)}`。

```json
{
  "lsp": {
    "cpp": {
      "command": "clangd",
      "args": ["--background-index"],
      "extensions": [".c", ".cc", ".cpp", ".h", ".hpp"],
      "idle_minutes": 10
    }
  }
}
```

配了才注册 `lsp` 工具(definition/references/symbols/diagnostics 语义查询),懒启动、闲置自动关停,`/lsp` 看各语言服务器状态。

## 五、extra_body / extra_headers:任意模型特殊参数

有的模型服务藏着自家专属开关——GLM 的 `thinking` 思考开关、别家的分级 `reasoning_effort`、某个厂商才认的顶层字段——lubancode 不会挨个内置,靠这两个字段自己往请求上加。顶层单 provider 配置、`providers` 数组里的每一条,都认这两个字段。

- **`extra_body`**:JSON object。每次请求都浅合并进请求体顶层——同名键**整个覆盖**内置逻辑(`thinking`、`native_web_search` 的 `tools` 声明等)算出来的值,不做深合并。provider 配置先合并，模型目录当前 variant 的 `extra_body` 最后拍板。
- **`extra_headers`**:JSON object,值必须是字符串。每次请求追加/覆盖 HTTP 头,同名覆盖内置头(包括 `Authorization`——自己配自己认);值留空表示删掉这条头。

不想手改 JSON,`/provider set` 也能改(`extra_body` 是整段替换语义,不是往里加键;设成 `{}` 或留空清掉):

```
/provider set glm extra_body {"thinking":{"type":"enabled"},"reasoning_effort":"max"}
/provider set glm extra_header X-Api-Version 2024-06-01
```

`/provider list` 只提示配了几个键/几条头(如 `extra_body=2键`),不会把 JSON 原文糊到屏幕上。

## 六、provider 实战两例

### 例一:MiniMax,anthropic 协议

```json
{
  "providers": [
    {
      "name": "minimax",
      "base_url": "https://api.minimaxi.com/anthropic",
      "wire": "anthropic",
      "key_env": "MINIMAX_API_KEY",
      "model": "MiniMax-M3"
    }
  ]
}
```

配好环境变量 `MINIMAX_API_KEY`,`/provider switch minimax` 即可切过去;也可以用 `/provider add` 向导一步步填,或者一行式:

```
/provider add minimax https://api.minimaxi.com/anthropic anthropic --key-env MINIMAX_API_KEY --model MiniMax-M3
```

### 例二:GLM,Chat Completions + extra_body 思考参数

GLM 系模型用 `thinking.type` 开关思考模式,外加一个自定义分级 `reasoning_effort`,两者都不是 lubancode 内置字段,走 `extra_body` 透传:

```json
{
  "providers": [
    {
      "name": "glm",
      "base_url": "https://open.bigmodel.cn/api/paas/v4",
      "wire": "chat_completions",
      "key_env": "GLM_API_KEY",
      "model": "glm-5.2",
      "model_reasoning_effort": "max",
      "extra_body": { "thinking": { "type": "enabled" }, "tool_stream": true },
      "extra_headers": { "X-Api-Version": "2024-06-01" }
    }
  ]
}
```

这份手写例子与内置 GLM 预设走同一路。平日直接 `/provider add` 选“智谱 GLM”即可。

## 七、settings.local.json:项目级本地权限

`<cwd>/.lubancode/settings.local.json` 存本项目的权限约定(照 Claude Code / Codex 的路数),**本地文件,不该提交**(首次落地时 lubancode 会尽力在 `<cwd>/.gitignore` 补一行,没有 `.gitignore` 就只给提示)。

```json
{
  "permissions": {
    "allow_tools": ["write_file"],
    "allow_commands": ["npm test", "git status"],
    "deny_commands": ["rm -rf"],
    "default_confirm_mode": "auto"
  }
}
```

全部字段可选,坏 JSON 只告警跳过、不崩:

- `allow_tools`:这些工具启动即进"总是允许"集合,本会话免确认。
- `allow_commands`:`run_command` 命令前缀白名单,auto 档里命中前缀等价 Safe(补充白名单,不改内置判定)。
- `deny_commands`:`run_command` 命令前缀黑名单,命中永远问一句,压过 `allow_commands`、压过会话"总是允许";只在 confirm/auto 档生效,`--yes`/yolo 是显式全放,`deny` 不拦。
- `default_confirm_mode`:起手确认档 `auto`/`yolo`/`confirm`,优先级低于 `--yes`/`LUBANCODE_CONFIRM_MODE`,高于内置默认 `confirm`。

确认某工具时按 `a`(本会话总是允许),真控制台里会多问一句要不要永久写进 `settings.local.json`。`/config` 会打一行 `permissions` 摘要。

## 八、模型目录 models.json

程序先从内置 `catalog/providers.json` 取得常见模型资料；还可以在**主目录**放一份 `~/.lubancode/models.json` 覆盖或补充。用户文件同 `slug` 优先。整份 JSON 坏了或某条写坏，只告警跳过，仍可退回内置目录。

```json
{
  "models": [
    {
      "slug": "MiniMax-M3",
      "display_name": "MiniMax M3",
      "description": "MiniMax 旗舰模型,anthropic 兼容端点,支持 Adaptive Thinking",
      "default_think": "high",
      "supported_think_levels": [
        { "effort": "none", "description": "关闭思考,直答,最快", "extra_body": { "thinking": { "type": "disabled" } } },
        { "effort": "high", "description": "开启 Adaptive Thinking,想多深由模型自己定", "extra_body": { "thinking": { "type": "adaptive" } } }
      ],
      "base_instructions": "工具调用要果断,能并行读文件就并行读;回答用中文,简洁准确。",
      "context_window": "1m",
      "supports_parallel_tool_calls": true,
      "input_modalities": ["text"],
      "truncation_policy": "auto"
    }
  ]
}
```

`slug` 必填,其余全部可选。命中目录条目时,启动和 `/model` 切换后自动应用 `default_think`、`context_window`、`base_instructions`;`supports_parallel_tool_calls`/`input_modalities`/`truncation_policy` 三项先解析存储,眼下不启用。用户在环境变量/配置文件里显式写的 `think`/`context_window` 压过目录默认。

## 九、~/.lubancode/ 目录一览

```text
~/.lubancode/
  config.json                         主配置
  models.json                         模型目录,可选
  cache/provider-catalog.json         从 GitHub 更新的厂家目录缓存
  system_prompt.md                    人格段(法),首启脚手架生成
  SOUL.md                             默认风格叠加层(魂),首启脚手架生成
  souls/                              备选魂;首启附 wenyan.md(文言文示例)
  prompts/                            提示词运行时模块,首启播种自内置版
    core/                             身份/干活方式/答话风格(默认人格)
    features/                         各工具方针段
    platforms/                        协议平台段
  sessions/                           会话存档
  projects/<项目key>/memory/          项目记忆正文、index 与可重建 catalog
  memory-jobs/                        后台记忆任务的 pending/failed 队列
  plugins/                            DLL 与 Lua 插件
  skills/                            用户自装 Skill；同名时压过官方级
  languages/                          外部语言包(见 i18n.md)
```

项目级的 `.lubancode/`(在 `<cwd>` 下)能放 `config.json`(按字段压过全局)、`settings.local.json`(本地权限,不进版本库)与 `skills/`(同名技能时项目级压过主目录级)。

官方 Skill 不再播种进主目录。便携包、Windows 安装与开发构建从 `<exe-dir>/skills/` 读取；POSIX 前缀安装还会找 `<prefix>/share/lubancode/skills/`。同名优先级是项目级 > 主目录级 > 官方级。旧版主目录里带系统维护标记的 `lubancode-config` 副本会自动让位给发行包新版。

## 十、命令会改哪份文件

有些 slash 命令只改本场，有些会落盘。分清这层，排错省一半工夫。

| 动作 | 本场生效 | 默认落点 |
| --- | --- | --- |
| `/model` | 是 | 当前 Provider 的模型选择；按命令交互决定是否写回配置 |
| `/provider add/remove/set` | 是 | `~/.lubancode/config.json` 的 `providers` |
| `/provider switch` | 是 | 若项目已钉 `active_provider`，写项目配置；否则写全局配置 |
| `/think` | 是 | 可按交互选择写回配置；只切本场时不落盘 |
| `/soul` | 是 | 可写全局配置的 `soul` |
| `/language` | 是 | 可写全局配置的 `language` |
| `/prompt` | 是 | 管理主目录 prompt 文件；重建系统提示 |
| `/memory use on|off` | 是 | 不落盘，只改本场 |
| `/memory learn on|off` | 是 | 不落盘，只改本场 |
| 确认提示按 `a` | 是 | 用户同意后，可写项目 `settings.local.json` |
| `Shift+Tab` | 是 | 不落盘，只轮换本场确认档 |
| `/init` | 是 | 项目根 `AGENTS.md`；已有文件不覆盖 |

`/clear` 清对话，不重置配置。`/compact` 改会话历史，不改 `config.json`。`--reset-system-prompt` 只处理系统 prompt 文件。

## 十一、配置排错

### 先看最终值

```powershell
lubancode --config
```

它会列字段值与来源。别只盯某一份 JSON；上头可能还有环境变量，脚下也可能有项目配置。

### JSON 读不进来

配置须是 UTF-8 JSON object，不能写注释、尾逗号或裸反斜杠。路径在 JSON 里用 `/`，或把 `\` 写成 `\\`。

### 改了全局配置却没生效

依次查：

1. `LUBANCODE_*` 是否压在最上。
2. 当前目录 `.lubancode/config.json` 是否写了同字段。
3. `active_provider` 是否把连接参数展开成另一套值。
4. 是否只改了旧 `.lubancode.json`，而新 `config.json` 已存在。

### Provider 有 key，仍报未认证

若写 `key_env`，它存的是环境变量名，不是密钥。确认启动 LubanCode 的同一进程环境里真有该变量。明文 `api_key` 优先于 `key_env`，空串则按没设处理。

### MCP、LSP、Hooks 改了却不生效

这些段在启动时建运行对象。改完重启。项目段是整段覆盖；项目里哪怕只写一台 MCP，也会盖住全局整张 `mcpServers` 表。

### `web_search` 不见了

`search.provider` 只认 `tavily`、`brave`、`serper`，且 `api_key` 必须非空。缺一项就不注册。`web_fetch` 不受这段影响。

### 自动压缩太早或太晚

`context_window` 管 token 百分比；`max_context_chars` 是独立字节安全网。前者有两条触发线：回合前按服务端 usage 实测（80%），回合中按统一估算口径的 projected（系统提示+工具定义+历史+输出预留，80%）。后者防极端大历史。两项都要看。token 估算全库一把尺：ASCII 4 字符约 1 token，非 ASCII 每字约 1.5 token。

### 项目记忆开不起来

项目配置不能把全局关闭的记忆自行打开。先在 `~/.lubancode/config.json` 设 `memory.enabled=true`，再由项目配置收窄。

### 终端仍有颜色或完全没颜色

先看 `theme`，再看 stdout 是否为真终端，最后看 `LUBANCODE_FORCE_COLOR`。`plain` 主动禁色；强制颜色主要供集成测试和明确知道终端能力的场景。
