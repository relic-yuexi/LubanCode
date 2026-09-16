// QQ 接入单 Q5 总装册:假渠道 + 假模型 + 真 automation 泵 + 真渠道泵
// ——"从 QQ 建单次提醒和周期任务"的纵向链(todo §十一)。
//
// 验收对齐 Q5 交付五项:
//   1) QQ 消息"每天九点提醒我 X" → 模型调 create_reminder → 任务建账
//      (归属/交付目标/幂等键 = 渠道域+账号+消息 id);
//   2) 假时钟到点 → V2 泵派发(拍点生成)→ 渠道泵认领 → 隔离任务场执行
//      → reply selection → outbox 投回原会话(不投别的会话,不带陈旧锚);
//   3) 越权:他人任务不可见不可取消(list/cancel 归属闸);
//   4) 同信重发/同轮重调不双建(幂等);
//   5) 窗口过期/主动额度受限:不硬发不谎报,挂起待下次互动补投(锚定
//      新来信,窗内才算被动回复);
//   6) claim 后崩:重建后重派执行(attempt+1),模型不多跑。
// 真平台主动发送权限/真实回复窗口归 Q3 真机验收,本地 mock 不能代替。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "channel/manager.hpp"
#include "channel/types.hpp"
#include "fake_channel_sidecar.hpp"
#include "gateway/automation_schedule.hpp"
#include "gateway/automation_store.hpp"
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "runtime/automation_pump.hpp"
#include "runtime/channel_automation.hpp"
#include "runtime/channel_work_pump.hpp"
#include "runtime/headless_executor.hpp"
#include "tools/path_utils.hpp"
#include "tools/tool.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;
using lubancode::test_support::FakeChannelSidecar;

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

// 盘上调用计数:进程"重启"(装配销毁重建)后仍在——不多调用的判据。
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

