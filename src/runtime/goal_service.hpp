// GoalService(轨迹 v3 §4.67 Goal 模式 G0):goal 状态的唯一持久写口。
//
// 设计对照 todos/session轨迹v3 §4.67(G0 批)与 §4.55(持久控制状态):
//   - 快照:不可变版本文件 sessions/<id>/state/goals/<goalId>/rev-000NNN.json;
//     JSONL 账上只落 state.goal.applied 提交事件(goalId/旧新 stateRevision/
//     contractRevision/snapshotRef/snapshotSha256/causeRef)。未 applied 的
//     候选快照不生效;applied 已落、内存未更新时,恢复采用已提交版本。
//   - 提交事务(§4.55 次序):校验 expectedStateRevision/contractRevision 与
//     lifecycle 转换合法 -> 写新快照并落稳 -> 提交口再核对 -> 追加
//     state.goal.applied(PowerLoss) -> 发布内存。写盘失败 fail closed:
//     锁内存执行门(broken),不宣称已持久封账。
//   - 单写者:一卷 v3 JSONL 一只 V3Writer(账侧保证);GoalService 在其上
//     做 CAS(stateRevision 单调 +1),命令/模型/Hook 都只交候选,不经本口
//     不改状态。
//   - 只读投影:ProjectGoalState 验账(哈希链归 ReadV3Ledger)+ 逐 applied
//     校验快照存在/hash/转换合法,缺口明报不猜(§4.55"状态损坏")。
//
// 与 v1 运行面(GoalCoordinator)的关系(§4.67.9):coordinator 的 revision
// 校验/预算闸/完成门槛职责可复用,本件不复刻其内存状态机;lifecycle/phase
// 与双版本号(stateRevision/contractRevision)按 §4.67.3 收敛,命令面接线
// 归 G1(已接:app/commands/goal_commands.cpp 的 v3 分支 + goal_session_
// wiring 的认领/开轮/收口泵路)。v1 的 GoalState 枚举(Running/Evaluating
// 顶层态)与本处 lifecycle+phase 两层模型的合并收敛归 G2+(验收接进来才
// 有 evaluating 相位的真来源)。
//
// G0 定型、后续棒次补内容:checkpointRef/appliedEvaluationId 的真实来源
// 事件(G2)、waitTaskRefs 与巡检计划(G3)。pendingIntent(continuation
// 意图)G1 已定型:GoalPendingIntent + SetPendingIntent/ClaimPendingIntent。
//
// 依赖铁律:不 include cli/app/api;trajectory::v3 只前向声明(实现侧引
// writer/reader),goal_types 的纯数据(合同/预算/usage/计数)照抄复用。

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/goal_types.hpp"

namespace lubancode::trajectory::v3 {
class V3Writer;   // 单写者(账侧发号/哈希链/耐久)
struct V3Ledger;  // 只读投影的验后账
}  // namespace lubancode::trajectory::v3

