// `/goal` 持久目标(goal 单第 0/1 期):领域类型、状态机转换表、错误码。
//
// 单子的定案在这里落地:
//   - GoalTask/GoalContract/GoalBudget/GoalCheckpoint/GoalEvaluation/
//     GoalIteration/GoalEvidence 全在这一只头(纯数据,零 IO)。
//   - 合法状态转换集中成一张表,GoalCoordinator::Apply 是唯一写口;
//     CLI、TUI、Hook、模型工具都不许直改 GoalTask.state。
//   - 错误码是稳定字符串(线上与 UI 都用它,不拿中文正文当机器判断)。
//   - objective 上限按 Unicode 码点数(产品面写"最多 4,000 characters",
//     不拿 bytes 冒充);校验器 CLI 与 Runtime 共用这一只。
//
// 依赖铁律:只认标准库 + nlohmann/json,不 include cli/app/agent,不被
// 反向依赖——goal 状态机是最底层,谁都可以拿来用,它谁都不认。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::runtime::goal {

// ---------------------------------------------------------------------------
// 稳定枚举(线上是字符串,不是数字——数字重排就是存档破坏)
// ---------------------------------------------------------------------------

// 目标生命周期。terminal 状态(后四个 + Cleared)不自动复活;resume 只收
// Paused/AwaitingUser/Blocked。
enum class GoalState {
    Preparing,         // 已创建,合同未冻结(第 0 iteration 拟合同)
    Active,            // 合同已冻结,没有进行中的轮
    Running,           // 一枚 iteration 的执行轮在跑
    Evaluating,        // 执行轮收口,evaluator 在判
    Pausing,           // 收到 pause_requested,等安全边界收口
    Paused,            // 停排新 iteration(保留 checkpoint/budget/blocker 账)
    AwaitingApproval,  // 悬在工具审批上(不算无进展,不烧 iteration)
    AwaitingUser,      // 需要用户回答/选择(preflight 或 needs_user)
    Blocked,           // 同一 blocker 连续三轮且无实质进展;用户改条件后可 resume
    Achieved,          // 终点已验:criteria 全 pass + 新鲜证据
    BudgetExhausted,   // 预算见底(elapsed/iterations/token)
    SuspendedByPolicy, // 功能被关后 resume 读到 active goal 的落点:可查、不自动跑
    Failed,            // 存档写不落等宿主侧故障(fail closed)
    Cleared,           // 用户 clear:审计账不删,目标不再动
};
std::string ToString(GoalState state);
bool ParseGoalState(const std::string& s, GoalState& out);

bool IsGoalTerminal(GoalState state);
// resume 只收这三态(单子"terminal 状态不自动复活")。
bool IsGoalResumable(GoalState state);

// 一枚 iteration 的相位。
enum class GoalIterationPhase {
    Scheduled,     // 已排,主泵还没取
    Running,       // synthetic turn 在跑
    Checkpointed,  // 执行轮收口,checkpoint(可能合成)已有
    Evaluating,    // evaluator 在判
    Finished,      // 判词已落,iteration 收账
    Interrupted,   // 崩溃/pause 边界打断:审计保留,不重放副作用
};
std::string ToString(GoalIterationPhase phase);
bool ParseGoalIterationPhase(const std::string& s, GoalIterationPhase& out);

// checkpoint 的四档表态(执行模型只能申请,封不了账)。
enum class CheckpointStatus {
    Progress,            // 正常推进,还有活
    ReadyForEvaluation,  // 请宿主验收(不等于 achieved)
    Blocked,             // 碰墙(必须有 blocker_key)
    NeedsUser,           // 需要用户(必须有 question)
};
std::string ToString(CheckpointStatus status);
bool ParseCheckpointStatus(const std::string& s, CheckpointStatus& out);

// evaluator 的四路判词。
enum class GoalDecision { Continue, Achieved, Blocked, NeedsUser };
std::string ToString(GoalDecision decision);
bool ParseGoalDecision(const std::string& s, GoalDecision& out);

