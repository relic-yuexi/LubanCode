// 异步工具单 P2 册:AgentLoop 批次闸门 seam + AsyncToolRuntime 总装 +
// 流式提前档。假后端剧本(不碰真网络)+ 真 v3 账(temp 目录):
//   1. 一轮三枚普通调用:闸门在装也全 inline,下轮配齐三份结果、按声明
//      序、结果前不插文本(验收矩阵第一行);
//   2. start+wait 两调用次序:launch 先注册(同响应的 wait 查得到 job),
//      已完成 job 的业务结果排在 wait 自身状态结果前(§7);
//   3. native_deferred(注入 verified 探针):a 欠账 b 先送;a 完成后沿
//      原 call 配对,下一次请求才引用(§8 native 轨迹);
//   4. 完成信封隔离:未知 job/重复终态信封拒收,账面无重复观测;
//   5. 流式提前档:单枚 call item 完整即派发(宿主继续消费流),重复终
//      帧只派发一次;JSON 半截/call_id 未定不派发;
//   6. 提前派发后流断:账面 dispatched 无终态 → 恢复 disposition
//      unknown_hold,不盲重跑(§6 表);
//   7. 宿主装配冒烟:AttachDefaultAsyncToolRuntime(终端/one-shot/
//      AppServer 共用装配路径)挂上闸门,一轮全 inline 走通。
//
// 夹具纪律(同 P1 册):遍历账面先落局部 V3Ledger,不许 range-for 直接
// 吃 ReadV3Ledger(...).value().events;nlohmann 缺键先 contains。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/async_tool_runtime.hpp"
#include "runtime/session_runtime.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "tools/job_tools.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/tool_job_coordinator.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;
namespace v3 = lubancode::trajectory::v3;

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

// 受控执行闸(P1 册同款):executor 挂 future,测试精确放行。
struct Gate {
    std::promise<void> release;
    std::future<void> released;
    std::atomic<bool> opened{false};
    Gate() : released(release.get_future()) {}
    void Open() {
        bool expected = false;
        if (opened.compare_exchange_strong(expected, true)) {
            release.set_value();
        }
    }
};

class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;
    std::optional<std::size_t> cancel_after_event_index;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        const auto& script = scripts[idx];
        for (std::size_t i = 0; i < script.size(); ++i) {
            on_event(script[i]);
            if (cancel_after_event_index.has_value() && i == *cancel_after_event_index) {
                return std::unexpected(api::Error{api::ErrorKind::Cancelled, "FakeBackend: 模拟流断", 0});
            }
        }
        return {};
    }
};

// 立即回话的假工具(inline 路用)。
class EchoTool : public tools::Tool {
public:
    std::string name() const override { return "echo"; }
    std::string description() const override { return "echo for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json& input) override {
        return {input.value("text", "ok"), false};
    }
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

struct CallSpec {
    std::string id;
    std::string name;
    std::string arguments = "{}";
    bool async_call = false;
};

std::vector<api::StreamEvent> CallsScript(const std::vector<CallSpec>& calls,
                                          const std::string& stop = "tool_use") {
    std::vector<api::StreamEvent> events;
    events.push_back(api::MessageStart{"msg", "model"});
    for (std::size_t i = 0; i < calls.size(); ++i) {
        api::ToolUseStart start;
        start.index = static_cast<int>(i);
        start.id = calls[i].id;
        start.name = calls[i].name;
        start.async_call = calls[i].async_call;
        events.push_back(start);
        events.push_back(api::ToolUseInputDelta{static_cast<int>(i), calls[i].arguments});
        api::ContentBlockDone done;
        done.index = static_cast<int>(i);
        done.tool_use_id = calls[i].id;
        events.push_back(done);
    }
    events.push_back(api::MessageDone{stop, api::Usage{}});
    return events;
}

tools::JobAuthDecision AllowAll(const std::string&, const nlohmann::json&) {
    return tools::JobAuthDecision{true, false, ""};
}

// 一场异步装配:v3 账 + AsyncToolRuntime(可注白名单/探针/执行闸) +
// 注册表 + Agent。hooks 的桥面口(reserved/origin/turn)注入固定值——
// loop 级剧本不带真轮桥,桥面集成由宿主装配冒烟案覆盖。
struct AsyncHarness {
    EnvGuard v3env{"LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1"};
    std::filesystem::path dir;
    std::optional<v3::V3Writer> writer;
    std::shared_ptr<std::recursive_mutex> writer_mutex = std::make_shared<std::recursive_mutex>();
    std::unique_ptr<runtime::AsyncToolRuntime> runtime;
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    FakeBackend backend;
    tools::ToolRegistry registry;
    std::unique_ptr<agent::Agent> agent;

