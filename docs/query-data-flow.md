# 一条 Query 怎样流过 LubanCode

这篇只追一条输入：

```text
帮我看一下当前项目
```

读者只须熟悉 OpenAI Chat Completions：`messages`、`system`、`assistant`、`tool_calls`、`role=tool`。下文拿它作底稿，再看 Anthropic Messages 与 OpenAI Responses 怎样换一身衣裳。

先把结论说破：**底座仍是多轮对话。**

可它不是“一次回车，只发一次 HTTP”。用户看见一轮问答，`AgentLoop` 里头常转好几圈：

```text
一轮人机对话
= 1 条真人 user 消息
+ N 次模型请求（N >= 1）
+ 0 到若干次本地工具执行
+ 1 条最终 assistant 正文
```

模型若先要读文件，LubanCode 便执行工具，把结果添进历史，立刻再问模型。这个过程不等用户再按回车。故而，**界面是一轮，协议上却可能已经走了两三轮 assistant/tool 来回。**

本文按当前源码写。JSON 里的字段、角色与拼接次序都是真实现；模型选哪个工具、流式分成几片，却没有定数。下文拿 `read_file("README.md")` 模拟一条常见支路，免得把随机输出冒充真实抓包。

## 先认三层数据

LubanCode 不让主循环直接拼三家 JSON。中间横着一层自家的数据结构：

| 层 | 手里拿什么 | 谁来管 |
| --- | --- | --- |
| 会话层 | `api::Message`、`TextBlock`、`ToolUseBlock`、`ToolResultBlock` | `AgentLoop` |
| 协议层 | Chat `messages`、Responses `input`、Anthropic `content` | 三家 adapter |
| 界面层 | 文本增量、思考增量、工具状态、统计行 | `RunTurn` 与 TUI |

会话层只认两种角色：`User` 与 `Assistant`。工具结果在这层仍是一条 `User` 消息，只是内容块不是文字，而是 `ToolResultBlock`。等到协议层，Chat adapter 才把它翻成 `role: "tool"`。

这层中立结构很要紧。主循环只写一遍；换 provider 时，只换最外头的翻译法。

## 全程鸟瞰

```mermaid
sequenceDiagram
    actor User as 用户
    participant UI as Composer / RunTurn
    participant Loop as AgentLoop
    participant Wire as 协议 Adapter
    participant API as 模型 API
    participant Tools as ToolRegistry
    participant Store as SessionStore

    User->>UI: 帮我看一下当前项目
    UI->>Loop: api::Message(User, TextBlock)
    Loop->>Loop: 追加到 history
    Loop->>Loop: 拼 system、裁 history、列 tools
    Loop->>Wire: 中立 api::Request
    Wire->>API: 第 1 次 HTTP + SSE
    API-->>Wire: tool call 增量
    Wire-->>Loop: ToolUseStart / InputDelta / MessageDone
    Loop->>Loop: 拼成 Assistant + ToolUseBlock
    Loop->>Tools: read_file({path: README.md})
    Tools-->>Loop: 文件正文
    Loop->>Loop: 追加 User + ToolResultBlock
    Loop->>Wire: 中立 api::Request（带完整新历史）
    Wire->>API: 第 2 次 HTTP + SSE
    API-->>Wire: 最终正文增量
    Wire-->>Loop: TextDelta / MessageDone
    Loop->>Loop: 追加最终 Assistant
    Loop-->>UI: Run 返回
    UI->>Store: 把本轮新增 history 逐条写进 JSONL
```

上图有两处容易看岔：

- 每次请求都带一份完整 `system`、当前 `history` 与可用 `tools`。不是只把“新增那一句”发过去。
- 工具结果写进历史以后，第二次请求才发出。模型服务并不知道本机刚做过什么，全靠这条结果告诉它。

## 第 0 步：输入先变成中立消息

交互循环读到正文，先查 slash 命令。`帮我看一下当前项目` 不是 slash 命令，于是走普通消息支路。

这一轮还会先做几件事：

