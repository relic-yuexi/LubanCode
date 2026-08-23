// runtime 合同测试(显示系统剥离单第一步:立合同,不改画面)。
//
// 钉的是:
//   1. 各类型 to_json/from_json 往返保真——事件/命令/审批/提问/回执,
//      序列化前后字段一字不差;线上枚举是稳定字符串,不是数字。
//   2. 认不得的枚举字符串要抛,不许静默映射成默认值(那会把协议错误藏
//      成数据错误)。
//   3. 交互四态决定(accept/accept_for_session/decline/cancel)往返与
//      语义齐整;InteractionRequestId 空=非法。
//   4. QuestionRequest 与 tools::AskUserQuestion 字段对齐(中立镜像是
//      手抄的,漂移靠这里钉)。
//   5. EventEnvelope 的 seq/timestamp 类型纪律。

#include <doctest/doctest.h>

#include <stdexcept>

#include "runtime/command.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/interaction_broker.hpp"
#include "tools/ask_user.hpp"

namespace rt = lubancode::runtime;

// ---- 往返保真 --------------------------------------------------------------

TEST_CASE("ServerEvent:往返保真(带全套字段)") {
    rt::ServerEvent e;
    e.envelope.thread_id = "th-1";
    e.envelope.seq = 42;
    e.envelope.timestamp_ms = 1724300000123;
    e.kind = rt::ServerEventKind::ItemCompleted;
    e.turn_id = "turn-7";
    e.item_id = "item-9";
    e.item_kind = rt::ItemKind::Tool;
    e.outcome = rt::Outcome::Declined;
    e.text = "写文件被拒";
    e.payload = nlohmann::json{{"tool_name", "write_file"}, {"path", "a.txt"}};

    const nlohmann::json j = e.to_json();
    // 线上枚举是稳定字符串。
    CHECK(j["kind"] == "item.completed");
    CHECK(j["item_kind"] == "tool");
    CHECK(j["outcome"] == "declined");

    const rt::ServerEvent back = rt::ServerEvent::from_json(j);
    CHECK(back.envelope.thread_id == e.envelope.thread_id);
    CHECK(back.envelope.seq == e.envelope.seq);
    CHECK(back.envelope.timestamp_ms == e.envelope.timestamp_ms);
    CHECK(back.kind == e.kind);
    CHECK(back.turn_id == e.turn_id);
    CHECK(back.item_id == e.item_id);
    CHECK(back.item_kind == e.item_kind);
    REQUIRE(back.outcome.has_value());
    CHECK(*back.outcome == *e.outcome);
    CHECK(back.text == e.text);
    CHECK(back.payload == e.payload);
}

TEST_CASE("ServerEvent:最小事件(thread.started)往返,可选字段缺席不添") {
    rt::ServerEvent e;
    e.envelope.thread_id = "th-min";
    e.envelope.seq = 1;
    e.kind = rt::ServerEventKind::ThreadStarted;

    const nlohmann::json j = e.to_json();
    CHECK(j["kind"] == "thread.started");
    CHECK_FALSE(j.contains("turn_id"));
    CHECK_FALSE(j.contains("item_id"));
    CHECK_FALSE(j.contains("outcome"));

    const rt::ServerEvent back = rt::ServerEvent::from_json(j);
    CHECK(back.kind == rt::ServerEventKind::ThreadStarted);
    CHECK(back.turn_id.empty());
    CHECK(back.item_id.empty());
    CHECK_FALSE(back.outcome.has_value());
}

TEST_CASE("ServerEvent:每 kind 往返一轮,枚举字符串全覆盖") {
    const rt::ServerEventKind kinds[] = {
        rt::ServerEventKind::ThreadStarted,      rt::ServerEventKind::ThreadUpdated,
        rt::ServerEventKind::TurnStarted,        rt::ServerEventKind::TurnCompleted,
        rt::ServerEventKind::ItemStarted,        rt::ServerEventKind::ItemDelta,
        rt::ServerEventKind::ItemCompleted,      rt::ServerEventKind::ApprovalRequested,
        rt::ServerEventKind::QuestionRequested,  rt::ServerEventKind::InteractionResolved,
        rt::ServerEventKind::UsageUpdated,       rt::ServerEventKind::ContextUpdated,
        rt::ServerEventKind::Warning,            rt::ServerEventKind::Error,
    };
    for (const rt::ServerEventKind kind : kinds) {
        rt::ServerEvent e;
        e.envelope.thread_id = "th";
        e.envelope.seq = 3;
        e.kind = kind;
        const rt::ServerEvent back = rt::ServerEvent::from_json(e.to_json());
        CHECK(back.kind == kind);
        // ToString/Parse 是一对,单射。
        rt::ServerEventKind parsed{};
        CHECK(rt::ParseServerEventKind(rt::ToString(kind), parsed));
        CHECK(parsed == kind);
    }
}

