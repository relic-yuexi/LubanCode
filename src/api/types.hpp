// api 层的中立类型:不带任何厂商字眼(不提 Anthropic、不提 MiniMax)。
// anthropic/、responses/ 两个后端各自把厂商私有的 JSON 结构翻译成这里的类型,
// 翻译完的东西对 agent 层长得一模一样。

#pragma once

#include <cstdint>
#include <map>
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

// 用户附上的图片。data 存不带 data URL 前缀的 base64，方便两套 wire 各按
// 自家的格式包一层；filename/宽高只给本地界面、会话存档和导出展示用。
struct ImageBlock {
    std::string media_type;
    std::string data;
    std::string filename;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
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

// 模型的思考过程(extended thinking / reasoning)。text 是思考正文,
// signature 是 Anthropic extended thinking 的签名——续会话重放历史时
// thinking 块必须带 signature,否则第二轮会被服务端以 400 拒掉。
struct ThinkingBlock {
    std::string text;
    std::string signature;
};

using ContentBlock = std::variant<TextBlock, ImageBlock, ToolUseBlock, ToolResultBlock, ThinkingBlock>;

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
    // 当前模型 variant 的请求级私有参数。provider 级 extra_body 先合并，
    // 这里后合并；同名顶层键由 variant 覆盖。
    nlohmann::json extra_body = nlohmann::json::object();
};

// ---------------------------------------------------------------------------
// 流式事件
// ---------------------------------------------------------------------------

// usage 的统一口径(前缀缓存守恒单,2026-08):三家 wire 翻到这层时必须
// 摊成同一副语义,不许同一字段在一家表示"非缓存输入"、在另一家表示
// "输入总数":
//
//   input_tokens          本次未从缓存读取、按普通输入处理的 token
//   cache_read_tokens     从缓存读取的输入 token(读命中)
//   cache_creation_tokens 本次写缓存的输入 token(provider 有此概念才非 0)
//   output_tokens         输出 token
//   output_reasoning_tokens 输出 token 里属于 reasoning 的部分(含在
//               output_tokens 里,不是另加的一笔;服务端拆了账才非 0——
//               chat wire 的 completion_tokens_details.reasoning_tokens、
//               responses wire 的 output_tokens_details.reasoning_tokens;
//               没拆就是 0,不许拿 0 冒充"reasoning 为零")
//
// 各 wire 的映射(细节在各自 events.cpp):
//   anthropic   input_tokens 本来就不含 cache read/creation,原样照抄;
//   chat        DeepSeek 顶层 prompt_cache_hit/miss_tokens:input=miss,
//               cache_read=hit;OpenAI/Qwen 风格 prompt_tokens_details.
//               cached_tokens:cache_read=cached,input=max(prompt-cached,0);
//               都没有:input=prompt_tokens,cache_read=0;
//   responses   cache_read=input_tokens_details.cached_tokens,
//               input=max(input_tokens-cached_tokens,0)。
struct Usage {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_reasoning_tokens = 0;
};

// 完整输入(非缓存输入 + 缓存读 + 缓存写)的唯一算法。UI/统计/汇总一律
// 走这只 helper,不许各家各算——DeepSeek 49k hit + 1k miss 就该是 50k,
// 不是 50k+49k,也不是 1k。
inline std::int64_t TotalInputTokens(const Usage& usage) {
    return usage.input_tokens + usage.cache_read_tokens + usage.cache_creation_tokens;
}

// 一次独立模型请求的 usage 报告:on_usage 的入参(前缀缓存守恒单)。
// usage 之外带上这笔账的身份——第几步、哪个请求、什么模型、哪个缓存
// epoch、这一步前缀是不是上一份的原样追加版——逐步流水账(StepUsageRecord)
// 才有键可落,整轮汇总才能按 token 求和而不是拿百分比平均。
// request_id/model 取流里 MessageStart 的值,provider 不给就是空;
// cache_epoch/epoch_break_reason/prefix_append_only 由 AgentLoop 的前缀
// 记账(agent/prefix.hpp)在发请求前填:epoch 断不是失败,无名无姓地断
// 才是失败。
struct UsageReport {
    Usage usage;
    int step_index = 0;              // Run() 内的步号(0-based,一步一次模型请求)
    std::string request_id;          // 服务端消息 id(MessageStart.id),可空
    std::string model;               // MessageStart.model,可空
    int cache_epoch = 1;             // 请求落在哪个缓存 epoch(1 起)
    std::string epoch_break_reason;  // 本步断了 epoch 时的点名(空 = 没断)
    bool prefix_append_only = true;  // 本步请求是否上一份的原样追加版

    // provider 是否真回报了 usage(五项全零 = 没给,真实请求不可能全零)。
    // 没回报就记 unknown,不许拿 0 冒充"真未命中"。
    bool reported() const {
        return usage.input_tokens > 0 || usage.output_tokens > 0 || usage.cache_read_tokens > 0 ||
               usage.cache_creation_tokens > 0 || usage.output_reasoning_tokens > 0;
    }
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

// 思考过程的增量片段。text 是思考正文的一段,signature 是 Anthropic
// extended thinking 签名的一段(signature_delta)。chat/responses wire
// 没有 signature,signature 字段恒空。流式拼 + 复用 ContentBlockDone 收尾,
// 不加 ThinkingStart。
struct ThinkingDelta {
    std::string text;
    std::string signature;
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

// Responses 等协议的服务端内置工具。它已由模型服务执行，客户端只展示
// 轨迹，绝不能塞进 ToolUseBlock 再本地执行一遍。
struct BuiltinToolStart {
    std::string id;
    std::string name;
    nlohmann::json input = nlohmann::json::object();
};

struct BuiltinToolDone {
    std::string id;
    std::string name;
    nlohmann::json input = nlohmann::json::object();
    std::string summary;
    bool is_error = false;
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

using StreamEvent = std::variant<MessageStart, TextDelta, ThinkingDelta, ToolUseStart, ToolUseInputDelta,
                                 ContentBlockDone, BuiltinToolStart, BuiltinToolDone, MessageDone, StreamError>;

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

// 鉴权三态(向导重排单):按 token 组装请求基础头。token 非空给 Content-Type
// + Authorization;token 为空(无鉴权,或 env 缺值)只给 Content-Type,彻底
// 不发 Authorization——绝不发一枚空 Bearer 冒充无鉴权。三套正式 client
// (anthropic/responses/chat)与 ListModels 同吃这一份,header 行为单测钉在这。
inline std::map<std::string, std::string> RequestBaseHeaders(const std::string& auth_token) {
    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!auth_token.empty()) {
        headers["Authorization"] = "Bearer " + auth_token;
    }
    return headers;
}

}  // namespace lubancode::api
