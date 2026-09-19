// 常驻助理 Web 主界面单 W2 的任务/审批/补账册:方法面(task/*、
// approval/*、assistant/events/read)与装配件(AssistantAutomationRuntime
// 进程内直驱 GatewayAutomationPump)的纵向闭环。
//
// 覆盖(单 §九 W2 验收):
//   - 单次任务全链:task/create → 泵推进(执行/结算/投递)→ task/read
//     查结果(deliveryState=delivered + 冻结正文 + 发布文件在盘);
//   - 幂等:同 clientOperationId 双提交不重复执行(任务恰好一个、模型
//     恰好一次、duplicate 回原受理);
//   - 审批:needs_confirm 工具经页面审批口——超时默认拒绝不默认放行
//     (工具零执行、任务 failed、denial 如实);批准后放行(工具执行);
//   - 补账:事件账 seq 单调/bootId 绑定/缺口 reset;审批答复 stale;
//   - 取消:task/cancel CAS + 幂等重取消;任务面不可用的稳定错误。
//
// V3 会话格式的用例显式 pin EnvGuard("1")(纪律第 5 条)。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/assistant_tasks.hpp"
#include "app_server/dispatcher.hpp"
#include "gateway/automation_store.hpp"
#include "gateway/profile.hpp"
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;
using namespace lubancode::app;

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

std::int64_t WallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 假 provider:脚本吐事件;可带工具轮(第一答 tool_use,第二轮收了
// tool_result 再答正文)。计数在内存("模型恰好一次"的判据)。
class ScriptBackend : public api::Backend {
public:
    explicit ScriptBackend(std::vector<std::vector<api::StreamEvent>> scripts)
        : scripts_(std::move(scripts)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& /*request*/,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        if (cancel != nullptr && cancel->load()) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled"});
        }
        ++model_calls_;
        if (calls_ >= scripts_.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "script exhausted"});
        }
        for (const auto& event : scripts_[calls_]) {
            on_event(event);
        }
        ++calls_;
        return {};
    }

    std::size_t model_calls() const { return model_calls_; }

private:
    std::vector<std::vector<api::StreamEvent>> scripts_;
    std::size_t calls_ = 0;
    std::size_t model_calls_ = 0;
};

// 受控 needs_confirm 工具:调用计数在内存(审批闸判据)。
class GuardedTool : public tools::Tool {
public:
    explicit GuardedTool(std::string name) : name_(std::move(name)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "受控审批工具"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return true; }
    tools::Tool::Result execute(const nlohmann::json& /*input*/) override {
        ++executions_;
        return tools::Tool::Result{"guarded 工具已执行", false};
    }

