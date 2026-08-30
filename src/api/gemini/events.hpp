// Google Gemini 原生 Generate Content API(v1beta)的流式事件翻译层:
// 把 SSE 帧里的 GenerateContentResponse JSON 翻成中立 StreamEvent。
// 只认得 Gemini 的字段,认不得的静默跳过——坏帧不崩流(解析兜底在
// events.cpp 尾部的 catch,理由同 responses/events.cpp)。

#pragma once

#include <string>
#include <vector>

#include "api/sse_framing.hpp"
#include "api/types.hpp"

namespace lubancode::api::gemini {

// Gemini 的 streamGenerateContent?alt=sse 每帧 data: 都是一只完整的
// GenerateContentResponse(没有 event: 名字段,SseFramer 照通用 SSE 规则
// 当 "message" 处理,这里只看 data 不看 event 名,与 chat wire 同一副
// 吃法)。文本/思考增量到帧就吐;functionCall part 是整只到达的(不像
// chat 的 tool_calls 会按 index 流式拼参数),先按到达次序攒着,等
// finishReason 到了(或 Finish())再一口气按次序吐中立工具事件,免得
// MessageAssembler 串错参数——套路与 chat 的 EventParser 一致。
class EventParser {
public:
    std::vector<StreamEvent> Consume(const SseFrame& frame);
    std::vector<StreamEvent> Finish();
    bool finished() const { return finished_; }

private:
    struct PendingCall {
        std::string name;
        nlohmann::json args;
    };

    // 攒齐的 Finish() 落锤:工具事件 + MessageDone 一并吐出。
    std::vector<StreamEvent> Flush();

    std::vector<PendingCall> calls_;
    Usage usage_;
    bool usage_reported_ = false;  // 流里真见过 usageMetadata(Token 账本单 A0)
    std::string finish_reason_;
    std::string model_;
    bool started_ = false;
    bool saw_payload_ = false;
    bool finished_ = false;
};

}  // namespace lubancode::api::gemini
