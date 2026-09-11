# 模型接入怎么抽象：四套 wire 与一轮工具调用

核对日期：2026-09-10。本文按当前源码讲实现。JSON 是教学示例，按 adapter 组包规则手写，没调用真实模型；工具 `read_file` 也是示例名称，不代表项目实际注册名。模型名用占位符。示例关闭思考，不带图片、缓存和厂商扩展参数。

## 1. 面试先这样答

> 我把模型接入拆成统一数据结构、统一调用接口、四套协议适配。Agent 用 Request 表达请求，用 Message 和内容块表达对话，通过 Backend::send_stream 调模型。下面按 wire 选一套 adapter，把请求翻成厂商 JSON，再把流式响应翻回统一事件。MessageAssembler 把事件拼成消息，主循环从中取工具调用，执行，写回结果，再发下一次请求。
>
> 当前内部结构更接近 Anthropic：User、Assistant 两种角色，加正文、思考、工具调用、工具结果等内容块。system 单放。抽象落在请求、内容块和事件上；各家消息怎么拆、字段怎么排，交给 adapter。

四套 wire 是四种协议，不是四家厂商。OpenAI 占两套；兼容同一协议的不同厂商，可以复用 adapter。每次请求只走选中那套，不会把四套都发一遍。

## 2. 究竟抽出了什么

先看主循环要办什么：发对话，收正文，接工具调用，把结果送回去。再把这些动作所需数据定成类型。

以下是裁剪后的 C++ 结构，方便口述，完整定义见 [types.hpp](../src/api/types.hpp)：

```cpp
enum class Role { User, Assistant };

struct TextBlock { std::string text; };
struct ToolUseBlock {
    std::string id;
    std::string name;
    nlohmann::json input;
    // 完整定义还保留 caller 等信息。
};
struct ToolResultBlock {
    std::string tool_use_id;
    std::string content;
    bool is_error = false;
    // 完整定义还有富内容块与 structured_content。
};

// 示例只列三种，实际还含图片、思考、服务端工具块等。
using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock>;
struct Message {
    Role role;
    std::vector<ContentBlock> content;
};

struct Request {
    std::string model;
    std::string system;
    std::vector<Message> messages;
    std::optional<int> max_tokens;
    std::vector<ToolDefinition> tools;
    // 实际还含推理配置与 extra_body 等字段。
};
```

角色回答“这条消息放在哪一方”；内容块回答“这里装了什么”。内部 `User + ToolResultBlock` 表达工具结果，不能只看 User 就认作真人输入。

统一接口在 [backend.hpp](../src/api/backend.hpp)：

```cpp
virtual std::expected<void, Error> send_stream(
    const Request& request,
    const std::function<void(const StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel = nullptr) = 0;
```

主循环依赖这份接口。四套后端实现它，负责组包、发送、解析。错误通过返回值或流事件交给上层；工具权限、执行和重试策略另有归属。

这里既有接口抽象，也有数据抽象。只套一个虚函数，却让主循环到处读 `tool_calls`、`functionCall`，协议差异照样漏到上层。

### 2.1 转成 wire 之前，原始形状长什么样

这里说的“原始形状”，指 Agent 手里的 `api::Request`，还没转成厂商请求体。它是 C++ 对象；工具参数 `input` 用 `nlohmann::json` 保存，整份 Request 尚未序列化成出站 JSON。

工具执行完，准备再请求模型时，这份对象大致如下。仍用教学工具 `read_file` 举例：

```text
Request
├── model: "当前模型"
├── system: "你是一名编程助手。"
├── max_tokens: 1024
├── tools:
│   └── ToolDefinition
│       ├── name: "read_file"
│       ├── description: "读取文本文件"
│       └── input_schema:
│           {"type":"object",
│            "properties":{"path":{"type":"string"}},
│            "required":["path"]}
│
└── messages:
    ├── Message
    │   ├── role: User
    │   └── content:
    │       └── TextBlock
    │           └── text: "读一下 README.md"
    │
    ├── Message
    │   ├── role: Assistant
    │   └── content:
    │       └── ToolUseBlock
    │           ├── id: "call_1"
    │           ├── name: "read_file"
    │           └── input: {"path":"README.md"}
    │
    └── Message
        ├── role: User
        └── content:
            └── ToolResultBlock
                ├── tool_use_id: "call_1"
                ├── content: "项目说明正文"
                └── is_error: false
```

