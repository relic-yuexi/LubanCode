// SessionRuntime(显示系统剥离单第六步:拆 SessionRuntime)。
//
// 一场会话真正的核心状态,从 app/interactive_session.cpp 的 InteractiveSession
// (缩成 TerminalSessionController)手里搬出来的那半:
//   - thread 身份与统一发号(IdAuthority:thread/turn/item/request/seq);
//   - 会话存档账:SessionStore、SessionMeta、标题、落盘基线、压缩序号、
//     建档失败旗——Begin/Resume/追加/标题事件/压缩事件的开账与收口;
//   - 会话权限账:"总是允许"的工具集合(按 a / accept_for_session 落进来);
//   - 事件接线:Attach 一只 EventSink,每轮经 TurnEventAdapter 把
//     AgentLoop 回调翻成 ServerEvent 流——终端、app-server、Web/Tauri
//     接同一颗。
//
// 边界(单子"Runtime 不碰界面"):本类不 include cli/app/frontend,不读
// stdin、不写 stdout/stderr——成败用返回值交账,人话由前端印。控制器
// (TerminalSessionController)持有本类并按引用续用老成员名,行为不变;
// 远端前端(app-server/Web/Tauri)直接持本类装配,不再复制 InteractiveSession
// 的那一套栈。
//
// 剩余留在控制器一侧的(按单子次序后续批次搬):工具全栈(ToolRuntime)、
// backend 栈、peer、steering 泵、面板与键位——那些与终端回调缠得深,
// 一次搬完风险大,本步先落"账本"这一层。

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "sessions/session_store.hpp"
#include "api/types.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/plan_mode.hpp"
#include "runtime/trajectory_session.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "workspace/identity.hpp"  // P0-1:Options.trajectory_workspace_identity

namespace lubancode::runtime {

// 建档结果:控制器据此决定要不要打一行错误(本类不打印)。
enum class SessionBeginResult {
    Active,    // 已经建过档(或刚建成功)
    Begun,     // 本次新建成功
    Failed,    // 本次尝试失败(store_broken 已置位)
    Disabled,  // 没主目录/先前已 broken:根本没试
};

// 增量落盘结果。
enum class SessionPersistResult {
    Nothing,    // 没有新消息(或本来就不落盘)
    Appended,   // 追加成功
    BrokenNow,  // 追加失败,broken 在这一刻置位(只报一次)
};

class SessionRuntime {
public:
    struct Options {
        std::string sessions_dir;  // 空 = 找不到主目录,不落盘
        std::string wire_name;     // meta.wire(provider 协议名)
        std::string start_ts;      // 会话 id 的时间戳底子(NowIdTimestamp)
        // P0-2 轨迹(flag 开的会话走 Trajectory v1,§十七"内部预览"):
        // true 时本会话只写 Trajectory Journal,不写旧 SessionStore——
        // 禁 dual-write;开张失败由 trajectory_open_error 报,装配层须
        // 让会话启动失败,不许回退旧写口。
        bool trajectory_enabled = false;
        // P0-1:装配层按四级裁决冻好的身份整份递进(终端/app-server/子代理
        // 同一把钥匙);空 = ledger 按启动 cwd 现场裁决(兜底,只服务测试)。
        workspace::WorkspaceIdentity trajectory_workspace_identity;
        // 轨迹根:空 = <home>/.lubancode/trajectories(生产默认);测试与
        // P0-2 根迁移装配用它注入。
        std::filesystem::path trajectory_trajectories_root;
        std::string lubancode_version;
        // P0-3 resume(§10.4):--continue 启动路开 start_reason=resume 的
        // 新场(source 只读,永不 reopen append)。source id 空 = 取本
        // workspace 最近一场可恢复的;没有可恢复场回落普通开张。
        bool trajectory_resume_at_launch = false;
        std::string trajectory_resume_source_session_id;
    };

    explicit SessionRuntime(Options options);
    ~SessionRuntime();

    SessionRuntime(const SessionRuntime&) = delete;
    SessionRuntime& operator=(const SessionRuntime&) = delete;

    // ---- thread 身份与事件 --------------------------------------------------
    // 事件层的 thread_id(IdAuthority 发的 thread-<n>)。存档的会话 id
    // (MakeSessionId 的时间戳+slug)是另一本账,在 store 里,两者不混。
    const std::string& thread_id() const { return thread_id_; }
    IdAuthority& ids() { return ids_; }

