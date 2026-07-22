# LubanCode 配置

[文档首页](README.md) · [扩展指南](extensions.md) · [架构说明](architecture.md) · [中文 README](../README.md) · [English README](../README.en.md)

lubancode 要跟大模型对话,得知道 `wire`(协议)、`base_url`、`api_key`、`model` 这几件事。本文档核实自 `src/config/config.hpp`、`src/config/prompt_files.cpp`(内置的 `lubancode-config` 技能手册)与 `lubancode --help` 的真实输出,字段名与语义以代码为准。

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
4. **通用环境变量**(向后兼容,跟 Claude Code、Codex 等工具共用同名变量容易撞车,建议改用第 1 级):`wire=anthropic` 时读 `ANTHROPIC_BASE_URL`/`ANTHROPIC_AUTH_TOKEN`/`ANTHROPIC_MODEL`;`wire=responses` 时读 `OPENAI_BASE_URL`/`OPENAI_API_KEY`/`OPENAI_MODEL`。
5. **内置默认值**。

逐字段合并:项目级写了某字段就用项目级那一份,项目级缺的字段回退全局,全局也缺再往下一级找。`hooks`、`mcpServers`、`search`、`lsp` 这几段是**整段回退**(不做键级混合)——项目级写了 `hooks` 就用项目级那一整段 `hooks`,否则用全局那一整段。`tool_search_threshold`、`connect_timeout_ms`、`stream_idle_timeout_secs`、`request_timeout_secs` 只从配置文件(项目级 > 全局)或内置默认值来,没有环境变量这一级。

`/config`(或 `lubancode --config`)会打出每个字段最终来自哪一级,排查配置问题用。

### 为什么要有专属环境变量

不少人机器上已经装了 Claude Code、Codex 之类的工具,全局环境变量里早设好了 `ANTHROPIC_BASE_URL`、`ANTHROPIC_AUTH_TOKEN`——那是给那些工具专用的中转服务配的,lubancode 要是也去读,轻则连错服务,重则被中转拒之门外。推荐直接用 `LUBANCODE_*` 专属变量,或放一份配置文件,不跟别的工具打架。

## 二、config.json 字段表

`config.json` 顶层须是 JSON object;字段可只写一部分,缺的往下一级找。

| 字段 | 类型与取值 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `wire` | `anthropic` / `responses` | `anthropic` | 选 Anthropic Messages API 还是 OpenAI Responses API。 |
| `base_url` | 字符串 | 无内置默认值 | 模型服务根地址。 |
| `api_key` | 字符串 | 无内置默认值 | 模型服务认证值,别提交进仓库。 |
| `model` | 字符串 | 无内置默认值 | 发请求所用模型名。 |
| `active_provider` | 字符串 | 空 | 上次选中的 provider 名；启动时从 `providers` 展开连接、密钥来源、模型与私有参数。项目级可钉住选择，否则回退全局。 |
| `theme` | `dark` / `light` / `plain` | `dark` | 终端配色;管道或重定向到文件时自动降为 `plain`。 |
| `think` | `none`/`low`/`medium`/`high`,可留空 | 空串 | 推理强度;空串时不往请求里带推理参数(跟无此功能的旧版本行为一致)。 |
| `soul` | 空串 / `default` / `off` / `souls/` 下文件名(不带 `.md`) | 空串 | 风格叠加层。空串和 `default` 读 `SOUL.md`;`off` 不叠加。 |
| `context_window` | 字符串或整数,支持 `256k`/`512k`/`1m` 或裸数字 | `256000` | 会话上下文窗口(token),`k=1000`、`m=1000000`(十进制)。 |
| `compact_model` | 字符串,可留空 | 空串 | `/compact` 专用模型;空串就沿用会话模型。 |
| `max_context_chars` | 正整数 | `600000` | 旧的按字符数硬切安全网,跟 `context_window` 不是一回事,两条防线互不依赖。 |
| `max_turns` | 非负整数 | `0`(无上限) | agent 主循环一次来回的轮数上限。不配或配 `0` = 不设上限,防跑飞靠 ESC/Ctrl+C;配正整数才是硬上限,超过就报错停止。负数或非法值静默忽略。 |
| `system_prompt_file` | 字符串,UTF-8 文本路径 | 无 | 人格段文件路径;没配就用内置人格,`--system-prompt` 命令行参数会压过它。 |
| `tool_search_threshold` | 非负整数 | `20` | 注册工具总数超过此数才启用延迟挂载(工具搜索);`0` 永不延迟。 |
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

### providers 数组字段

