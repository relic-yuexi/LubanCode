<p align="center">
  <img src="assets/lubancode-mark.png" alt="LubanCode mark" width="132">
</p>

# LubanCode 文档

<p align="center">
  <a href="../README.md">中文首页</a> · <a href="../README.en.md">English README</a> · <a href="https://github.com/relic-yuexi/LubanCode/releases">Releases</a>
</p>

这里不摆宣传话，只收能拿来做事的说明。当前文档对应 `v0.23.0`。字段名、命令名若与程序输出冲突，以 `lubancode --help`、`lubancode --config` 与源码为准。

## 从哪一页读起

| 你要做什么 | 去哪里 |
| --- | --- |
| 十分钟内装好、跑起来 | [中文 README：安装与快速上手](../README.md#安装) |
| 接模型服务，管多个 provider | [配置手册](configuration.md) |
| 配 hooks、MCP、搜索或 LSP | [配置手册：外接服务](configuration.md#四hooks--mcpservers--search--lsp) |
| 写 Skill、Lua 工具或原生插件 | [扩展指南](extensions.md) |
| 看模块怎么分、请求怎么走 | [架构说明](architecture.md) |
| 看工作动画、排队、提问与确认 | [终端交互](terminal-ui.md) |
| 改系统提示词模块 | [提示词模块说明](../src/prompts/README.md) |
| 查全部命令与按键 | 运行 `lubancode --help`，或在会话里输入 `/help` |

## 推荐路线

### 只想用

先读 [README](../README.md)，跑完初次向导。需要第二家模型服务，再翻 [provider 配置](configuration.md#providers-数组字段)。别一上来便手写整份配置，向导和 `/provider add` 足够应付大半场景。

### 想接进自己的工作流

先看 [项目级权限](configuration.md#七settingslocaljson项目级本地权限) 与 hooks。再按需要接 MCP、LSP 或 Skill。外部工具都能执行代码，装之前要看清来源。

### 想改源码

从 [架构说明](architecture.md) 起步。主链是 `cli -> agent -> api/tools`，平台差异收在 `src/platform/`。新工具通常落进 `src/tools/`，再进 registry；新模型协议则不该塞进现有后端里硬凑。

## 文档边界

- `README.md` 与 `README.en.md` 管入口、安装与常用工作流。
- `docs/` 管稳定接口与设计边界。
- `src/prompts/README.md` 管提示词源文件。
- 根目录三份协议笔记是开发参考，不等同于 LubanCode 的公开配置接口。
- `git log --oneline` 记功能来路；GitHub Releases 记对外版本。

## 平台状态

| 平台 | 编译器 | CI | 发行包 |
| --- | --- | --- | --- |
| Windows x64 | MSVC | Build + Test | `.zip` |
| Linux x64 | GCC | Build + Test | `.tar.gz` |
| macOS arm64 | Clang | Build + Test | `.tar.gz` |

推送 `v*` 标签后，发布流水线会创建 Release、生成说明，并挂上三份包。
