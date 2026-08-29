// app-server 的出站有界队列:慢客户端不可堵住代理工作线程(单子协议
// 底线第三节)。生产者(回合驱动线程)往里 Push 事件,消费者(写线程)
// Drain 出去逐行写 stdout。
//
// 三件事:
//   1. 有界队列:Push 撞上限不阻塞、不抛,而是丢事件、记溢出账;
//   2. overflow 通报的类型:溢出后队首插一条明确的 queue/overflow 事件
//      (不许悄悄丢),终态事件(turn/completed)与审批类事件绝不丢——
//      撞上限时它们仍要入队(挤掉的是可丢的 delta 之类的中间事件,
//      由 DropPolicy 分型);
//   3. delta 合并(阶段 3 起):撞满时先把新行并进队里最后一条"同
//      itemId 的可丢 item/delta"(两条 JSON 的 params.delta 拼接,
//      seq 取后到的那枚),并不动队列长度、不丢内容——这一步救不下
//      才走丢事件的路。coalesced 计数记的就是靠合并省下的条数。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app_server/protocol.hpp"

namespace lubancode::app_server {

// 一条出站消息 + 它的保全等级。
struct OutboundEntry {
    std::string line; // 已序列化好的一行 JSON(不带换行)
    bool must_keep = false; // 终态/审批类:溢出时也不许丢
};

// 有界出站队列。线程安全:Push/Drain 各持一把内部锁。
// 阶段 2 起回合在工作线程跑、事件从写线程出,加条件变量让写线程安静地
// 等(PopWait),不再靠轮询。
class BoundedOutbox {
public:
    explicit BoundedOutbox(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    // 入队一条已序列化的行。must_keep 的行(终态、审批)满了也要想办法
    // 塞:先丢队里可丢的,还塞不进(全满 must_keep)才丢它自己——丢了
    // 也记进溢出账,绝不阻塞调用线程。
    // 可丢的 item/delta 撞满时先试合并(见文件头 3),合并成功不入队、
    // 计入 coalesced。
    // 返回 true = 已入队;false = 被丢弃(已计溢出)。
    bool Push(std::string line, bool must_keep = false);

    // 取走队列头部一条(队里已有 must_keep 挤出的 overflow 通报也算一条)。
    std::optional<std::string> Pop();

    // 限时等一条:队列空时睡到有人 Push(Notify)或时限到。写线程用。
    std::optional<std::string> PopWait(std::chrono::milliseconds timeout);

    // 唤醒所有在 PopWait 里睡着的(收线时叫醒写线程退场)。
    void Notify();

    // 把队列整个倒出来(关闭前的冲刷用),队里剩余按序返回。
    std::vector<std::string> PopAll();

    // 累计丢掉的事件条数(overflow 通报里的 dropped 用)。
    std::uint64_t dropped() const;

    // 累计靠合并省下的条数(overflow 通报里的 coalesced 用)。阶段 3 起
    // 真算,不再恒 0。
    std::uint64_t coalesced() const;

    // 队列是否已满(诊断用)。
    bool full() const;

    std::size_t size() const;
    std::size_t capacity() const { return capacity_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<OutboundEntry> queue_;
    const std::size_t capacity_;
    std::uint64_t dropped_ = 0;
    std::uint64_t coalesced_ = 0;
};

// 出站事件分类:哪些事件丢了客户端还能对上账(可丢),哪些绝不能丢
// (must_keep)。骨架期的分型:
//   - turn/completed:唯一终态,丢了客户端永远等下去,必须保;
//   - queue/overflow 本身:丢了就没有溢出通报,必须保;
//   - 其余(item/delta 等):可丢,丢了有 overflow 通报兜底。
// 审批反向请求(permission/request、user/ask)也是 must_keep——
// 审批丢了客户端不知道要答,回合就挂死在等答复上。
// 浏览器族(阶段 3):browser/stopped 与 browser/crashed 是会话终态,
// browser/action/completed 是动作终态,browser/screenshot/ready 没有
// 查询口(丢了就真丢了图)——这四枚必须保;console/network 批量事件
// 可丢(有 sinceSeq 补账),其余浏览器事件可丢(status/list 可重建)。
inline bool EventMustKeep(std::string_view method) {
    return method == kEventTurnCompleted || method == kEventQueueOverflow ||
           method == kMethodPermissionRequest || method == kMethodUserAsk ||
           method == kEventBrowserStopped || method == kEventBrowserCrashed ||
           method == kEventBrowserActionCompleted || method == kEventBrowserScreenshotReady;
}

}  // namespace lubancode::app_server