class CaptureBackend : public api::Backend {
public:
    CaptureBackend(std::filesystem::path counter, std::vector<std::vector<api::StreamEvent>> scripts)
        : counter_(std::move(counter)), scripts_(std::move(scripts)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
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

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& name,
                                            const std::string& input_json) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::ToolUseStart{0, tool_id, name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

class FakeTransport final : public channel::ChannelBridgeTransport {
public:
    explicit FakeTransport(FakeChannelSidecar& sidecar) : sidecar_(sidecar) {}
    void WriteToSidecar(const std::byte* data, std::size_t size) override {
        sidecar_.FeedFromHost(data, size);
    }
    std::vector<std::byte> DrainFromSidecar() override { return sidecar_.DrainToHost(); }

private:
    FakeChannelSidecar& sidecar_;
};

channel::ChannelInboundEvent MakeDmAt(const std::string& delivery_id,
                                      const std::string& provider_event_id,
                                      const std::string& conversation, const std::string& text,
                                      const std::string& message_id, std::int64_t received_at_ms) {
    channel::ChannelInboundEvent event;
    event.delivery_id = delivery_id;
    event.provider_event_id = provider_event_id;
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.received_at_ms = received_at_ms;
    event.provider_at_ms = received_at_ms;
    event.conversation.kind = channel::ConversationKind::Direct;
    event.conversation.id = conversation;
    event.sender.id = "sender-" + conversation;
    event.sender.display_name = "用户";
    event.message_id = message_id;
    channel::ChannelPart part;
    part.type = channel::ChannelPartType::Text;
    part.text = text;
    event.parts.push_back(part);
    return event;
}

// 纵向装配:manager(真 Bridge 帧)+ automation 泵(真,拍点生成/本地任务)
// + 渠道泵(真,渠道任务认领/执行/补投)+ 任务桥(真,三枚工具注册)。
// 重建即"进程重启"(盘上账:ingress/映射/work/outbox/automation 全保留)。
struct Q5Fixture {
    std::filesystem::path root;
    gateway::GatewayProfilePaths paths;
    std::filesystem::path workspaces_root;
    std::filesystem::path channels_root;
    std::filesystem::path ws_root;
    std::filesystem::path counter_file;
    std::int64_t now = 1724700000000;
    std::vector<std::vector<api::StreamEvent>> scripts;

    FakeChannelSidecar sidecar;
    FakeTransport transport{sidecar};
    std::unique_ptr<CaptureBackend> backend;
    std::unique_ptr<channel::ChannelManager> manager;
    std::optional<runtime::GatewayAutomationPump> automation;
    std::shared_ptr<runtime::ChannelAutomationBridge> bridge;
    std::optional<runtime::ChannelWorkPump> pump;
    workspace::WorkspaceIdentity identity;

    Q5Fixture(const char* tag, bool rebuild = false)
        : root(std::filesystem::temp_directory_path() /
               ("lubancode-q5-job-" + std::string(tag))) {
        if (!rebuild) {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(root, ec);
        }
        paths = gateway::ResolveGatewayProfilePaths(root / "gateway", "default");
        workspaces_root = root / "workspaces";
        channels_root = root / "channels";
        ws_root = root / "ws";
        counter_file = root / "calls.log";
        std::error_code ec;
        std::filesystem::create_directories(ws_root, ec);

        channel::ChannelManagerOptions manager_options;
        manager_options.state_root = channels_root;
        manager_options.now_ms = [this] { return now; };
        manager_options.alive_checker = [](unsigned long) { return true; };
        manager = std::make_unique<channel::ChannelManager>(std::move(manager_options));

        channel::ChannelAccountUserConfig config;
        config.enabled = true;
        config.transport = "websocket";
        config.secret_env = "QQBOT_SECRET";
        config.dm_policy = channel::DmPolicy::Open;
        config.tools.allow = {"create_reminder", "list_reminders", "cancel_reminder"};
        REQUIRE(manager->AddAccount("qqbot", "main", config, &transport).status ==
                channel::ChannelManager::AddAccountResult::Status::Ok);
        REQUIRE_FALSE(manager->StartAccount("qqbot", "main").has_value());
        manager->Pump("qqbot", "main");
        manager->Pump("qqbot", "main");
        REQUIRE(manager->Snapshot("qqbot", "main")->state ==
                channel::ChannelAccountState::Running);
        identity = workspace::MakeFallbackIdentity(ws_root);
    }

    // 开双泵(automation 先:渠道泵借用它的 store/outbox——生产同款单写者)
    // + 任务桥三件套。失败 REQUIRE 带错误信息。
    bool OpenPumps(tools::ToolRegistry& registry) {
        backend = std::make_unique<CaptureBackend>(counter_file, scripts);
        runtime::GatewayAutomationPump::Options pump_options;
        pump_options.paths = paths;
        pump_options.workspaces_root = workspaces_root;
        pump_options.workspace_identity = identity;
        pump_options.cwd_utf8 = tools::PathToUtf8(root);
        pump_options.lubancode_version = "0.26.267-test";
        pump_options.wire_name = "test-wire";
        pump_options.model = "test-model";
        pump_options.max_steps_per_turn = 8;
        automation.emplace();
        const auto open = runtime::GatewayAutomationPump::Open(&*automation, *backend, registry,
                                                               std::move(pump_options));
        REQUIRE_MESSAGE(open.ok, open.error);
        if (!open.ok) {
            return false;
        }
        automation->set_owner_epoch("q5-test-epoch");
        // 任务桥 + 三枚工具(注册名与 QQ 模板 allow 对账)。
        bridge = std::make_shared<runtime::ChannelAutomationBridge>(automation->store(),
                                                                    [this] { return now; });
        runtime::RegisterChannelAutomationTools(registry, bridge);
        // 渠道泵。
        runtime::ChannelWorkPump::Options work_options;
        work_options.manager = manager.get();
        work_options.outbox = automation->outbox();
        work_options.channels_state_root = channels_root;
        work_options.workspaces_root = workspaces_root;
        work_options.workspace_identity = identity;
        work_options.cwd_utf8 = tools::PathToUtf8(root);
        work_options.lubancode_version = "0.26.267-test";
        work_options.wire_name = "test-wire";
        work_options.model = "test-model";
        work_options.tools.allow = {"create_reminder", "list_reminders", "cancel_reminder"};
        work_options.max_steps_per_turn = 8;
        work_options.automation_store = automation->store();
        work_options.automation_bridge = bridge;
        pump.emplace();
        const auto work_open =
            runtime::ChannelWorkPump::Open(&*pump, *backend, registry, std::move(work_options));
        REQUIRE_MESSAGE(work_open.ok, work_open.error);
        if (!work_open.ok) {
            return false;
        }
        pump->set_owner_epoch("q5-test-epoch");
        return true;
    }

    // 一 tick:automation 泵先(拍点生成/本地任务),渠道泵后——与生产
    // CompositeGatewayPump 同序。时间走一步。
    bool Tick(std::int64_t step_ms = 100) {
        const bool automation_ok = automation->TickOnce(now);
        const bool channel_ok = pump->TickOnce(now);
        now += step_ms;
        return automation_ok && channel_ok;
    }

    void EmitAndIngest(const channel::ChannelInboundEvent& event) {
        sidecar.EmitInboundEvent(event);
        manager->Pump("qqbot", "main");
    }

    void TickUntilQuiet(int max_ticks = 40) {
        for (int i = 0; i < max_ticks; ++i) {
            Tick();
        }
    }

    gateway::AutomationStore* store() { return automation->store(); }

    // 渠道任务段的 outbox 项(按 source_ref 前缀挑)。
    std::optional<gateway::ReplyOutboxItem> ChanjobItem() {
        for (const auto& item : automation->outbox()->ListItems()) {
            if (item.source_ref.rfind("chanjob:", 0) == 0) {
                return item;
            }
        }
        return std::nullopt;
    }

    std::string IngressStateNameOf(std::int64_t sid) {
        const auto records = manager->IngressRecords("qqbot", "main");
        for (const auto& record : records) {
            if (record.sid == sid) return channel::IngressEventStateName(record.state);
        }
        return "missing";
    }
};

std::string CreateOnceInput(std::int64_t at_ms) {
    return std::string("{\"description\":\"提醒喝水\",\"at_ms\":") + std::to_string(at_ms) + "}";
}

const char* kCreateCronInput =
    "{\"description\":\"每天九点提醒我喝水\",\"cron\":\"0 9 * * *\","
    "\"timezone\":\"Asia/Shanghai\"}";

}  // namespace

// ---------------------------------------------------------------------------
// 验收 1+4:聊天建周期任务 → 到点 → 渠道泵认领执行 → 投回原会话
// ---------------------------------------------------------------------------

TEST_CASE("QQ 建周期提醒全链:建账归属/隔离场/到点执行/结果投回原会话不带陈旧锚") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q5Fixture fixture("cron-full");
    fixture.scripts = {ToolUseScript("tu-1", "create_reminder", kCreateCronInput),
                       TextScript("已设置:每天早上九点(Asia/Shanghai)提醒你喝水。"),
                       TextScript("喝水时间到了,记得喝一杯。")};
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));

    // 1) 来信 → 模型调工具 → 建账(归属/交付/计划全落)。
    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "每天九点提醒我喝水", "m-1", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 2);  // 工具轮 + 终答
    const auto jobs = fixture.store()->ListJobs();
    REQUIRE(jobs.size() == 1);
    const auto& job = jobs[0];
    CHECK(job.ChannelBacked());
    CHECK(job.owner_channel == "qqbot");
    CHECK(job.owner_account == "main");
    CHECK(job.owner_sender == "sender-dm-a");
    CHECK(job.delivery_conversation == "dm-a");
    CHECK(job.schedule_kind == gateway::ScheduleKind::Cron);
    CHECK(job.cron_expr == "0 9 * * *");
    CHECK(job.timezone == "Asia/Shanghai");
    CHECK(job.prompt.find("每天九点提醒我喝水") != std::string::npos);
    // 聊天回复(被动,锚定 m-1)已投。
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);
    CHECK(fixture.sidecar.sent_messages()[0].params["reply_to_message_id"] == "m-1");
    CHECK(fixture.IngressStateNameOf(1) == "delivered");

    // 2) 假时钟到点:automation 泵生成拍点 → 渠道泵认领(本地泵不抢)→
    //    隔离任务场执行 → reply selection → outbox。
    gateway::ScheduleSpec spec;
    spec.kind = gateway::ScheduleKind::Cron;
    spec.cron_expr = "0 9 * * *";
    spec.timezone = "Asia/Shanghai";
    const auto fire = gateway::FirstSlotAfter(spec, fixture.now);
    REQUIRE(fire.found);
    fixture.now = fire.utc_ms + 1;
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 3);  // + 任务轮一次
    // 任务结果投回原会话 dm-a(不投别的会话);距来信已久 → 主动消息,
    // 不带陈旧 msg_id 锚(窗口过期不硬发,§11.3)。
    REQUIRE(fixture.sidecar.sent_messages().size() == 2);
    const auto& delivered = fixture.sidecar.sent_messages()[1];
    CHECK(delivered.params["conversation"]["id"] == "dm-a");
    CHECK_FALSE(delivered.params.contains("reply_to_message_id"));
    CHECK(delivered.params["parts"][0]["text"] == "喝水时间到了,记得喝一杯。");
    // 隔离:任务自己的场,不进聊天会话(§11.2 不自动带聊天史)。
    const std::string chat_session = fixture.pump->session_id_for("qqbot", "main", "dm-a");
    const std::string job_session = fixture.pump->job_session_id_for("qqbot", "main", job.job_id);
    REQUIRE_FALSE(chat_session.empty());
    REQUIRE_FALSE(job_session.empty());
    CHECK(chat_session != job_session);
    // occurrence 结算:执行成功且已交投递账(投递态在 outbox,不谎报)。
    const auto occurrences = fixture.store()->ListJobOccurrences(job.job_id);
    REQUIRE(occurrences.size() == 1);
    CHECK(occurrences[0].state == gateway::AutomationOccurrence::State::Settled);
    CHECK(occurrences[0].outcome == "succeeded");
    const auto item = fixture.ChanjobItem();
    REQUIRE(item.has_value());
    CHECK(item->state == "sent");
    CHECK(item->target_conversation_id == "dm-a");
}

