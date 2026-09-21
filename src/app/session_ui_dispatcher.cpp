// SessionUiDispatcher 的实现(按代理状态投影单 P2:收拢写者)。队列/
// 消费/合并的机制自 UiEventPump 移植,三处 P2 的差:渲染器按世代登记
// (sink 每轮一只,迟到命令不悬垂)、RunSync 在调用线程排干+就地执行
// (不跨线程等锁)、统一提交锁成为一切落笔的核对位。HC-02 再补一道
// 顺序闸:出队与执行捆成一个闸单元,先提交先执行——设计见头文件。

#include "app/session_ui_dispatcher.hpp"

#include <cstdio>

namespace lubancode::app {

void SessionUiDispatcher::Start() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (started_ || stopped_.load()) {
            return;
        }
        started_ = true;
    }
    consumer_ = std::thread([this] { ConsumerMain(); });
}

void SessionUiDispatcher::Stop() {
    std::thread consumer;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!started_) {
            return;
        }
        started_ = false;
        stopped_.store(true);
        consumer = std::move(consumer_);
        wake_.notify_all();
    }
    if (consumer.joinable()) {
        consumer.join();
    }
    // 停表后的余量(停表与消费的夹缝里进来的)就地排干,不丢事实。同样
    // 过顺序闸(HC-02):并发还在跑的 RunSync 单元先落定,收尾排干不插队。
    {
        std::lock_guard<std::recursive_mutex> order(order_mutex_);
        RunEntriesOnCaller(StealPending());
    }
    consumer_id_.store(std::thread::id{});
    {
        std::lock_guard<std::mutex> lock(renderers_mutex_);
        renderers_.clear();
    }
}

std::uint64_t SessionUiDispatcher::AttachRenderer(EventRenderer renderer) {
    std::lock_guard<std::mutex> lock(renderers_mutex_);
    const std::uint64_t id = next_renderer_id_++;
    renderers_.emplace_back(id, std::move(renderer));
    return id;
}

void SessionUiDispatcher::DetachRenderer(std::uint64_t renderer_id) {
    std::lock_guard<std::mutex> lock(renderers_mutex_);
    for (auto it = renderers_.begin(); it != renderers_.end(); ++it) {
        if (it->first == renderer_id) {
            renderers_.erase(it);
            return;
        }
    }
}

bool SessionUiDispatcher::PostEvent(std::uint64_t renderer_id, const runtime::ServerEvent& event) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stopped_.load() || !started_) {
            return false;  // 停表:调用方自行处置(sink 的迟到事件走就地退化)
        }
        // 合并:队尾相邻、同渲染器、同 item 的 ItemDelta(正文/思考)就地
        // 拼接——与旧泵 PostDelta 同款,洪峰不涨元素数。
        if (event.kind == runtime::ServerEventKind::ItemDelta && !pending_.empty()) {
            Entry& tail = pending_.back();
            if (tail.renderer_id != 0 && tail.renderer_id == renderer_id &&
                tail.event.kind == runtime::ServerEventKind::ItemDelta &&
                tail.event.item_id == event.item_id && tail.event.item_kind == event.item_kind) {
                tail.event.text += event.text;
                tail.event.envelope.seq = event.envelope.seq;
                return true;
            }
        }
        Entry entry;
        entry.renderer_id = renderer_id;
        entry.event = event;
        pending_.push_back(std::move(entry));
    }
    wake_.notify_one();
    return true;
}

std::future<void> SessionUiDispatcher::PostAction(std::function<void()> action) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> future = done->get_future();
    bool enqueue = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        enqueue = !stopped_.load() && started_;
        if (enqueue) {
            Entry entry;
            entry.done = std::move(done);
            entry.action = std::move(action);
            pending_.push_back(std::move(entry));
        }
    }
    if (enqueue) {
        wake_.notify_one();
        return future;
    }
    // 停表:就地执行,回执即成。异常吞进 future(与异步路同貌)。与一切
    // 执行路同一道顺序闸+提交锁(HC-02)——收尾排干进行中也不插队;顺带
    // 把旧款"在队锁里就地跑"修掉(嵌套投递在队锁上会自锁)。
    std::lock_guard<std::recursive_mutex> order(order_mutex_);
    std::lock_guard<std::recursive_mutex> commit(commit_mutex_);
    try {
        action();
        done->set_value();
    } catch (...) {
        done->set_exception(std::current_exception());
    }
    return future;
}

std::vector<SessionUiDispatcher::Entry> SessionUiDispatcher::StealPending() {
    std::vector<Entry> stolen;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    stolen.reserve(pending_.size());
    while (!pending_.empty()) {
        stolen.push_back(std::move(pending_.front()));
        pending_.pop_front();
    }
    // 在飞记账与出队同一把队锁:Quiesce 的判据(pending 空 && inflight 0)
    // 不存在"已出队、还没计账"的窗——否则静默屏障会提前放行,收口 chrome
    // 与在飞渲染之间又见了缝。
    inflight_ += stolen.size();
    return stolen;
}

void SessionUiDispatcher::RunSync(std::function<void()> body) {
    if (body == nullptr) {
        return;
    }
    // 顺序闸(HC-02):"排干+body"是一个闸单元。在飞的旧批(已被消费
    // 线程或另一路 RunSync 领走、还没落笔的)先落定,本单元才领队列——
    // 不然新批先抢提交锁,先提交的后执行,开始/结束事件就配不上对。
    // 排干余量(FIFO 保住"先提交的先落笔"),再在提交锁内就地执行:
    // 调用线程即执行线程,见文件头。
    std::lock_guard<std::recursive_mutex> order(order_mutex_);
    RunEntriesOnCaller(StealPending());
    std::lock_guard<std::recursive_mutex> commit(commit_mutex_);
    body();
}

