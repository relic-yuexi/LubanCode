// 请求级恢复经 AgentLoop 的合同测试(监督器单 P0-1 验收线):本地假后端
// 造各路故障——连接复位(connect reset)、半截正文断流、半截 tool JSON 断流、
// 工具结果已提交后下一请求断流——每路都按合同收口:
//   * 首字节前/未提交断流可安全重发:从同一提交边界重来,attempt 连号;
//   * 半截 text 不拼两段正文:history 里只有成功那份完整消息;
//   * 半截 tool JSON 零执行:assembler 没收口,工具一个不跑;
//   * 已提交的 ToolResult 绝不重放:工具只执行一次,重发的只是模型请求;
//   * 重试用尽:结构化收口,文案如实写"已自动重试 N 次"。
// 主路与子路共用这一环(两路都经 AgentLoop::Run),这里钉主路;子路台账
// 回滚在 test_agent_progress 钉。

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "agent/turn_budget.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/middleware_runtime.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 故障注入的假后端:每调一次 send_stream 按脚本走;脚本项 = (事件序列, 收场)。
// 收场为 nullopt = 流式成功;有值 = 返回那枚错误(半截断流:事件先吐,再断)。
class FlakyBackend : public api::Backend {
public:
    struct Take {
        std::vector<api::StreamEvent> events;
        std::optional<api::Error> error;
    };
    std::vector<Take> script;
    int calls = 0;
    std::vector<std::size_t> message_counts;  // 每次请求带的消息条数(重发边界对账)

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        message_counts.push_back(request.messages.size());
        const std::size_t idx = static_cast<std::size_t>(calls++);
        if (idx >= script.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FlakyBackend: 脚本用完了", 0});
        }
        for (const auto& event : script[idx].events) {
            on_event(event);
        }
        if (script[idx].error.has_value()) {
            return std::unexpected(*script[idx].error);
        }
        return {};
    }
};

class CountingTool : public tools::Tool {
public:
    std::string name() const override { return "counting_tool"; }
    std::string description() const override { return "counts calls"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++call_count;
        return {"第 " + std::to_string(call_count) + " 次执行", false};
    }
    int call_count = 0;
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolScript(const std::string& tool_id) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, "counting_tool"},
        api::ToolUseInputDelta{0, "{\"n\":1}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

api::Error ConnectReset() { return api::Error{api::ErrorKind::Network, "Connection reset by peer", 0}; }

struct Turn {
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter;
    Turn() : adapter("test", ids) { adapter.Start(); }
};

// P1-C(失败与恢复单 FA-03)的边界录音替身:prepared/sent 逐枚记账,sent
// 的成败可按请求序号脚本化——发第 N 枚请求时 sent 记不住就在它身上拦。
class SentGateRecorder final : public agent::LoopBoundaryRecorder {
public:
    // 第几枚请求(1 起)的 sent 记不住;0 = 全部落稳。
    int fail_sent_on_request = 0;
    int prepared = 0;
    int sent = 0;
    std::vector<std::string> request_ids;
    std::vector<std::string> response_started_for;

    std::string OnRequestPrepared(const api::Request&, const agent::RequestPreparedContext&) override {
        ++prepared;
        request_ids.push_back("req-" + std::to_string(prepared));
        return request_ids.back();
    }
    bool OnRequestSent(const std::string& request_id) override {
        ++sent;
        if (fail_sent_on_request == sent) {
            return false;  // 发送前最后一笔本地账写不住
        }
        sent_ok_.push_back(request_id);
        return true;
    }
    void OnResponseStarted(const std::string& request_id) override {
        response_started_for.push_back(request_id);
    }
    void OnUsageRecorded(const std::string&, const api::Usage&, bool, const std::string&, int, bool,
                         bool, bool, const std::string&) override {}
    bool OnOutputCompleted(const std::string&, const api::Message&, const std::string&,
                           const std::string&) override {
        return true;
    }
    void OnOutputFailed(const std::string&, const std::string&) override {}
    void OnOutputCancelled(const std::string&, agent::OutputCancelSource) override {}

    // 落稳 sent 的请求(按序)。
    const std::vector<std::string>& sent_ok() const { return sent_ok_; }

private:
    std::vector<std::string> sent_ok_;
};

class ModelSendGateBackend final : public FlakyBackend {
public:
    std::vector<nlohmann::json> actual_requests;
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>* cancel) override {
        actual_requests.push_back(runtime::BuildRequestSnapshotJson(request));
        return FlakyBackend::send_stream(request, emit, cancel);
    }
};

