# 扩展 LubanCode

[文档首页](README.md) · [工具手册](tools.md) · [配置手册](configuration.md) · [架构说明](architecture.md)

LubanCode 留了六扇门：Skill、MCP、LSP、Lua、C ABI 插件、Hooks。分量不同，风险也不同。先挑最窄的一扇，够用便止。

## 1. 怎么选

| 路子 | 适合什么 | 运行位置 | 要不要重编主程序 | 风险 |
| --- | --- | --- | --- | --- |
| Skill | 教模型一套章法，附范例、模板、脚本 | 提示上下文；附带脚本另算 | 否 | 中 |
| MCP | 接数据库、浏览器、云服务、已有工具生态 | 独立 stdio 子进程 | 否 | 中至高 |
| LSP | 查定义、引用、符号、诊断 | 语言服务器子进程 | 否 | 中 |
| Lua | 写轻量本地工具 | LubanCode 进程内 | 否 | 高 |
| C ABI | 接原生库、系统 API、高性能逻辑 | LubanCode 进程内 | 插件要编，主程序不用 | 最高 |
| Hooks | 会话或工具前后跑命令，做审计与拦截 | 平台默认 shell 子进程 | 否 | 高 |

只要是“告诉模型该怎么做”，先用 Skill。要把独立服务暴露成工具，用 MCP。只查代码语义，用 LSP。Lua 和原生插件都进宿主进程，须当可执行代码审查。

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
| `description` | 讲清用途与参数，必填 |
| `input_schema` | JSON Schema 字符串，顶层通常是 object |
| `execute` | 收 Lua table，返回可转成文本的结果 |

若文件叫 `word_count.lua`，最终名称为：

```text
plugin__word_count__word_count
```

完整示例见 [examples/plugins/word_count.lua](../examples/plugins/word_count.lua)。插件在启动时扫描。改完 `.lua` 后要重启 LubanCode。

Lua 与宿主同进程。死循环会卡住会话，耗尽内存会拖垮程序。插件工具默认需要确认，但确认只能挡“是否调用”，挡不住插件内部写坏内存或滥用已开放的库。

## 6. C ABI 原生插件

现版原生插件只在 Windows 加载 `.dll`。公共 ABI 头在 [include/luban_plugin.h](../include/luban_plugin.h)。插件须导出：

```c
const luban_plugin_manifest* luban_plugin_entry(void);
```

示例构建：

```bash
cmake -S examples/plugins/hello_plugin -B build/hello-plugin
cmake --build build/hello-plugin --config Release
```

把主 DLL 与依赖 DLL 放进：

```text
%USERPROFILE%\.lubancode\plugins\
```

LubanCode 会略过没有 `luban_plugin_entry` 的依赖库。假设主文件叫 `hello_plugin.dll`，其中工具叫 `reverse_text`，最终名称为：

```text
plugin__hello_plugin__reverse_text
```

三条 ABI 硬规矩：

1. `api_version` 必须等于 `LUBAN_PLUGIN_API_VERSION`。
2. `execute` 返回的 `content` 必须是 UTF-8，并以 `\0` 收尾。
3. 谁分配，谁释放。插件须提供 `free_result`；宿主不跨 CRT 直接 `free`。

完整工程见 [examples/plugins/hello_plugin](../examples/plugins/hello_plugin)。原生插件可崩宿主，也能取得宿主进程权限。只加载信得过、版本对得上的二进制。

## 7. Hooks

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

## 8. 延迟挂载

注册表工具总数超过 `tool_search_threshold` 时，部分动态与低频工具先不把 schema 发给模型。模型只看见 `tool_search`，搜到工具后再挂载。

默认阈值是 20。设 `0` 关闭延迟，所有工具每轮都进 schema。工具多时，关闭会明显吃输入 token。

`/tools` 可看已注册、已挂载与延迟工具。工具名没出现在本轮 schema，不等于插件没加载。

## 9. 确认与权限

- Lua、原生插件工具默认确认。
- MCP 工具按外部工具处理，调用前可进入确认流程。
- LSP 查询只读，通常不问。
- Hooks 由配置直接触发，不另问。
- Skill 不是工具；它引出的实际工具仍各走自己的确认。

可信项目可在 `.lubancode/settings.local.json` 写 `allow_tools`、`allow_commands`、`deny_commands`。该文件是本机权限，不该提交。详见[配置手册](configuration.md#七settingslocaljson项目级本地权限)。

## 10. 分发

**Skill**

一项技能一个目录。只带必要资源。写清依赖、平台与安装步骤。随 LubanCode 发布的官方 Skill 放仓库 `skills/`，打包后保持同样目录；用户自装 Skill 仍落 `~/.lubancode/skills/`，升级程序不覆盖。

**MCP**

固定服务版本。把日志写 stderr。密钥走环境。提供一条能单独启动并自检的命令。

**Lua**

一文件一工具最省事。文件名别轻易改；一改，命名空间也跟着变。

**原生插件**

主 DLL 与依赖同目录。标明 ABI 版本、架构和 MSVC runtime。不要只发一枚来路不明的 DLL。

**Hooks**

命令要短，要有超时意识。跨平台仓库分别考虑 `cmd.exe` 与 `/bin/sh` 语法。

## 11. 发布前检查

- 工具名稳定，说明与 schema 对得上。
- 参数缺失、坏类型、外部服务退出都有清楚错误。
- 没把 API key 写进源码、示例、Skill、日志或目录。
- 写操作会触发正确确认。
- 输出设了上限，不把几百兆数据塞回模型。
- MCP stdout 没有日志。
- Lua/C ABI 经坏输入、重复调用与退出清理测试。
- 文档写明安装路径、重载方式和卸载办法。

## 12. 总排错

`/skills`、`/mcp`、`/lsp`、`/plugins`、`/tools` 五条命令先查状态。再看启动警告。

扩展文件改了却不生效，要分门看：Skill 管理命令会热刷新；LSP 进程按需拉起；MCP、Lua、原生插件和 Hooks 改配置后要重启进程。

某件工具已加载却模型不调用，先看它是否延迟挂载，再看 description 是否说清触发条件。工具能调却总报参数错，多半是 schema 与实现两张皮。
