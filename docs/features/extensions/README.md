# 扩展 LubanCode

[文档首页](../../README.md) · [工具手册](../../reference/tools.md) · [进程内插件深挖](../../architecture/extensions/plugin-runtime.md) · [配置手册](../../reference/configuration.md) · [安全模型](../../development/security.md) · [测试手册](../../development/testing.md) · [排错手册](../../getting-started/troubleshooting.md) · [架构说明](../../architecture/README.md)

LubanCode 留了七扇门：Skill、MCP、LSP、Lua、process 插件、native 插件、Hooks。分量不同，风险也不同。先挑最窄的一扇，够用便止。

## 1. 怎么选

| 路子 | 适合什么 | 运行位置 | 要不要重编主程序 | 风险 |
| --- | --- | --- | --- | --- |
| Skill | 教模型一套章法，附范例、模板、脚本 | 提示上下文；附带脚本另算 | 否 | 中 |
| MCP | 长驻服务、数据库连接池、已有 MCP 生态 | 独立 stdio 子进程 + JSON-RPC | 否 | 中至高 |
| LSP | 查定义、引用、符号、诊断 | 语言服务器子进程 | 否 | 中 |
| Lua（裸 `.lua`） | 纯本地计算：改文本、算数、查表 | LubanCode 进程内 | 否 | 高 |
| Lua（manifest v2） | 确定的 HTTPS 调用，Secret 由宿主代填 | LubanCode 进程内 | 否 | 中 |
| process 插件 | Python/Rust/Go/Node/C++ 可执行程序当工具 | 短命子进程（stdin/stdout JSON） | 否 | 中 |
| native 插件 | 接原生库、极低延迟、大数据量 | LubanCode 进程内（.dll/.so/.dylib） | 插件要编，主程序不用 | 最高 |
| Hooks | 会话或工具前后跑命令，做审计与拦截 | 平台默认 shell 子进程 | 否 | 高 |

只要是“告诉模型该怎么做”，先用 Skill。要把独立服务暴露成工具，用 MCP。只查代码语义，用 LSP。要调 HTTPS API 又不想拖运行时，用 manifest v2 Lua。Lua 和原生插件都进宿主进程，须当可执行代码审查。

若功能只有一枚可信本地函数，不必硬搭 MCP server。四条短路按风险从低到高挑：

- **process 插件**（默认主路）：任何能从 stdin 读 JSON、往 stdout 写 JSON 的程序都能挂。Python 冷启动毫秒级，进程崩了只坏当次调用。写法见第 6 节。
- **Lua（裸 `.lua`）**：零依赖零编译，`~/.lubancode/plugins/` 丢一枚 `.lua` 即挂。pure 画像缺省关 `io`/`os.execute`，死循环有指令预算落锤。纯计算，没有联网能力——裸 `.lua` 里不存在 `luban` 模块。见第 5 节。
- **Lua（manifest v2）**：联网的 Lua 必须带 `plugin.json`（`manifest_version: 2`）。HTTP、TLS、DNS、取消、字节帽全由 C++ 宿主执行，Lua 只描述请求；Secret 只拿不透明引用，看不见原文。零外部运行时——不装 Python、Node、第三方 Lua 模块。见第 5.3 节。
- **native 插件**：极低延迟、大数据不搬进程时才用；加载即执行库 constructor，崩了带倒宿主。ABI v2 三平台（Windows .dll / Linux .so / macOS .dylib）。见第 7 节。

设计动机、安装实证、ABI、并发与 corner case 见[进程内插件系统深挖](../../architecture/extensions/plugin-runtime.md)。

## 2. Skills

Skill 是一份按需展开的工作说明。它可带参考资料、模板和脚本。模型先看名称与摘要；任务命中后，再读完整 `SKILL.md`。

### 2.1 目录与优先级

```text
<exe-dir>/skills/<skill-name>/SKILL.md
<prefix>/share/lubancode/skills/<skill-name>/SKILL.md
~/.agents/skills/<skill-name>/SKILL.md
~/.lubancode/skills/<skill-name>/SKILL.md
<project>/.agents/skills/<skill-name>/SKILL.md
<project>/.lubancode/skills/<skill-name>/SKILL.md
```

前两处是同一层“官方级”：便携包和 Windows 安装从可执行文件旁找；POSIX 前缀安装还会找 `share/lubancode/skills`。发行包、安装脚本与 CMake install 会把官方 Skill 一并带上。

同名优先级为：项目 `.lubancode` > 项目 `.agents` > 用户 `.lubancode` > 用户 `.agents` > 官方级。项目总压过用户；同一层里，LubanCode 专用版本压过跨客户端共享版本。每次冲突都会写警告日志。旧版本曾把官方 `lubancode-config` 播种进用户目录；若那份文件带系统维护标记，现版会让它退位，改用发行包新版。真正由用户自建的同名 Skill 仍可覆盖官方级。