骨架就三层：`Request.messages` 装消息，`Message.content` 装内容块，`ContentBlock` 用 `std::variant` 区分块类型。system 在消息数组外，工具结果没有独立 Tool role。

一条消息还能装多块。比如 assistant 先说话，再调工具：

```text
Message(role=Assistant)
└── content:
    ├── TextBlock(text="我先读一下文件。")
    └── ToolUseBlock(id="call_1", name="read_file",
                    input={"path":"README.md"})
```

上面是对象树示意，不是可直接编译的 C++ 初始化语句。四套 adapter 都从这份结构出发：Anthropic 翻成内容块，Chat 提取 tool_calls 并拆出 tool 消息，Responses 翻成独立 item，Gemini 翻成 functionCall/functionResponse。

面试可以这样说：

> 原始数据是一份 Request，里面按 Message 组织对话，再用不同 ContentBlock 表达正文、调用和结果。主循环处理这份对象，选中的 adapter 才负责把它序列化成对应协议。

## 3. 一轮工具调用，实际走两次模型请求

假设用户说“读一下 README.md”，工具返回“项目说明正文”。正常完成这段交互，要走下面几步：

1. 第一次请求：发 system、用户消息和工具定义。
2. 模型第一次响应：要求调用 `read_file`，参数是 `{"path":"README.md"}`。
3. 宿主执行工具，拿到“项目说明正文”。模型只提出调用，本地工具由宿主执行。
4. 第二次请求：带上用户消息、模型刚发出的工具调用、对应工具结果，工具定义继续随行。
5. 模型第二次响应：读结果，回答用户。

下面四份 JSON 都展示第 4 步：**工具执行完，发给模型的第二次请求体**。这样一份就能看见工具定义、调用与结果。要看第 1 步，把历史中末尾两项（调用、结果）删去即可，其余字段不变。

模型响应里的调用会先归一成 `ToolUseBlock`，再由 adapter 重编码进第二次请求；下例中那条调用不是宿主凭空编造。响应外层、usage 和 SSE 帧不属于请求体，未列在这里。

## 4. Anthropic Messages

由 [anthropic/client.cpp](../src/api/anthropic/client.cpp) 的 `BuildRequestJson` 与 `ContentBlockToJson` 组包。

```json
{
  "model": "<anthropic-model>",
  "max_tokens": 1024,
  "stream": true,
  "system": "你是一名编程助手。",
  "tools": [
    {
      "name": "read_file",
      "description": "读取文本文件",
      "input_schema": {
        "type": "object",
        "properties": {"path": {"type": "string"}},
        "required": ["path"]
      }
    }
  ],
  "messages": [
    {
      "role": "user",
      "content": [{"type": "text", "text": "读一下 README.md"}]
    },
    {
      "role": "assistant",
      "content": [
        {
          "type": "tool_use",
          "id": "call_1",
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
          "tool_use_id": "call_1",
          "content": "项目说明正文"
        }
      ]
    }
  ]
}
```

`messages[].role` 只有 user、assistant。system 在顶层，没有 `role=system`；工具结果在 user 内容块里，没有 `role=tool`。`tool_use_id` 对上调用的 `id`。工具参数 `input` 是 JSON 对象。

## 5. OpenAI Chat Completions

由 [chat/request.cpp](../src/api/chat/request.cpp) 的 `BuildRequestJson` 组包。

```json
{
  "model": "<chat-model>",
  "max_tokens": 1024,
  "stream": true,
  "tools": [
    {
      "type": "function",
      "function": {
        "name": "read_file",
        "description": "读取文本文件",
        "parameters": {
          "type": "object",
          "properties": {"path": {"type": "string"}},
          "required": ["path"]
        }
      }
    }
  ],
  "messages": [
    {"role": "system", "content": "你是一名编程助手。"},
    {"role": "user", "content": "读一下 README.md"},
    {
      "role": "assistant",
      "content": null,
      "tool_calls": [
        {
          "id": "call_1",
          "type": "function",
          "function": {
            "name": "read_file",
            "arguments": "{\"path\":\"README.md\"}"
          }
        }
      ]
    },
    {"role": "tool", "tool_call_id": "call_1", "content": "项目说明正文"}
  ]
}
```

