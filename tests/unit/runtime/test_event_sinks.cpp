// 同流验收(显示系统剥离单第五步后半;骨架拆解批二加钉):同一份假
// backend 脚本驱动一轮回合,产生的事件流分别喂 TerminalEventSink 与
// JsonEventSink——单子验收原文:"事件 id、次序、终态和领域数据完全一致,
// 只有渲染不同"。
//
// 这里把"同一事件流"的产线也立起来:TurnEventAdapter(agent::Callbacks ->
// ServerEvent 流 -> EventSink)就是第 5/6 步交界的那只适配器,终端与
// JSON 两家都从它手上拿饭吃。此测试钉三件事:
//   1. 同一 callbacks 驱动,两 sink 收到的条目 id、次序、正文累计、终态
//      逐字段一致;
//   2. JsonEventSink 落的 ndjson 逐行 parse 回 ServerEvent,与 TerminalEventSink
//      账本同源(事件 id / seq 单调 / 终态四分不丢);
//   3. seq 单调递增、终态唯一(ItemCompleted 对同一 id 只发一次)。
// 批二加钉(升正房):领域数据(工具名/入参/结果/usage 数字)两 sink
// 逐字段同源、payload 深比;显式 turn_id 透传;没收尾的条目按 Cancelled
// 收口;FanoutEventSink 一表事件多家齐收。

#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/event_sinks.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace rt = lubancode::runtime;
using namespace lubancode;

namespace {

// 按脚本吐事件的假后端(test_loop.cpp 同款)。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;

    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        if (call_count >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const api::StreamEvent& event : scripts[call_count]) {
            on_event(event);
        }
        ++call_count;
        return {};
    }
    std::size_t call_count = 0;
};

class FakeTool : public tools::Tool {
public:
    std::string name() const override { return "fake_tool"; }
    std::string description() const override { return "fake"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"工具跑完了", false}; }
};

std::vector<api::StreamEvent> TextThenToolScript() {
    return {
        api::MessageStart{"msg-1", "fake-model"},
        api::TextDelta{"先说一句话。"},
        api::ContentBlockDone{0},
        api::ToolUseStart{1, "toolu_1", "fake_tool"},
        api::ToolUseInputDelta{1, "{}"},
        api::ContentBlockDone{1},
        api::MessageDone{"tool_use", api::Usage{10, 5, 0, 0, 0}},
    };
}

// 事件录音机:两路 sink 之外再录一份原始流,对账用。
class RecordingSink final : public rt::EventSink {
public:
    void Emit(const rt::ServerEvent& event) override { events.push_back(event); }
    std::vector<rt::ServerEvent> events;
};

}  // namespace

