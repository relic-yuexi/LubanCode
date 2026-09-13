// 常驻总装 V1:AutomationStore 单元册。钉的合同(automation_store.hpp 与
// contracts.md §11):
//   - 账落得住:创建/触发/认领/绑定/结算各行走 PowerLoss 追加行;
//   - 重启重建得起:重开从账重放,内存投影与关前一致;
//   - 身份不随恢复洗掉:occurrenceId 定式散列,同 (jobId, revision, slot)
//     恒同一 id;
//   - 幂等:创建/触发同键同载荷回原回执;同键异载荷 conflict;
//   - claim 次序:slot 最老先得;结算幂等(首笔为准);
//   - 写盘失败:账行落不了 → broken,后续受理全拒(写盘失败停止受理)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gateway/automation_store.hpp"
#include "gateway/profile.hpp"
#include "platform/wall_clock.hpp"

using namespace lubancode::gateway;

namespace {

std::filesystem::path FreshLog(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-automation-store-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir / "jobs.jsonl";
}

AutomationStore::OpenResult OpenStore(AutomationStore* store, const std::filesystem::path& log) {
    return AutomationStore::Open(store, log);
}

std::size_t CountLines(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (text.empty()) return 0;
    std::size_t lines = 1;
    for (const char c : text) {
        if (c == '\n') ++lines;
    }
    return lines;
}

}  // namespace

TEST_CASE("occurrenceId 定式:同 (jobId, revision, slot) 恒同一 id,带 occ- 前缀") {
    const std::string a = MakeOccurrenceId("job-1", 1, 1000);
    const std::string b = MakeOccurrenceId("job-1", 1, 1000);
    const std::string c = MakeOccurrenceId("job-1", 1, 1001);
    const std::string d = MakeOccurrenceId("job-2", 1, 1000);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != d);
    CHECK(a.rfind("occ-", 0) == 0);
    CHECK(a.size() == 4 + 16);
}

TEST_CASE("once 创建:建 job 同笔落首枚 occurrence;账行数可数") {
    AutomationStore store;
    const auto log = FreshLog("create");
    const auto open = OpenStore(&store, log);
    REQUIRE(open.ok);

    const auto receipt = store.CreateOnceJob("", "每天九点检查仓库", 5000, 1000, "key-1");
    CHECK(receipt.accepted);
    CHECK(receipt.job_id == "job-1");
    CHECK(!receipt.occurrence_id.empty());

    // 两行:job.created + occurrence.created。
    CHECK(CountLines(log) == 2);

    const auto job = store.FindJob("job-1");
    REQUIRE(job.has_value());
    CHECK(job->prompt == "每天九点检查仓库");
    CHECK(job->due_at_ms == 5000);

    const auto occurrence = store.FindOccurrence(receipt.occurrence_id);
    REQUIRE(occurrence.has_value());
    CHECK(occurrence->state == AutomationOccurrence::State::Scheduled);
    CHECK(occurrence->slot_ms == 5000);
}

TEST_CASE("幂等:创建同键同载荷回原回执;同键异载荷 conflict;run-now 同键回原 occurrence") {
    AutomationStore store;
    const auto open = OpenStore(&store, FreshLog("idem"));
    REQUIRE(open.ok);

    const auto first = store.CreateOnceJob("j1", "问一句", 100, 1, "k1");
    REQUIRE(first.accepted);
    const auto again = store.CreateOnceJob("j1", "问一句", 100, 2, "k1");
    CHECK(again.duplicate);
    CHECK(again.job_id == "j1");

    const auto conflict = store.CreateOnceJob("", "另一句", 100, 3, "k1");
    CHECK_FALSE(conflict.accepted);
    CHECK_FALSE(conflict.duplicate);
    CHECK(conflict.error_code == "automation.revision_conflict");

    const auto run1 = store.RequestRunNow("j1", 2000, "rk1");
    REQUIRE(run1.accepted);
    const auto run2 = store.RequestRunNow("j1", 3000, "rk1");
    CHECK(run2.duplicate);
    CHECK(run2.occurrence_id == run1.occurrence_id);
}

