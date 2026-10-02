// AR-01(子代理后台线程退场仍借用已析构门面)的合同册:用可控阻塞的
// 假后端把"worker 在关闭超时后才返回"的交错钉进秒级,验证四条验收线——
//   1. 关闭超时后晚归 worker 无悬垂访问:门面析构有界返回,detach 放行;
//      放闸后 worker 靠冻结 run_state 跑完全程、落账、退出(协调器被钉活,
//      台账可查终态)。旧码在此路径读已析构门面的成员,ASan 下即
//      heap-use-after-free。
//   2. 先强收业务终态再延迟 worker 退出:监督器墙钟把台账翻成 Failed 而
//      线程仍挂——此刻再派工(ReapExitedThreads)与析构(JoinAllBounded)
//      都不得提前 join(旧码按业务终态收柄,会无期限押死)。
//   3. 并发关闭与递归派工只能稳定拒绝:RequestClose 后任何 handle 派工
//      回"收场";门面析构后兼容壳的 description/input_schema 退静态文案,
//      不摸已亡门面。
//   4. worktree 收场仍保留待审改动:晚归 worker 的 FinishIsolationRoom
//      走冻结 git_runner,有活的房照样保留并在结果里附路径。
// 栅栏全用条件变量,不靠随机 sleep 碰运气(挂死后端忽略取消旗,模拟
// "backend 不理 cancel"的绝境)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "cli/i18n.hpp"
#include "platform/process.hpp"
#include "runtime/worktree.hpp"
#include "runtime/trajectory_subagent_bridge.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/write_file.hpp"

using namespace lubancode;

namespace {

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 前台后端在本册不会被问到(全部走后台派工),给一只最简桩。
class NullBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(const api::Request&,
                                                const std::function<void(const api::StreamEvent&)>&,
                                                const std::atomic<bool>* = nullptr) override {
        return std::unexpected(api::Error{api::ErrorKind::Api, "NullBackend: 不该被问到", 0});
    }
};

// 挂死后端:第 hang_on 次请求进来先在栅栏上等测试放闸,不理取消旗(取消
// 链对它无效,正是"所有超时全失效"的绝境模型);放闸后按脚本吐事件。
// 其余请求直通——worktree 册把挂点放在第二个请求,先让工具真跑出待审
// 改动,再撞关闭窗。
class HangBackend : public api::Backend {
public:
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        int started_calls = 0;   // 已起跑的请求数(含挂住的那次)
        bool release = false;
        std::size_t hang_on = 0;  // 第几次请求挂住(0 起)
        std::vector<std::vector<api::StreamEvent>> scripts;
    };

    explicit HangBackend(std::shared_ptr<Shared> state) : state_(std::move(state)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& /*request*/,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel 故意不理:挂死绝境*/) override {
        const std::size_t idx = call_count_++;
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            ++state_->started_calls;
            state_->cv.notify_all();
            if (idx == state_->hang_on) {
                // 30 秒保险丝:测试逻辑坏了也别把 CI 挂死。
                state_->cv.wait_for(lock, std::chrono::seconds(30), [&] { return state_->release; });
            }
        }
        if (idx >= state_->scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "HangBackend: 脚本用完了", 0});
        }
        for (const auto& event : state_->scripts[idx]) {
            on_event(event);
        }
        return {};
    }

private:
    std::shared_ptr<Shared> state_;
    std::size_t call_count_ = 0;
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name,
                                            const std::string& input_json) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 等到谓词为真(20ms 一拍,带截止)。栅栏之外的轮询专用。
bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

void ReleaseHang(const std::shared_ptr<HangBackend::Shared>& state) {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release = true;
    }
    state->cv.notify_all();
}

// 等 worker 真挂进后端(第 hang_on+1 次请求已起跑)。
void AwaitHangStarted(const std::shared_ptr<HangBackend::Shared>& state) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, std::chrono::seconds(5),
                       [&] { return state->started_calls >= static_cast<int>(state->hang_on) + 1; });
}

// 真 git 临时仓库(worktree 收场用),与 test_agent_isolation 同款。
struct GitRepo {
    std::filesystem::path root;