TEST_CASE("同信重发/同轮重调不双建:幂等键 = 渠道域+账号+消息 id") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q5Fixture fixture("idem");
    // due 固定在建单时刻(scripts 与桥面重调用同一值——幂等比较看载荷)。
    const std::int64_t due_ms = fixture.now + 3600000;
    fixture.scripts = {ToolUseScript("tu-1", "create_reminder", CreateOnceInput(due_ms)),
                       // 同轮模型重调一次(同输入):幂等命中,不建第二笔。
                       ToolUseScript("tu-2", "create_reminder", CreateOnceInput(due_ms)),
                       TextScript("已设置一小时的提醒。")};
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));

    const auto event =
        MakeDmAt("in-1", "pe-1", "dm-a", "一小时后提醒我喝水", "m-1", fixture.now);
    fixture.EmitAndIngest(event);
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 3);  // 两次工具轮 + 终答
    REQUIRE(fixture.store()->ListJobs().size() == 1);      // 只有一笔

    // 同信重发(sidecar 未收 ack 的重投,同 delivery id):ingress 去重,
    // 不再进模型,也不再建。
    fixture.EmitAndIngest(event);
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 3);
    REQUIRE(fixture.store()->ListJobs().size() == 1);

    // 桥面直证:同消息上下文再调一次创建 → duplicate 回原 jobId。
    runtime::ChannelAutomationBridge::TurnContext context;
    context.channel_id = "qqbot";
    context.account_id = "main";
    context.conversation_id = "dm-a";
    context.sender_id = "sender-dm-a";
    context.message_id = "m-1";
    context.received_at_ms = fixture.now;
    const runtime::ChannelAutomationBridge::TurnScope scope(*fixture.bridge, context);
    const auto again = fixture.bridge->CreateReminder(nlohmann::json::parse(CreateOnceInput(due_ms)));
    REQUIRE(again.ok);
    CHECK(again.duplicate);
    CHECK(again.payload["jobId"] == fixture.store()->ListJobs()[0].job_id);
}