1. 上下文超过阈值时，先自动压缩。
2. 若开了项目记忆，拿 query 检索一次，把命中内容做成本轮 system 后缀。
3. `RunTurn` 处理图片附件、后台子代理完成通知、ESC 与排队输入。
4. 最后才调用 `AgentLoop::Run(message)`。

纯文本输入在中立层长这样：

```cpp
api::Message{
    .role = api::Role::User,
    .content = {
        api::TextBlock{"帮我看一下当前项目"}
    }
}
```

`AgentLoop` 先把它压进 `history_`。若是新会话，此刻历史只有一条：

```text
history_[0]
└─ User
   └─ TextBlock("帮我看一下当前项目")
```

## 第 1 步：system 不是一块死文本

请求里的 `system` 是层层接起来的。当前主会话按这一次序铺开：

```text
人格（法；没自定义便用 src/prompts/core/*）

# 运行环境
- 工作目录: D:\lubancode
- 今天日期: 2026-08-14
- 操作系统: Windows
凡是能动手做的事……优先调用工具……

项目指令（AGENTS.md 分层加载结果）

features/files.md
features/shell.md
features/delegation.md
features/todo.md

features/skills.md + 当前技能清单          （有技能才添）
features/web.md                           （开了 web 才添）
features/mcp.md                           （配了 MCP 才添）
features/lsp.md                           （配了 LSP 才添）

platforms/<当前 wire>.md

# 项目记忆 + 本轮召回内容                 （本轮开了记忆才添）

轮数将尽提醒                             （设了硬上限且将用尽才添）

延迟工具索引                             （工具太多、启用 tool_search 才添）

模型专属指令                             （models.json 有 base_instructions 才添）

风格叠加层（魂）                         （SOUL 内容非空才添；永远压轴）
```

这里分三种寿命：

| 内容 | 何时算 |
| --- | --- |
| 人格、环境、项目指令、feature、platform | 建立或重建 `AgentLoop` 时拼 |
| 项目记忆、轮数提醒 | 每个外层用户回合或每个内部请求按需添 |
| 延迟工具索引、模型专属指令、魂、think/model override | 真发请求前由 backend 包装层现添 |

项目记忆虽塞进 `system`，正文却明说它只是线索，不是新指令。它会随这条 query 一起用于本次 `AgentLoop::Run` 里的每个内部请求，不写进对话 `history_`。

工具定义也不靠 prompt 里手写。`ToolRegistry` 每一圈现列一遍，再变成独立的 `request.tools`。这使 JSON Schema 真正受协议约束，也让 `tool_search` 新挂载的工具能在下一圈立刻出现。

## 第 2 步：先拼一份中立 Request

第一圈里，`AgentLoop` 拼出的意思如下。为看清骨架，只展开 `read_file`，其余工具以省略号代替：

```cpp
api::Request{
    .model = "<当前模型>",
    .system = "<上节各段拼成的完整文本>",
    .messages = {
        api::Message{
            .role = User,
            .content = {TextBlock{"帮我看一下当前项目"}}
        }
    },
    .max_tokens = 4096,
    .tools = {
        ToolDefinition{
            .name = "read_file",
            .description = "读取文件内容,每行前面带上行号……",
            .input_schema = {
                {"type", "object"},
                {"properties", {
                    {"path",   {{"type", "string"}}},
                    {"offset", {{"type", "integer"}}},
                    {"limit",  {{"type", "integer"}}}
                }},
                {"required", {"path"}}
            }
        },
        // search, run_command, write_file, edit_file, agent, ...
    }
}
```

默认主工具表不止这些。基础表含 `read_file`、`run_command`、后台命令工具、写改文件、`search`、`skill`、`web_fetch`；配置齐全时再挂 `web_search`、MCP、LSP、插件。主会话另有 `agent`、`todo_write`、交互态下的 `ask_user`，项目记忆可写时还有 `memory_save`。

这份 request 发出前还会被包装层改几笔：

- `/model` 选中的模型盖过初始 model。
- `/think` 写入推理档位和模型 variant 私有参数。
- 延迟工具索引、模型专属指令与魂接到 system 尾部。
- provider `extra_body` 与模型 variant `extra_body` 最后做顶层浅合并；同名键以后者为准。