    GitRepo() {
        const auto base = std::filesystem::temp_directory_path() /
                          ("lubancode_agentlife_" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        root = base / "repo";
        std::filesystem::create_directories(root);
        RunGit({"init", "-q", "-b", "main"});
        RunGit({"config", "user.email", "test@example.com"});
        RunGit({"config", "user.name", "Test"});
        std::ofstream(root / "seed.txt") << "seed\n";
        RunGit({"add", "."});
        RunGit({"commit", "-q", "-m", "init"});
    }
    ~GitRepo() {
        std::error_code ec;
        std::filesystem::remove_all(root.parent_path(), ec);
    }

    platform::ProcessResult RunGit(std::vector<std::string> args) const {
        std::vector<std::string> argv = {"git", "-C", PathToUtf8(root)};
        argv.insert(argv.end(), std::make_move_iterator(args.begin()), std::make_move_iterator(args.end()));
        return platform::RunProcess(argv, 60000);
    }
};

}  // namespace

namespace {

struct StartGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    void Release() {
        { std::lock_guard lock(mutex); released = true; }
        cv.notify_all();
    }
    bool AwaitEntered() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    void EnterAndWait() {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        if (!cv.wait_for(lock, std::chrono::seconds(5), [&] { return released; }))
            throw std::runtime_error("startup gate timed out");
    }
};

struct ReleaseStartGate {
    std::shared_ptr<StartGate> gate;
    ~ReleaseStartGate() { gate->Release(); }
};

class StartupWatchdog {
public:
    StartupWatchdog() : thread_([this] {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(15), [&] { return done_; })) std::abort();
    }) {}
    ~StartupWatchdog() {
        { std::lock_guard lock(mutex_); done_ = true; }
        cv_.notify_all();
        thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};

struct FailedBridgeState {
    int finishes = 0;
    bool ok = true;
    std::string reason;
};

class StartupBridge : public runtime::TrajectorySubagentBridge {
public:
    explicit StartupBridge(std::shared_ptr<FailedBridgeState> state)
        : state_(std::move(state)), turn_(nullptr, nullptr, {}, {}) {}
    const std::string& run_id() const override { return id_; }
    runtime::TrajectoryTurnBridge& turn_bridge() override { return turn_; }
    runtime::SubagentTerminalReceipt Finish(runtime::SubagentExecutionOutcome execution,
                                             const std::string& reason) override {
        ++state_->finishes;
        state_->ok = execution == runtime::SubagentExecutionOutcome::Succeeded;
        state_->reason = reason;
        runtime::SubagentTerminalReceipt receipt;
        receipt.run_id = id_;
        receipt.execution = execution;
        receipt.reason = reason;
        receipt.append_error_code = "startup.test_unconfirmed";
        return receipt;
    }
private:
    std::shared_ptr<FailedBridgeState> state_;
    std::string id_ = "startup-child";
    runtime::TrajectoryTurnBridge turn_;
};

void AttachFailedStartupBridge(tools::AgentTool& tool, const std::shared_ptr<FailedBridgeState>& state) {
    tools::AgentTool::Hooks hooks;
    hooks.trajectory_spawn = [state](const std::string&, const std::string&, runtime::SubagentSpawnFailure*, runtime::SubagentDispatchMode) {
        return std::make_unique<StartupBridge>(state);
    };
    tool.SetHooks(std::move(hooks));
}

void SetImmediateBackend(tools::AgentTool& tool) {
    auto backend = std::make_shared<HangBackend::Shared>();
    backend->release = true;
    backend->scripts = {TextScript("started after failure")};
    tool.SetDetachedBackendFactory([backend] {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<HangBackend>(backend);
        detached.request_profile.model = "startup-test-model";
        return detached;
    });
}

}  // namespace

