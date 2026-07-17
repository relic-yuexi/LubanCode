// agent/compact.hpp:BuildCompactedHistory 纯逻辑(archive + 最近一轮完整
// 对话,tool_use/tool_result 永远配对)、Compact() 用 FakeBackend 按脚本吐
// 事件(不碰真网络),验证请求怎么拼、失败时怎么处理。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "agent/compact.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"

using namespace lubancode;

namespace {

api::Message UserText(const std::string& text) {
    api::Message m;
    m.role = api::Role::User;
    m.content.push_back(api::TextBlock{text});
    return m;
}

api::Message AssistantText(const std::string& text) {
    api::Message m;
    m.role = api::Role::Assistant;
    m.content.push_back(api::TextBlock{text});
    return m;
}

api::Message AssistantToolUse(const std::string& id, const std::string& name) {
    api::Message m;
    m.role = api::Role::Assistant;
    m.content.push_back(api::ToolUseBlock{id, name, nlohmann::json::object()});
    return m;
}

api::Message UserToolResult(const std::string& tool_use_id, const std::string& content) {
    api::Message m;
    m.role = api::Role::User;
    m.content.push_back(api::ToolResultBlock{tool_use_id, content, false});
    return m;
}

// 按脚本吐事件的假后端,复用 test_loop.cpp 里那套写法:记下收到的
// Request,方便断言 system 里带没带 focus 文本、messages 是不是整份历史。
class FakeBackend : public api::Backend {
public:
    std::vector<api::StreamEvent> script;
    bool fail = false;
    std::string fail_message;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event) override {
        captured_requests.push_back(request);
        if (fail) {
            return std::unexpected(api::Error{api::ErrorKind::Network, fail_message, 0});
        }
        for (const auto& event : script) {
            on_event(event);
        }
        return {};
    }
};

std::vector<api::StreamEvent> SummaryScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

}  // namespace

TEST_CASE("BuildCompactedHistory: 新历史 = archive + 最近一轮完整对话") {
    std::vector<api::Message> history;
    history.push_back(UserText("第一句话,提到一个名字:张三"));
    history.push_back(AssistantText("好的,记住了"));
    history.push_back(UserText("最近一次提问"));
    history.push_back(AssistantToolUse("tool_1", "read_file"));
    history.push_back(UserToolResult("tool_1", "文件内容"));
    history.push_back(AssistantText("读完了"));

    api::Message archive = UserText("[对话存档,此前内容已压缩] 摘要正文");
    const auto new_history = agent::BuildCompactedHistory(history, archive);

    // archive 打头
    REQUIRE_FALSE(new_history.empty());
    CHECK(new_history[0].role == api::Role::User);
    REQUIRE(std::holds_alternative<api::TextBlock>(new_history[0].content[0]));
    CHECK(std::get<api::TextBlock>(new_history[0].content[0]).text.find("对话存档") != std::string::npos);

    // 后面接的是"最近一次提问"开始的那一轮,不是"第一句话"那一轮
    REQUIRE(new_history.size() == 1 + 4);
    REQUIRE(std::holds_alternative<api::TextBlock>(new_history[1].content[0]));
    CHECK(std::get<api::TextBlock>(new_history[1].content[0]).text == "最近一次提问");

    // "第一句话"/"张三"不该出现在新历史里(已经被压缩进 archive,不在这里重复)
    bool found_old = false;
    for (const auto& message : new_history) {
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::TextBlock>(block)) {
                if (std::get<api::TextBlock>(block).text.find("张三") != std::string::npos) {
                    found_old = true;
                }
            }
        }
    }
    CHECK_FALSE(found_old);
}

TEST_CASE("BuildCompactedHistory: tool_use 和 tool_result 永远配对,不会被切开") {
    std::vector<api::Message> history;
    history.push_back(UserText("问题一"));
    history.push_back(AssistantText("回答一"));
    history.push_back(UserText("问题二,需要用工具"));
    history.push_back(AssistantToolUse("tool_9", "search"));
    history.push_back(UserToolResult("tool_9", "搜索结果"));
    history.push_back(AssistantText("回答二"));

    api::Message archive = UserText("[对话存档,此前内容已压缩] 摘要");
    const auto new_history = agent::BuildCompactedHistory(history, archive);

    for (std::size_t i = 0; i < new_history.size(); ++i) {
        for (const auto& block : new_history[i].content) {
            if (std::holds_alternative<api::ToolUseBlock>(block)) {
                const auto& tool_use = std::get<api::ToolUseBlock>(block);
                REQUIRE(i + 1 < new_history.size());
                bool found_pair = false;
                for (const auto& next_block : new_history[i + 1].content) {
                    if (std::holds_alternative<api::ToolResultBlock>(next_block) &&
                        std::get<api::ToolResultBlock>(next_block).tool_use_id == tool_use.id) {
                        found_pair = true;
                    }
                }
                CHECK(found_pair);
            }
        }
    }
}

