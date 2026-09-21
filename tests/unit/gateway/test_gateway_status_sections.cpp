// 常驻总装 V1 第五件事:status 分栏册。钉的合同(status.hpp):
//   - process/work/execution/delivery 四栏分报——进程 running 不冒充任务
//     成功:没有任务账时 work/execution 栏如实空,不因进程活着画出成功;
//   - 投影只读:账不在(目录根本不存在)零建目录零写盘(disabled 零副作用
//     合同 §6 的 status 面不破);
//   - execution 栏带 session/turn 落点(绑定账),outcome 如实(in_flight
//     未结算的枚也算"跑过/在跑",不混进 scheduled)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "gateway/automation_store.hpp"
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "gateway/status.hpp"

using namespace lubancode::gateway;

namespace {

struct Fixture {
    std::filesystem::path root;
    GatewayProfilePaths paths;

    explicit Fixture(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-gw-status-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        paths = ResolveGatewayProfilePaths(root, "default");
    }
};

}  // namespace

TEST_CASE("空 profile:三栏全空、零建目录零写盘") {
    Fixture fixture("empty");
    const auto sections = ProbeStatusSections(fixture.paths);
    CHECK_FALSE(sections.work_ledger_present);
    CHECK_FALSE(sections.delivery_ledger_present);
    CHECK(sections.jobs_total == 0);
    CHECK(sections.recent_executions.empty());
    CHECK(sections.delivery_pending == 0);
    // 零副作用:profile 目录不该被造出来。
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(fixture.paths.profile_dir, ec));
}

TEST_CASE("有账分报:work/execution/delivery 各归各栏,不拿进程冒充任务") {
    Fixture fixture("sections");
    // automation 账:一枚 succeeded(带绑定)、一枚 in_flight。
    {
        AutomationStore store;
        REQUIRE(AutomationStore::Open(&store, fixture.paths.automation_log).ok);
        const auto ok_job = store.CreateOnceJob("job-done", "跑完的", 1000, 1, "");
        const auto running = store.CreateOnceJob("job-run", "在跑的", 2000, 2, "");
        REQUIRE(ok_job.accepted);
        REQUIRE(running.accepted);
        const auto claimed_done = store.ClaimDue("e", 3000);
        REQUIRE(claimed_done.has_value());
        REQUIRE(store.BindOccurrence(claimed_done->occurrence_id, "sess-9", "turn-9", 3100));
        REQUIRE(store.SettleOccurrence(claimed_done->occurrence_id, "succeeded", "delivered", 3200));
        const auto claimed_run = store.ClaimDue("e", 3300);
        REQUIRE(claimed_run.has_value());
        REQUIRE(store.BindOccurrence(claimed_run->occurrence_id, "sess-8", "turn-8", 3400));
    }
    // delivery 账:一枚 pending。
    {
        DurableReplyOutbox outbox;
        DurableReplyOutbox::Paths paths;
        paths.log_file = fixture.paths.outbox_log;
        paths.replies_dir = fixture.paths.replies_dir;
        paths.published_dir = fixture.paths.published_dir;
        REQUIRE(DurableReplyOutbox::Open(&outbox, paths).ok);
        REQUIRE(outbox.Enqueue("sel-x", "正文", "s", "t", 1).accepted);
    }

    const auto sections = ProbeStatusSections(fixture.paths);
    CHECK(sections.work_ledger_present);
    CHECK(sections.delivery_ledger_present);
    CHECK(sections.jobs_total == 2);
    CHECK(sections.occurrences_succeeded == 1);
    CHECK(sections.occurrences_in_flight == 1);
    CHECK(sections.occurrences_failed == 0);
    CHECK(sections.delivery_pending == 1);

    // execution 栏:两枚都进(in_flight 不结算也如实报),带 session/turn。
    REQUIRE(sections.recent_executions.size() == 2);
    bool saw_succeeded = false;
    bool saw_in_flight = false;
    for (const auto& entry : sections.recent_executions) {
        if (entry.outcome == "succeeded") {
            saw_succeeded = true;
            CHECK(entry.session_id == "sess-9");
            CHECK(entry.turn_id == "turn-9");
        } else if (entry.outcome.empty()) {
            saw_in_flight = true;
            CHECK(entry.session_id == "sess-8");
        }
    }
    CHECK(saw_succeeded);
    CHECK(saw_in_flight);

    // JSON 面与文本面都出得来(字段抽查;人话行含四栏标题)。
    const nlohmann::json json = SectionsToJson(sections);
    CHECK(json.contains("work"));
    CHECK(json.contains("execution"));
    CHECK(json.contains("delivery"));
    CHECK(json["work"]["occurrences_succeeded"] == 1);
    const auto lines = FormatSectionLines(sections);
    bool has_work = false;
    bool has_delivery = false;
    for (const std::string& line : lines) {
        if (line.rfind("[work]", 0) == 0) has_work = true;
        if (line.rfind("[delivery]", 0) == 0) has_delivery = true;
    }
    CHECK(has_work);
    CHECK(has_delivery);
}