`.agents/skills` 是 Agent Skills 的跨客户端共享目录，LubanCode 会自动扫描。`.codex/skills`、`.claude/skills` 仍不扫；需要时可用 `/skill install` 装进 LubanCode 用户目录。

### 2.2 最小文件

```markdown
---
name: release-check
description: 发版前核对版本、测试、变更记录与产物。
---

# Release Check

先读版本号与 git 状态，再跑测试。任何一步失败，停下说明，不得打 tag。
```

`name` 须与父目录同名，长 1 到 64 字符，只用小写字母、数字和单横线；横线不可顶头、收尾或连写。`description` 长 1 到 1024 字符，要把“能做什么、何时该用”一并写清。正文列步骤、边界和失败处置。脚本、模板、参考资料放在同一目录，用相对路径引用。

解析走真正的 YAML，故多行 description、`license`、`compatibility`、`metadata` 与实验字段 `allowed-tools` 都可保留。发现时只把 `name + description` 放进上下文；命中后才载正文，资源仍按需读取。格式不规范的名字会记警告后兼容加载；缺 `name`、缺 `description` 或 YAML 完全读不通，则跳过该 Skill。

### 2.3 管理命令

```text
/skills
/skill list
/skill install https://github.com/owner/repo/tree/main/my-skill
/skill install C:\Users\me\.codex\skills\my-skill
/skill install D:\notes\release-check\SKILL.md
/skill update my-skill
/skill remove my-skill
```

裸敲 `/skill` 会打印可用写法与实际落盘目录。`install` 接 HTTP(S) 地址、本地目录、`SKILL.md` 或独立 Markdown；统一装进 `~/.lubancode/skills/<skill-name>`。装好、更新、删除后，本会话立即刷新技能清单。

### 2.4 安全边界

Skill 自身是文本，却能叫模型读文件、跑脚本、访问网络。安装前先读：

- 有没有索取密钥或上传仓库内容。
- 脚本是否改系统目录、安装全局依赖、发送外网请求。
- 指令是否企图绕过确认或盖过用户任务。
- 资源路径是否越出 Skill 目录。

## 3. MCP

MCP 把外部服务的工具清单接进 `ToolRegistry`。LubanCode 充当 stdio client，负责拉起服务、握手、发现工具、转发 JSON-RPC 调用。

### 3.1 配置

```json
{
  "mcpServers": {
    "local-tools": {
      "command": "node",
      "args": ["C:/tools/mcp-server.js"],
      "env": {
        "MCP_MODE": "stdio",
        "SERVICE_TOKEN": "replace-me"
      }
    }
  }
}
```

| 字段 | 必填 | 含义 |
| --- | --- | --- |
| `command` | 是 | 可执行文件或命令 |
| `args` | 否 | 字符串参数数组 |
| `env` | 否 | 传给子进程的环境变量 |

配置在用户或项目 `config.json`。项目段一旦出现，会整段盖过用户段，不做逐服务器深合并。

### 3.2 工具命名

握手成功后，工具名加命名空间：

```text
mcp__<server-name>__<tool-name>
```

例如 `mcp__local-tools__query_db`。命名空间免得两台服务器都叫 `search` 时撞车。

### 3.3 生命周期

MCP 服务器在会话启动阶段拉起并握手。工具发现成功才注册。会话结束时，客户端关管道、收子进程。

服务器 stdout 只能写协议帧。调试日志一律写 stderr。若把普通日志混进 stdout，JSON-RPC 分帧会坏。

`/mcp` 列服务器状态和完整工具名。改了配置后重启 LubanCode，才能重建服务器与工具表。

### 3.4 排错

- “起不来”：先在同一 shell 手工执行 `command + args`。
- “握手超时”：看服务是否真用 stdio transport。
- “JSON 解析失败”：查 stdout 有没有日志。
- “工具不见了”：看 `/mcp` 与启动警告；也可能处在延迟挂载状态，先 `/tools` 或让模型用 `tool_search`。
- “找不到密钥”：`env` 只传显式值；若想继承外层环境，确保启动 LubanCode 前已经设置。

## 4. LSP

LSP 不是另一套代码搜索。它问语言服务器，拿编译语义回答定义、引用、文档符号与诊断。

```json
{
  "lsp": {
    "cpp": {
      "command": "clangd",
      "args": ["--background-index"],
      "extensions": [".c", ".cc", ".cpp", ".h", ".hpp"],
      "idle_minutes": 10
    },
    "python": {
      "command": "pyright-langserver",
      "args": ["--stdio"],
      "extensions": [".py"],
      "idle_minutes": 10
    }
  }
}
```

配置键同时充当 `languageId`。`extensions` 负责路由文件。某扩展名没配置，`lsp` 工具会明说，不偷偷退成文本搜索。

模型调用统一的 `lsp` 工具，`action` 可取：

- `definition`
- `references`
- `symbols`
- `diagnostics`

行号、列号对模型按 1 起算；发给服务器时转成 LSP 的 0 起算。首次查询才拉起语言服务器。闲置达到 `idle_minutes` 后关停，下次再起。诊断先看缓存，必要时短等推送，最长约 2 秒，不会无限挂住。

