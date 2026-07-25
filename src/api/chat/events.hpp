#pragma once

#include <map>
#include <string>
#include <vector>

#include "api/sse_framing.hpp"
#include "api/types.hpp"

namespace lubancode::api::chat {

// Chat Completions 的 tool_calls 参数会按 index 交错流出。这里先逐项攒齐，
// 到 [DONE] 再按次序吐中立工具事件，免得 MessageAssembler 串错参数。
class EventParser {
public:
    std::vector<StreamEvent> Consume(const SseFrame& frame);
    std::vector<StreamEvent> Finish();
    bool finished() const { return finished_; }

private:
    struct ToolCall {
        std::string id;
        std::string name;
        std::string arguments;
    };

    std::map<int, ToolCall> tool_calls_;
    Usage usage_;
    std::string finish_reason_;
    bool started_ = false;
    bool saw_payload_ = false;
    bool finished_ = false;
};

}  // namespace lubancode::api::chat