void SessionUiDispatcher::Flush() {
    // 排干也过顺序闸(HC-02):与在飞批互不插队,出队次序即提交次序。
    std::lock_guard<std::recursive_mutex> order(order_mutex_);
    RunEntriesOnCaller(StealPending());
}

void SessionUiDispatcher::Quiesce() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    while (!pending_.empty() || inflight_ != 0) {
        idle_.wait(lock);
        // 醒来重查:叫醒时可能又有新提交,或另一路 RunSync 正在就地执行
        // (inflight_ 也计它)。停表路(Stop 之后)消费线程已 join,这里
        // 只会等就地路收尾,有限。
    }
}

void SessionUiDispatcher::RunEntriesOnCaller(std::vector<Entry> entries) {
    if (!entries.empty()) {
        // inflight_ 的加账在出队处(StealPending/消费线程的同一把队锁),
        // 这里只减账:执行中始终计入,Quiesce 不会提前放行。
        for (Entry& entry : entries) {
            std::lock_guard<std::recursive_mutex> commit(commit_mutex_);
            ExecuteEntryLocked(entry);
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            inflight_ -= entries.size();
            if (pending_.empty() && inflight_ == 0) {
                idle_.notify_all();
            }
        }
    }
}

void SessionUiDispatcher::ExecuteEntryLocked(const Entry& entry) {
    if (entry.renderer_id != 0) {
        EventRenderer renderer = LookupRenderer(entry.renderer_id);
        if (renderer == nullptr) {
            return;  // 世代已摘(回合收口):迟到的事件命令整枚丢弃
        }
        try {
            renderer(entry.event);
        } catch (const std::exception& e) {
            // 消费路上的异常不叫整条队列暴毙(旧泵同款);RunSync 直呼路
            // 由 LookupRenderer 之后的调用方包裹,这里只兜异步路。
            std::fprintf(stderr, "[session-ui] event render failed: %s\n", e.what());
            std::fflush(stderr);
        }
        return;
    }
    if (entry.action == nullptr) {
        return;
    }
    try {
        entry.action();
        if (entry.done != nullptr) {
            entry.done->set_value();
        }
    } catch (...) {
        // 异步动作的异常进 promise(等待方拿到),不穿线程——消费线程
        // 暴毙会拖垮整场会话。done 为空的防御路同样吞掉,stderr 留名。
        if (entry.done != nullptr) {
            entry.done->set_exception(std::current_exception());
        }
        std::fprintf(stderr, "[session-ui] action failed\n");
        std::fflush(stderr);
    }
}

SessionUiDispatcher::EventRenderer SessionUiDispatcher::LookupRenderer(std::uint64_t renderer_id) {
    std::lock_guard<std::mutex> lock(renderers_mutex_);
    for (const auto& [id, renderer] : renderers_) {
        if (id == renderer_id) {
            return renderer;
        }
    }
    return nullptr;
}

void SessionUiDispatcher::ConsumerMain() {
    consumer_id_.store(std::this_thread::get_id());
    while (true) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            wake_.wait(lock, [this] { return stopped_.load() || !pending_.empty(); });
            if (pending_.empty()) {
                if (stopped_.load()) {
                    idle_.notify_all();
                    return;
                }
                continue;
            }
        }
        // 有活:先过顺序闸(HC-02)再出队——出队与执行同在一个闸单元。
        // 等闸时可能正有另一路(RunSync/Flush/Stop 排干)在执行自己的
        // 单元:它领不走本线程已看见的命令,本线程也不抢它手里的批。
        std::lock_guard<std::recursive_mutex> order(order_mutex_);
        std::vector<Entry> batch = StealPending();
        if (batch.empty()) {
            continue;  // 队列被同在闸内的并发单元领走了,回去接着等
        }
        // 测试钉子:批已出队、第一枚未提交——正是 HC-02 的病窗,单测在
        // 此拿屏障钉出确定性交错。
        FireDebugHook(DebugPoint::ConsumerBatchStolen);
        RunEntriesOnCaller(std::move(batch));
    }
}

void SessionUiDispatcher::SetDebugHook(std::function<void(DebugPoint)> hook) {
    std::lock_guard<std::mutex> lock(debug_hook_mutex_);
    debug_hook_ = std::move(hook);
}

void SessionUiDispatcher::FireDebugHook(DebugPoint point) {
    std::function<void(DebugPoint)> hook;
    {
        std::lock_guard<std::mutex> lock(debug_hook_mutex_);
        hook = debug_hook_;
    }
    if (hook == nullptr) {
        return;
    }
    try {
        hook(point);
    } catch (const std::exception& e) {
        // 钉子只归测试用;钩内抛异常不该拖垮消费线程(与事件渲染同款兜底)。
        std::fprintf(stderr, "[session-ui] debug hook failed: %s\n", e.what());
        std::fflush(stderr);
    } catch (...) {
        std::fprintf(stderr, "[session-ui] debug hook failed\n");
        std::fflush(stderr);
    }
}

std::size_t SessionUiDispatcher::PendingApprox() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return pending_.size();
}

}  // namespace lubancode::app
