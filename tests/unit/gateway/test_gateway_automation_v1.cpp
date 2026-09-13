// 常驻总装 V1 的纵向闭环册 + 放行门故障注入册(单子 §十 V1):
// 一次任务从受理到本地结果的最短闭环,与三处硬杀窗口的不变量。
//
// 放行门口径(单内原文):假 provider + 受控工具,进程退出重启后能查原
// 任务与原结果;生成结束、入 outbox、发布本地文件三处硬杀窗口都不多
// 调用模型或工具。
//
// "进程退出重启"的模拟法(与 V0 durability 册同款,如实分账):盘上账
// 是唯一真源,内存装配全部销毁重建——语义等价于进程死透再拉起。模型/
// 工具调用计数落盘上计数文件(每次调用追加一行),重开后仍在——这是
// "不多调用"的判据。真起子进程在具名栅栏硬杀的冒烟未做(CI 内不可重
// 复),单内勾选如实记"未验"。
//
// V3 会话格式的用例显式 pin EnvGuard("1")(纪律第 5 条);另钉一案
// pin "0" 验"遇 v2 明报拒绝"(单子 §二:不偷偷双写)。
#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "gateway/automation_store.hpp"
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "gateway/status.hpp"
#include "runtime/automation_pump.hpp"
#include "runtime/headless_executor.hpp"
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"  // ResolveDirByWorkspaceKey(恢复路同款 resolver)

using namespace lubancode;

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

// 盘上调用计数:每次模型/工具调用追加一行。进程"重启"(装配销毁重建)
// 后仍在——不多调用的判据。
void CountCall(const std::filesystem::path& counter, const std::string& who) {
    std::error_code ec;
    std::filesystem::create_directories(counter.parent_path(), ec);
    std::ofstream stream(counter, std::ios::binary | std::ios::app);
    stream << who << "\n";
}

std::size_t CountOf(const std::filesystem::path& counter, const std::string& who) {
    std::ifstream stream(counter, std::ios::binary);
    std::string line;
    std::size_t count = 0;
    while (std::getline(stream, line)) {
        if (line == who) ++count;
    }
    return count;
}

// 假 provider:脚本吐事件;可选工具轮(第一答 tool_use,第二轮收了
// tool_result 再答正文)。cancel 置位时返回 Cancelled(取消链判据)。
class ScriptBackend : public api::Backend {
public:
    ScriptBackend(std::filesystem::path counter, std::vector<std::vector<api::StreamEvent>> scripts)
        : counter_(std::move(counter)), scripts_(std::move(scripts)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& /*request*/,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        if (cancel != nullptr && cancel->load()) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled"});
        }
        CountCall(counter_, "model");
        if (calls_ >= scripts_.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "script exhausted"});
        }
        for (const auto& event : scripts_[calls_]) {
            on_event(event);
        }
        ++calls_;
        return {};
    }

private:
    std::filesystem::path counter_;
    std::vector<std::vector<api::StreamEvent>> scripts_;
    std::size_t calls_ = 0;
};

// 受控工具:调用计数落盘;结果固定文本。
class ControlledTool : public tools::Tool {
public:
    ControlledTool(std::filesystem::path counter, std::string name)
        : counter_(std::move(counter)), name_(std::move(name)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "受控测试工具"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json& /*input*/) override {
        CountCall(counter_, "tool");
        return tools::Tool::Result{"工具观测:仓库干净,无新提交。", false};
    }

private:
    std::filesystem::path counter_;
    std::string name_;
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

// 一套纵向装配:profile + workspaces + 泵。重建即"进程重启"。
struct V1Fixture {
    std::filesystem::path root;
    gateway::GatewayProfilePaths paths;
    std::filesystem::path workspaces_root;
    std::filesystem::path counter_file;
    std::vector<std::vector<api::StreamEvent>> scripts;

    explicit V1Fixture(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-gw-v1-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        paths = gateway::ResolveGatewayProfilePaths(root / "gateway", "default");
        workspaces_root = root / "workspaces";
        counter_file = root / "calls.log";
    }

    std::unique_ptr<ScriptBackend> MakeBackend() {
        return std::make_unique<ScriptBackend>(counter_file, scripts);
    }

