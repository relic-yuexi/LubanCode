<p align="center">
  <img src="docs/assets/lubancode-banner.png" alt="LubanCode" width="100%">
</p>

<h1 align="center">LubanCode</h1>

<p align="center"><strong>一把装在终端里的 AI 编程工具。C++23 写成，双协议接入，能读代码，也能动手干活。</strong></p>

<p align="center">
  <strong>简体中文</strong> · <a href="README.en.md">English</a>
</p>

<p align="center">
  <a href="https://github.com/relic-yuexi/LubanCode/actions/workflows/ci.yml"><img src="https://github.com/relic-yuexi/LubanCode/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/relic-yuexi/LubanCode/actions/workflows/release.yml"><img src="https://github.com/relic-yuexi/LubanCode/actions/workflows/release.yml/badge.svg" alt="Release"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white" alt="C++23">
  <img src="https://img.shields.io/badge/version-0.23.5-CB2C31" alt="v0.23.5">
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-444444" alt="Windows, Linux and macOS">
</p>

LubanCode 原生支持 Anthropic Messages API 与 OpenAI Responses API。模型能读文件、改代码、跑命令、查资料，也能调度子代理、MCP 与 LSP。界面不只是几行日志。流式正文、Markdown、diff、确认档、会话存档、上下文压缩，都在终端里铺开。

鲁班造物，先正绳墨，再下斧凿。LubanCode 也守这条规矩：先看清，再动手；改了什么，明明白白摆给你看。

> 当前版本：`v0.23.5`。Windows、Ubuntu、macOS 三路 CI 均会编译并跑全量测试。

## 一眼看懂

| | 能力 |
| --- | --- |
| **模型接入** | Anthropic / Responses 双协议；多 provider 随时切换并记住上次选择；`extra_body`、`extra_headers` 可透传厂商私有参数。 |
| **代码工具** | 读、写、容错编辑、搜索文件；前台或后台跑命令；改动先看 diff，再落盘。 |
| **语义与外接工具** | LSP 定义、引用、符号、诊断；MCP stdio；联网搜索与网页抓取。 |
| **代理工作流** | 子代理、待办清单、`ask_user` 选择题、`AGENTS.md` 项目指令、工具延迟挂载、隔离 worktree、项目级权限。 |
| **终端体验** | 分段 Markdown 渲染、动态工作状态、常驻消息队列、折叠式多行粘贴、逐键编辑、折叠与聚焦、三档确认。 |
| **上下文与存档** | token 占用分析、自动压缩、独立压缩模型、会话恢复、标题、Markdown 导出、默认关闭的项目记忆。 |
| **扩展与定制** | Skills、Lua 工具、C ABI DLL 插件、hooks、主题、i18n、soul 与 system prompt。 |

## 安装

### Windows：一行安装

PowerShell 5.1 及以上可直接拉取最新 Release：

```powershell
irm https://raw.githubusercontent.com/relic-yuexi/LubanCode/main/scripts/install.ps1 | iex
```

程序会装到 `%LOCALAPPDATA%\Programs\lubancode`，并写入当前用户 PATH。无需管理员权限。重复执行便是覆盖升级。

手工安装也成。到 [Releases](https://github.com/relic-yuexi/LubanCode/releases) 下载 `lubancode-vX.Y.Z-windows-x64.zip`，解压后运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

卸载时运行包里的 `uninstall.ps1`。

### Linux / macOS

从 [Releases](https://github.com/relic-yuexi/LubanCode/releases) 下载对应压缩包，解开，进入目录：

```bash
./install.sh
```

若 `~/.local/bin` 已在 PATH，脚本便装到那里；否则转去 `/usr/local/bin`，需要时会提示 `sudo`。

| 平台 | 发行包 |
| --- | --- |
| Windows x64 | `lubancode-vX.Y.Z-windows-x64.zip` |
| Linux x64 | `lubancode-vX.Y.Z-linux-x64.tar.gz` |
| macOS arm64 | `lubancode-vX.Y.Z-macos-arm64.tar.gz` |

### 从源码构建

需要 CMake 3.21 以上，以及支持 C++23 的编译器。依赖先走 vcpkg manifest；找不到 vcpkg，CMake 会改用 FetchContent。

Windows：

```powershell
cmake --preset release
cmake --build --preset release
.\build\release\Release\lubancode.exe --version
```

Linux / macOS：

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/lubancode --version
```

Debian / Ubuntu 缺编译环境时，先装：

```bash
sudo apt-get install -y build-essential cmake ninja-build libssl-dev
```

## 快速上手

第一次运行，不带参数即可。缺少模型配置时，向导会问语言、协议、地址、密钥与模型，写好配置后径直进会话。

```text
$ lubancode
=== lubancode 初次配置向导 ===
界面语言 / Language: 1) 中文  2) English
接口格式: 1) anthropic  2) responses
base_url: https://your-provider.example/v1
api_key: sk-...
model: your-model
```

往后可这样用：

```bash
# 交互会话
lubancode

# 单发任务，工具照常可用
lubancode "先读项目，再找出最该修的三个问题"

