// runtime/ 各类型头的序列化实现(显示系统剥离单第一步)。
//
// 全是纯函数:结构 <-> JSON。线上枚举一律字符串(理由见 event.hpp 的
// ServerEvent 注释);from_json 遇到认不得的枚举字符串抛异常,让调用方
// 决定跳过还是断链——不静默映射成某个默认值,那会把协议错误藏成数据
// 错误。

#include "runtime/command.hpp"
#include "runtime/event.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/interaction_broker.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace lubancode::runtime {

// 进程级发号局(第四步):static 局部,首次用到时构造,退出时自然收。
IdAuthority& ProcessIdAuthority() {
    static IdAuthority authority;
    return authority;
}

// ---- 小工具 ----------------------------------------------------------------

namespace {

// json 里取字符串字段,缺字段/类型不对给缺省空串——协议演进里"新加的
// 可选字符串字段,老对端发的报文没有它"是常态,不抛。
std::string GetStr(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

std::uint64_t GetU64(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_unsigned()) {
        return 0;
    }
    return it->get<std::uint64_t>();
}

std::int64_t GetI64(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) {
        return 0;
    }
    return it->get<std::int64_t>();
}

nlohmann::json GetObj(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_object()) {
        return nlohmann::json::object();
    }
    return *it;
}

}  // namespace

// ---- EventEnvelope ---------------------------------------------------------

nlohmann::json EventEnvelope::to_json() const {
    return nlohmann::json{
        {"thread_id", thread_id},
        {"seq", seq},
        {"timestamp_ms", timestamp_ms},
    };
}

EventEnvelope EventEnvelope::from_json(const nlohmann::json& j) {
    EventEnvelope e;
    e.thread_id = GetStr(j, "thread_id");
    e.seq = GetU64(j, "seq");
    e.timestamp_ms = GetI64(j, "timestamp_ms");
    return e;
}

// ---- 枚举 <-> 稳定字符串 ----------------------------------------------------

std::string ToString(ServerEventKind kind) {
    switch (kind) {
        case ServerEventKind::ThreadStarted: return "thread.started";
        case ServerEventKind::ThreadUpdated: return "thread.updated";
        case ServerEventKind::TurnStarted: return "turn.started";
        case ServerEventKind::TurnCompleted: return "turn.completed";
        case ServerEventKind::ItemStarted: return "item.started";
        case ServerEventKind::ItemDelta: return "item.delta";
        case ServerEventKind::ItemCompleted: return "item.completed";
        case ServerEventKind::ApprovalRequested: return "interaction.approval_requested";
        case ServerEventKind::QuestionRequested: return "interaction.question_requested";
        case ServerEventKind::InteractionResolved: return "interaction.resolved";
        case ServerEventKind::UsageUpdated: return "usage.updated";
        case ServerEventKind::ContextUpdated: return "context.updated";
        case ServerEventKind::Warning: return "warning";
        case ServerEventKind::Error: return "error";
    }
    return "error";
}

std::string ToString(ItemKind kind) {
    switch (kind) {
        case ItemKind::Tool: return "tool";
        case ItemKind::Thinking: return "thinking";
        case ItemKind::Text: return "text";
        case ItemKind::Command: return "command";
        case ItemKind::Diff: return "diff";
        case ItemKind::Todo: return "todo";
        case ItemKind::Subagent: return "subagent";
    }
    return "tool";
}

std::string ToString(EventLayer layer) {
    switch (layer) {
        case EventLayer::Thread: return "thread";
        case EventLayer::Turn: return "turn";
        case EventLayer::Item: return "item";
    }
    return "thread";
}

std::string ToString(Outcome outcome) {
    switch (outcome) {
        case Outcome::Succeeded: return "succeeded";
        case Outcome::Failed: return "failed";
        case Outcome::Declined: return "declined";
        case Outcome::Cancelled: return "cancelled";
    }
    return "failed";
}