class ModelSendGateRecorder final : public agent::LoopBoundaryRecorder {
public:
    std::vector<api::Request> prepared;
    std::vector<std::string> sent;
    std::vector<std::pair<std::string, std::string>> failed;
    std::vector<std::string> completed;
    std::vector<std::string> cancelled;
    std::vector<std::string> responses;
    std::vector<bool> usage_reported;
    std::string OnRequestPrepared(const api::Request& request, const agent::RequestPreparedContext&) override {
        prepared.push_back(request);
        return "gate-request-" + std::to_string(prepared.size());
    }
    bool OnRequestSent(const std::string& id) override { sent.push_back(id); return true; }
    void OnResponseStarted(const std::string& id) override { responses.push_back(id); }
    void OnUsageRecorded(const std::string&, const api::Usage&, bool reported, const std::string&,
                         int, bool, bool, bool, const std::string&) override {
        usage_reported.push_back(reported);
    }
    bool OnOutputCompleted(const std::string& id, const api::Message&, const std::string&,
                           const std::string&) override { completed.push_back(id); return true; }
    void OnOutputFailed(const std::string& id, const std::string& reason) override {
        failed.emplace_back(id, reason);
    }
    void OnOutputCancelled(const std::string& id, agent::OutputCancelSource) override {
        cancelled.push_back(id);
    }
};

