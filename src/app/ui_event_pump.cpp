// UiEventPump 的实现(终端画面隔网先行批)。消费侧是过渡档:凑批后仍调
// 既有直写渲染闭包,画面与老路逐字节一致;按帧落屏的正式消费侧后续批换。

#include "app/ui_event_pump.hpp"

#include <exception>
#include <utility>

#include "cli/terminal_port.hpp"  // TermErr:消费线程的兜底报信

namespace lubancode::app {

UiEventPump::UiEventPump(Renderer renderer) : renderer_(std::move(renderer)) {
    consumer_ = std::thread([this] { ConsumerMain(); });
}

UiEventPump::~UiEventPump() { StopAndDrain(); }

void UiEventPump::PostDelta(const runtime::ServerEvent& event) {
    if (stopped_.load(std::memory_order_acquire)) {
        // 停表后的迟到流式事件(Stop 钩子续跑的正文):退化成就地画,
        // 与老路一字不差。
        std::lock_guard<std::mutex> render(render_mutex_);
        renderer_(event);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // 合并:队尾是同 item 的同类 ItemDelta 就地拼接。正文与思考各占
        // 一枚 item,思考块收束后正文另起 item,不会错拼到一起。
        if (!pending_.empty() && pending_.back().kind == runtime::ServerEventKind::ItemDelta &&
            event.kind == runtime::ServerEventKind::ItemDelta && pending_.back().item_id == event.item_id &&
            pending_.back().item_kind == event.item_kind) {
            pending_.back().text += event.text;
        } else {
            pending_.push_back(event);
        }
    }
    // 投递即醒:零散节奏(人手打字般)延迟与老路相同;洪峰下消费线程忙着
    // 画上一批,生产期的增量全并在队列里,醒来一口气收——凑批不靠计时。
    wake_.notify_one();
}

void UiEventPump::DispatchInline(const runtime::ServerEvent& event) {
    std::lock_guard<std::mutex> render(render_mutex_);
    DrainLocked();  // 先排干 pending 的流式事件:正文先于工具卡,次序同老路
    renderer_(event);
}

void UiEventPump::StopAndDrain() {
    stopped_.store(true, std::memory_order_release);
    wake_.notify_all();
    if (consumer_.joinable()) {
        consumer_.join();
    }
    std::lock_guard<std::mutex> render(render_mutex_);
    DrainLocked();  // 余量在调用线程就地画完,一个 delta 不丢
}

void UiEventPump::DrainLocked() {
    std::deque<runtime::ServerEvent> batch;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        batch.swap(pending_);
    }
    for (const runtime::ServerEvent& event : batch) {
        renderer_(event);
    }
}

void UiEventPump::ConsumerMain() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            wake_.wait(lock, [this] {
                return stopped_.load(std::memory_order_acquire) || !pending_.empty();
            });
            if (stopped_.load(std::memory_order_acquire)) {
                return;
            }
        }
        // 排干这一批(画笔锁 + 既有直写闭包)。渲染异常接住、落一行
        // stderr、继续伺候下一批——老路上这类异常会穿过 libcurl 的 C
        // 回调(UB),挪出回调之后才有得接。
        try {
            std::lock_guard<std::mutex> render(render_mutex_);
            DrainLocked();
        } catch (const std::exception& e) {
            lubancode::cli::TermErr() << "\n[ui-pump] " << e.what() << "\n";
            lubancode::cli::TermErr().flush();
        } catch (...) {
            lubancode::cli::TermErr() << "\n[ui-pump] unknown exception\n";
            lubancode::cli::TermErr().flush();
        }
    }
}

}  // namespace lubancode::app
