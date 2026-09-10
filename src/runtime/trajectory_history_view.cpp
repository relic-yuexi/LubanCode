// v3 历史时间线 → 显示 DTO 实现(P3 显示侧第一棒)。纯读:不开写柄、
// 不调模型、不重跑工具(§5.1"只读 replay 零调用零重跑")。
#include "runtime/trajectory_history_view.hpp"

#include <map>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"

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

// 一条消息行的完整来源键(链合流去重):跨场抄本的 sourceMessageRef
// 已是 "<场>/<msg>";原生行用本场场名拼同形键。返回空串 = 无从取键。
std::string SourceKeyOf(const trajectory::v3::V3Ledger& ledger,
                        const trajectory::v3::MessageLine& line) {
    if (line.source_message_ref.has_value()) {
        const std::string& ref = *line.source_message_ref;
        const std::size_t slash = ref.find('/');
        if (slash != std::string::npos && slash > 0 &&
            ref.compare(0, slash, ledger.session_id) != 0) {
            return ref;  // 跨场抄本
        }
    }
    return ledger.session_id + "/" + line.message_id;
}

// 一场账 → 显示 DTO 项(时间线原序)。keys 与 items 逐位对齐(压缩
// 标记给空键,不去重);抄本(键带来源场名)的调用配对键原样沿用,
// 原生行走 provider→action 映射。
struct LedgerViewItems {
    std::vector<RestoredHistoryItem> items;
    std::vector<std::string> keys;
};

LedgerViewItems BuildLedgerViewItems(const trajectory::v3::V3Ledger& ledger) {
    LedgerViewItems result;
    const trajectory::v3::HistoryTimeline timeline = trajectory::v3::ProjectHistoryTimeline(ledger);
    const std::vector<trajectory::v3::ToolActionSnapshot> snapshots =
        trajectory::v3::FoldToolActions(ledger);
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
            result.items.push_back(std::move(out));
            result.keys.emplace_back();
            continue;
        }
        if (item.kind != trajectory::v3::HistoryTimeline::Item::Kind::Message) {
            continue;  // system 切换/降档标记/其余事件:详情档后续棒,不进首版时间线
        }
        const auto line = ledger.FindMessage(item.id);
        if (line == nullptr) {
            continue;
        }
        const std::string role = line->message.value("role", std::string("user"));
        if (role == "system") {
            continue;  // 上下文根不是会话正文,不进显示 DTO
        }
        RestoredMessageView& message = out.message;
        message.message_id = item.message.message_id;
        message.timestamp = item.message.timestamp;
        message.in_current_context = item.message.in_current_context;
        message.replaced_by_derivation = item.message.replaced_by_derivation;
        message.removed_by_compacts = item.message.removed_by_compacts;
        message.hidden = item.message.display == trajectory::v3::DisplayMode::Hidden;

        // 跨场抄本:调用键已带来源场名(在新账自配对),不走映射。
        const std::string source_key = SourceKeyOf(ledger, *line);
        const bool is_import_copy = line->source_message_ref.has_value() &&
                                    *line->source_message_ref == source_key;
        api::Message& projected = message.message;
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
                    if (is_import_copy) {
                        use.id = provider_id;
                    } else {
                        const auto mapped = call_to_action.find(provider_id);
                        use.id = mapped != call_to_action.end() ? mapped->second : provider_id;
                    }
                    if (call.contains("function") && call["function"].is_object()) {
                        use.name = call["function"].value("name", std::string());
                        nlohmann::json arguments = nlohmann::json::parse(
                            call["function"].value("arguments", std::string("{}")), nullptr,
                            /*allow_exceptions=*/false);
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
        result.items.push_back(std::move(out));
        result.keys.push_back(source_key);
    }
    return result;
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
    // 来源链祖先(§4.10 第 3 条:沿 resume.source.attached 读取,链护栏
    // 归 ProjectResume——防环 + 深度封顶)。最老祖先在前;某级账读不动
    // 只跳过该段(显示侧允许"仍可验证的历史 + 缺口"),不拦其余。
    std::vector<const trajectory::v3::V3Ledger*> ancestors;  // 老 → 新
    auto projection = trajectory::v3::ProjectResume(v3_jsonl);
    if (projection.has_value()) {
        for (auto step = projection->source_chain.rbegin();
             step != projection->source_chain.rend(); ++step) {
            if (step->ledger.has_value()) {
                ancestors.push_back(&*step->ledger);
                view.source_sessions.push_back(step->ledger->session_id);
            }
        }
    }
    view.source_sessions.push_back(ledger->session_id);
    // 合流:祖先段(老 → 新)在前,直接源段在后;完整来源键去重——
    // 祖先原装先到先得,后代账上的链史抄本让位;祖先缺失时抄本顶上,
    // 不因缺源少画。seq 各段各自有效,不跨文件混排(§4.10)。
    std::set<std::string> seen;
    const auto merge = [&](const trajectory::v3::V3Ledger& segment) {
        LedgerViewItems built = BuildLedgerViewItems(segment);
        for (std::size_t i = 0; i < built.items.size(); ++i) {
            if (!built.keys[i].empty() && !seen.insert(built.keys[i]).second) {
                continue;  // 同一条史已画(祖先原装),不重复显示
            }
            view.items.push_back(std::move(built.items[i]));
        }
    };
    for (const trajectory::v3::V3Ledger* ancestor : ancestors) {
        merge(*ancestor);
    }
    merge(*ledger);
    return view;
}

