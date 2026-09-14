// 常驻总装 V2 的周期调度与恢复册(单子 §十 V2 + §八表)。
//
// 放行门口径(单内原文):停机跨多个周期后按政策补一拍;时钟倒拨不重跑
// 原 slot;未知副作用停住;三次 resume 不重复执行已完成工作。
//
// "进程退出重启"模拟法与 V1 册同款(如实分账):盘上账是唯一真源,内存
// 装配销毁重建 = 语义等价的进程重启;模型/工具调用计数落盘上计数文件。
// 恢复裁决的账态注入(claim 后无绑定/有绑定无场)走 store 公开面——账
// 态注入册,CI 可重复。
//
// V3 会话格式的执行用例显式 pin EnvGuard("1")(纪律第 5 条)。
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "gateway/automation_store.hpp"
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "runtime/automation_pump.hpp"
#include "runtime/headless_executor.hpp"
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "workspace/identity.hpp"

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

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// V2 装配:与 V1 册同形。
struct V2Fixture {
    std::filesystem::path root;
    gateway::GatewayProfilePaths paths;
    std::filesystem::path workspaces_root;
    std::filesystem::path counter_file;
    std::vector<std::vector<api::StreamEvent>> scripts;

    explicit V2Fixture(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-gw-v2-" + std::string(tag));
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
        runtime::GatewayAutomationPump* pump, api::Backend& backend,
        tools::ToolRegistry& registry) {
        runtime::GatewayAutomationPump::Options options;
        options.paths = paths;
        options.workspaces_root = workspaces_root;
        options.cwd_utf8 = tools::PathToUtf8(root);
        options.lubancode_version = "0.26.238-test";
        options.wire_name = "test-wire";
        options.model = "test-model";
        options.workspace_identity = workspace::MakeFallbackIdentity(root);
        auto open = runtime::GatewayAutomationPump::Open(pump, backend, registry,
                                                         std::move(options));
        if (open.ok) {
            pump->set_owner_epoch("gw-v2-epoch");
        }
        return open;
    }
};

tools::ToolRegistry MakeRegistry() {
    tools::ToolRegistry registry;  // 空注册表:本文本脚本不请求工具
    return registry;
}

gateway::GatewayJobAddCommand IntervalAdd(std::int64_t interval_seconds, const std::string& prompt,
                                          bool heartbeat = false,
                                          std::int64_t deadline_ms = 0) {
    gateway::GatewayJobAddCommand add;
    add.prompt = prompt;
    add.idempotency_key = "k-" + prompt;
    add.schedule.set_interval = true;
    add.schedule.interval_seconds = interval_seconds;
    if (heartbeat) {
        add.schedule.set_notify_on_change = true;
        add.schedule.notify_on_change = true;
    }
    if (deadline_ms > 0) {
        add.schedule.set_deadline = true;
        add.schedule.deadline_ms = deadline_ms;
    }
    return add;
}

}  // namespace

// ---------------------------------------------------------------------------
// 放行门 1:停机跨多个周期后按政策(coalesce)补一拍
// ---------------------------------------------------------------------------

TEST_CASE("停机跨多周期:重启后合并补一拍,只执行一次") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V2Fixture fixture("downtime");
    fixture.scripts = {TextScript("第 61000ms 拍的答复。")};
    {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry();
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(gateway::WriteJobAddCommand(fixture.paths.control_dir,
                                            IntervalAdd(60, "巡检"))
                    .empty());
        REQUIRE(pump.TickOnce(1000));  // 消费命令建 job(锚点 1000,无拍可跑)
        CHECK(CountOf(fixture.counter_file, "model") == 0);
    }
    // "停机":装配销毁,时间前进五个周期(61k..301k)。
    {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry();
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(pump.TickOnce(301000));  // 补一拍 + 执行 + 投递
    }
    CHECK(CountOf(fixture.counter_file, "model") == 1);  // 只执行一次
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    REQUIRE(projection.occurrences.size() == 1);
    const auto& occurrence = projection.occurrences.begin()->second;
    CHECK(occurrence.slot_ms == 61000);      // 落最老一拍(计划内 slot)
    CHECK(occurrence.missed_count == 4);     // 覆盖范围在账
    CHECK(occurrence.outcome == "succeeded");
    const auto outbox = gateway::ReadOutboxProjection(fixture.paths.outbox_log);
    REQUIRE(outbox.items.size() == 1);       // 一拍一份投递,不多送
}

// ---------------------------------------------------------------------------
// 放行门 2:时钟倒拨不重跑原 slot
// ---------------------------------------------------------------------------

