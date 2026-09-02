// SessionRuntime(显示系统剥离单第六步:拆 SessionRuntime)。
//
// 一场会话真正的核心状态,从 app/interactive_session.cpp 的 InteractiveSession
// (缩成 TerminalSessionController)手里搬出来的那半:
//   - thread 身份与统一发号(IdAuthority:thread/turn/item/request/seq);
//   - 会话真账:一场恒开的 TrajectorySessionLedger(P0-2 起唯一 Session,
//     旧 SessionStore/轮末补抄路已随 P0-6 删净);
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
#include "api/types.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/plan_mode.hpp"
#include "runtime/trajectory_session.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "workspace/identity.hpp"  // P0-1:Options.trajectory_workspace_identity

namespace lubancode::runtime {

class SessionRuntime {
public:
    struct Options {
        std::string wire_name;     // meta.wire(provider 协议名)
        std::string start_ts;      // 会话 id 的时间戳底子(NowIdTimestamp)
        // P0-2(Trajectory 升为唯一 Session):feature/env 开关已删,本类
        // 恒开一场 TrajectorySessionLedger——开张失败由 trajectory_open_error
        // 报,装配层须让会话启动失败,不回退旧写口(禁 dual-write)。P0-6:
        // 旧 sessions_dir 与 SessionStore 成员已删,旧路不存在了。
        // P0-1:装配层按四级裁决冻好的身份整份递进(终端/app-server/子代理
        // 同一把钥匙);空 = ledger 按启动 cwd 现场裁决(兜底,只服务测试)。
        workspace::WorkspaceIdentity trajectory_workspace_identity;
        // 唯一持久化根:空 = <home>/.lubancode/workspaces(生产默认);
        // 测试与装配注入临时根。旧 trajectories/ 根零读零写。
        std::filesystem::path trajectory_workspaces_root;
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
    // 在 TrajectorySessionLedger 手里,两者不混。
    const std::string& thread_id() const { return thread_id_; }
    IdAuthority& ids() { return ids_; }

    // 事件桥的 wire 名(旁路桥 identity;与主轮桥同源)。
    const std::string& wire_name() const { return options_.wire_name; }
    // 会话 id 的时间戳底子(/clear 翻新场次用)。
    const std::string& start_ts() const { return options_.start_ts; }

    // 事件出口:不持有,调用方保证存活;可空(终端老路不接)。
    void AttachSink(EventSink* sink) { sink_ = sink; }
    EventSink* sink() const { return sink_; }

    // ---- 轨迹账(P0-2:SessionRuntime 持有 Recorder 所有权) ----------------
    // 本类持一场 TrajectorySessionLedger(main recorder + 目录 + 子代理
    // 注册表)——Session 的定义本身,恒开。空 = 开张失败(装配层须让会话
    // 启动失败,见 trajectory_open_error)。
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
    // 开不出账的说明(空 = 开出来了)。装配层据此失败会话启动。
    const std::string& trajectory_open_error() const { return trajectory_open_error_; }

    // 开一轮的事件适配器:把 loop 的回调翻成 ServerEvent 流,落到 AttachSink
    // 挂的那只 sink(没挂就只发号不落笔)。每轮各开一只,轮间不共用状态。
    TurnEventAdapter MakeTurnAdapter();

    // ---- 会话权限账 ----------------------------------------------------------
    // "总是允许"的工具集合:确认档按 a、远端审批 accept_for_session 落进来,
    // 本场该工具免问。settings.local.json 的 allow_tools 由装配层注入。
    std::set<std::string>& always_allowed() { return always_allowed_; }

    // ---- 协作模式账(Plan 模式单:两根轴的会话级真值) ------------------------
    // ModeState 归本类所有(单子:"不塞进 LineEditor 的 ConfirmMode 原子"),
    // 前端只读快照。切档走 SetCollaborationMode;事件的落账在 trajectory
    // 侧(control.mode.changed),旧 mode_v1 存档行已随 P0-6 删。
    const ModeState& mode_state() const { return mode_state_; }
    CollaborationMode collaboration_mode() const { return mode_state_.active; }

    // 切档。reason 是稳定短码("slash"/"approved"/"resume"/"clear"),随
    // control.mode.changed 事件落账(装配层接)。permission_before_plan 在
    // 切入 Plan 时由调用方传当前确认档("confirm"/"auto"/"yolo");切回
    // Default 时本类不动确认档——批准框选的新档只改本 session,由装配层落。
    // 返回值:切换是否真的发生(同档重复切给 false)。
    bool SetCollaborationMode(CollaborationMode mode, const std::string& reason,
                              const std::string& permission_before_plan = std::string());
    // resume 专用:只回内存真值,不落事件(档位是回放出来的,再记
    // 一笔会把 resume 当一次切换记账)。
    void RestoreCollaborationMode(CollaborationMode mode, std::uint64_t revision) {
        mode_state_.active = mode;
        mode_state_.revision = revision;
    }

    // ---- 计划成品账 -----------------------------------------------------------
    // 最近一份 PlanDocument(按 revision supersede;/resume 按新账重建)。
    // null = 本场还没交过计划。
    void RecordPlanDocument(const PlanDocument& plan);
    // resume 专用:只回内存真值,不落账(账已在 trajectory,重复落会翻倍)。
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

    // P0-2 轨迹账:每场恒开一只(可选构造;move-only)。
    std::optional<TrajectorySessionLedger> trajectory_;
    std::string trajectory_open_error_;

    std::set<std::string> always_allowed_;

    // Plan 模式单:两轴真值与计划成品账。
    ModeState mode_state_;
    std::optional<PlanDocument> latest_plan_;
};

}  // namespace lubancode::runtime
