<p align="center">
  <img src="docs/assets/lubancode-banner.png" alt="LubanCode" width="100%">
</p>

<h1 align="center">LubanCode</h1>

<p align="center"><strong>一把装在终端里的 AI 编程工具。C++23 写成，三协议接入，能读代码，也能动手干活。</strong></p>

<p align="center">
  <strong>简体中文</strong> · <a href="README.en.md">English</a>
</p>

<p align="center">
  <a href="https://github.com/relic-yuexi/LubanCode/actions/workflows/ci.yml?query=branch%3Amain+event%3Apush"><img src="https://github.com/relic-yuexi/LubanCode/actions/workflows/ci.yml/badge.svg?branch=main&event=push" alt="main 分支 CI"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white" alt="C++23">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-3D6DCC" alt="Apache-2.0"></a>
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-444444" alt="Windows, Linux and macOS">
</p>

LubanCode 原生支持 Anthropic Messages、OpenAI Responses 与 Chat Completions 兼容接口。模型能读文件、改代码、跑命令、查资料，也能调度子代理、MCP 与 LSP。75 家 Provider 预设装在目录里，搜到便可配置。流式正文、Markdown、diff、代理坞、消息队列、会话存档，也都在终端里铺开。

鲁班造物，先正绳墨，再下斧凿。LubanCode 也守这条规矩：先看清，再动手；改了什么，明明白白摆给你看。

> 源码版本只认 [`src/app/version.hpp`](src/app/version.hpp)。主分支代码改动会在 Windows、Ubuntu、macOS 三路编译并跑全量测试；结果以页首 CI badge 为准。

## 真机界面

下面两张图由 Windows Release 二进制接本地测试 Provider 实机截取。没有设计稿，也没放真密钥。

<p align="center">
  <img src="docs/assets/screenshots/agent-queue.png" alt="LubanCode 子代理、忙时输入框、消息队列与状态栏" width="100%">
</p>
<p align="center"><sub>主代理工作时仍可输入下一条；子代理、队列与状态栏同屏。</sub></p>

<p align="center">
  <img src="docs/assets/screenshots/markdown-session.png" alt="LubanCode 在终端中渲染 Markdown 标题、表格与代码块" width="100%">
</p>
<p align="center"><sub>Markdown 标题、表格与代码块直接画在终端里。</sub></p>

## 为什么是 LubanCode

五件事，先摆在桌上。

### 1. 下载即用，不用另装运行时

不需要 Node.js。不需要 Python。不需要 `npm install`。Windows x64 Release 主程序约 8.2 MiB，一份原生二进制便能起跑。Windows 一行命令安装，Linux / macOS 解压即用。官方 skills、文档与许可证另随发行包一道带上。

### 2. 忙时也能接着输入

模型还在思考，你照样可以键入下一条。回车一按，消息进队列；当前回合收口，下一条自己接上。子代理另有一片导航坞，谁在跑、跑了多久、用了几件工具，一眼便知。终端不是日志瀑布，是一张能办事的桌子。

### 3. 魂/法分离的提示词系统

两套提示词各管一段：

- **法**（`~/.lubancode/system_prompt.md`）：行为骨架——工具调用、代码规范、工作流程的规矩。稳得住，不轻易动。
- **魂**（`~/.lubancode/SOUL.md`）：风格叠加层——"只用文言文答话""回答控制在三句话内"。盖在法之上，随叫随切。

`/soul` 一键切换，`~/.lubancode/souls/` 下放多个备选魂文件。改风格不必碰行为逻辑，改行为不必重写人格。别的工具系统提示是一整坨，LubanCode 把它拆开了。

### 4. 项目记忆

LubanCode 会记住你的项目事实与偏好——构建命令、代码风格、踩过的坑。`/memory` 管理召回，后台自动整理。

一个细节：同一个 Git 仓库的主工作树和 linked worktree（`/worktree` 新建的隔离工作树）按 common git dir 归到同一身份，**共享同一份记忆**。你在主分支教过的东西，切到 worktree 里不用重教。

### 5. 终端 Markdown 与 LaTeX 渲染

模型回答里的 Markdown 表格、代码块、列表、引用，LubanCode 在终端里原地渲染——不是糊一堆纯文本。LaTeX 公式按行内 `$...$` 和独立 `$$...$$` 渲染成终端可显示的形式。diff 有着色，工具输出有折叠，长粘贴有智能收纳。