TEST_CASE("线程启动事务: factory失败收唯一失败账与子账,退槽后可再派") {
    NullBackend backend;
    tools::ToolRegistry registry;
    tools::AgentTool tool(backend, registry, "/work/dir");
    tool.SetDispatchGovernance(1, 5);
    SetImmediateBackend(tool);
    auto bridge = std::make_shared<FailedBridgeState>();
    AttachFailedStartupBridge(tool, bridge);
    SUBCASE("native thread creation rejection") {
        tool.SetBackgroundThreadFactoryForTesting([](tools::AgentTaskCoordinator::ThreadBody) -> std::thread {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        });
    }
    SUBCASE("allocation failure in startup factory") {
        tool.SetBackgroundThreadFactoryForTesting([](tools::AgentTaskCoordinator::ThreadBody) -> std::thread {
            throw std::bad_alloc();
        });
    }
    SUBCASE("invalid empty factory result") {
        tool.SetBackgroundThreadFactoryForTesting([](tools::AgentTaskCoordinator::ThreadBody) { return std::thread{}; });
    }
    const auto failure = tool.execute({{"title", "failed-start"}, {"prompt", "never run"}, {"run_in_background", true}});
    REQUIRE(failure.is_error);
    CHECK(failure.error_code == "agent.thread_start_failed");
    CHECK(failure.content.find("已启动。") == std::string::npos);
    CHECK(failure.content.find("trajectory.child_terminal_persistence_failed") != std::string::npos);
    const auto failed = tool.coordinator()->ledger().Snapshots();
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].state == tools::AgentTaskState::Failed);
    CHECK(failed[0].outcome.status == tools::TaskOutcomeStatus::Failed);
    CHECK(failed[0].outcome.reason == tools::TaskOutcomeReason::InitializationFailed);
    CHECK_FALSE(tool.coordinator()->ledger().HasRunningTasks());
    CHECK(bridge->finishes == 1);
    CHECK_FALSE(bridge->ok);
    CHECK(bridge->reason == "thread_start_failed");
    tool.SetHooks({});
    tool.SetBackgroundThreadFactoryForTesting({});
    const auto success = tool.execute({{"title", "retry-slot"}, {"prompt", "run"}, {"run_in_background", true}});
    REQUIRE_FALSE(success.is_error);
    REQUIRE(WaitUntil([&] { return !tool.coordinator()->ledger().HasRunningTasks(); }, std::chrono::seconds(5)));
    const auto all = tool.coordinator()->ledger().Snapshots();
    REQUIRE(all.size() == 2);
    CHECK(all[0].state == tools::AgentTaskState::Failed);
    CHECK(all[1].state == tools::AgentTaskState::Done);
    CHECK(bridge->finishes == 1);
}

TEST_CASE("线程启动事务: 创建失败收干净隔离房与子账") {
    GitRepo repo;
    NullBackend backend;
    tools::ToolRegistry registry;
    tools::AgentTool tool(backend, registry, PathToUtf8(repo.root));
    SetImmediateBackend(tool);
    auto bridge = std::make_shared<FailedBridgeState>();
    AttachFailedStartupBridge(tool, bridge);
    tool.SetBackgroundThreadFactoryForTesting([](tools::AgentTaskCoordinator::ThreadBody) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    const auto failed = tool.execute({{"title", "isolated-failure"}, {"prompt", "never run"},
        {"isolation", "worktree"}, {"run_in_background", true}});
    REQUIRE(failed.is_error);
    CHECK(failed.error_code == "agent.thread_start_failed");
    const auto tasks = tool.coordinator()->ledger().Snapshots();
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0].state == tools::AgentTaskState::Failed);
    CHECK(tasks[0].worktree_removed);
    CHECK_FALSE(tasks[0].worktree_awaiting_review);
    CHECK_FALSE(tasks[0].isolation_branch.empty());
    CHECK(bridge->finishes == 1);
    CHECK_FALSE(bridge->ok);
    std::error_code error;
    const auto rooms = repo.root / ".lubancode" / "worktrees";
    if (std::filesystem::exists(rooms)) {
        for (const auto& entry : std::filesystem::directory_iterator(rooms, error))
            CHECK_FALSE(PathToUtf8(entry.path().filename()).starts_with("agent-"));
        CHECK_FALSE(error);
    }
}