void CheckActualModelSendGate() {
    std::vector<nlohmann::json> empty_gate_requests;
    for (const std::string mode : {"empty", "allow", "deny", "empty-deny", "throw", "nonstd-throw", "retry-deny",
                                   "retry-throw", "cancel"}) {
        CAPTURE(mode);
        const bool succeeds = mode == "empty" || mode == "allow";
        const bool retry_denied = mode == "retry-deny" || mode == "retry-throw";
        const bool has_first_send = succeeds || retry_denied;
        bool gate_allowed = mode == "allow" || retry_denied;
        const std::string secret = "private-policy-material-must-not-leak";
        ModelSendGateBackend backend;
        backend.script = {{{}, ConnectReset()}, {TextScript("permitted response"), std::nullopt}};
        tools::ToolRegistry registry;
        agent::Agent loop(backend, registry,
            agent::AgentProfile{.request{.model = "gate-model"}, .system_prompt = "gate-system"});
        ModelSendGateRecorder recorder;
        agent::TurnBudgetAccount account(2);
        auto budget = agent::MakeLocalTurnBudgetGate(&account);
        std::atomic<bool> cancel{false};
        std::vector<api::ModelRequestAttempt> gated_attempts;
        std::vector<agent::ModelTurnBudgetSnapshot> gated_budgets;
        std::vector<std::size_t> prepared_at_gate;
        std::vector<std::size_t> sent_at_gate;
        std::vector<api::Request> gate_requests;
        std::vector<api::ModelRequestAttempt> terminal_attempts;
        if (mode != "empty") {
            agent::AgentWiring host;
            host.on_model_send_gate = [&](const api::Request& request, const api::ModelRequestAttempt& attempt)
                -> std::expected<void, std::string> {
                gate_requests.push_back(request);
                gated_attempts.push_back(attempt);
                gated_budgets.push_back(account.SnapshotLock());
                prepared_at_gate.push_back(recorder.prepared.size());
                sent_at_gate.push_back(recorder.sent.size());
                if (mode == "cancel") { cancel.store(true); return {}; }
                if (gate_allowed) return {};
                if (mode == "throw" || mode == "retry-throw") throw std::runtime_error(secret);
                if (mode == "nonstd-throw") throw 7;
                return std::unexpected(mode == "empty-deny" ? std::string() : secret);
            };
            loop.SetWiring(std::move(host));
        }
        int pre_request_calls = 0;
        int backoffs = 0;
        agent::TurnWiring turn;
        turn.boundary_recorder = &recorder;
        turn.turn_budget = &budget;
        turn.on_pre_request_hooks = [&](const auto&, const auto&, const auto&, const auto&) {
            ++pre_request_calls;
            return std::string();
        };
        turn.wait_request_backoff = [&](std::chrono::milliseconds, const std::atomic<bool>*) {
            ++backoffs;
            if (retry_denied) gate_allowed = false; // Host revokes between actual send attempts.
            return true;
        };
        turn.on_request_attempt = [&](const api::ModelRequestAttempt& attempt, api::RequestAttemptPhase phase) {
            if (phase == api::RequestAttemptPhase::Exhausted) terminal_attempts.push_back(attempt);
        };
        const auto result = loop.Run("actual gated request", turn, &cancel);
        const auto snapshot = account.SnapshotLock();
        const std::size_t attempts = has_first_send ? 2 : 1;
        CHECK(pre_request_calls == 1); // The old hook is not a per-retry gate.
        CHECK(recorder.prepared.size() == attempts);
        CHECK(backend.calls == (succeeds ? 2 : retry_denied ? 1 : 0));
        CHECK(recorder.sent.size() == static_cast<std::size_t>(backend.calls));
        CHECK(backoffs == (has_first_send ? 1 : 0));
        CHECK(snapshot.reserved == 0);
        CHECK(snapshot.attempted == (has_first_send ? 1 : 0));
        CHECK(snapshot.completed == (succeeds ? 1 : 0));
        if (mode != "empty") {
            REQUIRE(gated_attempts.size() == attempts);
            for (std::size_t i = 0; i < attempts; ++i) {
                CHECK(gated_attempts[i].attempt == static_cast<int>(i + 1));
                CHECK_FALSE(gated_attempts[i].saw_headers);
                CHECK_FALSE(gated_attempts[i].saw_stream_event);
                CHECK_FALSE(gated_attempts[i].history_commit_hash.empty());
                CHECK(gated_attempts[i].logical_request_id == gated_attempts[0].logical_request_id);
                CHECK(gated_attempts[i].history_commit_hash == gated_attempts[0].history_commit_hash);
                CHECK(prepared_at_gate[i] == i + 1);
                CHECK(sent_at_gate[i] == i);
                CHECK(gated_budgets[i].reserved == (i == 0 ? 1 : 0));
                CHECK(gated_budgets[i].attempted == (i == 0 ? 0 : 1));
                CHECK(gate_requests[i].model == "gate-model");
                CHECK(gate_requests[i].system == "gate-system");
                CHECK(gate_requests[i].messages.size() == recorder.prepared[i].messages.size());
                CHECK(runtime::BuildRequestSnapshotJson(gate_requests[i]) ==
                      runtime::BuildRequestSnapshotJson(recorder.prepared[i]));
                if (i < backend.actual_requests.size()) {
                    CHECK(runtime::BuildRequestSnapshotJson(gate_requests[i]) == backend.actual_requests[i]);
                }
            }
        }
        if (succeeds) {
            REQUIRE(result.has_value());
            CHECK_FALSE(result->cancelled);
            CHECK(recorder.failed.empty());
            CHECK(recorder.cancelled.empty());
            REQUIRE(recorder.completed.size() == 1);
            CHECK(recorder.completed[0] == "gate-request-2");
            REQUIRE(recorder.responses.size() == 1);
            CHECK(recorder.responses[0] == "gate-request-2");
            REQUIRE(backend.message_counts.size() == 2);
            CHECK(backend.message_counts[0] == backend.message_counts[1]);
            REQUIRE(loop.History().size() == 2);
            REQUIRE_FALSE(loop.History().back().content.empty());
            REQUIRE(std::holds_alternative<api::TextBlock>(loop.History().back().content.front()));
            CHECK(std::get<api::TextBlock>(loop.History().back().content.front()).text == "permitted response");
            if (mode == "empty") empty_gate_requests = backend.actual_requests;
            else CHECK(backend.actual_requests == empty_gate_requests);
        } else if (mode == "cancel") {
            REQUIRE(result.has_value());
            CHECK(result->cancelled);
            CHECK(recorder.failed.empty());
            CHECK(recorder.completed.empty());
            CHECK(recorder.responses.empty());
            REQUIRE(recorder.cancelled.size() == 1);
            CHECK(recorder.cancelled[0] == "gate-request-1");
            REQUIRE(recorder.usage_reported.size() == 1);
            CHECK_FALSE(recorder.usage_reported[0]);
        } else {
            REQUIRE_FALSE(result.has_value());
            const std::string code = mode == "throw" || mode == "retry-throw" || mode == "nonstd-throw"
                ? "model.send.gate_exception" : "model.send.denied";
            CHECK(result.error().find(code) != std::string::npos);
            CHECK(result.error().find(secret) == std::string::npos);
            REQUIRE(recorder.failed.size() == 1);
            CHECK(recorder.failed[0].first == "gate-request-" + std::to_string(attempts));
            CHECK(recorder.failed[0].second == code);
            REQUIRE(terminal_attempts.size() == 1);
            CHECK(terminal_attempts[0].error_code == "api." + code);
            CHECK(recorder.completed.empty());
            CHECK(recorder.responses.empty());
            CHECK(recorder.usage_reported.empty());
            CHECK(loop.History().size() == 1);
            if (retry_denied) {
                REQUIRE(recorder.sent.size() == 1);
                CHECK(recorder.sent[0] == "gate-request-1");
            }
        }
    }

    // Existing trace and budget callbacks are outside the new gate catch.
    for (const bool trace_throws : {true, false}) {
        FlakyBackend backend;
        backend.script = {{TextScript("must not be sent"), std::nullopt}};
        tools::ToolRegistry registry;
        agent::Agent loop(backend, registry,
            agent::AgentProfile{.request{.model = "gate-model"}, .system_prompt = "gate-system"});
        ModelSendGateRecorder recorder;
        agent::TurnBudgetAccount account(2);
        auto budget = agent::MakeLocalTurnBudgetGate(&account);
        int gate_calls = 0;
        agent::AgentWiring host;
        host.on_model_send_gate = [&](const auto&, const auto&) -> std::expected<void, std::string> {
            ++gate_calls;
            return {};
        };
        loop.SetWiring(std::move(host));
        agent::TurnWiring turn;
        turn.boundary_recorder = &recorder;
        if (trace_throws) {
            turn.on_request_attempt = [](const auto&, auto) { throw std::runtime_error("original trace error"); };
            CHECK_THROWS_WITH(loop.Run("trace failure", turn), "original trace error");
            CHECK(gate_calls == 0);
        } else {
            budget.commit_sent = [&](const agent::ModelTurnPermit& permit) -> std::expected<int, std::string> {
                account.AbortBeforeSendLock(permit);
                throw std::runtime_error("original budget error");
            };
            turn.turn_budget = &budget;
            CHECK_THROWS_WITH(loop.Run("budget failure", turn), "original budget error");
            CHECK(gate_calls == 1);
        }
        CHECK(backend.calls == 0);
        CHECK(recorder.sent.empty());
        CHECK(recorder.failed.empty());
        CHECK(account.SnapshotLock().attempted == 0);
        CHECK(account.SnapshotLock().reserved == 0);
    }
}

}  // namespace