    runtime::GatewayAutomationPump::OpenResult OpenPump(
        runtime::GatewayAutomationPump* pump, api::Backend& backend, tools::ToolRegistry& registry,
        const std::function<std::string(runtime::HeadlessExecutor::Options::FaultPoint)>& fault = {},
        const std::function<std::string()>& fault_after_enqueue = {}) {
        runtime::GatewayAutomationPump::Options options;
        options.paths = paths;
        options.workspaces_root = workspaces_root;
        options.cwd_utf8 = tools::PathToUtf8(root);
        options.lubancode_version = "0.26.238-test";
        options.wire_name = "test-wire";
        options.model = "test-model";
        options.tools.allow = {"repo_probe"};
        options.fault_injection = fault;
        options.fault_after_enqueue = fault_after_enqueue;
        // 身份:fallback(测试 root 就是身份根)。
        options.workspace_identity = workspace::MakeFallbackIdentity(root);
        auto open = runtime::GatewayAutomationPump::Open(pump, backend, registry,
                                                         std::move(options));
        if (open.ok) {
            pump->set_owner_epoch("gw-test-epoch");
        }
        return open;
    }
};

tools::ToolRegistry MakeRegistry(const std::filesystem::path& counter) {
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<ControlledTool>(counter, "repo_probe"));
    return registry;
}

}  // namespace

// ---------------------------------------------------------------------------
// 纵向闭环
// ---------------------------------------------------------------------------

TEST_CASE("闭环:once 任务 -> V3 执行(含工具轮)-> reply selection -> 本地结果文件") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V1Fixture fixture("loop");
    // 第一轮模型答 tool_use,第二轮收了工具结果答正文——真实工具 Action
    // 栅栏(ToolTraceHub)与多步执行都在这条路上。
    fixture.scripts = {ToolUseScript("tu-1", "repo_probe"), TextScript("检查完毕:一切正常。")};

    auto backend = fixture.MakeBackend();
    tools::ToolRegistry registry = MakeRegistry(fixture.counter_file);
    runtime::GatewayAutomationPump pump;
    REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);

    // 持久任务经泵的命令面创建(不直接改文件)。
    gateway::GatewayJobAddCommand add;
    add.prompt = "检查指定仓库";
    add.idempotency_key = "k-loop";
    REQUIRE(gateway::WriteJobAddCommand(fixture.paths.control_dir, add).empty());

    REQUIRE(pump.TickOnce(1000));  // 消费命令(本 tick 先入账)
    REQUIRE(pump.TickOnce(2000));  // claim + 执行 + 选择 + 入箱 + 投递

    // 领域账:occurrence 结算 succeeded,带 session/turn 绑定。
    const gateway::AutomationProjection projection = gateway::ReadAutomationProjection(
        fixture.paths.automation_log);
    REQUIRE(projection.occurrences.size() == 1);
    const auto& occurrence = projection.occurrences.begin()->second;
    CHECK(occurrence.state == gateway::AutomationOccurrence::State::Settled);
    CHECK(occurrence.outcome == "succeeded");
    CHECK(!occurrence.session_id.empty());
    CHECK(!occurrence.turn_id.empty());

    // 调用计数:模型 2 次(工具轮 + 终答)、工具 1 次。
    CHECK(CountOf(fixture.counter_file, "model") == 2);
    CHECK(CountOf(fixture.counter_file, "tool") == 1);

    // V3 流:gateway.work.bound 与 reply.selection.committed 都在。
    const auto room = workspace::ResolveDirByWorkspaceKey(
        fixture.workspaces_root, workspace::MakeFallbackIdentity(fixture.root).workspace_key);
    REQUIRE(room.has_value());
    const auto stream = trajectory::v3::FindV3SessionStream(*room / "sessions" /
                                                             occurrence.session_id);
    REQUIRE(stream.has_value());
    const auto ledger = trajectory::v3::ReadV3Ledger(*stream);
    REQUIRE(ledger.has_value());
    bool saw_bound = false;
    bool saw_selection = false;
    std::string published_text;
    for (const auto& event : ledger->events) {
        if (event.kind == trajectory::v3::EventKindV3::GatewayWorkBound) {
            saw_bound = true;
            REQUIRE(event.payload.contains("workId"));
            REQUIRE(event.payload.contains("ownerEpoch"));
            CHECK(event.payload["workId"] == occurrence.occurrence_id);
            CHECK(event.payload["ownerEpoch"] == "gw-test-epoch");
        }
        if (event.kind == trajectory::v3::EventKindV3::ReplySelectionCommitted) {
            saw_selection = true;
            REQUIRE(event.payload.contains("selectionId"));
            CHECK(event.payload["selectionId"] == "sel-" + occurrence.turn_id);
        }
    }
    CHECK(saw_bound);
    CHECK(saw_selection);

    // 本地结果文件:内容 = 冻结正文。
    const std::string delivery_id = gateway::MakeDeliveryId("sel-" + occurrence.turn_id,
                                                            "local:file", 1);
    const auto published = fixture.paths.published_dir / (delivery_id + ".txt");
    REQUIRE(std::filesystem::exists(published));
    std::ifstream published_stream(published, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(published_stream)),
                           std::istreambuf_iterator<char>());
    CHECK(text == "检查完毕:一切正常。");

    // status 分栏:work succeeded 1、delivery delivered 1(进程不跑也能查)。
    const auto sections = gateway::ProbeStatusSections(fixture.paths);
    CHECK(sections.occurrences_succeeded == 1);
    CHECK(sections.delivery_delivered == 1);
    CHECK(sections.delivery_pending == 0);
}