    std::size_t executions() const { return executions_; }

private:
    std::string name_;
    std::atomic<std::size_t> executions_{0};
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& name) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::ToolUseStart{0, tool_id, name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 一套 W2 装配:临时根 + 假后端 + 受控工具 + hub/broker/face/runtime
// (start_thread=false,单测同步推泵)。
struct W2Fixture {
    std::filesystem::path root;
    gateway::GatewayProfilePaths paths;
    std::shared_ptr<AssistantEventHub> hub;
    std::shared_ptr<AssistantApprovalBroker> broker;
    std::shared_ptr<AssistantAutomationFace> face;
    std::unique_ptr<AssistantAutomationRuntime> runtime;
    std::unique_ptr<ScriptBackend> backend;
    std::unique_ptr<tools::ToolRegistry> registry;
    GuardedTool* guarded = nullptr;  // 所有权在 registry
    std::string boot_id = "asst-w2-test";

    explicit W2Fixture(const char* tag,
                       std::vector<std::vector<api::StreamEvent>> scripts,
                       std::int64_t approval_timeout_ms = 300) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-asst-w2-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        paths = gateway::ResolveGatewayProfilePaths(root / "gateway", "default");
        backend = std::make_unique<ScriptBackend>(std::move(scripts));
        registry = std::make_unique<tools::ToolRegistry>();
        auto guarded_tool = std::make_unique<GuardedTool>("guarded_write");
        guarded = guarded_tool.get();
        registry->Register(std::move(guarded_tool));
        hub = std::make_shared<AssistantEventHub>(boot_id);
        broker = std::make_shared<AssistantApprovalBroker>(hub.get(), approval_timeout_ms);
        face = std::make_shared<AssistantAutomationFace>(paths, hub.get(), broker.get());
        AssistantAutomationRuntime::Options options;
        options.paths = paths;
        options.workspaces_root = root / "workspaces";
        options.workspace_identity = workspace::MakeFallbackIdentity(root);
        options.cwd_utf8 = tools::PathToUtf8(root);
        options.lubancode_version = "0.26.265-test";
        options.wire_name = "test-wire";
        options.model = "test-model";
        options.approval_timeout_ms = approval_timeout_ms;
        options.max_steps_per_turn = 8;
        options.max_wall_secs = 30;
        auto opened = AssistantAutomationRuntime::Open(*backend, *registry, std::move(options),
                                                       hub.get(), broker,
                                                       /*start_thread=*/false);
        if (opened.runtime != nullptr) {
            runtime = std::move(opened.runtime);
        } else {
            face->set_available(false);
            face->set_unavailable_reason(opened.unavailable_reason);
        }
    }

    ~W2Fixture() {
        if (runtime != nullptr) {
            runtime->Stop();
        }
    }

    // 推泵到目标 occurrence 结算(或超时)。返回结算后的 occurrence。
    std::optional<gateway::AutomationOccurrence> PumpUntilSettled(const std::string& job_id,
                                                                   std::int64_t timeout_ms) {
        const std::int64_t deadline = WallMs() + timeout_ms;
        while (WallMs() < deadline) {
            runtime->TickAndPublish(WallMs());
            const gateway::AutomationProjection projection =
                gateway::ReadAutomationProjection(paths.automation_log);
            for (const auto& [id, occurrence] : projection.occurrences) {
                if (occurrence.job_id == job_id &&
                    occurrence.state == gateway::AutomationOccurrence::State::Settled) {
                    return occurrence;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::nullopt;
    }

    // W4:推泵到该 job 攒够 settled_count 枚结算(周期任务多拍判据)。
    std::vector<gateway::AutomationOccurrence> PumpUntilSettledCount(const std::string& job_id,
                                                                     std::size_t count,
                                                                     std::int64_t timeout_ms) {
        const std::int64_t deadline = WallMs() + timeout_ms;
        while (WallMs() < deadline) {
            runtime->TickAndPublish(WallMs());
            std::vector<gateway::AutomationOccurrence> settled;
            const gateway::AutomationProjection projection =
                gateway::ReadAutomationProjection(paths.automation_log);
            for (const auto& [id, occurrence] : projection.occurrences) {
                if (occurrence.job_id == job_id &&
                    occurrence.state == gateway::AutomationOccurrence::State::Settled) {
                    settled.push_back(occurrence);
                }
            }
            if (settled.size() >= count) {
                return settled;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return {};
    }

    // create/cancel 的回执要等泵消费命令(生产里泵线程自转;单测同步,
    // 这里把方法调用放后台线程,主线程推泵直到回执)。
    nlohmann::json Create(const std::string& key, const std::string& prompt,
                          std::int64_t due_at_ms = 0) {
        return PumpWhileCalling([&]() {
            int error_code = 0;
            std::string error_message;
            return face->HandleTaskCreate(
                nlohmann::json{{"prompt", prompt},
                               {"clientOperationId", key},
                               {"dueAtMs", due_at_ms}},
                error_code, error_message);
        });
    }

    // W4:带任意额外参数的创建(周期/heartbeat 面)。
    nlohmann::json CreateWithParams(const nlohmann::json& params) {
        return PumpWhileCalling([&]() {
            int error_code = 0;
            std::string error_message;
            return face->HandleTaskCreate(params, error_code, error_message);
        });
    }

    // W4:pause/resume(CAS 命令同 cancel 的回执轮询)。
    nlohmann::json StateOp(const char* method, const std::string& verb,
                           const std::string& job_id, std::int64_t expected_revision,
                           const std::string& key) {
        return PumpWhileCalling([&]() {
            int error_code = 0;
            std::string error_message;
            return verb == "pause" ? face->HandleTaskPause(
                                         nlohmann::json{{"jobId", job_id},
                                                        {"expectedRevision", expected_revision},
                                                        {"clientOperationId", key}},
                                         error_code, error_message)
                                   : face->HandleTaskResume(
                                         nlohmann::json{{"jobId", job_id},
                                                        {"expectedRevision", expected_revision},
                                                        {"clientOperationId", key}},
                                         error_code, error_message);
        });
    }

    nlohmann::json RunNow(const std::string& job_id, const std::string& key) {
        return PumpWhileCalling([&]() {
            int error_code = 0;
            std::string error_message;
            return face->HandleTaskRunNow(
                nlohmann::json{{"jobId", job_id}, {"clientOperationId", key}}, error_code,
                error_message);
        });
    }

    nlohmann::json Cancel(const std::string& job_id, std::int64_t expected_revision,
                          const std::string& key) {
        return PumpWhileCalling([&]() {
            int error_code = 0;
            std::string error_message;
            return face->HandleTaskCancel(
                nlohmann::json{{"jobId", job_id},
                               {"expectedRevision", expected_revision},
                               {"clientOperationId", key}},
                error_code, error_message);
        });
    }

    // 后台线程跑方法(它会轮询回执),主线程推泵到它返回。
    nlohmann::json PumpWhileCalling(const std::function<nlohmann::json()>& call) {
        std::future<nlohmann::json> result = std::async(std::launch::async, call);
        const std::int64_t deadline = WallMs() + 20000;
        while (result.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
            if (WallMs() > deadline) {
                return nlohmann::json();
            }
            if (runtime != nullptr) {
                runtime->TickAndPublish(WallMs());
            }
        }
        return result.get();
    }

};

}  // namespace

// ---------------------------------------------------------------------------
// 事件账(seq/bootId/缺口)
// ---------------------------------------------------------------------------

TEST_CASE("事件账:seq 单调、增量读、bootId 绑定、缺口 reset") {
    AssistantEventHub hub("boot-a", 4);
    hub.Push("assistant/task/event", {{"n", 1}});
    hub.Push("assistant/task/event", {{"n", 2}});
    hub.Push("assistant/task/event", {{"n", 3}});

    const auto incremental = hub.Read("boot-a", 1);
    CHECK(incremental.reset == false);
    REQUIRE(incremental.events.size() == 2);
    CHECK(incremental.events[0].seq == 2);
    CHECK(incremental.events[1].seq == 3);
    CHECK(incremental.current_seq == 3);

    const auto fresh = hub.Read("boot-a", 0);
    CHECK(fresh.reset == false);
    REQUIRE(fresh.events.size() == 3);

    // bootId 不符(重启过):reset + 最近一批。
    const auto rebooted = hub.Read("boot-b", 2);
    CHECK(rebooted.reset == true);
    CHECK(rebooted.events.size() == 3);

    // 容量挤出但游标仍能续(客户端见过的下一枚还在账里):正常增量。
    hub.Push("assistant/task/event", {{"n", 4}});
    hub.Push("assistant/task/event", {{"n", 5}});
    const auto stillContinuous = hub.Read("boot-a", 1);
    CHECK(stillContinuous.reset == false);
    REQUIRE(stillContinuous.events.size() == 4);
    CHECK(stillContinuous.events.front().seq == 2);

    // 缺口被帽挤出:再推一枚,seq 2 被挤出;last_seq=1 的下一枚(2)
    // 不在账里 → 续不上,reset + 最近一批。
    hub.Push("assistant/task/event", {{"n", 6}});
    const auto evicted = hub.Read("boot-a", 1);
    CHECK(evicted.reset == true);
    REQUIRE(evicted.events.size() == 4);
    CHECK(evicted.events.front().seq == 3);

    // last_seq 落在覆盖范围内:正常增量。
    const auto tail = hub.Read("boot-a", 3);
    CHECK(tail.reset == false);
    REQUIRE(tail.events.size() == 3);
    CHECK(tail.events[0].seq == 4);
}

// ---------------------------------------------------------------------------
// 单次任务全链(结果面)
// ---------------------------------------------------------------------------

TEST_CASE("任务全链:create -> 执行 -> settled -> task/read 结果与发布文件") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    W2Fixture fixture("loop",
                      {TextScript("任务结果正文:仓库干净。")});
    REQUIRE(fixture.runtime != nullptr);

    const nlohmann::json created = fixture.Create("W2-KEY-1", "检查仓库状态");
    REQUIRE(created.contains("jobId"));
    CHECK(created.value("duplicate", true) == false);
    // 受理即回的合同:occurrenceId 必须齐(job 行与 occurrence 行两笔
    // append 的中间态不许漏进回执——windows 腿栽过)。
    CHECK(created.contains("occurrenceId"));
    CHECK(created["occurrenceId"].get<std::string>().empty() == false);
    const std::string job_id = created["jobId"].get<std::string>();

    const auto settled = fixture.PumpUntilSettled(job_id, 15000);
    REQUIRE(settled.has_value());
    CHECK(settled->outcome == "succeeded");

    int error_code = 0;
    std::string error_message;
    const nlohmann::json read = fixture.face->HandleTaskRead(
        nlohmann::json{{"jobId", job_id}}, error_code, error_message);
    CHECK(error_code == 0);
    REQUIRE(read.contains("occurrences"));
    REQUIRE(read["occurrences"].size() == 1);
    const nlohmann::json& occurrence = read["occurrences"][0];
    CHECK(occurrence["state"] == "settled");
    CHECK(occurrence["outcome"] == "succeeded");
    REQUIRE(occurrence.contains("result"));
    const nlohmann::json& result = occurrence["result"];
    CHECK(result["deliveryState"] == "delivered");
    CHECK(result["replyText"].get<std::string>().find("仓库干净") != std::string::npos);
    CHECK(result["publishedPath"].get<std::string>().find("delivery/out/") !=
          std::string::npos);
    // 发布文件真在盘上(V1 的 out/<deliveryId>.txt 合同)。
    const std::filesystem::path published =
        fixture.paths.profile_dir / result["publishedPath"].get<std::string>();
    CHECK(std::filesystem::exists(published));
    // 泵恰好一次执行(幂等链上的判据)。
    CHECK(fixture.backend->model_calls() == 1);
}

TEST_CASE("任务列表:job 摘要 + 最近 occurrence + 结果状态") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    W2Fixture fixture("list", {TextScript("列表任务的结果")});
    REQUIRE(fixture.runtime != nullptr);
    const nlohmann::json created = fixture.Create("W2-KEY-LIST", "跑一次");
    // 回执防御:create 命令丢失/泵忙时方法面回 null(15 秒受理超时)。后
    // 面的下标访问打在 null 上会抛 305 炸整册——先钉住,把失败变成带人话
    // 的断言(用例语义不变)。
    REQUIRE_MESSAGE(created.is_object(), "task/create 没回执(命令丢失或泵忙),回 null");
    const std::string job_id = created["jobId"].get<std::string>();
    REQUIRE(fixture.PumpUntilSettled(job_id, 15000).has_value());

    int error_code = 0;
    std::string error_message;
    const nlohmann::json listed = fixture.face->HandleTaskList(nlohmann::json::object(),
                                                                 error_code, error_message);
    CHECK(error_code == 0);
    REQUIRE(listed["tasks"].size() == 1);
    const nlohmann::json& task = listed["tasks"][0];
    CHECK(task["jobId"] == job_id);
    CHECK(task["state"] == "active");
    CHECK(task["scheduleKind"] == "once");
    REQUIRE(task.contains("latest"));
    CHECK(task["latest"]["outcome"] == "succeeded");
    REQUIRE(task["latest"].contains("result"));
    CHECK(task["latest"]["result"]["deliveryState"] == "delivered");
}

// ---------------------------------------------------------------------------
// 幂等:两次提交不重复执行
// ---------------------------------------------------------------------------

TEST_CASE("幂等:同 clientOperationId 双提交回原受理,不双建不双跑") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    W2Fixture fixture("idem", {TextScript("幂等任务结果")});
    REQUIRE(fixture.runtime != nullptr);

    // 在飞窗口的双提交:第一枚命令还没被泵消费(账上无键),第二枚
    // 再写一枚命令文件——泵消费两枚,store 的同键幂等只建一个任务、
    // 一枚 occurrence,执行恰好一次。
    const nlohmann::json first = fixture.Create("W2-IDEM", "跑一次幂等任务");
    REQUIRE(first.contains("jobId"));
    const std::string job_id = first["jobId"].get<std::string>();
    REQUIRE(fixture.PumpUntilSettled(job_id, 15000).has_value());

    // 结算后再交同键:回原受理(duplicate),不再建任务、不再执行。
    const nlohmann::json second = fixture.Create("W2-IDEM", "跑一次幂等任务");
    CHECK(second.value("duplicate", false) == true);
    // 幂等回执同款齐整:挡分支也不许漏 occurrenceId(两行 append 中间态)。
    CHECK(second.contains("occurrenceId"));
    CHECK(second["occurrenceId"].get<std::string>().empty() == false);
    CHECK(second["jobId"] == job_id);

    int error_code = 0;
    std::string error_message;
    const nlohmann::json listed = fixture.face->HandleTaskList(nlohmann::json::object(),
                                                                 error_code, error_message);
    REQUIRE(listed["tasks"].size() == 1);
    CHECK(fixture.backend->model_calls() == 1);
}

// ---------------------------------------------------------------------------
// 审批:超时默认拒绝;批准放行
// ---------------------------------------------------------------------------

TEST_CASE("审批超时:默认拒绝,工具零执行,事件轨迹如实") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    // 两轮脚本:第一答 tool_use(guarded_write);拒绝后 tool_result(带
    // 超时文案)回模型,第二轮答正文收尾——任务终态可能 succeeded,但
    // 工具零执行是硬判据(超时默认拒绝不默认放行)。
    W2Fixture fixture("timeout",
                      {ToolUseScript("tu_1", "guarded_write"), TextScript("被拒后收尾")});
    REQUIRE(fixture.runtime != nullptr);

    const nlohmann::json created = fixture.Create("W2-APPRO-TIMEOUT", "写点东西");
    // 同上:create 回 null 时下标访问抛 type_error.305(CI run 35403217677
    // 的 windows 腿即栽在此)——钉成带人话的断言失败。
    REQUIRE_MESSAGE(created.is_object(), "task/create 没回执(命令丢失或泵忙),回 null");
    const std::string job_id = created["jobId"].get<std::string>();
    // 不答复:审批 300ms 超时 → 拒绝 → 工具不执行。
    const auto settled = fixture.PumpUntilSettled(job_id, 15000);
    REQUIRE(settled.has_value());
    CHECK(fixture.guarded->executions() == 0);

    // 事件账里审批的完整轨迹:request + resolved(timeout_declined)。
    const auto events = fixture.hub->Read("asst-w2-test", 0);
    bool saw_request = false;
    bool saw_timeout = false;
    for (const auto& entry : events.events) {
        if (entry.method == "assistant/approval/request") {
            saw_request = true;
            CHECK(entry.params.value("toolName", std::string()) == "guarded_write");
        }
        if (entry.method == "assistant/approval/resolved" &&
            entry.params.value("outcome", std::string()) == "timeout_declined") {
            saw_timeout = true;
        }
    }
    CHECK(saw_request);
    CHECK(saw_timeout);
}