`/lsp` 看各语言的未启动、运行、闲置关停或异常退出状态。

## 5. Lua 插件

Lua 有两副面孔：裸 `.lua` 只做纯计算；要联网，必须带 `plugin.json`（`manifest_version: 2`）。两副面孔同一只解释器、同一个 pure 画像，差别只在有没有宿主 API。

### 5.1 裸 `.lua`：纯计算，零 Host API

把 `.lua` 放进：

```text
~/.lubancode/plugins/
```

每个文件返回一张工具表：

```lua
return {
    name = "word_count",
    description = "统计文本里的词数。",
    input_schema = [[{
        "type": "object",
        "properties": {
            "text": { "type": "string" }
        },
        "required": ["text"]
    }]],
    execute = function(input)
        local count = 0
        for _ in string.gmatch(input.text, "%S+") do
            count = count + 1
        end
        return "word count: " .. count
    end,
}
```

字段含义：

| 字段 | 规矩 |
| --- | --- |
| `name` | 模型所见的短工具名，必填 |
| `description` | 可省；最好讲清用途、时机与参数 |
| `input_schema` | JSON Schema 字符串，顶层通常是 object |
| `execute` | 收 Lua table，返回可转成文本的结果 |

若文件叫 `word_count.lua`，最终名称为：

```text
plugin__word_count__word_count
```

完整示例见 [examples/plugins/word_count.lua](../../../examples/plugins/word_count.lua)。插件在启动时扫描。改完 `.lua` 后要重启 LubanCode。

**裸 `.lua` 没有 Host API。** `luban` 模块只在 manifest v2 插件里注册——裸脚本调 `luban.http.request` 就是调 nil，结构性碰不到网络与 Secret。要联网，写 manifest（5.3 节）；要文件与命令能力，改走 process 插件（进程隔离）。

### 5.2 三道软墙（两副面孔共用）

Lua 与宿主同进程，风险用三道软墙兜底（`pure` 画像缺省生效）：

- **库关门**：`os.execute`、`os.exit`、`io`、`package.loadlib` 拿不到——要文件与命令能力的，改走 process 插件（进程隔离），不要悄悄开 trusted。
- **指令预算**：死循环约 2 亿条虚拟机指令内被 `luaL_error` 掐断，报错文案自带预算数；ESC 也走同一条落锤路。
- **内存帽**：狂吃内存按 OOM 报脚本错误，宿主堆不破。

三道墙都是软的：拦跑野的脚本，不是恶意绕洞。真不可信代码走 process 隔离。`trusted` 画像（全开）须显式批准。插件工具默认需要确认，但确认只能挡“是否调用”，挡不住插件内部写坏内存。

### 5.3 manifest v2 Lua：受控 HTTP 与 Secret

一句话：**Lua 只描述请求，宿主握住水管和钥匙。** HTTP、TLS、代理、DNS、取消和字节帽都由 C++ 宿主执行，Lua 不碰 socket；Secret 由宿主解析并代填请求头，Lua 只拿不透明引用。零外部运行时——用户不用装 Python、Node 或第三方 Lua 模块。

目录一插件一目录：

```text
~/.lubancode/plugins/my-api/
  plugin.json      # manifest v2：工具、网络账、Secret、帽
  my-api.lua       # runtime.entry 指的脚本
```

`plugin.json` 最小样例（全字段规矩见 `src/runtime/plugin_contract.hpp` 注释）：

```json
{
  "manifest_version": 2,
  "id": "my-api",
  "version": "0.1.0",
  "language": "lua",
  "runtime": {"kind": "embedded-lua", "entry": "my-api.lua"},
  "permissions": {
    "network": [
      {"scheme": "https", "host": "api.example.com", "port": 443,
       "methods": ["GET", "POST"]}
    ],
    "secrets": [
      {"id": "api_key", "env": "MY_API_KEY", "required": false}
    ]
  },
  "limits": {"http_timeout_ms": 20000, "http_response_bytes": 4194304},
  "tools": [
    {"name": "search", "entry": "search", "description": "……",
     "input_schema": {"type": "object", "properties": {}}}
  ]
}
```

Lua 脚本只返回 handler 表，键名与 `tools[].entry` 一一对应；工具合同只认 manifest，不在 Lua 里抄第二份 schema：

```lua
return {
  search = function(input)
    local response, err = luban.http.request({
      method = "POST",
      url = "https://api.example.com/v1/search",
      headers = { Accept = "application/json" },
      json = input,
      auth = { type = "bearer", secret = "api_key", optional = true },
      timeout_ms = 20000,
    })
    if err ~= nil then
      error(err.code .. ": " .. err.message)
    end
    return { status = response.status, body = response.json }
  end,
}
```

宿主代管的账：

