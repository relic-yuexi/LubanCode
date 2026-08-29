// 问题二回归(2026-08-29 实测记录问题 2,单子点名的"工具边界早于轮末"
// 岔路):忙碌会话往队列塞 /context,回合过两个工具边界,断言——
//   1) 任何一次模型请求里查无 "/context":工具边界收件点(TakeDeliverable)
//      在队列层对 slash 让路,只送普通文字作 steering;
//   2) 回合收尾后 /context 仍在队列,轮末会话泵(TakeFirstAutoSendable)
//      取得到,且 ProcessLine(interactive_session.cpp:501-514)开头那颗
//      ParseSlashCommand 认得它——轮末走本地分派(开面板/打印),不是发
//      模型。面板真出现由管道冒烟(假后端跑真 exe)另验,这里钉请求纯净
//      与轮末取件这一岔;
//   3) 普通排队文字照旧在工具边界作为 [用户排队消息] steering 注入;
//   4) /help、/todos、/compact 与 /context 同一条规则;
//   5) slash 与文字混排各走各:文字按落队顺序注入,slash 留到轮末,互不
//      拦路。
// 收件点复刻 interactive_session_wiring.cpp:730-749 的取件核心(Take-
// Deliverable + [用户排队消息] 组包就是生产代码本尊,队列层过滤即所测);
// Echo/Persist 属终端装饰,不进请求内容。队列用局部实例(风格同
// test_queue_model.cpp,不碰全局 SessionSteeringQueue)。

#include <doctest/doctest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "cli/queue_model.hpp"
#include "cli/slash_commands.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

class FakeTool : public tools::Tool {
public:
    std::string name() const override { return "fake_tool"; }
    std::string description() const override { return "fake tool for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"ok", false}; }
};

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, "fake_tool"},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// 一次请求里全部可见正文拼起来(断言"查无 /context"用)。
std::string AllRequestText(const api::Request& request) {
    std::string out;
    for (const auto& message : request.messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                out += text->text;
                out += '\n';
            }
        }
    }
    return out;
}

// 生产收件点的取件核心(interactive_session_wiring.cpp:730-749 同款):
// TakeDeliverable 过滤即队列层本尊,组包文案一字不差。
agent::AgentWiring MakeBoundaryInbox(cli::SteeringQueue& queue) {
    agent::AgentWiring wiring;
    wiring.inbox = [&queue]() -> std::optional<api::Message> {
        const auto queued = queue.TakeDeliverable(cli::MessageTarget::Main());
        if (queued.empty()) {
            return std::nullopt;
        }
        api::Message inject;
        inject.role = api::Role::User;
        for (const auto& item : queued) {
            inject.content.push_back(api::TextBlock{
                "[用户排队消息] 用户在上一只工具执行期间补了话,按排队顺序接上,不另起新任务:\n" +
                item.text});
        }
        return inject;
    };
    return wiring;
}

}  // namespace

TEST_CASE("忙碌排队的 /context:两个工具边界都不进请求,轮末留给本地分派") {
    cli::SteeringQueue queue;
    queue.Enqueue(cli::MessageTarget::Main(), "/context");

    FakeBackend backend;
    // 两只工具 = 两次工具边界,第三次请求文本收尾。
    backend.scripts = {
        ToolUseScript("toolu_1"),
        ToolUseScript("toolu_2"),
        TextOnlyScript("干完了。"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    loop.SetWiring(MakeBoundaryInbox(queue));

    REQUIRE(loop.Run("去干活", agent::TurnWiring{}).has_value());

    // 断言一:三次请求(首请求 + 两次边界后的请求)里查无 /context。
    REQUIRE(backend.captured_requests.size() == 3);
    for (const auto& request : backend.captured_requests) {
        CHECK(AllRequestText(request).find("/context") == std::string::npos);
    }
    // 断言二:回合收尾后 /context 还在队列,轮末泵取得到,且 ProcessLine 的
    // 判法认得它是本地命令。
    const auto snapshot = queue.Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].text == "/context");
    const auto head = queue.TakeFirstAutoSendable(cli::MessageTarget::Main());
    REQUIRE(head.has_value());
    CHECK(head->text == "/context");
    CHECK(cli::ParseSlashCommand(head->text).command == cli::SlashCommand::Context);
    CHECK(queue.empty());  // 取走即消费,不会二次投递
}