    explicit AsyncHarness(const char* tag, std::map<std::string, runtime::AsyncToolPolicy> tools,
                          std::optional<std::string> probe = std::nullopt,
                          agent::ToolDispatchPoint dispatch = agent::ToolDispatchPoint::OnAssistantComplete) {
        for (auto& [name, policy] : tools) {
            policy.dispatch_point = dispatch;
        }
        dir = std::filesystem::temp_directory_path() / ("lubancode-async-p2-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        auto started = v3::V3Writer::Start(dir / "s1.jsonl", "20260916-120000-ASYNCP2", "run-000001",
                                           "system prompt");
        REQUIRE(started.has_value());
        writer = std::move(*started);

        runtime::AsyncToolRuntime::Hooks hooks;
        hooks.writer = &*writer;
        hooks.writer_mutex = writer_mutex;
        hooks.auth = AllowAll;
        // 执行闸:worker 等 future(测试精确放行;即回型任务不挂闸也行)。
        std::shared_ptr<Gate> gate_copy = gate;
        hooks.executor = [gate_copy](const tools::JobExecutionContext& context) {
            if (context.input.value("hold", false)) {
                gate_copy->released.get();
            }
            tools::Tool::Result result;
            result.content = context.input.value("text", "job-done");
            return result;
        };
        hooks.reserved_assistant_message_id = [](const std::string&) -> std::optional<std::string> {
            return std::string("msg-reserved-0001");
        };
        hooks.call_origin_resolver =
            [](const std::string& call_id) -> std::optional<tools::JobStartRequest> {
            tools::JobStartRequest request;
            request.turn_id = "turn-000001";
            request.step_id = "step-000001";
            request.assistant_message_ref = "msg-declared-" + call_id;
            return request;
        };
        hooks.current_turn_id = [] { return std::string("turn-000001"); };
        hooks.response_evidence = [](const std::string&) -> std::optional<std::string> {
            return std::nullopt;  // 无真轮桥:回执证据缺失 → uncertain 口径
        };

        runtime::AsyncToolRuntimeOptions options;
        options.provider = "openai";
        options.wire = "responses";
        options.model = "gpt-test";
        options.tools = std::move(tools);
        options.native_probe_status = probe;
        options.native_probe_evidence = "fake-probe-verified";
        runtime = runtime::AsyncToolRuntime::Create(std::move(hooks), std::move(options));
        REQUIRE(runtime != nullptr);
        tools::RegisterJobTools(registry, runtime->coordinator());
        registry.Register(std::make_unique<EchoTool>());
        agent = std::make_unique<agent::Agent>(
            backend, registry,
            agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    }

    agent::TurnWiring MakeWiring(runtime::TurnEventAdapter* events) {
        agent::TurnWiring wiring;
        wiring.events = events;
        wiring.turn_id = "turn-000001";  // 提前档与注册链的信封 turnId
        wiring.tool_batch_gate = runtime->gate();
        wiring.delivery_planner = runtime->planner();
        return wiring;
    }

    v3::V3Ledger Read() {
        auto ledger = v3::ReadV3Ledger(dir / "s1.jsonl");
        REQUIRE_MESSAGE(ledger.has_value(), ledger.error_or(""));
        return *ledger;
    }
};

std::optional<nlohmann::json> ParseJson(const std::string& text) {
    try {
        return nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
    }
}

// 第二份请求里最后一条 user 消息的 ToolResultBlock 序列(配对断言用)。
std::vector<api::ToolResultBlock> LastResultsOf(const api::Request& request) {
    for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it) {
        if (it->role != api::Role::User) {
            continue;
        }
        std::vector<api::ToolResultBlock> results;
        for (const auto& block : it->content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                results.push_back(*result);
            }
        }
        return results;
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 三枚普通调用:闸门在装也全 inline;下轮配齐、按声明序、文本不插结果前
// ---------------------------------------------------------------------------
TEST_CASE("三枚普通调用:批次闸门在装全 inline,下轮配齐三份结果且按声明序") {
    std::map<std::string, runtime::AsyncToolPolicy> no_tools;
    AsyncHarness h("inline3", std::move(no_tools));
    h.backend.scripts = {
        CallsScript({{"call_A", "echo"}, {"call_B", "echo"}, {"call_C", "echo"}}),
        TextScript("done"),
    };
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter("test", ids);
    adapter.Start();
    agent::TurnWiring wiring = h.MakeWiring(&adapter);

    const auto outcome = h.agent->Run("三个都查", wiring);
    REQUIRE(outcome.has_value());

    REQUIRE(h.backend.captured_requests.size() == 2);
    const std::vector<api::ToolResultBlock> results = LastResultsOf(h.backend.captured_requests[1]);
    REQUIRE(results.size() == 3);
    CHECK(results[0].tool_use_id == "call_A");
    CHECK(results[1].tool_use_id == "call_B");
    CHECK(results[2].tool_use_id == "call_C");
    CHECK_FALSE(results[0].is_error);
    // 结果前不插文本:那条 user 消息的首块就是第一份结果。
    const api::Message& tail = h.backend.captured_requests[1].messages.back();
    REQUIRE(!tail.content.empty());
    CHECK(std::holds_alternative<api::ToolResultBlock>(tail.content.front()));
}

// ---------------------------------------------------------------------------
// 2. start+wait 次序:launch 先注册;已完成 job 的业务结果排在 wait 状态前
// ---------------------------------------------------------------------------
TEST_CASE("job_handle:同响应先注册 launch 再处理 wait,已完成结果排在状态前") {
    std::map<std::string, runtime::AsyncToolPolicy> tools;
    runtime::AsyncToolPolicy policy;
    policy.execution.side_effect_class = "read_only";
    tools["slow_search"] = policy;
    AsyncHarness h("startwait", std::move(tools));

    h.backend.scripts = {
        // 第一轮:只有 start(launch)。job-000001 接单即配(worker 被
        // hold 闸挂住,终态由测试信封口确定性投递)。
        CallsScript({{"call_A", "slow_search", "{\"query\":\"资料\",\"hold\":true}"}}, "tool_use"),
        TextScript("第一轮收口"),
        // 第二轮:wait(已完成的 job-000001)+ launch(job-000002)+
        // wait(job-000002,超时 0 只取快照)同一响应——次序纪律在此验。
        CallsScript({{"call_B", "job_wait",
                      "{\"jobIds\":[\"job-000001\"],\"timeout_ms\":1000,\"mode\":\"any\"}"},
                     {"call_C", "slow_search", "{\"query\":\"二查\"}"},
                     {"call_D", "job_wait",
                      "{\"jobIds\":[\"job-000002\"],\"timeout_ms\":0,\"mode\":\"any\"}"}},
                    "tool_use"),
        TextScript("done"),
    };
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter("test", ids);
    adapter.Start();
    agent::TurnWiring wiring = h.MakeWiring(&adapter);

    const auto outcome = h.agent->Run("先查后等", wiring);
    REQUIRE(outcome.has_value());
    REQUIRE(h.backend.captured_requests.size() == 2);

    // 第一轮的批次消息:接单结果即配 call_A(声明序在前)。
    const std::vector<api::ToolResultBlock> first = LastResultsOf(h.backend.captured_requests[1]);
    REQUIRE(first.size() == 1);
    CHECK(first[0].tool_use_id == "call_A");
    CHECK(first[0].job_admission);
    const auto admission = ParseJson(first[0].content);
    REQUIRE(admission.has_value());
    CHECK((*admission)["jobId"] == "job-000001");

    // 确定性完成:P1 的测试信封口(同款校验),投 job-000001 的终态。
    REQUIRE(h.runtime->coordinator()->DebugSubmitEnvelope(
        "job-000001", "epoch-1", tools::Tool::Result{"资料的业务结果", false}));

    const auto outcome2 = h.agent->Run("再等", wiring);
    REQUIRE(outcome2.has_value());
    REQUIRE(h.backend.captured_requests.size() == 4);

    // 第二轮批次(声明序 B/C/D):结果随第四份请求(script3)入史。
    const std::vector<api::ToolResultBlock> second = LastResultsOf(h.backend.captured_requests[3]);
    REQUIRE(second.size() == 3);
    CHECK(second[0].tool_use_id == "call_B");
    CHECK(second[1].tool_use_id == "call_C");
    CHECK(second[2].tool_use_id == "call_D");
    // B:已完成 job 的业务结果排在 wait 自身状态前(§7;键序即 dump 序)。
    const auto wait_done = ParseJson(second[0].content);
    REQUIRE(wait_done.has_value());
    CHECK(second[0].content.find("\"results\"") < second[0].content.find("\"statuses\""));
    REQUIRE((*wait_done)["results"].size() == 1);
    CHECK((*wait_done)["results"][0]["jobId"] == "job-000001");
    // 预览是 §4.18 合同的带壳投影文本(字节账+文件指引+正文)——断言正文
    // 在投影里,不苛求裸串。
    CHECK((*wait_done)["results"][0]["preview"].get<std::string>().find("资料的业务结果") !=
          std::string::npos);
    // C:同响应的 launch 也接单即配。
    CHECK(second[1].job_admission);
    const auto admission2 = ParseJson(second[1].content);
    REQUIRE(admission2.has_value());
    CHECK((*admission2)["jobId"] == "job-000002");
    // D:wait 查得到 C 的 job(不是 unknown_job)——launch 在 wait 之前
    // 已注册(先注册所有 launch,再处理 wait)。
    const auto wait_new = ParseJson(second[2].content);
    REQUIRE(wait_new.has_value());
    REQUIRE((*wait_new)["statuses"].size() == 1);
    CHECK((*wait_new)["statuses"][0]["jobId"] == "job-000002");
    CHECK((*wait_new)["statuses"][0]["status"] != "unknown_job");

    h.gate->Open();  // 收尾放行被挂住的 worker(迟到信封由协调器拒收)
}

// ---------------------------------------------------------------------------
// 3. native_deferred(verified 探针注入):a 欠账 b 先送;a 完成后沿原 call
// ---------------------------------------------------------------------------
TEST_CASE("native_deferred:a 欠账 b 先送,完成结果沿原 call 下次请求才引用") {
    std::map<std::string, runtime::AsyncToolPolicy> tools;
    runtime::AsyncToolPolicy policy;
    policy.execution.side_effect_class = "read_only";
    policy.require_call_async_mark = true;
    tools["native_search"] = policy;
    AsyncHarness h("nativeab", std::move(tools), /*probe=*/std::string("verified"));

    h.backend.scripts = {
        CallsScript({{"call_a", "native_search", "{\"query\":\"资料\",\"hold\":true}", /*async=*/true},
                     {"call_b", "echo", "{\"text\":\"b 的结果\"}"}},
                    "tool_use"),
        TextScript("先答复不依赖资料的部分"),
        TextScript("引用资料补完"),
    };
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter("test", ids);
    adapter.Start();
    agent::TurnWiring wiring = h.MakeWiring(&adapter);

    const auto outcome = h.agent->Run("查资料", wiring);
    REQUIRE(outcome.has_value());
    REQUIRE(h.backend.captured_requests.size() == 2);

    // a 欠账:第二份请求只带 b 的结果,不带 a 的配对。
    const std::vector<api::ToolResultBlock> second = LastResultsOf(h.backend.captured_requests[1]);
    REQUIRE(second.size() == 1);
    CHECK(second[0].tool_use_id == "call_b");
    CHECK(second[0].content == "b 的结果");

    // 账面:注册带 wireCallRef(async=true),协议欠账未配。
    {
        v3::V3Ledger ledger = h.Read();
        const auto errors = v3::ValidateAsyncToolSequence(ledger);
        for (const auto& error : errors) {
            FAIL_CHECK(error.code << ": " << error.message);
        }
        auto obligations = v3::ProjectProtocolObligations(ledger);
        const auto* obligation_a = v3::FindProtocolObligation(obligations, "action-job-000001");
        REQUIRE(obligation_a != nullptr);
        CHECK(obligation_a->mode == "native_deferred");
        CHECK(obligation_a->async_call);
        CHECK_FALSE(obligation_a->paired);
    }

    // a 完成(测试信封口,同款校验)+ 批次闸门泵 → 完成通知入 mailbox。
    REQUIRE(h.runtime->coordinator()->DebugSubmitEnvelope(
        "job-000001", "epoch-1", tools::Tool::Result{"a 的业务结果", false}));
    h.runtime->gate()->PumpBatchBoundary();

    // 下一次请求边界才引用:第二份请求已冻结(按值存档),后续投递不改
    // 它;第三次请求带 a 的原 call 配对。
    const std::string second_snapshot = [&] {
        std::string dump;
        for (const auto& message : h.backend.captured_requests[1].messages) {
            dump += std::to_string(static_cast<int>(message.role));
            dump += ":" + std::to_string(message.content.size()) + ";";
        }
        return dump;
    }();
    const auto outcome2 = h.agent->Run("继续", wiring);
    REQUIRE(outcome2.has_value());
    REQUIRE(h.backend.captured_requests.size() == 3);
    // 冻结断言:第二份请求的消息序列没因后续投递改变。
    const std::string second_snapshot_after = [&] {
        std::string dump;
        for (const auto& message : h.backend.captured_requests[1].messages) {
            dump += std::to_string(static_cast<int>(message.role));
            dump += ":" + std::to_string(message.content.size()) + ";";
        }
        return dump;
    }();
    CHECK(second_snapshot == second_snapshot_after);

    // 第三次请求:沿原 call_a 配对(投递正文 = 业务结果的带壳预览)。
    const std::vector<api::ToolResultBlock> third = LastResultsOf(h.backend.captured_requests[2]);
    REQUIRE(third.size() == 1);
    CHECK(third[0].tool_use_id == "call_a");
    CHECK(third[0].content.find("a 的业务结果") != std::string::npos);
    h.gate->Open();  // 收尾放行被挂住的 worker(迟到信封按终态后迟到拒收)

    // 账面:欠账配齐(tool 消息落账)。
    {
        v3::V3Ledger ledger = h.Read();
        const auto errors = v3::ValidateAsyncToolSequence(ledger);
        for (const auto& error : errors) {
            FAIL_CHECK(error.code << ": " << error.message);
        }
        auto obligations = v3::ProjectProtocolObligations(ledger);
        const auto* obligation_a = v3::FindProtocolObligation(obligations, "action-job-000001");
        REQUIRE(obligation_a != nullptr);
        CHECK(obligation_a->paired);
    }
}

// ---------------------------------------------------------------------------
// 4. 完成信封隔离:未知 job / 重复终态信封拒收,账面无重复观测
// ---------------------------------------------------------------------------
TEST_CASE("完成信封隔离:未知 job 与重复终态拒收,计数暴露") {
    std::map<std::string, runtime::AsyncToolPolicy> tools;
    runtime::AsyncToolPolicy policy;
    tools["slow_search"] = policy;
    AsyncHarness h("envelope", std::move(tools));

    h.backend.scripts = {
        CallsScript({{"call_A", "slow_search", "{\"query\":\"资料\",\"hold\":true}"}}, "tool_use"),
        TextScript("done"),
    };
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter("test", ids);
    adapter.Start();
    agent::TurnWiring wiring = h.MakeWiring(&adapter);
    REQUIRE(h.agent->Run("查", wiring).has_value());

    // 未知 job:拒收。
    CHECK_FALSE(h.runtime->coordinator()->DebugSubmitEnvelope(
        "job-999999", "epoch-1", tools::Tool::Result{"x", false}));
    // 旧租约(job 在跑、租约是 epoch-1):拒收,计 stale。
    CHECK_FALSE(h.runtime->coordinator()->DebugSubmitEnvelope("job-000001", "epoch-9",
                                                              tools::Tool::Result{"stale", false}));
    CHECK(h.runtime->coordinator()->stale_envelopes_rejected() >= 1);
    // 合法终态:第一枚收;终态后迟到(无论租约新旧):拒收,计 duplicate。
    CHECK(h.runtime->coordinator()->DebugSubmitEnvelope("job-000001", "epoch-1",
                                                        tools::Tool::Result{"ok", false}));
    CHECK_FALSE(h.runtime->coordinator()->DebugSubmitEnvelope("job-000001", "epoch-1",
                                                              tools::Tool::Result{"again", false}));
    CHECK(h.runtime->coordinator()->duplicate_terminal_envelopes_rejected() >= 1);

    h.runtime->gate()->PumpBatchBoundary();
    h.gate->Open();  // 真.worker 放行收尾
    v3::V3Ledger ledger = h.Read();
    const auto errors = v3::ValidateAsyncToolSequence(ledger);
    for (const auto& error : errors) {
        FAIL_CHECK(error.code << ": " << error.message);
    }
    // 账面终态观测唯一(不因重复信封双写)。
    int terminal_observed = 0;
    for (const auto& event : ledger.events) {
        if (event.kind == v3::EventKindV3::ToolJobObserved &&
            event.payload.contains("observedStatus")) {
            const std::string status = event.payload["observedStatus"].get<std::string>();
            if (status == "succeeded" || status == "failed") {
                ++terminal_observed;
            }
        }
    }
    CHECK(terminal_observed == 1);
}

// ---------------------------------------------------------------------------
// 5. 流式提前档:单枚 call item 完整即派发;重复终帧只派发一次
// ---------------------------------------------------------------------------
TEST_CASE("流式提前档:call item 完整即派发,重复终帧只派发一次") {
    std::map<std::string, runtime::AsyncToolPolicy> tools;
    runtime::AsyncToolPolicy policy;
    policy.execution.side_effect_class = "read_only";
    tools["slow_search"] = policy;
    AsyncHarness h("early", std::move(tools), std::nullopt,
                   agent::ToolDispatchPoint::OnCallItemComplete);

    // 脚本:call item 完整 → 重复终帧 → 正文收尾。worker 被 hold 闸挂住,
    // 证明"派发了、宿主继续消费流"(正文照样收)。
    h.backend.scripts = {
        [&] {
            std::vector<api::StreamEvent> events;
            events.push_back(api::MessageStart{"msg", "model"});
            events.push_back(api::ToolUseStart{0, "call_A", "slow_search"});
            events.push_back(api::ToolUseInputDelta{0, "{\"query\":\"资料\",\"hold\":true}"});
            api::ContentBlockDone done;
            done.index = 0;
            done.tool_use_id = "call_A";
            events.push_back(done);
            events.push_back(done);  // 重复终帧:只派发一次
            events.push_back(api::TextDelta{"正文还在流里"});
            events.push_back(api::ContentBlockDone{1});
            events.push_back(api::MessageDone{"tool_use", api::Usage{}});
            return events;
        }(),
        TextScript("done"),
    };
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter("test", ids);
    adapter.Start();
    agent::TurnWiring wiring = h.MakeWiring(&adapter);

    const auto outcome = h.agent->Run("查", wiring);
    REQUIRE(outcome.has_value());
    // 提前派发恰好一次(重复终帧去重);worker 在跑(hold 闸没放)。
    CHECK(h.runtime->early_dispatched_count() == 1);
    CHECK(h.runtime->coordinator()->running_count() == 1);
    // 宿主继续消费了流:第二份请求带着接单结果与正文。
    REQUIRE(h.backend.captured_requests.size() == 2);
    const std::vector<api::ToolResultBlock> first = LastResultsOf(h.backend.captured_requests[1]);
    REQUIRE(first.size() == 1);
    CHECK(first[0].tool_use_id == "call_A");
    CHECK(first[0].job_admission);

    h.gate->Open();
    h.runtime->gate()->PumpBatchBoundary();
    v3::V3Ledger ledger = h.Read();
    for (const auto& error : v3::ValidateAsyncToolSequence(ledger)) {
        FAIL_CHECK(error.code << ": " << error.message);
    }
}

// ---------------------------------------------------------------------------
// 6. 提前派发后流断:账面 dispatched 无终态 → 恢复 unknown_hold 不盲重跑
// ---------------------------------------------------------------------------
TEST_CASE("提前派发后流断:账面未知态,恢复 disposition 不盲重跑") {
    std::map<std::string, runtime::AsyncToolPolicy> tools;
    runtime::AsyncToolPolicy policy;
    policy.execution.side_effect_class = "read_only";
    tools["slow_search"] = policy;
    AsyncHarness h("earlybreak", std::move(tools), std::nullopt,
                   agent::ToolDispatchPoint::OnCallItemComplete);

    h.backend.scripts = {
        [&] {
            std::vector<api::StreamEvent> events;
            events.push_back(api::MessageStart{"msg", "model"});
            events.push_back(api::ToolUseStart{0, "call_A", "slow_search"});
            events.push_back(api::ToolUseInputDelta{0, "{\"query\":\"资料\",\"hold\":true}"});
            api::ContentBlockDone done;
            done.index = 0;
            done.tool_use_id = "call_A";
            events.push_back(done);
            events.push_back(api::TextDelta{"半截"});
            events.push_back(api::ContentBlockDone{1});
            events.push_back(api::MessageDone{"tool_use", api::Usage{}});
            return events;
        }(),
    };
    // 流在 call item 完整后、正文半截处断(提前派发已发生)。
    h.backend.cancel_after_event_index = 4;

    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter("test", ids);
    adapter.Start();
    agent::TurnWiring wiring = h.MakeWiring(&adapter);

    const auto outcome = h.agent->Run("查", wiring);
    REQUIRE(outcome.has_value());
    CHECK(outcome->cancelled);
    CHECK(h.runtime->early_dispatched_count() == 1);

    // 账面:registered + dispatched + started(2),无终态观测;接单事实在、
    // 接单消息缺(提前档链序:消息等声明落账后补,流断了就没补)。
    // 恢复计划 = complete_delivery(admission_chain_missing)——补消息不重跑、
    // 不给在跑的 attempt 伪造终态;工作的执行投影留在 running(归
    // unknown_hold/接管裁决,下一轮恢复面)。worker 还挂着——真实
    // "已启动、去向不明"。
    v3::V3Ledger ledger = h.Read();
    for (const auto& error : v3::ValidateAsyncToolSequence(ledger)) {
        FAIL_CHECK(error.code << ": " << error.message);
    }
    const auto plan = tools::ToolJobCoordinator::PlanRecovery(ledger);
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].job_id == "job-000001");
    CHECK(plan.items[0].disposition == "complete_delivery");
    CHECK(plan.items[0].detail == "admission_chain_missing");
    CHECK(plan.items[0].attempt == 2);
    CHECK(plan.items[0].attempt_started);