TEST_CASE("同一假 backend 脚本:Terminal 与 Json 两 sink 同流同账") {
    FakeBackend backend;
    backend.scripts = {
        TextThenToolScript(),
        {
            api::MessageStart{"msg-2", "fake-model"},
            api::ThinkingDelta{"想一想"},
            api::TextDelta{"。"},
            api::ContentBlockDone{0},
            api::MessageDone{"end_turn", api::Usage{20, 7, 0, 0, 0}},
        },
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::AgentRuntimeProfile profile;
    agent::Agent loop(backend, registry, std::move(profile), std::string("same-stream-test"));

    RecordingSink recorder;
    rt::TerminalEventSink terminal_sink;
    std::ostringstream json_out;
    rt::JsonEventSink json_sink([&json_out](const std::string& line) { json_out << line; });

    // 一只三通适配器:同一事件流同时进录音机、终端账本、JSON 落笔。
    rt::TurnEventAdapter adapter("th-same", rt::ProcessIdAuthority());
    adapter.Attach([&](const rt::ServerEvent& event) {
        recorder.Emit(event);
        terminal_sink.Emit(event);
        json_sink.Emit(event);
    });
    agent::Callbacks callbacks = adapter.MakeCallbacks();

    const auto outcome = loop.Run(std::string("问一句"), callbacks);
    REQUIRE(outcome.has_value());

    // ---- 1. 录音机与终端账本对账:条目 id、次序、正文、终态 ----
    const auto records = terminal_sink.Snapshot();
    // 四枚:第一条消息的正文与工具、第二条消息的思考与收尾正文。
    REQUIRE(records.size() == 4);
    CHECK(records[0].kind == rt::ItemKind::Text);
    CHECK(records[0].text == "先说一句话。");
    CHECK(records[0].completed);
    CHECK(records[0].has_outcome);
    CHECK(records[0].outcome == rt::Outcome::Succeeded);
    CHECK(records[1].kind == rt::ItemKind::Tool);
    CHECK(records[1].tool_name == "fake_tool");
    CHECK(records[1].completed);
    CHECK(records[2].kind == rt::ItemKind::Thinking);
    CHECK(records[2].text == "想一想");
    CHECK(records[3].kind == rt::ItemKind::Text);
    CHECK(records[3].text == "。");

    // 终端账本里的 item_id 在原始事件流里都出现过,次序一致。
    std::size_t cursor = 0;
    for (const auto& record : records) {
        bool found = false;
        for (std::size_t i = cursor; i < recorder.events.size(); ++i) {
            if (recorder.events[i].kind == rt::ServerEventKind::ItemStarted &&
                recorder.events[i].item_id == record.item_id) {
                found = true;
                cursor = i + 1;
                break;
            }
        }
        CHECK(found);
    }

    // ---- 2. ndjson 逐行 parse 回来,与录音机同源 ----
    std::vector<rt::ServerEvent> from_json;
    {
        std::istringstream in(json_out.str());
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            from_json.push_back(rt::ServerEvent::from_json(nlohmann::json::parse(line)));
        }
    }
    REQUIRE(from_json.size() == recorder.events.size());
    for (std::size_t i = 0; i < from_json.size(); ++i) {
        CHECK(from_json[i].envelope.seq == recorder.events[i].envelope.seq);
        CHECK(from_json[i].envelope.thread_id == recorder.events[i].envelope.thread_id);
        CHECK(from_json[i].kind == recorder.events[i].kind);
        CHECK(from_json[i].item_id == recorder.events[i].item_id);
        CHECK(from_json[i].turn_id == recorder.events[i].turn_id);
        CHECK(from_json[i].text == recorder.events[i].text);
        CHECK(from_json[i].item_kind == recorder.events[i].item_kind);
        CHECK(from_json[i].outcome.has_value() == recorder.events[i].outcome.has_value());
        if (from_json[i].outcome.has_value()) {
            CHECK(*from_json[i].outcome == *recorder.events[i].outcome);
        }
    }

    // ---- 3. seq 单调、终态唯一 ----
    std::uint64_t last_seq = 0;
    std::map<std::string, int> completions;
    for (const auto& event : recorder.events) {
        CHECK(event.envelope.seq > last_seq);
        last_seq = event.envelope.seq;
        if (event.kind == rt::ServerEventKind::ItemCompleted) {
            ++completions[event.item_id];
        }
    }
    for (const auto& [id, count] : completions) {
        CHECK(count == 1);
    }
    CHECK(terminal_sink.emitted() == recorder.events.size());
    CHECK(json_sink.emitted() == recorder.events.size());

    // ---- 4. 批二加钉:领域数据同源(工具名/入参/结果/错误位/usage) ----
    // 录音机是两家共同的原始流,从它对剧本:第一轮工具 fake_tool 带
    // toolu_1/入参 {},结果"工具跑完了"无错;两轮 usage 的 token 数与
    // 脚本一致;ndjson 逐行与录音机 payload 深比(不只 id,载荷一字不差)。
    const rt::ServerEvent* tool_started = nullptr;
    const rt::ServerEvent* tool_completed = nullptr;
    std::vector<std::int64_t> usage_inputs;
    for (const auto& event : recorder.events) {
        if (event.kind == rt::ServerEventKind::ItemStarted && event.item_kind == rt::ItemKind::Tool &&
            tool_started == nullptr) {
            tool_started = &event;
        }
        if (event.kind == rt::ServerEventKind::ItemCompleted && event.item_kind == rt::ItemKind::Tool &&
            tool_completed == nullptr) {
            tool_completed = &event;
        }
        if (event.kind == rt::ServerEventKind::UsageUpdated) {
            usage_inputs.push_back(event.payload.value("input_tokens", std::int64_t{0}));
        }
    }
    REQUIRE(tool_started != nullptr);
    REQUIRE(tool_completed != nullptr);
    CHECK(tool_started->payload.value("tool_name", std::string()) == "fake_tool");
    CHECK(tool_started->payload.value("tool_use_id", std::string()) == "toolu_1");
    CHECK(tool_completed->payload.value("result", std::string()) == "工具跑完了");
    CHECK(tool_completed->payload.value("is_error", false) == false);
    REQUIRE(usage_inputs.size() == 2);
    CHECK(usage_inputs[0] == 10);
    CHECK(usage_inputs[1] == 20);
    for (std::size_t i = 0; i < from_json.size(); ++i) {
        CHECK(from_json[i].payload == recorder.events[i].payload);
    }

    // 终端账本对工具条目的领域数据:tool_name 从 ItemStarted 载荷投影。
    bool ledger_tool_ok = false;
    for (const auto& record : records) {
        if (record.kind == rt::ItemKind::Tool && record.tool_name == "fake_tool" && record.completed &&
            record.outcome == rt::Outcome::Succeeded) {
            ledger_tool_ok = true;
        }
    }
    CHECK(ledger_tool_ok);
}