TEST_CASE("审批批准:答复回灌后工具执行,任务 succeeded") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    W2Fixture fixture("accept",
                      {ToolUseScript("tu_1", "guarded_write"), TextScript("工具跑完了")});
    REQUIRE(fixture.runtime != nullptr);

    // 答复方先起:Create 的回执轮询会顺手把泵推起来(命令消费→执行→
    // 审批),答复线程必须在执行开跑前就位,否则 300ms 审批窗白等。
    // doctest 断言不进线程——结果收回来主线程断。
    std::atomic<bool> responded{false};
    std::thread approver([&fixture, &responded]() {
        const std::int64_t deadline = WallMs() + 20000;
        while (WallMs() < deadline) {
            const auto pending = fixture.broker->ListPending();
            if (!pending.empty()) {
                const std::string request_id = pending[0]["requestId"].get<std::string>();
                const auto outcome = fixture.broker->Respond(request_id, true);
                responded.store(outcome.resolved);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    // 栈回卷必 join:本用例体里任何早退(断言失败/异常)销毁 joinable
    // 线程都是 std::terminate——CI run 35374324620 的 "Terminate handler
    // called" 即此放大(真因是 create 回 null 抛 305)。RAII 保底,语义
    // 不变(正常路径在下面显式 join,这里空跑)。
    struct JoinApproverOnExit {
        std::thread& thread;
        ~JoinApproverOnExit() {
            if (thread.joinable()) {
                thread.join();
            }
        }
    } join_approver_on_exit{approver};

    const nlohmann::json created = fixture.Create("W2-APPRO-ACCEPT", "写点东西");
    REQUIRE_MESSAGE(created.is_object(), "task/create 没回执(命令丢失或泵忙),回 null");
    const std::string job_id = created["jobId"].get<std::string>();
    const auto settled = fixture.PumpUntilSettled(job_id, 20000);
    approver.join();
    REQUIRE(settled.has_value());
    CHECK(responded.load());
    CHECK(settled->outcome == "succeeded");
    CHECK(fixture.guarded->executions() == 1);

    // 审批事件的归属带上了任务上下文(jobId/occurrenceId)。
    const auto events = fixture.hub->Read("asst-w2-test", 0);
    for (const auto& entry : events.events) {
        if (entry.method == "assistant/approval/request") {
            CHECK(entry.params.value("jobId", std::string()) == job_id);
            CHECK(entry.params.value("occurrenceId", std::string()).empty() == false);
        }
    }
}

TEST_CASE("审批答复:stale 请求如实回,不冒充已答") {
    AssistantEventHub hub("boot-stale");
    AssistantApprovalBroker broker(&hub, 300);
    const auto outcome = broker.Respond("appro-nope", true);
    CHECK(outcome.resolved == false);
    CHECK(outcome.reason == "stale_request_id");

    AssistantAutomationFace face(gateway::GatewayProfilePaths{}, &hub, &broker);
    int error_code = 0;
    std::string error_message;
    const nlohmann::json result = face.HandleApprovalRespond(
        nlohmann::json{{"requestId", "appro-nope"}, {"decision", "accept"}}, error_code,
        error_message);
    CHECK(error_code == 0);
    CHECK(result.value("resolved", true) == false);
    CHECK(result.value("reason", std::string()) == "stale_request_id");
}

// ---------------------------------------------------------------------------
// 取消与不可用面
// ---------------------------------------------------------------------------

TEST_CASE("任务取消:CAS 落账,重复取消幂等") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    W2Fixture fixture("cancel", {TextScript("不该跑")});
    REQUIRE(fixture.runtime != nullptr);

    // due 放在未来一小时的 once:创建即入账,但不到点不执行。
    const nlohmann::json created = fixture.Create("W2-CANCEL", "一小时后跑",
                                                   WallMs() + 3600 * 1000);
    REQUIRE_MESSAGE(created.is_object(), "task/create 没回执(命令丢失或泵忙),回 null");
    const std::string job_id = created["jobId"].get<std::string>();
    // 推几拍(确认 scheduled 不被认领)。
    for (int i = 0; i < 3; ++i) {
        fixture.runtime->TickAndPublish(WallMs());
    }
    CHECK(fixture.backend->model_calls() == 0);

    const nlohmann::json cancelled = fixture.Cancel(job_id, 1, "W2-CANCEL-OP");
    CHECK(cancelled["state"] == "cancelled");
    // cancel 不 bump spec revision(账行记取消时的旧值,重放同源)。
    CHECK(cancelled["revision"] == 1);

    // 已取消的重复取消:回当前态(duplicate),不写第二枚命令。
    const nlohmann::json again = fixture.Cancel(job_id, 1, "W2-CANCEL-OP-2");
    CHECK(again.value("duplicate", false) == true);
    CHECK(fixture.backend->model_calls() == 0);
}

TEST_CASE("任务面不可用:方法回稳定错误,审批/事件面仍如实回") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    W2Fixture fixture("unavailable", {TextScript("不该跑")});
    // 不打开 runtime(模拟锁被占):face 标不可用。
    fixture.runtime.reset();
    fixture.face->set_available(false);
    fixture.face->set_unavailable_reason("同 profile 的 Gateway 实例正在运行");

    // 协议层门(注册器):握手齐后 task/create 回稳定错误码。
    app_server::Dispatcher dispatcher;
    RegisterAssistantTaskMethods(dispatcher, fixture.face);
    app_server::DispatchContext context;
    (void)dispatcher.HandleRequest(
        app_server::IncomingRequest{1, "initialize", nlohmann::json{{"clientName", "t"}}},
        context);
    (void)dispatcher.HandleNotification(
        app_server::IncomingNotification{"initialized", nlohmann::json::object()}, context);
    const auto blocked = dispatcher.HandleRequest(
        app_server::IncomingRequest{2, "task/create",
                                    nlohmann::json{{"prompt", "x"}, {"clientOperationId", "K"}}},
        context);
    REQUIRE(blocked.outbound.size() == 1);
    const nlohmann::json reply = nlohmann::json::parse(blocked.outbound[0], nullptr, false);
    REQUIRE(reply.contains("error"));
    CHECK(reply["error"].value("code", 0) == app_server::kErrInternalError);
    CHECK(reply["error"]["data"].value("code", std::string()) ==
          "assistant.automation_unavailable");

    // 审批/事件面不受门:approval/list 空、events/read 如实回。
    const auto approvals = dispatcher.HandleRequest(
        app_server::IncomingRequest{3, "approval/list", nlohmann::json::object()}, context);
    REQUIRE(approvals.outbound.size() == 1);
    const nlohmann::json approval_reply =
        nlohmann::json::parse(approvals.outbound[0], nullptr, false);
    CHECK(approval_reply.contains("result"));
    CHECK(approval_reply["result"]["pending"].size() == 0);
    const auto events = dispatcher.HandleRequest(
        app_server::IncomingRequest{4, "assistant/events/read",
                                    nlohmann::json{{"bootId", "asst-w2-test"}, {"lastSeq", 0}}},
        context);
    REQUIRE(events.outbound.size() == 1);
    const nlohmann::json events_reply =
        nlohmann::json::parse(events.outbound[0], nullptr, false);
    CHECK(events_reply.contains("result"));
    CHECK(events_reply["result"]["bootId"] == "asst-w2-test");
}

// ---------------------------------------------------------------------------
// 事件 diff(泵线程的状态变迁推事件)
// ---------------------------------------------------------------------------

TEST_CASE("任务事件:created/结算变迁进事件账,seq 单调") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    W2Fixture fixture("events", {TextScript("事件链结果")});
    REQUIRE(fixture.runtime != nullptr);
    const nlohmann::json created = fixture.Create("W2-EVENTS", "跑事件链");
    REQUIRE_MESSAGE(created.is_object(), "task/create 没回执(命令丢失或泵忙),回 null");
    const std::string job_id = created["jobId"].get<std::string>();
    REQUIRE(fixture.PumpUntilSettled(job_id, 15000).has_value());

    const auto read = fixture.hub->Read("asst-w2-test", 0);
    bool saw_job_created = false;
    bool saw_occurrence = false;
    bool saw_settled = false;
    std::uint64_t last_seq = 0;
    bool monotonic = true;
    for (const auto& entry : read.events) {
        if (entry.seq <= last_seq) {
            monotonic = false;
        }
        last_seq = entry.seq;
        if (entry.method != "assistant/task/event") {
            continue;
        }
        if (entry.params.value("jobId", std::string()) != job_id) {
            continue;
        }
        if (entry.params.value("kind", std::string()) == "job.created") {
            saw_job_created = true;
        }
        if (entry.params.value("kind", std::string()) == "occurrence.created") {
            saw_occurrence = true;
        }
        // 同步泵可能一拍内 claim→执行→结算,settled 的变迁按状态钉
        //(created 直落 settled 或 changed 到 settled 都算)。
        if (entry.params.value("state", std::string()) == "settled") {
            saw_settled = true;
            CHECK(entry.params.value("outcome", std::string()) == "succeeded");
        }
    }
    CHECK(monotonic);
    CHECK(saw_job_created);
    CHECK(saw_occurrence);
    CHECK(saw_settled);
}