// ---------------------------------------------------------------------------
// 验收 3:归属闸——他人任务不可见不可取消
// ---------------------------------------------------------------------------

TEST_CASE("越权:dm-b 查不到也取消不了 dm-a 的任务,任务保持 active") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q5Fixture fixture("ownership");
    fixture.scripts = {ToolUseScript("tu-1", "create_reminder",
                                     CreateOnceInput(fixture.now + 7200000)),
                       TextScript("已设置两小时后的提醒。"),
                       ToolUseScript("tu-2", "cancel_reminder", "{\"jobId\":\"job-1\"}"),
                       TextScript("这任务不是你的,取消不了。")};
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));

    // dm-a 建任务。
    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "两小时后提醒我喝水", "m-1", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(fixture.store()->ListJobs().size() == 1);
    REQUIRE(fixture.store()->ListJobs()[0].job_id == "job-1");

    // dm-b 越权取消:工具拒(归属闸),任务原样。
    fixture.EmitAndIngest(
        MakeDmAt("in-2", "pe-2", "dm-b", "取消我的任务 job-1", "m-2", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 4);  // 建 2 + 取消 2
    const auto job = fixture.store()->FindJob("job-1");
    REQUIRE(job.has_value());
    CHECK(job->state == gateway::AutomationJobState::Active);  // 越权没得逞

    // 桥面直证:dm-b 看不到 dm-a 的任务;dm-a 自己看得到、能取消。
    runtime::ChannelAutomationBridge::TurnContext ctx_b;
    ctx_b.channel_id = "qqbot";
    ctx_b.account_id = "main";
    ctx_b.conversation_id = "dm-b";
    ctx_b.sender_id = "sender-dm-b";
    ctx_b.message_id = "m-2";
    {
        const runtime::ChannelAutomationBridge::TurnScope scope(*fixture.bridge, ctx_b);
        const auto list_b = fixture.bridge->ListReminders();
        REQUIRE(list_b.ok);
        CHECK(list_b.payload["jobs"].empty());
        const auto cancel_b = fixture.bridge->CancelReminder(nlohmann::json{{"jobId", "job-1"}});
        CHECK_FALSE(cancel_b.ok);
        CHECK(cancel_b.error_code == "channel_job.not_your_job");
    }
    runtime::ChannelAutomationBridge::TurnContext ctx_a = ctx_b;
    ctx_a.conversation_id = "dm-a";
    ctx_a.sender_id = "sender-dm-a";
    ctx_a.message_id = "m-1";
    {
        const runtime::ChannelAutomationBridge::TurnScope scope(*fixture.bridge, ctx_a);
        const auto list_a = fixture.bridge->ListReminders();
        REQUIRE(list_a.ok);
        REQUIRE(list_a.payload["jobs"].size() == 1);
        CHECK(list_a.payload["jobs"][0]["jobId"] == "job-1");
        const auto cancel_a =
            fixture.bridge->CancelReminder(nlohmann::json{{"jobId", "job-1"}});
        REQUIRE(cancel_a.ok);
        CHECK(fixture.store()->FindJob("job-1")->state == gateway::AutomationJobState::Cancelled);
    }
}