TEST_CASE("线程启动事务: closing先落不调factory也不造线程") {
    tools::AgentTaskCoordinator coordinator;
    int factory_calls = 0;
    coordinator.SetThreadFactoryForTesting([&](tools::AgentTaskCoordinator::ThreadBody body) {
        ++factory_calls;
        return std::thread(std::move(body));
    });
    coordinator.RequestClose();
    auto receipt = std::make_shared<std::atomic<bool>>(false);
    CHECK_FALSE(coordinator.StartThread(1, [receipt] { receipt->store(true); }, receipt));
    CHECK(factory_calls == 0);
    CHECK_FALSE(receipt->load());
    coordinator.JoinAllBounded();
}

TEST_CASE("线程启动事务: 准备中closing拒启仍收台账与已有子账") {
    NullBackend backend;
    tools::ToolRegistry registry;
    tools::AgentTool tool(backend, registry, "/work/dir");
    const auto coordinator = tool.coordinator();
    tool.SetDetachedBackendFactory([coordinator] {
        coordinator->RequestClose();
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<NullBackend>();
        return detached;
    });
    auto bridge = std::make_shared<FailedBridgeState>();
    AttachFailedStartupBridge(tool, bridge);
    int factory_calls = 0;
    tool.SetBackgroundThreadFactoryForTesting([&](tools::AgentTaskCoordinator::ThreadBody body) {
        ++factory_calls;
        return std::thread(std::move(body));
    });
    const auto rejected = tool.execute({{"title", "closing-start"}, {"prompt", "never run"}, {"run_in_background", true}});
    REQUIRE(rejected.is_error);
    CHECK(rejected.error_code == "agent.session_closing");
    CHECK(factory_calls == 0);
    const auto tasks = coordinator->ledger().Snapshots();
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0].state == tools::AgentTaskState::Cancelled);
    CHECK(tasks[0].outcome.reason == tools::TaskOutcomeReason::SessionClosing);
    CHECK_FALSE(coordinator->ledger().HasRunningTasks());
    CHECK(bridge->finishes == 1);
    CHECK_FALSE(bridge->ok);
    CHECK(bridge->reason == "session_closing");
}

