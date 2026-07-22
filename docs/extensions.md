# 扩展 LubanCode

[文档首页](README.md) · [配置手册](configuration.md) · [架构说明](architecture.md) · [中文 README](../README.md) · [English README](../README.en.md)

LubanCode 有四条扩展路：Skill、MCP/LSP、Lua、C ABI DLL。它们分量不同，风险也不同。能用 Skill 说清的事，不必先写 DLL；要接现成服务，MCP 往往比自造协议省事。

## 怎么选

| 路子 | 适合什么 | 是否进程内执行 | 平台 |
| --- | --- | --- | --- |
| Skill | 教模型一套做事章法，带说明、范例与约束 | 否，本质是提示与资源 | 全平台 |
| MCP | 接独立工具服务、数据库、浏览器或已有生态 | 否，stdio 子进程 | 全平台 |
| LSP | 查定义、引用、符号与诊断 | 否，语言服务器子进程 | 全平台 |
| Lua | 写一件轻量、可分发的模型工具 | 是 | 全平台 |
| C ABI DLL | 接原生库、系统 API、高性能逻辑 | 是 | Windows |

## Skills

用户级技能放在：

```text
~/.lubancode/skills/<skill-name>/SKILL.md
```

项目级技能放在：

```text
<project>/.lubancode/skills/<skill-name>/SKILL.md
```

同名时，项目级盖过用户级。最小文件如下：

```markdown
---
name: release-check
description: 发版前核对版本、测试、变更记录与产物。
---

# Release Check

先读版本号与 git 状态，再跑测试。任何一步失败，停下说明，不得打 tag。
```

会话里可用：

```text
/skills
/skill list
/skill install https://github.com/owner/repo/tree/main/my-skill
/skill update my-skill
/skill remove my-skill
```

远端技能会落进用户目录。安装前先读内容。Skill 能引导模型调用工具，来路不明的一样有风险。

## MCP

MCP 服务器写进 `config.json` 顶层 `mcpServers`：

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

握手成功后，工具名形如：

```text
mcp__local-tools__tool_name
```

`/mcp` 可看服务器与工具清单。服务器按 stdio 通信，stdout 须留给协议帧；调试日志请写 stderr。

## LSP

LSP 配置按扩展名把文件路由到语言服务器：

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

配好后，模型可查 `definition`、`references`、`symbols`、`diagnostics`。服务懒启动，闲置后自动关。`/lsp` 可看状态。

MCP、LSP 完整字段见 [配置手册](configuration.md#四hooks--mcpservers--search--lsp)。

## Lua 插件

把 `.lua` 文件放进 `~/.lubancode/plugins/`。每个文件返回一张表：

```lua
return {
    name = "word_count",
    description = "统计文本里的词数。入参 {\"text\": string}。",
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

若文件名是 `word_count.lua`，最终工具名便是：

```text
plugin__word_count__word_count
```

完整示例在 [examples/plugins/word_count.lua](../examples/plugins/word_count.lua)。Lua 插件与宿主同进程。死循环、耗尽内存、调用危险库，都会连累主程序。

## C ABI DLL 插件

原生插件只在 Windows 加载。公共头文件是 [include/luban_plugin.h](../include/luban_plugin.h)。插件须导出：

```c
const luban_plugin_manifest* luban_plugin_entry(void);
```

最小流程：

```bash
cmake -S examples/plugins/hello_plugin -B build/hello-plugin
cmake --build build/hello-plugin --config Release
```

把产出的 `hello_plugin.dll` 放进 `%USERPROFILE%\.lubancode\plugins\`。工具名会加前缀：

```text
plugin__hello_plugin__reverse_text
```

有三条硬规矩：

1. `api_version` 必须等于 `LUBAN_PLUGIN_API_VERSION`。
2. `execute` 返回的 `content` 必须是 UTF-8、以 `\0` 收尾。
3. 谁分配内存，谁释放。插件要提供 `free_result`，宿主不会跨 CRT 直接 `free`。

完整工程在 [examples/plugins/hello_plugin](../examples/plugins/hello_plugin)。DLL 与宿主同进程。野指针、越界、除零，宿主兜不住。

## 挂载与确认

- `/plugins` 列出已挂载的 Lua 与 DLL 工具。
- 插件工具默认需要确认。
- 工具总数超过 `tool_search_threshold` 时，外挂工具可能先处于延迟挂载状态；模型用 `tool_search` 找到后再调用。
- `settings.local.json` 可为可信工具设项目级权限。黑白名单写法见 [项目级权限](configuration.md#七settingslocaljson项目级本地权限)。

## 分发建议

- Skill 用独立目录，连同 `SKILL.md` 与必要资源一起发。
- Lua 插件一文件一工具，文件名保持稳定；改名会改工具前缀。
- DLL 主文件与依赖库可放同一目录。LubanCode 会略过没有 `luban_plugin_entry` 的依赖 DLL。
- 不要把 API key 写进 Skill、Lua、DLL、示例或日志。MCP 密钥走配置里的 `env`，模型服务密钥走 `key_env`。