| 账 | 规矩 |
| --- | --- |
| 网络 | 只收 `https` + 精确 DNS host + 443 端口 + `GET`/`POST`。通配符、IP 字面量、明文 HTTP 一概不收。manifest 没声明网络，`luban.http.request` 永远报 `network_not_declared` |
| 边界 | URL 解析、目的地命中、DNS 私网/metadata 段、连接期钉地址防 rebinding、重定向一概不跟（3xx 原样交 Lua）——五道边界全在宿主 |
| Secret | 按逻辑 id 声明。宿主环境变量优先，其次插件数据目录的 `.env`；Lua 用 `luban.secrets.available("api_key")` 探有无，拿不到明文，`tostring` 只得 `<secret:api_key>` |
| 禁写头 | Lua 自写 `Authorization`、`Cookie`、`Host`、`Content-Length` 直接拒——这些由宿主代填 |
| 帽 | URL/请求头/请求体/响应头/响应体/墙钟六处落锤，全部在数据入口处掐，不先收完再看。`limits` 只许下调宿主硬上限；0 不是无限，是非法 |
| 时机 | 启动加载期（顶层代码）调任何 Host API 一律报 `no_active_tool_call`——零网络、零 Secret 解析。只有工具 `execute` 的动态作用域里可用 |
| 错误 | 17 枚稳定错误码（`network_target_denied`、`response_too_large`、`cancelled`、`timeout`……），Lua 与测试都按码判断；HTTP 非 2xx 不冒充网络错，status 原样带回 |

`.env` 放数据目录，永不放插件源码树（那会进内容指纹，还容易打包带走）：

```text
standalone：~/.lubancode/plugin-data/<plugin-id>/.env
packaged：  ~/.lubancode/package-data/<package-id>/plugins/<local-id>/.env
```

Windows 的 `~` 取 `%USERPROFILE%`。文件是窄语法（`KEY=value`、引号包值、`#` 注释，不做插值与命令替换），只装 manifest 声明过的键。

信任与诊断：项目目录与 Package 里的 v2 插件是外来代码，过内容指纹信任门——`/plugin trust` 亮全份材料（Lua entry、精确网络目的地、Secret 逻辑名与 env 名、资源帽、指纹），权限一改旧信任即失效。`/plugin inspect` 看权限真账，`/plugin doctor` 只读体检（含逐目的地 DNS 安全检查），不带 Secret 发网。安全边界的全份规矩见[安全模型](../../development/security.md)。

完整参考实现见 [examples/packages/anysearch](../../../examples/packages/anysearch/README.md)：一只 manifest + 208 行 Lua，四件搜索工具，零 Python、零 Node。

v1 manifest 写 `embedded-lua` 会明报“manifest-backed Lua 需 v2”；旧裸 `.lua`、v1 process、native 三条旧路行为不变。

## 6. process 插件（默认主路）

任何能从 stdin 读一份 JSON、往 stdout 写一份 JSON 的程序都能当工具：Python、Rust、Go、Node、Deno、Ruby、C/C++ 可执行文件、shell wrapper 都一样。每次调用起一只短命进程，进程退出调用结束——无握手、无常驻 server、无 JSON-RPC。

### 6.1 一插件一目录

```text
~/.lubancode/plugins/local-math/
  plugin.json      # 静态真账：id/runtime/tools 的 manifest
  runner.py        # 你的脚本
```

`plugin.json` 的全字段规矩（强校验，坏了整件拒绝加载，不悄悄宽化）见 `src/runtime/plugin_contract.hpp` 的注释。最小样例：

```json
{
  "manifest_version": 1,
  "id": "local-math",
  "version": "1.0.0",
  "language": "python",
  "runtime": {
    "kind": "process",
    "command": "python3",
    "args": ["${plugin_dir}/runner.py"],
    "timeout_ms": 30000
  },
  "tools": [
    {
      "name": "add",
      "description": "把两个数字相加。",
      "input_schema": {
        "type": "object",
        "properties": {"a": {"type": "number"}, "b": {"type": "number"}},
        "required": ["a", "b"],
        "additionalProperties": false
      }
    }
  ],
  "permissions": {"network": false, "env": []}
}
```

生成脚手架最快：`lubancode plugin init python my-math` 生成 plugin.json + runner.py + test_runner.py 三件套，本地 `python test_runner.py` 先自测。

### 6.2 协议(v1 与 v2)

请求（stdin，恰好一份 JSON，写完即关，脚本可 `json.load(sys.stdin)` 读到 EOF）：

```json
{"protocol": 2, "call_id": "req-7", "plugin": "local-math", "tool": "add",
 "arguments": {"a": 1, "b": 2}, "context": {"cwd": "D:/project"}}
```

响应（stdout，恰好一份 JSON）：

```json
{"protocol": 2, "call_id": "req-7", "ok": true,
 "content": [{"type": "text", "text": "3"}], "structured": 3}
```

v2 在 v1 之上加一件事：**工具结果可以带图**。`content` 里许用 `type=image`
块（仅当响应 `protocol` 为 2）：

