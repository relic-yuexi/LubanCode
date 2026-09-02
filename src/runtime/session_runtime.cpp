// SessionRuntime 的实现(显示系统剥离单第六步)。
//
// P0-2(Trajectory 升为唯一 Session)之后本类只剩三件事:开账、cwd 对账、
// 权限/模式/计划的内存真值。旧 SessionStore 的建档/轮末补抄/事件行
// append 已随 P0-6 删净——事实由 typed 事件即时落账。

#include "runtime/session_runtime.hpp"

#include <utility>

#include "config/config.hpp"      // HomeLubancodeDir:身份裁决的全局件止步
#include "platform/paths.hpp"
#include "tools/path_utils.hpp"   // Utf8ToPath
#include "workspace/identity.hpp"

namespace lubancode::runtime {

SessionRuntime::SessionRuntime(Options options) : options_(std::move(options)) {
    thread_id_ = ids_.NextThreadId();
    // P0-2(Trajectory 升为唯一 Session):恒开一场(进程一场,
    // LaunchSession/resume-as-new)。开不出来记 error,由装配层让会话启动
    // 失败——本类不回退旧写口,也不留"先聊聊再落账"的暗门。
    TrajectorySessionLedger::Options ledger_options;
    ledger_options.workspaces_root = options_.trajectory_workspaces_root;
    ledger_options.workspace_identity = options_.trajectory_workspace_identity;
    ledger_options.lubancode_version = options_.lubancode_version;
    ledger_options.resume_at_launch = options_.trajectory_resume_at_launch;
    ledger_options.resume_source_session_id = options_.trajectory_resume_source_session_id;
    ledger_options.approval_mode = options_.approval_mode;
    auto ledger = TrajectorySessionLedger::Open(std::move(ledger_options));
    if (ledger.has_value()) {
        trajectory_.emplace(std::move(*ledger));
    } else {
        trajectory_open_error_ = ledger.error();
    }
}

SessionRuntime::~SessionRuntime() = default;

std::string SessionRuntime::NoteWorkingDirectoryChanged(const std::filesystem::path& new_cwd) {
    std::filesystem::path home_dir;
    if (const auto home = config::HomeLubancodeDir(); home.has_value()) {
        home_dir = tools::Utf8ToPath(*home);
    }
    auto identity = workspace::ResolveWorkspaceIdentity(new_cwd, home_dir);
    if (!identity.has_value()) {
        return identity.error();
    }
    if (trajectory_.has_value()) {
        auto change = trajectory_->HandleCwdChange(*identity);
        if (change.same_workspace) {
            return change.error;
        }
        // 跨 workspace:封当前场,再在新 workspace 开新场(§4.5 不许往旧房
        // 搬账)。旧场留在旧 workspace,sessions 索引可查可 resume。新场落
        // 同一个持久化根(从旧场 session 目录三层上推:session 在
        // <root>/<key>/sessions/<id>,布局是合同)。
        const std::filesystem::path workspaces_root =
            trajectory_->session_dir().parent_path().parent_path().parent_path();
        trajectory_->CloseSession("workspace_switch");
        TrajectorySessionLedger::Options ledger_options;
        ledger_options.workspaces_root = workspaces_root;
        ledger_options.workspace_identity = std::move(*identity);
        ledger_options.lubancode_version = options_.lubancode_version;
        ledger_options.approval_mode = options_.approval_mode;
        ledger_options.launch_cwd = platform::PathToUtf8(ledger_options.workspace_identity.launch_cwd);
        auto ledger = TrajectorySessionLedger::Open(std::move(ledger_options));
        if (!ledger.has_value()) {
            return ledger.error();
        }
        // ledger 的 move 赋值删了(引用成员),optional 走 reset + emplace。
        trajectory_.reset();
        trajectory_.emplace(std::move(*ledger));
        return {};
    }
    TrajectorySessionLedger::Options ledger_options;
    ledger_options.workspace_identity = std::move(*identity);
    ledger_options.lubancode_version = options_.lubancode_version;
    ledger_options.launch_cwd = platform::PathToUtf8(ledger_options.workspace_identity.launch_cwd);
    auto ledger = TrajectorySessionLedger::Open(std::move(ledger_options));
    if (!ledger.has_value()) {
        return ledger.error();
    }
    trajectory_.reset();
    trajectory_.emplace(std::move(*ledger));
    return {};
}

TurnEventAdapter SessionRuntime::MakeTurnAdapter() {
    // 适配器按值构造会拷 map/串——MoveCallbacks 的正确姿势是调用方写
    // auto adapter = runtime.MakeTurnAdapter();。构造函数捕获 thread_id_
    // 与 ids_ 引用,轮内不再变。
    // 批二接线补完:落点自动挂到 AttachSink 装的那只(没挂就只发号不落
    // 笔)。装配点从此只管配 sink,不再各自手拼回调。
    TurnEventAdapter adapter(thread_id_, ids_);
    if (sink_ != nullptr) {
        EventSink* sink = sink_;
        adapter.Attach([sink](const ServerEvent& event) { sink->Emit(event); });
    }
    return adapter;
}

bool SessionRuntime::SetCollaborationMode(CollaborationMode mode, const std::string& reason,
                                          const std::string& permission_before_plan) {
    if (mode_state_.active == mode) {
        return false;  // 同档重复切:不动账
    }
    mode_state_.active = mode;
    mode_state_.revision += 1;
    if (mode == CollaborationMode::Plan) {
        mode_state_.permission_before_plan = permission_before_plan;
    }
    // 档位变更的事件账(control.mode.changed)由装配层接 trajectory 落;
    // 本类只管内存真值。
    (void)reason;
    return true;
}

void SessionRuntime::RecordPlanDocument(const PlanDocument& plan) {
    // 新稿 supersede 旧稿;事件账在 trajectory 侧,这里只留内存真值。
    if (latest_plan_.has_value() && latest_plan_->plan_id == plan.plan_id &&
        latest_plan_->state == PlanReviewState::Presented) {
        PlanDocument superseded = *latest_plan_;
        superseded.state = PlanReviewState::Superseded;
        latest_plan_ = superseded;
    }
    latest_plan_ = plan;
    mode_state_.latest_plan_id = plan.plan_id;
}

SessionRuntime::PlanReviewOutcome SessionRuntime::ReviewPlan(const std::string& plan_id, std::uint64_t revision,
                                                             const std::string& sha256, bool approve) {
    if (!latest_plan_.has_value() || latest_plan_->plan_id != plan_id ||
        latest_plan_->revision != revision || latest_plan_->content_sha256 != sha256) {
        return PlanReviewOutcome::Stale;  // 旧 dialog 的迟到回答,不落账
    }
    latest_plan_->state = approve ? PlanReviewState::Approved : PlanReviewState::Rejected;
    return approve ? PlanReviewOutcome::Approved : PlanReviewOutcome::Rejected;
}

}  // namespace lubancode::runtime
