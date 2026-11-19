// 工具入参兜底(开源模型坏 JSON 单)的 loop 层:FakeBackend 按脚本吐坏
// 入参,钉住——坏 JSON 与长度截断不执行、配对的 is_error 结果回喂模型、
// 终态栅栏照落;修补过的照执行且结果附注不静默;schema 浅检拦缺必填与
// 类型错,数字字符串宽容转换。

#include <doctest/doctest.h>

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "api/backend.hpp"
#include "api/tool_args_repair.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端(同 test_loop 的款,删去取消/序列化分支)。
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

// 带 schema 的假写工具:path 必填 string,content string,timeout integer。
class SchemaTool : public tools::Tool {
public:
    std::string name() const override { return "write_file"; }
    std::string description() const override { return "fake write tool"; }
    nlohmann::json input_schema() const override {
        return nlohmann::json{
            {"type", "object"},
            {"properties",
             nlohmann::json{{"path", nlohmann::json{{"type", "string"}}},
                            {"content", nlohmann::json{{"type", "string"}}},
                            {"timeout", nlohmann::json{{"type", "integer"}}}}},
            {"required", nlohmann::json::array({"path"})},
        };
    }
    bool needs_confirm() const override { return false; }

    tools::Tool::Result execute(const nlohmann::json& input) override {
        ++call_count;
        last_input = input;
        return {"写好了", false};
    }

    int call_count = 0;
    nlohmann::json last_input;
};

std::vector<api::StreamEvent> ToolScript(const std::string& raw, const std::string& stop_reason = "tool_use") {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, "toolu_1", "write_file"},
        api::ToolUseInputDelta{0, raw},
        api::ContentBlockDone{0},
        api::MessageDone{stop_reason, api::Usage{}},
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

struct Outcome {
    int call_count = 0;
    nlohmann::json last_input;
    std::optional<api::ToolResultBlock> result;
    std::size_t requests = 0;
    bool second_request_paired = false;  // 第二次请求带着同 id 的 tool_result
    std::vector<agent::ToolTraceEvent> finished;
};

// 跑一轮:坏入参一步,随后模型收口一句话。tool_result 从 history[2] 取。
Outcome RunWith(const std::string& raw, const std::string& stop_reason = "tool_use") {
    FakeBackend backend;
    backend.scripts = {ToolScript(raw, stop_reason), TextOnlyScript("收到")};
    tools::ToolRegistry registry;
    auto owned = std::make_unique<SchemaTool>();
    SchemaTool* tool = owned.get();
    registry.Register(std::move(owned));
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    Outcome outcome;
    agent::TurnWiring callbacks;
    callbacks.on_tool_trace = [&outcome](const agent::ToolTraceEvent& event) {
        if (event.kind == agent::ToolTraceEventKind::ExecutionFinished) {
            outcome.finished.push_back(event);
        }
    };
    const auto run = loop.Run("写个文件", callbacks);
    REQUIRE(run.has_value());
    outcome.call_count = tool->call_count;
    outcome.last_input = tool->last_input;
    outcome.requests = backend.captured_requests.size();
    REQUIRE(loop.history().size() >= 3);
    const auto& tool_result_msg = loop.history()[2];
    REQUIRE(tool_result_msg.content.size() == 1);
    REQUIRE(std::holds_alternative<api::ToolResultBlock>(tool_result_msg.content[0]));
    outcome.result = std::get<api::ToolResultBlock>(tool_result_msg.content[0]);
    if (backend.captured_requests.size() >= 2 && backend.captured_requests[1].messages.size() >= 3) {
        const auto& paired = backend.captured_requests[1].messages[2];
        for (const auto& block : paired.content) {
            if (const auto* r = std::get_if<api::ToolResultBlock>(&block); r != nullptr && r->tool_use_id == "toolu_1") {
                outcome.second_request_paired = true;
            }
        }
    }
    return outcome;
}

}  // namespace

TEST_CASE("入参兜底 loop:坏 JSON 不执行,配对 is_error 结果写清工具名、病因与原文片段") {
    const Outcome outcome = RunWith("{path: a.txt, content: hi}");
    CHECK(outcome.call_count == 0);
    REQUIRE(outcome.result.has_value());
    CHECK(outcome.result->tool_use_id == "toolu_1");
    CHECK(outcome.result->is_error);
    CHECK(outcome.result->content.find("write_file") != std::string::npos);
    CHECK(outcome.result->content.find("不是合法 JSON") != std::string::npos);
    CHECK(outcome.result->content.find("{path: a.txt") != std::string::npos);
    CHECK(outcome.second_request_paired);
    CHECK(outcome.requests == 2);
    // 终态栅栏照落:稳定码 tool.input.invalid_json,按 schema 拒(确定没执行)。
    REQUIRE(outcome.finished.size() == 1);
    CHECK(outcome.finished[0].error_code == api::kErrToolInputInvalidJson);
    CHECK(outcome.finished[0].outcome == agent::ToolOutcome::SchemaRejected);
}