TEST_CASE("v2 场明报拒绝:pin 0 时执行不偷偷双写") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    V1Fixture fixture("v2refuse");
    fixture.scripts = {TextScript("不该跑到这里")};

    auto backend = fixture.MakeBackend();
    tools::ToolRegistry registry = MakeRegistry(fixture.counter_file);
    runtime::GatewayAutomationPump pump;
    REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
    REQUIRE(pump.store()->CreateOnceJob("j", "问", 1000, 1000, "").accepted);
    REQUIRE(pump.TickOnce(2000));

    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    REQUIRE(projection.occurrences.size() == 1);
    const auto& occurrence = projection.occurrences.begin()->second;
    CHECK(occurrence.state == gateway::AutomationOccurrence::State::Settled);
    CHECK(occurrence.outcome == "failed");
    CHECK(CountOf(fixture.counter_file, "model") == 0);  // 模型一次都没调
}

TEST_CASE("取消链:cancel 旗置位即掐断,occurrence 如实 failed") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V1Fixture fixture("cancel");
    fixture.scripts = {TextScript("不该跑完")};

    auto backend = fixture.MakeBackend();
    tools::ToolRegistry registry = MakeRegistry(fixture.counter_file);
    runtime::GatewayAutomationPump pump;
    REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
    REQUIRE(pump.store()->CreateOnceJob("j", "问", 1000, 1000, "").accepted);

    // 泵的 cancel 旗没有外部口(生产属 Shutdown 面);这里经执行器直接验
    // 装配的取消链:backend 收 cancel 旗即 Cancelled 分型,Executor 结
    // 果 error=cancelled。用一个持有 cancel 的 ScriptBackend 包装——泵
    // 内部 cancel_flag 恒 false,所以直接驱动 executor 一场。
    std::atomic<bool> cancel{true};
    // ScriptBackend 在 cancel 置位时回 Cancelled:经泵跑(TickOnce 内部
    // cancel 恒 false)无法注入,改为直接构造 executor 验装配链。
    runtime::HeadlessExecutor::Options options;
    options.workspaces_root = fixture.workspaces_root;
    options.workspace_root = fixture.root;
    options.cwd_utf8 = tools::PathToUtf8(fixture.root);
    options.lubancode_version = "0.26.238-test";
    options.wire_name = "test-wire";
    options.model = "test-model";
    options.replies_dir = fixture.paths.replies_dir;
    options.tools.allow = {"repo_probe"};
    runtime::HeadlessExecutor executor(*backend, registry, std::move(options));
    runtime::HeadlessWorkBinding binding;
    binding.work_id = "work-cancel";
    binding.source_kind = "automation";
    binding.source_id = "j";
    binding.owner_epoch = "e";
    const auto result = executor.Execute("问", binding, {}, &cancel);
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "gateway.turn_failed");
    CHECK(CountOf(fixture.counter_file, "model") == 0);  // cancel 先于发送
}

// ---------------------------------------------------------------------------
// 放行门:三处硬杀窗口(装配销毁重建 = 进程退出重启;计数在盘上)
// ---------------------------------------------------------------------------