// ---------------------------------------------------------------------------
// 验收 5:窗口过期/主动额度受限——挂起待互动补投
// ---------------------------------------------------------------------------

TEST_CASE("主动额度受限:不硬发不谎报,挂起;下一封来信进窗后锚定补投") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q5Fixture fixture("await-interaction");
    // 到点在来信 10 分钟后(超 4 分钟被动窗):任务结果是主动消息。
    fixture.scripts = {ToolUseScript("tu-1", "create_reminder",
                                     CreateOnceInput(fixture.now + 600000)),
                       TextScript("已设置十分钟后提醒。"),
                       TextScript("提醒:该喝水了。"),
                       TextScript("在的,你说。")};
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));

    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "十分钟后提醒我喝水", "m-1", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);  // 聊天回复照常(被动)

    // 到点前把发送脚本换成"主动额度受限一次,之后放行"——受限只砸在
    // 任务结果的主动发送上。
    fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::RateLimitedFirst);
    fixture.sidecar.set_rate_limited_first(1);

    fixture.now = fixture.now + 600000 + 1;  // 到点(来信已出窗)
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    // 主动发送被限:不硬发(只试了一次)、不谎报(账上 sending——发过
    // 一次、结局待补投;不记成功也不记终态失败)。
    auto item = fixture.ChanjobItem();
    REQUIRE(item.has_value());
    CHECK(item->state == "sending");  // 记过一次尝试,挂起(非终态、非成功)
    CHECK(fixture.sidecar.send_count_for(item->delivery_id) == 1);
    // 那一次发送是主动消息:不带陈旧 msg_id 锚。
    const auto chanjob_delivery = item->delivery_id;
    const auto& proactive_send = fixture.sidecar.sent_messages()[1];
    REQUIRE(proactive_send.client_id == chanjob_delivery);
    CHECK_FALSE(proactive_send.params.contains("reply_to_message_id"));
    // occurrence 已结算(执行事实与投递分账),投递态留账可查。
    const auto jobs_before = fixture.store()->ListJobs();
    REQUIRE(jobs_before.size() == 1);
    const auto occurrences = fixture.store()->ListJobOccurrences(jobs_before[0].job_id);
    REQUIRE(occurrences.size() == 1);
    CHECK(occurrences[0].outcome == "succeeded");
    CHECK(occurrences[0].detail == "enqueued_to_channel_outbox");

    // 用户来了新来信(进被动窗):挂起段解锁,锚定 m-2 补投。
    fixture.EmitAndIngest(
        MakeDmAt("in-2", "pe-2", "dm-a", "在吗", "m-2", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    item = fixture.ChanjobItem();
    REQUIRE(item.has_value());
    CHECK(item->state == "sent");
    CHECK(fixture.sidecar.send_count_for(chanjob_delivery) == 2);
    // 补投的第二次发送锚定新来信(窗内被动回复),载荷同 delivery。
    std::size_t anchored = 0;
    for (const auto& send : fixture.sidecar.sent_messages()) {
        if (send.client_id != chanjob_delivery) {
            continue;
        }
        ++anchored;
        if (anchored == 2) {
            REQUIRE(send.params.contains("reply_to_message_id"));
            CHECK(send.params["reply_to_message_id"] == "m-2");
        }
    }
    REQUIRE(anchored == 2);
}

// ---------------------------------------------------------------------------
// A06:改锚补投的身份账——旧尝试先裁决(账行留迹),新投递用新锚新号,
// 同 delivery 两代身份可追溯;被动窗内重试不偷换身份。
// ---------------------------------------------------------------------------

TEST_CASE("A06 改锚补投:旧身份裁决留迹,新身份新号(账行可追溯)") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q5Fixture fixture("reanchor-identity");
    fixture.scripts = {ToolUseScript("tu-1", "create_reminder",
                                     CreateOnceInput(fixture.now + 600000)),
                       TextScript("已设置十分钟后提醒。"),
                       TextScript("提醒:该喝水了。"),
                       TextScript("在的,你说。")};
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));

    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "十分钟后提醒我喝水", "m-1", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);

    fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::RateLimitedFirst);
    fixture.sidecar.set_rate_limited_first(1);
    fixture.now = fixture.now + 600000 + 1;  // 到点(来信出窗):首投走主动
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    const auto chanjob_delivery = fixture.ChanjobItem()->delivery_id;
    // 首投(主动消息):无锚无 seq——身份冻结为 {空锚, 0}。
    const auto& proactive = fixture.sidecar.sent_messages()[1];
    REQUIRE(proactive.client_id == chanjob_delivery);
    CHECK_FALSE(proactive.params.contains("msg_seq"));

    // 新来信进窗 → 旧身份被裁决(限频挂起的裁决行)→ 新身份按新锚发号。
    fixture.EmitAndIngest(
        MakeDmAt("in-2", "pe-2", "dm-a", "在吗", "m-2", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    std::size_t anchored = 0;
    for (const auto& send : fixture.sidecar.sent_messages()) {
        if (send.client_id != chanjob_delivery) {
            continue;
        }
        ++anchored;
        if (anchored == 2) {
            REQUIRE(send.params.contains("reply_to_message_id"));
            CHECK(send.params["reply_to_message_id"] == "m-2");
            // 新锚的第一号(独立计数,与主动首投无撞号)。
            REQUIRE(send.params.contains("msg_seq"));
            CHECK(send.params["msg_seq"] == 1);
        }
    }
    REQUIRE(anchored == 2);
    // 账行可追溯:同 delivery 留有两笔分配 + 一笔裁决(原因带限频挂起)。
    std::ifstream journal(fixture.paths.outbox_log);
    std::string line;
    int assigned_lines = 0;
    bool retired_seen = false;
    while (std::getline(journal, line)) {
        if (line.find("\"msgseq.assigned\"") != std::string::npos &&
            line.find(chanjob_delivery) != std::string::npos) {
            ++assigned_lines;
        }
        if (line.find("\"msgseq.retired\"") != std::string::npos &&
            line.find(chanjob_delivery) != std::string::npos &&
            line.find("active_quota_rejected_await_interaction") != std::string::npos) {
            retired_seen = true;
        }
    }
    CHECK(assigned_lines == 2);  // 主动一次 + 锚定补投一次
    CHECK(retired_seen);         // 改锚前旧尝试已裁决
}

TEST_CASE("回复窗口过期分型:挂起不转终态失败;普通聊天回复维持 Q2 终态规矩") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q5Fixture fixture("window-expired");
    fixture.scripts = {ToolUseScript("tu-1", "create_reminder",
                                     CreateOnceInput(fixture.now + 600000)),
                       TextScript("已设置十分钟后提醒。"),
                       TextScript("提醒:该喝水了。")};
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));
    // 永久拒绝,且 detail 带 expired → 分型 reply_window_expired。
    fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::PermanentReject);
    fixture.sidecar.set_reject_detail("msg_id window expired");

    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "十分钟后提醒我喝水", "m-1", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    fixture.now = fixture.now + 600000 + 1;
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    // 窗口过期:渠道任务段挂起(sending——试过一次,非终态),不转终态
    // 失败、不谎报成功。
    const auto item = fixture.ChanjobItem();
    REQUIRE(item.has_value());
    CHECK(item->state == "sending");
    CHECK(fixture.sidecar.send_count_for(item->delivery_id) == 1);
    // 没有新来信就不硬发:再 tick 也不发。
    fixture.TickUntilQuiet(10);
    CHECK(fixture.sidecar.send_count_for(item->delivery_id) == 1);
}