当前 adapter 把 system 放成首条消息，把内部 `User + ToolResultBlock` 拆成 `role=tool`。无正文的 assistant 用 `content: null`。`arguments` 是 JSON 字符串，要先序列化，不能直接塞对象。

若内部一条 User 同时装正文和工具结果，这里会拆出 user、tool 两条。因此，内部消息数与出站消息数未必相等。

## 6. OpenAI Responses

由 [responses/request.cpp](../src/api/responses/request.cpp) 的 `BuildRequestJson` 与 `ContentBlockToItem` 组包。

```json
{
  "model": "<responses-model>",
  "max_output_tokens": 1024,
  "stream": true,
  "store": false,
  "instructions": "你是一名编程助手。",
  "tools": [
    {
      "type": "function",
      "name": "read_file",
      "description": "读取文本文件",
      "parameters": {
        "type": "object",
        "properties": {"path": {"type": "string"}},
        "required": ["path"]
      }
    }
  ],
  "input": [
    {
      "type": "message",
      "role": "user",
      "content": [{"type": "input_text", "text": "读一下 README.md"}]
    },
    {
      "type": "function_call",
      "call_id": "call_1",
      "name": "read_file",
      "arguments": "{\"path\":\"README.md\"}"
    },
    {"type": "function_call_output", "call_id": "call_1", "output": "项目说明正文"}
  ]
}
```

这里用 `input` item 数组。工具调用与结果各占独立 item，不必塞进 assistant/user 消息。两项靠 `call_id` 配对；别把响应里的 item `id` 和 `call_id` 混为一谈。参数仍是 JSON 字符串。

当前实现设 `store: false`，自己带历史，不靠 `previous_response_id` 串上下文。工具定义比 Chat 少一层 `function` 包装。

## 7. Gemini Generate Content

由 [gemini/request.cpp](../src/api/gemini/request.cpp) 的 `BuildRequestJson` 组包。模型名走请求 URL；流式请求由 client 选择 `streamGenerateContent`，请求体不放 `model`、`stream`。见 [gemini/client.cpp](../src/api/gemini/client.cpp)。

```json
{
  "systemInstruction": {"parts": [{"text": "你是一名编程助手。"}]},
  "generationConfig": {"maxOutputTokens": 1024},
  "tools": [
    {
      "functionDeclarations": [
        {
          "name": "read_file",
          "description": "读取文本文件",
          "parameters": {
            "type": "object",
            "properties": {"path": {"type": "string"}},
            "required": ["path"]
          }
        }
      ]
    }
  ],
  "contents": [
    {"role": "user", "parts": [{"text": "读一下 README.md"}]},
    {
      "role": "model",
      "parts": [{"functionCall": {"name": "read_file", "args": {"path": "README.md"}}}]
    },
    {
      "role": "user",
      "parts": [{"functionResponse": {"name": "read_file", "response": {"result": "项目说明正文"}}}]
    }
  ]
}
```

assistant 映成 `model`；工具结果放进 user 的 `functionResponse`。`args` 是对象，`response` 也是对象。当前 adapter 遇到纯文本结果，会包成 `{"result": "..."}`；若结果本身是 JSON 对象，则直接使用。

当前 adapter 没把内部调用 ID 写进这份请求。它先遍历历史，建立 `tool_use_id → 函数名` 映射，再为结果填 `name`。这是本仓当前映射方式，不表示所有 Gemini 版本都没有调用 ID，也不表示覆盖了所有模型要求。

## 8. 四套放在一起，差异在哪

| 内部概念 | Anthropic | Chat Completions | Responses | Gemini |
| --- | --- | --- | --- | --- |
| system | 顶层 system | system 消息 | instructions | systemInstruction |
| 对话容器 | messages | messages | input | contents |
| 助手角色 | assistant | assistant | 普通消息用 assistant；调用另作 item | model |
| 工具调用 | tool_use 块 | tool_calls 数组 | function_call item | functionCall part |
| 工具参数 | input 对象 | arguments 字符串 | arguments 字符串 | args 对象 |
| 工具结果 | user 内 tool_result | tool 消息 | function_call_output item | user 内 functionResponse |
| 当前结果关联 | tool_use_id | tool_call_id | call_id | adapter 从内部 ID 找回 name |

所以 adapter 做的是结构转换：移动 system，拆分消息，转换对象与字符串，维持调用关联。只改 role 字符串，远远不够。