    // 单写者收唯一终态(真完成不改成"未执行"):确定性走测试信封口
    //(真实 worker 的迟到信封按终态后迟到拒收,teardown 放行)。
    REQUIRE(h.runtime->coordinator()->DebugSubmitEnvelope(
        "job-000001", "epoch-1", tools::Tool::Result{"结果", false}));
    h.runtime->gate()->PumpBatchBoundary();
    const auto view = h.runtime->coordinator()->GetJob("job-000001");
    CHECK(view.state == "succeeded");
    h.gate->Open();  // 收尾放行被挂住的 worker(迟到信封由协调器拒收)
}

// ---------------------------------------------------------------------------
// 7. 宿主装配冒烟:AttachDefaultAsyncToolRuntime(终端/one-shot/AppServer
//    共用装配路径)——挂上 dormant 闸门,一轮全 inline 走通
// ---------------------------------------------------------------------------
TEST_CASE("宿主装配冒烟:SessionRuntime 挂 dormant 异步运行时,全 inline 回合") {
    EnvGuard v3env{"LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1"};
    const auto root = std::filesystem::temp_directory_path() / "lubancode-async-p2-hostsmoke";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    runtime::SessionRuntime::Options options;
    options.wire_name = "responses";
    options.start_ts = "20260916-120000";
    options.lubancode_version = "0.26.0-test";
    options.trajectory_workspaces_root = root / "workspaces";
    options.trajectory_workspace_identity = workspace::MakeFallbackIdentity(root / "ws");
    runtime::SessionRuntime session(options);
    REQUIRE(session.trajectory() != nullptr);
    REQUIRE_MESSAGE(session.trajectory_open_error().empty(), session.trajectory_open_error());

    CHECK(runtime::AttachDefaultAsyncToolRuntime(session, "responses"));
    runtime::AsyncToolRuntime* async_runtime = session.async_tool_runtime();
    REQUIRE(async_runtime != nullptr);
    CHECK(async_runtime->gate() != nullptr);
    CHECK(async_runtime->planner() != nullptr);
    CHECK(async_runtime->coordinator() != nullptr);
    CHECK(runtime::AttachDefaultAsyncToolRuntime(session, "responses"));  // 幂等
    // 宿主每轮开拍前刷新模型身份(能力快照 basis 三件齐才落)。
    async_runtime->NoteModelIdentity("openai", "test-model");

    FakeBackend backend;
    backend.scripts = {
        CallsScript({{"call_A", "echo"}}),
        TextScript("done"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<EchoTool>());
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"},
                                          .system_prompt = "system prompt"});
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter("test", ids);
    adapter.Start();
    agent::TurnWiring wiring;
    wiring.events = &adapter;
    wiring.tool_batch_gate = async_runtime->gate();
    wiring.delivery_planner = async_runtime->planner();

    const auto outcome = loop.Run("冒烟", wiring);
    REQUIRE(outcome.has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    const std::vector<api::ToolResultBlock> results = LastResultsOf(backend.captured_requests[1]);
    REQUIRE(results.size() == 1);
    CHECK(results[0].tool_use_id == "call_A");
    // dormant(零策略):没有 job 落账——能力快照一行,job 族零行。
    auto ledger = v3::ReadV3Ledger(session.trajectory()->v3_main_writer()->path());
    REQUIRE(ledger.has_value());
    bool capability_seen = false;
    int job_events = 0;
    for (const auto& event : ledger->events) {
        if (event.kind == v3::EventKindV3::ToolCapabilityRecorded) {
            capability_seen = true;
        }
        if (event.kind == v3::EventKindV3::ToolJobRegistered ||
            event.kind == v3::EventKindV3::ToolJobDispatched ||
            event.kind == v3::EventKindV3::ToolJobObserved) {
            ++job_events;
        }
    }
    CHECK(capability_seen);  // fail-closed 判定照实落快照(native unknown)
    CHECK(job_events == 0);  // 零策略:没有任何 job
}