// ---------------------------------------------------------------------------
// 验收 6:claim 后崩——重派执行(attempt+1),模型不多跑
// ---------------------------------------------------------------------------

TEST_CASE("claim 后崩(渠道任务):重建后重派执行,结果照投,模型恰一次") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    std::int64_t fire_time = 0;
    {
        Q5Fixture fixture("claim-crash");
        fixture.scripts = {ToolUseScript("tu-1", "create_reminder",
                                         CreateOnceInput(fixture.now + 300000)),
                           TextScript("已设置五分钟后提醒。")};
        tools::ToolRegistry registry;
        REQUIRE(fixture.OpenPumps(registry));
        fixture.EmitAndIngest(
            MakeDmAt("in-1", "pe-1", "dm-a", "五分钟后提醒我喝水", "m-1", fixture.now));
        REQUIRE(fixture.Tick());
        fixture.TickUntilQuiet();
        REQUIRE(CountOf(fixture.counter_file, "model") == 2);
        // 到点后由"进程"直接认领(泵在 claim 后、绑定行落账前瞬间死)。
        fire_time = fixture.now + 300000 + 1;
        fixture.now = fire_time;
        REQUIRE(fixture.store()
                    ->ClaimDue("crash-epoch", fixture.now,
                               gateway::AutomationStore::ClaimScope::ChannelBackedOnly)
                    .has_value());
    }
    {
        // 重建:新进程的钟要拨回同一时刻(重建夹具的 now 归零回 T0,
        // 不拨的话 occurrence 不到点,重派了也不会认领)。
        Q5Fixture fixture("claim-crash", /*rebuild=*/true);
        fixture.now = fire_time + 1;
        fixture.scripts = {TextScript("提醒:五分钟到了。")};
        tools::ToolRegistry registry;
        REQUIRE(fixture.OpenPumps(registry));
        REQUIRE(fixture.Tick());
        fixture.TickUntilQuiet();
        // 重派(attempt+1)后执行恰一次:模型总数 3(建账轮 2 + 任务轮 1)。
        REQUIRE(CountOf(fixture.counter_file, "model") == 3);
        REQUIRE(fixture.sidecar.sent_messages().size() == 1);
        CHECK(fixture.sidecar.sent_messages()[0].params["parts"][0]["text"] == "提醒:五分钟到了。");
        const auto jobs = fixture.store()->ListJobs();
        REQUIRE(jobs.size() == 1);
        const auto occurrences = fixture.store()->ListJobOccurrences(jobs[0].job_id);
        REQUIRE(occurrences.size() == 1);
        CHECK(occurrences[0].outcome == "succeeded");
        CHECK(occurrences[0].attempt == 2);  // 重派过一次
    }
}

