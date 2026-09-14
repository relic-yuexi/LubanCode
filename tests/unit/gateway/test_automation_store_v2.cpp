// 常驻总装 V2:AutomationStore 周期语义与领域操作册。钉的合同
// (automation_store.hpp 与 contracts.md §13):
//   - 周期生成:interval/cron 到点出拍;停机跨多周期 coalesce 补一拍
//     (最老一拍 + missedCount 覆盖范围)/ skip 跳过等未来;时钟倒拨不
//     重跑原 slot(游标只前进);同 job 不重叠;队列帽满停在原地;
//   - 领域操作:update/pause/resume/cancel 带 expectedRevision(CAS,0 拒)
//     与幂等键;pause 停生成停派发,resume 不补跑 paused 窗口,cancel
//     先停未来派发(scheduled 就地结算 cancelled,历史保留);
//   - deadline:过线不派、结算 cancelled 不判 failed;
//   - 重派:claim 后无绑定重派同一 occurrence(attempt+1),attempt 帽
//     (3)到顶拒;身份定式不洗;
//   - heartbeat 观察账:occurrence.observed 与 job.last_observed_sha;
//   - /loop 导入 receipt:同来源幂等,不双跑;
//   - 重放:全部新行类型回放后状态与关前一致(账是唯一真源)。
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "gateway/automation_store.hpp"

using namespace lubancode::gateway;

namespace {

std::filesystem::path FreshLog(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-automation-store-v2-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir / "jobs.jsonl";
}

AutomationStore::JobSpec IntervalSpec(const std::string& prompt, std::int64_t interval_seconds,
                                      MisfirePolicy misfire = MisfirePolicy::Coalesce) {
    AutomationStore::JobSpec spec;
    spec.prompt = prompt;
    spec.kind = ScheduleKind::Interval;
    spec.interval_seconds = interval_seconds;
    spec.misfire = misfire;
    return spec;
}

}  // namespace

TEST_CASE("创建校验:坏 interval/坏 cron/坏时区/continuation 明拒;once 不受影响") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("validate")).ok);

    auto bad_interval = IntervalSpec("问", 0);
    auto receipt = store.CreateJob(bad_interval, 1000, "");
    CHECK_FALSE(receipt.accepted);
    CHECK(receipt.error_code.rfind("automation.schedule_invalid", 0) == 0);

    AutomationStore::JobSpec bad_cron;
    bad_cron.prompt = "问";
    bad_cron.kind = ScheduleKind::Cron;
    bad_cron.cron_expr = "0 9 * * 9";  // 周越界
    receipt = store.CreateJob(bad_cron, 1000, "");
    CHECK_FALSE(receipt.accepted);

    AutomationStore::JobSpec bad_tz;
    bad_tz.prompt = "问";
    bad_tz.kind = ScheduleKind::Cron;
    bad_tz.cron_expr = "0 9 * * *";
    bad_tz.timezone = "Mars/Olympus";
    receipt = store.CreateJob(bad_tz, 1000, "");
    CHECK_FALSE(receipt.accepted);
    CHECK(receipt.error_code.rfind("automation.timezone_invalid", 0) == 0);

    AutomationStore::JobSpec continuation;
    continuation.prompt = "问";
    continuation.session_policy = "continuation";
    receipt = store.CreateJob(continuation, 1000, "");
    CHECK_FALSE(receipt.accepted);  // continuation 归 V3 渠道线,明拒不猜

    receipt = store.CreateOnceJob("once-j", "问", 1000, 1000, "");
    CHECK(receipt.accepted);  // V1 once 路不受影响
}