// ---------------------------------------------------------------------------
// W4:周期任务(interval/cron 透传、pause/resume CAS、run-now、heartbeat)
// ---------------------------------------------------------------------------

TEST_CASE("周期任务:interval 创建 → 两拍执行 → 列表带计划与下次到期") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    W2Fixture fixture("interval",
                      {TextScript("第一拍结果"), TextScript("第二拍结果")});
    REQUIRE(fixture.runtime != nullptr);

    const nlohmann::json created = fixture.CreateWithParams(nlohmann::json{
        {"prompt", "周期检查"}, {"clientOperationId", "W4-INTERVAL-1"}, {"intervalSeconds", 1}});
    REQUIRE(created.contains("jobId"));
    CHECK(created.value("duplicate", true) == false);
    // 周期任务建账不建 occurrence(V2 语义:拍点归 SweepSchedule 生成)。
    CHECK(created.value("occurrenceId", std::string("有")) == std::string());
    REQUIRE(created.contains("schedule"));
    CHECK(created["schedule"]["kind"] == "interval");
    CHECK(created["schedule"]["intervalSeconds"] == 1);
    CHECK(created["schedule"].contains("nextDueMs"));
    const std::string job_id = created["jobId"].get<std::string>();

    // 两拍都结算(interval=1s;锚点=创建时刻,首拍在 1s 后)。
    const auto settled = fixture.PumpUntilSettledCount(job_id, 2, 30000);
    REQUIRE(settled.size() == 2);
    for (const auto& occurrence : settled) {
        CHECK(occurrence.outcome == "succeeded");
    }
    CHECK(fixture.backend->model_calls() == 2);

    // 列表投影:计划摘要 + misfire + 下次到期。
    int error_code = 0;
    std::string error_message;
    const nlohmann::json listed = fixture.face->HandleTaskList(nlohmann::json::object(),
                                                               error_code, error_message);
    CHECK(error_code == 0);
    REQUIRE(listed["tasks"].size() == 1);
    const nlohmann::json& task = listed["tasks"][0];
    CHECK(task["scheduleKind"] == "interval");
    REQUIRE(task.contains("schedule"));
    CHECK(task["schedule"]["kind"] == "interval");
    CHECK(task["schedule"]["intervalSeconds"] == 1);
    CHECK(task["schedule"]["misfirePolicy"] == "coalesce");
    CHECK(task["schedule"].contains("nextDueMs"));
}

