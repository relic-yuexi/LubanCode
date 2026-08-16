<p align="center">
  <img src="assets/lubancode-mark.png" alt="LubanCode mark" width="132">
</p>

# LubanCode 文档

<p align="center">
  <a href="../README.md">中文首页</a> · <a href="../README.en.md">English README</a> · <a href="https://github.com/relic-yuexi/LubanCode/releases">Releases</a>
</p>

这里收 LubanCode 当前主线的用户手册、参考资料与工程说明。版本号以 `CMakeLists.txt` 和 `src/app/version.hpp` 为准；已发布变化看 [CHANGELOG](../CHANGELOG.md)。文档若与 `lubancode --help`、`lubancode --config` 或源码冲突，以程序和源码为准，并把文档差错补回来。

## 先找路

| 你眼下要做什么 | 先读 | 再读 |
| --- | --- | --- |
| 安装并跑起第一场会话 | [根 README](../README.md#安装) | [命令与按键](commands.md) |
| 看软件到底能做什么 | [功能全览](feature-reference.md) | [工具参考](tools.md) |
| 接一家模型服务 | [配置手册](configuration.md) | [Provider 目录](provider-catalog.md) |
| 查 slash 命令或快捷键 | [命令与按键](commands.md) | [终端交互](terminal-ui.md) |
| 查工具参数、确认与上限 | [工具参考](tools.md) | [安全模型](security-model.md) |
| 恢复、导出或压缩会话 | [会话与上下文](sessions-and-context.md) | [Query 数据流](query-data-flow.md) |
| 开项目记忆 | [项目记忆](memory-system-design.md) | [安全模型](security-model.md) |
| 写 Hook | [Hooks 手册](hooks.md) | [配置手册](configuration.md) |
| 用程序化工具调用 | [PTC 手册](ptc.md) | [工具参考](tools.md) |
| 写 Skill、接 MCP/LSP 或做插件 | [扩展指南](extensions.md) | [安全模型](security-model.md) |
| 用 `/init` 给仓库立规矩 | [项目指令](project-instructions.md) | [配置手册](configuration.md) |
| 编译、调试或改代码 | [开发指南](development-guide.md) | [架构说明](architecture.md) |
| 加测试或跑终端回归 | [测试指南](testing-guide.md) | [终端交互](terminal-ui.md) |
| 遇到故障，先找排查顺序 | [排错手册](troubleshooting.md) | 对应专题页 |
| 改文档或审文档 PR | [文档规范](documentation-standard.md) | [命名与计数规范](naming-conventions.md) |

## 文档分层

文档分五层。每页只管自己那层，少抄，勤链。

### 1. 产品入口

| 页面 | 职责 |
| --- | --- |
| [根 README](../README.md) | 安装、产品定位、最短上手与发行入口。 |
| [功能全览](feature-reference.md) | 当前已实现能力的总账，不收未来设想。 |
| [命令与按键](commands.md) | 启动参数、slash 命令与键位参考。 |

### 2. 用户参考

| 页面 | 职责 |
| --- | --- |
| [配置手册](configuration.md) | 配置分层、字段、环境变量与合并规矩。 |
| [Provider 目录](provider-catalog.md) | Provider/模型目录 schema、缓存与维护。 |
| [工具参考](tools.md) | 工具 schema、上限、确认、结果与排错。 |
| [Hooks 手册](hooks.md) | 事件、匹配、stdin/stdout、归并与信任审查。 |
| [PTC 手册](ptc.md) | 程序化工具调用、能力画像、runner 与边界。 |
| [界面多语言](i18n.md) | 语言包、回退链与新增语言。 |

### 3. 工作指南

| 页面 | 职责 |
| --- | --- |
| [终端交互](terminal-ui.md) | composer、排队、转录、公式、diff 与面板。 |
| [会话与上下文](sessions-and-context.md) | history、session、token、缓存与 compact。 |
| [项目指令](project-instructions.md) | `/init`、AGENTS 层级、覆盖与上限。 |
| [项目记忆](memory-system-design.md) | 召回、候选、学习档、后台写入与数据边界。 |
| [扩展指南](extensions.md) | Skill、MCP、LSP、Lua、C ABI 与分发。 |
| [排错手册](troubleshooting.md) | 从症状出发的检查顺序与证据清单。 |

### 4. 内部设计

| 页面 | 职责 |
| --- | --- |
| [架构说明](architecture.md) | 模块边界、启动、请求链、线程与平台。 |
| [Query 数据流](query-data-flow.md) | 一条输入怎样走过 prompt、history、wire、工具与子代理。 |
| [命名与计数规范](naming-conventions.md) | turn、step、request、token 等统一词典。 |
| [提示词模块](../src/prompts/README.md) | 内置 prompt 的拆分、嵌入、播种与覆盖。 |

### 5. 工程与治理

| 页面 | 职责 |
| --- | --- |
| [开发指南](development-guide.md) | 工具链、构建、目录、改动落位与本地工作流。 |
| [测试指南](testing-guide.md) | 单测、集成、真终端、真模型、CI 与基准口径。 |
| [安全模型](security-model.md) | 信任边界、权限、密钥、扩展与本地数据。 |
| [文档规范](documentation-standard.md) | 文档类型、权威来源、写法、同步矩阵与验收。 |
| [求职项目手册](job-portfolio.md) | 演示与面试素材；不是产品事实的权威来源。 |

## 事实听谁的

同一件事不许多页各写一套。冲突时按下表追根。

| 事实 | 权威来源 | 文档入口 |
| --- | --- | --- |
| 当前版本 | `CMakeLists.txt`、`src/app/version.hpp` | [CHANGELOG](../CHANGELOG.md) |
| 启动参数 | `src/app/cli_options.*` | [命令与按键](commands.md) |
| Slash 命令 | `src/cli/slash_commands.*`、`src/app/interactive_session.cpp` | [命令与按键](commands.md) |
| 配置字段与默认值 | `src/config/config.hpp`、`src/config/config.cpp` | [配置手册](configuration.md) |
| Provider schema | `src/config/provider_catalog.*`、`catalog/providers.json` | [Provider 目录](provider-catalog.md) |
| 工具名与 schema | 各 `src/tools/*` 实现、`ToolRegistry` | [工具参考](tools.md) |
| Hook 事件与决策 | `src/hooks/*`、配置解析 | [Hooks 手册](hooks.md) |
| 终端键位与状态机 | `src/cli/line_editor.*`、`console_input.*` | [终端交互](terminal-ui.md) |
| 会话格式 | `src/agent/session_store.*` | [会话与上下文](sessions-and-context.md) |
| 测试目标 | `tests/CMakeLists.txt`、`.github/workflows/ci.yml` | [测试指南](testing-guide.md) |
| 发行包 | `.github/workflows/release.yml`、安装脚本 | [开发指南](development-guide.md) |

## 十分钟上手

### 1. 启动

```powershell
lubancode
```

缺配置时，向导会问语言、协议、地址、鉴权与模型。写完便进会话。

### 2. 给仓库立规矩

```text
/init
```

它在 Git 根生成 `AGENTS.md`。已有文件不覆盖，当前会话立即重载。

### 3. 交代任务

```text
先读项目结构，找出配置入口和测试命令。不要改文件。
```

模型会用文件、搜索或 LSP 工具查仓库。要改文件时，写入工具先画 diff，再按当前确认档执行。

### 4. 收尾

```text
/todos
/context
/export
```

下次从同一目录续接：

```powershell
lubancode --continue
```

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

<project>/.lubancode/
  config.json                    项目级配置
  settings.local.json            本地权限，不该提交
  skills/                        项目级 Skill
```

官方 Skill 随发行包走。项目记忆与 session 留在用户目录，不写进仓库。完整目录与覆盖规矩见[配置手册](configuration.md)。

## 三条总规矩

1. **现状与计划分开。** 已实现能力进功能表；未来设计进“后续”或 `todos/`。
2. **规范与例子分开。** 规范页定契约，专题页给例子；别让一段样例反过来定义协议。
3. **数字须能复测。** 性能、缓存、token、测试数都要写环境、样本与来源。一次手测不进产品总览。

改用户可见行为时，从[文档规范](documentation-standard.md)的同步矩阵查该动哪些页。改完至少查本地链接、示例命令与 `git diff --check`。