namespace lubancode::runtime::goal {

// ---------------------------------------------------------------------------
// lifecycle 与 phase(§4.67.3 表;线上是字符串,改了便是改 schema)
// ---------------------------------------------------------------------------

// 目标生命周期。terminal 三枚(achieved/cleared/failed)不自动复活;
// paused/awaiting_user/blocked/budget_exhausted 可经显式恢复转回
// active(各带停因与解锁条件);suspended_by_policy 只许 clear。
// 与 v1 GoalState 的差异:running/evaluating 降为 phase,新增 waiting,
// v1 的 pausing/awaiting_approval 不在本表(审批等待归会话权限面,
// §4.67.1"沿原权限入口")。
enum class GoalLifecycle {
    Preparing,
    Active,
    Waiting,
    Paused,
    AwaitingUser,
    Blocked,
    BudgetExhausted,
    SuspendedByPolicy,
    Achieved,
    Cleared,
    Failed,
};
std::string ToString(GoalLifecycle lifecycle);
bool ParseGoalLifecycle(const std::string& s, GoalLifecycle& out);
bool IsLifecycleTerminal(GoalLifecycle lifecycle);

// 工作相位(§4.67.3 active 行):idle/queued/running/evaluating。
// "活动目标"不等于"模型正跑";waiting/paused 等停态下 phase 回 idle。
enum class GoalPhase { Idle, Queued, Running, Evaluating };
std::string ToString(GoalPhase phase);
bool ParseGoalPhase(const std::string& s, GoalPhase& out);

// lifecycle 转换表(纯函数,单测钉死)。合法转换:
//   preparing    -> active / paused / awaiting_user / cleared / failed
//   active       -> waiting / paused / awaiting_user / blocked /
//                   budget_exhausted / suspended_by_policy / achieved /
//                   cleared / failed
//   waiting      -> active / paused / awaiting_user / blocked /
//                   budget_exhausted / cleared / failed
//   paused       -> active / suspended_by_policy / cleared / failed
//   awaiting_user-> active / paused / blocked / cleared / failed
//   blocked      -> active / paused / cleared / failed
//   budget_exhausted -> active(显式加预算并复核)/ cleared / failed
//   suspended_by_policy -> cleared
//   achieved/cleared/failed -> (terminal,不再转)
// 设计待定项:budget_exhausted/blocked -> active 带前置条件(加预算/改
// 条件),G0 只放开转换边,条件核验钩子归 G1 命令面。
std::vector<GoalLifecycle> AllowedLifecycleTransitions(GoalLifecycle from);
bool IsValidLifecycleTransition(GoalLifecycle from, GoalLifecycle to);

// ---------------------------------------------------------------------------
// 证据引用(§4.67.5)
// ---------------------------------------------------------------------------

// 证据指回账上何处。
enum class GoalEvidenceSource { ToolAction, Message, Artifact };
std::string ToString(GoalEvidenceSource source);
bool ParseGoalEvidenceSource(const std::string& s, GoalEvidenceSource& out);

// 一枚证据引用:回指 session/run、来源(toolCallId/messageId/artifact)、
// 内容 hash、观测时点、工作区基线与 criterionId。宿主采,不信模型自报;
// fresh=false 表示相关改动后旧验证不再可信。
struct GoalEvidenceRef {
    std::string id;                // ev-<n>
    std::string kind;              // EvidenceKind 稳定串(command_exit/…)
    GoalEvidenceSource source = GoalEvidenceSource::ToolAction;
    std::string source_ref;        // actionId 或 messageId(同会话 string)
    nlohmann::json artifact_ref;   // source=Artifact 时的六键 artifactRef
    std::string session_id;        // 回指来源账
    std::string run_id;
    std::string content_sha256;    // hex64
    std::int64_t observed_at_ms = 0;
    std::string workspace_baseline;  // 工作区基线指纹(空 = 未记)
    std::string criterion_id;        // 绑定的验收项 id(空 = 未绑定)
    bool fresh = true;
    bool truncated = false;

    nlohmann::json ToJson() const;
    static std::optional<GoalEvidenceRef> FromJson(const nlohmann::json& j, std::string* error);
};

// 证据引用的合同校验(单行可判:必选字段/类型/枚举/hex64)。返回空串 = 过。
std::string ValidateEvidenceRef(const nlohmann::json& j);

// ---------------------------------------------------------------------------
// 快照 schema(§4.67.3"快照至少保存"全表)
// ---------------------------------------------------------------------------

// 一版不可变 goal 状态快照。字段 = 设计 §4.67.3 清单:
// goalId、来源 session 引用、stateRevision、contractRevision、合同、
// lifecycle、phase、stopReason、当前 iterationId、checkpointRef、
// evidenceRefs、pendingQuestion、waitTaskRefs、预算与累计 usage、
// 进展/错误计数、待办调度项及已采用 evaluationId。
struct GoalStateSnapshot {
    std::string goal_id;
    std::string parent_goal_id;  // fork lineage;空 = 本链原生
    std::string session_id;      // 来源 session 引用
    std::string run_id;