TEST_CASE("周期任务:坏 cron/坏时区/互相冲突的参数在方法面明拒不猜") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    W2Fixture fixture("reject", {TextScript("不该跑")});
    REQUIRE(fixture.runtime != nullptr);

    const auto expect_reject = [&fixture](const nlohmann::json& params, const char* needle) {
        int error_code = 0;
        std::string error_message;
        const nlohmann::json result = fixture.face->HandleTaskCreate(params, error_code,
                                                                     error_message);
        CHECK(error_code == app_server::kErrInvalidParams);
        CHECK(error_message.find(needle) != std::string::npos);
    };
    // 七字段 cron(六字段也拒):受限子集是五字段,明拒不猜。
    expect_reject(nlohmann::json{{"prompt", "x"},
                                 {"clientOperationId", "W4-BAD-1"},
                                 {"cronExpr", "0 0 * * * *"}},
                  "cron");
    // 英文名字段不认。
    expect_reject(nlohmann::json{{"prompt", "x"},
                                 {"clientOperationId", "W4-BAD-2"},
                                 {"cronExpr", "0 12 * * mon"}},
                  "cron");
    // 两年内无拍(Feb-30)。
    expect_reject(nlohmann::json{{"prompt", "x"},
                                 {"clientOperationId", "W4-BAD-3"},
                                 {"cronExpr", "0 0 30 2 *"}},
                  "cron");
    // 认不得的时区。
    expect_reject(nlohmann::json{{"prompt", "x"},
                                 {"clientOperationId", "W4-BAD-4"},
                                 {"cronExpr", "*/5 * * * *"},
                                 {"timezone", "Mars/Olympus"}},
                  "时区");
    // interval 与 cron 同时给。
    expect_reject(nlohmann::json{{"prompt", "x"},
                                 {"clientOperationId", "W4-BAD-5"},
                                 {"intervalSeconds", 60},
                                 {"cronExpr", "*/5 * * * *"}},
                  "二选一");
    // interval 越界(超过 10 年上限)。
    expect_reject(nlohmann::json{{"prompt", "x"},
                                 {"clientOperationId", "W4-BAD-6"},
                                 {"intervalSeconds", 315360001}},
                  "interval");
    // 坏 misfire 政策。
    expect_reject(nlohmann::json{{"prompt", "x"},
                                 {"clientOperationId", "W4-BAD-7"},
                                 {"intervalSeconds", 60},
                                 {"misfirePolicy", "guess"}},
                  "misfirePolicy");
    // 全拒:一条命令文件都不落(控制目录零文件)。
    std::error_code ec;
    const std::size_t command_files =
        std::filesystem::exists(fixture.paths.control_dir, ec)
            ? std::distance(std::filesystem::directory_iterator(fixture.paths.control_dir, ec),
                            std::filesystem::directory_iterator())
            : 0;
    CHECK(command_files == 0);
    CHECK(fixture.backend->model_calls() == 0);
}

