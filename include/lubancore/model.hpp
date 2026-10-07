#pragma once

#include <lubancore/api.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lubancore {

struct ToolCall { std::string id; std::string name; std::string input_json; };
struct ToolReply { std::string call_id; std::string text; bool is_error = false; };
struct Message {
    std::string role;
    std::string text;
    std::vector<ToolCall> tool_calls;
    std::vector<ToolReply> tool_replies;
};
struct ToolDefinition { std::string name; std::string description; std::string input_schema_json; };
struct ModelRequest {
    std::string model;
    std::string system;
    std::vector<Message> messages;
    std::vector<ToolDefinition> tools;
    std::optional<int> max_output_tokens;
};
struct Usage { std::int64_t input_tokens = 0; std::int64_t output_tokens = 0; };
struct ModelReply {
    std::string text;
    std::vector<ToolCall> tool_calls;
    std::optional<Usage> usage;
};

// Text/tool-call injection surface, useful for an embedded provider or fixture.
// Unsupported rich history is rejected explicitly, never silently flattened.
class Backend {
public:
    virtual ~Backend() = default;
    virtual Result<ModelReply> Generate(const ModelRequest&, Cancellation) = 0;
};

enum class Wire { Anthropic, ChatCompletions, Responses, Gemini };
struct Connection {
    Wire wire = Wire::ChatCompletions;
    std::string base_url;
    std::string api_key;
    int connect_timeout_ms = 10000;
    int idle_timeout_seconds = 60;
    int request_timeout_seconds = 300;
};
} // namespace lubancore
