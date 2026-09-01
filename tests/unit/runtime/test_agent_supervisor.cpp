// AgentSupervisor(监督器单 P0-2)的单元测试:会话级单线程(100 只任务
// 只起 1 根监督线,不再每任务一根看门狗);健康拍按软线翻健康;通知按
// task+epoch+reason 去重;空转两轮投 host notice、三轮发停止信号并按
// NoMeaningfulProgress 强收(部分结果保留);终态/取消及时退场;墙钟期限
// 由同一根线落锤(迁移动作,与旧行为等价由 test_agent_activity 的墙钟用例
// 盯着)。

#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent_progress.hpp"
#include "runtime/agent_supervisor.hpp"
#include "tools/task_ledger.hpp"

using namespace lubancode;

namespace {

using Clock = std::chrono::steady_clock;

std::shared_ptr<tools::TaskRecord> MakeTask(tools::TaskLedger& ledger, const std::string& title) {
    tools::AgentTaskSnapshot snapshot;
    snapshot.title = title;
    snapshot.prompt = "test";
    snapshot.start_time = Clock::now();
    return ledger.Register(std::move(snapshot));
}

// 测试尺:软线全部压到 1 秒上下,判定逻辑用注入的 vitals.now(纯函数)钉;
// 线程路径只验"单线程"与期限落锤。
agent::SupervisionThresholds FastThresholds() {
    agent::SupervisionThresholds thresholds;
    thresholds.first_byte_soft_secs = 1;
    thresholds.streaming_soft_secs = 1;
    thresholds.tool_soft_secs = 1;
    thresholds.exec_idle_soft_secs = 1;
    thresholds.stale_notice_rounds = 2;
    thresholds.stale_fail_rounds = 3;
    return thresholds;
}

}  // namespace

TEST_CASE("单线程验收:100 只任务登记,监督线只有 1 根") {
    tools::TaskLedger ledger;
    runtime::AgentSupervisor supervisor(ledger);
    supervisor.SetThresholds(FastThresholds());
    std::vector<std::shared_ptr<tools::TaskRecord>> tasks;
    for (int i = 0; i < 100; ++i) {
        const auto task = MakeTask(ledger, "任务" + std::to_string(i));
        supervisor.WatchTask(task);
        tasks.push_back(task);
    }
    CHECK(supervisor.supervisor_thread_count_for_test() == 1);
    // 墙钟也登记:仍是同一根线。
    supervisor.ArmWallClock(tasks.front(), 60, 30);
    CHECK(supervisor.supervisor_thread_count_for_test() == 1);
    supervisor.RequestStop();
}

TEST_CASE("健康拍:等首字节越软线翻 SuspectTransport,通知去重只弹一次") {
    tools::TaskLedger ledger;
    runtime::AgentSupervisor supervisor(ledger);
    supervisor.SetThresholds(FastThresholds());
    const auto task = MakeTask(ledger, "首字节慢");
    supervisor.WatchTask(task);
    ledger.RecordRequestStarted(task, 1, "hash");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    supervisor.TickHealthForTest();
    CHECK(ledger.ProgressOf(task->snapshot.id).health == agent::AgentHealthState::SuspectTransport);
    // 第二拍:同因同代际,不重复弹。
    const auto first_notices = ledger.TakeSupervisorNotices();
    REQUIRE(first_notices.size() == 1);
    CHECK(first_notices[0].find("疑似断流") != std::string::npos);
    supervisor.TickHealthForTest();
    CHECK(ledger.TakeSupervisorNotices().empty());
    // 传输活了且有新的实质进展(完整消息提交,指纹变了):回 Healthy,弹一条恢复短报。
    ledger.RecordTransportActivity(task);
    ledger.RecordAssistantMessage(task, agent::FingerprintOfParts("msg", "恢复后的新内容"));
    supervisor.TickHealthForTest();
    CHECK(ledger.ProgressOf(task->snapshot.id).health == agent::AgentHealthState::Healthy);
    const auto recovered = ledger.TakeSupervisorNotices();
    REQUIRE(recovered.size() == 1);
    CHECK(recovered[0].find("恢复正常") != std::string::npos);
    supervisor.RequestStop();
}

