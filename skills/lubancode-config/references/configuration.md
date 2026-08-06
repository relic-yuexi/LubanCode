# 配置、环境变量与主题

## 卷内目录

- 配置文件分层与优先级
- `config.json` 字段
- `LUBANCODE_*` 环境变量
- `~/.lubancode/` 目录
- 改主题

## 配置文件分层与优先级

`config.json` 分两级，两级都读、按字段合并（不是整份谁盖谁）：

- **项目级**：当前目录 `<cwd>/.lubancode/config.json`。
- **全局**：主目录 `~/.lubancode/config.json`。

每一级各自还认旧位置 `<目录>/.lubancode.json`（读到且对应新位置尚无文件时，lubancode 会把它迁移到新位置）。

字段按五级逐个决，高到低：

1. `LUBANCODE_*` 专属环境变量。
2. **项目级** `config.json`。
3. **全局** `config.json`。
4. 通用环境变量。`wire=anthropic` 读 `ANTHROPIC_BASE_URL`、`ANTHROPIC_AUTH_TOKEN`、`ANTHROPIC_MODEL`；`wire=responses` 或 `chat_completions` 读 `OPENAI_BASE_URL`、`OPENAI_API_KEY`、`OPENAI_MODEL`。
5. 内置默认值。

逐字段合并：项目级写了某字段就用项目级那一份，项目级缺的字段回退全局，全局也缺再往下找。`/config` 会标出每个字段到底来自「项目级配置」还是「全局配置」。

`hooks`、`mcpServers`、`search`、`lsp` 只从配置文件读，按「整段」回退——项目级写了就用项目级那一整段，否则用全局那一整段。`tool_search_threshold` 只从配置文件或内置默认值来。其余字段依上面五级找；没有通用环境变量的字段，直接略过第四级。

只在主目录里跑（`cwd` 就是主目录）时，只当项目级一份读，不重复。

## config.json 字段

| 字段 | 类型与取值 | 默认值 | 用处 |
| --- | --- | --- |
| `wire` | `anthropic` / `responses` / `chat_completions` | `anthropic` | 选 Anthropic Messages、OpenAI Responses 或 Chat Completions 兼容接口。 |
| `base_url` | 字符串 | 无 | 模型服务根地址。 |
| `api_key` | 字符串 | 无 | 模型服务认证值。不要把真实值写进仓库。 |
| `model` | 字符串 | 无 | 发请求所用模型名。 |
| `active_provider` | 字符串 | 空 | 上次选中的 provider 名；启动时从 `providers` 展开。项目级可钉住，否则回退全局。 |
| `theme` | 字符串：`dark`、`light`、`plain` | `dark` | 终端配色；管道或重定向时会降为 `plain`。 |
| `think` | 字符串：`none`、`low`、`medium`、`high`，也可留空 | 空串 | 推理强度；空串时不向请求带推理参数。 |
| `soul` | 字符串：空串、`default`、`off`，或 `souls/` 下文件名（不带 `.md`） | 空串 | 选风格叠加层。空串和 `default` 读 `SOUL.md`；`off` 不叠加。 |
| `context_window` | 字符串或整数；正数，认 `256k`、`512k`、`1m` 或裸数字 | `256000` | 会话上下文窗口，单位 token。`k=1000`，`m=1000000`。 |
| `compact_model` | 字符串，可留空 | 空串 | `/compact` 专用模型；空串时沿用会话模型。 |
| `max_context_chars` | 正整数 | `600000` | 旧的按字符硬切安全网，和 `context_window` 不是一回事。 |
| `max_turns` | 非负整数 | `0`（无上限） | agent 主循环一次来回最多几轮；不配或配 `0` = 不设上限，防跑飞靠 ESC/Ctrl+C；配正整数才是硬上限，超过就报错停止。负数或非法值静默忽略，落到下一级或默认值。 |
| `system_prompt_file` | 字符串，UTF-8 文本路径 | 无 | 人格段文件。没配时用内置人格；`--system-prompt` 会压过它。 |
| `tool_search_threshold` | 非负整数 | `20` | 注册工具总数超过此数才启用延迟挂载；`0` 永不延迟。 |
| `hooks` | JSON object | 四类数组都空 | 外部命令钩子，详见下节。 |
| `mcpServers` | JSON object | 空 object | MCP stdio 服务器表，详见下节。 |
| `search` | JSON object | 未配置 | 搜索服务；不写时 `web_search` 工具不注册。 |
| `lsp` | JSON object | 空 object | 语言服务器表；不写时 `lsp` 工具不注册。 |
| `extra_body` | JSON object | 空 object | 每次请求浅合并进请求体顶层的额外字段，详见下节。 |
| `extra_headers` | JSON object（字符串到字符串） | 空 | 每次请求附带的额外 HTTP 头，详见下节。 |
| `providers` | 数组 | 空 | 多端模型配置；`/provider switch` 会记住选中的名字。 |