TEST_CASE("断流恢复:假后端前三次 503,第 4 次尝试恢复成功") {
    FlakyBackend backend;
    for (int i = 0; i < 3; ++i) {
        backend.script.push_back({{}, api::Error{api::ErrorKind::HttpStatus, "overloaded", 503}});
    }
    backend.script.push_back({TextScript("第四次恢复"), std::nullopt});
    api::Request request;
    api::RequestRecoveryHooks hooks;
    hooks.wait_backoff = [](std::chrono::milliseconds, const std::atomic<bool>*) { return true; };
    int events = 0;
    const auto result = api::SendStreamWithRecovery(
        backend, request, [&events](const api::StreamEvent&) { ++events; }, hooks, nullptr);
    REQUIRE(result.has_value());
    CHECK(backend.calls == 4);
    CHECK(events == 4);
}

TEST_CASE("断流恢复:HTTP 200 的 upstream_error 事件进入重试环后恢复") {
    FlakyBackend backend;
    backend.script = {
        {{api::StreamError{"Upstream request failed (code=upstream_error)", "upstream_error"}}, std::nullopt},
        {TextScript("上游恢复后的完整回答"), std::nullopt},
    };
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.wait_request_backoff = [](std::chrono::milliseconds, const std::atomic<bool>*) { return true; };

    const auto result = loop.Run("问一句", wiring);

    REQUIRE(result.has_value());
    CHECK(backend.calls == 2);
    REQUIRE(loop.History().size() == 2);
    REQUIRE(std::holds_alternative<api::TextBlock>(loop.History().back().content.front()));
    CHECK(std::get<api::TextBlock>(loop.History().back().content.front()).text == "上游恢复后的完整回答");
}