TEST_CASE("批二·显式 turn_id 透传;没收尾的条目按 Cancelled 收口,终态唯一") {
    // 适配器单元级:Start 可带宿主已发的 turn_id(trace 口径同源);Finish
    // 把没收尾的正文条目按 Succeeded、工具条目按 Cancelled 兜底,再发唯一
    // 一枚 TurnCompleted。同一事件流喂终端/JSON 两家,深比 payload。
    rt::TurnEventAdapter adapter("th-cancel", rt::ProcessIdAuthority());
    rt::TerminalEventSink terminal_sink;
    std::ostringstream json_out;
    rt::JsonEventSink json_sink([&json_out](const std::string& line) { json_out << line; });
    adapter.Attach([&](const rt::ServerEvent& event) {
        terminal_sink.Emit(event);
        json_sink.Emit(event);
    });

    CHECK(adapter.Start("turn-explicit") == "turn-explicit");
    agent::Callbacks callbacks = adapter.MakeCallbacks();
    callbacks.on_tool_start("toolu_9", "read_file", nlohmann::json{{"path", "a.txt"}});
    callbacks.on_text_delta("被打断的半截话");
    adapter.Finish(rt::Outcome::Cancelled);
    adapter.Finish(rt::Outcome::Cancelled);  // 幂等:重复收口不再发终态

    std::vector<rt::ServerEvent> events;
    {
        std::istringstream in(json_out.str());
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) {
                events.push_back(rt::ServerEvent::from_json(nlohmann::json::parse(line)));
            }
        }
    }
    // 期望次序:TurnStarted → 工具 ItemStarted → 正文 ItemStarted → 正文
    // ItemDelta → 正文 ItemCompleted(Succeeded)→ 工具 ItemCompleted
    //(Cancelled)→ TurnCompleted(Cancelled),恰七枚(重复 Finish 不加账)。
    REQUIRE(events.size() == 7);
    CHECK(events[0].kind == rt::ServerEventKind::TurnStarted);
    CHECK(events[1].kind == rt::ServerEventKind::ItemStarted);
    CHECK(events[1].item_kind == rt::ItemKind::Tool);
    CHECK(events[1].payload.value("tool_name", std::string()) == "read_file");
    CHECK(events[1].payload.value("tool_use_id", std::string()) == "toolu_9");
    CHECK(events[2].kind == rt::ServerEventKind::ItemStarted);
    CHECK(events[2].item_kind == rt::ItemKind::Text);
    CHECK(events[3].kind == rt::ServerEventKind::ItemDelta);
    CHECK(events[3].text == "被打断的半截话");
    CHECK(events[4].kind == rt::ServerEventKind::ItemCompleted);
    CHECK(events[4].item_kind == rt::ItemKind::Text);
    CHECK(events[4].outcome == rt::Outcome::Succeeded);
    CHECK(events[5].kind == rt::ServerEventKind::ItemCompleted);
    CHECK(events[5].item_kind == rt::ItemKind::Tool);
    CHECK(events[5].outcome == rt::Outcome::Cancelled);
    CHECK(events[6].kind == rt::ServerEventKind::TurnCompleted);
    CHECK(events[6].outcome == rt::Outcome::Cancelled);

    std::uint64_t last_seq = 0;
    for (const auto& event : events) {
        CHECK(event.turn_id == "turn-explicit");
        CHECK(event.envelope.thread_id == "th-cancel");
        CHECK(event.envelope.seq > last_seq);
        last_seq = event.envelope.seq;
    }
    CHECK(terminal_sink.emitted() == events.size());
    CHECK(json_sink.emitted() == events.size());

    // 终端账本同源:正文条目收尾 Succeeded、工具条目收尾 Cancelled。
    const auto records = terminal_sink.Snapshot();
    REQUIRE(records.size() == 2);
    CHECK(records[0].kind == rt::ItemKind::Tool);
    CHECK(records[0].completed);
    CHECK(records[0].outcome == rt::Outcome::Cancelled);
    CHECK(records[1].kind == rt::ItemKind::Text);
    CHECK(records[1].text == "被打断的半截话");
    CHECK(records[1].completed);
    CHECK(records[1].outcome == rt::Outcome::Succeeded);
}