# 接管本目录最近一场会话
lubancode --continue

# 管道模式
git diff --cached | lubancode "替我审一遍这份改动"
```

若你管着多家模型服务，用 `providers` 更省事。密钥只记环境变量名，不必写进 JSON：

```json
{
  "active_provider": "work",
  "providers": [
    {
      "name": "work",
      "wire": "responses",
      "base_url": "https://your-provider.example/v1",
      "key_env": "WORK_MODEL_API_KEY",
      "model": "your-model",
      "context_window": "256k"
    }
  ]
}
```

把它存到 `~/.lubancode/config.json`，再设好 `WORK_MODEL_API_KEY`。`/provider switch work` 成功后也会自动写入 `active_provider`，下次启动仍走这一路。完整字段、优先级与厂商参数透传，见 [配置手册](docs/configuration.md)。

## 项目指令

进入仓库，敲一声 `/init`。LubanCode 会在 Git 根生成 `AGENTS.md`，填入项目布局、构建测试与改动规矩。已有文件便只读不改。文件写成后，当前会话立刻重载，主代理、子代理一并照办。

启动时，LubanCode 从 Git 根一路走到当前目录。每层先看 `AGENTS.override.md`，再看 `AGENTS.md`；近处内容排在后头，能压过远处。空文件跳过，总量封顶 32 KiB。细则见 [项目指令](docs/project-instructions.md)。

## 常用命令

| 命令 | 用处 |
| --- | --- |
| `/provider` | 添加、列出、切换、删除模型服务。 |
| `/init` | 生成并载入项目级 `AGENTS.md`。 |
| `/model` · `/think` | 切模型与推理强度。 |
| `/context` · `/compact` | 看上下文占用，手工压缩历史。 |
| `/skills` · `/skill install <url>` | 管理本地与远端技能。 |
| `/mcp` · `/lsp` · `/plugins` | 看外接工具与语言服务器状态。 |
| `/tools` · `/todos` | 看工具挂载状态与待办清单。 |
| `/memory` | 管本场项目记忆、同步召回与后台写入。 |
| `/sessions` · `/resume` · `/export` | 列存档、续聊、导出 Markdown。 |
| `/worktree new\|list\|exit` | 在隔离工作树里干活。 |
| `/soul` · `/prompt` | 调风格，或替换系统提示人格段。 |
| `/language` · `/image` | 切界面语言，附本地图片。 |
| `/help` | 在程序里看完整命令表。 |

几个键也常用：

- `Shift+Tab`：循环切换 `confirm`、`auto`、`yolo`。
- `Ctrl+O`：工具输出在紧凑与详细之间切换。
- `Ctrl+E`：聚焦查看当前工具条目全文。
- `Shift+Enter`：输入框里换行。
- 多行粘贴：自动折成 `[粘贴内容 N 字符]`，发送时仍交付完整原文。
- `Esc`：打断当前轮，或退出聚焦画面。

模型作答时可直接键入下一条并回车。消息会留在输入框上方，当前回合收尾后依次发送。

## 扩展

LubanCode 留了四扇门：

1. **Skills**：一份带 frontmatter 的 `SKILL.md`，可放主目录，也可随项目走。
2. **MCP / LSP**：在配置里挂 stdio 服务与语言服务器。
3. **Lua 插件**：一个 `.lua` 文件就是一件工具，适合轻量扩展。
4. **C ABI 插件**：Windows DLL 同进程加载，适合原生能力与已有 C/C++ 库。

写法、目录、示例与安全边界，见 [扩展指南](docs/extensions.md)。

## 文档

| 文档 | 讲什么 |
| --- | --- |
| [文档首页](docs/README.md) | 阅读路线、版本状态、各页入口。 |
| [配置手册](docs/configuration.md) | 配置优先级、项目记忆、providers、hooks、MCP、搜索、LSP、models.json。 |
| [项目记忆设计](docs/memory-system-design.md) | 目录、召回、后台更新、安全边界与后续路数。 |
| [扩展指南](docs/extensions.md) | Skills、Lua、C ABI 插件、MCP 与 LSP。 |
| [架构说明](docs/architecture.md) | 分层、请求链、双后端、工具与平台边界。 |
| [终端交互](docs/terminal-ui.md) | 工作动画、消息队列、`ask_user`、确认与编辑匹配。 |
| [项目指令](docs/project-instructions.md) | `/init`、`AGENTS.md` 层级、覆盖与大小边界。 |
| [提示词模块](src/prompts/README.md) | 内置 prompt 如何拆分、嵌入与覆盖。 |

## CI 与发布

每次 push 与 pull request 都会在下列环境编译、测试：

- Windows + MSVC
- Ubuntu + GCC
- macOS + Clang

推送 `v*` 标签会触发发布流水线。三平台分别打包，随后自动创建 GitHub Release、生成发布说明并上传产物：

```bash
git tag -a v0.23.5 -m "v0.23.5"
git push origin v0.23.5
```

## 许可

仓库尚未附开源许可证。在 `LICENSE` 落地之前，代码版权仍归作者保留。
