// 进展合同(监督器单 P0-0)的数据层测试:四本时钟各自刷新、互不串账;
// keepalive/token 流量只抬 transport 不抬 progress;完整消息、工具收口、
// 孩子交付抬 progress;重复相同指纹连计空转轮,新成果归零;判定尺
// (EvaluateSupervision)用注入时钟钉各相位的软线与睡眠甄别。
// 台账写口顺带钉:content_revision 与 progress_revision 分家、重试回滚
// 不拼两段正文。

#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "agent/agent_progress.hpp"
#include "tools/task_ledger.hpp"

using namespace lubancode;
using agent::AgentHealthState;
using agent::AgentSupervisionStage;

namespace {

using Clock = std::chrono::steady_clock;

// 造一只进台账的任务(不走 admission 的旧口,测试直用)。
std::shared_ptr<tools::TaskRecord> MakeTask(tools::TaskLedger& ledger, const std::string& title) {
    tools::AgentTaskSnapshot snapshot;
    snapshot.title = title;
    snapshot.prompt = "test";
    snapshot.start_time = Clock::now();
    return ledger.Register(std::move(snapshot));
}

}  // namespace

TEST_CASE("四本时钟:token 流量只抬 transport,完整消息才抬 progress") {
    tools::TaskLedger ledger;
    const auto task = MakeTask(ledger, "时钟");
    const auto& before = ledger.ProgressOf(task->snapshot.id);

    ledger.RecordRequestStarted(task, 1, "hash-1");
    ledger.RecordTransportActivity(task);  // 一枚 SSE 帧(纯 token 流量)
    ledger.RecordTransportActivity(task);
    const auto& after_transport = ledger.ProgressOf(task->snapshot.id);
    CHECK(after_transport.transport_revision == before.transport_revision + 2);
    CHECK(after_transport.last_transport_at.time_since_epoch().count() != 0);
    // 铁律(单子 §6.1):transport 活动不许冒充进展。
    CHECK(after_transport.progress_revision == before.progress_revision);
    CHECK(after_transport.last_meaningful_progress_at.time_since_epoch().count() == 0);

    // 完整 assistant 消息提交:新指纹,一次 meaningful progress。
    ledger.RecordAssistantMessage(task, agent::FingerprintOfParts("msg", "第一轮内容"));
    const auto& after_message = ledger.ProgressOf(task->snapshot.id);
    CHECK(after_message.progress_revision == before.progress_revision + 1);
    CHECK(after_message.last_meaningful_progress_at.time_since_epoch().count() != 0);
    CHECK(after_message.stale_rounds == 0);
    CHECK(after_message.stage == AgentSupervisionStage::AwaitingToolInputComplete);
}

TEST_CASE("四本时钟:工具起止抬 execution,收口按结果指纹判进展") {
    tools::TaskLedger ledger;
    const auto task = MakeTask(ledger, "工具账");
    {
        std::lock_guard<std::mutex> lock(ledger.mutex);
        ledger.RecordToolStartedLocked(task);
    }
    const auto& started = ledger.ProgressOf(task->snapshot.id);
    CHECK(started.last_execution_at.time_since_epoch().count() != 0);
    CHECK(started.tool_started_at.has_value());
    CHECK(started.stage == AgentSupervisionStage::RunningTool);

    const std::string fp = agent::FingerprintOfParts("tool:read_file", "同一份结果");
    {
        std::lock_guard<std::mutex> lock(ledger.mutex);
        ledger.RecordToolCompletedLocked(task, fp);
        ledger.RecordToolCompletedLocked(task, fp);  // 同结果再来一次:不算新进展
    }
    const auto& completed = ledger.ProgressOf(task->snapshot.id);
    CHECK(completed.progress_revision == 1);
    CHECK(completed.stage == AgentSupervisionStage::AwaitingNextModelTurn);
    CHECK_FALSE(completed.tool_started_at.has_value());
}