TEST_CASE("进程没跑任务也如实:账在但全 scheduled,execution 栏空") {
    Fixture fixture("noexec");
    {
        AutomationStore store;
        REQUIRE(AutomationStore::Open(&store, fixture.paths.automation_log).ok);
        REQUIRE(store.CreateOnceJob("future", "还没到点", 999999999, 1, "").accepted);
    }
    const auto sections = ProbeStatusSections(fixture.paths);
    CHECK(sections.work_ledger_present);
    CHECK(sections.jobs_total == 1);
    CHECK(sections.recent_executions.empty());  // 没跑过就没记录,不画成功
}

TEST_CASE("channel 栏:读账号状态快照/入站水位/投递错误,不带密钥与平台事件") {
    Fixture fixture("channel");
    // channels 根与 gateway 根同级(生产布局:两棵状态树同挂 <状态根> 下,
    // 见 gateway::DefaultGatewayRoot/channel::DefaultChannelsStateRoot;
    // ProbeStatusSections 由 paths.root.parent_path() 推导)。
    const std::filesystem::path channels_root = fixture.root.parent_path() / "channels";
    {
        std::error_code ec;
        std::filesystem::remove_all(channels_root, ec);  // 防跨轮残留
    }
    const std::filesystem::path account_dir = channels_root / "qqbot" / "main";
    {
        std::error_code ec;
        std::filesystem::create_directories(account_dir / "ingress", ec);
        // 账号状态快照(manager 状态迁移写的)。
        const nlohmann::json status = nlohmann::json::object({
            {"schema", 1},
            {"channelId", "qqbot"},
            {"accountId", "main"},
            {"state", "running"},
            {"generation", 2},
            {"updatedAtMs", 1724700000000},
            {"lastReason", ""},
            {"lastDetail", ""},
        });
        std::ofstream status_file(account_dir / "account-status.json", std::ios::binary);
        status_file << status.dump();
        // 入站账:一枚 queued(待处理)。event 须是能过 FromJsonStrict 的
        // 合法事件——SV-05 起状态投影与恢复器共用同一行合同,缺合法
        // event 的 evt 行不再计入水位。
        std::ofstream journal(account_dir / "ingress" / "journal.jsonl", std::ios::binary);
        journal << nlohmann::json({{"schema", 1},
                                   {"t", "evt"},
                                   {"sid", 1},
                                   {"dedupe", "p:qqbot:main:pe-1"},
                                   {"tier", 1},
                                   {"parts_sha256", "x"},
                                   {"event", nlohmann::json::object({
                                       {"schema", 1},
                                       {"delivery_id", "in-1"},
                                       {"provider_event_id", "pe-1"},
                                       {"channel_id", "qqbot"},
                                       {"account_id", "main"},
                                       {"received_at_ms", 1724700000000},
                                       {"conversation", nlohmann::json::object({
                                           {"kind", "direct"},
                                           {"id", "dm-1"},
                                       })},
                                       {"sender", nlohmann::json::object({{"id", "sender-1"}})},
                                       {"message_id", "m-1"},
                                   })}})
                       .dump()
                << "\n";
        journal << nlohmann::json({{"schema", 1}, {"t", "tr"}, {"sid", 1},
                                   {"to", "queued"}, {"reason", ""}})
                       .dump()
                << "\n";
    }
    // outbox:一枚渠道项终态失败(投递错误栏)。
    {
        DurableReplyOutbox outbox;
        DurableReplyOutbox::Paths paths;
        paths.log_file = fixture.paths.outbox_log;
        paths.replies_dir = fixture.paths.replies_dir;
        paths.published_dir = fixture.paths.published_dir;
        REQUIRE(DurableReplyOutbox::Open(&outbox, paths).ok);
        DurableReplyOutbox::ChannelTarget target;
        target.channel_id = "qqbot";
        target.account_id = "main";
        target.conversation_id = "dm-owner";
        target.source_ref = "ingress:qqbot:main:1";
        const auto receipt = outbox.EnqueueChannel("sel-ch", "正文", "s", "t", target, 1);
        REQUIRE(receipt.accepted);
        REQUIRE(outbox.RecordAttempt(receipt.delivery_ids[0], 2));
        REQUIRE(outbox.MarkChannelFailed(receipt.delivery_ids[0], "platform_reject", 3));
    }

    const auto sections = ProbeStatusSections(fixture.paths);
    CHECK(sections.channel_ledger_present);
    REQUIRE(sections.channels.size() == 1);
    const auto& entry = sections.channels[0];
    CHECK(entry.channel_id == "qqbot");
    CHECK(entry.account_id == "main");
    CHECK(entry.connection_state == "running");
    CHECK(entry.generation == 2);
    CHECK(entry.ingress_pending == 1);
    REQUIRE(entry.delivery_errors.size() == 1);
    CHECK(entry.delivery_errors[0].find("platform_reject") != std::string::npos);

    const nlohmann::json json = SectionsToJson(sections);
    REQUIRE(json.contains("channel"));
    REQUIRE(json["channel"].size() == 1);
    CHECK(json["channel"][0]["connection_state"] == "running");
    CHECK(json["channel"][0]["delivery_errors"].size() == 1);
    const auto lines = FormatSectionLines(sections);
    bool has_channel_line = false;
    bool has_error_line = false;
    for (const std::string& line : lines) {
        if (line.rfind("[channel]", 0) == 0) has_channel_line = true;
        if (line.find("投递错误") != std::string::npos) has_error_line = true;
    }
    CHECK(has_channel_line);
    CHECK(has_error_line);
}
