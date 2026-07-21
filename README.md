# lubancode

一个用 C++ 写的 AI 编程 CLI。双协议接多家模型服务,读写编辑、命令执行、搜索、子代理、技能、MCP、LSP、钩子一应俱全,配一副真控制台交互的皮囊——流式渲染、diff 预览、确认档、逐键编辑,不比图形界面差。

当前版本 v0.23.0。

## 特性

### 多模型

- **双协议**:Anthropic Messages API(`wire=anthropic`)与 OpenAI Responses API(`wire=responses`)都能说,底层实现分目录隔离,互不干扰。
- **多 provider**:`/provider add|list|switch|remove|set` 一族命令管理多个模型服务端,配置落盘持久,一行切换。
- **万能参数口**:`extra_body`/`extra_headers` 每个 provider 各自可配,模型服务商的私有开关(思考模式、分级推理强度……)不用等 lubancode 内置支持,自己往请求上加。
- **推理强度**:`/think`(`/effort` 同义)切 `none`/`low`/`medium`/`high` 等档位,Anthropic 协议映射 `thinking`,Responses 协议映射 `reasoning.effort`。
- **模型目录**:主目录放一份 `models.json`,给每个模型定制默认推理档、上下文窗口、专属指令,借鉴 Codex 的 model-catalog 思路。
- **原生联网搜索**:支持服务端自带 web_search 的模型,按 provider 开关声明,两种协议各自翻译。

### 工具全家桶

- 文件读写编辑(`read_file`/`write_file`/`edit_file`),带 diff 预览确认。
- 命令执行(`run_command`),支持 `run_in_background` 后台起长命进程、跨命令存活。
- 网络搜索与抓取(`web_search`/`web_fetch`),搜索需配置 tavily/brave/serper 之一。
- 子代理(`agent` 工具):派生独立上下文的子任务,主对话上方常驻进度条。
- 技能(`/skill` 联网分发):`install`/`list`/`update`/`remove`,远端技能装进本地,项目级技能可覆盖全局同名技能。
- MCP:配置 stdio 服务器,工具自动挂载,`/mcp` 看状态。
- LSP:配置语言服务器(如 clangd),提供 definition/references/symbols/diagnostics 语义查询,懒启动、闲置自动关停。
- 钩子(hooks):`pre_tool`/`post_tool`/`session_start`/`session_end` 四类外部命令钩子。
- 插件:C ABI DLL 与 Lua 两条路自行扩展工具,详见下方插件说明。

### 交互体验

- 真终端交互:流式吐字、markdown 渲染(标题/列表/表格/代码块)、LaTeX 公式转 Unicode。
- 工具调用前 diff 预览 + 逐条确认,三档确认模式(`confirm`/`auto`/`yolo`)Shift+Tab 循环切换。
- 输入框逐键编辑:历史翻页、Tab 补全 slash 命令、宽字符光标定位、多行输入(Shift+Enter)。
- 紧凑/详细双态折叠(Ctrl+O 切换)、聚焦查看全文(Ctrl+E)、ESC 打断当前轮。
- 会话管理:`/sessions`/`/resume`/`/export`/`/title`,`--continue` 自动续上次会话。
- 隔离工作树:`/worktree new|list|exit`,独立分支干活不脏主树。
- 上下文管理:`/context` 分类占用分析(系统提示/工具定义/历史明细 + 条形图),超 80% 自动 `/compact` 压缩,可指定 `compact_model` 单独跑压缩。

### 可扩展 / 可定制

- **soul(魂)**:风格叠加层,`/soul` 切换或直接写内容,注入系统提示末尾,只改语气不改工具调用能力。内置文言文示例魂。
- **system_prompt(法)**:`--system-prompt` 或配置文件替换人格段,环境段(工作目录、工具调用规矩)原样保留。
- **i18n 双语**:界面文案中英内置,`languages/*.json` 可扩展第三种语言;`/language` 会话内切换。
- **主题**:`dark`/`light`/`plain` 三套终端配色,管道/重定向自动降级纯文本。

## 安装

三条路,任选其一。

### 方式一:下载 Release 包