namespace {

// 三窗共用骨架:注入 fault 跑半程 -> 销毁装配(进程死)-> 重建 -> 恢复
// tick -> 断言不多调模型/工具、结果落文件、occurrence succeeded。
void RunHardKillWindowCase(const char* tag,
                           runtime::HeadlessExecutor::Options::FaultPoint point) {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V1Fixture fixture(tag);
    fixture.scripts = {TextScript("窗口期答复。")};

    std::string fault_hit;
    {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry(fixture.counter_file);
        runtime::GatewayAutomationPump pump;
        auto fault = [&fault_hit, point](runtime::HeadlessExecutor::Options::FaultPoint at) {
            if (at == point) {
                fault_hit = "hit";
                return std::string("hard_kill");
            }
            return std::string();
        };
        REQUIRE(fixture.OpenPump(&pump, *backend, registry, fault).ok);
        REQUIRE(pump.store()->CreateOnceJob("j", "窗口任务", 1000, 1000, "").accepted);
        REQUIRE(pump.TickOnce(2000));  // 跑到注入点"死掉"
    }
    REQUIRE(fault_hit == "hit");
    // 半程死掉:模型恰好调过 1 次(生成已完成)、工具 0 次。
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    REQUIRE(CountOf(fixture.counter_file, "tool") == 0);
    // 账上:claimed 未结算(进程死,结算行也没落)。
    {
        const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
        REQUIRE(projection.occurrences.size() == 1);
        REQUIRE(projection.occurrences.begin()->second.state ==
                gateway::AutomationOccurrence::State::Claimed);
    }

    // "重启":全新装配(backend 计数仍落同一文件),无 fault。
    {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry(fixture.counter_file);
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(pump.TickOnce(9000));  // 恢复扫描 + 补选择 + 入箱 + 投递
        // 没有新活可跑(恢复 tick 不再执行)。
        REQUIRE(pump.TickOnce(9500));
    }

    // 不变量:模型总数仍 1、工具仍 0——恢复全程没碰模型/工具。
    CHECK(CountOf(fixture.counter_file, "model") == 1);
    CHECK(CountOf(fixture.counter_file, "tool") == 0);

    // 结果:occurrence succeeded;发布文件在,内容即窗口期答复。
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    REQUIRE(projection.occurrences.size() == 1);
    const auto& occurrence = projection.occurrences.begin()->second;
    CHECK(occurrence.state == gateway::AutomationOccurrence::State::Settled);
    CHECK(occurrence.outcome == "succeeded");
    const std::string delivery_id =
        gateway::MakeDeliveryId("sel-" + occurrence.turn_id, "local:file", 1);
    const auto published = fixture.paths.published_dir / (delivery_id + ".txt");
    REQUIRE(std::filesystem::exists(published));
    std::ifstream stream(published, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(text == "窗口期答复。");
}

}  // namespace

TEST_CASE("窗口 1:生成结束、reply selection 未提交——重启补齐,不调模型") {
    RunHardKillWindowCase("window1",
                          runtime::HeadlessExecutor::Options::FaultPoint::AfterGeneration);
}

TEST_CASE("窗口 2:selection 已提交、outbox 未投影——重启补同一 delivery") {
    RunHardKillWindowCase("window2",
                          runtime::HeadlessExecutor::Options::FaultPoint::AfterSelectionCommitted);
}

TEST_CASE("窗口 3:入 outbox 后、发布本地文件前——重启续投,不重跑执行") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V1Fixture fixture("window3");
    fixture.scripts = {TextScript("入箱后答复。")};

    {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry(fixture.counter_file);
        runtime::GatewayAutomationPump pump;
        bool fault_armed = false;
        auto fault_after_enqueue = [&fault_armed]() {
            fault_armed = true;
            return std::string("hard_kill");
        };
        REQUIRE(fixture.OpenPump(&pump, *backend, registry, {}, fault_after_enqueue).ok);
        REQUIRE(pump.store()->CreateOnceJob("j", "窗口三", 1000, 1000, "").accepted);
        REQUIRE(pump.TickOnce(2000));  // 执行 + 入箱 + 注入"死"
        REQUIRE(fault_armed);
    }
    // 半程:模型 1 次;outbox 已入箱但未发布。
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    {
        const auto outbox = gateway::ReadOutboxProjection(fixture.paths.outbox_log);
        REQUIRE(outbox.items.size() == 1);
        REQUIRE(outbox.items.begin()->second.state == "pending");
    }