历史若太长，`TrimHistory` 只裁“发给模型看的副本”。它保住最早一轮与最近三轮，整轮丢掉中段；工具调用与结果同进同退。单条工具结果仍太大，才截正文并加标记。`history_` 本身不因这道硬裁剪而缩短；真正改活历史的是 `/compact` 或自动 compact。

请求拼装前还有两道工序：先是**无损结构压缩**（agent/context_events）——从历史派生规范化事件账，冷区里同键同指纹的只读工具结果（重复读取、重复搜索）只留一份正文加引用计数，被新版本覆盖的旧读取保头部预览并标注，超长结果换 artifact 引用；这层只改"发给模型的视图"，活历史与 session JSONL 一字不动，副作用工具不判重、绝不因此跳执行。再是 mid-turn 评估：系统提示 + 工具定义 + 全份历史 + 输出预留（统一 token 口径：ASCII 4 字符约 1 token，非 ASCII 每字约 1.5 token）估过有效窗口的 80%，就在这个"工具结果已攒完、请求尚未发出"的安全点先做一次语义压缩，不再等下一条用户消息。轮级硬裁真丢了东西（丢轮或截结果）时，会向终端发一条显式告警——有损降级不许静默发生。

## 第 3 步：翻成 OpenAI Chat JSON

熟悉 Chat Completions 的读者，先看这一份。真实请求会带全部工具定义；这里仍只展开 `read_file`：

```json
{
  "model": "<当前模型>",
  "stream": true,
  "max_tokens": 4096,
  "messages": [
    {
      "role": "system",
      "content": "<完整 system 文本>"
    },
    {
      "role": "user",
      "content": "帮我看一下当前项目"
    }
  ],
  "reasoning_effort": "<当前 think 档；空时不发>",
  "tools": [
    {
      "type": "function",
      "function": {
        "name": "read_file",
        "description": "读取文件内容,每行前面带上行号……",
        "parameters": {
          "type": "object",
          "properties": {
            "path": {"type": "string", "description": "要读取的文件路径,相对或绝对均可"},
            "offset": {"type": "integer", "description": "从第几行开始读(从 1 计数),不填就从第 1 行开始"},
            "limit": {"type": "integer", "description": "最多读多少行,不填就读到文件末尾"}
          },
          "required": ["path"]
        }
      }
    }
  ]
}
```

地址是：

```text
<base_url>/chat/completions
```

到这里，与普通的 OpenAI Chat 调用并无玄虚。差别还没露头。

## 第 4 步：模型先回工具调用

模型若肯直接答，流里只来文字，第一圈便收工。可“看一下当前项目”须读仓库。这里模拟它先要 `README.md`。

Chat SSE 可能分成下面几片。**分片边界不固定**，有时名字和参数会拆得更碎：

```text
data: {"id":"chatcmpl_01","model":"<当前模型>","choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_01","type":"function","function":{"name":"read_file","arguments":""}}]}}]}

data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"path\":\"README.md\"}"}}]},"finish_reason":"tool_calls"}]}

data: [DONE]
```

Chat parser 先把零碎参数攒成字符串，再吐给会话层：

```text
MessageStart(id="chatcmpl_01", model="<当前模型>")
ContentBlockDone(index=0)  // 收掉工具前可能已有的正文块；本例为空
ToolUseStart(index=0, id="call_01", name="read_file")
ToolUseInputDelta(index=0, partial_json="{\"path\":\"README.md\"}")
ContentBlockDone(index=0)
MessageDone(stop_reason="tool_use", usage=...)
```

`MessageAssembler` 再把事件拼成一条完整消息：

```cpp
api::Message{
    .role = Assistant,
    .content = {
        ToolUseBlock{
            .id = "call_01",
            .name = "read_file",
            .input = {{"path", "README.md"}}
        }
    }
}
```

这条 assistant 消息立刻写进 `history_`。此时还没有最终答话，`RunTurn` 也没有返回。

