// v3 历史时间线 → 显示 DTO 实现(P3 显示侧第一棒)。纯读:不开写柄、
// 不调模型、不重跑工具(§5.1"只读 replay 零调用零重跑")。
#include "runtime/trajectory_history_view.hpp"

#include <map>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancode::runtime {

namespace {

// 消息本体正文(text 块;字符串或 blocks 数组两形)。
void AppendTextBlocks(const nlohmann::json& content, api::Message* out) {
    if (content.is_string()) {
        if (!content.get<std::string>().empty()) {
            out->content.push_back(api::TextBlock{content.get<std::string>()});
        }
        return;
    }
    if (!content.is_array()) {
        return;
    }
    for (const auto& part : content) {
        if (part.is_object() && part.value("type", std::string()) == "text" &&
            part.contains("text") && part["text"].is_string()) {
            out->content.push_back(api::TextBlock{part["text"].get<std::string>()});
        }
    }
}

// provider 调用号 → actionId(§4.15 全局调用身份):显示层的工具配对键
// 用 actionId,与 tool 消息的 tool_call_id 同键。
std::map<std::string, std::string> ProviderCallToAction(
    const std::vector<trajectory::v3::ToolActionSnapshot>& snapshots) {
    std::map<std::string, std::string> mapping;
    for (const auto& action : snapshots) {
        if (action.provider_tool_call_id.has_value() && !action.provider_tool_call_id->empty()) {
            mapping[*action.provider_tool_call_id] = action.tool_call_id;
        }
    }
    return mapping;
}

// 一次调用的折叠终态是否失败(显示卡的红绿灯)。
bool ActionFailed(const std::vector<trajectory::v3::ToolActionSnapshot>& snapshots,
                  const std::string& action_id) {
    for (const auto& action : snapshots) {
        if (action.tool_call_id == action_id) {
            return action.effective_outcome == "failed";
        }
    }
    return false;
}

}  // namespace

RestoredHistoryView ProjectRestoredHistory(const std::filesystem::path& v3_jsonl) {
    RestoredHistoryView view;
    view.source_jsonl = platform::PathToUtf8(v3_jsonl);
    auto ledger = trajectory::v3::ReadV3Ledger(v3_jsonl);
    if (!ledger.has_value()) {
        return view;  // 验卷不过:空 view,调用方按"无可显示旧史"处理
    }
    view.session_id = ledger->session_id;
    const trajectory::v3::HistoryTimeline timeline = trajectory::v3::ProjectHistoryTimeline(*ledger);
    const std::vector<trajectory::v3::ToolActionSnapshot> snapshots =
        trajectory::v3::FoldToolActions(*ledger);
    const std::map<std::string, std::string> call_to_action = ProviderCallToAction(snapshots);

    for (const auto& item : timeline.items) {
        RestoredHistoryItem out;
        out.seq = item.seq;
        out.timestamp = item.timestamp;
        if (item.kind == trajectory::v3::HistoryTimeline::Item::Kind::CompactMarker) {
            out.kind = RestoredHistoryItem::Kind::Compact;
            out.compact.compact_id = item.compact.compact_id;
            out.compact.event_id = item.compact.event_id;
            out.compact.timestamp = item.compact.timestamp;
            out.compact.context_tokens_before = item.compact.context_tokens_before;
            out.compact.context_tokens_after = item.compact.context_tokens_after;
            out.compact.removed_message_refs = item.compact.removed_message_refs;
            out.compact.retained_message_refs = item.compact.retained_message_refs;
            view.items.push_back(std::move(out));
            continue;
        }
        if (item.kind != trajectory::v3::HistoryTimeline::Item::Kind::Message) {
            continue;  // system 切换/降档标记/其余事件:详情档后续棒,不进首版时间线
        }
        const auto line = ledger->FindMessage(item.id);
        if (line == nullptr) {
            continue;
        }
        RestoredMessageView& message = out.message;
        message.message_id = item.message.message_id;
        message.timestamp = item.message.timestamp;
        message.in_current_context = item.message.in_current_context;
        message.replaced_by_derivation = item.message.replaced_by_derivation;
        message.removed_by_compacts = item.message.removed_by_compacts;
        message.hidden = item.message.display == trajectory::v3::DisplayMode::Hidden;

        const std::string role =
            line->message.value("role", std::string("user"));
        api::Message& projected = message.message;
        if (role == "system") {
            continue;  // 上下文根不是会话正文,不进显示 DTO
        }
        if (role == "assistant") {
            projected.role = api::Role::Assistant;
            if (line->message.contains("content")) {
                AppendTextBlocks(line->message["content"], &projected);
            }
            if (line->message.contains("tool_calls") && line->message["tool_calls"].is_array()) {
                for (const auto& call : line->message["tool_calls"]) {
                    if (!call.is_object()) {
                        continue;
                    }
                    api::ToolUseBlock use;
                    const std::string provider_id = call.value("id", std::string());
                    const auto mapped = call_to_action.find(provider_id);
                    use.id = mapped != call_to_action.end() ? mapped->second : provider_id;
                    if (call.contains("function") && call["function"].is_object()) {
                        use.name = call["function"].value("name", std::string());
                        nlohmann::json arguments =
                            nlohmann::json::parse(call["function"].value("arguments", std::string("{}")),
                                                  nullptr, /*allow_exceptions=*/false);
                        use.input = arguments.is_discarded() ? nlohmann::json::object() : arguments;
                    }
                    projected.content.push_back(std::move(use));
                }
            }
        } else if (role == "tool") {
            // 工具结果以 user 消息携带(与 v2 投影/hub 回喂同形)。
            projected.role = api::Role::User;
            api::ToolResultBlock result;
            result.tool_use_id = line->message.value("tool_call_id", std::string());
            if (line->message.contains("content") && line->message["content"].is_string()) {
                result.content = line->message["content"].get<std::string>();
            }
            result.is_error = ActionFailed(snapshots, result.tool_use_id);
            projected.content.push_back(std::move(result));
        } else {
            projected.role = api::Role::User;
            if (line->message.contains("content")) {
                AppendTextBlocks(line->message["content"], &projected);
            }
        }
        out.kind = RestoredHistoryItem::Kind::Message;
        view.items.push_back(std::move(out));
    }
    return view;
}

}  // namespace lubancode::runtime