```json
{"protocol": 2, "call_id": "req-9", "ok": true,
 "content": [
   {"type": "text", "text": "已截图 800x600"},
   {"type": "image", "mime_type": "image/png", "path": "C:/.../shot.png"}
 ]}
```

- 图片来源二选一：`data`（base64 正文，响应帧里自带）或 `path`（插件
  自己落好的文件，宿主读）。两样恰给其一。
- 宿主照 MCP 富结果的同一条规矩验身：魔数复核（声明 MIME 与字节对不上
  整次拒，`image_rejected`）、单块 20MB 帽、单次调用 64MB 合计帽；落账成
  会话 artifact（内容寻址），随后由四家 wire 原生回喂模型（anthropic 的
  tool_result image 块 / OpenAI responses 的 input_image 数组 / Gemini 3+
  的 inlineData；chat wire 明降级为路径附注）。
- 版本协商向后兼容：宿主请求帧说 `protocol: 2`；v1 旧插件不读这个字段、
  照旧回 `protocol: 1` 纯文本，宿主两侧都收。v1 响应里冒出 image 块仍按
  协议错拒（UnknownContent），v1 的铁律不动。

铁律：

- stdout 是结果专线。日志只写 stderr；stdout 前后混任何字节，整次调用判协议错，不从字堆里猜 JSON。
- `content` 认 `type=text` 与（v2）`type=image`；别的类型按协议错拒，不静默转字符串。
- 非零退出、崩溃、超时、取消、坏 UTF-8、坏 JSON、call_id 不合、输出超限、图片拒收各有唯一宿主错误码，`/plugin doctor` 与错误文案对得上。
- 超时到点先温和终止，过宽限期杀整棵进程树（Windows Job Object / POSIX 进程组）；ESC 走同一条取消路。
- 入参在发送前先过 manifest 声明的 JSON Schema 子集验证；缺字段、类型不对在宿主侧就报清，不用等脚本炸。

### 6.3 环境与边界

- 起进程不用 shell：argv 直传，参数里的引号、空格、`&;|` 不可能变成命令。
- 子进程环境是**最小集**：PATH 与必要系统变量 + manifest `permissions.env` allowlist 点名的；宿主整份环境（连 API key）一概不递。密钥要给就在 allowlist 里显式点名。
- cwd 缺省项目根。
- 进程崩溃只坏本次调用。进程隔离不等于安全沙箱——子进程仍有当前用户的文件与网络权限。

### 6.4 各语言样例

**Python**（`lubancode plugin init python` 生成的就是这份的成器版）：

```python
import json, sys
for s in (sys.stdin, sys.stdout, sys.stderr):
    try: s.reconfigure(encoding="utf-8")   # Windows 管道默认本地代码页
    except Exception: pass
request = json.load(sys.stdin)
try:
    value = request["arguments"]["a"] + request["arguments"]["b"]
    json.dump({"protocol": 1, "call_id": request["call_id"], "ok": True,
               "content": [{"type": "text", "text": str(value)}]}, sys.stdout)
except Exception as e:
    json.dump({"protocol": 1, "call_id": request.get("call_id", ""), "ok": False,
               "error": {"code": "execution_failed", "message": str(e)}}, sys.stdout)
```

**Rust**（`cargo build --release` 出可执行文件，manifest 的 `command` 直接写它的绝对路径或 `${plugin_dir}/target/release/mytool`；用户机器不需要装 Rust）：

```rust
use std::io::{self, Read, Write};

fn main() {
    let mut input = String::new();
    io::stdin().read_to_string(&mut input).unwrap();
    let request: serde_json::Value = serde_json::from_str(&input).unwrap();
    let a = request["arguments"]["a"].as_f64().unwrap_or(0.0);
    let b = request["arguments"]["b"].as_f64().unwrap_or(0.0);
    let response = serde_json::json!({
        "protocol": 1,
        "call_id": request["call_id"],
        "ok": true,
        "content": [{"type": "text", "text": (a + b).to_string()}],
    });
    io::stdout().write_all(response.to_string().as_bytes()).unwrap();
}
```

**C 可执行文件**（源码不能直接跑，编成独立 executable 走同一条协议；用户机器不需要编译器）：

```c
#include <stdio.h>
#include <string.h>

/* 读全部 stdin，抽出 "a":N 与 "b":N（示例级解析，正经用 jsmn 之类小库） */
int main(void) {
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, stdin);
    buf[n] = '\0';
    double a = 0, b = 0;
    sscanf(strstr(buf, "\"a\""), "\"a\":%lf", &a);
    sscanf(strstr(buf, "\"b\""), "\"b\":%lf", &b);
    char call_id[64] = "";
    sscanf(strstr(buf, "\"call_id\""), "\"call_id\":\"%63[^\"]", call_id);
    printf("{\"protocol\":1,\"call_id\":\"%s\",\"ok\":true,\"content\":[{\"type\":\"text\",\"text\":\"%g\"}]}",
           call_id, a + b);
    return 0;
}
```