## 第 5 步：本机执行工具，再把结果伪成下一条消息

`AgentLoop` 在本机 registry 里找 `read_file`。它先跑 hook 与权限判断，再执行工具。`read_file` 是只读工具，不须确认。

工具返回的是一段普通字符串，例如：

```text
     1  # LubanCode
     2
     3  一个面向终端的 coding agent……
     4  ...
```

LubanCode 清理非法 UTF-8 后，把它包成：

```cpp
api::Message{
    .role = User,
    .content = {
        ToolResultBlock{
            .tool_use_id = "call_01",
            .content = "     1\t# LubanCode\n     2\t...",
            .is_error = false
        }
    }
}
```

于是内部历史成了：

```text
history_[0]  User       Text("帮我看一下当前项目")
history_[1]  Assistant  ToolUse(call_01, read_file, {path: README.md})
history_[2]  User       ToolResult(call_01, "README 正文……")
```

这里的 `User` 不是说真人又发了一句话。它只是中立层借用 `User` 角色，装工具结果。协议 adapter 自会翻成各家的正规形状。

同一条 assistant 若一次叫了三个工具，LubanCode 会依次执行，最后把三个 `ToolResultBlock` 装进同一条 `User` 消息，再发下一圈。

## 第 6 步：第二次请求把整段历史重发

第二圈没有新的真人输入。`AgentLoop` 直接重建 request：同一份 system、更新后的完整 history、当下可用工具表。下面只展开 `messages`；`tools` 与第一份请求一样，仍带完整定义。

Chat wire 会把中立历史翻成大家熟悉的样子：

```json
{
  "model": "<当前模型>",
  "stream": true,
  "max_tokens": 4096,
  "messages": [
    {"role": "system", "content": "<完整 system 文本>"},
    {"role": "user", "content": "帮我看一下当前项目"},
    {
      "role": "assistant",
      "content": null,
      "tool_calls": [
        {
          "id": "call_01",
          "type": "function",
          "function": {
            "name": "read_file",
            "arguments": "{\"path\":\"README.md\"}"
          }
        }
      ]
    },
    {
      "role": "tool",
      "tool_call_id": "call_01",
      "content": "     1\t# LubanCode\n     2\t..."
    }
  ]
}
```

看明白这一段，Agent 主循环也就看明白七成了：**工具循环本身没有另造一种神秘协议，不过是程序自动替用户续了一轮 `assistant tool_call -> tool result`。**

## 同一份历史，Anthropic 怎样穿

Anthropic Messages 没有 `role: "tool"`。工具调用与结果都是 content block：

```json
{
  "model": "<当前模型>",
  "max_tokens": 4096,
  "stream": true,
  "system": "<完整 system 文本>",
  "messages": [
    {
      "role": "user",
      "content": [
        {"type": "text", "text": "帮我看一下当前项目"}
      ]
    },
    {
      "role": "assistant",
      "content": [
        {
          "type": "tool_use",
          "id": "call_01",
          "name": "read_file",
          "input": {"path": "README.md"}
        }
      ]
    },
    {
      "role": "user",
      "content": [
        {
          "type": "tool_result",
          "tool_use_id": "call_01",
          "content": "     1\t# LubanCode\n     2\t..."
        }
      ]
    }
  ],
  "tools": [
    {
      "name": "read_file",
      "description": "读取文件内容,每行前面带上行号……",
      "input_schema": {
        "type": "object",
        "properties": {
          "path": {"type": "string", "description": "要读取的文件路径,相对或绝对均可"},
          "offset": {"type": "integer", "description": "从第几行开始读(从 1 计数),不填就从第 1 行开始"},
          "limit": {"type": "integer", "description": "最多读多少行,不填就读到文件末尾"}
        },
        "required": ["path"]
      }
    }
  ]
}
```

地址是 `<base_url>/v1/messages`。Anthropic 的 `system` 在顶层；tools 直接用 `name/description/input_schema`，不再套 `type:function/function`。

