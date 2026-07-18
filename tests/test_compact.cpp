// agent/compact.hpp:BuildCompactedHistory 纯逻辑(archive + 最近一轮完整
// 对话,tool_use/tool_result 永远配对)、Compact() 用 FakeBackend 按脚本吐
// 事件(不碰真网络),验证请求怎么拼、失败时怎么处理。0.21.x 加钉:
// 压缩指令带固定栏目头、空/短摘要拒收、压缩前后 AgentLoop 的 system
// 逐字节不变(守护测试)。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "agent/compact.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"

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
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
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

// 空/短摘要拒收(40 字门槛)落地后,脚本里的"成功摘要"都得够长。旧断言
// 用的"摘要正文"(4 字)会被拒收,统一换成这份带栏目头的长摘要。
const std::string kLongSummary =
    "## 任务目标\n实现上下文管理与压缩\n## 已证实的事实\n关键文件是 config.cpp\n"
    "## 关键决策\n按栏目输出存档\n## 涉及文件与符号\nconfig.cpp\n"
    "## 关键命令与结果\n(无)\n## 未完成事项\n补齐单元测试与文档说明";

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

    // 存档正文并入保留轮第一条 user 消息的开头——不再单独成一条消息,
    // 不然存档 user 紧跟保留轮的 user 输入,相邻两条 user 违反角色交替。
    REQUIRE_FALSE(new_history.empty());
    CHECK(new_history[0].role == api::Role::User);
    REQUIRE(std::holds_alternative<api::TextBlock>(new_history[0].content[0]));
    const std::string& first_text = std::get<api::TextBlock>(new_history[0].content[0]).text;
    CHECK(first_text.find("对话存档") != std::string::npos);
    CHECK(first_text.find("摘要正文") != std::string::npos);
    CHECK(first_text.find("最近一次提问") != std::string::npos);
    // 存档在前,原始输入在后。
    CHECK(first_text.find("对话存档") < first_text.find("最近一次提问"));

    // 消息总数 = 保留轮的 4 条(第一条已与存档合并),后面紧跟 assistant。
    REQUIRE(new_history.size() == 4);
    CHECK(new_history[1].role == api::Role::Assistant);

    // 合并后不出现相邻两条 user 消息。
    for (std::size_t i = 0; i + 1 < new_history.size(); ++i) {
        const bool both_user =
            new_history[i].role == api::Role::User && new_history[i + 1].role == api::Role::User;
        CHECK_FALSE(both_user);
    }

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
    // 旧断言的短摘要"任务是实现 M6.6,关键文件是 config.cpp"(27 字)会被
    // 40 字门槛拒收,换成长摘要,原有关键短语保留。
    backend.script = SummaryScript(kLongSummary);

    std::vector<api::Message> history;
    history.push_back(UserText("帮我实现上下文管理"));
    history.push_back(AssistantText("好的,开始"));

    const auto result = agent::Compact(backend, "test-model", history, "");

    REQUIRE(result.has_value());
    CHECK(result->role == api::Role::User);
    REQUIRE(std::holds_alternative<api::TextBlock>(result->content[0]));
    const std::string& text = std::get<api::TextBlock>(result->content[0]).text;
    CHECK(text.find("[对话存档,此前内容已压缩]") == 0);
    CHECK(text.find("关键文件是 config.cpp") != std::string::npos);

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
    backend.script = SummaryScript(kLongSummary);

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
    backend.script = SummaryScript(kLongSummary);

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, "数据库连接字符串");

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].system.find("重点保留:数据库连接字符串") != std::string::npos);
}

TEST_CASE("Compact: focus 为空时,请求的 system 指令里不出现 重点保留") {
    FakeBackend backend;
    backend.script = SummaryScript(kLongSummary);

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

TEST_CASE("Compact: 压缩系统指令带全部固定栏目头,只写确证不许猜补") {
    FakeBackend backend;
    backend.script = SummaryScript(kLongSummary);

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, "");

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    const std::string& system = backend.captured_requests[0].system;
    for (const char* header : {"## 任务目标", "## 已证实的事实", "## 关键决策", "## 涉及文件与符号",
                                "## 关键命令与结果", "## 未完成事项"}) {
        CHECK(system.find(header) != std::string::npos);
    }
    CHECK(system.find("不许猜补") != std::string::npos);
}

