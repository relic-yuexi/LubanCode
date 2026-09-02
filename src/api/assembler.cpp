#include "api/assembler.hpp"

#include <type_traits>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "platform/text_encoding.hpp"  // SanitizeExternalText:模型输出进历史前的编码关口

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
        if (open_tool_->is_server) {
            // 服务端工具搜索(动态工具 P3):攒成 provider 事实块,本地不执行。
            ServerToolUseBlock block;
            block.id = open_tool_->id;
            block.name = open_tool_->name;
            block.input = std::move(input);
            content_.push_back(std::move(block));
        } else {
            content_.push_back(
                ToolUseBlock{open_tool_->id, open_tool_->name, std::move(input), open_tool_->caller});
        }
        open_tool_.reset();
    } else if (open_thinking_.has_value()) {
        // 模型流里的思考正文/签名可能带坏串(服务端或中转的问题),进历史
        // 前洗掉,免得下一轮重放时 wire 序列化 316。
        content_.push_back(ThinkingBlock{platform::SanitizeExternalText(open_thinking_->text),
                                         platform::SanitizeExternalText(open_thinking_->signature)});
        open_thinking_.reset();
    } else if (open_text_.has_value()) {
        // 同上:模型输出的文本块进历史前清洗。
        content_.push_back(TextBlock{platform::SanitizeExternalText(open_text_->text)});
        open_text_.reset();
    }
}

void MessageAssembler::Feed(const StreamEvent& event) {
    std::visit(
        [this](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, TextDelta>) {
                // chat wire 的 reasoning_content → content 过渡没有
                // ContentBlockDone,这里手动收掉开着的思考块。
                if (open_thinking_.has_value()) {
                    FinalizeCurrent();
                }
                if (!open_text_.has_value()) {
                    open_text_ = OpenText{};
                }
                open_text_->text += e.text;
            } else if constexpr (std::is_same_v<T, ThinkingDelta>) {
                if (!open_thinking_.has_value()) {
                    open_thinking_ = OpenThinking{};
                }
                open_thinking_->text += e.text;
                open_thinking_->signature += e.signature;
            } else if constexpr (std::is_same_v<T, ToolUseStart>) {
                FinalizeCurrent();  // 上一个块(多半是文本)先收尾
                open_tool_ = OpenToolUse{e.id, e.name, std::string{}, e.caller, /*is_server=*/false};
            } else if constexpr (std::is_same_v<T, ToolUseInputDelta>) {
                if (open_tool_.has_value()) {
                    open_tool_->partial_json += e.partial_json;
                }
                // 没有正在累积的 tool_use 块却来了输入片段:协议乱了,静默丢弃,不崩。
            } else if constexpr (std::is_same_v<T, ContentBlockDone>) {
                FinalizeCurrent();
            } else if constexpr (std::is_same_v<T, ServerToolUseStart>) {
                // 服务端工具搜索(动态工具 P3):开服务端累积器,后续的
                // ToolUseInputDelta 按 index 归它——assembler 本就只持一只开着的
                // tool 累积器(块序即流序),不必再按 index 分桶。
                FinalizeCurrent();
                OpenToolUse open;
                open.id = e.id;
                open.name = e.name;
                open.is_server = true;
                open_tool_ = std::move(open);
            } else if constexpr (std::is_same_v<T, ServerToolResult>) {
                // 搜索结果整块到齐(官方流里随 content_block_start 一次给完,
                // 没有增量):前一块先收尾,然后直接落事实块。
                FinalizeCurrent();
                ServerToolResultBlock block;
                block.tool_use_id = e.tool_use_id;
                block.content = e.content;
                content_.push_back(std::move(block));
            } else if constexpr (std::is_same_v<T, MessageDone>) {
                FinalizeCurrent();  // 防御性收尾:正常流程里 ContentBlockDone 应该已经收过了
                stop_reason_ = e.stop_reason;
                usage_ = e.usage;
                usage_seen_ = e.usage_reported;  // 显式位:wire 见没见过 usage 帧
                cache_seen_ = e.cache_reported;
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
