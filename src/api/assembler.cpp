#include "api/assembler.hpp"

#include <type_traits>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "api/tool_args_repair.hpp"  // 入参兜底:宽松解析与截断/失败人话
#include "platform/text_encoding.hpp"  // SanitizeExternalText:模型输出进历史前的编码关口

namespace lubancode::api {

void MessageAssembler::FinalizeCurrent() {
    if (open_tool_.has_value()) {
        nlohmann::json input = nlohmann::json::object();
        const std::string& raw = open_tool_->partial_json;
        // 入参兜底(开源模型坏 JSON 单):先严格 parse,失败按序宽松修补;
        // 修不成就记病因,块照出(配对不断),入参给 {}、input_error 带人话,
        // loop 见了不执行、合成 is_error 结果回喂。服务端块不走修补(那是
        // provider 执行过的事实,本地不改写)。
        ToolArgsRepairResult parsed;
        if (open_tool_->is_server) {
            if (!raw.empty()) {
                try {
                    input = nlohmann::json::parse(raw);
                } catch (const nlohmann::json::parse_error& e) {
                    parse_error_ = "工具 " + open_tool_->name + " 的入参 JSON 解析失败: " + e.what();
                    input = nlohmann::json::object();
                }
            }
        } else {
            parsed = RepairToolArgs(raw);
            if (parsed.ok) {
                input = std::move(parsed.value);
            } else {
                parse_error_ = "工具 " + open_tool_->name + " 的入参 JSON 解析失败: " + parsed.error;
            }
        }
        if (open_tool_->is_server) {
            // 服务端工具搜索(动态工具 P3):攒成 provider 事实块,本地不执行。
            ServerToolUseBlock block;
            block.id = open_tool_->id;
            block.name = open_tool_->name;
            block.input = std::move(input);
            content_.push_back(std::move(block));
        } else if (open_tool_->id.empty()) {
            // 子代理空轨迹单 P0-F:provider 没给身份的 function call 不进
            // ToolUseBlock——不执行、不合成局部工具结果、不伪造 provider
            // call id(假 id 只会把故障推迟到下一次回喂)。计数留给消费端
            // 把这份输出按畸形收口。
            ++idless_tool_calls_dropped_;
            open_tool_.reset();
            return;
        } else {
            ToolUseBlock block{open_tool_->id, open_tool_->name, std::move(input), open_tool_->caller};
            block.async_call = open_tool_->async_call;
            if (!parsed.ok) {
                block.input_error = BuildToolArgsParseErrorText(open_tool_->name, parsed.error, raw);
                block.input_error_code = kErrToolInputInvalidJson;
            } else if (parsed.repaired) {
                for (const std::string& repair : parsed.repairs) {
                    block.input_repair += (block.input_repair.empty() ? "" : "、") + repair;
                }
            }
            // 截断嫌疑(修不成,或靠补齐末尾才救回):stop_reason 要到
            // MessageDone 才知道,先记下块位与原文,收口时再裁。
            if (!parsed.ok || parsed.closed_truncation) {
                truncation_suspects_.push_back(TruncationSuspect{content_.size(), raw});
            }
            // 流式提前档探针(异步工具单 P2 §7):单枚 call item 完整即报。
            // 只报严格一次过的——修补过的、坏掉的等整份消息收口再裁,不提前派发。
            if (parsed.ok && !parsed.repaired) {
                completed_tool_uses_.push_back(block);
            }
            content_.push_back(std::move(block));
        }
        open_tool_.reset();
    } else if (open_thinking_.has_value()) {
        // 模型流里的思考正文/签名可能带坏串(服务端或中转的问题),进历史
        // 前洗掉,免得下一轮重放时 wire 序列化 316。
        content_.push_back(ThinkingBlock{platform::SanitizeExternalText(open_thinking_->text),
                                         platform::SanitizeExternalText(open_thinking_->signature),
                                         open_thinking_->responses_item});
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
                    FinalizeCurrent();
                    open_thinking_ = OpenThinking{};
                }
                open_thinking_->text += e.text;
                open_thinking_->signature += e.signature;
                if (e.responses_item.is_object()) {
                    open_thinking_->responses_item = e.responses_item;
                    // done 条目含完整正文;没有 delta 的端也能留全思考。
                    std::string text;
                    const auto& item = e.responses_item;
                    const auto content = item.find("content");
                    const auto summary = item.find("summary");
                    const nlohmann::json* parts = content != item.end() && content->is_array() && !content->empty()
                        ? &*content : summary != item.end() && summary->is_array() ? &*summary : nullptr;
                    if (parts != nullptr && !parts->empty()) {
                        for (const auto& part : *parts) {
                            if (part.is_object() && part.contains("text") && part["text"].is_string())
                                text += part["text"].get<std::string>();
                        }
                        open_thinking_->text = std::move(text);
                    }
                    FinalizeCurrent();
                }
            } else if constexpr (std::is_same_v<T, ToolUseStart>) {
                FinalizeCurrent();  // 上一个块(多半是文本)先收尾
                OpenToolUse open;
                open.index = e.index;  // 记下 output 下标:终帧按它合并身份(P0-F)
                open.id = e.id;
                open.name = e.name;
                open.caller = e.caller;
                open.async_call = e.async_call;
                open_tool_ = std::move(open);
            } else if constexpr (std::is_same_v<T, ToolUseInputDelta>) {
                if (open_tool_.has_value()) {
                    open_tool_->partial_json += e.partial_json;
                }
                // 没有正在累积的 tool_use 块却来了输入片段:协议乱了,静默丢弃,不崩。
            } else if constexpr (std::is_same_v<T, ContentBlockDone>) {
                // P0-F:早帧缺 id、终帧补 id 的流式协议(Responses 兼容端实测
                // 形状)按 output index 合并终帧身份,再收块。
                if (open_tool_.has_value() && open_tool_->index == e.index &&
                    open_tool_->id.empty() && !e.tool_use_id.empty()) {
                    open_tool_->id = e.tool_use_id;
                }
                FinalizeCurrent();
            } else if constexpr (std::is_same_v<T, ServerToolUseStart>) {
                // 服务端工具搜索(动态工具 P3):开服务端累积器,后续的
                // ToolUseInputDelta 按 index 归它——assembler 本就只持一只开着的
                // tool 累积器(块序即流序),不必再按 index 分桶。
                FinalizeCurrent();
                OpenToolUse open;
                open.index = e.index;
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
            } else if constexpr (std::is_same_v<T, RedactedThinking>) {
                // 加密思考块整块到齐(轨迹 v3 差距清单 §8.2 第 6 条):前一块
                // 先收尾,不透明载荷洗过编码关直接落历史——块序即流序,与
                // thinking/text 块同位保序,下一轮原样回传。
                FinalizeCurrent();
                content_.push_back(
                    RedactedThinkingBlock{platform::SanitizeExternalText(e.data)});
            } else if constexpr (std::is_same_v<T, MessageDone>) {
                FinalizeCurrent();  // 防御性收尾:正常流程里 ContentBlockDone 应该已经收过了
                stop_reason_ = e.stop_reason;
                usage_ = e.usage;
                usage_seen_ = e.usage_reported;  // 显式位:wire 见没见过 usage 帧
                // 读/写明报位各自一枚(C2):只报写入不能证明读取为零。
                cache_read_seen_ = e.cache_read_reported;
                cache_creation_seen_ = e.cache_creation_reported;
                usage_anomaly_ = e.usage_anomaly;
                // 长度截断(max_tokens/length)且入参不完整:绝不补齐执行
                //(写半截文件是灾难),按解析失败处理,病因点明截断。
                if (IsLengthStopReason(stop_reason_)) {
                    for (const TruncationSuspect& suspect : truncation_suspects_) {
                        if (suspect.content_index >= content_.size()) {
                            continue;
                        }
                        auto* block = std::get_if<ToolUseBlock>(&content_[suspect.content_index]);
                        if (block == nullptr) {
                            continue;
                        }
                        block->input = nlohmann::json::object();
                        block->input_repair.clear();
                        block->input_error = BuildToolArgsTruncatedText(block->name, suspect.raw);
                        block->input_error_code = kErrToolInputTruncated;
                        parse_error_ = "工具 " + block->name + " 的入参因输出长度截断而不完整";
                    }
                }
                truncation_suspects_.clear();
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
