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
}

SessionRuntime::~SessionRuntime() = default;

TurnEventAdapter SessionRuntime::MakeTurnAdapter(agent::AgentLoop& loop) {
    // 适配器按值构造会拷 map/串——MoveCallbacks 的正确姿势是调用方写
    // auto adapter = runtime.MakeTurnAdapter(loop);。构造函数捕获 thread_id_
    // 与 ids_ 引用,轮内不再变。
    return TurnEventAdapter(thread_id_, loop, ids_);
}

SessionBeginResult SessionRuntime::EnsureBegun(const std::string& first_text, const std::string& model,
                                               const std::string& cwd) {
    if (store_.active()) {
        return SessionBeginResult::Active;
    }
    if (options_.sessions_dir.empty() || store_broken_) {
        return SessionBeginResult::Disabled;
    }
    meta_ = agent::SessionMeta{};
    meta_.wire = options_.wire_name;
    meta_.model = model;
    meta_.cwd = cwd;
    meta_.started_at = agent::NowTimestamp();
    const std::string session_id = agent::MakeSessionId(options_.start_ts, first_text);
    if (!store_.Begin(meta_, session_id)) {
        store_broken_ = true;
        return SessionBeginResult::Failed;
    }
    // 建档前设过的标题:现在有文件了,把事件行补上。
    if (title_pending_ && !title_.empty()) {
        store_.AppendTitleEvent(title_);
    }
    title_pending_ = false;
    return SessionBeginResult::Begun;
}

SessionPersistResult SessionRuntime::PersistNew(const std::vector<api::Message>& history, const std::string& model,
                                                const std::string& cwd) {
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

}  // namespace lubancode::runtime
