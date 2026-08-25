// 跨会话传话:AgentLoop 的安全收件点。FakeBackend 按脚本吐事件(同
// test_loop.cpp 的手艺),验证:
//   - 工具轮回之间(下一次请求尚未发出)收到的信注进了 history,并且
//     出现在第二次请求的 messages 里;
//   - 注入是"末条 user 消息追加文本块"(不破 user/assistant 交替),末条
//     是 assistant 时新起一条 user;
//   - 确认回调当口来信不可能被"作答"——收件点只在循环边界被调,权限
//     确认回调里根本看不见它(结构上就不存在那条路,这里钉的是收件点
//     的调用时机:确认回调执行期间 inbox 一次都没被调过)。

#include <doctest/doctest.h>

#include <atomic>
#include <optional>
#include <variant>
#include <vector>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
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

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

api::Message IncomingText(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

}  // namespace

TEST_CASE("InjectIncomingMessage:末条是 user 时追加文本块,不另起一条") {
    std::vector<api::Message> history;
    history.push_back(IncomingText("第一句"));
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"回答"});
    history.push_back(std::move(assistant));
    api::Message tool_results;
    tool_results.role = api::Role::User;
    tool_results.content.push_back(api::ToolResultBlock{"toolu_1", "结果", false});
    history.push_back(std::move(tool_results));

    agent::InjectIncomingMessage(history, IncomingText("[来自另一场会话] 接口改好了"));
    REQUIRE(history.size() == 3);  // 没多出一条
    const auto& last = history.back();
    REQUIRE(last.content.size() == 2);
    CHECK(std::holds_alternative<api::ToolResultBlock>(last.content[0]));
    CHECK(std::get<api::TextBlock>(last.content[1]).text.find("接口改好了") != std::string::npos);
}

TEST_CASE("InjectIncomingMessage:末条是 assistant 时新起一条 user") {
    std::vector<api::Message> history;
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"回答"});
    history.push_back(std::move(assistant));

    agent::InjectIncomingMessage(history, IncomingText("字条"));
    REQUIRE(history.size() == 2);
    CHECK(history.back().role == api::Role::User);
    CHECK(std::get<api::TextBlock>(history.back().content[0]).text == "字条");
}

TEST_CASE("InjectIncomingMessage:空消息/非 user 消息不动 history") {
    std::vector<api::Message> history;
    history.push_back(IncomingText("原样"));
    agent::InjectIncomingMessage(history, api::Message{});  // 空消息
    CHECK(history.size() == 1);
    api::Message not_user = IncomingText("x");
    not_user.role = api::Role::Assistant;
    agent::InjectIncomingMessage(history, std::move(not_user));
    CHECK(history.size() == 1);
}

TEST_CASE("收件点:工具轮回之间来信,注进 history 并出现在下一次请求里") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_1", "fake_tool"),
        TextOnlyScript("收到,继续。"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());

    agent::Agent loop(backend, registry, "test-model", "system prompt");
    int inbox_calls = 0;
    loop.SetInbox([&]() -> std::optional<api::Message> {
        ++inbox_calls;
        // 第一次(也是唯一一次)收件边界:工具结果攒完、下一请求未发。
        if (inbox_calls == 1) {
            return IncomingText("[来自另一场会话的字条] 前端可以跟进了");
        }
        return std::nullopt;
    });

    REQUIRE(loop.Run("帮我用一下工具", agent::Callbacks{}).has_value());

    REQUIRE(backend.captured_requests.size() == 2);
    // 第二次请求的末条消息里既有 tool_result 也有来信文本(同一条 user)。
    const auto& second = backend.captured_requests[1];
    REQUIRE(second.messages.size() == 3);
    const auto& last = second.messages[2];
    REQUIRE(last.content.size() == 2);
    CHECK(std::holds_alternative<api::ToolResultBlock>(last.content[0]));
    const auto& note = std::get<api::TextBlock>(last.content[1]);
    CHECK(note.text.find("另一场会话") != std::string::npos);
    // history 也一样:3 条消息 + 末条含两块。
    REQUIRE(loop.history().size() == 4);
    REQUIRE(loop.history()[2].content.size() == 2);
}