TEST_CASE("线程启动事务: 关闭接住已准入但尚未交接线程") {
    StartupWatchdog watchdog;
    auto coordinator = std::make_shared<tools::AgentTaskCoordinator>();
    auto gate = std::make_shared<StartGate>();
    auto receipt = std::make_shared<std::atomic<bool>>(false);
    coordinator->SetThreadFactoryForTesting([gate](tools::AgentTaskCoordinator::ThreadBody body) {
        gate->EnterAndWait();
        return std::thread(std::move(body));
    });
    std::future<bool> start;
    std::future<void> close;
    ReleaseStartGate release{gate};
    start = std::async(std::launch::async, [coordinator, receipt] {
        return coordinator->StartThread(1, [receipt] { receipt->store(true); }, receipt);
    });
    REQUIRE(gate->AwaitEntered());
    coordinator->RequestClose();
    close = std::async(std::launch::async, [coordinator] { coordinator->JoinAllBounded(); });
    CHECK(close.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    gate->Release();
    REQUIRE(start.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(start.get());
    REQUIRE(close.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    close.get();
    CHECK(receipt->load());
    CHECK_FALSE(coordinator->StartThread(2, [] {}, std::make_shared<std::atomic<bool>>(false)));
}

TEST_CASE("线程启动事务: 退出capture重入关闭不被Reap锁住") {
    StartupWatchdog watchdog;
    auto coordinator = std::make_shared<tools::AgentTaskCoordinator>();
    auto gate = std::make_shared<StartGate>();
    auto receipt = std::make_shared<std::atomic<bool>>(false);
    struct Cleanup {
        std::shared_ptr<tools::AgentTaskCoordinator> coordinator;
        std::shared_ptr<StartGate> gate;
        ~Cleanup() {
            gate->EnterAndWait();
            coordinator->RequestClose();
        }
    };
    std::future<void> reap;
    std::future<void> close;
    auto cleanup = std::make_shared<Cleanup>();
    cleanup->coordinator = coordinator;
    cleanup->gate = gate;
    ReleaseStartGate release{gate};
    REQUIRE(coordinator->StartThread(1, [cleanup, receipt] { receipt->store(true); }, receipt));
    cleanup.reset();
    REQUIRE(gate->AwaitEntered());
    reap = std::async(std::launch::async, [coordinator] { coordinator->ReapExitedThreads(); });
    REQUIRE(WaitUntil([&] { return coordinator->HasReapingThreadForTesting(); }, std::chrono::seconds(5)));
    coordinator->RequestClose();
    close = std::async(std::launch::async, [coordinator] { coordinator->JoinAllBounded(); });
    CHECK(close.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    gate->Release();
    REQUIRE(reap.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    reap.get();
    REQUIRE(close.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    close.get();
    CHECK(coordinator->closing());
}

// ---- 验收 1:关闭超时后晚归 worker 无悬垂访问 ------------------------------
TEST_CASE("线程寿命: 门面析构有界返回,晚归 worker 靠冻结 run_state 跑完落账") {
    NullBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    auto hang = std::make_shared<HangBackend::Shared>();
    hang->scripts = {TextScript("晚归结论:账落齐了")};

    std::shared_ptr<tools::AgentTaskCoordinator> coordinator;
    std::optional<tools::AgentTool> agent_tool(
        std::in_place, foreground_backend, sub_registry, "/work/dir");
    coordinator = agent_tool->coordinator();
    agent_tool->SetDetachedBackendFactory([hang]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<HangBackend>(hang);
        detached.request_profile.model = "hang-model";
        return detached;
    });
    // 收口窗收窄:交错压进百毫秒级,不真睡 10 秒。
    coordinator->SetShutdownJoinWindow(std::chrono::milliseconds(150));

    const auto launch = agent_tool->execute(
        nlohmann::json{{"title", "挂死后跑"}, {"prompt", "在后台查清楚"}, {"run_in_background", true}});
    REQUIRE_FALSE(launch.is_error);
    CHECK(launch.content.find("#1") != std::string::npos);
    AwaitHangStarted(hang);

    // 析构有界返回:取消已广播、后端不理取消,窗口尽了 detach 放行。
    const auto dtor_begin = std::chrono::steady_clock::now();
    agent_tool.reset();
    const auto dtor_duration = std::chrono::steady_clock::now() - dtor_begin;
    CHECK(dtor_duration < std::chrono::seconds(2));

    // 放闸:worker 在门面身后继续跑。旧码此刻读已析构门面的成员(ASan 红);
    // 新码全程吃冻结 run_state——台账被协调器钉活,终态照落。
    ReleaseHang(hang);
    REQUIRE(WaitUntil([&] { return !coordinator->ledger().HasRunningTasks(); }, std::chrono::seconds(10)));
    const auto snapshots = coordinator->ledger().Snapshots();
    REQUIRE(snapshots.size() == 1);
    // 析构的取消广播先于放闸落进任务账:终态收成 Cancelled(不是 Done),
    // 结论文本照留——"取消归取消,晚归的账一笔不丢"正是要验的合同。
    CHECK(snapshots[0].state == tools::AgentTaskState::Cancelled);
    CHECK(snapshots[0].result == "晚归结论:账落齐了");

    // 收柄按回执行到点:线程已退,ReapExitedThreads 不 hang。
    coordinator->ReapExitedThreads();
}

// ---- 验收 2:先强收业务终态,线程未退,主线程不提前 join --------------------
TEST_CASE("线程寿命: 墙钟强收翻 Failed 后,再派工与析构都不提前 join") {
    NullBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    auto hang = std::make_shared<HangBackend::Shared>();
    hang->scripts = {TextScript("晚归结论:强收没吃掉我")};

    std::optional<tools::AgentTool> agent_tool(
        std::in_place, foreground_backend, sub_registry, "/work/dir");
    auto coordinator = agent_tool->coordinator();
    agent_tool->SetDetachedBackendFactory([hang]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<HangBackend>(hang);
        return detached;
    });
    agent_tool->SetWallClockTimeout(/*secs=*/1, /*grace=*/1);
    coordinator->SetShutdownJoinWindow(std::chrono::milliseconds(150));

    const auto launch = agent_tool->execute(
        nlohmann::json{{"title", "挂死等强收"}, {"prompt", "慢慢查"}, {"run_in_background", true}});
    REQUIRE_FALSE(launch.is_error);
    AwaitHangStarted(hang);

    // 监督器墙钟落锤:台账翻 Failed/WallClockTimeout,而 OS 线程仍挂在后端里
    // ——业务终态先行,线程未退。这正是旧码"按业务终态收柄"的雷区。
    REQUIRE(WaitUntil([&] {
        const auto snapshots = coordinator->ledger().Snapshots();
        return !snapshots.empty() && snapshots[0].state == tools::AgentTaskState::Failed;
    }, std::chrono::seconds(10)));

    // 此刻再派一只(先撤墙钟,别让二号也被强收):孵化口的收柄
    // (ReapExitedThreads)不得被"终态先行"骗去 join 还在跑的线程——旧码在
    // 这里无期限押死,本册超时即红。
    agent_tool->SetWallClockTimeout(0);
    const auto second_begin = std::chrono::steady_clock::now();
    const auto second = agent_tool->execute(
        nlohmann::json{{"title", "二号任务"}, {"prompt", "接着查"}, {"run_in_background", true}});
    const auto second_duration = std::chrono::steady_clock::now() - second_begin;
    REQUIRE_FALSE(second.is_error);
    CHECK(second.content.find("#2") != std::string::npos);
    CHECK(second_duration < std::chrono::seconds(2));

    // 析构同样不得提前 join:窗口尽 detach 放行。
    const auto dtor_begin = std::chrono::steady_clock::now();
    agent_tool.reset();
    const auto dtor_duration = std::chrono::steady_clock::now() - dtor_begin;
    CHECK(dtor_duration < std::chrono::seconds(2));

    // 放闸收尾:一号保持强收那份终态(Failed/WallClockTimeout),只补"晚归"
    // 事件,不把账翻回去;二号正常 Done。
    ReleaseHang(hang);
    const std::string late_text = lubancode::cli::tr("agent_outcome.wall_clock_late");
    REQUIRE(WaitUntil([&] {
        for (const auto& event : coordinator->ledger().Events(1)) {
            if (event.text == late_text) {
                return true;
            }
        }
        return false;
    }, std::chrono::seconds(10)));
    REQUIRE(WaitUntil([&] { return !coordinator->ledger().HasRunningTasks(); }, std::chrono::seconds(10)));
    const auto snapshots = coordinator->ledger().Snapshots();
    REQUIRE(snapshots.size() == 2);
    const auto task1 = coordinator->ledger().Detail(1);
    const auto task2 = coordinator->ledger().Detail(2);
    REQUIRE(task1.has_value());
    REQUIRE(task2.has_value());
    CHECK(task1->state == tools::AgentTaskState::Failed);
    CHECK(task1->outcome.reason == tools::TaskOutcomeReason::WallClockTimeout);
    // 二号被析构的取消广播收成 Cancelled,结论文本照留。
    CHECK(task2->state == tools::AgentTaskState::Cancelled);
    CHECK(task2->result == "晚归结论:强收没吃掉我");
    // 析构已发生,协调器仍被晚归线程钉活——查账不崩,收口旗常真。
    CHECK(coordinator->closing());
    coordinator->ReapExitedThreads();
}

// ---- 验收 3:并发关闭与递归派工只能稳定拒绝 --------------------------------
TEST_CASE("线程寿命: 收场后派工稳定拒绝,兼容壳退静态文案不摸亡门面") {
    NullBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    std::optional<tools::AgentTool> agent_tool(
        std::in_place, foreground_backend, sub_registry, "/work/dir");
    const auto coordinator = agent_tool->coordinator();
    REQUIRE(coordinator != nullptr);

    // 收场中的稳定拒绝:RequestClose 之后任何 handle 派工都回"收场"。
    coordinator->RequestClose();
    const auto refused = agent_tool->execute(nlohmann::json{{"title", "关门后"}, {"prompt", "查"}});
    CHECK(refused.is_error);
    CHECK(refused.content.find("收场") != std::string::npos);

    // 门面身后:兼容壳(env 为空的 main handle)不再摸门面——description/
    // input_schema 退静态文案,不崩(协调器随门面走,weak 落空走空表兜底)。
    tools::AgentDispatchTool shell(*agent_tool);
    agent_tool.reset();
    const std::string fallback_description = shell.description();
    const auto fallback_schema = shell.input_schema();
    CHECK_FALSE(fallback_description.empty());
    CHECK(fallback_schema.is_object());
}

// ---- 验收 4:worktree 收场仍保留待审改动(晚归路)--------------------------
TEST_CASE("线程寿命: 晚归 worker 的 worktree 收场照跑,有活的房保留待审") {
    GitRepo repo;
    NullBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<tools::WriteFileTool>());
    auto hang = std::make_shared<HangBackend::Shared>();
    hang->scripts = {
        ToolUseScript("t1", "write_file", "{\"path\":\"out.txt\",\"content\":\"late\"}"),
        TextScript("晚归写手:交活"),
    };
    hang->hang_on = 1;  // 第一次请求直通(工具真跑出待审改动),第二次挂住撞关闭窗

    std::shared_ptr<tools::AgentTaskCoordinator> coordinator;
    {
        std::optional<tools::AgentTool> agent_tool(
            std::in_place, foreground_backend, sub_registry, PathToUtf8(repo.root));
        coordinator = agent_tool->coordinator();
        agent_tool->SetDetachedBackendFactory([hang]() {
            tools::DetachedAgentBackend detached;
            detached.backend = std::make_unique<HangBackend>(hang);
            return detached;
        });
        agent_tool->SetDetachedRegistryFactory([]() {
            auto registry = std::make_unique<tools::ToolRegistry>();
            registry->Register(std::make_unique<tools::WriteFileTool>());
            return registry;
        });
        // 后台免问只有预放行一条路:不放行 write_file,工具调用会被
        // "后台无法弹权限确认"拒掉,房永远干净、收场即删——测不到
        // "有活的房保留待审"。
        agent_tool->SetBackgroundPermissionSource([]() {
            tools::BackgroundPermissionLedger ledger;
            ledger.always_allowed.insert("write_file");
            return ledger;
        });
        coordinator->SetShutdownJoinWindow(std::chrono::milliseconds(150));

        const auto launch = agent_tool->execute(
            nlohmann::json{{"title", "房里晚归"}, {"prompt", "去房里写个文件"},
                           {"isolation", "worktree"}, {"run_in_background", true}});
        REQUIRE_FALSE(launch.is_error);
        AwaitHangStarted(hang);
    }  // ~AgentTool:worker 还挂在后端里,房仍在。

    // 放闸:晚归 worker 在房里写文件、收工走冻结 git_runner 清房——有活
    //(未提交改动)的房保留待审,结果照常落账。
    ReleaseHang(hang);
    REQUIRE(WaitUntil([&] { return !coordinator->ledger().HasRunningTasks(); }, std::chrono::seconds(15)));
    const auto snapshots = coordinator->ledger().Snapshots();
    REQUIRE(snapshots.size() == 1);
    // 取消广播先落,终态 Cancelled;但房里的待审改动与房态账不受影响。
    CHECK(snapshots[0].state == tools::AgentTaskState::Cancelled);
    CHECK(snapshots[0].worktree_awaiting_review);
    CHECK_FALSE(snapshots[0].isolation_branch.empty());
    CHECK(snapshots[0].result.find("晚归写手") != std::string::npos);

    // 房真在:agent- 前缀、文件落进房。
    bool saw_room = false;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(repo.root / ".lubancode" / "worktrees", ec)) {
        const std::string name = PathToUtf8(entry.path().filename());
        if (name.starts_with("agent-")) {
            saw_room = true;
            CHECK(std::filesystem::exists(entry.path() / "out.txt"));
        }
    }
    CHECK(saw_room);
    coordinator->ReapExitedThreads();
}