终端不该是降级体验。

## 一眼看懂

| | 能力 |
| --- | --- |
| **模型接入** | Anthropic / Responses / Chat Completions 三协议；75 家 Provider 目录；多路连接随时切换并记住上次选择。 |
| **代码工具** | 读、写、容错编辑、搜索文件；前台或后台跑命令；改动先看 diff，再落盘。 |
| **语义与外接工具** | LSP 定义、引用、符号、诊断；MCP stdio；联网搜索与网页抓取。 |
| **代理工作流** | 子代理、三档角色模型、Plan 模式、待办清单、`ask_user`、`AGENTS.md`、隔离 worktree 与项目级权限。 |
| **终端体验** | 分段 Markdown 渲染、动态工作状态、常驻消息队列、智能粘贴折叠、逐键编辑、折叠与聚焦、三档确认。 |
| **上下文与存档** | token 占用分析、确定性裁面、可追回 artifact、按需局部摘要、会话恢复、标题、Markdown 导出。 |
| **扩展与定制** | Skills、Lua 工具、C ABI DLL 插件、hooks、主题、i18n、soul 与 system prompt。 |

## 安装

### Windows：一行安装

PowerShell 5.1 及以上可直接拉取最新 Release：

```powershell
irm https://raw.githubusercontent.com/relic-yuexi/LubanCode/main/scripts/install.ps1 | iex
```

程序会装到 `%LOCALAPPDATA%\Programs\lubancode`，并写入当前用户 PATH。官方 `skills/` 与 `docs/` 跟程序一同安装、一道升级。无需管理员权限。重复执行便是覆盖升级。

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

若 `~/.local/bin` 已在 PATH，脚本便装到那里；否则转去 `/usr/local/bin`，需要时会提示 `sudo`。官方 `skills/` 与 `docs/` 会成对落在同一 `share/lubancode/` 资源根下。

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

第一次运行，不带参数即可。还没配模型时，开局给两条路：当场添加 Provider，或先跳过、径直进主界面。跳过不会写一份假连接；稍后敲 `/provider` 或 `/provider add` 再配也成。