bool ParseServerEventKind(const std::string& s, ServerEventKind& out) {
    if (s == "thread.started") { out = ServerEventKind::ThreadStarted; return true; }
    if (s == "thread.updated") { out = ServerEventKind::ThreadUpdated; return true; }
    if (s == "turn.started") { out = ServerEventKind::TurnStarted; return true; }
    if (s == "turn.completed") { out = ServerEventKind::TurnCompleted; return true; }
    if (s == "item.started") { out = ServerEventKind::ItemStarted; return true; }
    if (s == "item.delta") { out = ServerEventKind::ItemDelta; return true; }
    if (s == "item.completed") { out = ServerEventKind::ItemCompleted; return true; }
    if (s == "interaction.approval_requested") { out = ServerEventKind::ApprovalRequested; return true; }
    if (s == "interaction.question_requested") { out = ServerEventKind::QuestionRequested; return true; }
    if (s == "interaction.resolved") { out = ServerEventKind::InteractionResolved; return true; }
    if (s == "usage.updated") { out = ServerEventKind::UsageUpdated; return true; }
    if (s == "context.updated") { out = ServerEventKind::ContextUpdated; return true; }
    if (s == "warning") { out = ServerEventKind::Warning; return true; }
    if (s == "error") { out = ServerEventKind::Error; return true; }
    return false;
}

bool ParseItemKind(const std::string& s, ItemKind& out) {
    if (s == "tool") { out = ItemKind::Tool; return true; }
    if (s == "thinking") { out = ItemKind::Thinking; return true; }
    if (s == "text") { out = ItemKind::Text; return true; }
    if (s == "command") { out = ItemKind::Command; return true; }
    if (s == "diff") { out = ItemKind::Diff; return true; }
    if (s == "todo") { out = ItemKind::Todo; return true; }
    if (s == "subagent") { out = ItemKind::Subagent; return true; }
    return false;
}

bool ParseOutcome(const std::string& s, Outcome& out) {
    if (s == "succeeded") { out = Outcome::Succeeded; return true; }
    if (s == "failed") { out = Outcome::Failed; return true; }
    if (s == "declined") { out = Outcome::Declined; return true; }
    if (s == "cancelled") { out = Outcome::Cancelled; return true; }
    return false;
}

// ---- ServerEvent -----------------------------------------------------------

nlohmann::json ServerEvent::to_json() const {
    nlohmann::json j = envelope.to_json();
    j["kind"] = ToString(kind);
    if (!turn_id.empty()) {
        j["turn_id"] = turn_id;
    }
    if (!item_id.empty()) {
        j["item_id"] = item_id;
        j["item_kind"] = ToString(item_kind);
    }
    if (outcome.has_value()) {
        j["outcome"] = ToString(*outcome);
    }
    if (!text.empty()) {
        j["text"] = text;
    }
    if (!payload.is_null() && !payload.empty()) {
        j["payload"] = payload;
    }
    return j;
}

ServerEvent ServerEvent::from_json(const nlohmann::json& j) {
    ServerEvent e;
    e.envelope = EventEnvelope::from_json(j);
    const std::string kind_str = GetStr(j, "kind");
    if (!ParseServerEventKind(kind_str, e.kind)) {
        throw std::invalid_argument("runtime: 未知 ServerEventKind: " + kind_str);
    }
    e.turn_id = GetStr(j, "turn_id");
    e.item_id = GetStr(j, "item_id");
    if (!e.item_id.empty()) {
        const std::string item_kind_str = GetStr(j, "item_kind");
        if (!ParseItemKind(item_kind_str, e.item_kind)) {
            throw std::invalid_argument("runtime: 未知 ItemKind: " + item_kind_str);
        }
    }
    if (const auto it = j.find("outcome"); it != j.end() && it->is_string()) {
        Outcome outcome = Outcome::Failed;
        if (!ParseOutcome(it->get<std::string>(), outcome)) {
            throw std::invalid_argument("runtime: 未知 Outcome: " + it->get<std::string>());
        }
        e.outcome = outcome;
    }
    e.text = GetStr(j, "text");
    e.payload = GetObj(j, "payload");
    return e;
}

EventLayer LayerOf(const ServerEvent& event) {
    switch (event.kind) {
        case ServerEventKind::ThreadStarted:
        case ServerEventKind::ThreadUpdated:
            return EventLayer::Thread;
        case ServerEventKind::TurnStarted:
        case ServerEventKind::TurnCompleted:
        case ServerEventKind::UsageUpdated:
        case ServerEventKind::ContextUpdated:
        case ServerEventKind::Warning:
        case ServerEventKind::Error:
            return EventLayer::Turn;
        case ServerEventKind::ItemStarted:
        case ServerEventKind::ItemDelta:
        case ServerEventKind::ItemCompleted:
        case ServerEventKind::ApprovalRequested:
        case ServerEventKind::QuestionRequested:
        case ServerEventKind::InteractionResolved:
            return EventLayer::Item;
    }
    return EventLayer::Thread;
}