TEST_CASE("claim/绑定/结算:slot 最老先得;结算幂等;OpenOccurrences 只报未结算") {
    AutomationStore store;
    const auto open = OpenStore(&store, FreshLog("claim"));
    REQUIRE(open.ok);

    store.CreateOnceJob("late", "晚", 9000, 1, "");
    store.CreateOnceJob("early", "早", 3000, 2, "");
    store.RequestRunNow("late", 4000, "");

    // due:early(3000) < late(9000)。
    const auto claimed = store.ClaimDue("epoch-a", 10000);
    REQUIRE(claimed.has_value());
    const auto early_job = store.FindJob("early");
    REQUIRE(early_job.has_value());
    // claimed 的 occurrence 归属 early job(slot 3000 那枚,once 首枚)。
    const auto early_occ = store.FindOccurrence(MakeOccurrenceId("early", 1, 3000));
    REQUIRE(early_occ.has_value());
    CHECK(claimed->occurrence_id == early_occ->occurrence_id);
    CHECK(claimed->owner_epoch == "epoch-a");

    CHECK(store.BindOccurrence(claimed->occurrence_id, "sess-1", "turn-1", 11000));
    const auto bound = store.FindOccurrence(claimed->occurrence_id);
    REQUIRE(bound.has_value());
    CHECK(bound->session_id == "sess-1");
    CHECK(bound->turn_id == "turn-1");

    CHECK(store.SettleOccurrence(claimed->occurrence_id, "succeeded", "delivered", 12000));
    // 幂等:重复结算拒,首笔为准。
    CHECK_FALSE(store.SettleOccurrence(claimed->occurrence_id, "failed", "重翻", 13000));
    const auto settled = store.FindOccurrence(claimed->occurrence_id);
    REQUIRE(settled.has_value());
    CHECK(settled->outcome == "succeeded");

    // OpenOccurrences 只报 claimed 未结算。
    REQUIRE(store.OpenOccurrences().size() == 0);
    const auto next = store.ClaimDue("epoch-a", 20000);
    REQUIRE(next.has_value());
    CHECK(store.OpenOccurrences().size() == 1);
}

TEST_CASE("重启重建:重开投影与关前一致(账是唯一真源)") {
    const auto log = FreshLog("replay");
    std::string occurrence_id;
    {
        AutomationStore store;
        REQUIRE(OpenStore(&store, log).ok);
        const auto receipt = store.CreateOnceJob("j", "问", 100, 1, "k");
        occurrence_id = receipt.occurrence_id;
        store.RequestRunNow("j", 500, "rk");
        REQUIRE(store.ClaimDue("e1", 600).has_value());
        store.BindOccurrence(occurrence_id, "s1", "t1", 700);
    }
    {
        AutomationStore store;
        const auto open = OpenStore(&store, log);
        REQUIRE(open.ok);
        CHECK(open.skipped_lines == 0);

        const auto jobs = store.ListJobs();
        REQUIRE(jobs.size() == 1);
        CHECK(jobs[0].job_id == "j");

        const auto occurrence = store.FindOccurrence(occurrence_id);
        REQUIRE(occurrence.has_value());
        CHECK(occurrence->state == AutomationOccurrence::State::Claimed);
        CHECK(occurrence->session_id == "s1");
        CHECK(occurrence->turn_id == "t1");
        CHECK(store.DueCount(100000) == 1);  // run-now 那枚还 scheduled
        REQUIRE(store.OpenOccurrences().size() == 1);
    }
}

TEST_CASE("坏账容错:坏行跳过留数,不崩宿主;幂等键穿透重建") {
    const auto log = FreshLog("badlines");
    {
        AutomationStore store;
        REQUIRE(OpenStore(&store, log).ok);
        store.CreateOnceJob("j", "问", 100, 1, "k");
    }
    {
        std::ofstream append(log, std::ios::binary | std::ios::app);
        append << "{不是 json\n";  // 坏行
    }
    AutomationStore store;
    const auto open = OpenStore(&store, log);
    REQUIRE(open.ok);
    CHECK(open.skipped_lines == 1);
    // 幂等键仍生效:同键同载荷 duplicate(不重复建)。
    const auto again = store.CreateOnceJob("j", "问", 100, 2, "k");
    CHECK(again.duplicate);
}

TEST_CASE("写盘失败:账行落不了 → broken,后续受理全拒") {
    AutomationStore store;
    // 目录占位顶掉账文件路径:AppendLine 必失败。
    const auto dir = std::filesystem::temp_directory_path() /
                     "lubancode-automation-store-writefail";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "jobs.jsonl", ec);  // 目录占位
    const auto open = OpenStore(&store, dir / "jobs.jsonl");
    REQUIRE(open.ok);  // 打开(不写)成功;首笔写才撞
    const auto receipt = store.CreateOnceJob("j", "问", 100, 1, "");
    CHECK_FALSE(receipt.accepted);
    CHECK(receipt.error_code == "automation.append_failed");
    CHECK(store.broken());
    // broken 传播:后续一切写口全拒。
    CHECK_FALSE(store.CreateOnceJob("j2", "问", 100, 2, "").accepted);
    CHECK_FALSE(store.SettleOccurrence("x", "succeeded", "", 1));
}

TEST_CASE("只读投影:文件不存在给空投影,零副作用") {
    const auto missing = std::filesystem::temp_directory_path() /
                         "lubancode-automation-store-missing-root/jobs.jsonl";
    std::error_code ec;
    std::filesystem::remove_all(missing.parent_path(), ec);
    const AutomationProjection projection = ReadAutomationProjection(missing);
    CHECK(projection.jobs.empty());
    CHECK(projection.occurrences.empty());
    CHECK(projection.skipped_lines == 0);
    CHECK_FALSE(std::filesystem::exists(missing.parent_path(), ec));
}