TEST_CASE("断流恢复:确定性 Api code 零重试") {
    FlakyBackend backend;
    backend.script = {
        {{api::StreamError{"参数不合法 (code=invalid_request)", "invalid_request"}}, std::nullopt},
        {TextScript("不应发送第二次"), std::nullopt},
    };
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.wait_request_backoff = [](std::chrono::milliseconds, const std::atomic<bool>*) { return true; };

    const auto result = loop.Run("问一句", wiring);

    REQUIRE_FALSE(result.has_value());
    CHECK(backend.calls == 1);
    CHECK(result.error().find("参数不合法") != std::string::npos);
}

TEST_CASE("断流恢复:首字节都没到就断,重发后整轮跑完,attempt 连号") {
    FlakyBackend backend;
    backend.script = {
        {/*空*/},
        {TextScript("网络回来后的完整回答"), std::nullopt},
    };
    backend.script[0].error = ConnectReset();
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    const auto result = loop.Run("问一句", wiring);
    REQUIRE(result.has_value());
    CHECK(backend.calls == 2);
    // 两次请求同一条消息(1 条 user):同一提交边界重发。
    REQUIRE(backend.message_counts.size() == 2);
    CHECK(backend.message_counts[0] == backend.message_counts[1]);
    REQUIRE(loop.History().size() == 2);
    bool saw_text = false;
    for (const auto& block : loop.History().back().content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            saw_text = true;
            CHECK(text->text == "网络回来后的完整回答");  // 半截的不算,只有完整那份
        }
    }
    CHECK(saw_text);
}

TEST_CASE("断流恢复:半截正文断流,history 只留成功那份,不拼两段") {
    FlakyBackend backend;
    backend.script = {
        {std::vector<api::StreamEvent>{
             api::MessageStart{"msg", "model"},
             api::TextDelta{"半截的话还没说完"},
         },
         ConnectReset()},
        {TextScript("重取后的完整回答"), std::nullopt},
    };
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    const auto result = loop.Run("继续说", wiring);
    REQUIRE(result.has_value());
    CHECK(backend.calls == 2);
    for (const auto& block : loop.History().back().content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            CHECK(text->text.find("半截的话") == std::string::npos);  // 断流那份不落地
        }
    }
}

