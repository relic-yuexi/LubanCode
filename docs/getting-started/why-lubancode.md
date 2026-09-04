# 为什么是 LubanCode

[文档首页](../README.md) · [三分钟上手](quickstart.md) · [功能全览](../reference/feature-index.md) · [架构说明](../architecture/README.md)

多数代码 Agent 都会读文件、改代码、跑命令。LubanCode 更关心任务中途出了岔子怎么办。换过模型，拉长任务，甚至杀掉进程，还能否从本地记录里还原刚才做过什么？

## 1. 它到底是什么

LubanCode 是本地 Agent Harness，也带一套终端界面。模型端点由用户配置，运行时不押某一家 Provider。

```text
四种模型协议
    -> Agent loop
        -> 权限与工具运行时
            -> CLI / Workflow / app-server / Gateway 骨架
                -> session / usage / artifact / trajectory
```

模型可走 Anthropic Messages、OpenAI Responses、OpenAI-compatible Chat Completions 或 Gemini Generate Content。前端可用终端，也可接 app-server。无论从哪一头进来，工具都会校验 schema，写文件与跑命令都要过权限，取消信号也会传到正在运行的工具。运行事实最后写进同一种轨迹格式。

CLI 处理输入与显示。Harness 还要管模型怎样调用工具，如何恢复、核验和导出一场运行，以及别的前端怎样接进来。

## 2. “轻量”指什么

这里的“轻”不指功能少，也不靠一句“C++ 更快”撑着。它指安装和闲时负担。

- Release 不要求 Node.js 或 Python。包内另带 `rg`，文件搜索不用临时下载工具。
- LSP 按需启动，闲置到期便退出。
- MCP 与插件工具太多时，先留索引；模型用 `tool_search` 找到后再挂载。
- 普通 CLI 不暗起 Gateway、渠道 sidecar 或监听端口。这些进程只认显式入口。
- 管道与重定向不用复杂终端重画，直接输出 plain 文本。

这些都有源码与运行合同可查。启动耗时、峰值内存、安装包大小会随版本和构建方式变化。没有同机基准，就不报“快多少”“省多少”。

## 3. 为什么用 C++23

### 原生交付

用户拿到的是原生程序，不必先备 Node.js 或 Python。原生编译、随包资源、平台安装脚本，把运行环境压成一只可交付物。

Codex CLI 用 Rust，也能交原生二进制。Pi 如今也能打 Bun standalone binary。原生交付并非 LubanCode 独有，选 C++ 还要看终端与进程怎样实现。

### 终端和进程就在核心里

Agent Harness 常年守着几样难缠东西：HTTP 流、终端输入、后台命令、子进程树、信号、超时、取消、文件原子替换。它们一旦各跑各的，便会留下半截状态：界面说停了，孙进程还活着；回复出来了，账没写完；文件换了一半，旧件先没了。

LubanCode 把这层写在 C++ 里，直接握住系统接口：

| 边界 | Windows | Linux / macOS |
| --- | --- | --- |
| 起进程 | `CreateProcessW` | `fork/exec` |
| 收进程树 | Job Object | 进程组与信号 |
| 路径 | UTF-16 系统调用 | UTF-8 / 原生路径 |
| 终端 | Win32 控制台能力探测 | POSIX terminal / `poll` |

平台层收好差异，上层只接统一结果。LubanCode 因此用 C++ 写宿主层。

### 直接接入原生库

LSP、MCP 与进程插件都能跨语言。要贴近现成 C/C++ 库时，C ABI 插件还能直接进宿主。少绕一层 RPC，少养一只 sidecar。这个入口三平台都认——Windows `.dll`、Linux `.so`、macOS `.dylib`；不可信代码仍该走进程外 MCP，不能拿“原生”二字遮掉隔离问题。

### C++ 的成本

- 编译比脚本语言慢，改一处扩展也未必能即刻热载。
- 平台 API 各有脾气，Windows 与 POSIX 两路都要测。
- C++ 不替你兜内存安全。所有权、边界检查、模糊输入与回归测试一项也省不得。
- 想写小工具，Lua、MCP 或进程插件往往比改核心更合算。

仓库没有拿四款 CLI 做同机启动、内存或吞吐基准，本文也不宣称 C++ 一定更快。LubanCode 借它直接管理系统资源，并把几种进程形态放进同一套宿主代码。相应成本落在编译、平台适配与内存安全上。

## 4. 和三款工具放在一起看

下表取自 2026-09-04 的公开产品页与主仓库。这里只列较稳的设计差异，不数命令，也不排高下。

| 维度 | Claude Code | Codex CLI | Pi | LubanCode |
| --- | --- | --- | --- | --- |
| 根 | Claude 产品与 Agent 体验 | OpenAI Codex 的本地执行面 | 可塑的最小 Agent Harness | Provider 中立的原生执行与事实账 |
| 主要宿主 | 官方原生安装；覆盖 CLI、IDE、桌面、Web | Rust CLI；另接 IDE、桌面与云端 | TypeScript monorepo；npm 与 standalone binary | C++23 原生程序；CLI 与 app-server |
| 模型路线 | Claude 为中心，也公开支持部分第三方 Provider | OpenAI / ChatGPT 为中心 | 统一多 Provider API | 四协议、68 家预设、自定义兼容端 |
| 扩展重心 | Skills、hooks、MCP、Agent SDK、产品集成 | Skills、MCP、配置、SDK 与 OpenAI 平台 | TypeScript extensions、skills、prompts、themes、packages | Skills、Workflow、MCP、LSP、Lua、进程与 C ABI 插件 |
| 界面语言 | 公开 CLI 参考未列切换项 | 官方配置参考未列 `locale` / `language` | 公开 TUI 文档未列内置 i18n | 跟随系统；内置中英文；`/language` 即时切换；JSON 语言包可扩展 |
| 安全取向 | 产品内权限、企业策略与沙箱能力 | OS 原生 sandbox + approval policy | 默认承接启动用户权限；官方建议另加容器或沙箱 | 五档审批、项目 allow/deny；尚无通用强化沙箱 |
| 长任务与多端 | 云端、远程、桌面、Web、团队面很齐 | 本地、IDE、桌面、云端同属 Codex 体系 | 核心小，靠扩展拼形态 | 本地 session、Workflow、轨迹、app-server；Gateway 尚在分批落地 |
| 最值得选它时 | 你要 Claude 全家桶 | 你要 OpenAI 全家桶与强沙箱 | 你要最快改造 Harness | 你要换端点、查执行、做原生集成或 Harness 实验 |