TEST_CASE("周期任务:pause/resume 透传 CAS;重复操作幂等;终态拒收") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    W2Fixture fixture("pause", {TextScript("不该自己跑")});
    REQUIRE(fixture.runtime != nullptr);

    // interval 拉长(1 小时):创建后不会有自动拍,pause 面好验。
    const nlohmann::json created = fixture.CreateWithParams(nlohmann::json{
        {"prompt", "一小时周期"}, {"clientOperationId", "W4-PAUSE-1"}, {"intervalSeconds", 3600}});
    REQUIRE_MESSAGE(created.is_object(), "task/create 没回执(命令丢失或泵忙),回 null");
    const std::string job_id = created["jobId"].get<std::string>();
    const std::uint64_t revision = created["revision"].get<std::uint64_t>();

    const nlohmann::json paused = fixture.StateOp("task/pause", "pause", job_id,
                                                  static_cast<std::int64_t>(revision),
                                                  "W4-PAUSE-OP");
    CHECK(paused["state"] == "paused");

    // 重复 pause:回当前态(duplicate),不写第二枚命令。
    const nlohmann::json paused_again = fixture.StateOp("task/pause", "pause", job_id,
                                                        static_cast<std::int64_t>(revision),
                                                        "W4-PAUSE-OP-2");
    CHECK(paused_again.value("duplicate", false) == true);
    CHECK(paused_again["state"] == "paused");

    // paused 期间推泵:不认领、不执行。
    for (int i = 0; i < 5; ++i) {
        fixture.runtime->TickAndPublish(WallMs());
    }
    CHECK(fixture.backend->model_calls() == 0);

    // resume:回 active;run-now 手动触发一次执行。
    const nlohmann::json state = fixture.StateOp("task/resume", "resume", job_id,
                                                 static_cast<std::int64_t>(revision),
                                                 "W4-RESUME-OP");
    CHECK(state["state"] == "active");

    const nlohmann::json run = fixture.RunNow(job_id, "W4-RUNNOW-OP");
    CHECK(run.value("duplicate", false) == false);
    REQUIRE(run.contains("occurrenceId"));
    REQUIRE(fixture.PumpUntilSettled(job_id, 15000).has_value());
    CHECK(fixture.backend->model_calls() == 1);

    // run-now 幂等:同键再触发回原 occurrence,不再执行。
    const nlohmann::json run_again = fixture.RunNow(job_id, "W4-RUNNOW-OP");
    CHECK(run_again.value("duplicate", false) == true);
    CHECK(fixture.backend->model_calls() == 1);

    // 取消后(终态)pause/resume 如实拒。
    const nlohmann::json cancelled = fixture.Cancel(job_id, static_cast<std::int64_t>(revision),
                                                    "W4-CANCEL-OP");
    REQUIRE(cancelled["state"] == "cancelled");
    int error_code = 0;
    std::string error_message;
    const nlohmann::json refused = fixture.face->HandleTaskPause(
        nlohmann::json{{"jobId", job_id},
                       {"expectedRevision", static_cast<std::int64_t>(revision)},
                       {"clientOperationId", "W4-PAUSE-LATE"}},
        error_code, error_message);
    CHECK(error_code == app_server::kErrInvalidParams);
    CHECK(error_message.find("已取消") != std::string::npos);
}