`base_url`、`api_key`、`model` 没有内置值。交互模式会走初次配置向导；单发和管道模式会报缺项。

管理多端时先用 `/provider add` 从内置厂家目录选；`/provider refresh` 从 LubanCode 仓库更新目录。最后一项仍可全手填。

## LUBANCODE_* 环境变量

| 环境变量 | 对应字段 | 值 |
| --- | --- | --- |
| `LUBANCODE_WIRE` | `wire` | `anthropic`、`responses` 或 `chat_completions`。 |
| `LUBANCODE_BASE_URL` | `base_url` | 模型服务根地址。 |
| `LUBANCODE_API_KEY` | `api_key` | 模型服务认证值。 |
| `LUBANCODE_MODEL` | `model` | 模型名。 |
| `LUBANCODE_MAX_CONTEXT` | `max_context_chars` | 正整数；无效或不大于零时当作没设。 |
| `LUBANCODE_MAX_TURNS` | `max_turns` | 非负整数；`0` = 不设上限，负数或无效值当作没设。 |
| `LUBANCODE_THEME` | `theme` | `dark`、`light` 或 `plain`。 |
| `LUBANCODE_SYSTEM_PROMPT_FILE` | `system_prompt_file` | UTF-8 人格文件路径。 |
| `LUBANCODE_CONTEXT_WINDOW` | `context_window` | `256k`、`512k`、`1m` 或正整数。 |
| `LUBANCODE_COMPACT_MODEL` | `compact_model` | 压缩模型名；空值视作没设。 |
| `LUBANCODE_THINK` | `think` | `none`、`low`、`medium` 或 `high`。 |
| `LUBANCODE_SOUL` | `soul` | `default`、`off` 或 `souls/` 下魂名。 |
| `LUBANCODE_FORCE_COLOR` | 终端颜色开关 | 设为 `1` 时，管道或重定向也强制尝试输出颜色；它不写入 `config.json`。 |

环境变量设为空串，按没设处理。`hooks`、`mcpServers`、`search`、`lsp`、`tool_search_threshold` 没有对应的 `LUBANCODE_*` 变量。

## ~/.lubancode/ 目录

```text
~/.lubancode/
  config.json                         主配置
  models.json                         模型目录，可选
  system_prompt.md                    人格段(法)，首启脚手架生成
  SOUL.md                             默认风格叠加层(魂)，首启脚手架生成
  souls/                              备选魂；首启附 wenyan.md
  prompts/                            提示词运行时模块，首启播种自内置版
    core/                             身份/干活方式/答话风格(默认人格)
    features/                         各工具方针段
    platforms/                        协议平台段
  sessions/                           会话存档
  plugins/                            DLL 与 Lua 插件
  skills/                              用户自行安装的技能
  languages/                          预留
```

`sessions/`、`plugins/`、`skills/`、`languages/` 都可按需出现。官方技能跟在发行包旁，不写进主目录。项目级的 `.lubancode/`（在 `<cwd>` 下）能放 `config.json`（按字段压过全局）、`settings.local.json`（本地权限，不进版本库）与 `skills/`；同名技能时项目级压过主目录级。

### 改主题

编辑同一份 `config.json`，把顶层 `theme` 改成下列一值：

```json
{
  "theme": "light"
}
```

另可设 `LUBANCODE_THEME=dark`、`light` 或 `plain`。管道输出仍会自动降为纯文本，除非另设 `LUBANCODE_FORCE_COLOR=1`。
