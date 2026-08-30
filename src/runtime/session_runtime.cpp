// SessionRuntime 的实现(显示系统剥离单第六步)。
//
// 建档/落盘/标题/压缩序号的账,原文自 InteractiveSession 的 EnsureSessionBegun
// 与 PersistNewMessages 搬来,语义一字不改;区别只有一处:错误不再当场
// std::cout,改用返回值交账,由前端决定怎么印(单子"Runtime 不碰界面")。

#include "runtime/session_runtime.hpp"

#include <utility>

namespace lubancode::runtime {

SessionRuntime::SessionRuntime(Options options) : options_(std::move(options)), store_(options_.sessions_dir) {
    thread_id_ = ids_.NextThreadId();
    // P0-2 轨迹账:flag 开的会话在这里开张(进程一场,LaunchSession)。
    // 开不出来记 error,由装配层决定会话启动失败——本类不回退旧写口。
    if (options_.trajectory_enabled) {
        TrajectorySessionLedger::Options ledger_options;
        ledger_options.workspace_root = options_.trajectory_workspace_root;
        ledger_options.readable_workspace_name = options_.trajectory_workspace_name;
        ledger_options.lubancode_version = options_.lubancode_version;
        auto ledger = TrajectorySessionLedger::Open(std::move(ledger_options));
        if (ledger.has_value()) {
            trajectory_.emplace(std::move(*ledger));
        } else {
            trajectory_open_error_ = ledger.error();
        }
    }
}

SessionRuntime::~SessionRuntime() = default;

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

SessionBeginResult SessionRuntime::EnsureBegun(const std::string& first_text, const std::string& model,
                                               const std::string& cwd) {
    if (options_.trajectory_enabled) {
        // P0-2:单一真账在 Trajectory Journal,旧 SessionStore 不建档
        //(禁 dual-write)。标题等控制事实后续走 control.* 事件。
        return SessionBeginResult::Disabled;
    }
    if (store_.active()) {
        return SessionBeginResult::Active;
    }
    if (options_.sessions_dir.empty() || store_broken_) {
        return SessionBeginResult::Disabled;
    }
    meta_ = sessions::SessionMeta{};
    meta_.wire = options_.wire_name;
    meta_.model = model;
    meta_.cwd = cwd;
    meta_.started_at = sessions::NowTimestamp();
    const std::string session_id = sessions::MakeSessionId(options_.start_ts, first_text);
    if (!store_.Begin(meta_, session_id)) {
        store_broken_ = true;
        return SessionBeginResult::Failed;
    }
    // 建档前设过的标题:现在有文件了,把事件行补上。
    if (title_pending_ && !title_.empty()) {
        store_.AppendTitleEvent(title_);
    }
    title_pending_ = false;
    // Plan 模式单:建档前切过的协作档(存档未开时只记了内存)补落 mode_v1。
    if (pending_mode_event_.has_value()) {
        store_.AppendModeEvent(*pending_mode_event_);
        pending_mode_event_.reset();
    }
    // /think history 同一道补落(P1):起手切过跨轮保留再开聊,档里也留账。
    if (pending_think_history_.has_value()) {
        store_.AppendThinkHistoryEvent(*pending_think_history_);
        pending_think_history_.reset();
    }
    return SessionBeginResult::Begun;
}

SessionPersistResult SessionRuntime::PersistNew(const std::vector<api::Message>& history, const std::string& model,
                                                const std::string& cwd) {
    if (options_.trajectory_enabled) {
        // P0-2:轮末补抄停用(§15.3"删除轮末按 persisted_count_ 扫 history
        // 追加这条路")。轨迹路的事实已随事件即时落账。
        return SessionPersistResult::Nothing;
    }
    if (options_.sessions_dir.empty() || store_broken_) {
        return SessionPersistResult::Nothing;
    }
    if (history.size() <= persisted_count_) {
        return SessionPersistResult::Nothing;
    }
    if (!store_.active()) {
        // 首条用户消息的第一段文本做 slug(图片消息拿文件名)。
        std::string first_text;
        for (const auto& message : history) {
            if (message.role != api::Role::User) {
                continue;
            }
            for (const auto& block : message.content) {
                if (const auto* tb = std::get_if<api::TextBlock>(&block)) {
                    first_text = tb->text;
                    break;
                }
                if (const auto* image = std::get_if<api::ImageBlock>(&block)) {
                    first_text = image->filename;
                    break;
                }
            }
            break;
        }
        // 正路(RunUserTurn)已建档;这里是兜底(peer 轮等不经正路的路径)。
        const SessionBeginResult begun = EnsureBegun(first_text, model, cwd);
        if (begun != SessionBeginResult::Begun && begun != SessionBeginResult::Active) {
            return SessionPersistResult::Nothing;
        }
    }
    for (std::size_t i = persisted_count_; i < history.size(); ++i) {
        if (!store_.AppendMessage(history[i])) {
            store_broken_ = true;
            return SessionPersistResult::BrokenNow;
        }
    }
    persisted_count_ = history.size();
    return SessionPersistResult::Appended;
}

void SessionRuntime::ClampPersisted(std::size_t history_size) {
    if (persisted_count_ > history_size) {
        persisted_count_ = history_size;
    }
}

bool SessionRuntime::SetCollaborationMode(CollaborationMode mode, const std::string& reason,
                                          const std::string& permission_before_plan) {
    if (mode_state_.active == mode) {
        return false;  // 同档重复切:不动账、不落事件行
    }
    mode_state_.active = mode;
    mode_state_.revision += 1;
    if (mode == CollaborationMode::Plan) {
        mode_state_.permission_before_plan = permission_before_plan;
    }
    // 存档活跃就落 mode_v1(append+flush);没建档先挂起(EnsureBegun 补落),
    // 写坏不拦切档——内存真值在,档是加层(与 queue 事件同取舍)。
    sessions::ModeEvent event;
    event.mode = ToString(mode);
    event.reason = reason;
    event.revision = mode_state_.revision;
    if (store_.active() && !store_broken_) {
        store_.AppendModeEvent(event);
    } else {
        pending_mode_event_ = event;
    }
    return true;
}

void SessionRuntime::RecordThinkHistory(const std::string& mode) {
    // /think history 的会话账(Kimi 保留式思考单 P1):存档活跃就落
    // think_history_v1;没建档先挂起(EnsureBegun 补落),写坏不拦切换。
    // 同档重复落也无害(append-only 最后一条胜),调用方自己省了就不落。
    sessions::ThinkHistoryEvent event;
    event.mode = mode;
    if (store_.active() && !store_broken_) {
        store_.AppendThinkHistoryEvent(event);
    } else {
        pending_think_history_ = event;
    }
}

void SessionRuntime::RecordPlanDocument(const PlanDocument& plan) {
    // 新稿 supersede 旧稿(旧账仍在 session 事件行,不删)。
    if (latest_plan_.has_value() && latest_plan_->plan_id == plan.plan_id &&
        latest_plan_->state == PlanReviewState::Presented) {
        PlanDocument superseded = *latest_plan_;
        superseded.state = PlanReviewState::Superseded;
        latest_plan_ = superseded;
        // supersede 也留一行账:审阅历史看得见"哪稿被哪稿顶了"。
        if (store_.active() && !store_broken_) {
            sessions::PlanEvent event;
            event.plan_id = superseded.plan_id;
            event.revision = superseded.revision;
            event.state = ToString(superseded.state);
            event.sha256 = superseded.content_sha256;
            event.turn_id = superseded.source_turn_id;
            store_.AppendPlanEvent(event);
        }
    }
    latest_plan_ = plan;
    mode_state_.latest_plan_id = plan.plan_id;
    if (store_.active() && !store_broken_) {
        sessions::PlanEvent event;
        event.plan_id = plan.plan_id;
        event.revision = plan.revision;
        event.state = ToString(plan.state);
        event.sha256 = plan.content_sha256;
        // 内联上限:超限不塞事件行(装配层已落 artifact,这里带引用)。
        if (plan.markdown.size() <= kPlanMarkdownInlineCap) {
            event.markdown = plan.markdown;
        } else {
            event.artifact_ref = plan.artifact_ref;
        }
        event.turn_id = plan.source_turn_id;
        store_.AppendPlanEvent(event);
    }
}

SessionRuntime::PlanReviewOutcome SessionRuntime::ReviewPlan(const std::string& plan_id, std::uint64_t revision,
                                                             const std::string& sha256, bool approve) {
    if (!latest_plan_.has_value() || latest_plan_->plan_id != plan_id ||
        latest_plan_->revision != revision || latest_plan_->content_sha256 != sha256) {
        return PlanReviewOutcome::Stale;  // 旧 dialog 的迟到回答,不落账
    }
    latest_plan_->state = approve ? PlanReviewState::Approved : PlanReviewState::Rejected;
    if (store_.active() && !store_broken_) {
        sessions::PlanReviewEvent event;
        event.plan_id = plan_id;
        event.revision = revision;
        event.decision = approve ? "approved" : "rejected";
        store_.AppendPlanReviewEvent(event);
    }
    return approve ? PlanReviewOutcome::Approved : PlanReviewOutcome::Rejected;
}

}  // namespace lubancode::runtime