// 证据种类(宿主采,不信 assistant 自报)。
enum class EvidenceKind {
    ToolResult,
    CommandExit,
    TestReport,
    FileDigest,
    GitDiffSummary,
    Artifact,
    UserDecision,
    RuntimeError,
};
std::string ToString(EvidenceKind kind);
bool ParseEvidenceKind(const std::string& s, EvidenceKind& out);

// ---------------------------------------------------------------------------
// 错误码(稳定字符串,不拿中文正文当机器判断)
// ---------------------------------------------------------------------------

inline constexpr const char* kErrGoalNotFound = "goal.not_found";
inline constexpr const char* kErrGoalAlreadyActive = "goal.already_active";
inline constexpr const char* kErrGoalTerminal = "goal.terminal";
inline constexpr const char* kErrGoalRevisionConflict = "goal.revision_conflict";
inline constexpr const char* kErrGoalBusy = "goal.busy";
inline constexpr const char* kErrGoalInvalidTransition = "goal.invalid_transition";
inline constexpr const char* kErrGoalBudgetExhausted = "goal.budget_exhausted";
inline constexpr const char* kErrGoalScopeChanged = "goal.scope_changed";
inline constexpr const char* kErrGoalStoreUnavailable = "goal.store_unavailable";
// objective 校验(goal 单"objective"节:trim 后非空、<= 4000 码点)。
inline constexpr const char* kErrGoalObjectiveEmpty = "goal.objective_empty";
inline constexpr const char* kErrGoalObjectiveTooLong = "goal.objective_too_long";
// achieved 的证据门槛不够,程序把 evaluator 的 achieved 改判 continue。
inline constexpr const char* kErrGoalEvidenceInsufficient = "goal.achievement_evidence_insufficient";
// checkpoint 工具:引用了本 goal/iteration 没产出的 evidence id。
inline constexpr const char* kErrGoalEvidenceUnknown = "goal.evidence_unknown";
// evaluator 输出不合 Schema(一次 repair 后仍坏 → Paused(evaluator_failed))。
inline constexpr const char* kErrGoalEvaluatorSchema = "goal.evaluator_schema_invalid";

// ---------------------------------------------------------------------------
// objective 合同(0 期:4000 characters 的准确计数法)
// ---------------------------------------------------------------------------

// 产品面上限:4000 characters(OpenAI /goal 合同同款)。
inline constexpr std::size_t kGoalObjectiveMaxChars = 4000;

// 按 UTF-8 码点数计数(不拿 bytes 冒充;emoji 4 字节算 1 个 character,
// 组合字符按码点各自计数——"准确计数法"取 Unicode scalar value 口径)。
std::size_t CountGoalObjectiveChars(const std::string& text);

// 校验:trim 后非空、码点数 <= 4000。空返 kErrGoalObjectiveEmpty,超长返
// kErrGoalObjectiveTooLong,合法返空串。CLI 与 Runtime 共这一只。
std::string ValidateGoalObjective(const std::string& objective);

// ---------------------------------------------------------------------------
// 领域模型
// ---------------------------------------------------------------------------

// 目标合同:做什么、不动什么、拿什么验、何时停。首轮 preflight 拟出,
// 首次有副作用工具前冻结;evaluator 永远按冻结 revision 判。
struct GoalCriterion {
    std::string id;       // 稳定 id(c-1、c-2…),criteria 状态向量用它对账
    std::string text;     // 一条可验证的验收句
    bool required = true; // false = 加分项,不卡 achieved
};

struct GoalContract {
    std::string objective;
    std::vector<std::string> in_scope;
    std::vector<std::string> out_of_scope;
    std::vector<GoalCriterion> criteria;
    std::vector<std::string> validation_commands;
    std::vector<std::string> required_artifacts;
    std::vector<std::string> constraints;
    std::vector<std::string> checkpoints;
    std::vector<std::string> pause_conditions;