// ---------------------------------------------------------------------------
// 工具面 fail closed:无渠道上下文/无 automation 域/非单聊/时间已过
// ---------------------------------------------------------------------------

TEST_CASE("工具面 fail closed:上下文缺位与坏输入明拒,不猜") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q5Fixture fixture("fail-closed");
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));

    // 无渠道上下文(终端路/自动任务路):必拒。
    auto outcome = fixture.bridge->CreateReminder(nlohmann::json{{"description", "x"}});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error_code == "channel_job.not_in_channel_turn");

    runtime::ChannelAutomationBridge::TurnContext context;
    context.channel_id = "qqbot";
    context.account_id = "main";
    context.conversation_id = "dm-a";
    context.sender_id = "sender-dm-a";
    context.message_id = "m-1";
    context.received_at_ms = fixture.now;
    const runtime::ChannelAutomationBridge::TurnScope scope(*fixture.bridge, context);

    // 时间已过:明拒,不静默改明天(§11.2)。
    outcome = fixture.bridge->CreateReminder(nlohmann::json{{"description", "喝水"},
                                                            {"at_ms", fixture.now - 1}});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error_code == "channel_job.time_in_past");
    // 缺时间规格:明拒不猜。
    outcome = fixture.bridge->CreateReminder(nlohmann::json{{"description", "喝水"}});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error_code == "channel_job.bad_input");
    // 非单聊:交付面不装懂(群聊建单归群聊开放批次)。
    runtime::ChannelAutomationBridge::TurnContext group_context = context;
    group_context.conversation_kind = channel::ConversationKind::Group;
    {
        const runtime::ChannelAutomationBridge::TurnScope group_scope(*fixture.bridge,
                                                                      group_context);
        outcome = fixture.bridge->CreateReminder(nlohmann::json{{"description", "喝水"},
                                                                {"at_ms", fixture.now + 60000}});
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.error_code == "channel_job.direct_only");
    }

    // automation 域缺席(store null):工具 fail closed。
    runtime::ChannelAutomationBridge storeless(/*store=*/nullptr);
    const runtime::ChannelAutomationBridge::TurnScope storeless_scope(storeless, context);
    outcome = storeless.CreateReminder(nlohmann::json{{"description", "喝水"},
                                                      {"at_ms", context.received_at_ms + 60000}});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error_code == "channel_job.domain_unavailable");
}