TEST_CASE("时钟倒拨:游标只前进,倒拨后不重跑、不出新拍") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V2Fixture fixture("rollback");
    fixture.scripts = {TextScript("第一拍答复。")};
    {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry();
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(gateway::WriteJobAddCommand(fixture.paths.control_dir,
                                            IntervalAdd(60, "巡检"))
                    .empty());
        REQUIRE(pump.TickOnce(1000));   // 建 job
        REQUIRE(pump.TickOnce(61000));  // 第一拍执行完(游标 61000)
    }
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    {
        // 时钟倒拨到 10000:不出新拍,不重跑,游标不动。
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry();
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(pump.TickOnce(10000));
        REQUIRE(pump.TickOnce(20000));
    }
    CHECK(CountOf(fixture.counter_file, "model") == 1);
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    REQUIRE(projection.occurrences.size() == 1);
    CHECK(projection.occurrences.begin()->second.outcome == "succeeded");
    CHECK(projection.jobs.begin()->second.schedule_cursor_ms == 61000);
}

// ---------------------------------------------------------------------------
// 放行门 3:未知副作用停住(§八:开轮后账对不上 → needs_review,不重跑)
// ---------------------------------------------------------------------------

TEST_CASE("未知副作用停住:绑定在场而 V3 场对不上 → needs_review,不碰模型") {
    V2Fixture fixture("unknown");
    auto backend = fixture.MakeBackend();
    tools::ToolRegistry registry = MakeRegistry();
    runtime::GatewayAutomationPump pump;
    REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
    REQUIRE(pump.store()->CreateOnceJob("j", "问", 1000, 1000, "").accepted);
    // 账态注入:claim + 绑定一个不存在的场(开轮后崩,场账断裂)。
    const std::string occurrence_id = gateway::MakeOccurrenceId("j", 1, 1000);
    REQUIRE(pump.store()->ClaimDue("gw-v2-epoch", 2000).has_value());
    REQUIRE(pump.store()->BindOccurrence(occurrence_id, "sess-ghost", "turn-ghost", 2000));

    REQUIRE(pump.TickOnce(3000));  // 恢复扫描:对账不了 → needs_review
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    const auto occurrence = projection.occurrences.find(occurrence_id);
    REQUIRE(occurrence != projection.occurrences.end());
    CHECK(occurrence->second.state == gateway::AutomationOccurrence::State::Settled);
    CHECK(occurrence->second.outcome == "needs_review");
    CHECK(CountOf(fixture.counter_file, "model") == 0);  // 不重跑
}

// ---------------------------------------------------------------------------
// §八:claim 后无开轮事实 → 重派同一 work(attempt+1);attempt 帽到顶停审
// ---------------------------------------------------------------------------

TEST_CASE("claim 后崩(无绑定):重启重派同一 occurrence,attempt+1,不双跑") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V2Fixture fixture("redispatch");
    fixture.scripts = {TextScript("重派后的答复。")};
    const std::string occurrence_id = gateway::MakeOccurrenceId("j", 1, 1000);
    {
        // 账态注入:claim 后"进程死"(未开轮——绑定行不在)。
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry();
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(pump.store()->CreateOnceJob("j", "问", 1000, 1000, "").accepted);
        REQUIRE(pump.store()->ClaimDue("gw-v2-epoch", 2000).has_value());
    }
    {
        // 重启:恢复裁决 → 重派(同 occurrenceId,attempt 2)→ 本 tick 执行。
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry();
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(pump.TickOnce(9000));
    }
    CHECK(CountOf(fixture.counter_file, "model") == 1);  // 只跑了一次
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    REQUIRE(projection.occurrences.size() == 1);  // 同一枚,没造第二枚
    const auto occurrence = projection.occurrences.find(occurrence_id);
    REQUIRE(occurrence != projection.occurrences.end());
    CHECK(occurrence->second.attempt == 2);
    CHECK(occurrence->second.outcome == "succeeded");
    CHECK(occurrence->second.occurrence_id == occurrence_id);  // 身份不洗
}