若开 extended thinking，返回的 `thinking` 块与 `signature` 会存进中立历史，后续请求仍按 Anthropic 形状重放。少了 signature，服务端可能拒绝。Chat 与 Responses 的思考块则不拿去续传。

## 同一份历史，Responses 怎样穿

Responses API 不叫 `messages`，叫 `input`。函数调用与函数结果各是一枚顶层 item：

```json
{
  "model": "<当前模型>",
  "max_output_tokens": 4096,
  "stream": true,
  "store": false,
  "instructions": "<完整 system 文本>",
  "input": [
    {
      "type": "message",
      "role": "user",
      "content": [
        {"type": "input_text", "text": "帮我看一下当前项目"}
      ]
    },
    {
      "type": "function_call",
      "call_id": "call_01",
      "name": "read_file",
      "arguments": "{\"path\":\"README.md\"}"
    },
    {
      "type": "function_call_output",
      "call_id": "call_01",
      "output": "     1\t# LubanCode\n     2\t..."
    }
  ],
  "tools": [
    {
      "type": "function",
      "name": "read_file",
      "description": "读取文件内容,每行前面带上行号……",
      "parameters": {
        "type": "object",
        "properties": {
          "path": {"type": "string", "description": "要读取的文件路径,相对或绝对均可"},
          "offset": {"type": "integer", "description": "从第几行开始读(从 1 计数),不填就从第 1 行开始"},
          "limit": {"type": "integer", "description": "最多读多少行,不填就读到文件末尾"}
        },
        "required": ["path"]
      }
    }
  ]
}
```

地址是 `<base_url>/responses`。

`store: false` 写得明白：LubanCode 不靠 `previous_response_id` 串会话，也不指望服务端替它记账。每次请求都从本地 history 重新展开。Responses 的 reasoning 也按一次性内容处置，不塞回下一次 `input`。

三家对照如下：

| 中立含义 | Chat Completions | Anthropic Messages | OpenAI Responses |
| --- | --- | --- | --- |
| 系统提示 | `messages[0].role=system` | 顶层 `system` | 顶层 `instructions` |
| 用户文字 | `role=user` | `role=user` + `type=text` | `type=message` + `input_text` |
| 工具定义 | `type=function.function.parameters` | `name + input_schema` | 平铺 `type=function + parameters` |
| 模型叫工具 | assistant `tool_calls` | assistant `tool_use` block | `function_call` item |
| 工具结果 | `role=tool` | user `tool_result` block | `function_call_output` item |
| 会话状态 | 每次重发 history | 每次重发 history | `store:false`，每次重发 history |

## 第 7 步：第二次流返回最终正文

模型读过 README，若资料够了，就开始吐正文。Chat SSE 大致如此：

```text
data: {"choices":[{"delta":{"content":"这是个 C++20 写的终端 coding agent。"}}]}

data: {"choices":[{"delta":{"content":"主入口在 src/main.cpp，模型协议分三套……"}}]}

data: {"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":1234,"completion_tokens":180}}

data: [DONE]
```

adapter 把三家各异的 SSE 归一成：

```text
TextDelta("这是个 C++20 写的终端 coding agent。")
TextDelta("主入口在 src/main.cpp，模型协议分三套……")
MessageDone(stop_reason="end_turn", usage={...})
```

`RunTurn` 一边收 `TextDelta`，一边往终端写。`MessageAssembler` 同时在背后攒全文。流结束后，它产出：

```cpp
api::Message{
    .role = Assistant,
    .content = {
        TextBlock{"这是个 C++20 写的终端 coding agent。主入口在 src/main.cpp，模型协议分三套……"}
    }
}
```

没有 `ToolUseBlock`，stop reason 也是 `end_turn`，内部循环就停。`AgentLoop::Run` 返回 `RunTurn`，统计行会把这轮各次请求的 token 相加，并显示请求次数。这个例子会是 `2 requests`，不是 `1`。

最终内存历史共有四条：

```text
0  User       Text("帮我看一下当前项目")
1  Assistant  ToolUse(call_01, read_file, ...)
2  User       ToolResult(call_01, README 正文, ok)
3  Assistant  Text("这是个 C++20 写的……")
```

