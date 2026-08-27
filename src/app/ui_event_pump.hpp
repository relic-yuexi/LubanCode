// UiEventPump(终端画面隔网批:条 1 的队列 + 条 2/5 的按帧消费侧)。
//
// 流式事件与终端画面之间的一道闸:产生流式事件的线程(SSE 流内回调,
// 今天与 Run 同体、后续批换成真网络线程)只管把 ServerEvent 投进来,一个
// 终端字节都不写;画面的活全挪到消费侧。本批的消费侧是按帧落屏:泵的
// 消费线程即 UI 线程,delta 批按帧节拍(16~33ms 一帧,环境变量
// LUBANCODE_UI_FRAME_MS,默认 33;0 = 老路"投递即醒立即画",两轨并存,
// 直写老路留一版再拆)收批,一帧最多一次落屏——渲染闭包仍是
// TerminalTurnSink::RenderEvent(闭包调用次序与老路逐一相同,画面不变),
// 变的只是"一帧收多少":洪峰期 1000 枚小 delta 并成一帧批,空闲节奏
// (人手打字般稀疏)投递即醒、立刻画,延迟与老路相同。
//
// 队列语义(合并/背压/关账):
//   - 合并:Post 侧就地拼接"队尾相邻、同 item 的同类 ItemDelta"——delta
//     洪峰不涨队列元素数,只涨单枚滚动 delta 的字节数;消费线程按帧收批,
//     帧间隔内的增量自然并成一批,凑批不需要刻意计时。
//   - 背压:不设上限,生产侧永不阻塞(EventSink 合同:"不许反过来阻塞
//     内核线程等画面")。有界性靠三道:Post 侧合并(见上)、消费线程排速
//     (每帧一批)远高于网络产速、每枚控制事件就地排干。终端再慢,撑大的
//     也只是"当前 item 那一枚 delta",与 Run 线程攒 history 同量级。
//   - 关账:StopAndDrain 幂等——置停、叫醒(含按帧等待中的消费线程)、
//     join、持画笔锁把余量在调用线程就地画完。收口事件(usage/Finish)
//     从来只走就地路(DispatchInline),不过队列,结构上就不可能丢;停表
//     之后迟到的流式事件(Stop 钩子续跑的正文)自动退化成就地画。
//
// 线程规矩:render_mutex_(画笔锁)串行化一切渲染——消费线程的按帧渲染、
// 就地路的排干与渲染都持它;各渲染闭包内部自拿 StdoutWriteMutex(锁序恒
// "画笔 -> stdout",无倒置)。队列锁(queue_mutex_)只护 deque,绝不 held
// 跨渲染,Post 在渲染期间不等人。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include "runtime/event.hpp"

namespace lubancode::app {

class UiEventPump {
public:
    // renderer:消费侧拿事件去画(现挂 TerminalTurnSink::RenderEvent)。
    // 就地路(DrainLocked/renderer_)的异常原样抛回调用方(老路行为);
    // 消费线程里被捕获、落一行 stderr,不叫线程暴毙(老路上这类异常会
    // 穿过 libcurl 的 C 回调,UB;挪出回调之后才有得接)。
    using Renderer = std::function<void(const runtime::ServerEvent&)>;

    // frame_interval:按帧落屏的节拍;0 = 老路(投递即醒立即画,两轨中的
    // 直写轨)。默认取环境变量 LUBANCODE_UI_FRAME_MS(缺省 33ms,即约
    // 30 帧;0 关掉帧节拍回老路)。
    explicit UiEventPump(Renderer renderer,
                         std::chrono::milliseconds frame_interval = FrameIntervalFromEnv());
    ~UiEventPump();  // StopAndDrain 兜底(幂等,正常路早收过)

    UiEventPump(const UiEventPump&) = delete;
    UiEventPump& operator=(const UiEventPump&) = delete;

    // 生产侧(SSE 流内回调):只投,不画,不阻塞。相邻同 item 的 delta
    // 就地合并(见文件头)。
    void PostDelta(const runtime::ServerEvent& event);

    // 控制路(工具起止/usage/批次边界/收口):先把队列里的流式事件排干
    // (次序钉死——正文永远先于工具卡落笔),再把这枚就地画掉。调用线程
    // 即产生事件的线程,与老路同一枚手。不等帧——控制路的即时性优先。
    void DispatchInline(const runtime::ServerEvent& event);

    // 关账:停消费线程、排干余量(见文件头)。幂等。
    void StopAndDrain();

    // 环境变量 LUBANCODE_UI_FRAME_MS 读帧间隔(进程内读一次):>0 为毫秒
    // 数(钳到 [1,1000]),0/非法/缺省回 33ms。单测不碰环境,直接给构造
    // 函数传显式值。
    static std::chrono::milliseconds FrameIntervalFromEnv();

private:
    // 画笔锁已持的前提下:取走全部 pending、逐枚交给 renderer。
    void DrainLocked();
    void ConsumerMain();

    Renderer renderer_;
    const std::chrono::milliseconds frame_interval_;
    std::mutex queue_mutex_;               // 只护 pending_,不跨渲染持有
    std::condition_variable wake_;         // 投递即醒;谓词 stopped_||!pending_
    std::deque<runtime::ServerEvent> pending_;
    std::mutex render_mutex_;              // 画笔锁:串行化一切渲染
    std::thread consumer_;
    std::atomic<bool> stopped_{false};
};

}  // namespace lubancode::app
