// 子代理进展合同(《子代理监督器、agent_watch 与停滞恢复设计》P0-0):把
// "观察活、传输活、执行活、语义进展"四本账分开记,谁也不冒充谁。
//
// 铁律(单子 §五):心跳只证明观察链还活,不刷 last_meaningful_progress_at;
// thinking/text 的 token 只算 transport activity,完整 assistant 消息提交后
// 才算一次 meaningful progress——免得"流一直吐套话,永不超时"。反过来,
// 长工具静默跑测试时 transport 不动也不算坏,那是 RunningTool 相位的事。
//
// 本文件只放数据形状与纯函数(判定尺 EvaluateHealth 可注入假钟单测);
// 写口在 TaskLedger(RecordXxx 系列),驱动在 AgentSupervisor。
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace lubancode::agent {

// 监督相位(单子 §6.2):比显示层的 AgentTaskActivity::Stage 细,显示层
// 再折短。Terminal 由收尾翻,Queued/Preparing 只在注册前短暂存在。
enum class AgentSupervisionStage {
    Queued,
    Preparing,
    AwaitingFirstByte,
    StreamingThinking,
    StreamingText,
    AwaitingToolInputComplete,
    RunningTool,
    AwaitingNextModelTurn,
    WaitingChildren,
    Recovering,
    Completing,
    Terminal,
};

// 稳定码(agent_watch/事件/遥测共用,蛇形小写)。
const char* SupervisionStageTag(AgentSupervisionStage stage);
// 人话短标签(通知/查看态用,中文)。
std::string SupervisionStageLabel(AgentSupervisionStage stage);

// 健康态(单子 §七):Supervisor 另挂的状态机,不改 AgentTaskState 的业务
// 终态。Recovering 由请求恢复层(P0-1)置,Healthy 由新进展回;Terminal
// 随收尾翻。
enum class AgentHealthState {
    Healthy,
    Quiet,
    SuspectTransport,
    SuspectTool,
    SuspectAgent,
    Recovering,
    Degraded,
    Terminal,
};

const char* HealthTag(AgentHealthState health);
std::string HealthLabel(AgentHealthState health);

// 四本时钟 + 恢复账(单子 §6.1/§8.1 的落地)。全部 steady_clock,睡眠跳变
// 由监督拍另行甄别(HostResumeSuspected),不在钟上做手脚。
struct AgentProgressClock {
    std::chrono::steady_clock::time_point last_observed_at{};
    std::chrono::steady_clock::time_point last_transport_at{};
    std::chrono::steady_clock::time_point last_execution_at{};
    std::chrono::steady_clock::time_point last_meaningful_progress_at{};

    std::chrono::steady_clock::time_point request_started_at{};
    std::optional<std::chrono::steady_clock::time_point> first_stream_event_at;
    std::optional<std::chrono::steady_clock::time_point> tool_started_at;

    std::uint64_t progress_revision = 0;  // 监督/agent_watch 用,与显示的 content_revision 分家
    std::uint64_t transport_revision = 0;
    std::string progress_fingerprint;

    // ---- 请求级恢复账(P0-1)----
    int request_attempt = 0;    // 当前逻辑请求的第几次尝试(1 起)
    int retry_count = 0;        // 整只任务累计自动重试次数
    std::string last_reason_code;  // 最近一枚稳定错误码(空 = 没错过)
    // 断流重试时的显示回滚锚:本次请求起跑那刻 live_output 的长度。重试
    // 时截回这里,不拼两段半截正文(单子 §8.3)。
    std::size_t live_output_mark = 0;

    // ---- 轮次账(空转判据)----
    // 连续"指纹没变"的完整轮数:完整 assistant 消息提交与工具收口都会对
    // 指纹;新指纹归零,旧指纹 +1。2 轮提醒、3 轮 NoMeaningfulProgress
    //(单子 §7.1 尺子)。
    int stale_rounds = 0;
    // 提醒是否已投(host notice 只投一次,自捎上下文后仍空转才升级)。
    bool host_notice_sent = false;

    // ---- 监督相位与健康(由 TaskRecord 持有的这份是真账)----
    AgentSupervisionStage stage = AgentSupervisionStage::Queued;
    AgentHealthState health = AgentHealthState::Healthy;
    std::uint64_t health_epoch = 0;  // 每翻一次健康 +1;通知按 epoch 去重
};

// 默认尺子(单子 §7.1,初稿;实施校准归 P2 真机统计)。硬线沿既有合同:
// 首字节/connect 超时沿请求级超时,流静默沿 stream_idle_timeout_secs,
// 总墙钟沿 SetWallClockTimeout——软线只管"显黄/提醒",不另立杀尺。
struct SupervisionThresholds {
    int first_byte_soft_secs = 20;   // 等首字节显黄
    int streaming_soft_secs = 30;    // 流式静默显黄
    int tool_soft_secs = 120;        // 工具静默显黄(无声明时限的工具只说静默,不说卡死)
    int exec_idle_soft_secs = 120;   // 等下一轮/等孩子的执行静默显黄
    int stale_notice_rounds = 2;     // 连续空转 N 轮投 host notice
    int stale_fail_rounds = 3;       // 连续空转 N 轮 NoMeaningfulProgress 收口
    // 总墙钟软线百分比(80% 显黄)。
    int wall_soft_percent = 80;
};

// 监督拍对一只活任务的只读视景(从 TaskRecord 在台账锁内拷出)。
struct TaskVitals {
    AgentSupervisionStage stage = AgentSupervisionStage::Queued;
    AgentHealthState health = AgentHealthState::Healthy;
    std::chrono::steady_clock::time_point now{};
    std::chrono::steady_clock::time_point task_started_at{};
    std::chrono::steady_clock::time_point request_started_at{};
    std::chrono::steady_clock::time_point last_transport_at{};
    std::chrono::steady_clock::time_point last_execution_at{};
    std::chrono::steady_clock::time_point last_meaningful_progress_at{};
    std::optional<std::chrono::steady_clock::time_point> tool_started_at;
    bool has_transport = false;  // 从未收到过传输字节
    bool has_execution = false;
    bool has_progress = false;
    bool cancel_requested = false;
    int stale_rounds = 0;
    bool host_notice_sent = false;
    int wall_limit_secs = 0;  // 0 = 不设
    std::uint64_t progress_revision = 0;  // P2:AgentSupervisionEvent 的共同字段
    int request_attempt = 0;
    std::string reason_code;  // 最近一枚稳定错误码(事件里带上,不留正文)
};

// 监督事件(P2,单子 §11.1 的共同字段):健康翻页、恢复起讫、工具结果
// 不明、强收——宿主侧只读 hook(AgentHealthChanged 那一路)与指标计数共
// 用这份形状。只带 id/枚举/计数/时长与稳定码,不带 thinking、正文、Secret
// 与完整工具参数(单子 §五·11)。
enum class AgentSupervisionEventKind {
    HealthChanged,       // 健康翻页(含 Quiet/Suspect*/Recovered/Terminal)
    RecoveryStarted,     // 请求重试决定(退避后重发)
    RecoverySucceeded,   // 重试后传输侧自愈
    RecoveryExhausted,   // 重试链用尽,按错误收口
    ToolIndeterminate,   // 工具被取消后结果不明(不自动重跑)
    ForceFinalized,      // 墙钟/空转强收
};

const char* SupervisionEventTag(AgentSupervisionEventKind kind);

struct AgentSupervisionEvent {
    AgentSupervisionEventKind kind = AgentSupervisionEventKind::HealthChanged;
    int task_id = 0;
    int parent_task_id = 0;
    int root_task_id = 0;
    AgentSupervisionStage stage = AgentSupervisionStage::Queued;
    AgentHealthState old_health = AgentHealthState::Healthy;
    AgentHealthState new_health = AgentHealthState::Healthy;
    std::string reason_code;  // 稳定码;工具不明事件带工具名前缀
    std::uint64_t progress_revision = 0;
    int attempt = 0;
    std::int64_t elapsed_ms = 0;
    std::int64_t transport_idle_ms = -1;  // -1 = 从未有过传输
    std::int64_t progress_idle_ms = -1;   // -1 = 从未有过实质进展
};

// 监督拍的裁决动作(单子 §七的流转,纯函数可假钟单测)。
enum class SupervisionAction {
    None,            // 没新事
    MarkQuiet,       // 越阶段软线,显黄(只翻健康,不动任务)
    MarkSuspectTransport,
    MarkSuspectTool,
    MarkSuspectAgent,
    Recovered,       // 原先 Quiet/Suspect*,此刻见新进展
    HostNotice,      // 空转到提醒线:投一枚 host notice 给一轮自救上下文
    StopNoProgress,  // 空转到收口线:发停止信号,按 NoMeaningfulProgress 收账
};

struct SupervisionVerdict {
    SupervisionAction action = SupervisionAction::None;
    AgentHealthState new_health = AgentHealthState::Healthy;
    std::string reason_code;  // 稳定码,如 "transport.first_byte_quiet"
};

// 一次监督拍的判定(单子 §7.1 尺子 + §7.2 睡眠甄别)。host_resume_suspected
// 为真时不判 SuspectAgent——睡眠跨度不许记到 Agent 头上;软线照算(网络
// 断了该黄还是黄)。Recovering/Degraded/Terminal 是别的层翻的态,这里只
// 认活态任务的升/降级。
SupervisionVerdict EvaluateSupervision(const TaskVitals& vitals, const SupervisionThresholds& thresholds,
                                       bool host_resume_suspected);

// 进展指纹的散列原料:模型消息按块折哈希(文本取内容、tool_use 取名+入参,
// 不掺每轮必变的 tool_use id),工具结果取名+结果——"同一只工具反复读同
// 一份内容"指纹不变,"读了新东西"指纹必变。只留短哈希,不留正文。
std::string FingerprintOfParts(const std::string& a, const std::string& b);

}  // namespace lubancode::agent