    // 事件出口:不持有,调用方保证存活;可空(终端老路不接)。
    void AttachSink(EventSink* sink) { sink_ = sink; }
    EventSink* sink() const { return sink_; }

    // ---- 轨迹账(P0-2:SessionRuntime 持有 Recorder 所有权) ----------------
    // flag 开的会话:本类持一场 TrajectorySessionLedger(main recorder +
    // 目录 + 子代理注册表)。空 = flag 关,旧路照旧。开张失败给
    // trajectory_open_error(),装配层据此失败会话启动。
    TrajectorySessionLedger* trajectory() { return trajectory_.has_value() ? &*trajectory_ : nullptr; }
    // P0-1(§4.5):cwd 变化对账(/worktree 切房等)。同 workspace:落
    // control.cwd.changed + checkout 登记,账不换房;跨 workspace:封当前
    // 场(end_reason=workspace_switch)后按新身份整只重开 ledger——旧账
    // 留在新 workspace 的 sessions/ 里可 resume,不偷偷搬。返回空 = 顺;
    // 否则错误说明(调用方打警告,不拦切房主流程)。
    std::string NoteWorkingDirectoryChanged(const std::filesystem::path& new_cwd);
    const TrajectorySessionLedger* trajectory() const {
        return trajectory_.has_value() ? &*trajectory_ : nullptr;
    }
    bool trajectory_enabled() const { return options_.trajectory_enabled; }
    const std::string& trajectory_open_error() const { return trajectory_open_error_; }

    // 开一轮的事件适配器:把 loop 的回调翻成 ServerEvent 流,落到 AttachSink
    // 挂的那只 sink(没挂就只发号不落笔)。每轮各开一只,轮间不共用状态。
    TurnEventAdapter MakeTurnAdapter();

    // ---- 会话存档账(本类持有,控制器按引用续用) ----------------------------
    sessions::SessionStore& store() { return store_; }
    const sessions::SessionStore& store() const { return store_; }
    const std::string& sessions_dir() const { return options_.sessions_dir; }
    const std::string& wire_name() const { return options_.wire_name; }
    const std::string& start_ts() const { return options_.start_ts; }
    sessions::SessionMeta& meta() { return meta_; }
    std::string& title() { return title_; }
    bool& title_pending() { return title_pending_; }
    std::size_t& persisted_count() { return persisted_count_; }
    int& compact_epoch() { return compact_epoch_; }
    bool& store_broken() { return store_broken_; }

    // 建档:meta 填账 + Begin + 建档前挂起的标题补事件行。失败置 broken。
    // 首条文本做 slug;model/cwd 由调用方给(会话模型与目录是控制器的活)。
    // Plan 模式单:建档前切过的协作档(存档未开时 SetCollaborationMode 只
    // 记内存)在这里补落 mode_v1——起手 --mode plan 的场子,档里第一行
    // 用户消息之前就有 mode 账,resume 才接得回档位。
    // P0-2 轨迹路:trajectory_enabled 的会话不建旧档(单一真账在
    // Trajectory Journal),直接回 Disabled。
    SessionBeginResult EnsureBegun(const std::string& first_text, const std::string& model,
                                   const std::string& cwd);

    // 增量落盘:history 里 persisted_count 之后逐条追加(只增不减);store
    // 还没开张时先按兜底建档(首条用户文本抽出来做 slug)。失败置 broken。
    // P0-2 轨迹路:轮末补抄这条路整个停用——事实由
    // input.received/model.output.completed/tool.result.committed 事件
    // 即时落账,轮末只验状态机,不补抄消息(§15.3)。
    SessionPersistResult PersistNew(const std::vector<api::Message>& history, const std::string& model,
                                    const std::string& cwd);

    // 落盘基线收到新长度(/compact 换史后由调用方校正;
    // 只收不放,防旧账重写)。
    void ClampPersisted(std::size_t history_size);

    // ---- 会话权限账 ----------------------------------------------------------
    // "总是允许"的工具集合:确认档按 a、远端审批 accept_for_session 落进来,
    // 本场该工具免问。settings.local.json 的 allow_tools 由装配层注入。
    std::set<std::string>& always_allowed() { return always_allowed_; }

