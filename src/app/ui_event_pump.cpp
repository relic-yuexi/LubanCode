// UiEventPump 的实现(终端画面隔网批:按帧落屏的消费侧)。渲染闭包仍是
// 既有直写路(TerminalTurnSink::RenderEvent),按帧节拍收批——洪峰并成
// 一帧批、一帧一次落屏;零散节奏投递即醒立刻画,与老路同延迟。

#include "app/ui_event_pump.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

#include "cli/terminal_port.hpp"  // TermErr:消费线程的兜底报信
#include "runtime/event.hpp"

namespace lubancode::app {

namespace {

// 按帧落屏的缺省节拍(约 30 帧/秒,单上"16~33ms 一帧"的保守端)。
constexpr int kDefaultFrameMs = 33;

std::chrono::milliseconds ClampFrameMs(long long ms) {
    if (ms <= 0) {
        return std::chrono::milliseconds(0);  // 老路:投递即醒立即画
    }
    if (ms > 1000) {
        ms = 1000;
    }
    return std::chrono::milliseconds(ms);
}

}  // namespace

std::chrono::milliseconds UiEventPump::FrameIntervalFromEnv() {
    static const std::chrono::milliseconds cached = [] {
        const char* raw = std::getenv("LUBANCODE_UI_FRAME_MS");
        if (raw == nullptr || *raw == '\0') {
            return std::chrono::milliseconds(kDefaultFrameMs);
        }
        try {
            return ClampFrameMs(std::stoll(raw));
        } catch (...) {
            return std::chrono::milliseconds(kDefaultFrameMs);
        }
    }();
    return cached;
}

UiEventPump::UiEventPump(Renderer renderer, std::chrono::milliseconds frame_interval)
    : renderer_(std::move(renderer)), frame_interval_(frame_interval) {
    consumer_ = std::thread([this] { ConsumerMain(); });
}

UiEventPump::~UiEventPump() { StopAndDrain(); }

void UiEventPump::PostDelta(const runtime::ServerEvent& event) {
    bool stopped_now = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // 停表检查收进队列锁:与 StopAndDrain 的置停互斥。锁外检查则有
        // "查得 false、置停全套跑完、这里才入队"的窗口——余量成死账,
        // 消费线程已收工,永不画。
        if (stopped_.load(std::memory_order_acquire)) {
            stopped_now = true;
        } else {
            // 合并:队尾是同 item 的同类 ItemDelta 就地拼接。正文与思考
            // 各占一枚 item,思考块收束后正文另起 item,不会错拼到一起。
            if (!pending_.empty() &&
                pending_.back().kind == runtime::ServerEventKind::ItemDelta &&
                event.kind == runtime::ServerEventKind::ItemDelta &&
                pending_.back().item_id == event.item_id &&
                pending_.back().item_kind == event.item_kind) {
                pending_.back().text += event.text;
            } else {
                pending_.push_back(event);
            }
        }
    }
    if (stopped_now) {
        // 停表后的迟到流式事件(Stop 钩子续跑的正文):退化成就地画,
        // 与老路一字不差。画笔锁在队列锁外拿——锁序恒 render->queue,
        // 两锁绝不 nested 反持。
        std::lock_guard<std::mutex> render(render_mutex_);
        renderer_(event);
        return;
    }
    // 投递即醒:消费线程醒了再决定要不要等帧边界(见 ConsumerMain)——
    // 稀疏节奏立刻画,洪峰自然并进下一帧。
    wake_.notify_one();
}

void UiEventPump::DispatchInline(const runtime::ServerEvent& event) {
    std::lock_guard<std::mutex> render(render_mutex_);
    DrainLocked();  // 先排干 pending 的流式事件:正文先于工具卡,次序同老路
    renderer_(event);
}

void UiEventPump::StopAndDrain() {
    {
        // 置停收进队列锁:首等待的谓词检查在锁内、"入队+释锁"原子完成,
        // 锁内置停便与二者必有序——store 晚于检查,则 notify_all 时等待者
        // 必已入队,一定叫得醒;store 早于检查,谓词必见 true。若在锁外
        // 置停+notify,存在"谓词已查过(false)、等待者尚未入队"的窗口,
        // notify_all 空打,消费线程睡回无 deadline 的首等待,join 永等
        // ——CI run 33887350246 ubuntu 腿 180s 挂死即此(栈:主线程 join,
        // 消费线程 ui_event_pump.cpp 首等待)。
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stopped_.store(true, std::memory_order_release);
    }
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
    // 帧节拍账:上一帧落屏时刻。首帧(epoch)不等待——稀疏节奏下第一枚
    // delta 投递即画,延迟与老路相同;此后一帧间隔内的增量都在队尾合并,
    // 到点一口气收,一帧一次落屏。
    std::chrono::steady_clock::time_point last_frame{};
    for (;;) {
        bool batch_has_delta = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            wake_.wait(lock, [this] {
                return stopped_.load(std::memory_order_acquire) || !pending_.empty();
            });
            if (stopped_.load(std::memory_order_acquire)) {
                return;
            }
            for (const runtime::ServerEvent& event : pending_) {
                if (event.kind == runtime::ServerEventKind::ItemDelta) {
                    batch_has_delta = true;
                    break;
                }
            }
        }
        // 按帧等待(条 2/5):批里是流式 delta、距上一帧不足一帧——睡到帧
        // 边界再收。等待持有队列锁但谓词只认 stopped_(投递的 notify 顶多
        // 白醒一次,醒来谓词不成立便继续睡到点),StopAndDrain 的 notify_all
        // 一定叫得动。控制路事件不等帧:DispatchInline 在产生线程就地排干,
        // 次序照旧。
        if (batch_has_delta && frame_interval_.count() > 0 &&
            last_frame.time_since_epoch().count() != 0) {
            const std::chrono::steady_clock::time_point deadline = last_frame + frame_interval_;
            if (std::chrono::steady_clock::now() < deadline) {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                wake_.wait_until(lock, deadline,
                                 [this] { return stopped_.load(std::memory_order_acquire); });
                if (stopped_.load(std::memory_order_acquire)) {
                    return;
                }
            }
        }
        // 排干这一批(画笔锁 + 既有渲染闭包)。渲染异常接住、落一行
        // stderr、继续伺候下一批——老路上这类异常会穿过 libcurl 的 C 回调
        // (UB),挪出回调之后才有得接;出了异常也记一帧时戳,别叫坏帧
        // 之后的重试连环空转。
        try {
            std::lock_guard<std::mutex> render(render_mutex_);
            DrainLocked();
            if (batch_has_delta) {
                last_frame = std::chrono::steady_clock::now();
            }
        } catch (const std::exception& e) {
            lubancode::cli::TermErr() << "\n[ui-pump] " << e.what() << "\n";
            lubancode::cli::TermErr().flush();
            last_frame = std::chrono::steady_clock::now();
        } catch (...) {
            lubancode::cli::TermErr() << "\n[ui-pump] unknown exception\n";
            lubancode::cli::TermErr().flush();
            last_frame = std::chrono::steady_clock::now();
        }
    }
}

}  // namespace lubancode::app
