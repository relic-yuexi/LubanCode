#include "api/assembler.hpp"

#include <type_traits>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace lubancode::api {

void MessageAssembler::FinalizeCurrent() {
    if (open_tool_.has_value()) {
        nlohmann::json input = nlohmann::json::object();
        const std::string& raw = open_tool_->partial_json;
        if (!raw.empty()) {
            try {
                input = nlohmann::json::parse(raw);
            } catch (const nlohmann::json::parse_error& e) {
                parse_error_ = "工具 " + open_tool_->name + " 的入参 JSON 解析失败: " + e.what();
                input = nlohmann::json::object();
            }
        }
        content_.push_back(ToolUseBlock{open_tool_->id, open_tool_->name, std::move(input)});
        open_tool_.reset();
    } else if (open_text_.has_value()) {
        content_.push_back(TextBlock{std::move(open_text_->text)});
        open_text_.reset();
    }
}

void MessageAssembler::Feed(const StreamEvent& event) {
    std::visit(
        [this](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, TextDelta>) {
                if (!open_text_.has_value()) {
                    open_text_ = OpenText{};
                }
                open_text_->text += e.text;
            } else if constexpr (std::is_same_v<T, ToolUseStart>) {
                FinalizeCurrent();  // 上一个块(多半是文本)先收尾
                open_tool_ = OpenToolUse{e.id, e.name, std::string{}};
            } else if constexpr (std::is_same_v<T, ToolUseInputDelta>) {
                if (open_tool_.has_value()) {
                    open_tool_->partial_json += e.partial_json;
                }
                // 没有正在累积的 tool_use 块却来了输入片段:协议乱了,静默丢弃,不崩。
            } else if constexpr (std::is_same_v<T, ContentBlockDone>) {
                FinalizeCurrent();
            } else if constexpr (std::is_same_v<T, MessageDone>) {
                FinalizeCurrent();  // 防御性收尾:正常流程里 ContentBlockDone 应该已经收过了
                stop_reason_ = e.stop_reason;
                usage_ = e.usage;
            }
            // MessageStart / StreamError:不影响攒出来的内容。
        },
        event);
}

Message MessageAssembler::BuildMessage() const {
    Message message;
    message.role = Role::Assistant;
    message.content = content_;
    return message;
}

}  // namespace lubancode::api