### 6.5 项目级插件与信任

`<项目>/.lubancode/plugins/` 也认（一插件一目录，格式同上）。项目目录里的插件是外来代码——首次见到按内容指纹（全部文件的 SHA-256）查信任账 `~/.lubancode/plugin-trust.json`，未信任的跳过并警告，改一个字节指纹即变、须重审。用户主目录的插件是亲手放的，不进这本账。

### 6.6 完整案例：GUI Agent（Windows 桌面自动化）

单工具示例见 [examples/plugins/](../../../examples/plugins/README.md)；要看的“多工具、有安全合同的完整插件”，见 **[examples/packages/gui-agent](../../../examples/packages/gui-agent/README.md)**——九件 GUI 工具（截图/枚举/聚焦/移动/点击/滚轮/输入/按键/状态），带：

- 坐标合同：`virtual_screen` 物理像素与 `window_client` 两种口径，动作前换算、越界拒绝；
- stale observation：截图时的窗口矩形进 observation，动作带 `expected_window_rect` 复查，窗口挪了即拒，不拿旧图坐标硬点；
- dry-run（`LUBANCODE_GUI_DRY_RUN=1`）：动作只校验只报计划，一枚输入事件都不发；
- 危险键闸（Win 组合、Alt+F4 默认禁）、文本/滚轮/截图上限、中文 Unicode 直注（不走剪贴板）；
- 教学夹具（本地 tkinter 小窗）、离线自测（零真输入）、真桌面 E2E（默认 SKIP）。

它示范的选型判断：**状态在桌面与目标程序手里的，用短命 process 插件；状态要住进你进程里的（DOM handle、连接池、事件订阅），才上 MCP。**

## 7. native 原生插件（三平台）

三平台同一份纯 C ABI：Windows 加载 `.dll`、Linux 加载 `.so`、macOS 加载 `.dylib`。公共 ABI 头在 [include/luban_plugin.h](../../../include/luban_plugin.h)。插件导出同一枚符号：

```c
const void* luban_plugin_entry(void);
```

返回的 manifest 首字段判版本：`1` = ABI v1（legacy，宿主兼容读取，加载行明报）；`2` = ABI v2（当前）。其余值拒绝加载，不静默拿错结构体。

示例构建（三平台各出各的产物）：

```bash
cmake -S examples/plugins/hello_plugin -B build/hello-plugin
cmake --build build/hello-plugin --config Release
# Windows 出 hello_plugin.dll / Linux 出 libhello_plugin.so / macOS 出 libhello_plugin.dylib
```

放进主目录的插件目录：

```text
%USERPROFILE%\.lubancode\plugins\        # Windows
~/.lubancode/plugins/                    # Linux / macOS
```

宿主按当前平台扩展名扫描，会略过没有 `luban_plugin_entry` 的依赖库。工具名前缀取 v2 manifest 的 `plugin_id` 字段（v1 取文件名去扩展名）：

```text
plugin__hello_plugin__reverse_text
```

ABI v2 的硬规矩：

1. `abi_tag` 必须是 `LUBAN_PLUGIN_ABI_V2`（v1 插件写 1，照挂）。
2. `struct_size` 写 `sizeof(luban_plugin_manifest_v2)`——宿主按它前向兼容。
3. `api_min`/`api_max` 与宿主的版本域须有交集。
4. `execute` 返回的 `content` 必须是 UTF-8、`\0` 收尾。
5. buffer 契约：content 是谁分配的，`free_result` 就还给谁。声明了 `CAP_HOST_ALLOCATOR` 的插件用 manifest 里 `host_callbacks.allocate` 拿 buffer，`free_result` 里 `release` 交还——跨 CRT 不混堆。
6. `shutdown` 钩子（可空）在卸载前调一次，插件在这收自己的线程与资源。

一只包带多平台产物时按 OS + arch 分目录放（`bin/windows-x64/`、`bin/linux-x64/`……），manifest 指对当前 target；找不到就标 unavailable，不拿相近文件试载。完整工程见 [examples/plugins/hello_plugin](../../../examples/plugins/hello_plugin)。

原生插件加载即执行库 constructor（Windows 的 DllMain 同理），崩了带倒宿主，也能取得宿主进程权限。只加载信得过、版本对得上的二进制；native 插件必须单独批准并记文件 hash（信任账见第 6.5 节）。

## 8. Hooks

Hooks 在会话或工具边界跑外部命令。适合审计、格式检查、策略拦截和通知。事件全表(PreToolUse/PermissionRequest/PostToolUse/SessionStart/End/UserPromptSubmit/Stop/Pre-PostCompact/SubagentStart-Stop)、stdin/stdout JSON 协议、决策归并、`/hooks` 管理面与信任模型见 **[Hooks 手册](hooks.md)**。这里只给速览。

**schema 2(推荐)**:键为 PascalCase 事件名,handler 走 stdin JSON + 结构化 stdout,支持 exec form 与超时:

