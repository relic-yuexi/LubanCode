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
#include <mutex>
#include <utility>
#include <vector>

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

// 一份事件流喂多家(骨架拆解批二:装配点"配 sink 列表"配的就是它)。
// Add 任意时刻可调(装配期挂满是常态);Emit 在产生事件的线程上被调,
// 逐家转发——各家自己管线程安全(合同第 1 条),这里只保次序不重排。
// 空表 = 黑洞:装配未完成期的事件安静丢弃,不炸、不攒。
class FanoutEventSink final : public EventSink {
public:
    void Add(EventSink* sink) {
        if (sink == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.push_back(sink);
    }

    void Emit(const ServerEvent& event) override {
        // 拷一份再投:投递期间另一线程 Add 不互相卡锁;sink 指针的存活期
        // 由装配方保证(与 AttachSink 同一规矩)。
        std::vector<EventSink*> sinks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sinks = sinks_;
        }
        for (EventSink* sink : sinks) {
            sink->Emit(event);
        }
    }

private:
    std::mutex mutex_;
    std::vector<EventSink*> sinks_;
};

}  // namespace lubancode::runtime
