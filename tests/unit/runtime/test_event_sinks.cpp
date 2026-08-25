// 同流验收(显示系统剥离单第五步后半):同一份假 backend 脚本驱动一轮
// 回合,产生的事件流分别喂 TerminalEventSink 与 JsonEventSink——单子验收
// 原文:"事件 id、次序、终态和领域数据完全一致,只有渲染不同"。
//
// 这里把"同一事件流"的产线也立起来:TurnEventAdapter(agent::Callbacks ->
// ServerEvent 流 -> EventSink)就是第 5/6 步交界的那只适配器,终端与
// JSON 两家都从它手上拿饭吃。此测试钉三件事:
//   1. 同一 callbacks 驱动,两 sink 收到的条目 id、次序、正文累计、终态
//      逐字段一致;
//   2. JsonEventSink 落的 ndjson 逐行 parse 回 ServerEvent,与 TerminalEventSink
//      账本同源(事件 id / seq 单调 / 终态四分不丢);
//   3. seq 单调递增、终态唯一(ItemCompleted 对同一 id 只发一次)。

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
}