交互模式随后把这四条新增消息逐条 append + flush 到：

```text
~/.lubancode/sessions/<session-id>.jsonl
```

文件首行是会话 meta，往后每行一条中立消息。上面的模拟落盘后，大致如下；`ts`、model 与工具正文按实值写：

```jsonl
{"version":1,"wire":"chat_completions","model":"<当前模型>","cwd":"D:\\lubancode","started_at":"<时间>"}
{"role":"user","content":[{"type":"text","text":"帮我看一下当前项目"}],"ts":"<时间>"}
{"role":"assistant","content":[{"type":"tool_use","id":"call_01","name":"read_file","input":{"path":"README.md"}}],"ts":"<时间>"}
{"role":"user","content":[{"type":"tool_result","tool_use_id":"call_01","content":"     1\t# LubanCode\n     2\t...","is_error":false}],"ts":"<时间>"}
{"role":"assistant","content":[{"type":"text","text":"这是个 C++20 写的终端 coding agent……"}],"ts":"<时间>"}
```

存档用的是中立格式，不是某一家 wire 的原始 JSON。故而恢复以后还能切 provider，再由新 adapter 把同一份 history 翻出去。

它不是每来一个 SSE 字符就写盘。要等这一轮 `RunTurn` 收口，才把新增的完整消息落下。成功、报错、ESC 打断都走这道收尾。

## 下一句又怎样

用户接着问：

```text
那它怎么切 provider？
```

`AgentLoop` 不会只发这一句。它先把新 user 消息压到上面四条之后，再把当前有效历史整个交给 adapter：

```text
User("帮我看一下当前项目")
Assistant(ToolUse read_file)
User(ToolResult README)
Assistant("这是个 C++20……")
User("那它怎么切 provider？")
```

这便是普通多轮对话。差别只在前一轮中间夹了工具往返。

上下文涨到 80% 左右时，主循环会先调用 compact 模型，把老历史摘要成一条 archive（六栏存档 + 末尾 JSON manifest，活动待办漏一项就拒收、历史不动），再按 token 预算保留最近热区。压缩模型自己的窗口单独算预算，装不下就明确拒绝，不静默截史。若还没来得及 compact，`TrimHistory` 另有字符上限作最后拦网，触发时打醒目告警。

## `agent` 子代理为何不一样

普通工具没有自己的对话。`read_file` 收 JSON，跑函数，吐字符串，仅此而已。

`agent` 虽也以工具身份挂在主 registry，里头却会新建一只空历史的 `AgentLoop`：

```text
主 history
└─ Assistant ToolUse("agent", {prompt, agent_type, ...})
   └─ 子 AgentLoop（全新 system、全新 history、自己的多次模型请求）
      ├─ User(prompt)
      ├─ Assistant ToolUse(search)
      ├─ User ToolResult(...)
      ├─ Assistant ToolUse(read_file)
      ├─ User ToolResult(...)
      └─ Assistant Text(最终结论)
└─ User ToolResult("子代理最终结论")
```

主历史只收两样：委托入参和最终结果。子代理中途搜了什么、试错几回，不挤进主上下文。

`agent_type=Explore` 正是特定子代理类型，不是一条普通搜索命令。它有独立 persona，工具表也从代码上锁死为只读：`read_file`、`search`、`web_fetch`、可选 `web_search`、可选 `lsp`。它拿不到 `run_command`、写文件工具、`skill`，也拿不到 `agent`，不能再生孙代理。

`general-purpose` 则能拿基础工具，做多步任务。前台子代理跑完才回主循环；后台子代理先回任务编号，另在线程里跑。后台结果完成后，会在后续外层用户消息里作为一段“不可信参考资料”附上，再交给主模型。故而后台 agent 更不像普通同步 tool call。

## 还有几条岔路

### 模型一次也不叫工具

只走一次 HTTP。历史是 `User -> Assistant Text`。这就是最朴素的多轮聊天。

### 模型连续叫好几轮工具