```text
$ lubancode
开始使用 lubancode

还没有可用的模型连接。
现在可以添加 Provider，也可以先进入主界面。

> 添加 Provider  - 从服务商目录选择，填好密钥后立即使用
  暂时跳过      - 稍后用 /provider 或 /provider add 配置
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

若你管着多家模型服务，直接敲 `/provider add`。75 家预设会自动进搜索页；键入厂家名，选中，再填 Key。地址、协议、默认模型和推理参数由目录带上。全手填仍留在最后一项。手写配置也照旧可用：

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

把它存到 `~/.lubancode/config.json`，再设好 `WORK_MODEL_API_KEY`。`/provider switch work` 成功后也会自动写入 `active_provider`，下次启动仍走这一路。完整字段、优先级与厂商参数透传，见 [配置手册](docs/reference/configuration.md)。

## 项目指令

进入仓库，敲一声 `/init`。LubanCode 会在 Git 根生成 `AGENTS.md`，填入项目布局、构建测试与改动规矩。已有文件便只读不改。文件写成后，当前会话立刻重载，主代理、子代理一并照办。

启动时，LubanCode 从 Git 根一路走到当前目录。每层先看 `AGENTS.override.md`，再看 `AGENTS.md`；近处内容排在后头，能压过远处。空文件跳过，总量封顶 32 KiB。细则见 [项目指令](docs/features/project-instructions/README.md)。

## 常用命令

| 命令 | 用处 |
| --- | --- |
| `/provider` | 从厂家目录添加、更新目录、列出、切换、删除模型服务。 |
| `/init` | 生成并载入项目级 `AGENTS.md`。 |
| `/model` · `/think` | 切模型与推理强度；`/model roles` 看三档路由，`/model <role> <id>` 直接改某一档。 |
| `/doctor` | 诊断本地兼容端：`effort` 发极小探针看档位实发值与 usage 拆账，`cache` 读服务端指标、对账固定前缀命中率。 |
| `/context` · `/compact` | 看上下文占用（最近一次主请求的占用，不含子代理累计），手工压缩历史。 |
| `/skills` · `/skill` | 使用 Agent Skills；扫描 `.agents/skills`，安装默认落 `~/.lubancode/skills`。 |
| `/mcp` · `/lsp` · `/plugins` | 看外接工具与语言服务器状态。 |
| `/tools` · `/todos` | 看工具挂载状态与待办清单。 |
| `/plan` | 进只读规划档，先查清、列计划，再审后执行。 |
| `/trace` | 翻工具生命周期账；文件改错后可由 `undo_file_edit` 按凭据撤回。 |
| `/goal` · `/loop` | 管持久目标与定时循环；两项均由 feature gate 控制。 |
| `/memory` | 管本场项目记忆、同步召回与后台写入。 |
| `/sessions` · `/resume` · `/export` | 全屏会话台账：搜索、筛选、排序、三种查看态，重放历史续聊或导出。 |
| `/archive` · `/delete` | 归档或永久删除当前会话；顶层另有 `lubancode archive/unarchive/delete <id>`。 |
| `/worktree new\|list\|exit` | 在隔离工作树里干活。 |
| `/soul` · `/prompt` | 调风格，或替换系统提示人格段。 |
| `/language` · `/image` | 切界面语言，附本地图片。 |
| `/help` | 在程序里看完整命令表。 |

几个键也常用：

- `Shift+Tab`：循环切换 `confirm`、`auto`、`yolo`。
- `Ctrl+O`：工具输出在紧凑与详细之间切换。
- `Ctrl+E`：聚焦查看当前工具条目全文。
- `Shift+Enter`：输入框里换行。
- 粘贴内容：1000 字符内直接显示；超过后折成 `[粘贴内容 N 字符]`，提交时展开原文。
- `Esc`：打断当前轮，或退出聚焦画面。

模型作答时可直接键入下一条并回车。消息会留在输入框上方，当前回合收尾后依次发送。

## 扩展

LubanCode 留了四扇门：

1. **Skills**：一份标准 `SKILL.md`；可放 `.agents/skills` 跨客户端共享，也可放 `.lubancode/skills`。
2. **MCP / LSP**：在配置里挂 stdio 服务与语言服务器。
3. **Lua 插件**：一个 `.lua` 文件就是一件工具，适合轻量扩展。
4. **C ABI 插件**：Windows DLL 同进程加载，适合原生能力与已有 C/C++ 库。

写法、目录、示例与安全边界，见 [扩展指南](docs/features/extensions/README.md)。

## 文档

| 文档 | 讲什么 |
| --- | --- |
| [文档首页](docs/README.md) | 阅读路线、版本状态、各页入口。 |
| [配置手册](docs/reference/configuration.md) | 配置优先级、项目记忆、providers、hooks、MCP、搜索、LSP、models.json。 |
| [Provider 目录](docs/features/providers/catalog.md) | 常见厂家预设、在线更新、缓存、Schema 与安全边界。 |
| [项目记忆设计](docs/architecture/memory/design.md) | 目录、召回、后台更新、安全边界与后续路数。 |
| [扩展指南](docs/features/extensions/README.md) | Skills、Lua、C ABI 插件、MCP 与 LSP。 |
| [架构说明](docs/architecture/README.md) | 分层、请求链、双后端、工具与平台边界。 |
| [终端交互](docs/features/terminal/README.md) | 工作动画、消息队列、`ask_user`、确认与编辑匹配。 |
| [项目指令](docs/features/project-instructions/README.md) | `/init`、`AGENTS.md` 层级、覆盖与大小边界。 |
| [提示词模块](src/prompts/README.md) | 内置 prompt 如何拆分、嵌入与覆盖。 |

## CI 与发布

代码改动 push 或提交 pull request 后，会在下列环境编译、测试：

- Windows + MSVC
- Ubuntu + GCC
- macOS + Clang

推送 `v*` 标签会触发发布流水线。三平台分别打包，随后自动创建 GitHub Release、生成发布说明并上传产物：

```bash
version=vX.Y.Z
git tag -a "$version" -m "$version"
git push origin "$version"
```

## 许可

LubanCode 采用 [Apache License 2.0](LICENSE)。你可以使用、修改与分发代码，也可以把它用于商业项目；分发时须遵守许可证中的署名、改动说明与专利条款。