TEST_CASE("ItemKind/Outcome/EventLayer:枚举字符串单射往返") {
    const rt::ItemKind item_kinds[] = {
        rt::ItemKind::Tool, rt::ItemKind::Thinking,   rt::ItemKind::Text,
        rt::ItemKind::Command, rt::ItemKind::Diff, rt::ItemKind::Todo,
        rt::ItemKind::Subagent,
    };
    for (const rt::ItemKind kind : item_kinds) {
        rt::ItemKind parsed{};
        CHECK(rt::ParseItemKind(rt::ToString(kind), parsed));
        CHECK(parsed == kind);
    }
    const rt::Outcome outcomes[] = {
        rt::Outcome::Succeeded, rt::Outcome::Failed, rt::Outcome::Declined, rt::Outcome::Cancelled,
    };
    for (const rt::Outcome outcome : outcomes) {
        rt::Outcome parsed{};
        CHECK(rt::ParseOutcome(rt::ToString(outcome), parsed));
        CHECK(parsed == outcome);
    }
    // 层次路由:thread/turn/item 三层各归各位。
    rt::ServerEvent thread_event;
    thread_event.kind = rt::ServerEventKind::ThreadStarted;
    CHECK(rt::LayerOf(thread_event) == rt::EventLayer::Thread);
    rt::ServerEvent turn_event;
    turn_event.kind = rt::ServerEventKind::TurnCompleted;
    CHECK(rt::LayerOf(turn_event) == rt::EventLayer::Turn);
    rt::ServerEvent item_event;
    item_event.kind = rt::ServerEventKind::ItemDelta;
    CHECK(rt::LayerOf(item_event) == rt::EventLayer::Item);
}

TEST_CASE("ErrorPayload:code+details+fallback 往返") {
    rt::ErrorPayload p;
    p.code = "http.transport_failed";
    p.details = nlohmann::json{{"status", 502}, {"url", "https://api.example.com"}};
    p.fallback_message = "网络出了岔子";

    const rt::ErrorPayload back = rt::ErrorPayload::from_json(p.to_json());
    CHECK(back.code == p.code);
    CHECK(back.details == p.details);
    CHECK(back.fallback_message == p.fallback_message);
}

TEST_CASE("ClientCommand:各 kind 往返,领域字段保真") {
    {
        rt::ClientCommand c;
        c.kind = rt::ClientCommandKind::StartTurn;
        c.thread_id = "th-1";
        c.text = "帮我看下这个报错";
        const rt::ClientCommand back = rt::ClientCommand::from_json(c.to_json());
        CHECK(back.kind == c.kind);
        CHECK(back.thread_id == c.thread_id);
        CHECK(back.text == c.text);
    }
    {
        rt::ClientCommand c;
        c.kind = rt::ClientCommandKind::ResolveApproval;
        c.thread_id = "th-1";
        c.payload = nlohmann::json{{"request_id", "req-5"}, {"decision", "accept_for_session"}};
        const rt::ClientCommand back = rt::ClientCommand::from_json(c.to_json());
        CHECK(back.kind == c.kind);
        CHECK(back.payload == c.payload);
    }
    {
        rt::ClientCommand c;
        c.kind = rt::ClientCommandKind::AnswerQuestion;
        c.answers = {"用 SQLite", "自己填:用 DuckDB"};
        const rt::ClientCommand back = rt::ClientCommand::from_json(c.to_json());
        CHECK(back.kind == c.kind);
        CHECK(back.answers == c.answers);
    }
    {
        rt::ClientCommand c;
        c.kind = rt::ClientCommandKind::SetModel;
        c.value = "deepseek-chat";
        const rt::ClientCommand back = rt::ClientCommand::from_json(c.to_json());
        CHECK(back.value == c.value);
    }

    const rt::ClientCommandKind kinds[] = {
        rt::ClientCommandKind::StartThread,     rt::ClientCommandKind::ResumeThread,
        rt::ClientCommandKind::ListThreads,     rt::ClientCommandKind::ReadThread,
        rt::ClientCommandKind::StartTurn,       rt::ClientCommandKind::SteerTurn,
        rt::ClientCommandKind::InterruptTurn,   rt::ClientCommandKind::ResolveApproval,
        rt::ClientCommandKind::AnswerQuestion,  rt::ClientCommandKind::SetModel,
        rt::ClientCommandKind::SetThink,        rt::ClientCommandKind::SetProvider,
        rt::ClientCommandKind::SetLanguage,     rt::ClientCommandKind::ClearThread,
        rt::ClientCommandKind::SetTitle,        rt::ClientCommandKind::Compact,
        rt::ClientCommandKind::Export,
    };
    for (const rt::ClientCommandKind kind : kinds) {
        rt::ClientCommand c;
        c.kind = kind;
        const rt::ClientCommand back = rt::ClientCommand::from_json(c.to_json());
        CHECK(back.kind == kind);
        rt::ClientCommandKind parsed{};
        CHECK(rt::ParseClientCommandKind(rt::ToString(kind), parsed));
        CHECK(parsed == kind);
    }
}