    // ---- 协作模式账(Plan 模式单:两根轴的会话级真值) ------------------------
    // ModeState 归本类所有(单子:"不塞进 LineEditor 的 ConfirmMode 原子"),
    // 前端只读快照。切档走 SetCollaborationMode——它顺手落 mode_v1 事件行
    // (存档活跃时),/resume 按"最后一条 mode 事件"恢复档位。
    const ModeState& mode_state() const { return mode_state_; }
    CollaborationMode collaboration_mode() const { return mode_state_.active; }

    // 切档。reason 是稳定短码("slash"/"approved"/"resume"/"clear"),随事件
    // 行落账。permission_before_plan 在切入 Plan 时由调用方传当前确认档
    // ("confirm"/"auto"/"yolo");切回 Default 时本类不动确认档——批准框选的
    // 新档只改本 session,由装配层落。
    // 返回值:切换是否真的发生(同档重复切给 false,不落事件行)。
    bool SetCollaborationMode(CollaborationMode mode, const std::string& reason,
                              const std::string& permission_before_plan = std::string());
    // resume 专用:只回内存真值,不落 mode 事件行(档位是回放出来的,再落
    // 一行会把 resume 当一次切换记账)。
    void RestoreCollaborationMode(CollaborationMode mode, std::uint64_t revision) {
        mode_state_.active = mode;
        mode_state_.revision = revision;
    }

    // ---- 跨轮保留账(Kimi 保留式思考单 P1)-----------------------------------
    // /think history default|all 的会话账:存档活跃就落 think_history_v1 事件
    // 行(append+flush),没建档先挂起(EnsureBegun 补落)。mode 是稳定短码
    // ("default"/"all"),/resume 按"最后一条"恢复并按当前模型重新校验。
    void RecordThinkHistory(const std::string& mode);

    // ---- 计划成品账 -----------------------------------------------------------
    // 最近一份 PlanDocument(按 revision supersede;旧稿仍在 session 事件
    // 行里,/resume 按 plan_v1 行重建全账)。null = 本场还没交过计划。
    void RecordPlanDocument(const PlanDocument& plan);
    // resume 专用:只回内存真值,不落 plan 事件行(账已在档里,重复落会
    // 翻倍),也不做 supersede 侧写。
    void RestorePlanDocument(const PlanDocument& plan) {
        latest_plan_ = plan;
        mode_state_.latest_plan_id = plan.plan_id;
    }
    const PlanDocument* latest_plan() const { return latest_plan_.has_value() ? &*latest_plan_ : nullptr; }

    // 计划审批:批准/拒绝/继续规划。批准须同时匹配 id/revision/hash(单子:
    // "用户审的是哪一稿,账上写清");不匹配给 stale,不落账。
    enum class PlanReviewOutcome { Approved, Rejected, Stale };
    PlanReviewOutcome ReviewPlan(const std::string& plan_id, std::uint64_t revision, const std::string& sha256,
                                 bool approve);

private:
    Options options_;
    IdAuthority ids_;
    std::string thread_id_;
    EventSink* sink_ = nullptr;

    sessions::SessionStore store_;
    sessions::SessionMeta meta_{};
    std::string title_;
    bool title_pending_ = false;
    // persisted_count_:旧路(轮末按 history 追加补抄)的落盘基线。轨迹路
    // (P0-2)不读不写它——事实由事件即时落账,这条补抄路停用;flag 关的
    // 会话照旧。
    std::size_t persisted_count_ = 0;
    int compact_epoch_ = 0;
    bool store_broken_ = false;

    // P0-2 轨迹账:flag 开的会话持一场(可选构造;move-only)。
    std::optional<TrajectorySessionLedger> trajectory_;
    std::string trajectory_open_error_;

    std::set<std::string> always_allowed_;

    // Plan 模式单:两轴真值与计划成品账。
    ModeState mode_state_;
    // 存档未开时切过的档(起手 --mode plan):建档那一刻补落。
    std::optional<sessions::ModeEvent> pending_mode_event_;
    // 存档未开时切过的跨轮保留选择(起手 /think history all):建档补落。
    std::optional<sessions::ThinkHistoryEvent> pending_think_history_;
    std::optional<PlanDocument> latest_plan_;
};

}  // namespace lubancode::runtime
