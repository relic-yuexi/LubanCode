// 会话级 UI 调度(按代理状态投影单 P2:收拢写者,原子换页)。
//
// §五"终端只留一处落笔"的调度队列本体:一场交互会话一只,把"谁能往终端
// 上写字"从"谁产生事件"里剥出来——
//   - 业务线程(SSE 流内回调、工具执行、usage/收口)只 Post:流内事件
//     (ItemDelta/内置工具起止)与控制路事件(TerminalTurnSink 旧路的
//     DispatchInline 就地画)一律提交为命令,产生事件的线程一个终端字节
//     都不写。旧 DispatchInline 的"画前排干"语义由队列 FIFO 天然保住:
//     正文永远先于工具卡落笔。
//   - 消费线程(UI 线程)按提交次序逐枚执行,每枚都握统一提交锁
//     (commit_mutex_)。渲染闭包内部自拿 StdoutWriteMutex,锁序恒
//     "commit -> stdout",无倒置。
//   - 顺序闸(HC-02):出队与执行捆成一个闸单元。消费线程的每一批、
//     RunSync/Flush 的"排干+body"、Stop 的收尾排干、停表后 PostAction
//     的就地执行,一律先过闸(order_mutex_)再领队列,领完在闸内跑完才
//     放闸。提交锁只保证不同时画,保证不了先后——旧批被领走还没落笔,
//     新批照样先抢锁:工具终态赶在开始前头,open_tools_ 空查,对就散了。
//     闸一收口,谁领走的批谁落笔,后来者(抢醒的消费循环、压上来的
//     RunSync)在闸上等旧批落定,先提交先执行由此成立。闸递归:命令体内
//     嵌 RunSync/Flush 合法,嵌套单元在持有者自己的单元内就地跑(闸开着
//     的整个单元里队列只有持有者一个读者,旁人领不走)。锁序恒
//     "顺序闸 -> 队列锁 -> 提交锁 -> stdout/登记簿",闸之外不许反拿。
//   - 同步口 RunSync(换页事务/收口 chrome/插行这些调用方要"回来时已画
//     完"的场合):先把队列里的余量在本线程排干,再在提交锁内就地执行。
//     调用线程就是执行线程的合同不变;过闸时可能等在飞的一批落定——与
//     旧款等提交锁同一量级的有界等待(在飞批就是画屏),调用方自持的锁
//     (ConsoleReadMutex 一类)仍不会被 UI 线程反等,锁序图无环。过渡批
//     由此保留多名 writer(监听线程/composer 主线程/RunTurn 收口),但
//     全部在统一提交锁内核对身份,单子 §五"过渡批"条款的正路;P3 再把
//     空闲路整帧也收成异步命令。
//
// 事件命令带渲染方世代(renderer id):TerminalTurnSink 每轮登记一只渲染
// 器,RunTurn 收口 Detach——迟到的命令查无此号,整枚丢弃,不留悬垂引用。
// (sink 自己的迟到事件不 Post:StopUiPump 之后 Emit 退化成就地执行,与
// 旧泵"停表后 PostDelta 就地画"同款。)
//
// 合并:与旧泵同款,队尾相邻、同 item 的 ItemDelta 就地拼接——delta 洪峰
// 不涨队列元素数,只涨单枚滚动 delta 的字节数。背压不设上限,生产侧永不
// 阻塞(EventSink 合同)。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/event.hpp"

namespace lubancode::app {

class SessionUiDispatcher {
public:
    // 事件渲染器:消费侧拿事件去画(挂 TerminalTurnSink::HandleEvent 的
    // 那半)。按世代登记,执行时拷走再调,登记表小锁绝不跨渲染持有。
    using EventRenderer = std::function<void(const runtime::ServerEvent&)>;

    SessionUiDispatcher() { Start(); }
    ~SessionUiDispatcher() { Stop(); }

    SessionUiDispatcher(const SessionUiDispatcher&) = delete;
    SessionUiDispatcher& operator=(const SessionUiDispatcher&) = delete;

    // ---- 渲染方登记 -------------------------------------------------------

    // 登记一只事件渲染器,返回世代号(从 1 起)。登记新的不废旧号——旧号
    // 照样可查,直到显式 Detach;这样同会话多轮次(每轮一只 sink)的号互
    // 不串。返回 0 = 已停,别再 Post。
    std::uint64_t AttachRenderer(EventRenderer renderer);

    // 摘号:此后 PostEvent(旧号) 整枚丢弃(查无此号)。幂等。
    void DetachRenderer(std::uint64_t renderer_id);

    // ---- 提交侧 -----------------------------------------------------------

    // 投一枚事件(业务线程唯一出路)。同 item 的相邻 ItemDelta 在队尾
    // 就地合并。停表后返回 false(调用方自行决定就地画或丢弃;TerminalTurnSink
    // 的迟到事件走自己的就地退化,不靠这里)。
    bool PostEvent(std::uint64_t renderer_id, const runtime::ServerEvent& event);