TEST_CASE("ClientReceipt:成功/失败两路往返") {
    {
        rt::ClientReceipt r;
        r.accepted = true;
        r.payload = nlohmann::json{{"threads", nlohmann::json::array({"th-1", "th-2"})}};
        const rt::ClientReceipt back = rt::ClientReceipt::from_json(r.to_json());
        CHECK(back.accepted);
        CHECK(back.error_code.empty());
        CHECK(back.payload == r.payload);
    }
    {
        rt::ClientReceipt r;
        r.accepted = false;
        r.error_code = rt::kStaleRequestId;
        r.error_message = "这条审批请求已经收口了";
        const rt::ClientReceipt back = rt::ClientReceipt::from_json(r.to_json());
        CHECK_FALSE(back.accepted);
        CHECK(back.error_code == rt::kStaleRequestId);
        CHECK(back.error_message == r.error_message);
    }
}

// ---- 审批与提问 ------------------------------------------------------------

TEST_CASE("审批:决定四态往返,空串决定要抛") {
    const rt::InteractionDecision decisions[] = {
        rt::InteractionDecision::Accept,
        rt::InteractionDecision::AcceptForSession,
        rt::InteractionDecision::Decline,
        rt::InteractionDecision::Cancel,
    };
    for (const rt::InteractionDecision decision : decisions) {
        rt::ApprovalResponse r;
        r.decision = decision;
        r.reason = "看过了,放行";
        const rt::ApprovalResponse back = rt::ApprovalResponse::from_json(r.to_json());
        CHECK(back.decision == decision);
        CHECK(back.reason == r.reason);

        rt::InteractionDecision parsed{};
        CHECK(rt::ParseInteractionDecision(rt::ToString(decision), parsed));
        CHECK(parsed == decision);
    }
    rt::InteractionDecision sink{};
    CHECK_FALSE(rt::ParseInteractionDecision("yolo", sink));  // 认不得的不静默映射
}

TEST_CASE("审批:ApprovalRequest 带 input JSON 与理由往返") {
    rt::ApprovalRequest req;
    req.tool_use_id = "toolu_P4";
    req.tool_name = "write_file";
    req.input = nlohmann::json{{"path", "src/main.cpp"}, {"content", "int main(){}"}};
    req.reason = "PreToolUse 钩子 ask:大改动";

    const nlohmann::json j = req.to_json();
    CHECK(j["tool_use_id"] == "toolu_P4");  // P4:审批钉在条目上的身份随行
    const rt::ApprovalRequest back = rt::ApprovalRequest::from_json(j);
    CHECK(back.tool_use_id == req.tool_use_id);
    CHECK(back.tool_name == req.tool_name);
    CHECK(back.input == req.input);
    CHECK(back.reason == req.reason);
}

TEST_CASE("提问:QuestionRequest/QuestionResponse 往返") {
    rt::QuestionRequest q;
    q.header = "选存储";
    q.question = "数据落在哪?";
    q.options.push_back({"SQLite", "单文件,零运维"});
    q.options.push_back({"Postgres", "并发强,要起服务"});
    q.multi_select = false;

    const nlohmann::json j = q.to_json();
    CHECK(j["options"].size() == 2);
    CHECK(j["options"][0]["label"] == "SQLite");
    CHECK(j["options"][0]["description"] == "单文件,零运维");

    const rt::QuestionRequest back = rt::QuestionRequest::from_json(j);
    CHECK(back.header == q.header);
    CHECK(back.question == q.question);
    REQUIRE(back.options.size() == 2);
    CHECK(back.options[0].label == "SQLite");
    CHECK(back.options[0].description == "单文件,零运维");
    CHECK(back.options[1].label == "Postgres");
    CHECK(back.multi_select == q.multi_select);

    rt::QuestionResponse resp;
    resp.answers = {"SQLite"};
    const rt::QuestionResponse resp_back = rt::QuestionResponse::from_json(resp.to_json());
    CHECK(resp_back.answers == resp.answers);
    CHECK(resp_back.error.empty());

    rt::QuestionResponse cancelled;
    cancelled.error = "用户按了 Esc";
    const rt::QuestionResponse cancelled_back = rt::QuestionResponse::from_json(cancelled.to_json());
    CHECK(cancelled_back.answers.empty());
    CHECK(cancelled_back.error == "用户按了 Esc");
}