std::optional<std::filesystem::path> FindV3HistoryStream(const std::filesystem::path& session_dir) {
    return trajectory::v3::FindV3SessionStream(session_dir);
}

V3ExportProjection ProjectExportMessages(const RestoredHistoryView& view) {
    V3ExportProjection projection;
    for (const auto& item : view.items) {
        if (item.kind == RestoredHistoryItem::Kind::Compact) {
            // 压缩标记折成分界位:插在已收的第 N 条消息之前(ExportSessionMarkdown
            // 的既有 compact 文案口径)。标记插在 applied 发生位,不挪位伪造顺序。
            projection.compact_positions.push_back(projection.messages.size());
            continue;
        }
        if (item.message.hidden) {
            continue;  // display.hidden 默认不导(§4.28);详情/开关档另算
        }
        if (projection.started_at.empty()) {
            projection.started_at = item.timestamp;
        }
        projection.messages.push_back(item.message.message);
    }
    return projection;
}

// ---------------------------------------------------------------------------
// 转录摘要行 + seq 游标分页(P3 第二棒)
// ---------------------------------------------------------------------------

namespace {

// token 数加千位逗号(§4.11 示意口径:"148,200")。
std::string FormatTranscriptTokenCount(std::uint64_t value) {
    std::string digits = std::to_string(value);
    std::string grouped;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        const std::size_t from_end = digits.size() - i;
        grouped += digits[i];
        if (from_end > 1 && (from_end - 1) % 3 == 0) {
            grouped += ',';
        }
    }
    return grouped;
}

// 一条消息正文的第一行(转录摘要一行只摆一行,长文进详情档)。
std::string FirstLineOf(const std::string& text) {
    const std::size_t newline = text.find('\n');
    return newline == std::string::npos ? text : text.substr(0, newline);
}

}  // namespace

std::vector<RestoredTranscriptLine> RenderRestoredTranscriptLines(const RestoredHistoryView& view) {
    std::vector<RestoredTranscriptLine> lines;
    for (const auto& item : view.items) {
        if (item.kind == RestoredHistoryItem::Kind::Compact) {
            std::string text = "  ◆ 上下文已压缩";
            if (item.compact.context_tokens_before > 0 || item.compact.context_tokens_after > 0) {
                text += ":前 ~" + FormatTranscriptTokenCount(item.compact.context_tokens_before) +
                        " → 后 ~" + FormatTranscriptTokenCount(item.compact.context_tokens_after) +
                        " tokens";
            }
            lines.push_back(RestoredTranscriptLine{item.seq, std::move(text)});
            continue;
        }
        // hidden 默认不渲染(§4.28:隐藏不等于删除,行表里没有这行)。
        if (item.message.hidden) {
            continue;
        }
        const api::Message& message = item.message.message;
        std::string role;
        std::string body;
        bool only_tool_result = true;
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                only_tool_result = false;
                if (body.empty()) {
                    body = FirstLineOf(text->text);
                }
            } else if (const auto* use = std::get_if<api::ToolUseBlock>(&block)) {
                only_tool_result = false;
                if (body.empty()) {
                    body = "[工具] " + use->name;
                }
            } else if (const auto* result = std::get_if<api::ToolResultBlock>(&block);
                       result != nullptr && body.empty()) {
                body = FirstLineOf(result->content);
            }
        }
        role = message.role == api::Role::Assistant
                   ? "assistant"
                   : (only_tool_result ? "tool" : "user");
        std::string text = "  " + role + " · " + body;
        // 上下文状态注脚(§1.3"哪段已压缩、当前模型还能看哪段,界面要
        // 分得清";§4.38 降档退链的原版另注)。
        if (!item.message.removed_by_compacts.empty()) {
            text += " · 已压缩";
        }
        if (item.message.replaced_by_derivation) {
            text += " · 已降档";
        }
        lines.push_back(RestoredTranscriptLine{item.seq, std::move(text)});
    }
    return lines;
}

RestoredTranscriptPage SliceRestoredTranscript(const std::vector<RestoredTranscriptLine>& lines,
                                                const std::optional<std::uint64_t>& before_seq,
                                                const std::optional<std::uint64_t>& after_seq,
                                                std::size_t max_lines) {
    // 按游标定候选区间 [begin, end)(旧→新;游标一次只给一枚,两枚同给
    // 不在合同里)。
    std::size_t begin = 0;
    std::size_t end = lines.size();
    if (before_seq.has_value()) {
        end = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].seq < *before_seq) {
                end = i + 1;
            }
        }
    }
    if (after_seq.has_value()) {
        begin = lines.size();
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].seq > *after_seq) {
                begin = i;
                break;
            }
        }
    }
    RestoredTranscriptPage page;
    if (begin >= end) {
        page.has_older = end > 0;
        page.has_newer = end < lines.size();
        return page;
    }
    if (max_lines > 0 && end - begin > max_lines) {
        if (after_seq.has_value()) {
            // 向新翻:候选区间从头取最早的 max_lines 行。
            end = begin + max_lines;
        } else {
            // 首开(取尾页)与向旧翻:候选区间从尾取最新的 max_lines 行。
            begin = end - max_lines;
        }
    }
    page.has_older = begin > 0;
    page.has_newer = end < lines.size();
    page.oldest_seq = lines[begin].seq;
    page.newest_seq = lines[end - 1].seq;
    page.lines.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        page.lines.push_back(lines[i].text);
    }
    return page;
}

}  // namespace lubancode::runtime
