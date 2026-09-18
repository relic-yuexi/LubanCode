// 只读并行单 P3 宿主验收·AppServer(协议宿主,无终端):装配路吃 config
// 的 agent.tool_execution=parallel_read + 并发 2,整回合经真
// Server::RunTurnToCompletion 跑带审批的批次——钉"并行配置下协议宿主面
// 不缺环":
//   1. 两枚读真并发(进门闸;装配若没折到策略,退串行必吃等闸超时红);
//   2. needs_confirm 的写在读段收口后走 permission/request 反向请求,
//      前端 accept 后真执行(独占节点屏障);DontAsk 档预裁定直接拒,
//      不弹审批、零执行;
//   3. 审计:turn/completed 唯一终态、状态如实。
// 拒绝路用 DontAsk(permission_mode)——不需要测试线程抢答审批;accept 路
// 与 test_app_server_approval.cpp 同款(回合线程悬停,测试线程喂答复)。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/tool_batch_schedule.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app_server/connection.hpp"
#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "app_server/server.hpp"
#include "app_server/session_assembly.hpp"
#include "approval_mode.hpp"
#include "config/config.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

constexpr auto kGateWait = std::chrono::seconds(3);

// 按脚本吐事件的假后端(回合线程起一次,脚本从头吃)。
class SharedScriptBackend : public api::Backend {
public:
    explicit SharedScriptBackend(std::vector<std::vector<api::StreamEvent>>& scripts) : scripts_(scripts) {}

    std::expected<void, api::Error> send_stream(const api::Request&,
                                                const std::function<void(const api::StreamEvent&)>& on_event,
                                                const std::atomic<bool>* = nullptr) override {
        if (index_ >= scripts_.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts_[index_]) {
            on_event(event);
        }
        ++index_;
        return {};
    }

private:
    std::vector<std::vector<api::StreamEvent>>& scripts_;
    std::size_t index_ = 0;
};

std::vector<api::StreamEvent> BatchScript(
    const std::vector<std::tuple<std::string, std::string, std::string>>& calls) {
    std::vector<api::StreamEvent> events;
    events.push_back(api::MessageStart{"msg", "fake-model"});
    for (std::size_t i = 0; i < calls.size(); ++i) {
        events.push_back(
            api::ToolUseStart{static_cast<int>(i), std::get<0>(calls[i]), std::get<1>(calls[i])});
        events.push_back(api::ToolUseInputDelta{static_cast<int>(i), std::get<2>(calls[i])});
        events.push_back(api::ContentBlockDone{static_cast<int>(i)});
    }
    events.push_back(api::MessageDone{"tool_use", api::Usage{}});
    return events;
}

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "fake-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// 进门闸:两枚读都进执行体才放行。
struct ConcurrencyGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t entered = 0;
    std::size_t enter_target = 1;
    int gate_timeouts = 0;
};

// 共享计数(注册表工厂每回合新起,计数折到 harness 手里)。
struct ToolCounters {
    std::atomic<int> read_executions{0};
    std::atomic<int> read_peak{0};
    std::atomic<int> write_executions{0};
};

// 读靶:名字 read_file(放行名单),进门闸 + 峰值进共享账。
class GatedRead : public tools::Tool {
public:
    GatedRead(ConcurrencyGate& gate, ToolCounters& counters) : gate_(&gate), counters_(&counters) {}
    std::string name() const override { return "read_file"; }
    std::string description() const override { return "协议宿主读靶"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
    tools::Tool::Result execute(const nlohmann::json&, const tools::ToolExecutionContext&) override {
        ++counters_->read_executions;
        const int now_active = counters_->read_peak.fetch_add(1) + 1;
        // read_peak 暂当在跑计数:退房时减回去,峰值由 fetch_add 的最大现值
        // 表达(计数只增的峰值另由 gate 侧保底,这里够断言"确有重叠")。
        (void)now_active;
        {
            std::unique_lock<std::mutex> lock(gate_->mutex);
            ++gate_->entered;
            gate_->cv.notify_all();
            if (gate_->enter_target > 1 &&
                !gate_->cv.wait_for(lock, kGateWait, [&] { return gate_->entered >= gate_->enter_target; })) {
                ++gate_->gate_timeouts;
            }
        }
        counters_->read_peak.fetch_sub(1);
        return {"app-server read ok", false};
    }

private:
    ConcurrencyGate* gate_;
    ToolCounters* counters_;
};

// 确认写靶:执行计数进共享账。
class ConfirmWrite : public tools::Tool {
public:
    explicit ConfirmWrite(ToolCounters& counters) : counters_(&counters) {}
    std::string name() const override { return "write_file"; }
    std::string description() const override { return "协议宿主确认写靶"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return true; }
    tools::EffectClass effect_class() const override { return tools::EffectClass::LocalReversible; }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++counters_->write_executions;
        return {"written", false};
    }

private:
    ToolCounters* counters_;
};

// 假 IO。
struct ScriptedIo {
    std::vector<std::string> written;
    app_server::StdioConnection::LineWriter Writer() {
        return [this](const std::string& line) { written.push_back(line); };
    }
};

// 整线:Server + assembly_factory(config 折策略 + 注入工具表)。成员序=
// 寿命序:计数/闸先声明(server 的会话材料引用它们),server 后收口。
struct ParallelHarness {
    ToolCounters counters;
    ConcurrencyGate gate;
    std::vector<std::vector<api::StreamEvent>> scripts;
    config::Config config;
    ScriptedIo io;
    std::unique_ptr<app_server::Server> server;