    std::uint64_t state_revision = 1;    // 每次状态提交 +1
    std::uint64_t contract_revision = 1; // 只随验收合同变化

    std::string objective;
    std::string objective_sha256;
    GoalContract contract;  // 复用 v1 纯数据形状
    bool contract_frozen = false;

    GoalLifecycle lifecycle = GoalLifecycle::Preparing;
    GoalPhase phase = GoalPhase::Idle;
    std::string stop_reason;       // paused/blocked/failed 等停态的原因
    std::string blocker_key;       // blocked 必带(解锁条件归 reason)
    std::string pending_question;  // awaiting_user 必带
    std::optional<std::string> iteration_id;       // 当前工作轮(可空)
    std::optional<std::string> checkpoint_ref;     // 指向账上 checkpoint 记录(G1+ 填)
    std::vector<GoalEvidenceRef> evidence_refs;
    std::vector<std::string> wait_task_refs;       // G3 填;空数组照落
    std::optional<std::string> applied_evaluation_id;  // 已采用判词(G2+ 填)

    GoalBudget budget;    // 复用(含 no-progress/blocker/provider 连败闸)
    GoalUsage usage;      // 累计(unknown 不冒充 0,usage_reported 管)
    GoalCounters counters;

    nlohmann::json pending_intent = nlohmann::json::object();  // G1 填

    std::string workspace_root;
    std::string workspace_identity;
    std::int64_t created_at_ms = 0;
    std::int64_t updated_at_ms = 0;

    nlohmann::json ToJson() const;
    // 严格解析:必选字段缺/类型错/枚举错返回错误串(schema 不静默补默认)。
    static std::optional<GoalStateSnapshot> FromJson(const nlohmann::json& j, std::string* error);
};

// 快照文件的规范字节(json dump,紧凑、UTF-8 直出):写盘与 hash 的底。
std::string SnapshotBytes(const GoalStateSnapshot& snapshot);

// 快照相对路径(state/goals/<goalId>/rev-%06u.json,正斜杠,账上统一)。
std::string SnapshotRefPath(const std::string& goal_id, std::uint64_t state_revision);

// ---------------------------------------------------------------------------
// continuation 意图(§4.67.4/G1:快照 pendingIntent 的定型形状)
// ---------------------------------------------------------------------------

// 一枚待续工作项的意图。§4.67.4:续跑去重键为
//   (goalId, contractRevision, predecessorIterationId, continuationOrdinal);
// 首轮 predecessor 为空、ordinal=1。意图含 workItemId(恢复按原 id 补队列,
// 不重排)、triggerRef/nextActionRef。claim 面:取走工作项先提交 claimed
// 状态和 writerEpoch 再调模型;他 epoch 已认领的意图不盲重放(恢复核验)。
struct GoalPendingIntent {
    std::string work_item_id;      // wi-<n>:恢复补队列的去重身份
    std::uint64_t contract_revision = 1;
    std::string predecessor_iteration_id;  // 空 = 首轮
    int continuation_ordinal = 1;
    std::string trigger_ref;        // 可空(触发意图的账引用)
    std::string next_action_ref;    // 可空
    bool claimed = false;
    std::string writer_epoch;       // claim 者(写者 run id)
    std::int64_t claimed_at_ms = 0;