// ---- ErrorPayload ----------------------------------------------------------

nlohmann::json ErrorPayload::to_json() const {
    return nlohmann::json{
        {"code", code},
        {"details", details},
        {"fallback_message", fallback_message},
    };
}

ErrorPayload ErrorPayload::from_json(const nlohmann::json& j) {
    ErrorPayload e;
    e.code = GetStr(j, "code");
    e.details = GetObj(j, "details");
    e.fallback_message = GetStr(j, "fallback_message");
    return e;
}

// ---- ClientCommand ---------------------------------------------------------

std::string ToString(ClientCommandKind kind) {
    switch (kind) {
        case ClientCommandKind::StartThread: return "thread.start";
        case ClientCommandKind::ResumeThread: return "thread.resume";
        case ClientCommandKind::ListThreads: return "thread.list";
        case ClientCommandKind::ReadThread: return "thread.read";
        case ClientCommandKind::StartTurn: return "turn.start";
        case ClientCommandKind::SteerTurn: return "turn.steer";
        case ClientCommandKind::InterruptTurn: return "turn.interrupt";
        case ClientCommandKind::ResolveApproval: return "interaction.resolve_approval";
        case ClientCommandKind::AnswerQuestion: return "interaction.answer_question";
        case ClientCommandKind::SetModel: return "set.model";
        case ClientCommandKind::SetThink: return "set.think";
        case ClientCommandKind::SetProvider: return "set.provider";
        case ClientCommandKind::SetLanguage: return "set.language";
        case ClientCommandKind::ClearThread: return "thread.clear";
        case ClientCommandKind::SetTitle: return "thread.set_title";
        case ClientCommandKind::Compact: return "thread.compact";
        case ClientCommandKind::Export: return "thread.export";
    }
    return "turn.start";
}

bool ParseClientCommandKind(const std::string& s, ClientCommandKind& out) {
    if (s == "thread.start") { out = ClientCommandKind::StartThread; return true; }
    if (s == "thread.resume") { out = ClientCommandKind::ResumeThread; return true; }
    if (s == "thread.list") { out = ClientCommandKind::ListThreads; return true; }
    if (s == "thread.read") { out = ClientCommandKind::ReadThread; return true; }
    if (s == "turn.start") { out = ClientCommandKind::StartTurn; return true; }
    if (s == "turn.steer") { out = ClientCommandKind::SteerTurn; return true; }
    if (s == "turn.interrupt") { out = ClientCommandKind::InterruptTurn; return true; }
    if (s == "interaction.resolve_approval") { out = ClientCommandKind::ResolveApproval; return true; }
    if (s == "interaction.answer_question") { out = ClientCommandKind::AnswerQuestion; return true; }
    if (s == "set.model") { out = ClientCommandKind::SetModel; return true; }
    if (s == "set.think") { out = ClientCommandKind::SetThink; return true; }
    if (s == "set.provider") { out = ClientCommandKind::SetProvider; return true; }
    if (s == "set.language") { out = ClientCommandKind::SetLanguage; return true; }
    if (s == "thread.clear") { out = ClientCommandKind::ClearThread; return true; }
    if (s == "thread.set_title") { out = ClientCommandKind::SetTitle; return true; }
    if (s == "thread.compact") { out = ClientCommandKind::Compact; return true; }
    if (s == "thread.export") { out = ClientCommandKind::Export; return true; }
    return false;
}

nlohmann::json ClientCommand::to_json() const {
    nlohmann::json j{
        {"kind", ToString(kind)},
    };
    if (!thread_id.empty()) {
        j["thread_id"] = thread_id;
    }
    if (!text.empty()) {
        j["text"] = text;
    }
    if (!value.empty()) {
        j["value"] = value;
    }
    if (!answers.empty()) {
        j["answers"] = answers;
    }
    if (!payload.is_null() && !payload.empty()) {
        j["payload"] = payload;
    }
    return j;
}

ClientCommand ClientCommand::from_json(const nlohmann::json& j) {
    ClientCommand c;
    const std::string kind_str = GetStr(j, "kind");
    if (!ParseClientCommandKind(kind_str, c.kind)) {
        throw std::invalid_argument("runtime: 未知 ClientCommandKind: " + kind_str);
    }
    c.thread_id = GetStr(j, "thread_id");
    c.text = GetStr(j, "text");
    c.value = GetStr(j, "value");
    if (const auto it = j.find("answers"); it != j.end() && it->is_array()) {
        for (const auto& a : *it) {
            if (a.is_string()) {
                c.answers.push_back(a.get<std::string>());
            }
        }
    }
    c.payload = GetObj(j, "payload");
    return c;
}