TEST_CASE("空转计数:连续同指纹的完整轮 +1,新指纹归零") {
    tools::TaskLedger ledger;
    const auto task = MakeTask(ledger, "空转");
    const std::string same = agent::FingerprintOfParts("msg", "一模一样的话");
    ledger.RecordAssistantMessage(task, same);
    ledger.RecordAssistantMessage(task, same);
    ledger.RecordAssistantMessage(task, same);
    CHECK(ledger.ProgressOf(task->snapshot.id).stale_rounds == 2);  // 首笔算新,后两笔空转

    ledger.RecordAssistantMessage(task, agent::FingerprintOfParts("msg", "换了新说法"));
    CHECK(ledger.ProgressOf(task->snapshot.id).stale_rounds == 0);
    CHECK(ledger.ProgressOf(task->snapshot.id).progress_revision == 2);
}

TEST_CASE("请求级恢复账:重试回滚半截显示账,不拼两段正文") {
    tools::TaskLedger ledger;
    const auto task = MakeTask(ledger, "断流重试");
    {
        std::lock_guard<std::mutex> lock(ledger.mutex);
        task->snapshot.live_output = "任务前言";  // 上一轮已提交的正文
    }
    ledger.RecordRequestStarted(task, 1, "hash-1");  // 起跑锚记在这截之后
    // 本轮起跑后,半截正文流进了显示账(pending/live_output)。
    {
        std::lock_guard<std::mutex> lock(ledger.mutex);
        task->pending_text = "半截正文";
        task->pending_reasoning = "半截思考";
        task->snapshot.live_output += "半截正文";
    }
    ledger.RecordRequestRetry(task, 2, "network.error");
    const auto& rolled = ledger.ProgressOf(task->snapshot.id);
    CHECK(rolled.retry_count == 1);
    CHECK(rolled.request_attempt == 2);
    CHECK(rolled.last_reason_code == "network.error");
    CHECK(rolled.stage == AgentSupervisionStage::Recovering);
    CHECK(rolled.health == AgentHealthState::Recovering);
    {
        std::lock_guard<std::mutex> lock(ledger.mutex);
        CHECK(task->pending_text.empty());
        CHECK(task->pending_reasoning.empty());
        // live_output 截回起跑锚:只留已提交的部分,半截正文不重复拼。
        CHECK(task->snapshot.live_output == "任务前言");
    }
    // 第二次尝试成功:健康回正常(Recovering -> Healthy)。
    ledger.RecordRequestOutcome(task, true, std::string());
    CHECK(ledger.ProgressOf(task->snapshot.id).health == AgentHealthState::Healthy);
}

TEST_CASE("判定尺:等首字节越软线判 SuspectTransport,不冒充工具或 Agent") {
    agent::SupervisionThresholds thresholds;
    thresholds.first_byte_soft_secs = 20;
    agent::TaskVitals v;
    v.stage = AgentSupervisionStage::AwaitingFirstByte;
    v.now = Clock::now();
    v.request_started_at = v.now - std::chrono::seconds(21);
    const auto verdict = agent::EvaluateSupervision(v, thresholds, /*host_resume_suspected=*/false);
    CHECK(verdict.action == agent::SupervisionAction::MarkSuspectTransport);
    CHECK(verdict.new_health == AgentHealthState::SuspectTransport);
    CHECK(verdict.reason_code == std::string("transport.first_byte_quiet"));

    // 软线之内:无事。
    v.request_started_at = v.now - std::chrono::seconds(5);
    CHECK(agent::EvaluateSupervision(v, thresholds, false).action == agent::SupervisionAction::None);
}

TEST_CASE("判定尺:流式静默按 transport 龄判,SSE keepalive 到货即回 Healthy") {
    agent::SupervisionThresholds thresholds;
    thresholds.streaming_soft_secs = 30;
    agent::TaskVitals v;
    v.now = Clock::now();
    v.stage = AgentSupervisionStage::StreamingText;
    v.has_transport = true;
    v.has_progress = true;
    v.last_transport_at = v.now - std::chrono::seconds(31);
    auto verdict = agent::EvaluateSupervision(v, thresholds, false);
    CHECK(verdict.action == agent::SupervisionAction::MarkSuspectTransport);

    // 31 秒没字节之后又收到帧(transport 活了),原本在 watch 态 -> 回 Healthy。
    v.health = AgentHealthState::SuspectTransport;
    v.last_transport_at = v.now;
    verdict = agent::EvaluateSupervision(v, thresholds, false);
    CHECK(verdict.action == agent::SupervisionAction::Recovered);
    CHECK(verdict.new_health == AgentHealthState::Healthy);
}

