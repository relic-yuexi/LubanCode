// api 层的中立类型:不带任何厂商字眼(不提 Anthropic、不提 MiniMax)。
// anthropic/、responses/ 两个后端各自把厂商私有的 JSON 结构翻译成这里的类型,
// 翻译完的东西对 agent 层长得一模一样。

#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::api {

// ---------------------------------------------------------------------------
// 内容块
// ---------------------------------------------------------------------------

// 一段纯文本。
struct TextBlock {
    std::string text;
};

// 模型发起的一次工具调用请求。
struct ToolUseBlock {
    std::string id;
    std::string name;
    nlohmann::json input;  // 工具入参,一个 JSON 对象
};

// 工具执行完,把结果回传给模型。
struct ToolResultBlock {
    std::string tool_use_id;
    std::string content;
    bool is_error = false;
};

using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock>;

// ---------------------------------------------------------------------------
// 消息
// ---------------------------------------------------------------------------

enum class Role { User, Assistant };

struct Message {
    Role role = Role::User;
    std::vector<ContentBlock> content;
};

// 工具定义,交给模型看的 JSON Schema。M1 阶段 Request::tools 恒为空,
// 这里先把形状定好,留给 M2 用。
struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
};

// ---------------------------------------------------------------------------
// 请求
// ---------------------------------------------------------------------------

struct Request {
    std::string model;
    std::string system;
    std::vector<Message> messages;
    int max_tokens = 4096;
    std::vector<ToolDefinition> tools;  // M1 先留空位,不填
    // M6.6:推理强度,none/low/medium/high,空串 = 不发这个参数(维持原有
    // 行为)。responses wire 翻成 "reasoning":{"effort":...};anthropic wire
    // 翻成 "thinking":{"type":"enabled","budget_tokens":...}(none 翻成
    // "type":"disabled"),映射关系见 anthropic/client.cpp 里的
    // BuildThinkingJson 注释。
    std::string reasoning_effort;
};

// ---------------------------------------------------------------------------
// 流式事件
// ---------------------------------------------------------------------------

struct Usage {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    // 缓存命中/缓存写入的 token 数,不是所有厂商/wire 都会给,给不出来就是 0
    // (M6.5 添加,anthropic wire 从 message_delta 的 usage 里读
    // cache_read_input_tokens/cache_creation_input_tokens;responses wire 从
    // response.completed 的 usage.input_tokens_details.cached_tokens 读
    // cache_read_tokens,没有对应的 cache_creation 概念,恒为 0)。
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
};

// 流的第一个事件,标记消息开始。
struct MessageStart {
    std::string id;
    std::string model;
};

// 文本内容的增量片段,一到就能往屏幕上写。
struct TextDelta {
    std::string text;
};

// 一次工具调用开始:拿到 id 和工具名,入参还没填。
struct ToolUseStart {
    int index = 0;
    std::string id;
    std::string name;
};

// 工具调用入参的增量片段(JSON 字符串,要靠调用方自己拼完整再解析)。
struct ToolUseInputDelta {
    int index = 0;
    std::string partial_json;
};

// 某个内容块结束。
struct ContentBlockDone {
    int index = 0;
};

// 流的最后一个语义事件:消息结束,带上停止原因和用量统计。
struct MessageDone {
    std::string stop_reason;
    Usage usage;
};

// 流里出现的错误(服务端主动报错,或者本地解析出的问题)。
struct StreamError {
    std::string message;
};

using StreamEvent = std::variant<MessageStart, TextDelta, ToolUseStart, ToolUseInputDelta, ContentBlockDone, MessageDone, StreamError>;

// ---------------------------------------------------------------------------
// 错误
// ---------------------------------------------------------------------------

enum class ErrorKind {
    Network,     // 连不上、断线之类
    HttpStatus,  // HTTP 状态码非 2xx
    Parse,       // JSON / SSE 解析不动
    Api,         // 服务端返回的业务错误(error 事件)
    Cancelled,   // 用户按 ESC 主动打断,不是真出错——调用方不该当错误报给用户
};

struct Error {
    ErrorKind kind = ErrorKind::Network;
    std::string message;
    int http_status = 0;  // kind == HttpStatus 时才有意义
};

}  // namespace lubancode::api
