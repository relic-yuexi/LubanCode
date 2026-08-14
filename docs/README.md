<p align="center">
  <img src="assets/lubancode-mark.png" alt="LubanCode mark" width="132">
</p>

# LubanCode 文档

<p align="center">
  <a href="../README.md">中文首页</a> · <a href="../README.en.md">English README</a> · <a href="https://github.com/relic-yuexi/LubanCode/releases">Releases</a>
</p>

这里收 `v0.25.1` 的使用手册与设计说明。当前 Release 测试为 `1244/1244`，共 `5999` 条断言。字段名、命令名若与文档冲突，以 `lubancode --help`、`lubancode --config` 和当前源码为准。

## 先找路

| 你眼下要做什么 | 先读 | 再读 |
| --- | --- | --- |
| 安装并跑起第一场会话 | [根 README](../README.md#安装) | [命令与按键](commands.md) |
| 看软件到底能做什么 | [功能全览](feature-reference.md) | [工具参考](tools.md) |
| 接一家或多家模型服务 | [配置手册](configuration.md) | [Provider 目录](provider-catalog.md) |
| 查所有 slash 命令、启动参数和快捷键 | [命令与按键](commands.md) | [终端交互](terminal-ui.md) |
| 查某件模型工具的参数与上限 | [工具参考](tools.md) | [配置手册](configuration.md) |
| 恢复、压缩或导出会话 | [会话与上下文](sessions-and-context.md) | [项目记忆](memory-system-design.md) |
| 写 Skill、接 MCP/LSP 或做插件 | [扩展指南](extensions.md) | [工具参考](tools.md) |
| 用 `/init` 给仓库立规矩 | [项目指令](project-instructions.md) | [配置手册](configuration.md) |
| 看模块、线程、进程和数据怎样流 | [架构说明](architecture.md) | [终端交互](terminal-ui.md) |
| 准备演示、简历或面试 | [求职项目手册](job-portfolio.md) | [架构说明](architecture.md) |

## 文档地图

### 使用手册

| 页面 | 收什么 |
| --- | --- |
| [功能全览](feature-reference.md) | 运行模式、协议、工具、代理、终端、会话、记忆、扩展、安全和当前边界。 |
| [命令与按键](commands.md) | 8 个公开启动参数、29 个 slash 命令、编辑/菜单/工具条目按键与非交互降级。 |
| [工具参考](tools.md) | 14 类内置或条件工具、动态 MCP/插件工具、参数、限制、确认和排错。 |
| [终端交互](terminal-ui.md) | 多行编辑、粘贴、消息排队、Markdown/LaTeX、diff、工具转录、状态面板与图片。 |
| [会话与上下文](sessions-and-context.md) | JSONL、恢复、标题、导出、token、缓存命中、压缩与项目记忆边界。 |

### 配置与项目

| 页面 | 收什么 |
| --- | --- |
| [配置手册](configuration.md) | 配置层级、所有顶层字段、环境变量、providers、hooks、MCP、搜索、LSP、权限与目录。 |
| [Provider 目录](provider-catalog.md) | 内置/在线目录、缓存、预设、模型 variant、合并优先级与维护办法。 |
| [项目指令](project-instructions.md) | `/init`、层级加载、override、32 KiB 上限、写法与排错。 |
| [项目记忆](memory-system-design.md) | 已实现路径、检索、后台写入、命令、安全，以及仍未实现的后续阶段。 |
| [界面多语言](i18n.md) | 语言包格式、回退链、添加新语言、覆盖内置措辞与排错。 |

### 扩展与开发

| 页面 | 收什么 |
| --- | --- |
| [扩展指南](extensions.md) | Skill、MCP、LSP、Lua、C ABI、hooks、延迟挂载、命名和信任边界。 |
| [架构说明](architecture.md) | 组件分层、三协议、请求链、工具表、提示词、会话、终端并发、平台抽象与测试。 |
| [提示词模块](../src/prompts/README.md) | 内置 prompt 怎样拆分、构建嵌入、运行时播种和覆盖。 |
| [求职项目手册](job-portfolio.md) | 项目数据、架构讲法、面试故事、演示脚本与源码证据。 |

## 十分钟上手

### 1. 启动

```powershell
lubancode
```

缺配置时，向导会依次问语言、协议、地址、密钥与模型。写完直接进会话，不必重启。

### 2. 给仓库立规矩

```text
/init
```

它在 Git 根生成 `AGENTS.md`，按仓库文件填入构建测试命令。已有文件不覆盖，当前会话立即重载。

### 3. 交代任务

```text
先读项目结构，找出配置入口和测试命令。不要改文件。
```

模型会用 `search`、`read_file` 或 LSP 查仓库。要改文件时，`write_file` / `edit_file` 先画 diff，再走确认。

### 4. 验证与续聊

```text
/todos
/context
/export
```

下次从同一目录启动：

```powershell
lubancode --continue
```

## 常用工作流

### 多 Provider

```text
/provider add
/provider list
/provider switch work
/model
/think high
```

预设能带出地址、协议、窗口和默认模型；密钥最好放 `key_env`。配置细节见[配置手册](configuration.md#providers-数组字段)。

### 长任务

1. 让模型用 `todo_write` 列计划。
2. 大范围检索交给 `agent` 子代理。
3. 模型工作时直接输入下一条，消息会排队。
4. `Ctrl+O` 展开参数与全文，`Ctrl+E` 聚焦单条。
5. 上下文逼近窗口时自动压缩；也可手工 `/compact`。

### 外接工具

```text
/mcp
/lsp
/plugins
/tools
```

MCP 与 LSP 是进程外服务；Lua 与 DLL 在宿主进程内。来路不明的扩展先读代码，再装。详见[扩展指南](extensions.md)。

### 检查新版

```text
/update
```

它只查 GitHub 最新 Release，不自动覆盖程序。升级时运行新版包内安装脚本，官方 Skills 会跟着更新，用户 Skills 保留。脚本场景可用 `lubancode --check-update`。

## 数据住在哪里

```text
~/.lubancode/
  config.json                    全局配置
  models.json                    用户模型目录
  cache/provider-catalog.json    在线 Provider 目录缓存
  system_prompt.md               法：人格段
  SOUL.md                        魂：风格叠加
  prompts/                       运行时 prompt 模块
  sessions/                      JSONL 会话
  projects/<key>/memory/         项目记忆
  memory-jobs/                   记忆后台任务
  skills/                        用户级 Skill
  plugins/                       Lua 与 DLL
  languages/                     外部语言包

<exe-dir>/skills/                便携包、Windows 安装与开发构建的官方 Skill
<prefix>/share/lubancode/skills/ POSIX 前缀安装的官方 Skill

<project>/.lubancode/
  config.json                    项目级配置
  settings.local.json            本地权限，不该提交
  skills/                        项目级 Skill
```

`AGENTS.md` 跟仓库走；项目记忆与 session 不写进仓库。配置的具体覆盖规则见[配置手册](configuration.md#一配置分层与优先级)。

Skill 同名时，项目级压用户级，用户级压官方级。官方 Skill 随发行包更新；用户目录不会被程序升级当成普通官方资源覆盖。

## 平台状态

| 平台 | 编译器 | 终端路径 | 发行包 |
| --- | --- | --- | --- |
| Windows x64 | MSVC | Win32 控制台、Windows Terminal、VS Code Terminal | `.zip` |
| Linux x64 | GCC | POSIX TTY；复杂原地重画按能力降级 | `.tar.gz` |
| macOS arm64 | Clang | POSIX TTY；复杂原地重画按能力降级 | `.tar.gz` |

管道与重定向自动用 plain 主题，不输出动画和原地改写。设 `LUBANCODE_FORCE_COLOR=1` 可强制颜色，但不会把管道伪装成真终端。

## 安全边界

- `confirm`、`auto`、`yolo` 三档只管确认策略，不是操作系统沙箱。
- `--yes` 与 yolo 是显式全放。项目 deny 规则不会假装替用户推翻它。
- `key_env` 比明文 `api_key` 更稳；示例、日志、Skill、插件都不该写 key。
- hooks、MCP、Lua、DLL、Skill 都可能引导或执行代码。安装来源须可信。
- Lua 与 DLL 在宿主进程内；不可信扩展应改走 MCP 进程边界。
- 项目记忆默认关闭，也不能由受版本控制的项目配置自行开启。

## 文档维护规矩

改用户可见功能时，至少检查这些页：

1. [功能全览](feature-reference.md)有没有登记。
2. [命令与按键](commands.md)或[工具参考](tools.md)是否要补入口与参数。
3. 对应专题页有没有写成功路、失败路和安全边界。
4. [更新记录](../CHANGELOG.md)有没有一条用户能看懂的变化。

数据刷新用：

```powershell
cmake --build build\release --config Release --target lubancode_tests
.\build\release\tests\Release\lubancode_tests.exe --no-skip
git status --short
```

文档不要抄未来设计当成现状。实现到哪便写到哪；未实现的单列“后续”，不能混进功能表。