    // 投一枚异步动作,拿 future(要等的等,不等就弃)。停表后返回无效
    // future,动作已在调用线程就地执行(提交即完成)。
    std::future<void> PostAction(std::function<void()> action);

    // 同步执行:先把队列余量在本线程排干(保 FIFO),再在提交锁内就地
    // 执行 body。调用线程就是执行线程——自持锁不被反等,见文件头。
    // body 抛出的异常原样还给调用方。
    void RunSync(std::function<void()> body);

    // 屏障:等此前提交的命令全执行完(RunSync 的空体)。停表后空操作。
    void Flush();

    // 静默屏障(StopUiPump/确认菜单开屏用):不止排干 pending,还要等
    // 消费线程手里已取走在飞的这一批跑完——对齐旧泵"join 之后才收口"
    // 的严格序,收口 chrome 与在飞渲染之间不留缝。停表后空操作。
    void Quiesce();

    // ---- 统一提交锁 -------------------------------------------------------

    // 一切落笔的必经之门(过渡批多 writer 的核对位;UI 线程逐枚命令也握
    // 它)。递归:命令内嵌 RunSync 合法。锁序恒 "commit -> stdout ->
    // 登记簿锁",本锁之外不许反拿。
    std::recursive_mutex& commit_mutex() { return commit_mutex_; }

    // 当前线程是不是消费线程(命令体内判断用;单测探针)。
    bool IsUiThread() const { return std::this_thread::get_id() == consumer_id_.load(); }

    // ---- 测试钉子(HC-02) --------------------------------------------------
    // 出队/提交边界的可控调度点。单测用它把消费线程钉在"批已出队、一笔
    // 未提交"的病窗上,拿屏障造确定性交错,不靠 sleep 碰运气。生产恒空。
    enum class DebugPoint {
        ConsumerBatchStolen,  // 消费线程领走一批、第一枚还没提交(顺序闸内)
    };

    // 换钩线程安全。钩子在顺序闸内、队列/提交锁外被调,可以放心阻塞;
    // 钩内抛异常不拖垮消费线程(stderr 留名后照跑)。
    void SetDebugHook(std::function<void(DebugPoint)> hook);

    // ---- 生命周期 ---------------------------------------------------------

    void Start();  // 幂等;起消费线程
    // 排干余量、停线程。幂等;析构兜底再收一次。停表后的 PostAction 就地
    // 执行、PostEvent 拒收(见上)。
    void Stop();

    // 队列深度(单测/诊断;近似值)。
    std::size_t PendingApprox();

private:
    struct Entry {
        std::uint64_t renderer_id = 0;            // 0 = 动作命令
        runtime::ServerEvent event;               // 事件命令
        std::shared_ptr<std::promise<void>> done; // 动作命令的回执(可空)
        std::function<void()> action;             // 动作命令
    };

    // 偷走全部待处理命令(队锁内),返回给调用方在本线程执行。
    std::vector<Entry> StealPending();

    // 在本线程执行一批命令:逐枚握提交锁,事件查渲染器世代,动作跑完把
    // 回执填了。异常:事件渲染落一行 stderr 不叫整批暴毙(旧泵消费线程
    // 同款),动作异常进 promise(等待方拿到),RunSync 直呼的那枚原样抛。
    void RunEntriesOnCaller(std::vector<Entry> entries);
    void ExecuteEntryLocked(const Entry& entry);
    EventRenderer LookupRenderer(std::uint64_t renderer_id);

    void ConsumerMain();
    void FireDebugHook(DebugPoint point);

    // 顺序闸(HC-02):出队与执行捆成一个闸单元,谁领走的批谁落笔。
    // 递归:命令体内嵌 RunSync/Flush 合法。恒为最外层锁,见文件头锁序。
    std::recursive_mutex order_mutex_;
    std::mutex queue_mutex_;               // 只护 pending_,不跨渲染持有
    std::condition_variable wake_;         // 谓词 stopped_ || !pending_.empty()
    std::condition_variable idle_;         // Quiesce 用:一批跑完且队列空时叫
    std::deque<Entry> pending_;
    std::size_t inflight_ = 0;             // 已出队、正在执行的命令数(queue_mutex_ 护)
    std::recursive_mutex commit_mutex_;    // 统一提交锁(递归)
    std::mutex renderers_mutex_;
    std::uint64_t next_renderer_id_ = 1;
    std::vector<std::pair<std::uint64_t, EventRenderer>> renderers_;  // 登记表(小,线性查)
    std::mutex debug_hook_mutex_;
    std::function<void(DebugPoint)> debug_hook_;  // 测试钉子(生产恒空)
    // stopped_ 须先于 consumer_ 声明(成员按声明序构造;消费线程一起跑就
    // 读 stopped_,构造序竞态的坑旧泵注释里写过,同款防御)。
    std::atomic<bool> stopped_{false};
    std::atomic<std::thread::id> consumer_id_{};
    std::thread consumer_;
    bool started_ = false;
};

}  // namespace lubancode::app