    nlohmann::json to_json() const;
    static GoalContract from_json(const nlohmann::json& j);
};

// 预算与刹车。首版至少开 elapsed/iterations/no-progress 三只硬闸;token
// 未回报时不能拿 0 冒充没花(此时 time/iteration 仍能收口)。
struct GoalBudget {
    std::optional<std::int64_t> max_total_tokens;
    std::optional<std::int64_t> max_elapsed_ms;
    std::optional<int> max_iterations;
    std::optional<std::int64_t> max_cost_micros;
    int max_no_progress_iterations = 3;
    int max_same_blocker_iterations = 3;
    int max_consecutive_provider_failures = 3;

    // suggested defaults 之外的自定义闸,存档侧原样收(不在这里验合法性,
    // 消费方按 max_* 语义用)。
    nlohmann::json to_json() const;
    static GoalBudget from_json(const nlohmann::json& j);
};

// 分角色 usage 账(execution/evaluator/subagent 分列;cache 与 input/
// output 分开存,口径沿用 TurnUsageStats)。
struct GoalUsage {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t reasoning_tokens = 0;
    std::int64_t request_count = 0;   // 模型请求数(execution+evaluator+subagent)
    std::int64_t duration_ms = 0;     // 累计墙钟(steady 差)
    bool usage_reported = false;      // provider 不报 usage 时 false;不许画 0

    void Add(const GoalUsage& other);
    nlohmann::json to_json() const;
    static GoalUsage from_json(const nlohmann::json& j);
};

// 检查点:这一轮改了什么、验了什么、还欠什么。要短,是下轮路标,
// 不是整段聊天摘要。
struct GoalCheckpoint {
    int version = 1;
    std::string summary;
    std::vector<std::string> completed;
    std::vector<std::string> remaining;
    std::vector<std::string> validations;
    std::vector<std::string> evidence_ids;
    std::string next_action;
    std::optional<std::string> blocker_key;
    std::optional<std::string> question;
    // 宿主算,不信模型自己填(指纹同,算无进展)。
    std::string progress_fingerprint;
    // checkpoint 工具没被调、宿主合成的那枚,标 true(status 退 Progress)。
    bool synthesized = false;

    nlohmann::json to_json() const;
    static GoalCheckpoint from_json(const nlohmann::json& j);
};

// evaluator 判词的 criterion 明细。
struct CriterionVerdict {
    std::string id;
    std::string status;  // pass / fail / unknown / stale
    std::vector<std::string> evidence_ids;
    std::string reason;
};

// 一次 evaluator 判定。
struct GoalEvaluation {
    std::string id;  // eval-<n>
    GoalDecision decision = GoalDecision::Continue;
    std::string summary;
    bool progress = false;
    std::vector<CriterionVerdict> criteria;
    std::string next_action;
    std::optional<std::string> blocker_key;
    std::optional<std::string> question;
    double confidence = 0.0;
    // 程序硬门槛不够时,achieved 改判 continue 的痕迹(审计用)。
    bool overridden_achieved = false;
    std::string override_reason;

    nlohmann::json to_json() const;
    static GoalEvaluation from_json(const nlohmann::json& j);
};

// 一枚证据(宿主从 tool trace/git/测试/用户回答里采)。
struct GoalEvidence {
    std::string id;      // ev-<n>
    EvidenceKind kind = EvidenceKind::ToolResult;
    std::string goal_id;
    std::string iteration_id;
    std::string tool_use_id;
    std::string producer;         // run_command / write_file / evaluator / host…
    nlohmann::json facts;         // 结构化事实(命令、退出码、digest…)
    std::string content_sha256;   // 内容摘要(证据新鲜度与防伪对账)
    std::int64_t observed_at_ms = 0;
    bool fresh = true;            // 相关改动后翻 stale
    bool truncated = false;

    nlohmann::json to_json() const;
    static GoalEvidence from_json(const nlohmann::json& j);
};