TEST_CASE("Compact: focus 非空时,系统指令另带 ## 重点保留 栏目头") {
    FakeBackend backend;
    backend.script = SummaryScript(kLongSummary);

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, "数据库连接字符串");

    REQUIRE(result.has_value());
    CHECK(backend.captured_requests[0].system.find("## 重点保留") != std::string::npos);
}

TEST_CASE("Compact: 摘要剥空白后为空 → 拒收,返回错误不给存档") {
    FakeBackend backend;
    backend.script = SummaryScript("  \n\t  \r\n ");

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, "");

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("历史未动") != std::string::npos);
}

TEST_CASE("Compact: 摘要不足 40 字 → 拒收(prefill continuation 只回一个字的老坑)") {
    FakeBackend backend;
    backend.script = SummaryScript("2");  // 实测出过:模型顺着最后一条 assistant 只接一个"2"

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));
    history.push_back(AssistantText("2"));

    const auto result = agent::Compact(backend, "test-model", history, "");

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("过短") != std::string::npos);
    CHECK(result.error().message.find("历史未动") != std::string::npos);
}

TEST_CASE("Compact: 刚过 40 字门槛的摘要照常收下") {
    FakeBackend backend;
    // 正好 40 个字符(码点):门槛是"不足 40 拒收",40 整该收。
    std::string exactly40;
    for (int i = 0; i < 40; ++i) {
        exactly40 += "查";
    }
    backend.script = SummaryScript(exactly40);

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, "");
    REQUIRE(result.has_value());
}

// ---------------------------------------------------------------------------
// 守护测试(0.21.x):压缩只动 history,绝不动 system——AgentLoop 构造时
// 定死的系统提示,压缩前后发出去的请求里必须逐字节一致。用记录请求的
// FakeBackend 跑一轮、压缩替换历史、再跑一轮,对比两次的 Request.system。
// ---------------------------------------------------------------------------
TEST_CASE("守护:压缩前后 AgentLoop 发出的 system 逐字节不变") {
    const std::string system_prompt =
        "系统提示原文——含人格段、环境段、features 段,一个字节都不许因压缩而变。";

    FakeBackend loop_backend;
    loop_backend.script = SummaryScript("这一轮的普通回答,不短,凑够字数免得跟压缩门槛混为一谈。");
    tools::ToolRegistry registry;  // 空表:这轮不调工具
    agent::AgentLoop loop(loop_backend, registry, "test-model", system_prompt);

    REQUIRE(loop.Run("第一问", agent::Callbacks{}).has_value());
    REQUIRE_FALSE(loop_backend.captured_requests.empty());
    const std::string system_before = loop_backend.captured_requests.front().system;
    CHECK(system_before == system_prompt);

    // 压缩走独立后端(跟 main.cpp 用裸 real_backend 一个路数),替换历史。
    FakeBackend compact_backend;
    compact_backend.script = SummaryScript(kLongSummary);
    const auto archive = agent::Compact(compact_backend, "test-model", loop.History(), "");
    REQUIRE(archive.has_value());
    loop.ReplaceHistory(agent::BuildCompactedHistory(loop.History(), *archive));

    REQUIRE(loop.Run("第二问", agent::Callbacks{}).has_value());
    const std::string system_after = loop_backend.captured_requests.back().system;

    // 逐字节:压缩前后完全一致,且就是构造时那份。
    CHECK(system_after == system_before);
    CHECK(system_after == system_prompt);
    // 顺带钉死:压缩后的历史确实换了(存档进了第一条 user 消息)。
    bool archived = false;
    for (const auto& message : loop_backend.captured_requests.back().messages) {
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::TextBlock>(block) &&
                std::get<api::TextBlock>(block).text.find("对话存档") != std::string::npos) {
                archived = true;
            }
        }
    }
    CHECK(archived);
}