// ---------------------------------------------------------------------------
// 执行前重验:创建者失权 → occurrence cancelled,不越权执行
// ---------------------------------------------------------------------------

TEST_CASE("撤销准入后到点:ProbeRoute 重验不过,occurrence 结算 cancelled 零模型") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q5Fixture fixture("revoke");
    fixture.scripts = {ToolUseScript("tu-1", "create_reminder",
                                     CreateOnceInput(fixture.now + 600000)),
                       TextScript("已设置十分钟后提醒。")};
    tools::ToolRegistry registry;
    REQUIRE(fixture.OpenPumps(registry));
    fixture.EmitAndIngest(
        MakeDmAt("in-1", "pe-1", "dm-a", "十分钟后提醒我喝水", "m-1", fixture.now));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 2);

    // 撤销准入:manager 没有换账号配置的口,用同档双 binding 冲突制造
    // Rejected(同一只路由纯函数的重验面,与"权限撤销"Q2 案同款手法)。
    std::vector<channel::ChannelBindingConfig> bindings(2);
    for (auto& binding : bindings) {
        channel::ChannelBindingConversationMatch conversation;
        conversation.kind = "direct";
        conversation.id = "dm-a";
        binding.match.conversation = conversation;
    }
    fixture.manager->SetChannelBindings("qqbot", std::move(bindings));

    fixture.now = fixture.now + 600000 + 1;
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet(10);
    REQUIRE(CountOf(fixture.counter_file, "model") == 2);  // 重验不过,零模型
    const auto jobs = fixture.store()->ListJobs();
    REQUIRE(jobs.size() == 1);
    const auto occurrences = fixture.store()->ListJobOccurrences(jobs[0].job_id);
    REQUIRE(occurrences.size() == 1);
    CHECK(occurrences[0].outcome == "cancelled");
    CHECK(occurrences[0].detail.rfind("creator_not_admitted", 0) == 0);
}