TEST_CASE("heartbeat:notifyOnChange 任务两拍——首拍投递、次拍无变化不投递") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    // 两拍同正文(假模型固定文案):第二拍观察账判"未变化",不投递。
    W2Fixture fixture("heartbeat",
                      {TextScript("状态没变:仓库干净。"), TextScript("状态没变:仓库干净。")});
    REQUIRE(fixture.runtime != nullptr);

    const nlohmann::json created = fixture.CreateWithParams(nlohmann::json{
        {"prompt", "盯仓库"},
        {"clientOperationId", "W4-HB-1"},
        {"intervalSeconds", 1},
        {"notifyOnChange", true}});
    REQUIRE_MESSAGE(created.is_object(), "task/create 没回执(命令丢失或泵忙),回 null");
    const std::string job_id = created["jobId"].get<std::string>();
    REQUIRE(created.contains("schedule"));
    CHECK(created["schedule"]["notifyOnChange"] == true);

    const auto settled = fixture.PumpUntilSettledCount(job_id, 2, 30000);
    REQUIRE(settled.size() == 2);

    // task/read 的观察投影:首拍 changed+delivered;次拍 changed=false
    // (正文未变的拍安静,不是失败)。
    int error_code = 0;
    std::string error_message;
    const nlohmann::json read = fixture.face->HandleTaskRead(nlohmann::json{{"jobId", job_id}},
                                                             error_code, error_message);
    CHECK(error_code == 0);
    REQUIRE(read["occurrences"].size() == 2);
    std::size_t changed_count = 0;
    std::size_t unchanged_suppressed = 0;
    for (const auto& occurrence : read["occurrences"]) {
        REQUIRE(occurrence.contains("observed"));
        if (occurrence["observed"]["changed"] == true) {
            ++changed_count;
            CHECK(occurrence["observed"]["delivered"] == true);
        } else {
            ++unchanged_suppressed;
            CHECK(occurrence["observed"]["delivered"] == false);
            CHECK(occurrence["outcome"] == "succeeded");
        }
    }
    CHECK(changed_count == 1);
    CHECK(unchanged_suppressed == 1);

    // 观察事件进账(V2 的 notice 通知经事件账推前端):首拍 observed
    // changed=true;次拍 changed=false 的观察也如实发。
    const auto events = fixture.hub->Read("asst-w2-test", 0);
    std::size_t observed_events = 0;
    for (const auto& entry : events.events) {
        if (entry.method == "assistant/task/event" &&
            entry.params.value("kind", std::string()) == "occurrence.observed" &&
            entry.params.value("jobId", std::string()) == job_id) {
            ++observed_events;
        }
    }
    CHECK(observed_events == 2);
}