TEST_CASE("coalesce:停机跨多周期合并补一拍,occurrence 落最老一拍带覆盖范围") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("coalesce")).ok);
    // anchor = 1000,周期 60s:拍在 61000/121000/181000/241000/301000。
    REQUIRE(store.CreateJob(IntervalSpec("巡检", 60), 1000, "").accepted);
    {
        const auto sweep = store.SweepSchedule(301000);
        CHECK(sweep.ok);
        CHECK(sweep.generated == 1);
        CHECK(sweep.merged == 0);
        CHECK_FALSE(sweep.stalled);
    }
    const auto occurrences = store.ListOccurrences();
    REQUIRE(occurrences.size() == 1);
    CHECK(occurrences[0].slot_ms == 61000);      // 最老一拍(计划内 slot)
    CHECK(occurrences[0].missed_count == 4);     // 并掉 4 拍,覆盖范围在账
    CHECK(occurrences[0].reason == "schedule");
    const auto job = store.FindJob("job-1");
    REQUIRE(job.has_value());
    CHECK(job->schedule_cursor_ms == 301000);
    CHECK(occurrences[0].occurrence_id ==
          MakeOccurrenceId("job-1", 1, 61000));  // 身份定式不洗
    // 再扫无新拍(下一拍 361000 还没到)。
    const auto again = store.SweepSchedule(302000);
    CHECK(again.generated == 0);
    CHECK(again.merged == 0);
}

TEST_CASE("skip:迟到的拍不补不并,游标直进;最近一拍在宽限内照跑") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("skip")).ok);
    REQUIRE(store.CreateJob(IntervalSpec("巡检", 60, MisfirePolicy::Skip), 1000, "").accepted);
    {
        // 停机到 301000:61k..241k 四拍迟到(后继已到点)全跳;301000 这拍
        // 在宽限内(min(60s,60s))算当前拍,照跑。
        const auto sweep = store.SweepSchedule(301000);
        CHECK(sweep.ok);
        CHECK(sweep.generated == 1);
        CHECK(sweep.skipped_slots == 4);
    }
    const auto occurrences = store.ListOccurrences();
    REQUIRE(occurrences.size() == 1);
    CHECK(occurrences[0].slot_ms == 301000);
    CHECK(occurrences[0].missed_count == 4);  // 覆盖范围在账
    const auto job = store.FindJob("job-1");
    REQUIRE(job.has_value());
    CHECK(job->schedule_cursor_ms == 301000);
    // 当前拍收口后,下一拍正常出。
    REQUIRE(store.ClaimDue("epoch-a", 302000).has_value());
    REQUIRE(store.SettleOccurrence(MakeOccurrenceId("job-1", 1, 301000), "succeeded", "", 303000));
    const auto next = store.SweepSchedule(361000);
    CHECK(next.generated == 1);
    CHECK(next.skipped_slots == 0);
    const auto after = store.ListJobOccurrences("job-1");  // 按 slot 排序
    REQUIRE(after.size() == 2);
    CHECK(after[1].slot_ms == 361000);
    CHECK(after[1].missed_count == 0);
}

TEST_CASE("skip cron 全迟到:最近一拍超出宽限 → 全跳,下一拍等未来") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("skipall")).ok);
    AutomationStore::JobSpec spec;
    spec.prompt = "日报";
    spec.kind = ScheduleKind::Cron;
    spec.cron_expr = "0 9 * * *";
    spec.timezone = "UTC";
    spec.misfire = MisfirePolicy::Skip;
    // 当日 09:00 已过才建账:首拍在次日。
    const std::int64_t created = CivilToUtcMs(CivilTime{2026, 6, 1, 10, 0, 0});
    REQUIRE(store.CreateJob(spec, created, "").accepted);
    // 停机到 06-03 正午:06-02 与 06-03 的九点两拍,最近一拍已迟 3 小时
    //(宽限 60s)→ 全跳。
    const std::int64_t june3_noon = CivilToUtcMs(CivilTime{2026, 6, 3, 12, 0, 0});
    const auto sweep = store.SweepSchedule(june3_noon);
    CHECK(sweep.ok);
    CHECK(sweep.generated == 0);
    CHECK(sweep.skipped_slots == 2);
    CHECK(store.ListOccurrences().empty());
    const auto job = store.FindJob("job-1");
    REQUIRE(job.has_value());
    CHECK(job->schedule_cursor_ms == CivilToUtcMs(CivilTime{2026, 6, 3, 9, 0, 0}));
    // 下一拍到点即扫(宽限内):照常出活。
    const std::int64_t june4_0900 = CivilToUtcMs(CivilTime{2026, 6, 4, 9, 0, 0});
    const auto next = store.SweepSchedule(june4_0900);
    CHECK(next.generated == 1);
    CHECK(next.skipped_slots == 0);
    const auto occurrences = store.ListOccurrences();
    REQUIRE(occurrences.size() == 1);
    CHECK(occurrences[0].slot_ms == june4_0900);
    CHECK(occurrences[0].missed_count == 0);
}

