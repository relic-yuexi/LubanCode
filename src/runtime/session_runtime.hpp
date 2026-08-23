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
#include <set>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "api/types.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_event_adapter.hpp"

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

    // 开一轮的事件适配器:把 loop 的回调翻成 ServerEvent 流,落到 AttachSink
    // 挂的那只 sink(没挂就只发号不落笔)。每轮各开一只,轮间不共用状态。
    TurnEventAdapter MakeTurnAdapter();

    // ---- 会话存档账(本类持有,控制器按引用续用) ----------------------------
    agent::SessionStore& store() { return store_; }
    const agent::SessionStore& store() const { return store_; }
    const std::string& sessions_dir() const { return options_.sessions_dir; }
    const std::string& wire_name() const { return options_.wire_name; }
    const std::string& start_ts() const { return options_.start_ts; }
    agent::SessionMeta& meta() { return meta_; }
    std::string& title() { return title_; }
    bool& title_pending() { return title_pending_; }
    std::size_t& persisted_count() { return persisted_count_; }
    int& compact_epoch() { return compact_epoch_; }
    bool& store_broken() { return store_broken_; }

    // 建档:meta 填账 + Begin + 建档前挂起的标题补事件行。失败置 broken。
    // 首条文本做 slug;model/cwd 由调用方给(会话模型与目录是控制器的活)。
    SessionBeginResult EnsureBegun(const std::string& first_text, const std::string& model,
                                   const std::string& cwd);

    // 增量落盘:history 里 persisted_count 之后逐条追加(只增不减);store
    // 还没开张时先按兜底建档(首条用户文本抽出来做 slug)。失败置 broken。
    SessionPersistResult PersistNew(const std::vector<api::Message>& history, const std::string& model,
                                    const std::string& cwd);

    // 落盘基线收到新长度(/compact、microcompact 换史后由调用方校正;
    // 只收不放,防旧账重写)。
    void ClampPersisted(std::size_t history_size);

    // ---- 会话权限账 ----------------------------------------------------------
    // "总是允许"的工具集合:确认档按 a、远端审批 accept_for_session 落进来,
    // 本场该工具免问。settings.local.json 的 allow_tools 由装配层注入。
    std::set<std::string>& always_allowed() { return always_allowed_; }

private:
    Options options_;
    IdAuthority ids_;
    std::string thread_id_;
    EventSink* sink_ = nullptr;

    agent::SessionStore store_;
    agent::SessionMeta meta_{};
    std::string title_;
    bool title_pending_ = false;
    std::size_t persisted_count_ = 0;
    int compact_epoch_ = 0;
    bool store_broken_ = false;

    std::set<std::string> always_allowed_;
};

}  // namespace lubancode::runtime