### 对 Claude Code

Claude Code 覆盖终端、IDE、桌面、Web、远程、团队与 Agent SDK。已经使用 Claude 订阅，又需要跨端协同，直接用这套产品最顺。

LubanCode 没有 Claude 订阅登录，也没有同等规模的云端产品面。它允许用户自配模型协议与端点，能逐字段查配置来源，也给自建 vLLM 备了探针。工具、usage、trajectory 都能沿本地源码与记录往下追。

### 对 Codex CLI

Codex CLI 是原生开源终端 Agent，带 OS sandbox 与 approval policy。它还接入 OpenAI 的桌面、IDE 与云端产品。

LubanCode 支持四种模型协议，把兼容端、Workflow、轨迹导出、Lua/C ABI 与本地配置诊断写进自身运行时。当前没有一副能与 Codex OS sandbox 相比的强化沙箱。LubanCode 的审批只决定一项操作是否放行，不能隔离已经启动的进程。

### 对 Pi

Pi 和 LubanCode 都公开 Harness，也都支持多 Provider、TUI、Skills 与 packages。

Pi 保留一颗小核心，把 UI、工具和事件钩子交给 TypeScript extension。改这些东西，通常比重编 C++ 快。它的官方仓库也写明：默认没有内置权限隔离，需靠容器或沙箱补边界。

LubanCode 在宿主里实现进程治理、五档审批、LSP、强类型 Workflow、app-server、usage 与 trajectory。核心比 Pi 重，运行入口与本地记录则共用一套结构。

### 界面语言这件小事

模型能用中文回答，不等于 CLI 有中文界面。菜单、帮助、审批、状态栏、报错，都得程序自己翻。LubanCode 会认系统语言，也能用 `/language` 当场切换。内置表不够，还可往 `languages/*.json` 添语言包；没翻到的键回退中文，坏语言包只告警，不拦启动。

截至 2026-09-04，本次查阅 [Claude Code CLI reference](https://code.claude.com/docs/en/cli-reference)、[Codex CLI configuration reference](https://learn.chatgpt.com/docs/config-file/config-reference)与 [Pi TUI 文档](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/tui.md)，都未找到界面语言开关。这里记的是公开文档现状，不把“未找到”写成“绝不支持”。LubanCode 自己的覆盖边界见[界面多语言](../development/i18n.md)：中文为全量表，英文覆盖核心键；系统提示、工具描述与模型回复不走这套 i18n。

## 5. 什么时候该选 LubanCode

这些需求适合 LubanCode：

- 你手里不止一家模型，常接兼容端或本地 vLLM。
- 你要研究 Agent loop、上下文、工具调度、轨迹或评测，还要查看最终 diff 以外的运行记录。
- 你在 Windows 原生环境干活，关心进程树、UTF-16 路径与终端细节。
- 你想把同一副运行时接进 CLI、Workflow 或自己的前端。
- 你要一套能读源码、能改协议、能接 C/C++ 库的 Harness。

另几种情形，别绕远路：

- 团队全用 Claude，最看重云端与 IDE 协同：先用 Claude Code。
- 已买 ChatGPT，想要 OpenAI 沙箱与全套 Codex 体验：先用 Codex CLI。
- 只想用 TypeScript 飞快改一副小 Harness：先看 Pi。
- 只要补全代码，不想让 Agent 跑命令、改仓库：编辑器内补全工具更省事。

## 6. 资料与口径

- [Claude Code 官方概览](https://code.claude.com/docs/en/overview)：产品面、第三方 Provider、Skills、hooks、MCP、多 Agent 与远程能力。
- [Claude Code CLI reference](https://code.claude.com/docs/en/cli-reference)：当前命令与参数表；本次查阅未见界面语言项。
- [Codex CLI 官方文档](https://learn.chatgpt.com/docs/codex/cli)：本地仓库、权限、模型与评审入口。
- [Codex CLI configuration reference](https://learn.chatgpt.com/docs/config-file/config-reference)：当前配置项；本次查阅未见 `locale` 或界面 `language` 项。
- [Codex 开源仓库](https://github.com/openai/codex)：Rust CLI、原生发行包与 sandbox 入口。
- [Pi 官方仓库](https://github.com/earendil-works/pi)：多 Provider 包、TypeScript/Bun 交付、权限与容器边界。
- [Pi TUI 文档](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/tui.md)：当前终端界面说明；本次查阅未见内置 i18n 设置。
- [LubanCode 功能全览](../reference/feature-index.md)：当前实现入口与边界。
- [LubanCode 架构说明](../architecture/README.md)：源码分层、请求链、工具与平台层。

外部工具会继续变。若表格与一手资料冲突，以链接中的当前官方说明为准；LubanCode 自己的现状，则以程序输出、源码和测试为准。