`providers` 数组每一项对应一个模型服务端,`/provider` 命令族管理的就是这个数组(始终写进**全局**配置,不碰项目配置)。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `name` | 字符串,必填 | provider 名字,`/provider switch <名字>` 用。 |
| `base_url` | 字符串,必填 | 服务根地址。 |
| `wire` | `anthropic` / `responses` | 协议。 |
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
/provider add                          进分步向导(裸敲)
/provider add <名字>                    进分步向导(名字先给上)
/provider add <名字> <base_url> <anthropic|responses> [--key-env 变量名] [--key 明文key] [--model 模型] [--effort 档位] [--window 大小]
/provider switch <名字> [模型]
/provider remove <名字>
/provider set <名字> native_web_search on|off
/provider set <名字> extra_body <JSON object>
/provider set <名字> extra_header <头名> <值>
```

`/provider switch` 校验成功便记住选择。项目配置已写 `active_provider` 时继续写回项目；其余场景写入全局 `~/.lubancode/config.json`。这里只存名字，密钥仍留在 provider 的 `api_key` 或 `key_env`。`LUBANCODE_*` 专属环境变量照旧压在最上。

## 三、LUBANCODE_* 环境变量表

| 环境变量 | 对应字段 | 取值 |
| --- | --- | --- |
| `LUBANCODE_WIRE` | `wire` | `anthropic` 或 `responses`。 |
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

环境变量设为空串,按没设处理。`hooks`、`mcpServers`、`search`、`lsp`、`tool_search_threshold`、`connect_timeout_ms`、`stream_idle_timeout_secs`、`request_timeout_secs` 没有对应的 `LUBANCODE_*` 变量,只能写配置文件。

## 四、hooks / mcpServers / search / lsp

这四段只从配置文件读,没有环境变量、也没有内置默认值这两级——不写就是空。

### hooks

`hooks` 可有 `pre_tool`、`post_tool`、`session_start`、`session_end` 四个数组,每项须有字符串 `command`(交给 `cmd.exe` 执行)。`pre_tool`/`post_tool` 可再写字符串 `matcher`:精确工具名,或 `"*"` 匹配全部工具;省略/空串也当 `"*"`。`session_start`/`session_end` 不看 `matcher`。

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

- **`extra_body`**:JSON object。每次请求都浅合并进请求体顶层——同名键**整个覆盖**内置逻辑(`thinking`、`native_web_search` 的 `tools` 声明等)算出来的值,不做深合并。合并发生在所有内置逻辑拼完之后、发送之前,`extra_body` 永远最后拍板。
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

### 例二:GLM,responses 协议 + extra_body 思考参数

GLM 系模型用 `thinking.type` 开关思考模式,外加一个自定义分级 `reasoning_effort`,两者都不是 lubancode 内置字段,走 `extra_body` 透传:

```json
{
  "providers": [
    {
      "name": "glm",
      "base_url": "https://open.bigmodel.cn/api/paas/v4",
      "wire": "responses",
      "key_env": "GLM_API_KEY",
      "extra_body": { "thinking": { "type": "enabled" }, "reasoning_effort": "max" },
      "extra_headers": { "X-Api-Version": "2024-06-01" }
    }
  ]
}
```

合并顺序上 `extra_body` 最后拍板,不会被内置的 `reasoning.effort` 翻译逻辑覆盖回去。

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

除了上面的分级配置,还可以在**主目录**放一份模型目录:`~/.lubancode/models.json`,给每个模型写一条详细配置(思路借鉴 Codex 的 model-catalog)。目录是锦上添花,不是硬依赖:文件不存在就是空目录;整份 JSON 坏了或某条写坏,启动时告警跳过,不拦启动。

```json
{
  "models": [
    {
      "slug": "MiniMax-M3",
      "display_name": "MiniMax M3",
      "description": "MiniMax 旗舰模型,anthropic 兼容端点,支持 Adaptive Thinking",
      "default_think": "high",
      "supported_think_levels": [
        { "effort": "none", "description": "关闭思考,直答,最快" },
        { "effort": "high", "description": "开启 Adaptive Thinking,想多深由模型自己定" }
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
  system_prompt.md                    人格段(法),首启脚手架生成
  SOUL.md                             默认风格叠加层(魂),首启脚手架生成
  souls/                              备选魂;首启附 wenyan.md(文言文示例)
  prompts/                            提示词运行时模块,首启播种自内置版
    core/                             身份/干活方式/答话风格(默认人格)
    features/                         各工具方针段
    platforms/                        协议平台段
  sessions/                           会话存档
  plugins/                            DLL 与 Lua 插件
  skills/lubancode-config/SKILL.md    lubancode 自身配置手册,随版本自动更新
  languages/                          语言包,预留扩展
```

项目级的 `.lubancode/`(在 `<cwd>` 下)能放 `config.json`(按字段压过全局)、`settings.local.json`(本地权限,不进版本库)与 `skills/`(同名技能时项目级压过主目录级)。