TEST_CASE("断流恢复:半截 tool JSON 断流,工具零执行;重发后完整消息才跑工具") {
    FlakyBackend backend;
    backend.script = {
        {std::vector<api::StreamEvent>{
             api::MessageStart{"msg", "model"},
             api::ToolUseStart{0, "toolu_1", "counting_tool"},
             api::ToolUseInputDelta{0, "{\"n\""},  // JSON 劈在半道
         },
         ConnectReset()},
        {ToolScript("toolu_2"), std::nullopt},
        {TextScript("工具跑完,收口"), std::nullopt},
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<CountingTool>();
    CountingTool* counting = tool.get();
    registry.Register(std::move(tool));
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    const auto result = loop.Run("跑个工具", wiring);
    REQUIRE(result.has_value());
    CHECK(counting->call_count == 1);  // 半截那次零执行,重发后完整消息跑一次
    CHECK(backend.calls == 3);
}

TEST_CASE("断流恢复:工具结果已提交,下一请求断流重试不重跑工具") {
    FlakyBackend backend;
    backend.script = {
        {ToolScript("toolu_1"), std::nullopt},   // 工具执行,结果入 history
        {/*空*/},                                 // 下一请求:连接复位
        {TextScript("重试后的最终结论"), std::nullopt},
    };
    backend.script[1].error = ConnectReset();
    tools::ToolRegistry registry;
    auto tool = std::make_unique<CountingTool>();
    CountingTool* counting = tool.get();
    registry.Register(std::move(tool));
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    const auto result = loop.Run("跑完再说", wiring);
    REQUIRE(result.has_value());
    CHECK(counting->call_count == 1);  // 已提交的 ToolResult 绝不重放
    // 重发的第二次请求带着工具结果(3 条消息:1 user + 1 assistant + 1 tool_result)。
    REQUIRE(backend.message_counts.size() == 3);
    CHECK(backend.message_counts[1] == 3);
    CHECK(backend.message_counts[2] == 3);
}

TEST_CASE("断流恢复:重试用尽,结构化收口并如实写重试次数") {
    FlakyBackend backend;
    backend.script.resize(api::kMaxRequestAttempts);
    for (auto& take : backend.script) {
        take.error = ConnectReset();
    }
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.wait_request_backoff = [](std::chrono::milliseconds, const std::atomic<bool>*) { return true; };
    const auto result = loop.Run("必失败", wiring);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("已自动重试 5 次仍失败") != std::string::npos);
    CHECK(result.error().find("总时长") != std::string::npos);
    CHECK(result.error().find("含退避") != std::string::npos);
    CHECK(backend.calls == 6);  // 首发 + 5 次重试
}

TEST_CASE("断流恢复:采样实录形态用满预算,失败文案如实带重试次数") {
    // 错误形态账本(tests/fixtures/api/error_shapes.json)首批实录喂满预算:
    // Api 路(HTTP 200 + code=upstream_error)与 HttpStatus 500 路都按现行
    // 白名单可重试,重试用尽后的收口文案必须把真实重试次数与含退避的
    // 总时长写明——文案在 loop.cpp 对两条 kind 同一段拼装,这里各钉一路,
    // Api 分型路(200+错误体)此前只有 Network 路验过。
    bool api_shape = false;
    SUBCASE("HTTP 200 + upstream_error(账本实录形态)") {
        api_shape = true;
    }
    SUBCASE("HTTP 500(账本实录形态)") {
        api_shape = false;
    }
    FlakyBackend backend;
    backend.script.resize(api::kMaxRequestAttempts);
    for (auto& take : backend.script) {
        if (api_shape) {
            take.events = {api::StreamError{"Upstream request failed (code=upstream_error)",
                                            "upstream_error"}};
        } else {
            take.error = api::Error{api::ErrorKind::HttpStatus, "server failed", 500};
        }
    }
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.wait_request_backoff = [](std::chrono::milliseconds, const std::atomic<bool>*) {
        return true;
    };
    const auto result = loop.Run("必失败", wiring);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("已自动重试 5 次仍失败") != std::string::npos);
    CHECK(result.error().find("总时长") != std::string::npos);
    CHECK(result.error().find("含退避") != std::string::npos);
    CHECK(result.error().find(api_shape ? "Upstream request failed" : "500") != std::string::npos);
    CHECK(backend.calls == api::kMaxRequestAttempts);  // 首发 + 5 次重试
}