TEST_CASE("/help、/todos、/compact 与 /context 同规则:边界让路,轮末本地执行") {
    for (const std::string& command : {"/help", "/todos", "/compact 重点保住清单"}) {
        CAPTURE(command);
        cli::SteeringQueue queue;
        queue.Enqueue(cli::MessageTarget::Main(), command);

        FakeBackend backend;
        backend.scripts = {
            ToolUseScript("toolu_1"),
            TextOnlyScript("收尾。"),
        };
        tools::ToolRegistry registry;
        registry.Register(std::make_unique<FakeTool>());
        agent::Agent loop(backend, registry,
                          agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
        loop.SetWiring(MakeBoundaryInbox(queue));

        REQUIRE(loop.Run("干活", agent::TurnWiring{}).has_value());
        REQUIRE(backend.captured_requests.size() == 2);
        for (const auto& request : backend.captured_requests) {
            CHECK(AllRequestText(request).find(command) == std::string::npos);
        }
        const auto head = queue.TakeFirstAutoSendable(cli::MessageTarget::Main());
        REQUIRE(head.has_value());
        CHECK(head->text == command);
        CHECK(cli::ParseSlashCommand(head->text).command != cli::SlashCommand::NotSlash);
    }
}

TEST_CASE("普通排队文字照旧 steering:工具边界注入 [用户排队消息]") {
    cli::SteeringQueue queue;
    queue.Enqueue(cli::MessageTarget::Main(), "补充:只看只读文件");

    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_s"),
        TextOnlyScript("收到。"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    loop.SetWiring(MakeBoundaryInbox(queue));

    REQUIRE(loop.Run("查", agent::TurnWiring{}).has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    const std::string second = AllRequestText(backend.captured_requests[1]);
    CHECK(second.find("[用户排队消息]") != std::string::npos);
    CHECK(second.find("只看只读文件") != std::string::npos);
    CHECK(queue.empty());  // 文字在边界送达出队
}

TEST_CASE("混排各走各:文字按序在边界注入,slash 留到轮末,互不拦路") {
    cli::SteeringQueue queue;
    queue.Enqueue(cli::MessageTarget::Main(), "文字一");
    queue.Enqueue(cli::MessageTarget::Main(), "/context");
    queue.Enqueue(cli::MessageTarget::Main(), "文字二");

    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_m1"),
        ToolUseScript("toolu_m2"),
        TextOnlyScript("好。"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    loop.SetWiring(MakeBoundaryInbox(queue));

    REQUIRE(loop.Run("去", agent::TurnWiring{}).has_value());
    REQUIRE(backend.captured_requests.size() == 3);

    // 文字两条在第一个边界一并注入(一次边界攒下的多条合成一条 user 消息,
    // 一块一条 TextBlock,顺序保留);两枚文字都在,slash 不在。
    const std::string second = AllRequestText(backend.captured_requests[1]);
    const std::size_t first_pos = second.find("文字一");
    const std::size_t second_pos = second.find("文字二");
    REQUIRE(first_pos != std::string::npos);
    REQUIRE(second_pos != std::string::npos);
    CHECK(first_pos < second_pos);  // 文字之间次序不乱
    for (const auto& request : backend.captured_requests) {
        CHECK(AllRequestText(request).find("/context") == std::string::npos);
    }

    // 轮末:队列只剩 slash,泵取走交给 ProcessLine 本地执行。
    const auto snapshot = queue.Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].text == "/context");
}
