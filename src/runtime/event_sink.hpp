// EventSink 合同(显示系统剥离单第一步:立合同,不改画面)。
//
// 内核往外吐事件的出口形状:Runtime 只认这只接口,谁实现谁画——终端
// 实现(TerminalEventSink,后落 frontend/terminal)照旧画现有 TUI,JSON
// 实现(JsonEventSink)给 app-server/脚本桥,同一事件流两边渲染不同、
// 领域数据一字不差(单子验收:"同一份假 backend 分别喂两 sink,id、次序、
// 终态与领域数据完全一致")。
//
// 依赖铁律同 event.hpp:零实现依赖,不 include cli/app/frontend。
// 纯抽象接口放头文件即可,没有 .cpp。

#pragma once

#include <functional>

#include "runtime/event.hpp"

namespace lubancode::runtime {

// 事件出口。实现方注意事项(合同的一部分):
//   1. Emit 在产生事件的线程上被调(Run 所在线程或后台任务线程),实现
//      自己管线程安全(锁/投递队列),不许反过来阻塞内核线程等画面。
//   2. Emit 返回即"收下",不表成败——事件是单向广播,没有回执。
//   3. 事件次序由 seq 保证(见 event.hpp 文件头),实现不得重排。
class EventSink {
public:
    virtual ~EventSink() = default;

    virtual void Emit(const ServerEvent& event) = 0;
};

// 顺手给单测/桥接用的一只函数指针壳:不扛虚表也能把 lambda 塞进要
// EventSink 的地方。
class FunctionEventSink final : public EventSink {
public:
    explicit FunctionEventSink(std::function<void(const ServerEvent&)> fn) : fn_(std::move(fn)) {}

    void Emit(const ServerEvent& event) override { fn_(event); }

private:
    std::function<void(const ServerEvent&)> fn_;
};

}  // namespace lubancode::runtime
