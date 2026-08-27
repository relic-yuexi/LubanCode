// UiEventPump(终端画面隔网先行批:条 1 的队列与过渡消费侧)。
//
// 流式事件与终端画面之间的一道闸:产生流式事件的线程(SSE 流内回调,
// 今天与 Run 同体、后续批换成真网络线程)只管把 ServerEvent 投进来,一个
// 终端字节都不写;画面的活全挪到消费侧。本批的过渡消费侧是一只独立线程,
// 凑批后仍调既有直写渲染闭包(TerminalTurnSink::RenderEvent),画面与老路
// 逐字节一致;后续批把消费侧换成按帧落屏时,这只泵的队列与关账语义原样
// 留用,只换 renderer。
//
// 队列语义(合并/背压/关账):
//   - 合并:Post 侧就地拼接"队尾相邻、同 item 的同类 ItemDelta"——delta
//     洪峰不涨队列元素数,只涨单枚滚动 delta 的字节数;消费线程忙碌时,
//     生产期间的增量自然并成一批,凑批不需要刻意计时。零散到达(人手打字
//     般的节奏)则投递即醒、立刻画,延迟与老路相同。
//   - 背压:不设上限,生产侧永不阻塞(EventSink 合同:"不许反过来阻塞
//     内核线程等画面")。有界性靠三道:Post 侧合并(见上)、消费线程排速
//     远高于网络产速、每枚控制事件就地排干。终端再慢,撑大的也只是"当前
//     item 那一枚 delta",与 Run 线程攒 history 同量级,不另开账。
//   - 关账:StopAndDrain 幂等——置停、叫醒、join 消费线程、持画笔锁把
//     余量在调用线程就地画完。收口事件(usage/Finish)从来只走就地路
//     (DispatchInline),不过队列,结构上就不可能丢;停表之后迟到的流式
//     事件(Stop 钩子续跑的正文)自动退化成就地画,行为回到老路。
//
// 线程规矩:render_mutex_(画笔锁)串行化一切渲染——消费线程的凑批渲染、
// 就地路的排干与渲染都持它;各渲染闭包内部自拿 StdoutWriteMutex(锁序恒
// "画笔 -> stdout",无倒置)。队列锁(queue_mutex_)只护 deque,绝不 held
// 跨渲染,Post 在渲染期间不等人。
#pragma once

#include <atomic>
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

    explicit UiEventPump(Renderer renderer);
    ~UiEventPump();  // StopAndDrain 兜底(幂等,正常路早收过)

    UiEventPump(const UiEventPump&) = delete;
    UiEventPump& operator=(const UiEventPump&) = delete;

    // 生产侧(SSE 流内回调):只投,不画,不阻塞。相邻同 item 的 delta
    // 就地合并(见文件头)。
    void PostDelta(const runtime::ServerEvent& event);

    // 控制路(工具起止/usage/批次边界/收口):先把队列里的流式事件排干
    // (次序钉死——正文永远先于工具卡落笔),再把这枚就地画掉。调用线程
    // 即产生事件的线程,与老路同一枚手。
    void DispatchInline(const runtime::ServerEvent& event);

    // 关账:停消费线程、排干余量(见文件头)。幂等。
    void StopAndDrain();

private:
    // 画笔锁已持的前提下:取走全部 pending、逐枚交给 renderer。
    void DrainLocked();
    void ConsumerMain();

    Renderer renderer_;
    std::mutex queue_mutex_;               // 只护 pending_,不跨渲染持有
    std::condition_variable wake_;         // 投递即醒;谓词 stopped_||!pending_
    std::deque<runtime::ServerEvent> pending_;
    std::mutex render_mutex_;              // 画笔锁:串行化一切渲染
    std::thread consumer_;
    std::atomic<bool> stopped_{false};
};

}  // namespace lubancode::app