    std::string DedupeKey(const std::string& goal_id) const;
    nlohmann::json ToJson() const;
    static std::optional<GoalPendingIntent> FromJson(const nlohmann::json& j, std::string* error);
};

// 意图形状的单行合同校验(缺字段/类型错/枚举错/claim 面不自洽都拒)。
std::string ValidatePendingIntent(const nlohmann::json& j);

// §4.67.4 恢复补投影:这枚快照对当前写者还有没有可取的工作项。
//   - 意图未认领 + 非停态 + 合同版本对得上 → claimable(恢复按原
//     workItemId 补队列,重复 resume 只补同一项,不另发新工作);
//   - 他 epoch 已认领 → claimed_by_other(已启动未收口,恢复核验归 G2,
//     不重放副作用);
//   - 本 epoch 已认领且 phase=queued(claim 后、开轮前)→ 仍 claimable
//     (claim 幂等,沿用原项);
//   - 停态/终态/意图对着旧合同 → 不排(§4.67.4"旧 Goal 工作项不挤过
//     排在边界前的 pause/edit/clear")。
struct GoalWorkView {
    bool claimable = false;
    bool claimed_by_other = false;
    bool has_intent = false;
    GoalPendingIntent intent;
    std::string reason;  // 人话(为什么不能排)
};
GoalWorkView EvaluateGoalWork(const GoalStateSnapshot& snapshot, const std::string& writer_epoch);

// ---------------------------------------------------------------------------
// 只读投影(§4.67 G0"只读投影";§5.1 纯读零调用零重跑)
// ---------------------------------------------------------------------------

// 投影缺口(§4.55:事件指向的快照缺失/hash 不符时报告缺口,不用摘要猜)。
enum class GoalProjectionGap {
    None,
    NoGoal,             // 账上没有 state.goal.applied
    SnapshotMissing,    // applied 指向的快照文件不在
    SnapshotUnreadable, // 快照读不了/解析不了/合同校验不过
    HashMismatch,       // 快照内容 sha256 对不上 applied 里存的
    RevisionMismatch,   // 快照 stateRevision/contractRevision 与 applied 不一致
    IllegalTransition,  // applied 序列含非法 lifecycle 转换(terminal 复活等)
};
std::string ToString(GoalProjectionGap gap);

struct GoalProjection {
    bool has_goal = false;
    GoalStateSnapshot snapshot;   // 最新生效快照
    std::string goal_id;
    std::string session_id;             // 这份投影来自哪一卷(跨卷接管用)
    std::uint64_t applied_seq = 0;      // 最新 applied 的 seq
    std::string applied_event_id;
    std::string applied_line_hash;
    std::string snapshot_ref;           // 相对 session 根
    GoalProjectionGap gap = GoalProjectionGap::None;
    std::string gap_detail;             // 人话(哪一版、差在哪)
};

// 从验后账投影当前 goal 状态:扫全部 state.goal.applied(落盘序),逐条
// 校验 revision 递增与 lifecycle 转换合法,取最后一条为 head;按
// snapshotRef 实探快照并验 hash。ledger 须已过 ReadV3Ledger(哈希链已验);
// session_dir 为空时快照全部按 SnapshotMissing 报缺口。
//
// 跨卷续接(G1):resume-as-new 后 goal 的新 applied 落在新卷,首条
// fromStateRevision != 0——须带 adoptedFrom{sessionId,stateRevision} 且
// stateRevision == fromStateRevision(接管凭据),否则按非法序列报缺口。
GoalProjection ProjectGoalState(const trajectory::v3::V3Ledger& ledger,
                                const std::filesystem::path& session_dir);

// 沿 resume 来源链投影 goal head(§4.67.8"goal 沿 session lineage 持久
// 保存";§4.67 G1 命令面/恢复共用的单一读面)。从 current_session_dir
// 所在场起:先投本场卷;没有 goal 账且本场 start_reason=resume 时沿
// session.json 的 previousSessionId 逐级向上,取最近一份有 goal 账的卷
// (含缺口——缺口如实上报,不猜)。clear/fork 边不穿(goal 不跟 clear
// 走);链断(目录缺/manifest 坏)在 detail 里说明。深度护栏 32 跳、
// 防 id 回环。
struct GoalLineageProjection {
    bool found = false;          // 链上任一卷有 goal 账(含缺口)
    GoalProjection projection;   // found 时为最近一份;否则 gap=NoGoal
    std::vector<std::string> walked;  // 走过的 session id(审计)
    std::string detail;          // 人话(从哪卷来/链停在哪)
};
GoalLineageProjection ProjectGoalLineage(const std::filesystem::path& current_session_dir);


// ---------------------------------------------------------------------------
// GoalService:唯一持久写口
// ---------------------------------------------------------------------------

// 一枚提交的结果:ok=false 时 error_code 是稳定码(goal.*)。
struct GoalServiceResult {
    bool ok = false;
    std::string error_code;
    std::string error_message;
    nlohmann::json payload = nlohmann::json::object();
};

// 状态转换候选(命令/模型/Hook 都只交这个,GoalService 校验后提交)。
// 模型无权直接写 achieved 或改预算:候选不带合同与预算字段,合同改版走
// AmendContract,预算调整归 G3 的显式加预算路径。
struct GoalTransitionCandidate {
    std::string goal_id;
    std::uint64_t expected_state_revision = 0;     // CAS;0 = 拒(必须显式)
    std::uint64_t expected_contract_revision = 0;  // 0 = 不校验
    GoalLifecycle to_lifecycle = GoalLifecycle::Preparing;
    GoalPhase to_phase = GoalPhase::Idle;
    std::string stop_reason;
    std::string blocker_key;
    std::string pending_question;
    std::optional<std::string> iteration_id;
    std::vector<GoalEvidenceRef> evidence_additions;  // 追加,不整替
    std::optional<std::string> applied_evaluation_id;
    GoalUsage usage_addition;  // 只增不清零
    nlohmann::json cause_ref;  // applied 的 causeRef(合法引用或空)
};

class GoalService {
public:
    struct Options {
        // session 根(sessions/<id>/):快照落 <dir>/state/goals/。空 = 拒写
        //(只读投影不归这里)。
        std::filesystem::path session_dir;
        // 时钟注入(单测喂固定钟);空 = system_clock 毫秒。
        std::function<std::int64_t()> clock;
    };