TEST_CASE("批二·FanoutEventSink:一表事件多家齐收,次序与载荷一致") {
    // 装配点的"sink 列表":fanout 挂两家录音机(空指针忽略),同一适配器
    // 喂进去,两家收到的事件逐字段一致——加第三家不改产线。
    rt::FanoutEventSink fanout;
    RecordingSink first;
    RecordingSink second;
    fanout.Add(&first);
    fanout.Add(nullptr);  // 空指针安静忽略
    fanout.Add(&second);

    FakeBackend backend;
    backend.scripts = {TextThenToolScript(),
                       {
                           api::MessageStart{"msg-f", "fake-model"},
                           api::TextDelta{"收口。"},
                           api::ContentBlockDone{0},
                           api::MessageDone{"end_turn", api::Usage{1, 1, 0, 0, 0}},
                       }};
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::AgentRuntimeProfile profile;
    agent::Agent loop(backend, registry, std::move(profile), std::string("fanout-test"));

    rt::TurnEventAdapter adapter("th-fanout", rt::ProcessIdAuthority());
    adapter.Attach([&fanout](const rt::ServerEvent& event) { fanout.Emit(event); });
    const auto outcome = loop.Run(std::string("问一句"), adapter.MakeCallbacks());
    REQUIRE(outcome.has_value());
    adapter.Finish(rt::Outcome::Succeeded);

    REQUIRE(first.events.size() == second.events.size());
    REQUIRE(first.events.size() >= 4);
    for (std::size_t i = 0; i < first.events.size(); ++i) {
        CHECK(first.events[i].kind == second.events[i].kind);
        CHECK(first.events[i].envelope.seq == second.events[i].envelope.seq);
        CHECK(first.events[i].item_id == second.events[i].item_id);
        CHECK(first.events[i].text == second.events[i].text);
        CHECK(first.events[i].payload == second.events[i].payload);
    }
}

TEST_CASE("批二·ComposeDisplayCallbacks:事件流在前、终端老路在后,缺侧直通") {
    // 两轨并行的装配笔(turn_runner/AgentExecutor 共用):两侧都有时先事件
    // 后终端(事件是 canonical 账,先落账再画屏);一侧缺省只走另一侧;
    // 控制口(on_tool_confirm 这类带返回值的)不并。
    std::vector<std::string> calls;
    agent::Callbacks terminal;
    terminal.on_text_delta = [&calls](const std::string& text) { calls.push_back("term:" + text); };
    terminal.on_tool_confirm = [&calls](const std::string&, const std::string&, const nlohmann::json&) {
        calls.push_back("term:confirm");
        return true;
    };

    agent::Callbacks events;
    events.on_text_delta = [&calls](const std::string& text) { calls.push_back("event:" + text); };
    events.on_usage = [&calls](const api::UsageReport&) { calls.push_back("event:usage"); };

    rt::ComposeDisplayCallbacks(terminal, events);
    REQUIRE(terminal.on_text_delta);
    REQUIRE(terminal.on_usage);
    terminal.on_text_delta("正文");
    terminal.on_usage(api::UsageReport{});
    CHECK(calls == std::vector<std::string>{"event:正文", "term:正文", "event:usage"});

    // 事件侧缺省的口子直通终端;终端缺省的口子直通事件。
    agent::Callbacks only_back;
    only_back.on_thinking_delta = [&calls](const std::string&) { calls.push_back("back:think"); };
    rt::ComposeDisplayCallbacks(only_back, events);  // events 没有 thinking 口
    only_back.on_thinking_delta("想");
    CHECK(calls.back() == "back:think");

    // 控制口不并:events 侧就算给了 confirm 也不吃(那不是出水口,装配点
    // 自己配自己的)。
    events.on_tool_confirm = [](const std::string&, const std::string&, const nlohmann::json&) {
        return false;
    };
    rt::ComposeDisplayCallbacks(terminal, events);
    CHECK(terminal.on_tool_confirm({}, "run_command", nlohmann::json::object()) == true);  // 仍是终端那份
}