// 一轮执行(iteration 与 turn 一对一)。
struct GoalIteration {
    std::string id;         // goal-3/iter-7
    std::string goal_id;
    int index = 0;          // 每个 goal 从 1 起,单调增加
    int goal_revision = 0;  // 这枚 iteration 吃的 revision(旧轮回来不覆盖新目标)
    std::string turn_id;
    GoalIterationPhase phase = GoalIterationPhase::Scheduled;
    int attempt = 0;        // provider retry 只增 attempt,不增 index
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
    GoalUsage usage;
    std::string before_workspace_fingerprint;
    std::string after_workspace_fingerprint;
    std::vector<std::string> evidence_ids;
    std::optional<GoalCheckpoint> checkpoint;

    nlohmann::json to_json() const;
    static GoalIteration from_json(const nlohmann::json& j);
};

// 目标的计数器(防空转的账)。
struct GoalCounters {
    int iterations_started = 0;
    int no_progress_streak = 0;      // 连续无进展轮数(fingerprint 不变)
    int same_blocker_streak = 0;     // 同一 blocker_key 连续出现轮数
    std::string last_blocker_key;    // 归一后的 blocker(空 = 上轮无 blocker)
    std::string last_progress_fingerprint;  // 上一轮 fingerprint(比这一轮)
    int consecutive_provider_failures = 0;

    nlohmann::json to_json() const;
    static GoalCounters from_json(const nlohmann::json& j);
};

// GoalTask:一场 thread 同时只许一只非终态(active pointer 在 coordinator)。
struct GoalTask {
    std::string id;  // goal-<session-local monotonic id>
    int revision = 1;
    std::string objective;
    std::string objective_sha256;
    GoalContract contract;           // Preparing 期逐项补,冻结后 hash 记档
    bool contract_frozen = false;
    std::string contract_sha256;
    GoalState state = GoalState::Preparing;
    GoalBudget budget;
    GoalCounters counters;
    GoalCheckpoint checkpoint;       // 最近一枚(下一轮路标)
    std::optional<GoalEvaluation> last_evaluation;
    std::string workspace_root;
    std::string workspace_identity;  // 归一化比较键(root+branch 摘要)
    std::int64_t created_at_ms = 0;
    std::int64_t updated_at_ms = 0;
    std::optional<std::int64_t> started_at_ms;
    std::optional<std::int64_t> terminal_at_ms;
    GoalUsage usage;                 // 全 iteration 累计(evaluator/subagent 归入)

    nlohmann::json to_json() const;
    static GoalTask from_json(const nlohmann::json& j);
};

// ---------------------------------------------------------------------------
// 状态机转换表(纯函数,单测钉死)
// ---------------------------------------------------------------------------

// 源状态 -> 允许的目标状态集合。
std::vector<GoalState> AllowedTransitions(GoalState from);

// 这条转换合法吗(集中表查;非法返回 false,coordinator 报
// kErrGoalInvalidTransition)。
bool IsValidTransition(GoalState from, GoalState to);

// pause 请求在各状态下的落点(pausing 是中间态,等安全边界收口):
//   Active/Preparing -> Paused(立刻)
//   Running/Evaluating -> Pausing(记 pause_requested,边界收口)
//   AwaitingApproval/AwaitingUser -> Paused(取消悬问)
//   Paused -> Paused(幂等)
//   terminal -> 不动(报 kErrGoalTerminal)
// 返回 {目标状态, 是否立刻}。
struct PauseOutcome {
    GoalState target = GoalState::Paused;
    bool immediate = true;
    bool allowed = true;
};
PauseOutcome PauseTransition(GoalState from);

// terminal 之后再来的迟到事件(旧 evaluator/子代理/Hook)只留 evidence,
// 不改状态(单子 corner case"terminal 后迟到")。
inline bool IsLateArrivalAfterTerminal(GoalState current, GoalState) { return IsGoalTerminal(current); }

}  // namespace lubancode::runtime::goal
