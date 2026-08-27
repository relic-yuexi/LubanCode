# 扩展 LubanCode

[文档首页](../../README.md) · [工具手册](../../reference/tools.md) · [进程内插件深挖](../../architecture/extensions/plugin-runtime.md) · [配置手册](../../reference/configuration.md) · [安全模型](../../development/security.md) · [测试手册](../../development/testing.md) · [排错手册](../../getting-started/troubleshooting.md) · [架构说明](../../architecture/README.md)

LubanCode 留了七扇门：Skill、MCP、LSP、Lua、process 插件、native 插件、Hooks。分量不同，风险也不同。先挑最窄的一扇，够用便止。

## 1. 怎么选

| 路子 | 适合什么 | 运行位置 | 要不要重编主程序 | 风险 |
| --- | --- | --- | --- | --- |
| Skill | 教模型一套章法，附范例、模板、脚本 | 提示上下文；附带脚本另算 | 否 | 中 |
| MCP | 长驻服务、数据库连接池、已有 MCP 生态 | 独立 stdio 子进程 + JSON-RPC | 否 | 中至高 |
| LSP | 查定义、引用、符号、诊断 | 语言服务器子进程 | 否 | 中 |
| Lua | 写轻量本地工具，零依赖零编译 | LubanCode 进程内 | 否 | 高 |
| process 插件 | Python/Rust/Go/Node/C++ 可执行程序当工具 | 短命子进程（stdin/stdout JSON） | 否 | 中 |
| native 插件 | 接原生库、极低延迟、大数据量 | LubanCode 进程内（.dll/.so/.dylib） | 插件要编，主程序不用 | 最高 |
| Hooks | 会话或工具前后跑命令，做审计与拦截 | 平台默认 shell 子进程 | 否 | 高 |

只要是“告诉模型该怎么做”，先用 Skill。要把独立服务暴露成工具，用 MCP。只查代码语义，用 LSP。Lua 和原生插件都进宿主进程，须当可执行代码审查。

若功能只有一枚可信本地函数，不必硬搭 MCP server。三条短路按风险从低到高挑：

- **process 插件**（默认主路）：任何能从 stdin 读 JSON、往 stdout 写 JSON 的程序都能挂。Python 冷启动毫秒级，进程崩了只坏当次调用。写法见第 6 节。
- **Lua**：零依赖零编译，`~/.lubancode/plugins/` 丢一枚 `.lua` 即挂。pure 画像缺省关 `io`/`os.execute`，死循环有指令预算落锤。见第 5 节。
- **native 插件**：极低延迟、大数据不搬进程时才用；加载即执行库 constructor，崩了带倒宿主。ABI v2 三平台（Windows .dll / Linux .so / macOS .dylib）。见第 7 节。

设计动机、安装实证、ABI、并发与 corner case 见[进程内插件系统深挖](../../architecture/extensions/plugin-runtime.md)。

## 2. Skills

Skill 是一份按需展开的工作说明。它可带参考资料、模板和脚本。模型先看名称与摘要；任务命中后，再读完整 `SKILL.md`。

### 2.1 目录与优先级

```text
<exe-dir>/skills/<skill-name>/SKILL.md
<prefix>/share/lubancode/skills/<skill-name>/SKILL.md
~/.lubancode/skills/<skill-name>/SKILL.md
<project>/.lubancode/skills/<skill-name>/SKILL.md
```

前两处是同一层“官方级”：便携包和 Windows 安装从可执行文件旁找；POSIX 前缀安装还会找 `share/lubancode/skills`。发行包、安装脚本与 CMake install 会把官方 Skill 一并带上。

同名优先级为：项目级 > 用户级 > 官方级。旧版本曾把官方 `lubancode-config` 播种进用户目录；若那份文件带系统维护标记，现版会让它退位，改用发行包新版。真正由用户自建的同名 Skill 仍可覆盖官方级。

LubanCode 不自动扫描 `.codex/skills`、`.claude/skills` 或 `.agents/skills`；要用外部 Skill，先安装进自己的技能目录。

### 2.2 最小文件

```markdown
---
name: release-check
description: 发版前核对版本、测试、变更记录与产物。
---

# Release Check

先读版本号与 git 状态，再跑测试。任何一步失败，停下说明，不得打 tag。
```

`name` 要稳定。`description` 要写“何时该用”，别只写一串漂亮话。正文列步骤、边界和失败处置。脚本、模板、参考资料放在同一目录，用相对路径引用。

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