TEST_CASE("时钟倒拨:游标只前进,倒拨后扫描不重跑原 slot、不出新拍") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("rollback")).ok);
    REQUIRE(store.CreateJob(IntervalSpec("巡检", 60), 1000, "").accepted);
    REQUIRE(store.SweepSchedule(181000).generated == 1);
    REQUIRE(store.ClaimDue("epoch-a", 181000).has_value());  // 认领并结算
    REQUIRE(store.SettleOccurrence(
        MakeOccurrenceId("job-1", 1, 61000), "succeeded", "", 182000));
    const std::size_t settled = store.ListOccurrences().size();

    // 时钟倒拨到 100000(游标已是 181000):不出新拍,不动游标。
    const auto sweep = store.SweepSchedule(100000);
    CHECK(sweep.ok);
    CHECK(sweep.generated == 0);
    CHECK(sweep.merged == 0);
    CHECK_FALSE(sweep.stalled);
    CHECK(store.ListOccurrences().size() == settled);  // 已结算的不重开
    const auto job = store.FindJob("job-1");
    REQUIRE(job.has_value());
    CHECK(job->schedule_cursor_ms == 181000);
    // 倒拨后再 ClaimDue 也没有可认领的。
    CHECK_FALSE(store.ClaimDue("epoch-a", 100000).has_value());
}

TEST_CASE("同 job 不重叠:claimed 在途不生成不进游标;收口后合并补") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("overlap")).ok);
    REQUIRE(store.CreateJob(IntervalSpec("巡检", 60), 1000, "").accepted);
    REQUIRE(store.SweepSchedule(121000).generated == 1);
    const std::string occurrence_id = MakeOccurrenceId("job-1", 1, 61000);
    REQUIRE(store.ClaimDue("epoch-a", 121000).has_value());  // 在途
    {
        const auto sweep = store.SweepSchedule(301000);
        CHECK(sweep.ok);
        CHECK(sweep.generated == 0);
        CHECK(sweep.merged == 0);  // 在途:拍等它收口
    }
    const auto job = store.FindJob("job-1");
    REQUIRE(job.has_value());
    CHECK(job->schedule_cursor_ms == 121000);  // 游标不动
    // 收口后:余下的拍(181k..301k)合并成一枚新待办。
    REQUIRE(store.SettleOccurrence(occurrence_id, "succeeded", "", 302000));
    const auto sweep = store.SweepSchedule(302000);
    CHECK(sweep.generated == 1);
    const auto occurrences = store.ListJobOccurrences("job-1");
    REQUIRE(occurrences.size() == 2);
    CHECK(occurrences[1].slot_ms == 181000);
    CHECK(occurrences[1].missed_count == 2);
}

TEST_CASE("合并进既有待办:occurrence.merged 记覆盖范围,不建第二枚") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("merge")).ok);
    REQUIRE(store.CreateJob(IntervalSpec("巡检", 60), 1000, "").accepted);
    REQUIRE(store.SweepSchedule(181000).generated == 1);  // 待办在(61k)
    {
        const auto sweep = store.SweepSchedule(301000);   // 又到两拍(241k/301k)
        CHECK(sweep.ok);
        CHECK(sweep.generated == 0);
        CHECK(sweep.merged == 2);
    }
    const auto occurrences = store.ListOccurrences();
    REQUIRE(occurrences.size() == 1);
    CHECK(occurrences[0].slot_ms == 61000);
    CHECK(occurrences[0].missed_count == 2 + 2);  // 初并 2 + 追并 2
}