TEST_CASE("BuildCompactedHistory: 历史里找不到真正的用户文本输入时,只留 archive") {
    std::vector<api::Message> history;
    history.push_back(UserToolResult("tool_x", "孤立的工具结果"));  // 角色是 user,但不含 TextBlock

    api::Message archive = UserText("[对话存档,此前内容已压缩] 摘要");
    const auto new_history = agent::BuildCompactedHistory(history, archive);

    REQUIRE(new_history.size() == 1);
    CHECK(std::get<api::TextBlock>(new_history[0].content[0]).text.find("对话存档") != std::string::npos);
}

TEST_CASE("BuildCompactedHistory: 空历史,新历史只有 archive") {
    std::vector<api::Message> history;
    api::Message archive = UserText("[对话存档,此前内容已压缩] 摘要");
    const auto new_history = agent::BuildCompactedHistory(history, archive);
    REQUIRE(new_history.size() == 1);
}

TEST_CASE("Compact: 成功时返回 user 角色的存档消息,content 带固定前缀和模型给出的正文") {
    FakeBackend backend;
    backend.script = SummaryScript("任务是实现 M6.6,关键文件是 config.cpp");

    std::vector<api::Message> history;
    history.push_back(UserText("帮我实现上下文管理"));
    history.push_back(AssistantText("好的,开始"));

    const auto result = agent::Compact(backend, "test-model", history, "");

    REQUIRE(result.has_value());
    CHECK(result->role == api::Role::User);
    REQUIRE(std::holds_alternative<api::TextBlock>(result->content[0]));
    const std::string& text = std::get<api::TextBlock>(result->content[0]).text;
    CHECK(text.find("[对话存档,此前内容已压缩]") == 0);
    CHECK(text.find("任务是实现 M6.6") != std::string::npos);

    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].model == "test-model");
    // history 最后一条是 assistant("好的,开始"),会被补一条 user 收尾消息,
    // 避免 API 把整个请求当成"续写最后一条 assistant 消息"处理(见
    // compact.cpp 里的坑),所以这里是 history.size() + 1,不是 history.size()。
    REQUIRE(backend.captured_requests[0].messages.size() == history.size() + 1);
    CHECK(backend.captured_requests[0].messages.back().role == api::Role::User);
}

TEST_CASE("Compact: history 最后一条已经是 user 角色时,不额外补收尾消息") {
    FakeBackend backend;
    backend.script = SummaryScript("摘要正文");

    std::vector<api::Message> history;
    history.push_back(UserText("问题一"));
    history.push_back(AssistantText("回答一"));
    history.push_back(UserToolResult("tool_1", "工具结果"));  // 最后一条角色是 user

    const auto result = agent::Compact(backend, "test-model", history, "");

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].messages.size() == history.size());
}

TEST_CASE("Compact: focus 非空时,请求的 system 指令里带上 重点保留:xxx") {
    FakeBackend backend;
    backend.script = SummaryScript("摘要正文");

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, "数据库连接字符串");

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].system.find("重点保留:数据库连接字符串") != std::string::npos);
}

TEST_CASE("Compact: focus 为空时,请求的 system 指令里不出现 重点保留") {
    FakeBackend backend;
    backend.script = SummaryScript("摘要正文");

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, "");

    REQUIRE(result.has_value());
    CHECK(backend.captured_requests[0].system.find("重点保留") == std::string::npos);
}

TEST_CASE("Compact: 后端失败时返回错误,不影响传入的原始 history(调用方该继续用旧历史)") {
    FakeBackend backend;
    backend.fail = true;
    backend.fail_message = "连接超时";

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));
    history.push_back(AssistantText("回答"));
    const std::vector<api::Message> history_copy = history;

    const auto result = agent::Compact(backend, "test-model", history, "");

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == "连接超时");
    // history 本来就是 const&,压缩失败不该也不会改动它。
    REQUIRE(history.size() == history_copy.size());
    CHECK(std::get<api::TextBlock>(history[0].content[0]).text ==
          std::get<api::TextBlock>(history_copy[0].content[0]).text);
}

TEST_CASE("Compact: 流内报 StreamError 也算失败,返回错误") {
    FakeBackend backend;
    backend.script = {
        api::MessageStart{"msg", "model"},
        api::StreamError{"服务器繁忙"},
    };

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, "");

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("服务器繁忙") != std::string::npos);
}