    // 重启:恢复扫描发现 claimed 未结算——V3 selection 已在,直接续投。
    {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry(fixture.counter_file);
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(pump.TickOnce(9000));
    }

    CHECK(CountOf(fixture.counter_file, "model") == 1);  // 没多调模型
    CHECK(CountOf(fixture.counter_file, "tool") == 0);
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    REQUIRE(projection.occurrences.size() == 1);
    const auto& occurrence = projection.occurrences.begin()->second;
    CHECK(occurrence.state == gateway::AutomationOccurrence::State::Settled);
    CHECK(occurrence.outcome == "succeeded");
    const std::string delivery_id =
        gateway::MakeDeliveryId("sel-" + occurrence.turn_id, "local:file", 1);
    const auto published = fixture.paths.published_dir / (delivery_id + ".txt");
    REQUIRE(std::filesystem::exists(published));
    std::ifstream stream(published, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(text == "入箱后答复。");
    // outbox 不多一箱(同 deliveryId 幂等)。
    const auto outbox = gateway::ReadOutboxProjection(fixture.paths.outbox_log);
    CHECK(outbox.items.size() == 1);
    CHECK(outbox.items.begin()->second.state == "delivered");
}

// ---------------------------------------------------------------------------
// 放行门:进程退出重启后能查原任务与原结果
// ---------------------------------------------------------------------------

TEST_CASE("重启可查:泵销毁后,任务与结果都能从账只读查回") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V1Fixture fixture("query");
    fixture.scripts = {TextScript("可查答复。")};
    {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry(fixture.counter_file);
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(pump.store()->CreateOnceJob("daily-check", "每天九点检查仓库", 1000, 1000, "k1")
                    .accepted);
        REQUIRE(pump.TickOnce(2000));
        REQUIRE(pump.TickOnce(3000));
    }
    // "进程退出":上面装配已销毁。查询只走只读投影。
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    REQUIRE(projection.jobs.size() == 1);
    CHECK(projection.jobs.begin()->second.job_id == "daily-check");
    CHECK(projection.jobs.begin()->second.prompt == "每天九点检查仓库");
    REQUIRE(projection.occurrences.size() == 1);
    const auto& occurrence = projection.occurrences.begin()->second;
    CHECK(occurrence.outcome == "succeeded");

    const auto sections = gateway::ProbeStatusSections(fixture.paths);
    CHECK(sections.work_ledger_present);
    CHECK(sections.occurrences_succeeded == 1);
    CHECK(sections.delivery_delivered == 1);
    REQUIRE(sections.recent_executions.size() == 1);
    CHECK(sections.recent_executions[0].session_id == occurrence.session_id);
    CHECK(sections.recent_executions[0].turn_id == occurrence.turn_id);

    // 结果正文:从 delivery 原件读回。
    const auto item = gateway::ReadOutboxProjection(fixture.paths.outbox_log).items.begin()->second;
    std::ifstream reply_stream(fixture.paths.replies_dir / (item.selection_id + ".txt"),
                               std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(reply_stream)),
                           std::istreambuf_iterator<char>());
    CHECK(text == "可查答复。");
}

// ---------------------------------------------------------------------------
// 收尾次序(单子 V1 第一件:先暂停接活,再摘 wake,再收执行器和 writer)
// ---------------------------------------------------------------------------

TEST_CASE("关机次序:StopAccepting 后不取新活;Close 幂等收净") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V1Fixture fixture("shutdown");
    fixture.scripts = {TextScript("关机前答复。")};
    auto backend = fixture.MakeBackend();
    tools::ToolRegistry registry = MakeRegistry(fixture.counter_file);
    runtime::GatewayAutomationPump pump;
    REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
    // 一枚未来任务(不到点)+ 一枚立即任务。
    REQUIRE(pump.store()->CreateOnceJob("future", "未来", 999999999, 1000, "").accepted);
    REQUIRE(pump.store()->CreateOnceJob("now", "现在", 1000, 1000, "").accepted);

    pump.StopAccepting();  // 暂停接活
    CHECK_FALSE(pump.accepting());
    REQUIRE(pump.TickOnce(5000));  // 摘 wake 前的最后收尾:恢复扫描 + 投递照走,新执行不取
    CHECK(CountOf(fixture.counter_file, "model") == 0);  // 没执行任何任务
    CHECK(pump.Close(1000));  // 收执行器与 writer
    CHECK(pump.Close(1000));  // 幂等
}