## 9. 返回时怎么抽象

```text
Agent 组 api::Request
  → Backend::send_stream
  → 当前 adapter 的 BuildRequestJson
  → HTTP / SSE
  ← 厂商 JSON 事件
  ← adapter 翻成 api::StreamEvent
  → MessageAssembler::Feed
  → MessageAssembler::BuildMessage
  → Agent 取 ToolUseBlock，交给工具执行流程
  → 写回 ToolResultBlock，进入下一次请求
```

SSE 分帧只处理传输边界，不负责理解工具。各家 `events.cpp` 才读厂商字段，翻成 `TextDelta`、`ThinkingDelta`、`ToolUseStart`、`ToolUseInputDelta`、`MessageDone` 等事件。

工具参数可能分几帧才来齐。assembler 按块累积参数片段，收尾时解析，拼成 `ToolUseBlock`。主循环不用为四套 SSE 格式各写一遍拼包逻辑。参数校验、权限检查与真正执行留给后续流程，不能看见半截参数就开跑。

## 10. 面试时打开哪些代码

### 10.1 当前 adapter 就在哪几处

代码在 `src/api/`。adapter 是这里对“协议适配职责”的称呼，具体类叫 Backend，不必搜名为 Adapter 的类。

| wire | 具体后端类 | 请求转换入口 | 响应转换入口 |
| --- | --- | --- | --- |
| Anthropic Messages | AnthropicBackend | [anthropic/client.cpp](../src/api/anthropic/client.cpp) 的 BuildRequestJson | [anthropic/events.cpp](../src/api/anthropic/events.cpp) |
| Chat Completions | ChatCompletionsBackend | [chat/request.cpp](../src/api/chat/request.cpp) 的 BuildRequestJson | [chat/events.cpp](../src/api/chat/events.cpp) |
| Responses | ResponsesBackend | [responses/request.cpp](../src/api/responses/request.cpp) 的 BuildRequestJson | [responses/events.cpp](../src/api/responses/events.cpp) |
| Gemini | GeminiBackend | [gemini/request.cpp](../src/api/gemini/request.cpp) 的 BuildRequestJson | [gemini/events.cpp](../src/api/gemini/events.cpp) |

先打开 `BuildRequestJson()`：它接收统一 `api::Request`，返回对应协议 JSON。四套函数名字相同，分属不同 namespace；它们不是 Backend 上的虚函数。主循环调用虚函数 `send_stream()`，具体后端再调用自家 builder。

拿 Chat 来看，沿 `BuildRequestJson()` 往下读，就能找到这四步：

```text
Request.system  → messages 首条 role=system
TextBlock       → 消息 content
ToolUseBlock    → assistant.tool_calls，input.dump() 生成 arguments 字符串
ToolResultBlock → 独立 role=tool 消息，保留 tool_call_id
```

选哪套实现，看 [app/backend_stack.cpp](../src/app/backend_stack.cpp) 的 `BuildBackend()`：它按 `config.wire` 创建具体后端。`RebuildableBackend` 保持外层对象稳定，切换时换掉内部后端，再把 `send_stream()` 转发进去。

从调用处追到转换处，按这个顺序看：

```text
src/app/backend_stack.cpp       BuildBackend：按配置选后端
src/api/backend.hpp             Backend：定义统一 send_stream 接口
src/agent/loop.cpp              backend_.send_stream：主循环发起调用
src/api/<协议>/client.cpp       具体 send_stream：调 builder，再发 HTTP
src/api/<协议>/request.cpp      BuildRequestJson：Request → 厂商 JSON
                               （Anthropic 此函数就在 client.cpp）
src/api/<协议>/events.cpp       厂商事件 → StreamEvent
src/api/assembler.cpp           StreamEvent → Message
src/agent/loop.cpp              取 ToolUseBlock，进入工具执行流程
```

### 10.2 完整源码索引

路径与符号比行号耐久。按这张表打开，搜索符号即可。