前往 [Releases](https://github.com/OWNER/lubancode/releases) <!-- TODO: 仓库推上 GitHub 后替换 OWNER --> 下载对应平台的压缩包:

- Windows:`lubancode-vX.Y.Z-windows-x64.zip`(内含 exe、README、`install.ps1`、`uninstall.ps1`)
- Linux:`lubancode-vX.Y.Z-linux-x64.tar.gz`(内含 exe、README、`install.sh`)
- macOS:`lubancode-vX.Y.Z-macos-arm64.tar.gz`(内含 exe、README、`install.sh`)

**Windows**:解压后,右键 `install.ps1` → "使用 PowerShell 运行";或在终端里执行:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1
```

装到 `%LOCALAPPDATA%\Programs\lubancode`,自动加进用户 PATH,不需要管理员权限。卸载执行 `uninstall.ps1`,原样反向清干净。

**Linux / macOS**:

```bash
./install.sh
```

优先装到 `~/.local/bin`(已在 PATH 里就直接用);没有就装 `/usr/local/bin`,会提示 `sudo`。

### 方式二:Windows 一行式

仓库推上 GitHub 后可用(URL 里的 `OWNER` 是占位符,替换成实际仓库所有者):

```powershell
irm https://raw.githubusercontent.com/OWNER/lubancode/main/scripts/install.ps1 | iex
```

### 方式三:源码构建

依赖:CMake ≥ 3.21,支持 C++23 的编译器(MSVC 19.44+ / g++ 13+ / clang 15+),[cpr](https://github.com/libcpr/cpr) 与 [nlohmann-json](https://github.com/nlohmann/json)——两个都会自动拉取(vcpkg manifest 优先,探测不到就 `FetchContent` 拉源码构建),不用手装。

Windows(用 CMakePresets):

```bash
cmake --preset release
cmake --build --preset release
./build/release/Release/lubancode.exe --version
```

Linux / macOS:

```bash
sudo apt-get install -y build-essential cmake ninja-build libssl-dev   # Debian/Ubuntu 示例
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
./build/lubancode --version
```

跨平台细节(平台抽象层、POSIX 实现现状、CI 矩阵)见下方"CI 与发布现状"一节。

## 快速上手

第一次运行,不带参数直接敲 `lubancode`,没配过 `base_url`/`api_key`/`model` 会自动进初次配置向导:

```
$ lubancode
=== lubancode 初次配置向导 ===
界面语言 / Language: 1) 中文  2) English
接口格式: 1) anthropic (Claude 系)  2) responses (OpenAI 系)
base_url: https://api.minimaxi.com/anthropic
api_key: sk-...
model: (回车拉列表选,或直接输入模型名)

保存到 ~/.lubancode/config.json? [Y/n]:
lubancode 0.23.0  [anthropic] MiniMax-M3
> 
```

也可以跳过向导,直接单发一句:

```bash
lubancode "帮我看看这个项目的目录结构"
```

或者管道喂问题:

```bash
echo "这段代码有什么问题" | lubancode
```

### 常用 slash 命令

交互模式下,`/` 开头的一行走命令,不发给模型:

| 命令 | 作用 |
| --- | --- |
| `/help` | 列出所有命令 |
| `/model` / `/model 名字` | 拉模型列表选,或直接切到指定模型名 |
| `/provider` | 列、添、切、删、改模型服务端(`add`/`list`/`switch`/`remove`/`set`) |
| `/config` | 打印当前生效配置(密钥打码)和本会话在用的模型 |
| `/context` / `/context 512k` | 看上下文占用分析,或临时改窗口大小 |
| `/compact [重点说明]` | 手动压缩历史,超 80% 占用会自动触发 |
| `/think 档位` / `/effort 档位` | 切推理强度,档位以服务商为准 |
| `/soul` | 看/改/切风格叠加层(魂) |
| `/prompt` | 看当前系统提示词(法)来源;`/prompt reset` 还原默认 |
| `/skills` / `/skill install <url>` | 列已装技能 / 联网安装远端技能 |
| `/mcp` / `/lsp` / `/plugins` | 看 MCP 服务器 / LSP 服务器 / 插件工具挂载状态 |
| `/tools` | 列工具三态:核心 / 已加载 / 延迟未加载 |
| `/sessions` / `/resume <编号>` / `/export` | 列会话存档 / 续聊 / 导出 Markdown |
| `/worktree new\|list\|exit` | 新建/列出/退出隔离工作树 |
| `/language` | 列可选界面语言并切换 |
| `/image 路径` | 附本地图片(或消息里写 `@路径`) |
| `/clear` | 清空对话历史 |
| `/exit` | 退出(裸词 `exit`/`quit` 也认) |

Shift+Tab 循环切换确认档(`confirm`/`auto`/`yolo`);Ctrl+O 切紧凑/详细;Ctrl+E 聚焦查看全文;ESC 打断当前轮。完整命令表与快捷键说明,交互模式里敲 `/help` 或跑 `lubancode --help` 看最新版。

## 配置速览

配置来源按字段分五级合并,优先级从高到低:`LUBANCODE_*` 专属环境变量 → 项目级 `.lubancode/config.json` → 全局(主目录)`.lubancode/config.json` → 通用环境变量(`ANTHROPIC_*`/`OPENAI_*`,向后兼容)→ 内置默认值。

字段表、`LUBANCODE_*` 环境变量表、provider 实战示例(MiniMax、GLM 思考参数)、hooks/mcpServers/search/lsp 各段写法,见 [docs/configuration.md](docs/configuration.md)。

架构说明(分层依赖、api 双后端设计、工具层)见 [docs/architecture.md](docs/architecture.md)。

## CI 与发布现状

<!-- 仓库尚未推上 GitHub,下面两个徽章的 OWNER/REPO 是占位符,推送后替换成实际路径 -->
[![CI](https://github.com/OWNER/lubancode/actions/workflows/ci.yml/badge.svg)](https://github.com/OWNER/lubancode/actions/workflows/ci.yml)
[![Release](https://github.com/OWNER/lubancode/actions/workflows/release.yml/badge.svg)](https://github.com/OWNER/lubancode/actions/workflows/release.yml)

CI 矩阵三平台:`windows-latest`(MSVC,主平台,必须绿)、`ubuntu-latest`(g++)、`macos-latest`(clang)。POSIX 两腿 `continue-on-error`,观察中(Linux 已在 WSL 真机验证,macOS 尚未真机验证)。推 `v*` tag 触发 release 工作流,三平台各打一个包挂上 GitHub Release。

## 许可

仓库目前未附许可证文件。正式对外发布前需要补一份 `LICENSE`。