TEST_CASE("收件点:权限确认当口来信不作答——信只在确认收口后的边界才注入") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_c", "confirm_tool"),
        TextOnlyScript("好"),
    };
    tools::ToolRegistry registry;

    class ConfirmTool : public tools::Tool {
    public:
        std::string name() const override { return "confirm_tool"; }
        std::string description() const override { return "needs confirm"; }
        nlohmann::json input_schema() const override { return nlohmann::json::object(); }
        bool needs_confirm() const override { return true; }
        tools::Tool::Result execute(const nlohmann::json&) override { return {"ok", false}; }
    };
    registry.Register(std::make_unique<ConfirmTool>());

    agent::Agent loop(backend, registry, "test-model", "system prompt");
    // 第一次边界(首个请求之前)就把信备着:即便信早就到了,第一个请求
    // (含权限确认那次工具往返)也不带它——注入只发生在工具结果攒完、
    // 下一请求尚未发出的边界,确认回调里根本没有看见它的路。
    loop.SetInbox([first = true]() mutable -> std::optional<api::Message> {
        if (first) {
            first = false;
            return IncomingText("[来自另一场会话的字条] 跟进");
        }
        return std::nullopt;
    });

    bool confirm_asked = false;
    agent::Callbacks callbacks;
    callbacks.on_tool_confirm = [&](const std::string&, const std::string&, const nlohmann::json&) {
        confirm_asked = true;
        return true;
    };

    REQUIRE(loop.Run("用一下要确认的工具", callbacks).has_value());
    CHECK(confirm_asked);

    REQUIRE(backend.captured_requests.size() == 2);
    const auto request_text = [](const api::Request& request) {
        std::string out;
        for (const auto& message : request.messages) {
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                    out += text->text;
                }
            }
        }
        return out;
    };
    CHECK(request_text(backend.captured_requests[0]).find("另一场会话") ==
          std::string::npos);  // 确认前的请求不带信
    CHECK(request_text(backend.captured_requests[1]).find("另一场会话") !=
          std::string::npos);  // 确认收口后才注入
}

TEST_CASE("收件点:不设或交空,行为跟从前完全一致") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("好")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, "test-model", "system prompt");
    loop.SetInbox([]() -> std::optional<api::Message> { return std::nullopt; });
    REQUIRE(loop.Run("问", agent::Callbacks{}).has_value());
    REQUIRE(loop.history().size() == 2);
}

TEST_CASE("收件点:同一边界多条排队消息,按落队顺序一并注入下一请求") {
    // 0.28.x 排队消息:一次工具边界可能攒下好几条,收件点把多条合成一条
    // user 消息(一块一条 TextBlock),InjectIncomingMessage 追加在 tool_result
    // 之后——顺序不可倒,不可丢。
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_q", "fake_tool"),
        TextOnlyScript("都收到了。"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::Agent loop(backend, registry, "test-model", "system prompt");
    loop.SetInbox([&]() -> std::optional<api::Message> {
        api::Message batch;
        batch.role = api::Role::User;
        batch.content.push_back(api::TextBlock{"[用户排队消息] 第一条:只看只读"});
        batch.content.push_back(api::TextBlock{"[用户排队消息] 第二条:证据列全"});
        batch.content.push_back(api::TextBlock{"[用户排队消息] 第三条:收尾报数"});
        static bool drained = false;
        if (!drained) {
            drained = true;
            return batch;
        }
        return std::nullopt;
    });

    REQUIRE(loop.Run("去查", agent::Callbacks{}).has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    const auto& last = backend.captured_requests[1].messages.back();
    REQUIRE(last.content.size() == 4);  // tool_result + 三块排队消息
    CHECK(std::holds_alternative<api::ToolResultBlock>(last.content[0]));  // 工具结果在前
    const auto text_of = [](const api::ContentBlock& block) {
        return std::get<api::TextBlock>(block).text;
    };
    const std::size_t first_pos = text_of(last.content[1]).find("第一条");
    const std::size_t second_pos = text_of(last.content[2]).find("第二条");
    const std::size_t third_pos = text_of(last.content[3]).find("第三条");
    REQUIRE(first_pos != std::string::npos);
    REQUIRE(second_pos != std::string::npos);
    REQUIRE(third_pos != std::string::npos);
}

TEST_CASE("收件点:无工具自然收尾,本轮不注入——排队消息留给收场后的会话泵") {
    // 规格:turn==0 不收件(这一轮的用户消息刚落下);模型没调工具直接
    // end_turn 时,Run 在第一个请求后就返回,收件点一次都不被调。排队消息
    // 不抢跑、不落在本轮,由调用方在收场后当下一轮用户消息发出。
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("答完了")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, "test-model", "system prompt");
    int inbox_calls = 0;
    loop.SetInbox([&]() -> std::optional<api::Message> {
        ++inbox_calls;
        return IncomingText("[用户排队消息] 不该出现在本轮");
    });

    REQUIRE(loop.Run("普通一问", agent::Callbacks{}).has_value());
    CHECK(inbox_calls == 0);  // 没有工具边界,收件点压根没被调
    REQUIRE(backend.captured_requests.size() == 1);
    for (const auto& message : backend.captured_requests[0].messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                CHECK(text->text.find("不该出现在本轮") == std::string::npos);
            }
        }
    }
    REQUIRE(loop.history().size() == 2);  // user + assistant,干干净净
}