TEST_CASE("attempt 帽到顶:重派机会用尽 → needs_review,不再派") {
    V2Fixture fixture("exhausted");
    auto backend = fixture.MakeBackend();
    tools::ToolRegistry registry = MakeRegistry();
    runtime::GatewayAutomationPump pump;
    REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
    REQUIRE(pump.store()->CreateOnceJob("j", "问", 1000, 1000, "").accepted);
    const std::string occurrence_id = gateway::MakeOccurrenceId("j", 1, 1000);
    // 两轮"claim 后死"用掉两次重派(attempt 1→2→3);第三次 claim 后帽到顶。
    for (int round = 0; round < 2; ++round) {
        REQUIRE(pump.store()->ClaimDue("gw-v2-epoch", 2000 + round).has_value());
        REQUIRE(pump.store()->RedispatchOccurrence(occurrence_id, "claimed_without_binding",
                                                   3000 + round));
    }
    REQUIRE(pump.store()->ClaimDue("gw-v2-epoch", 9000).has_value());  // attempt 3
    CHECK_FALSE(pump.store()->RedispatchOccurrence(occurrence_id, "claimed_without_binding",
                                                   9100));  // 3+1 > 帽
    REQUIRE(pump.TickOnce(9500));  // 恢复:重派帽到顶 → needs_review
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    const auto occurrence = projection.occurrences.find(occurrence_id);
    REQUIRE(occurrence != projection.occurrences.end());
    CHECK(occurrence->second.outcome == "needs_review");
    CHECK(occurrence->second.detail.find("redispatch_exhausted") != std::string::npos);
    CHECK(CountOf(fixture.counter_file, "model") == 0);
}

// ---------------------------------------------------------------------------
// 放行门 4:三次 resume 不重复执行已完成工作
// ---------------------------------------------------------------------------

TEST_CASE("三次 resume:已完成工作不重开,模型与投递身份不变") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V2Fixture fixture("resume3");
    fixture.scripts = {TextScript("一次就够。")};
    {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry();
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(pump.store()->CreateOnceJob("j", "问", 1000, 1000, "").accepted);
        REQUIRE(pump.TickOnce(2000));  // 执行 + 投递 + 结算
    }
    std::string occurrence_id;
    std::string selection_id;
    {
        const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
        REQUIRE(projection.occurrences.size() == 1);
        occurrence_id = projection.occurrences.begin()->first;
        selection_id = "sel-" + projection.occurrences.begin()->second.session_id + "-" +
                       projection.occurrences.begin()->second.turn_id;  // V2 定式
    }
    // 三次"重启"(resume):每次都只读账 + 恢复扫描,不该有新执行。
    for (int round = 0; round < 3; ++round) {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry();
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(pump.TickOnce(3000 + round));
    }
    CHECK(CountOf(fixture.counter_file, "model") == 1);
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    REQUIRE(projection.occurrences.size() == 1);
    CHECK(projection.occurrences.find(occurrence_id)->second.outcome == "succeeded");
    // 投递身份稳定:同 deliveryId 只有一份,已发布。
    const auto outbox = gateway::ReadOutboxProjection(fixture.paths.outbox_log);
    REQUIRE(outbox.items.size() == 1);
    CHECK(outbox.items.begin()->second.state == "delivered");
    CHECK(outbox.items.begin()->first ==
          gateway::MakeDeliveryId(selection_id, "local:file", 1));
}

// ---------------------------------------------------------------------------
// heartbeat:观察结果与通知去重(§七)
// ---------------------------------------------------------------------------

TEST_CASE("heartbeat:正文未变不投递,变化才投;观察账在") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V2Fixture fixture("heartbeat");
    fixture.scripts = {TextScript("一切正常。"), TextScript("一切正常。"),
                       TextScript("发现新提交。")};
    {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry();
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(gateway::WriteJobAddCommand(
                    fixture.paths.control_dir, IntervalAdd(60, "巡检", /*heartbeat=*/true))
                    .empty());
        REQUIRE(pump.TickOnce(1000));    // 建 job(heartbeat)
        REQUIRE(pump.TickOnce(61000));   // 拍 1:有变化(首见)→ 投递
        REQUIRE(pump.TickOnce(121000));  // 拍 2:正文同 → 静默
        REQUIRE(pump.TickOnce(181000));  // 拍 3:变化 → 投递
    }
    CHECK(CountOf(fixture.counter_file, "model") == 3);  // 检查照跑
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    REQUIRE(projection.occurrences.size() == 3);
    std::vector<gateway::AutomationOccurrence> by_slot;
    for (const auto& [id, occ] : projection.occurrences) {
        by_slot.push_back(occ);
    }
    std::sort(by_slot.begin(), by_slot.end(),
              [](const gateway::AutomationOccurrence& a, const gateway::AutomationOccurrence& b) {
                  return a.slot_ms < b.slot_ms;
              });
    REQUIRE(by_slot.size() == 3);
    // 诊断落账:哪一拍什么 outcome/detail(失败时 CI 日志直接见因)。
    for (const auto& occ : by_slot) {
        MESSAGE("occ slot=", occ.slot_ms, " attempt=", occ.attempt,
                " outcome=", occ.outcome, " detail=", occ.detail,
                " changed=", occ.observed_changed, " delivered=", occ.observed_delivered,
                " session=", occ.session_id.empty() ? "-" : occ.session_id);
    }
    CHECK(by_slot[0].outcome == "succeeded");
    CHECK(by_slot[1].outcome == "succeeded");
    CHECK(by_slot[1].detail == "unchanged_notification_suppressed");
    CHECK(by_slot[2].outcome == "succeeded");
    // 投递去重:只有拍 1 与拍 3 出箱(拍 2 静默)。
    const auto outbox = gateway::ReadOutboxProjection(fixture.paths.outbox_log);
    for (const auto& [id, item] : outbox.items) {
        MESSAGE("outbox ", id, " sel=", item.selection_id, " state=", item.state);
    }
    CHECK(outbox.items.size() == 2);
    CHECK(outbox.items.begin()->second.state == "delivered");
}