TEST_CASE("队列帽:open occurrence 达帽停生成(游标不动),腾出后续跑") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("cap")).ok);
    store.set_max_open_occurrences(1);
    REQUIRE(store.CreateJob(IntervalSpec("甲", 60), 1000, "").accepted);
    REQUIRE(store.CreateJob(IntervalSpec("乙", 60), 1000, "").accepted);
    const auto sweep = store.SweepSchedule(121000);
    CHECK(sweep.stalled);           // 帽满:乙的拍停生成
    CHECK(sweep.generated == 1);    // 甲的待办占住唯一名额
    const auto job_b = store.FindJob("job-2");
    REQUIRE(job_b.has_value());
    CHECK(job_b->schedule_cursor_ms == 1000);  // 乙游标未动
    // 甲收口腾名额:乙的拍补上(合并)。
    REQUIRE(store.SettleOccurrence(MakeOccurrenceId("job-1", 1, 61000), "succeeded", "",
                                   122000));
    const auto again = store.SweepSchedule(122000);
    CHECK(again.generated == 1);
    const auto occurrences_b = store.ListJobOccurrences("job-2");
    REQUIRE(occurrences_b.size() == 1);
    CHECK(occurrences_b[0].slot_ms == 61000);
}

TEST_CASE("领域操作 CAS:revision 必须显式且相等;幂等键回原回执") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("cas")).ok);
    REQUIRE(store.CreateOnceJob("j", "旧正文", 1000, 1000, "").accepted);

    // 0 = 拒;错值 = 拒。
    AutomationStore::JobUpdatePatch patch;
    patch.set_prompt = true;
    patch.prompt = "新正文";
    auto receipt = store.UpdateJob("j", 0, patch, 2000, "");
    CHECK(receipt.error_code == "automation.revision_conflict");
    receipt = store.UpdateJob("j", 99, patch, 2000, "");
    CHECK(receipt.error_code == "automation.revision_conflict");
    // 对了:revision +1,字段换新。
    receipt = store.UpdateJob("j", 1, patch, 2000, "upd-1");
    CHECK(receipt.accepted);
    CHECK(receipt.revision == 2);
    CHECK(store.FindJob("j")->prompt == "新正文");
    // 幂等键:同键同操作回原回执。
    receipt = store.UpdateJob("j", 1, patch, 3000, "upd-1");
    CHECK(receipt.duplicate);
    CHECK(receipt.revision == 2);
    // 旧 revision 再用 = 拒。
    receipt = store.UpdateJob("j", 1, patch, 4000, "");
    CHECK(receipt.error_code == "automation.revision_conflict");
    // 不存在的任务。
    receipt = store.UpdateJob("nope", 1, patch, 4000, "");
    CHECK(receipt.error_code == "automation.job_not_found");
}

TEST_CASE("update 改排即重锚:新拍按新 revision 起时间轴,旧待办不动") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("reanchor")).ok);
    REQUIRE(store.CreateJob(IntervalSpec("巡检", 60), 1000, "").accepted);
    REQUIRE(store.SweepSchedule(121000).generated == 1);
    const std::string old_id = MakeOccurrenceId("job-1", 1, 61000);
    // 旧待办先收口(单待办语义:不收口则新拍并进它,不出第二枚)。
    REQUIRE(store.ClaimDue("epoch-a", 122000).has_value());
    REQUIRE(store.SettleOccurrence(old_id, "succeeded", "", 123000));

    AutomationStore::JobUpdatePatch patch;
    patch.set_interval = true;
    patch.interval_seconds = 120;
    REQUIRE(store.UpdateJob("job-1", 1, patch, 400000, "").accepted);
    const auto job = store.FindJob("job-1");
    REQUIRE(job.has_value());
    CHECK(job->interval_seconds == 120);
    CHECK(job->anchor_ms == 400000);
    CHECK(job->schedule_cursor_ms == 400000);  // 重锚
    // 旧待办(61k,rev 1)结算账原样在,新拍按 rev 2 出(400s+120s=520s)。
    CHECK(store.FindOccurrence(old_id).has_value());
    const auto sweep = store.SweepSchedule(530000);
    CHECK(sweep.generated == 1);
    CHECK(store.FindOccurrence(MakeOccurrenceId("job-1", 2, 520000)).has_value());
}