| 要证明什么 | 源码位置 | 搜索符号 |
| --- | --- | --- |
| 统一数据 | [api/types.hpp](../src/api/types.hpp) | Role、ContentBlock、Message、Request、StreamEvent |
| 统一调用合同 | [api/backend.hpp](../src/api/backend.hpp) | Backend、send_stream |
| 按 wire 选后端 | [app/backend_stack.cpp](../src/app/backend_stack.cpp) | BuildBackend、RebuildableBackend::Rebuild |
| Anthropic 组包 | [api/anthropic/client.cpp](../src/api/anthropic/client.cpp) | ContentBlockToJson、BuildRequestJson |
| Chat 组包 | [api/chat/request.cpp](../src/api/chat/request.cpp) | BuildRequestJson |
| Responses 组包 | [api/responses/request.cpp](../src/api/responses/request.cpp) | ContentBlockToItem、BuildRequestJson |
| Gemini 组包及函数名恢复 | [api/gemini/request.cpp](../src/api/gemini/request.cpp) | ToolNameByUseId、FunctionResponseBody、BuildRequestJson |
| Anthropic 响应翻译 | [api/anthropic/events.cpp](../src/api/anthropic/events.cpp) | ToolUseStart、ToolUseInputDelta |
| Chat 响应翻译 | [api/chat/events.cpp](../src/api/chat/events.cpp) | ToolUseStart、ToolUseInputDelta |
| Responses 响应翻译 | [api/responses/events.cpp](../src/api/responses/events.cpp) | ToolUseStart、ToolUseInputDelta |
| Gemini 响应翻译 | [api/gemini/events.cpp](../src/api/gemini/events.cpp) | ToolUseStart、ToolUseInputDelta |
| 公用传输与分帧 | [api/http_stream_transport.cpp](../src/api/http_stream_transport.cpp)、[api/sse_framing.cpp](../src/api/sse_framing.cpp) | PostSseStream；分帧实现 |
| 统一事件拼消息 | [api/assembler.cpp](../src/api/assembler.cpp) | MessageAssembler::Feed、BuildMessage |
| 主循环真正消费抽象 | [agent/loop.cpp](../src/agent/loop.cpp) | backend_.send_stream、assembler.Feed、assembler.BuildMessage、ToolUseBlock |
| 四套请求形状合同 | [test_wire_role_contract.cpp](../tests/unit/api/test_wire_role_contract.cpp) | system、工具调用、工具结果、消息数量 |
| HTTP/SSE 回放 | [test_wire_replay.cpp](../tests/integration/api/test_wire_replay.cpp) | 各 wire 回放用例 |

## 11. 追问怎么接

**“你这不就是照着 Anthropic 写的吗？”**

> 数据形状确实更接近 Anthropic，两种角色加内容块。不过 Agent 依赖自家类型与 Backend 接口。Chat、Responses、Gemini 都在边界转换。中立指上层不用按厂家分支，不是说设计不能借鉴某家协议。

**“加一家模型怎么办？”**

> 先看协议。兼容已有 wire，通常补端点、模型与能力配置；出现新字段或新事件，再补对应 adapter。全新协议则实现 Backend，补请求编码、事件解析和合同测试。若带来现有类型表达不了的新语义，还要扩中立类型及消费方，不能许诺永远只改配置。

**“各家特有能力怎么处理？”**

> 公共语义统一表达；特有数据该保留就保留。当前 ThinkingBlock 有 signature，Request 有推理配置和 extra_body，服务端工具另有块类型。adapter 按能力与回传规则落字段。做不到无损兼容的地方，要交代限制。

当前实现中，Anthropic 思考块带签名回传；Chat 按方言与策略选择；Responses、Gemini 组包会跳过 ThinkingBlock。这只是本仓现状，不能推成“这两套协议永远无需回传思考或签名”。本文关闭思考的示例，也不能证明所有推理模型都能这样续聊。

**“四种 role 和四种 wire 有什么关系？”**

> 没有一一对应关系。role 表达消息角色，wire 表达外部协议。当前 api::Role 只有 User/Assistant，system 单放，工具结果放内容块。trajectory v3 文档里的四角色统一壳是另一项改造，不能拿设计当当前实现。

**“怎么证明抽象没把数据转坏？”**

> 给四套 builder 同一份内部历史，逐项检查 system 落点、工具参数类型、调用与结果关联、消息拆分，再用流事件夹具和 HTTP 回放检查返回链。仓库已有对应合同与回放测试。测试证明覆盖场景，真实端点兼容还得另跑实测。

本次只核对源码、整理示例并检查文档；没有重跑 C++ 测试，也没有新增真实模型验收记录。