```json
{
  "hooks": {
    "schema_version": 2,
    "PreToolUse": [
      { "matcher": "run_command", "hooks": [ { "command": "python", "args": ["policy-check.py"] } ] }
    ],
    "SessionStart": [
      { "matcher": "startup", "hooks": [ { "command": "echo session-start" } ] }
    ]
  }
}
```

**旧格式(已废弃,兼容保留)**:`pre_tool`/`post_tool`/`session_start`/`session_end` 四个数组,环境变量输入,任意非零退出码拦 `pre_tool`:

工具 hooks 收到环境变量(仅旧格式):

| 变量 | 何时有 | 内容 |
| --- | --- | --- |
| `LUBAN_TOOL_NAME` | 前、后 | 完整工具名 |
| `LUBAN_TOOL_INPUT` | 前、后 | JSON 参数 |
| `LUBAN_TOOL_RESULT` | 后 | 结果前 8192 字节 |
| `LUBAN_TOOL_IS_ERROR` | 后 | `true` / `false` |

`pre_tool` 返回非零退出码，会拦住该工具，并把输出前几行交回模型。起进程失败或超时则告警后放行。`post_tool` 和 session hook 的失败只告警，不回滚已经发生的动作。

命令走平台默认 shell：Windows 用 `cmd.exe`，POSIX 用 `/bin/sh`。单条默认限时 30 秒(schema 2 可按 handler 配)。

**信任**:全局与项目两层 hooks 相加,都跑;**项目级 hooks 须经 `/hooks` 信任审查(按定义哈希)才执行**——未信任的绝不启进程,命令一改须重审。信任账在用户主目录,仓库改不动它。

## 9. 延迟挂载

注册表工具总数超过 `tool_search_threshold`，且延迟工具声明 token 本金（名字+描述+schema）不低于 `tool_search_token_floor` 时，部分动态与低频工具先不把 schema 发给模型。模型只看见 `tool_search`，搜到工具后按 `deferred_tool_mode` 走法发现与调用。

默认阈值是 20、预算门是 1500。阈值设 `0` 关闭延迟，所有工具每轮都进 schema；预算门设 `0` 只按枚数判定。工具多时，关闭会明显吃输入 token。