TEST_CASE("入参兜底 loop:max_tokens 截断的半截写入不补齐执行,错误点明拆小分多次写") {
    const Outcome outcome = RunWith(R"({"path":"big.txt","content":"第一段内容……)", "max_tokens");
    CHECK(outcome.call_count == 0);
    REQUIRE(outcome.result.has_value());
    CHECK(outcome.result->is_error);
    CHECK(outcome.result->content.find("截断") != std::string::npos);
    CHECK(outcome.result->content.find("拆小分多次写") != std::string::npos);
    CHECK(outcome.second_request_paired);
    REQUIRE(outcome.finished.size() == 1);
    CHECK(outcome.finished[0].error_code == api::kErrToolInputTruncated);
}

TEST_CASE("入参兜底 loop:尾逗号修补后照执行,结果尾巴附注修了什么") {
    const Outcome outcome = RunWith(R"({"path":"a.txt","content":"hi",})");
    CHECK(outcome.call_count == 1);
    CHECK(outcome.last_input.at("path") == "a.txt");
    REQUIRE(outcome.result.has_value());
    CHECK_FALSE(outcome.result->is_error);
    CHECK(outcome.result->content.find("写好了") != std::string::npos);
    CHECK(outcome.result->content.find("[宿主附注]") != std::string::npos);
    CHECK(outcome.result->content.find("逗号") != std::string::npos);
}

TEST_CASE("入参兜底 loop:schema 预检缺必填字段,不执行并写明缺哪个、期望类型") {
    const Outcome outcome = RunWith(R"({"content":"hi"})");
    CHECK(outcome.call_count == 0);
    REQUIRE(outcome.result.has_value());
    CHECK(outcome.result->is_error);
    CHECK(outcome.result->content.find("缺少必填字段 path") != std::string::npos);
    CHECK(outcome.result->content.find("string") != std::string::npos);
    CHECK(outcome.second_request_paired);
    REQUIRE(outcome.finished.size() == 1);
    CHECK(outcome.finished[0].error_code == api::kErrToolInputSchemaInvalid);
}

TEST_CASE("入参兜底 loop:schema 预检类型明显错,不执行并写明期望类型") {
    const Outcome outcome = RunWith(R"({"path":42})");
    CHECK(outcome.call_count == 0);
    REQUIRE(outcome.result.has_value());
    CHECK(outcome.result->is_error);
    CHECK(outcome.result->content.find("path") != std::string::npos);
    CHECK(outcome.result->content.find("类型应是 string") != std::string::npos);
}

TEST_CASE("入参兜底 loop:integer 字段收到数字字符串宽容转数,照执行并附注") {
    const Outcome outcome = RunWith(R"({"path":"a.txt","timeout":"120"})");
    CHECK(outcome.call_count == 1);
    REQUIRE(outcome.last_input.contains("timeout"));
    CHECK(outcome.last_input.at("timeout").is_number_integer());
    CHECK(outcome.last_input.at("timeout") == 120);
    REQUIRE(outcome.result.has_value());
    CHECK_FALSE(outcome.result->is_error);
    CHECK(outcome.result->content.find("120") != std::string::npos);
    CHECK(outcome.result->content.find("[宿主附注]") != std::string::npos);
}

TEST_CASE("入参兜底 loop:integer 字段给非数字串照拒;可选字段给 null 不拦") {
    {
        const Outcome outcome = RunWith(R"({"path":"a.txt","timeout":"soon"})");
        CHECK(outcome.call_count == 0);
        REQUIRE(outcome.result.has_value());
        CHECK(outcome.result->content.find("类型应是 integer") != std::string::npos);
    }
    {
        const Outcome outcome = RunWith(R"({"path":"a.txt","timeout":null})");
        CHECK(outcome.call_count == 1);
        REQUIRE(outcome.result.has_value());
        CHECK_FALSE(outcome.result->is_error);
        CHECK(outcome.result->content.find("[宿主附注]") == std::string::npos);
    }
}

TEST_CASE("入参兜底 loop:严格合法的入参一字不动,结果不带附注") {
    const Outcome outcome = RunWith(R"({"path":"a.txt","content":"hi","timeout":5})");
    CHECK(outcome.call_count == 1);
    REQUIRE(outcome.result.has_value());
    CHECK(outcome.result->content == "写好了");
}