Lua 与宿主同进程，风险用三道软墙兜底（`pure` 画像缺省生效）：

- **库关门**：`os.execute`、`os.exit`、`io`、`package.loadlib` 拿不到——要文件与命令能力的，改走 process 插件（进程隔离），不要悄悄开 trusted。
- **指令预算**：死循环约 2 亿条虚拟机指令内被 `luaL_error` 掐断，报错文案自带预算数；ESC 也走同一条落锤路。
- **内存帽**：狂吃内存按 OOM 报脚本错误，宿主堆不破。

三道墙都是软的：拦跑野的脚本，不是恶意绕洞。真不可信代码走 process 隔离。`trusted` 画像（全开）须显式批准。插件工具默认需要确认，但确认只能挡“是否调用”，挡不住插件内部写坏内存。

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

### 6.2 协议 v1

请求（stdin，恰好一份 JSON，写完即关，脚本可 `json.load(sys.stdin)` 读到 EOF）：

```json
{"protocol": 1, "call_id": "req-7", "plugin": "local-math", "tool": "add",
 "arguments": {"a": 1, "b": 2}, "context": {"cwd": "D:/project"}}
```

响应（stdout，恰好一份 JSON）：

```json
{"protocol": 1, "call_id": "req-7", "ok": true,
 "content": [{"type": "text", "text": "3"}], "structured": 3}
```

铁律：

- stdout 是结果专线。日志只写 stderr；stdout 前后混任何字节，整次调用判协议错，不从字堆里猜 JSON。
- `content` 首版只认 `type=text`；别的类型按协议错拒，不静默转字符串。
- 非零退出、崩溃、超时、取消、坏 UTF-8、坏 JSON、call_id 不合、输出超限各有唯一宿主错误码，`/plugin doctor` 与错误文案对得上。
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

单工具示例见 [examples/plugins/](../../../examples/plugins/README.md)；要看的“多工具、有安全合同的完整插件”，见 **[examples/agents/gui-agent](../../../examples/agents/gui-agent/README.md)**——九件 GUI 工具（截图/枚举/聚焦/移动/点击/滚轮/输入/按键/状态），带：

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

注册表工具总数超过 `tool_search_threshold` 时，部分动态与低频工具先不把 schema 发给模型。模型只看见 `tool_search`，搜到工具后再挂载。

默认阈值是 20。设 `0` 关闭延迟，所有工具每轮都进 schema。工具多时，关闭会明显吃输入 token。

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

一项技能一个目录。只带必要资源。写清依赖、平台与安装步骤。随 LubanCode 发布的官方 Skill 放仓库 `skills/`，打包后保持同样目录；用户自装 Skill 仍落 `~/.lubancode/skills/`，升级程序不覆盖。

**MCP**

固定服务版本。把日志写 stderr。密钥走环境。提供一条能单独启动并自检的命令。

**Lua**

一文件一工具最省事。文件名别轻易改；一改，命名空间也跟着变。声明只用 pure 画像认的库（string/table/math/os.time 之类），用户挂上即用，不必开 trusted。

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
- 写操作会触发正确确认。
- 输出设了上限，不把几百兆数据塞回模型。
- MCP stdout 没有日志；process 插件 stdout 恰好一份 JSON、日志全在 stderr。
- Lua/native 经坏输入、重复调用与退出清理测试；native 另在独立进程里测过崩溃路径。
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
| native 库没挂上 | 扩展名/架构/ABI 不合 | 启动警告给 `abi_tag` 与错误码;`.dll`/`.so`/`.dylib` 按平台放(第 7 节) |
| native 库挂上但调用崩宿主 | 插件野指针/ABI 错配 | native 插件崩了带倒宿主——先在独立进程里测插件本体 |
| 项目插件没挂上 | 未经信任 | 警告给内容指纹;批准走 `~/.lubancode/plugin-trust.json`(第 6.5 节) |
| 模型不调用已挂的工具 | 延迟挂载或 description 没说清时机 | `/tools` 看三态;description 写"何时该用" |
| 调用总报参数错 | schema 与实现两张皮 | manifest 的 `input_schema` 就是合同,调用前宿主先验;两处对齐 |

某件工具已加载却模型不调用，先看它是否延迟挂载，再看 description 是否说清触发条件。工具能调却总报参数错，多半是 schema 与实现两张皮。