TEST_CASE("pause/resume:停生成停派发;resume 不补跑 paused 窗口") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("pause")).ok);
    REQUIRE(store.CreateJob(IntervalSpec("巡检", 60), 1000, "").accepted);
    REQUIRE(store.SweepSchedule(61000).generated == 1);  // 一枚待办在

    // pause:CAS;待办原地等(不结算)。
    auto receipt = store.PauseJob("job-1", 1, 70000, "");
    CHECK(receipt.accepted);
    CHECK(store.FindJob("job-1")->state == AutomationJobState::Paused);
    CHECK_FALSE(store.ClaimDue("epoch-a", 70000).has_value());  // 暂停不派
    CHECK(store.ListOccurrences().size() == 1);                 // 待办保留
    // 暂停窗口的拍不生成。
    CHECK(store.SweepSchedule(181000).generated == 0);
    // 再 pause:幂等回执。
    receipt = store.PauseJob("job-1", 1, 80000, "");
    CHECK(receipt.duplicate);

    // resume:游标直进到 resume 时刻,paused 窗口不补。
    receipt = store.ResumeJob("job-1", 1, 1000000, "");
    CHECK(receipt.accepted);
    CHECK(store.FindJob("job-1")->state == AutomationJobState::Active);
    CHECK(store.FindJob("job-1")->schedule_cursor_ms == 1000000);
    CHECK(store.SweepSchedule(1000000).generated == 0);  // 下一拍 1060000
    // 那枚暂停前待办现在可派。
    const auto claimed = store.ClaimDue("epoch-a", 1000000);
    REQUIRE(claimed.has_value());
    CHECK(claimed->slot_ms == 61000);
}

TEST_CASE("cancel:先停未来派发(scheduled 就地 cancelled),历史保留;后续操作拒") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("cancel")).ok);
    REQUIRE(store.CreateJob(IntervalSpec("巡检", 60), 1000, "").accepted);
    REQUIRE(store.SweepSchedule(61000).generated == 1);
    const std::string occurrence_id = MakeOccurrenceId("job-1", 1, 61000);

    auto receipt = store.CancelJob("job-1", 1, 70000, "cancel-1");
    CHECK(receipt.accepted);
    CHECK(store.FindJob("job-1")->state == AutomationJobState::Cancelled);
    // 待办结算 cancelled(不判失败,历史保留)。
    const auto occurrence = store.FindOccurrence(occurrence_id);
    REQUIRE(occurrence.has_value());
    CHECK(occurrence->state == AutomationOccurrence::State::Settled);
    CHECK(occurrence->outcome == "cancelled");
    CHECK(occurrence->detail == "job_cancelled");
    // 停机后的拍不再生成。
    CHECK(store.SweepSchedule(1000000).generated == 0);
    // 后续操作:终态拒;run-now 拒;再 cancel 幂等。
    AutomationStore::JobUpdatePatch patch;
    patch.set_prompt = true;
    patch.prompt = "改不了";
    CHECK(store.UpdateJob("job-1", 1, patch, 80000, "").error_code == "automation.job_terminal");
    CHECK(store.RequestRunNow("job-1", 80000, "").error_code == "automation.job_terminal");
    receipt = store.CancelJob("job-1", 1, 90000, "");
    CHECK(receipt.duplicate);
}

TEST_CASE("deadline:过线不生成;待办过线结算 cancelled 不判 failed") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("deadline")).ok);
    AutomationStore::JobSpec spec = IntervalSpec("巡检", 60);
    spec.deadline_ms = 200000;
    REQUIRE(store.CreateJob(spec, 1000, "").accepted);
    REQUIRE(store.SweepSchedule(121000).generated == 1);  // 线前出拍
    const std::string occurrence_id = MakeOccurrenceId("job-1", 1, 61000);

    // 过线后:不生成新拍(241000/301000 都在线后)。
    const auto sweep = store.SweepSchedule(301000);
    CHECK(sweep.generated == 0);
    // 既有待办过线:ClaimDue 就地结算 cancelled(deadline_reached)。
    CHECK_FALSE(store.ClaimDue("epoch-a", 201000).has_value());
    const auto occurrence = store.FindOccurrence(occurrence_id);
    REQUIRE(occurrence.has_value());
    CHECK(occurrence->outcome == "cancelled");
    CHECK(occurrence->detail == "deadline_reached");
}