    // writer 归装配层所有(v3_main_writer() 递入);可空 = 服务只读不写
    //(投影走自由函数)。
    GoalService(trajectory::v3::V3Writer* writer, Options options);
    ~GoalService();
    GoalService(const GoalService&) = delete;
    GoalService& operator=(const GoalService&) = delete;

    // 建目标:写 rev-000001 快照 + 首条 applied(fromStateRevision=0)。
    // 已有非终态 goal 报 goal.already_active,不暗中替换(§4.67.2)。
    // draft.goal_id 为空则发号 goal-<n>(服务内单调);state_revision/
    // contract_revision 重置为 1。
    GoalServiceResult CreateGoal(GoalStateSnapshot draft, nlohmann::json cause_ref);

    // 状态提交(唯一写口):CAS + lifecycle 转换校验 + 快照落稳 + applied
    // 落账 + 内存发布。terminal 后迟到候选拒(只留调用方审计,不改账)。
    GoalServiceResult ApplyTransition(const GoalTransitionCandidate& candidate);

    // 合同改版(§4.67.2 /goal edit):contractRevision +1,合同重拟;
    // 旧证据全翻 stale(相关旧证据重新判有效期,§4.67.2"edit"行)。
    // expected_state_revision 同样 CAS;state_revision 随本次提交 +1。
    GoalServiceResult AmendContract(const GoalContract& contract, std::uint64_t expected_state_revision,
                                    std::uint64_t expected_contract_revision,
                                    nlohmann::json cause_ref);

    // fail-closed 门:写盘失败后锁住,后续提交全拒(§4.67.3"写盘失败先
    // 锁住内存执行门并报错")。换新实例/恢复流程归 G1+。
    bool broken() const { return broken_; }

    // ---- §4.67 G1:continuation 意图、iteration 归属、claim 与恢复去重 ----