TEST_CASE("空转流程:两轮投 host notice,三轮停止信号,宽限后强收留部分结果") {
    tools::TaskLedger ledger;
    runtime::AgentSupervisor supervisor(ledger);
    supervisor.SetThresholds(FastThresholds());
    supervisor.SetNoProgressGraceSecs(1);
    const auto task = MakeTask(ledger, "原地打转");
    supervisor.WatchTask(task);
    // 连续三轮同指纹:第一轮算新进展,后两轮空转 -> stale_rounds 到 2。
    const std::string same = agent::FingerprintOfParts("msg", "同样的回答");
    ledger.RecordAssistantMessage(task, same);
    ledger.RecordAssistantMessage(task, same);
    ledger.RecordAssistantMessage(task, same);
    {
        std::lock_guard<std::mutex> lock(ledger.mutex);
        ledger.RecordStageLocked(task, agent::AgentSupervisionStage::AwaitingNextModelTurn);
        ledger.RecordExecutionActivityLocked(task);
    }
    supervisor.TickHealthForTest();
    // 提醒进了 inbox(轮次边界注入),host_notice_sent 只投一次。
    REQUIRE_FALSE(ledger.PendingMessages(task->snapshot.id).empty());
    CHECK(ledger.PendingMessages(task->snapshot.id)[0].find("宿主监督提醒") != std::string::npos);
    CHECK(ledger.ProgressOf(task->snapshot.id).host_notice_sent);

    // 再空转一轮:停止信号(no_progress_fired + wall_stop,不置用户 cancel)。
    ledger.RecordAssistantMessage(task, same);
    supervisor.TickHealthForTest();
    CHECK(task->no_progress_fired.load());
    CHECK(task->wall_stop.load());
    CHECK_FALSE(task->cancel.load());

    // 台账里留了部分结果账(工具结果/实时输出),强收后仍带得走。
    {
        std::lock_guard<std::mutex> lock(ledger.mutex);
        task->snapshot.live_output = "已经干到一半的结论";
    }
    // 宽限 1s 后强收(真期限,由监督线落锤)。
    const auto deadline = Clock::now() + std::chrono::seconds(4);
    while (Clock::now() < deadline &&
           ledger.ProgressOf(task->snapshot.id).stage != agent::AgentSupervisionStage::Terminal) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto summaries = ledger.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].state == tools::AgentTaskState::Failed);
    CHECK(summaries[0].outcome_reason == tools::TaskOutcomeReason::NoMeaningfulProgress);
    const auto detail = ledger.Detail(task->snapshot.id);
    REQUIRE(detail.has_value());
    CHECK(detail->result.find("已经干到一半的结论") != std::string::npos);  // 部分成果保留
    CHECK_FALSE(ledger.TakeSupervisorNotices().empty());
    supervisor.RequestStop();
}

TEST_CASE("终态退场:收了账的任务不再进健康拍,deadline 落锤跳过已收口任务") {
    tools::TaskLedger ledger;
    runtime::AgentSupervisor supervisor(ledger);
    supervisor.SetThresholds(FastThresholds());
    const auto task = MakeTask(ledger, "先收场");
    supervisor.WatchTask(task);
    ledger.FinalizeFromToolResult(task, "结论", false);
    supervisor.TickHealthForTest();  // 不炸、不翻终态任务的账
    CHECK(ledger.ProgressOf(task->snapshot.id).health == agent::AgentHealthState::Terminal);
    CHECK(ledger.TakeSupervisorNotices().empty());
    supervisor.RequestStop();
}

TEST_CASE("睡眠甄别:长 tick 那拍不判空转(host_resume_suspected 压制)") {
    tools::TaskLedger ledger;
    runtime::AgentSupervisor supervisor(ledger);
    supervisor.SetThresholds(FastThresholds());
    const auto task = MakeTask(ledger, "合盖醒来");
    supervisor.WatchTask(task);
    const std::string same = agent::FingerprintOfParts("msg", "同样");
    ledger.RecordAssistantMessage(task, same);
    ledger.RecordAssistantMessage(task, same);
    ledger.RecordAssistantMessage(task, same);
    {
        std::lock_guard<std::mutex> lock(ledger.mutex);
        ledger.RecordStageLocked(task, agent::AgentSupervisionStage::AwaitingNextModelTurn);
        ledger.RecordExecutionActivityLocked(task);
    }
    supervisor.TickHealthForTest(/*host_resume_suspected=*/true);
    // 睡眠跨度不许记到 Agent 头上:停止信号与提醒都不发。
    CHECK_FALSE(task->no_progress_fired.load());
    CHECK(ledger.PendingMessages(task->snapshot.id).empty());
    supervisor.RequestStop();
}