TEST_CASE("重派:claim 后无绑定重派同一 occurrence(attempt+1);attempt 帽到顶拒") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("redispatch")).ok);
    REQUIRE(store.CreateOnceJob("j", "问", 1000, 1000, "").accepted);
    const std::string occurrence_id = MakeOccurrenceId("j", 1, 1000);

    REQUIRE(store.ClaimDue("epoch-a", 2000).has_value());
    CHECK(store.FindOccurrence(occurrence_id)->attempt == 1);
    // 无绑定 → 重派:同 id,attempt 2,回 Scheduled。
    REQUIRE(store.RedispatchOccurrence(occurrence_id, "claimed_without_binding", 3000));
    {
        const auto occurrence = store.FindOccurrence(occurrence_id);
        REQUIRE(occurrence.has_value());
        CHECK(occurrence->state == AutomationOccurrence::State::Scheduled);
        CHECK(occurrence->attempt == 2);
        CHECK(occurrence->session_id.empty());
    }
    // 再认领再重派:attempt 3(帽内)。
    REQUIRE(store.ClaimDue("epoch-b", 4000).has_value());
    REQUIRE(store.RedispatchOccurrence(occurrence_id, "claimed_without_binding", 5000));
    CHECK(store.FindOccurrence(occurrence_id)->attempt == 3);
    // 帽到顶(3):拒。
    REQUIRE(store.ClaimDue("epoch-c", 6000).has_value());
    CHECK_FALSE(store.RedispatchOccurrence(occurrence_id, "claimed_without_binding", 7000));
    // 已绑定的不重派(有开轮事实,归 V3 账裁决)。
    REQUIRE(store.BindOccurrence(occurrence_id, "sess-1", "turn-1", 8000));
    CHECK_FALSE(store.RedispatchOccurrence(occurrence_id, "claimed_without_binding", 9000));
}

TEST_CASE("heartbeat 观察账:RecordObservation 更新 occurrence 与 job 的已通知版本") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("observe")).ok);
    AutomationStore::JobSpec spec = IntervalSpec("巡检", 60);
    spec.notify_on_change = true;
    REQUIRE(store.CreateJob(spec, 1000, "").accepted);
    REQUIRE(store.SweepSchedule(61000).generated == 1);
    const std::string occurrence_id = MakeOccurrenceId("job-1", 1, 61000);
    REQUIRE(store.ClaimDue("epoch-a", 62000).has_value());

    // 未变(delivered=false,不更新已通知版本)。
    REQUIRE(store.RecordObservation(occurrence_id, "sha-a", false, false, false, 63000));
    CHECK(store.FindJob("job-1")->last_observed_sha.empty());
    CHECK(store.FindOccurrence(occurrence_id)->observed_sha == "sha-a");
    CHECK_FALSE(store.FindOccurrence(occurrence_id)->observed_changed);
    // 有变化且投递成:更新已通知版本。
    REQUIRE(store.RecordObservation(occurrence_id, "sha-b", true, true, true, 64000));
    CHECK(store.FindJob("job-1")->last_observed_sha == "sha-b");
    // 检查失败(changed=true 投递失败通知)不更新已通知版本。
    REQUIRE(store.RecordObservation(occurrence_id, "sha-err", true, true, false, 65000));
    CHECK(store.FindJob("job-1")->last_observed_sha == "sha-b");
}

TEST_CASE("/loop 导入:产 receipt;同来源幂等不双跑;凭 receipt 反查") {
    AutomationStore store;
    REQUIRE(AutomationStore::Open(&store, FreshLog("importloop")).ok);
    auto receipt = store.ImportLoop("sess-abc", "loop-2", "每天巡检仓库", 600, 1000, "imp-1");
    CHECK(receipt.accepted);
    CHECK(receipt.receipt_id.rfind("imp-", 0) == 0);
    CHECK(receipt.job_id == "job-1");
    // 建出的是 interval job,prompt 固定副本。
    const auto job = store.FindJob("job-1");
    REQUIRE(job.has_value());
    CHECK(job->schedule_kind == ScheduleKind::Interval);
    CHECK(job->interval_seconds == 600);
    CHECK(job->imported_from == "loop:sess-abc:loop-2");
    // 同来源再导:回原 receipt,不建第二个 job。
    receipt = store.ImportLoop("sess-abc", "loop-2", "每天巡检仓库", 600, 2000, "");
    CHECK(receipt.duplicate);
    CHECK(receipt.receipt_id == store.FindLoopImport("sess-abc", "loop-2")->receipt_id);
    CHECK(store.ListJobs().size() == 1);
    CHECK(store.ListLoopImports().size() == 1);
    // 无 receipt 的来源查不到:Gateway 不暗搬,账上没有就是没导过。
    CHECK_FALSE(store.FindLoopImport("sess-other", "loop-1").has_value());
}