nlohmann::json ClientReceipt::to_json() const {
    nlohmann::json j{
        {"accepted", accepted},
        {"error_code", error_code},
        {"error_message", error_message},
    };
    if (!payload.is_null() && !payload.empty()) {
        j["payload"] = payload;
    }
    return j;
}

ClientReceipt ClientReceipt::from_json(const nlohmann::json& j) {
    ClientReceipt r;
    if (const auto it = j.find("accepted"); it != j.end() && it->is_boolean()) {
        r.accepted = it->get<bool>();
    }
    r.error_code = GetStr(j, "error_code");
    r.error_message = GetStr(j, "error_message");
    r.payload = GetObj(j, "payload");
    return r;
}

// ---- InteractionBroker -----------------------------------------------------

std::string ToString(InteractionDecision decision) {
    switch (decision) {
        case InteractionDecision::Accept: return "accept";
        case InteractionDecision::AcceptForSession: return "accept_for_session";
        case InteractionDecision::Decline: return "decline";
        case InteractionDecision::Cancel: return "cancel";
    }
    return "decline";
}

bool ParseInteractionDecision(const std::string& s, InteractionDecision& out) {
    if (s == "accept") { out = InteractionDecision::Accept; return true; }
    if (s == "accept_for_session") { out = InteractionDecision::AcceptForSession; return true; }
    if (s == "decline") { out = InteractionDecision::Decline; return true; }
    if (s == "cancel") { out = InteractionDecision::Cancel; return true; }
    return false;
}

nlohmann::json ApprovalRequest::to_json() const {
    return nlohmann::json{
        {"tool_name", tool_name},
        {"input", input.is_null() ? nlohmann::json::object() : input},
        {"reason", reason},
    };
}

ApprovalRequest ApprovalRequest::from_json(const nlohmann::json& j) {
    ApprovalRequest r;
    r.tool_name = GetStr(j, "tool_name");
    r.input = GetObj(j, "input");
    r.reason = GetStr(j, "reason");
    return r;
}

nlohmann::json ApprovalResponse::to_json() const {
    return nlohmann::json{
        {"decision", ToString(decision)},
        {"reason", reason},
    };
}

ApprovalResponse ApprovalResponse::from_json(const nlohmann::json& j) {
    ApprovalResponse r;
    const std::string decision_str = GetStr(j, "decision");
    if (!ParseInteractionDecision(decision_str, r.decision)) {
        throw std::invalid_argument("runtime: 未知 InteractionDecision: " + decision_str);
    }
    r.reason = GetStr(j, "reason");
    return r;
}

nlohmann::json QuestionOption::to_json() const {
    return nlohmann::json{
        {"label", label},
        {"description", description},
    };
}

QuestionOption QuestionOption::from_json(const nlohmann::json& j) {
    QuestionOption o;
    o.label = GetStr(j, "label");
    o.description = GetStr(j, "description");
    return o;
}

nlohmann::json QuestionRequest::to_json() const {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& option : options) {
        items.push_back(option.to_json());
    }
    return nlohmann::json{
        {"header", header},
        {"question", question},
        {"options", std::move(items)},
        {"multi_select", multi_select},
    };
}

QuestionRequest QuestionRequest::from_json(const nlohmann::json& j) {
    QuestionRequest q;
    q.header = GetStr(j, "header");
    q.question = GetStr(j, "question");
    if (const auto it = j.find("options"); it != j.end() && it->is_array()) {
        for (const auto& raw : *it) {
            if (raw.is_object()) {
                q.options.push_back(QuestionOption::from_json(raw));
            }
        }
    }
    if (const auto it = j.find("multi_select"); it != j.end() && it->is_boolean()) {
        q.multi_select = it->get<bool>();
    }
    return q;
}

nlohmann::json QuestionResponse::to_json() const {
    return nlohmann::json{
        {"answers", answers},
        {"error", error},
    };
}

QuestionResponse QuestionResponse::from_json(const nlohmann::json& j) {
    QuestionResponse r;
    if (const auto it = j.find("answers"); it != j.end() && it->is_array()) {
        for (const auto& a : *it) {
            if (a.is_string()) {
                r.answers.push_back(a.get<std::string>());
            }
        }
    }
    r.error = GetStr(j, "error");
    return r;
}

}  // namespace lubancode::runtime