    // 意图提交(§4.67.4"continue 且有可行下一步时,将下轮意图随状态快照
    // 提交";/goal 接纳的首轮意图经 CreateGoal 的 draft.pending_intent 带)。
    // CAS 同 ApplyTransition。前一枚意图未认领时不许覆盖(欠队列的账不能
    // 静默丢,goal.intent_conflict);对着旧合同也不受理。状态提交 +1,
    // lifecycle/phase 不动。
    GoalServiceResult SetPendingIntent(GoalPendingIntent intent, std::uint64_t expected_state_revision,
                                       nlohmann::json cause_ref);

    // 认领(§4.67.4"取走工作项也先提交 claimed 状态和 writerEpoch,再调用
    // 模型";§4.67.8 单写者接管)。提交 claimed=true + writerEpoch +
    // phase=queued;恢复去重的锚就是这枚 applied:claim 落账后,重复 resume
    // 不再把同一工作项当未认领补队列。同 epoch 幂等(已认领照回 ok);
    // 他 epoch 已认领报 goal.intent_already_claimed(恢复核验归 G2)。
    GoalServiceResult ClaimPendingIntent(std::string writer_epoch, std::uint64_t expected_state_revision,
                                         nlohmann::json cause_ref);

    // 开轮(§4.67.4 主工作轮绑定 iteration:一个 iteration 表示一份主工作
    // 轮及其收口处理,iterationId 落快照,不再恒 null)。preparing 顺带转
    // active(objective 已在手即合同底稿;真 preflight 归 G2),counters.
    // iterations_started +1;停态/终态拒。
    GoalServiceResult BeginIteration(std::uint64_t expected_state_revision, nlohmann::json cause_ref);

    // 收工(G1 无验收:执行轮收口回 idle、清 pendingIntent——认领过的工作
    // 项销账,不留给下一次 resume 重复提交;判词与续排意图归 G2,不在这
    // 假装评过)。iteration 记录留在快照(status 可见)。
    GoalServiceResult EndIteration(std::uint64_t expected_state_revision, nlohmann::json cause_ref);

    // 只读查询:当前生效快照(applied 已落、内存已发布)。终态 goal 保留
    // 在案(审计);再 Create 会另起 goalId。
    const GoalStateSnapshot* current() const { return current_.has_value() ? &*current_ : nullptr; }

    // 最近一条 applied 的账面锚(eventId/seq/lineHash;resume 对账用)。
    std::optional<std::uint64_t> applied_seq() const { return applied_seq_; }
    const std::string& applied_event_id() const { return applied_event_id_; }
    const std::string& applied_line_hash() const { return applied_line_hash_; }

    // ---- 恢复(§4.67.8"先验存储,再补恢复结论";G0 只验不跑) ----------
    // 从只读投影接管:校验投影无缺口且 goalId/revision 对得上,内存发布。
    // 有缺口报 goal.projection_gap,不接管、不猜(§4.55"状态损坏")。
    GoalServiceResult AdoptFromProjection(const GoalProjection& projection);

private:
    GoalServiceResult Commit(GoalStateSnapshot next, const nlohmann::json& cause_ref);
    GoalServiceResult Fail(const char* code, const std::string& message);
    std::int64_t Now() const;

    trajectory::v3::V3Writer* writer_ = nullptr;
    Options options_;
    std::optional<GoalStateSnapshot> current_;
    std::uint64_t next_goal_number_ = 0;  // goal-<n> 发号(账内单调)
    bool broken_ = false;
    std::string applied_event_id_;
    std::string applied_line_hash_;
    std::optional<std::uint64_t> applied_seq_;
    // 跨卷接管凭据(G1):AdoptFromProjection 置位,本写者对这只 goal 的
    // 首次 Commit 在 applied 里带 adoptedFrom{sessionId,stateRevision},
    // 落稳后清零。ProjectGoalState 按它认"本卷从半路续接"的合法首条。
    bool adopted_carry_ = false;
    std::string adopted_from_session_;
};

}  // namespace lubancode::runtime::goal