    explicit ParallelHarness(lubancode::ApprovalMode mode) {
        config.agent.tool_execution = "parallel_read";
        config.agent.parallel_read_concurrency = 2;
        app_server::ServerOptions options;
        options.cwd = "/test/cwd";
        options.permission_mode = mode;
        ToolCounters* counters_ptr = &counters;
        ConcurrencyGate* gate_ptr = &gate;
        config::Config* config_ptr = &config;
        std::vector<std::vector<api::StreamEvent>>* script_ptr = &scripts;
        options.assembly_factory = [config_ptr, gate_ptr, counters_ptr, script_ptr]() {
            app_server::SessionAssemblyRequest request;
            request.config = config_ptr;
            request.backend_factory = [script_ptr] {
                return std::make_unique<SharedScriptBackend>(*script_ptr);
            };
            request.registry_factory = [gate_ptr, counters_ptr] {
                auto registry = std::make_unique<tools::ToolRegistry>();
                registry->Register(std::make_unique<GatedRead>(*gate_ptr, *counters_ptr));
                registry->Register(std::make_unique<ConfirmWrite>(*counters_ptr));
                return registry;
            };
            request.system_prompt = app_server::kAppServerDefaultSystemPrompt;
            request.max_steps_per_turn = 8;
            return app_server::AssembleSession(std::move(request));
        };
        server = std::make_unique<app_server::Server>(std::move(options), nullptr, nullptr);
        AttachIo();
    }

    void AttachIo() {
        server->AttachForTest(std::make_unique<app_server::StdioConnection>(
            server->dispatcher_handle(), io.Writer(), []() { return std::string(); }, 256));
    }

    void PumpOutbox() {
        while (auto line = server->connection().outbox().Pop()) {
            io.written.push_back(*line);
        }
    }

    std::optional<nlohmann::json> FindEvent(const std::string& method) {
        PumpOutbox();
        for (const std::string& line : io.written) {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (parsed.contains("method") && parsed["method"] == method) {
                return parsed;
            }
        }
        return std::nullopt;
    }

    app_server::IncomingResponse MakeApprovalResponse(const std::string& request_id,
                                                      const std::string& decision) {
        app_server::IncomingResponse response;
        response.id = 0;
        response.is_error = false;
        response.result = nlohmann::json{{"requestId", request_id}, {"decision", decision}};
        return response;
    }
};

}  // namespace

TEST_CASE("宿主(AppServer):并行读 + 审批 accept 的写,整回合不缺环") {
    ParallelHarness harness(lubancode::ApprovalMode::Default);
    harness.gate.enter_target = 2;
    harness.scripts.push_back(BatchScript({{"u0", "read_file", "{}"},
                                           {"u1", "read_file", "{}"},
                                           {"u2", "write_file", R"({"path":"a.txt"})"}}));
    harness.scripts.push_back(TextScript("办完了"));

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = start["threadId"];

    // 回合起在工作线程:审批悬停等前端答复,答复从测试线程喂。
    std::thread turn_thread([&] {
        std::string ec;
        harness.server->HandleTurnStart(thread_id, "读写各办", {}, ec);
    });

    std::optional<nlohmann::json> permission;
    for (int i = 0; i < 400 && !permission.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        permission = harness.FindEvent("permission/request");
    }
    REQUIRE(permission.has_value());
    CHECK((*permission)["params"]["threadId"] == thread_id);
    CHECK((*permission)["params"]["tool"].get<std::string>() == "write_file");
    CHECK_FALSE((*permission)["params"]["requestId"].get<std::string>().empty());

    harness.server->HandleInteractionResponse(harness.MakeApprovalResponse(
        (*permission)["params"]["requestId"].get<std::string>(), "accept"));
    turn_thread.join();
    harness.PumpOutbox();

    // 读并行成立:两枚都进门、没人吃等闸超时(装配没折到策略这里就红)。
    CHECK(harness.gate.gate_timeouts == 0);
    CHECK(harness.counters.read_executions.load() == 2);
    // 审批 accept 的写真执行了(独占节点,读段收口后才轮到它)。
    CHECK(harness.counters.write_executions.load() == 1);
    // 唯一终态:turn/completed,success。
    int completed = 0;
    for (const std::string& line : harness.io.written) {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (parsed.value("method", std::string()) == "turn/completed") {
            ++completed;
            CHECK(parsed["params"]["status"] == "success");
        }
    }
    CHECK(completed == 1);
}

TEST_CASE("宿主(AppServer):DontAsk 预裁定拒——不弹审批零执行,读照常并行") {
    ParallelHarness harness(lubancode::ApprovalMode::DontAsk);
    harness.gate.enter_target = 2;
    harness.scripts.push_back(BatchScript({{"u0", "read_file", "{}"},
                                           {"u1", "read_file", "{}"},
                                           {"u2", "write_file", R"({"path":"b.txt"})"}}));
    harness.scripts.push_back(TextScript("收到拒绝"));

    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    // DontAsk 没有悬停,同步驱动即可。
    harness.server->HandleTurnStart(start["threadId"], "试试写", {}, error_code);
    REQUIRE(error_code.empty());
    harness.PumpOutbox();

    CHECK(harness.gate.gate_timeouts == 0);  // 读并行未被审批拒绝挡住
    CHECK(harness.counters.read_executions.load() == 2);
    CHECK(harness.counters.write_executions.load() == 0);  // 预裁定直接拒,零执行
    CHECK(harness.FindEvent("permission/request") == std::nullopt);  // 不弹审批
    const auto completed = harness.FindEvent("turn/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["status"] == "success");
}