命中之后走哪条路（扩写回顶层 / 代理引用 / 原生引用）见[工具参考·延迟挂载](../../reference/tools.md#延迟挂载与工具搜索)。

`/tools` 可看已注册、已挂载与延迟工具。工具名没出现在本轮 schema，不等于插件没加载。

## 10. 确认与权限

- Lua、process、native 插件工具默认确认（外部代码一律先问）。
- MCP 工具按外部工具处理，调用前可进入确认流程。
- LSP 查询只读，通常不问。
- Hooks 由配置直接触发，不另问。
- Skill 不是工具；它引出的实际工具仍各走自己的确认。

可信项目可在 `.lubancode/settings.local.json` 写 `allow_tools`、`allow_commands`、`deny_commands`。该文件是本机权限，不该提交。插件工具照走同一本账（`plugin__` 前缀全名写进 allowlist 即免确认），不另开第二套。详见[配置手册](../../reference/configuration.md#七settingslocaljson项目级本地权限)。

## 11. 分发

**Skill**

一项技能一个目录。只带必要资源。写清依赖、平台与安装步骤。随 LubanCode 发布的官方 Skill 放仓库 `skills/`，打包后保持同样目录；用户自装 Skill 仍落 `~/.lubancode/skills/`，升级程序不覆盖。要与 VS Code、Codex 等兼容客户端共用，就手工放进 `.agents/skills/`。

**MCP**

固定服务版本。把日志写 stderr。密钥走环境。提供一条能单独启动并自检的命令。

**Lua**

裸 `.lua` 一文件一工具最省事。文件名别轻易改；一改，命名空间也跟着变。声明只用 pure 画像认的库（string/table/math/os.time 之类），用户挂上即用，不必开 trusted。

manifest v2 Lua 联网插件：网络账只写用得上的精确 host，别多报；`limits` 只许下调。Key 不进包——`.env` 放数据目录（第 5.3 节），包里只带 `docs/.env.example` 占位样板。照抄骨架见 [examples/packages/anysearch](../../../examples/packages/anysearch/README.md)。

**process 插件**

一插件一目录（plugin.json + 脚本 + 自带 venv/依赖）。`command` 写 venv 解释器或绝对路径，别假定目标机器 PATH 里有你的解释器。日志只写 stderr。跨平台发分别考虑 `python3`/`python`/`py -3` 或干脆把依赖冻结成可执行文件（PyInstaller、cargo build、go build——manifest 的 `command` 直接指产物，用户机器零依赖）。

**原生插件**

按 OS + arch 分目录带多平台产物（`bin/windows-x64/*.dll`、`bin/linux-x64/*.so`、`bin/macos-arm64/*.dylib`……），manifest 指对当前 target。标明 ABI 版本（v2 的 `struct_size`/`api_min`/`api_max` 会自报）、架构与 CRT。不要只发一枚来路不明的库；二进制不能审查源码，更要走 hash 信任账。

**Hooks**

命令要短，要有超时意识。跨平台仓库分别考虑 `cmd.exe` 与 `/bin/sh` 语法。

## 12. 发布前检查

- 工具名稳定，说明与 schema 对得上。
- 参数缺失、坏类型、外部服务退出都有清楚错误。
- 没把 API key 写进源码、示例、Skill、日志或目录。
- 联网插件只声明用得上的精确 host；`.env` 与 Key 不进包，样板用 `.env.example` 占位（第 5.3 节）。
- 写操作会触发正确确认。
- 输出设了上限，不把几百兆数据塞回模型。
- MCP stdout 没有日志；process 插件 stdout 恰好一份 JSON、日志全在 stderr。
- Lua/native 经坏输入、重复调用与退出清理测试；native 另在独立进程里测过崩溃路径。
- manifest v2 Lua 另过一遍：顶层不调 Host API、越权 host 有拒绝测试、响应超帽立刻断（照 `tests/integration/plugins/test_anysearch_package.cpp` 的假服务架子，不烧真网）。
- process 插件带 `test_runner.py`（或同位自测脚本），作者本地能一条命令自测。
- 文档写明安装路径、重载方式和卸载办法。

## 13. 排错矩阵

`/skills`、`/mcp`、`/lsp`、`/plugins`、`/tools` 先查状态；单枚插件用 `/plugin inspect <id>` 看详情、`/plugin doctor <id>` 探环境。再看启动警告。

扩展文件改了却不生效，要分门看：Skill 管理命令会热刷新；LSP 进程按需拉起；MCP、插件（三路）、Hooks 改配置后要重启进程。

插件常见病症一张表：

| 症状 | 多半是 | 怎么查 |
| --- | --- | --- |
| process 插件没挂上 | manifest 解析/校验失败 | 启动警告点名哪一项;`plugin.json` 的 JSON 文法与字段规矩(第 6.1 节) |
| process 插件挂上但调用报 spawn_failed | 解释器不在或 command 写岔 | `/plugin doctor <id>` 真跑 `--version`;manifest 的 `command` 可写绝对路径或 venv 解释器 |
| 调用报 bad_json | stdout 混了日志 | 日志只写 stderr;stdout 恰好一份 JSON(第 6.2 节) |
| 调用报 call_id_mismatch | 脚本回显的 call_id 对不上 | 响应里原样回请求的 `call_id` |
| 调用报 timed_out | 脚本比 timeout_ms 慢 | manifest 调 `timeout_ms`,或把慢路径拆小 |
| 中文入参/出参变 `?` | Windows 管道默认本地代码页 | 脚本先 `reconfigure(encoding="utf-8")`(scaffold 生成的 runner.py 已带) |
| Python 报缺依赖 | venv/依赖没装 | 插件自带 venv 并把 `command` 指到 venv 解释器;LubanCode 不代装 |
| Lua 脚本用 io/os.execute 报 nil | pure 画像关了这些库 | 确要文件/命令能力:改走 process 插件(隔离),不悄悄开 trusted |
| Lua 死循环被掐断 | 指令预算落锤(默认 2 亿条) | 拆小任务;报错文案自带预算数 |
| v2 Lua 工具报 network_not_declared | manifest 没写 permissions.network | 补网络账再重批信任(第 5.3 节);没声明的 host 一概拒发 |
| v2 Lua 报 no_active_tool_call | 顶层/加载期调了 luban.http 或 luban.secrets | Host API 只在工具 execute 里可用;顶层只返回 handler 表 |
| v2 Lua 报 secret_missing | required Secret 环境变量与 .env 都没配 | 配环境变量,或写数据目录 .env(第 5.3 节;源码树里的 .env 宿主不读) |
| v2 Lua 报 network_target_denied | scheme/host/port/method 不命中声明 | url 只写声明过的精确 host;越权 URL 宿主拒发 |
| native 库没挂上 | 扩展名/架构/ABI 不合 | 启动警告给 `abi_tag` 与错误码;`.dll`/`.so`/`.dylib` 按平台放(第 7 节) |
| native 库挂上但调用崩宿主 | 插件野指针/ABI 错配 | native 插件崩了带倒宿主——先在独立进程里测插件本体 |
| 项目插件没挂上 | 未经信任 | 警告给内容指纹;批准走 `~/.lubancode/plugin-trust.json`(第 6.5 节) |
| 模型不调用已挂的工具 | 延迟挂载或 description 没说清时机 | `/tools` 看三态;description 写"何时该用" |
| 调用总报参数错 | schema 与实现两张皮 | manifest 的 `input_schema` 就是合同,调用前宿主先验;两处对齐 |

某件工具已加载却模型不调用，先看它是否延迟挂载，再看 description 是否说清触发条件。工具能调却总报参数错，多半是 schema 与实现两张皮。
