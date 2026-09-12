// 语义层:把 SseFrame(SSE 分帧器吐出来的原始帧)翻译成中立的 StreamEvent。
// 只认得 Anthropic Messages API(MiniMax 兼容端点)的事件字段,认不得的
// 事件类型 / 内容块类型 / delta 类型一律静默跳过,不抛异常、不崩。

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "api/sse_framing.hpp"
#include "api/types.hpp"

namespace lubancode::api::anthropic {

// 解析一帧 SSE 数据。判定事件种类靠 data 里的 "type" 字段(不看 SSE 的
// event: 字段名——真机上两者应该一致,但只信一份权威来源更稳)。
//
// parse_server_tool_search(动态工具 P3):真时解析服务端工具搜索的原生块
// (server_tool_use / tool_search_tool_result),假时照旧跳过——只有本请求
// 声明过 server_tool_search 的流才该传真。
//
// 返回 std::nullopt 表示这一帧不需要往上抛任何 StreamEvent,可能是:
//   - JSON 解析失败(数据坏了,跳过而不是崩溃);
//   - 认得种类但语义上不需要单独发事件(比如 ping、text 内容块的起始);
//   - 完全没见过的事件类型。
std::optional<StreamEvent> parse_event(const SseFrame& frame, bool parse_server_tool_search = false);

// parse_event 的共用主体:对已 parse 好的 json 树做无状态翻译(判定逻辑
// 与 parse_event 完全一份)。EventParser::Consume 吸收完 usage 快照后走这
// 里,免得同一帧 parse 两遍。
std::optional<StreamEvent> parse_event_json(const nlohmann::json& data, bool parse_server_tool_search = false);

// 少数 Messages 兼容端会在工具续轮里把本该是 thinking_delta 的内容包成
// `<think>...</think>`，却仍塞进 text_delta。EventParser 只在调用方明确
// 开门时修这一个形状；普通响应照旧走 parse_event，不会把用户要求输出的
// XML/代码标签一概删掉。
//
// parse_server_tool_search(动态工具 P3):开门解析服务端工具搜索的原生块
// (server_tool_use / tool_search_tool_result)。这枚门只对本请求自己声明过
// server_tool_search 的流开——没声明的请求里冒出来的 server 块(兼容端的
// 杂音、别的 server tool)照旧行为:静默跳过,一块不解析。
//
// usage 快照(C1,缓存用量按 Wire 归一单):官方流式形状允许 message_start
// 就报齐输入侧 usage、末尾 message_delta 只报 output——无状态翻译会把开头
// 那份丢掉。EventParser 在有状态这一层跨帧合并:message_start 立快照(新
// 响应开始,先清旧账不串上次数字),后续 usage 帧只覆盖实际出现的字段,
// 缺字段不清零、显式零覆盖旧值;MessageDone 出口带合并后的账与三位明报
// 旗标。不逐帧相加(官方的 output_tokens 是累计值,直接取末值)。
class EventParser {
public:
    explicit EventParser(bool recover_tagged_thinking = false, bool parse_server_tool_search = false)
        : recover_tagged_thinking_(recover_tagged_thinking),
          parse_server_tool_search_(parse_server_tool_search) {}

    std::vector<StreamEvent> Consume(const SseFrame& frame);
    std::vector<StreamEvent> Finish();
    bool recovered_tagged_thinking() const { return recovered_tagged_thinking_; }

private:
    enum class TaggedThinkingState { Probe, Passthrough, Thinking, AwaitingAnswer, AfterThinking, Failed };

    // usage 快照:每字段独立记"出现过没有"——nullopt = 这条流还没报过它,
    // 合并出口上与"明报零"分家(旗标另记)。
    struct UsageSnapshot {
        std::optional<std::int64_t> input_tokens;
        std::optional<std::int64_t> output_tokens;
        std::optional<std::int64_t> cache_read;
        std::optional<std::int64_t> cache_creation;
    };

    std::vector<StreamEvent> ConsumeParsed(StreamEvent event);
    std::vector<StreamEvent> ConsumeText(std::string text);
    std::vector<StreamEvent> CloseOpenProbe();
    // 吸收一帧 usage 对象进快照(字段级覆盖);message_start 到来时先调
    // ResetUsageState 清旧账。
    void AbsorbUsageObject(const nlohmann::json& usage);
    void ResetUsageState();

    bool recover_tagged_thinking_ = false;
    bool parse_server_tool_search_ = false;
    bool recovered_tagged_thinking_ = false;
    TaggedThinkingState tagged_state_ = TaggedThinkingState::Probe;
    std::string pending_;
    // ---- usage 快照账(C1) ----
    UsageSnapshot usage_snapshot_;
    bool usage_seen_ = false;           // 任一帧真出现过 usage 对象(明报全零也算)
    bool cache_read_seen_ = false;      // cache_read_input_tokens 字段出现过
    bool cache_creation_seen_ = false;  // cache_creation_input_tokens 字段出现过
    std::string usage_anomaly_;         // 负数一类自相矛盾的账(空 = 自洽)
};

}  // namespace lubancode::api::anthropic