每轮都重复同一套动作：

```text
发完整 history
-> 收 assistant tool_use
-> 本机执行
-> 添 user tool_result
-> 再发完整 history
```

默认主循环不设内部轮数上限，靠模型 `end_turn` 或用户 ESC 收口；配置了正数 `max_turns` 才有硬闸。

### 服务端内置 web search

Responses 与 Anthropic 还可声明 provider 自带的 web search。它在服务端执行，LubanCode 只把事件画给用户看，绝不会再塞进本地 `ToolRegistry` 执行一遍。这与本地 `web_search` 函数工具是两条路。

### ESC 打断

半截文字也会收进 assistant 历史，并添上“用户按 ESC 打断”的标记。若半截流里已有孤零零的 tool call，LubanCode 会补一条报错 tool result，守住配对。否则下一次重放历史，API 多半直接报 400。

### 排队输入

模型流式输出时，用户键入下一句，只进 `pending_queue`。当前 `AgentLoop::Run` 收口后，交互循环再按顺序取出，仍走同一套 `process_line -> RunTurn`。它不会悄悄插进正在发的 HTTP 请求。

### 跨会话来信

跨会话 inbox 只在内部工具轮次的安全边界收信：上一个工具结果已经入历史，下一次模型请求还没发。它不在流式半截或权限确认当口抢话。纯文本轮没有这个内部缝，来信会排到本轮收口以后。

## 所以，它到底算不算“只是多轮对话”

从协议看，算。核心数据仍是：

```text
system + history + tools -> assistant -> tool result -> assistant
```

从程序行为看，又不止普通聊天。它多包了六层活：

| 普通 Chat 示例常由业务代码自己补 | LubanCode 已经包办 |
| --- | --- |
| 手工保存 `messages` | 中立 history + JSONL session |
| 手工识别 `tool_calls` | 三家 SSE 统一成 `StreamEvent` |
| 手工执行函数 | ToolRegistry、确认、hooks、结果清洗 |
| 手工再次请求模型 | `AgentLoop` 自动循环到 `end_turn` |
| 静态 system prompt | 人格、项目指令、能力、记忆、模型、魂逐层拼 |
| 一条会话线 | 可另起子代理独立 history，主线只收结论 |

一句话收住：**LubanCode 不是另造了一种对话协议；它在普通多轮消息之上，添了一只会自己执行工具、回填结果、继续请求的本地控制器。**

## 源码从哪儿看

| 想追什么 | 文件 |
| --- | --- |
| 外层输入、prompt 装配、工具注册、落盘 | [`src/main.cpp`](../src/main.cpp) |
| 内部请求循环、history、工具回填 | [`src/agent/loop.cpp`](../src/agent/loop.cpp) |
| 中立消息、内容块、流事件 | [`src/api/types.hpp`](../src/api/types.hpp) |
| 流事件拼成 assistant 消息 | [`src/api/assembler.cpp`](../src/api/assembler.cpp) |
| system prompt 模块次序 | [`src/agent/prompt_assembler.cpp`](../src/agent/prompt_assembler.cpp) |
| 模型指令、延迟索引、魂 | [`src/agent/prompts.hpp`](../src/agent/prompts.hpp) |
| 历史裁剪 | [`src/agent/context.cpp`](../src/agent/context.cpp) |
| Chat 请求与回包 | [`src/api/chat/request.cpp`](../src/api/chat/request.cpp)、[`events.cpp`](../src/api/chat/events.cpp) |
| Responses 请求与回包 | [`src/api/responses/request.cpp`](../src/api/responses/request.cpp)、[`events.cpp`](../src/api/responses/events.cpp) |
| Anthropic 请求与回包 | [`src/api/anthropic/client.cpp`](../src/api/anthropic/client.cpp)、[`events.cpp`](../src/api/anthropic/events.cpp) |
| 子代理与 Explore | [`src/tools/agent_tool.cpp`](../src/tools/agent_tool.cpp) |
| JSONL 会话 | [`src/agent/session_store.cpp`](../src/agent/session_store.cpp) |