TEST_CASE("恢复账:尝试相位从环里流出,started 连号、retrying 带稳定码") {
    FlakyBackend backend;
    backend.script = {
        {/*空*/},
        {TextScript("ok"), std::nullopt},
    };
    backend.script[0].error = ConnectReset();
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    std::vector<std::pair<int, api::RequestAttemptPhase>> phases;
    std::string retry_reason;
    wiring.on_request_attempt = [&phases, &retry_reason](const api::ModelRequestAttempt& attempt,
                                                          api::RequestAttemptPhase phase) {
        phases.emplace_back(attempt.attempt, phase);
        if (phase == api::RequestAttemptPhase::Retrying) {
            retry_reason = attempt.error_code;
        }
    };
    REQUIRE(loop.Run("记个账", wiring).has_value());
    REQUIRE(phases.size() == 4);  // started/retrying + started/succeeded
    CHECK(phases[0].first == 1);
    CHECK(phases[0].second == api::RequestAttemptPhase::Started);
    CHECK(phases[1].second == api::RequestAttemptPhase::Retrying);
    CHECK(phases[2].first == 2);
    CHECK(phases[3].second == api::RequestAttemptPhase::Succeeded);
    CHECK(retry_reason == "network.error");
}

// ---------------------------------------------------------------------------
// P1-C(失败与恢复单 FA-03):发送前写账硬闸——prepared 过、sent 记不住,
// backend 不得被调用;本地存储故障退出恢复环,不按 Network 错继续重试。
// ---------------------------------------------------------------------------

TEST_CASE("发送前写账硬闸: sent 记不住,backend 零调用,无重试") {
    CheckActualModelSendGate();
    FlakyBackend backend;
    backend.script = {{TextScript("不该被需要"), std::nullopt}};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    SentGateRecorder recorder;
    recorder.fail_sent_on_request = 1;  // 首枚请求的 sent 写不住
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.boundary_recorder = &recorder;
    const auto result = loop.Run("问一句", wiring);
    REQUIRE_FALSE(result.has_value());
    CHECK(backend.calls == 0);  // 本地已知账写不动,就不再发本次模型请求
    CHECK(recorder.prepared == 1);
    CHECK(recorder.sent == 1);
    CHECK(recorder.response_started_for.empty());  // 没有任何远端事实被冒认
    // 本地存储故障不是网络错误:恢复环直接退出,无第二次尝试。
    CHECK(result.error().find("轨迹账写盘失败") != std::string::npos);
}

TEST_CASE("发送前写账硬闸: 首次网络失败后,第二次尝试 sent 记不住,总数停在已发送次数") {
    FlakyBackend backend;
    backend.script = {
        {/*空*/},
        {TextScript("不该被需要"), std::nullopt},
    };
    backend.script[0].error = ConnectReset();
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    SentGateRecorder recorder;
    recorder.fail_sent_on_request = 2;  // 第二枚逻辑请求的 sent 写不住
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.boundary_recorder = &recorder;
    wiring.wait_request_backoff = [](std::chrono::milliseconds, const std::atomic<bool>*) { return true; };
    const auto result = loop.Run("问一句", wiring);
    REQUIRE_FALSE(result.has_value());
    // 第一次真发出去了(网络失败);第二次尝试在 sent 边界被拦——调用总数
    // 停在此前已发送次数(1),不再发。
    CHECK(backend.calls == 1);
    CHECK(recorder.prepared == 2);
    CHECK(recorder.sent == 2);
    REQUIRE(recorder.sent_ok().size() == 1);  // 只有第一枚落稳
}

TEST_CASE("发送前写账成功后 transport 失败: 本地尝试记录保留,按网络策略收口") {
    FlakyBackend backend;
    backend.script = {
        {/*空*/},
        {TextScript("重试后的完整回答"), std::nullopt},
    };
    backend.script[0].error = ConnectReset();
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    SentGateRecorder recorder;
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.boundary_recorder = &recorder;
    wiring.wait_request_backoff = [](std::chrono::milliseconds, const std::atomic<bool>*) { return true; };
    const auto result = loop.Run("问一句", wiring);
    REQUIRE(result.has_value());
    CHECK(backend.calls == 2);  // 网络策略照走:可安全重发的从提交边界重来
    // 两枚逻辑请求各有 prepared + sent(本地尝试记录保留)。
    CHECK(recorder.prepared == 2);
    REQUIRE(recorder.sent_ok().size() == 2);
    CHECK(recorder.sent_ok()[0] != recorder.sent_ok()[1]);  // 各次尝试独立身份
    // 不得推出服务端已处理:首枚请求没有任何 response.* 事实。
    REQUIRE(recorder.response_started_for.size() == 1);
    CHECK(recorder.response_started_for[0] == recorder.sent_ok()[1]);
}

