// 中立层:把 Backend 吐出来的 StreamEvent 流,攒成一条完整的 assistant Message。
// StreamEvent 本身已经是与厂商无关的中立类型,这一层不关心事件是从 Anthropic
// 还是 Responses 翻译过来的——两个后端将来共用这一层。

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "api/types.hpp"

namespace lubancode::api {

class MessageAssembler {
public:
    // 喂一个事件:
    //   TextDelta         —— 拼进当前正在累积的文本块
    //   ToolUseStart      —— 上一个块(多半是文本)先收尾,开一个新的 tool_use 块
    //   ToolUseInputDelta —— 拼进当前 tool_use 块的 input JSON 残片
    //   ContentBlockDone  —— 当前块收尾,追加进 content_
    //   MessageDone       —— 顺带把还没收尾的块也收个尾,再记下 stop_reason/usage
    //   MessageStart / StreamError —— 不影响攒出来的内容,静默忽略(StreamError
    //   如果调用方关心,自己在收到事件的时候另外处理,不劳这一层代传)
    void Feed(const StreamEvent& event);

    // 攒好的完整 assistant 消息,content 按内容块产生的先后顺序排列。
    Message BuildMessage() const;

    // 强制给还开着的块(文本或 tool_use)收尾,不等 ContentBlockDone/
    // MessageDone 事件到来。ESC 打断流式传输那种"半截话"场景专用:流被
    // cpr 从中间掐断,后面的收尾事件永远不会来了,调用方得手动催一下,
    // 不然半截文本会被 BuildMessage() 悄悄漏掉。正常收尾路径
    // (ContentBlockDone/MessageDone)不受影响,FinalizeCurrent() 本身
    // 对"没有任何开着的块"这种情况是空操作,重复调用安全。
    void FinalizeOpenBlock() { FinalizeCurrent(); }

    const std::string& stop_reason() const { return stop_reason_; }
    const Usage& usage() const { return usage_; }

    // tool_use 的 input JSON 拼完后解析失败时置位。就算解析失败,BuildMessage()
    // 依旧会给出可用的 Message——那个 tool_use 块的 input 会是个空对象,不会因为
    // 解析失败就丢数据、崩掉;调用方看 has_parse_error() 自己决定要不要额外报错。
    bool has_parse_error() const { return !parse_error_.empty(); }
    const std::string& parse_error() const { return parse_error_; }

private:
    struct OpenText {
        std::string text;
    };
    struct OpenToolUse {
        std::string id;
        std::string name;
        std::string partial_json;
    };

    std::vector<ContentBlock> content_;
    std::optional<OpenText> open_text_;
    std::optional<OpenToolUse> open_tool_;
    std::string stop_reason_;
    Usage usage_;
    std::string parse_error_;

    void FinalizeCurrent();
};

}  // namespace lubancode::api
