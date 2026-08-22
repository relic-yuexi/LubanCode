// app-server 的出站有界队列:慢客户端不可堵住代理工作线程(单子协议
// 底线第三节)。生产者(回合驱动线程)往里 Push 事件,消费者(写线程)
// Drain 出去逐行写 stdout。
//
// 骨架期先做两件事:
//   1. 有界队列:Push 撞上限不阻塞、不抛,而是丢事件、记溢出账;
//   2. overflow 通报的类型:溢出后队首插一条明确的 queue/overflow 事件
//      (不许悄悄丢),终态事件(turn/completed)与审批类事件绝不丢——
//      撞上限时它们仍要入队(挤掉的是可丢的 delta 之类的中间事件,
//      由 DropPolicy 分型)。
//
// 可合并 delta 的合并策略("超限先合并可合并的 delta")是后续阶段的活,
// 这里的 coalesced 计数恒 0,类型先立住。
#pragma once

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
class BoundedOutbox {
public:
    explicit BoundedOutbox(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    // 入队一条已序列化的行。must_keep 的行(终态、审批)满了也要想办法
    // 塞:先丢队里可丢的,还塞不进(全满 must_keep)才丢它自己——丢了
    // 也记进溢出账,绝不阻塞调用线程。
    // 返回 true = 已入队;false = 被丢弃(已计溢出)。
    bool Push(std::string line, bool must_keep = false);

    // 取走队列头部一条(队里已有 must_keep 挤出的 overflow 通报也算一条)。
    std::optional<std::string> Pop();

    // 把队列整个倒出来(关闭前的冲刷用),队里剩余按序返回。
    std::vector<std::string> PopAll();

    // 累计丢掉的事件条数(overflow 通报里的 dropped 用)。
    std::uint64_t dropped() const;

    // 队列是否已满(诊断用)。
    bool full() const;

    std::size_t size() const;
    std::size_t capacity() const { return capacity_; }

private:
    mutable std::mutex mutex_;
    std::deque<OutboundEntry> queue_;
    const std::size_t capacity_;
    std::uint64_t dropped_ = 0;
};

// 出站事件分类:哪些事件丢了客户端还能对上账(可丢),哪些绝不能丢
// (must_keep)。骨架期的分型:
//   - turn/completed:唯一终态,丢了客户端永远等下去,必须保;
//   - queue/overflow 本身:丢了就没有溢出通报,必须保;
//   - 其余(item/delta 等):可丢,丢了有 overflow 通报兜底。
// 审批反向请求(permission/request、user/ask)也是 must_keep——骨架期
// 没接线,类型先立住:审批丢了客户端不知道要答,回合就挂死在等答复上。
inline bool EventMustKeep(std::string_view method) {
    return method == kEventTurnCompleted || method == kEventQueueOverflow ||
           method == kMethodPermissionRequest || method == kMethodUserAsk;
}

}  // namespace lubancode::app_server