TEST_CASE("heartbeat:检查失败必投失败通知,不记无变化") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V2Fixture fixture("hb-fail");
    fixture.scripts = {TextScript("一切正常。")};  // 第二拍脚本耗尽 → 失败
    std::string failed_occurrence_id;
    {
        auto backend = fixture.MakeBackend();
        tools::ToolRegistry registry = MakeRegistry();
        runtime::GatewayAutomationPump pump;
        REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
        REQUIRE(gateway::WriteJobAddCommand(
                    fixture.paths.control_dir, IntervalAdd(60, "巡检", /*heartbeat=*/true))
                    .empty());
        REQUIRE(pump.TickOnce(1000));
        REQUIRE(pump.TickOnce(61000));   // 拍 1 成功投递
        REQUIRE(pump.TickOnce(121000));  // 拍 2 失败 → 失败通知必投
        const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
        for (const auto& [id, occ] : projection.occurrences) {
            if (occ.slot_ms == 121000) failed_occurrence_id = id;
        }
        REQUIRE_FALSE(failed_occurrence_id.empty());
    }
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    const auto failed = projection.occurrences.find(failed_occurrence_id);
    REQUIRE(failed != projection.occurrences.end());
    CHECK(failed->second.outcome == "failed");
    CHECK(failed->second.observed_changed);      // 失败不是"无变化"
    CHECK(failed->second.observed_delivered);
    // 失败通知进了 outbox(notice- 前缀,与 reply selection 分账)。
    const auto outbox = gateway::ReadOutboxProjection(fixture.paths.outbox_log);
    REQUIRE(outbox.items.size() == 2);
    bool saw_notice = false;
    for (const auto& [id, item] : outbox.items) {
        if (item.selection_id == "notice-" + failed_occurrence_id) saw_notice = true;
    }
    CHECK(saw_notice);
    // 已通知版本没被失败文本污染:job 的 last_observed 仍是拍 1 的正文
    //(严格等值归 store 册 RecordObservation 直测;这里验非空 + 未变语义)。
    const auto job = projection.jobs.find("job-1");
    REQUIRE(job != projection.jobs.end());
    CHECK_FALSE(job->second.last_observed_sha.empty());
}

// ---------------------------------------------------------------------------
// 领域操作与导入:经控制命令文件(CLI 同一条路)
// ---------------------------------------------------------------------------