TEST_CASE("提问:QuestionRequest 与 tools::AskUserQuestion 字段对齐") {
    // 中立镜像是手抄的,漂移靠这里钉:两边的字段名/语义一致,app-server
    // 接合同后做投影时不用猜。
    rt::QuestionRequest rt_q;
    rt_q.header = "h";
    rt_q.question = "q";
    rt_q.options.push_back({"a", "da"});
    rt_q.options.push_back({"b", "db"});
    rt_q.multi_select = true;

    lubancode::tools::AskUserQuestion tool_q;
    tool_q.header = rt_q.header;
    tool_q.question = rt_q.question;
    for (const auto& option : rt_q.options) {
        tool_q.options.push_back({option.label, option.description});
    }
    tool_q.multi_select = rt_q.multi_select;

    CHECK(tool_q.header == rt_q.header);
    CHECK(tool_q.question == rt_q.question);
    REQUIRE(tool_q.options.size() == rt_q.options.size());
    for (std::size_t i = 0; i < tool_q.options.size(); ++i) {
        CHECK(tool_q.options[i].label == rt_q.options[i].label);
        CHECK(tool_q.options[i].description == rt_q.options[i].description);
    }
    CHECK(tool_q.multi_select == rt_q.multi_select);
}

TEST_CASE("InteractionRequestId:空串不是合法 id") {
    rt::InteractionRequestId id;
    CHECK_FALSE(id.valid());
    id.value = "req-1";
    CHECK(id.valid());
}

// ---- 坏报文 ----------------------------------------------------------------

TEST_CASE("坏枚举字符串:from_json 抛,不静默吞") {
    const nlohmann::json bad_event = nlohmann::json{
        {"thread_id", "th"},
        {"seq", 1},
        {"kind", "magic.happened"},
    };
    CHECK_THROWS_AS(rt::ServerEvent::from_json(bad_event), std::invalid_argument);

    const nlohmann::json bad_item = nlohmann::json{
        {"thread_id", "th"},
        {"seq", 1},
        {"kind", "item.started"},
        {"item_id", "i"},
        {"item_kind", "widget"},
    };
    CHECK_THROWS_AS(rt::ServerEvent::from_json(bad_item), std::invalid_argument);

    const nlohmann::json bad_command = nlohmann::json{{"kind", "please.do"}};
    CHECK_THROWS_AS(rt::ClientCommand::from_json(bad_command), std::invalid_argument);

    const nlohmann::json bad_decision = nlohmann::json{{"decision", "maybe"}};
    CHECK_THROWS_AS(rt::ApprovalResponse::from_json(bad_decision), std::invalid_argument);
}

TEST_CASE("EventSink:函数壳把事件原样送出去") {
    rt::ServerEvent seen;
    rt::ServerEvent e;
    e.envelope.thread_id = "th";
    e.envelope.seq = 9;
    e.kind = rt::ServerEventKind::Warning;
    e.text = "上下文快满了";

    rt::FunctionEventSink sink([&seen](const rt::ServerEvent& event) { seen = event; });
    sink.Emit(e);
    CHECK(seen.kind == rt::ServerEventKind::Warning);
    CHECK(seen.envelope.seq == 9);
    CHECK(seen.text == e.text);
}

// ---- json 字节稳定 ---------------------------------------------------------

TEST_CASE("seq 用无符号整数序列化,不吃精度") {
    rt::EventEnvelope env;
    env.thread_id = "th";
    env.seq = 18014398509481984ULL;  // 2^54,双精度整数安全区上沿附近
    env.timestamp_ms = 9007199254740992LL;

    const rt::EventEnvelope back = rt::EventEnvelope::from_json(env.to_json());
    CHECK(back.seq == env.seq);
    CHECK(back.timestamp_ms == env.timestamp_ms);
}
