#pragma once

#include <map>
#include <string>
#include <vector>

#include "api/sse_framing.hpp"
#include "api/types.hpp"

namespace lubancode::api::chat {

// Chat Completions 的 tool_calls 参数会按 index 交错流出。这里先逐项攒齐，
// 到 [DONE] 再按次序吐中立工具事件，免得 MessageAssembler 串错参数。
//
// reasoning_delta_field:思考增量字段名的 provider 声明(见 ChatRequestOptions
// 同名字段)。空 = 自动兼容:reasoning_content(DeepSeek 系)与 reasoning
// (vLLM 0.27+/Qwen 系)两个只读别名都认;同一 chunk 两者都有时定优先级
// 去重,绝不吐两份(规则见 events.cpp)。声明了就只认那一个字段。
class EventParser {
public:
    explicit EventParser(std::string reasoning_delta_field = std::string())
        : reasoning_delta_field_(std::move(reasoning_delta_field)) {}

    std::vector<StreamEvent> Consume(const SseFrame& frame);
    std::vector<StreamEvent> Finish();
    bool finished() const { return finished_; }

    // 流里见过的结构化 reasoning_details 块数(OpenAI Responses 风格的
    // 结构化思考)。当前版本不映射成 ThinkingDelta——映射另定,先计数,
    // Finish 时打一行诊断,不静默吞掉。
    int reasoning_details_blocks() const { return reasoning_details_blocks_; }

private:
    struct ToolCall {
        std::string id;
        std::string name;
        std::string arguments;
    };

    std::map<int, ToolCall> tool_calls_;
    Usage usage_;
    std::string finish_reason_;
    std::string reasoning_delta_field_;
    bool started_ = false;
    bool saw_payload_ = false;
    bool finished_ = false;
    int reasoning_details_blocks_ = 0;
    bool reasoning_details_diagnostic_printed_ = false;
};

}  // namespace lubancode::api::chat