TEST_CASE("领域操作经命令文件:update/pause/cancel 的 CAS 与终态") {
    V2Fixture fixture("ops");
    auto backend = fixture.MakeBackend();
    tools::ToolRegistry registry = MakeRegistry();
    runtime::GatewayAutomationPump pump;
    REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
    REQUIRE(gateway::WriteJobAddCommand(fixture.paths.control_dir, IntervalAdd(60, "巡检"))
                .empty());
    REQUIRE(pump.TickOnce(1000));  // 建 job(rev 1)

    // 错 revision 的 update:拒。
    gateway::GatewayJobUpdateCommand wrong;
    wrong.job_id = "job-1";
    wrong.expected_revision = 99;
    wrong.prompt = "改不动";
    REQUIRE(gateway::WriteJobUpdateCommand(fixture.paths.control_dir, wrong).empty());
    REQUIRE(pump.TickOnce(2000));
    CHECK(pump.store()->FindJob("job-1")->prompt == "巡检");

    // 对的 revision:改周期(--every 120),重锚。
    gateway::GatewayJobUpdateCommand right;
    right.job_id = "job-1";
    right.expected_revision = 1;
    right.schedule.set_interval = true;
    right.schedule.interval_seconds = 120;
    REQUIRE(gateway::WriteJobUpdateCommand(fixture.paths.control_dir, right).empty());
    REQUIRE(pump.TickOnce(400000));
    {
        const auto job = pump.store()->FindJob("job-1");
        REQUIRE(job.has_value());
        CHECK(job->revision == 2);
        CHECK(job->interval_seconds == 120);
        CHECK(job->schedule_cursor_ms == 400000);
    }

    // pause → cancel(注意 pause 后 revision 不变,仍是 2)。
    gateway::GatewayJobStateCommand pause;
    pause.verb = "pause";
    pause.job_id = "job-1";
    pause.expected_revision = 2;
    REQUIRE(gateway::WriteJobStateCommand(fixture.paths.control_dir, pause).empty());
    REQUIRE(pump.TickOnce(410000));
    CHECK(pump.store()->FindJob("job-1")->state == gateway::AutomationJobState::Paused);

    gateway::GatewayJobStateCommand cancel;
    cancel.verb = "cancel";
    cancel.job_id = "job-1";
    cancel.expected_revision = 2;
    REQUIRE(gateway::WriteJobStateCommand(fixture.paths.control_dir, cancel).empty());
    REQUIRE(pump.TickOnce(420000));
    CHECK(pump.store()->FindJob("job-1")->state == gateway::AutomationJobState::Cancelled);
    // 全程零执行:61k..361k 的拍在 update(400k 重锚)前从未到过扫描时
    // 刻,pause/cancel 后也不再生成。
    CHECK(CountOf(fixture.counter_file, "model") == 0);
}

TEST_CASE("import-loop 经命令文件:产 receipt;同来源重导不双跑") {
    V2Fixture fixture("importloop");
    auto backend = fixture.MakeBackend();
    tools::ToolRegistry registry = MakeRegistry();
    runtime::GatewayAutomationPump pump;
    REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
    gateway::GatewayJobImportLoopCommand command;
    command.source_session_id = "sess-42";
    command.source_task_id = "loop-1";
    command.prompt = "每十分钟巡检仓库";
    command.interval_seconds = 600;
    command.idempotency_key = "imp-1";
    REQUIRE(gateway::WriteJobImportLoopCommand(fixture.paths.control_dir, command).empty());
    REQUIRE(pump.TickOnce(1000));
    {
        const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
        REQUIRE(projection.jobs.size() == 1);
        CHECK(projection.jobs.begin()->second.schedule_kind == gateway::ScheduleKind::Interval);
        CHECK(projection.jobs.begin()->second.imported_from == "loop:sess-42:loop-1");
        REQUIRE(projection.loop_imports.size() == 1);
        CHECK(projection.loop_imports.begin()->second.receipt_id.rfind("imp-", 0) == 0);
    }
    // 同来源再导(命令文件再来一枚):receipt 幂等,不建第二个 job。
    command.idempotency_key.clear();
    REQUIRE(gateway::WriteJobImportLoopCommand(fixture.paths.control_dir, command).empty());
    REQUIRE(pump.TickOnce(2000));
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    CHECK(projection.jobs.size() == 1);
    CHECK(projection.loop_imports.size() == 1);
    CHECK(CountOf(fixture.counter_file, "model") == 0);  // 导入本身零执行
}

TEST_CASE("deadline 已过:不生成不执行(到线先取消不判失败;结算面在 store 册)") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    V2Fixture fixture("deadline");
    fixture.scripts = {TextScript("不该跑到这里。")};
    auto backend = fixture.MakeBackend();
    tools::ToolRegistry registry = MakeRegistry();
    runtime::GatewayAutomationPump pump;
    REQUIRE(fixture.OpenPump(&pump, *backend, registry).ok);
    // deadline 已在过去:建 job 后到点也不生成、不执行。
    REQUIRE(gateway::WriteJobAddCommand(fixture.paths.control_dir,
                                        IntervalAdd(60, "巡检", false, /*deadline=*/2000))
                .empty());
    REQUIRE(pump.TickOnce(1000));   // 建 job(无拍可跑)
    REQUIRE(pump.TickOnce(61000));  // 到拍但已过线:生成停
    REQUIRE(pump.TickOnce(121000));
    CHECK(CountOf(fixture.counter_file, "model") == 0);  // 不执行
    const auto projection = gateway::ReadAutomationProjection(fixture.paths.automation_log);
    CHECK(projection.occurrences.empty());  // 过线连 occurrence 都不出
}