TEST_CASE("判定尺:工具静默判 SuspectTool,只说静默不动手") {
    agent::SupervisionThresholds thresholds;
    thresholds.tool_soft_secs = 120;
    agent::TaskVitals v;
    v.now = Clock::now();
    v.stage = AgentSupervisionStage::RunningTool;
    v.tool_started_at = v.now - std::chrono::seconds(121);
    const auto verdict = agent::EvaluateSupervision(v, thresholds, false);
    CHECK(verdict.action == agent::SupervisionAction::MarkSuspectTool);
    CHECK(verdict.new_health == AgentHealthState::SuspectTool);
    CHECK(verdict.reason_code == std::string("tool.silent"));
}

TEST_CASE("判定尺:空转两轮投提醒,三轮收口;睡眠甄别压住不误判") {
    agent::SupervisionThresholds thresholds;
    thresholds.stale_notice_rounds = 2;
    thresholds.stale_fail_rounds = 3;
    agent::TaskVitals v;
    v.now = Clock::now();
    v.stage = AgentSupervisionStage::AwaitingNextModelTurn;
    v.has_execution = true;
    v.last_execution_at = v.now;  // 执行不静默:只看空转计数

    v.stale_rounds = 2;
    auto verdict = agent::EvaluateSupervision(v, thresholds, false);
    CHECK(verdict.action == agent::SupervisionAction::HostNotice);
    CHECK(verdict.new_health == AgentHealthState::SuspectAgent);

    v.stale_rounds = 3;
    verdict = agent::EvaluateSupervision(v, thresholds, false);
    CHECK(verdict.action == agent::SupervisionAction::StopNoProgress);
    CHECK(verdict.reason_code == std::string("agent.no_meaningful_progress"));

    // 宿主刚醒(长 tick):同一空转计数不判 SuspectAgent——睡眠跨度不许记
    // 到 Agent 头上(单子 §7.2)。
    v.stale_rounds = 3;
    verdict = agent::EvaluateSupervision(v, thresholds, /*host_resume_suspected=*/true);
    CHECK(verdict.action != agent::SupervisionAction::StopNoProgress);
    CHECK(verdict.action != agent::SupervisionAction::HostNotice);

    // 已投过提醒:不重复投,只等收口线。
    v.stale_rounds = 2;
    v.host_notice_sent = true;
    verdict = agent::EvaluateSupervision(v, thresholds, false);
    CHECK(verdict.action != agent::SupervisionAction::HostNotice);
}

TEST_CASE("判定尺:收口相位不判静默,墙钟软线只升黄不换因") {
    agent::SupervisionThresholds thresholds;
    agent::TaskVitals v;
    v.now = Clock::now();
    v.stage = AgentSupervisionStage::Completing;
    v.task_started_at = v.now - std::chrono::seconds(1000);
    v.wall_limit_secs = 1000;
    CHECK(agent::EvaluateSupervision(v, thresholds, false).action == agent::SupervisionAction::None);
}

TEST_CASE("健康翻页:epoch 递增,通知按代际去重的地基") {
    tools::TaskLedger ledger;
    const auto task = MakeTask(ledger, "健康");
    const std::uint64_t epoch1 = ledger.ApplyHealth(task, AgentHealthState::SuspectTransport, "transport.stream_quiet");
    CHECK(epoch1 != 0);
    const std::uint64_t epoch_same = ledger.ApplyHealth(task, AgentHealthState::SuspectTransport, "again");
    CHECK(epoch_same == 0);  // 同态不重复翻
    const std::uint64_t epoch2 = ledger.ApplyHealth(task, AgentHealthState::Healthy, "progress.resumed");
    CHECK(epoch2 > epoch1);
}
