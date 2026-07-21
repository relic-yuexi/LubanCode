# lubancode 架构说明

## 一、分层与依赖方向

lubancode 分层,依赖只许单向,从上往下:

```
cli  →  agent  →  api / tools / mcp / lsp
                          ↑
                       config / platform / prompts(基础设施,各层可用)
```

- **cli**(`src/cli/`):命令行入口,解析参数、渲染输出(流式、markdown、diff、slash 命令、输入编辑器、i18n),只管人机交互,不碰业务逻辑。
- **agent**(`src/agent/`):核心循环。接收用户输入,调用 api 层与模型对话,按需调用 tools 层执行动作,再把结果喂回模型,如此往复;另外管上下文压缩(`compact`)、会话存档(`session_store`)、系统提示拼装(`prompt_assembler`)。
- **api**(`src/api/`):与大模型对话的通路。只认得"发消息、收流式事件"这一件事,不关心 agent 怎么用它。
- **tools**(`src/tools/`):模型可调用的具体能力(读文件、跑命令、搜索、子代理、技能、插件……),只管把一件事做好,不关心是谁在调它。
- **mcp**(`src/mcp/`):MCP stdio 客户端与传输层,给 tools 层的 MCP 工具适配器用。
- **lsp**(`src/lsp/`):语言服务器客户端、传输层与懒启动管理,给 tools 层的 `lsp` 工具用。
- **config**(`src/config/`):配置分层加载与合并、内置手册文本、prompt 脚手架文件。
- **platform**(`src/platform/`):进程、终端、路径的平台抽象,Windows 与 POSIX 各一份实现。
- **prompts**(`src/prompts/`):内置系统提示词的模块化源文本(`core`/`features`/`platforms`/`models`),编译期嵌入,首启播种到 `~/.lubancode/prompts/`。

上层认得下层,下层不认得上层。tools 不知道 agent 存在,api 不知道 cli 存在。这样改起来才不会牵一发动全身。

## 二、api 层:双后端设计

lubancode 要同时说两种"方言":Anthropic 的 Messages API,和 OpenAI 的 Responses API。早期开发主要拿 MiniMax 的兼容端点验证(参见仓库根目录 `Anthropic兼容-Messages.md`、`OpenAI兼容-Responses.md` 等文档),但协议本身与厂商无关——`config` 层的 `providers` 数组可以配任意多家服务端,各自选 `wire: anthropic` 或 `wire: responses`,`/provider switch` 切换。两种协议语义各不相同,不能混着写。

### 1. 中立类型 + 抽象接口

api 层对上只暴露一套与厂商无关的中立类型:

- `Message`——一轮对话里的一条消息(角色 + 内容块)。
- `ContentBlock`——文本、工具调用、工具结果等内容的载体。
- `ToolCall`——模型发起的一次工具调用请求。
- `StreamEvent`——流式响应中的一个事件(增量文本、工具调用片段、结束标记……)。

再加一个 `backend.hpp`,定义抽象接口(大致是"发一轮消息,拿到一串 StreamEvent"),agent 层只认这个接口,不关心背后是 Anthropic 还是 Responses 在干活。

### 2. 两层分开:分帧 vs 语义

流式响应的处理拆成两层,不要揉在一起:

- **SSE 分帧(通用)**:负责把 HTTP 响应体按 `data:` / `event:` 这类前缀切成一行一行的原始帧,这一层与厂商无关,谁都能复用。
- **事件语义(各后端各写)**:拿到一帧原始数据之后,怎么解析成 `StreamEvent`,每家协议字段不同,各自实现,互不干扰。

### 3. 目录划分

- `api/anthropic/`——对接 Anthropic Messages API 的实现。
- `api/responses/`——对接 OpenAI Responses API 的实现。

两边各自把厂商私有的 JSON 结构翻译成中立类型,翻译完的东西对 agent 层长得一模一样。

## 三、工具层

- `Tool` 基类:约定 `name`(工具名)、`schema`(参数 JSON Schema)、`execute`(执行逻辑)、`needs_confirm`(是否要先问用户)四件事。
- `registry`:工具注册表,agent 启动时把所有工具注册进去,按名字查找、按 schema 交给模型。
- 一个工具一个文件,新增能力只管加文件、注册,不用动别处。

## 四、错误处理

- 正常的、预期内的失败(网络错、参数错、工具执行失败……)一律用 `std::expected` 往上传,调用方自己判断、自己处理。
- 异常只留给"程序本身写错了"这种事(断言失败、不可能到达的分支),不拿异常当业务错误的传话筒。

## 五、里程碑(历史)

早期按里程碑推进,M0~M5 早已走完,此后不再逐一编号,按功能分支合并、定版打 tag(见 `git log`)。留一笔存档:

- **M0**——骨架搭起来:能编译、能跑、`--version` 有输出,依赖能拉、能链接。
- **M1**——打通 Anthropic 通路,能流式问答。
- **M2**——agent 循环跑起来,补上 `read_file`、`run_command` 两个工具。
- **M3**——打通 Responses 后端,双通路都能用。
- **M4**——工具补齐,加上执行前的权限确认。
- **M5**——像样的 TUI(多行 composer、条目化 transcript、diff 预览)。
- **M6~M9 及之后**——`~/.lubancode/` 安家、上下文管理(`/context`/`/compact`/`/think`)、终端体验(主题/转轮/token 统计)、MCP、插件(DLL+Lua)、websearch/webfetch、模型目录、会话存档、LSP、魂法分家(soul/system_prompt)、i18n、跨平台(POSIX 实现)、多 provider(`/provider` 族)、`extra_body`/`extra_headers` 万能参数口、后台命令执行(`run_in_background`)。

当前版本 v0.23.0 是功能完整的 AI 编程 CLI,不再是"骨架阶段"。最新特性脉络看 `git log --oneline`。
