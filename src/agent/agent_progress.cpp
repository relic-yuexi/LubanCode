// agent_progress.hpp 的实现:标签查表 + 监督拍判定 + 指纹散列。
#include "agent/agent_progress.hpp"

#include <algorithm>

#include "hooks/hash.hpp"  // Sha256Hex:指纹只留短哈希,不留正文

namespace lubancode::agent {

const char* SupervisionStageTag(AgentSupervisionStage stage) {
    switch (stage) {
        case AgentSupervisionStage::Queued:
            return "queued";
        case AgentSupervisionStage::Preparing:
            return "preparing";
        case AgentSupervisionStage::AwaitingFirstByte:
            return "awaiting_first_byte";
        case AgentSupervisionStage::StreamingThinking:
            return "streaming_thinking";
        case AgentSupervisionStage::StreamingText:
            return "streaming_text";
        case AgentSupervisionStage::AwaitingToolInputComplete:
            return "awaiting_tool_input_complete";
        case AgentSupervisionStage::RunningTool:
            return "running_tool";
        case AgentSupervisionStage::AwaitingNextModelTurn:
            return "awaiting_next_model_turn";
        case AgentSupervisionStage::WaitingChildren:
            return "waiting_children";
        case AgentSupervisionStage::Recovering:
            return "recovering";
        case AgentSupervisionStage::Completing:
            return "completing";
        case AgentSupervisionStage::Terminal:
            return "terminal";
    }
    return "unknown";
}

std::string SupervisionStageLabel(AgentSupervisionStage stage) {
    switch (stage) {
        case AgentSupervisionStage::Queued:
            return "排队中";
        case AgentSupervisionStage::Preparing:
            return "准备中";
        case AgentSupervisionStage::AwaitingFirstByte:
            return "等首字节";
        case AgentSupervisionStage::StreamingThinking:
            return "思考流";
        case AgentSupervisionStage::StreamingText:
            return "正文流";
        case AgentSupervisionStage::AwaitingToolInputComplete:
            return "等工具参数收齐";
        case AgentSupervisionStage::RunningTool:
            return "跑工具";
        case AgentSupervisionStage::AwaitingNextModelTurn:
            return "等下一轮";
        case AgentSupervisionStage::WaitingChildren:
            return "等子任务";
        case AgentSupervisionStage::Recovering:
            return "恢复中";
        case AgentSupervisionStage::Completing:
            return "收口中";
        case AgentSupervisionStage::Terminal:
            return "已收场";
    }
    return "";
}

const char* HealthTag(AgentHealthState health) {
    switch (health) {
        case AgentHealthState::Healthy:
            return "healthy";
        case AgentHealthState::Quiet:
            return "quiet";
        case AgentHealthState::SuspectTransport:
            return "suspect_transport";
        case AgentHealthState::SuspectTool:
            return "suspect_tool";
        case AgentHealthState::SuspectAgent:
            return "suspect_agent";
        case AgentHealthState::Recovering:
            return "recovering";
        case AgentHealthState::Degraded:
            return "degraded";
        case AgentHealthState::Terminal:
            return "terminal";
    }
    return "unknown";
}

std::string HealthLabel(AgentHealthState health) {
    switch (health) {
        case AgentHealthState::Healthy:
            return "正常";
        case AgentHealthState::Quiet:
            return "静默";
        case AgentHealthState::SuspectTransport:
            return "疑似断流";
        case AgentHealthState::SuspectTool:
            return "疑似工具卡";
        case AgentHealthState::SuspectAgent:
            return "疑似空转";
        case AgentHealthState::Recovering:
            return "恢复中";
        case AgentHealthState::Degraded:
            return "降级";
        case AgentHealthState::Terminal:
            return "已收场";
    }
    return "";
}

const char* SupervisionEventTag(AgentSupervisionEventKind kind) {
    switch (kind) {
        case AgentSupervisionEventKind::HealthChanged:
            return "agent.health.changed";
        case AgentSupervisionEventKind::RecoveryStarted:
            return "agent.recovery.started";
        case AgentSupervisionEventKind::RecoverySucceeded:
            return "agent.recovery.succeeded";
        case AgentSupervisionEventKind::RecoveryExhausted:
            return "agent.recovery.exhausted";
        case AgentSupervisionEventKind::ToolIndeterminate:
            return "agent.tool.indeterminate";
        case AgentSupervisionEventKind::ForceFinalized:
            return "agent.force_finalized";
    }
    return "agent.supervision.unknown";
}

namespace {

bool OlderThan(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point at, int soft_secs) {
    if (at.time_since_epoch().count() == 0) {
        return false;  // 从未发生过:没有龄可算,不算静默
    }
    return now - at >= std::chrono::seconds(soft_secs);
}

// 总墙钟软线(单子 §7.1"整只任务 80% 显黄"):只在设了上限时看。
bool PastWallSoftLine(const TaskVitals& v, const SupervisionThresholds& t) {
    if (v.wall_limit_secs <= 0 || v.task_started_at.time_since_epoch().count() == 0) {
        return false;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(v.now - v.task_started_at).count();
    return elapsed * 100 >= static_cast<std::int64_t>(v.wall_limit_secs) * t.wall_soft_percent;
}

}  // namespace

SupervisionVerdict EvaluateSupervision(const TaskVitals& v, const SupervisionThresholds& t,
                                       bool host_resume_suspected) {
    SupervisionVerdict out;
    // 先看恢复:原先不在 Healthy(且不是别的层管的 Recovering/Degraded),
    // 此刻有新进展就回 Healthy(单子 §七"Quiet -> 新进展 -> Healthy")。
    const bool has_any_progress = v.has_progress;
    const bool was_watch_state = v.health == AgentHealthState::Quiet || v.health == AgentHealthState::SuspectTransport ||
                                 v.health == AgentHealthState::SuspectTool ||
                                 v.health == AgentHealthState::SuspectAgent;
    // 各相位分别量尺(单子 §7.1:不用一枚尺子横切所有阶段)。
    bool quiet = false;
    const char* reason = "";
    switch (v.stage) {
        case AgentSupervisionStage::Queued:
        case AgentSupervisionStage::Preparing:
        case AgentSupervisionStage::Completing:
        case AgentSupervisionStage::Terminal:
            quiet = false;  // 收口相位不判静默——它本来就该安静
            break;
        case AgentSupervisionStage::AwaitingFirstByte:
            quiet = OlderThan(v.now, v.request_started_at, t.first_byte_soft_secs);
            reason = "transport.first_byte_quiet";
            break;
        case AgentSupervisionStage::StreamingThinking:
        case AgentSupervisionStage::StreamingText:
        case AgentSupervisionStage::AwaitingToolInputComplete:
            quiet = v.has_transport && OlderThan(v.now, v.last_transport_at, t.streaming_soft_secs);
            reason = "transport.stream_quiet";
            break;
        case AgentSupervisionStage::Recovering:
            // 请求恢复链正在按既定预算退避,长等待是健康的 Recovering,
            // 不能拿流静默尺误判成 SuspectTransport。
            quiet = false;
            break;
        case AgentSupervisionStage::RunningTool:
            // 没有进度回调的工具,宿主只说"静默",不说"卡死"(单子 §6.2);
            // 杀尺归工具自己的 timeout 与总墙钟,这里不越权。
            quiet = v.tool_started_at.has_value() && OlderThan(v.now, *v.tool_started_at, t.tool_soft_secs);
            reason = "tool.silent";
            break;
        case AgentSupervisionStage::AwaitingNextModelTurn:
        case AgentSupervisionStage::WaitingChildren:
            quiet = v.has_execution && OlderThan(v.now, v.last_execution_at, t.exec_idle_soft_secs);
            reason = "exec.idle";
            break;
    }

    // 空转判据(指纹多轮不变)压过相位静默:这是"Agent 侧"的病,不是传输
    // 侧的病。睡眠甄别在前:host 刚醒不判 SuspectAgent(单子 §7.2)。
    const bool stale_over_limit = !host_resume_suspected && v.stale_rounds >= t.stale_fail_rounds;
    const bool stale_over_notice = !host_resume_suspected && v.stale_rounds >= t.stale_notice_rounds;

    if (stale_over_limit) {
        // 收口线:已给过自救上下文仍空转 -> 停止信号 + NoMeaningfulProgress。
        // 现场不丢:台账里已完成的工具结果/实时输出随 CheckpointFallback 带走,
        // 隔离房按既有规矩保留(有改动不删)。
        out.action = SupervisionAction::StopNoProgress;
        out.new_health = AgentHealthState::SuspectAgent;
        out.reason_code = "agent.no_meaningful_progress";
        return out;
    }
    if (stale_over_notice && !v.host_notice_sent) {
        out.action = SupervisionAction::HostNotice;
        out.new_health = AgentHealthState::SuspectAgent;
        out.reason_code = "agent.stale_fingerprint";
        return out;
    }

    if (quiet) {
        // 分层归因(单子铁律:先判哪一层断了,再动哪一层):等首字节/流静默
        // 是传输侧;工具静默是执行侧;都不动任务本体。
        const bool transport_side = v.stage == AgentSupervisionStage::AwaitingFirstByte ||
                                    v.stage == AgentSupervisionStage::StreamingThinking ||
                                    v.stage == AgentSupervisionStage::StreamingText ||
                                    v.stage == AgentSupervisionStage::AwaitingToolInputComplete ||
                                    v.stage == AgentSupervisionStage::Recovering;
        const bool tool_side = v.stage == AgentSupervisionStage::RunningTool;
        // 墙钟软线只升黄,不换因。
        const bool wall_soft = PastWallSoftLine(v, t);
        if (tool_side) {
            out.action = SupervisionAction::MarkSuspectTool;
            out.new_health = AgentHealthState::SuspectTool;
        } else if (transport_side) {
            out.action = SupervisionAction::MarkSuspectTransport;
            out.new_health = AgentHealthState::SuspectTransport;
        } else {
            out.action = SupervisionAction::MarkQuiet;
            out.new_health = AgentHealthState::Quiet;
        }
        out.reason_code = wall_soft && std::string(reason) != "tool.silent" ? "wall_clock.soft_line" : reason;
        return out;
    }

    if (was_watch_state && has_any_progress) {
        out.action = SupervisionAction::Recovered;
        out.new_health = AgentHealthState::Healthy;
        out.reason_code = "progress.resumed";
        return out;
    }
    out.action = SupervisionAction::None;
    out.new_health = v.health;
    return out;
}

std::string FingerprintOfParts(const std::string& a, const std::string& b) {
    return hooks::Sha256Hex(a + "\x1f" + b).substr(0, 16);
}

}  // namespace lubancode::agent