TEST_CASE("重放:周期语义全量回放后状态与关前一致(账是唯一真源)") {
    const auto log = FreshLog("replay");
    std::string occurrence_id;
    {
        AutomationStore store;
        REQUIRE(AutomationStore::Open(&store, log).ok);
        AutomationStore::JobSpec spec = IntervalSpec("巡检", 60);
        spec.notify_on_change = true;
        spec.deadline_ms = 900000;
        REQUIRE(store.CreateJob(spec, 1000, "").accepted);
        REQUIRE(store.SweepSchedule(181000).generated == 1);
        REQUIRE(store.SweepSchedule(301000).merged == 2);
        occurrence_id = MakeOccurrenceId("job-1", 1, 61000);
        REQUIRE(store.ClaimDue("epoch-a", 302000).has_value());
        REQUIRE(store.RecordObservation(occurrence_id, "sha-x", true, true, true, 303000));
        REQUIRE(store.PauseJob("job-1", 1, 304000, "").accepted);
        REQUIRE(store.ResumeJob("job-1", 1, 400000, "").accepted);
        REQUIRE(store.UpdateJob("job-1", 1, [] {
                    AutomationStore::JobUpdatePatch patch;
                    patch.set_interval = true;
                    patch.interval_seconds = 120;
                    return patch;
                }(),
                          500000, "")
                    .accepted);
        REQUIRE(store.ImportLoop("sess-abc", "loop-2", "导入的活", 600, 600000, "").accepted);
        REQUIRE(store.CancelJob("job-2", 1, 700000, "").accepted);
    }
    {
        AutomationStore store;
        const auto open = AutomationStore::Open(&store, log);
        REQUIRE(open.ok);
        CHECK(open.skipped_lines == 0);
        const auto job = store.FindJob("job-1");
        REQUIRE(job.has_value());
        CHECK(job->revision == 2);  // 建 1 + update 2(pause/resume 不动 revision)
        CHECK(job->interval_seconds == 120);
        CHECK(job->anchor_ms == 500000);
        CHECK(job->schedule_cursor_ms == 500000);
        CHECK(job->state == AutomationJobState::Active);
        CHECK(job->notify_on_change);
        CHECK(job->deadline_ms == 900000);
        CHECK(job->last_observed_sha == "sha-x");
        const auto occurrence = store.FindOccurrence(occurrence_id);
        REQUIRE(occurrence.has_value());
        CHECK(occurrence->state == AutomationOccurrence::State::Claimed);
        CHECK(occurrence->missed_count == 4);
        CHECK(occurrence->observed_sha == "sha-x");
        CHECK(occurrence->observed_delivered);
        // 导入 receipt 与来源标记。
        const auto receipt = store.FindLoopImport("sess-abc", "loop-2");
        REQUIRE(receipt.has_value());
        CHECK(store.FindJob(receipt->job_id)->imported_from == "loop:sess-abc:loop-2");
        // 取消的终态与幂等。
        const auto cancelled_job = store.FindJob("job-2");
        REQUIRE(cancelled_job.has_value());
        CHECK(cancelled_job->state == AutomationJobState::Cancelled);
        CHECK(store.CancelJob("job-2", 1, 800000, "").duplicate);
        // 只读投影同源。
        const auto projection = ReadAutomationProjection(log);
        CHECK(projection.jobs.size() == 2);
        CHECK(projection.loop_imports.size() == 1);
    }
}