// ---------------------------------------------------------------------------
// P1-A(失败与恢复单 FA-01):批次结果持久提交回执——硬失败停止后续模型
// 发送并撤回内存 history;约定降级放行。
// ---------------------------------------------------------------------------

TEST_CASE("结果提交回执闸: 硬失败停止后续模型发送,内存 history 撤回到持久边界") {
    FlakyBackend backend;
    backend.script = {
        {ToolScript("toolu_1"), std::nullopt},  // 工具执行一次
        {TextScript("不该被需要"), std::nullopt},
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<CountingTool>();
    CountingTool* counting = tool.get();
    registry.Register(std::move(tool));
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.wait_request_backoff = [](std::chrono::milliseconds, const std::atomic<bool>*) { return true; };
    wiring.on_tool_trace = [](const agent::ToolTraceEvent&) {};
    runtime::ToolResultsCommitReceipt failed;
    failed.status = runtime::ToolResultsCommitReceipt::Status::Failed;
    failed.error_code = "tool.result.store_unavailable";
    wiring.on_tool_results_committed_receipt =
        [&failed](const std::string&, const api::Message&) { return failed; };
    const auto result = loop.Run("跑个工具", wiring);
    REQUIRE_FALSE(result.has_value());
    // 副作用只发生一次;backend 不进第二次调用(FA-01:不拿内存独有结果
    // 继续发请求)。
    CHECK(counting->call_count == 1);
    CHECK(backend.calls == 1);
    CHECK(result.error().find("工具结果未落账") != std::string::npos);
    CHECK(result.error().find("tool.result.store_unavailable") != std::string::npos);
    // 内存推进与持久接纳对齐:未提交的 tool_result 不留 history——但已
    // 落稳的 assistant 声明照旧在(不倒写已成立的事实)。
    const auto& history = loop.History();
    bool has_tool_result = false;
    bool has_assistant_call = false;
    for (const auto& message : history) {
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::ToolResultBlock>(block)) {
                has_tool_result = true;
            }
            if (std::holds_alternative<api::ToolUseBlock>(block)) {
                has_assistant_call = true;
            }
        }
    }
    CHECK_FALSE(has_tool_result);
    CHECK(has_assistant_call);
}

TEST_CASE("结果提交回执闸: 约定降级放行,后续模型请求照发,缺口码随回执交出") {
    FlakyBackend backend;
    backend.script = {
        {ToolScript("toolu_1"), std::nullopt},
        {TextScript("降级后的收口"), std::nullopt},
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<CountingTool>();
    CountingTool* counting = tool.get();
    registry.Register(std::move(tool));
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.on_tool_trace = [](const agent::ToolTraceEvent&) {};
    runtime::ToolResultsCommitReceipt degraded;
    degraded.status = runtime::ToolResultsCommitReceipt::Status::Degraded;
    degraded.degraded_codes.push_back("tool.result.persist_failed");
    std::vector<runtime::ToolResultsCommitReceipt> seen;
    wiring.on_tool_results_committed_receipt =
        [&degraded, &seen](const std::string&, const api::Message&) {
            seen.push_back(degraded);
            return degraded;
        };
    const auto result = loop.Run("跑个工具", wiring);
    REQUIRE(result.has_value());
    // 降级合同:主账正文已保住,放行——下一份模型请求带着结果照发。
    CHECK(counting->call_count == 1);
    CHECK(backend.calls == 2);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].ok());
    REQUIRE(seen[0].degraded_codes.size() == 1);
    CHECK(seen[0].degraded_codes[0] == "tool.result.persist_failed");
    // 第二份请求带着工具结果(3 条消息:1 user + 1 assistant + 1 tool_result)。
    REQUIRE(backend.message_counts.size() == 2);
    CHECK(backend.message_counts[1] == 3);
    bool has_tool_result = false;
    for (const auto& message : loop.History()) {
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::ToolResultBlock>(block)) {
                has_tool_result = true;
            }
        }
    }
    CHECK(has_tool_result);  // 降级:内存 history 保留(持久正文已保住)
}
