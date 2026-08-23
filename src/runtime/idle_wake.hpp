// IdleWakeCoordinator(loop 单第 1 期):空闲唤醒的多路总口。
//
// 旧貌:console_input 的 SetIdleWakeHook 只装得下一枚 callback,已经给
// "后台子代理完成"占着——loop 若直接覆盖,会弄丢子代理唤醒(单子"欠的"
// 第一条)。新貌:多路订阅,ReadLine 的 100ms 拍逐源问一遍 ready,任一
// true 即让位;session 析构经 RAII token 自动摘,不把已析构的 this 留在
// 全局槽里。
//
// 依赖铁律:只认标准库,不 include cli/app/agent。console_input 侧的
// 适配(SetIdleWakeHook 装一枚"问 coordinator"的总钩)由装配层接。

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::runtime {

// 一枚唤醒源:name 供诊断,ready 返回 true = 有活要让位。
// ready 只在主线程(ReadLine 的拍)被调,实现方不必自己加锁——但多路
// 之后各源自己保证线程安全(账面若有锁,快照后锁外答)。
class IdleWakeCoordinator {
public:
    // RAII 订阅 token:析构自动摘源。session 析构时 token 先于成员走,
    // 不留悬垂。
    class Subscription {
    public:
        Subscription() = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        ~Subscription();
        void reset();
        bool valid() const { return impl_ != nullptr; }

    private:
        friend class IdleWakeCoordinator;
        struct Impl;  // 内部:持回指,析构摘源
        std::unique_ptr<Impl> impl_;
    };

    // 挂一枚源,拿 RAII token。重名不拒(诊断用名,不是 id)。
    Subscription AddSource(std::string name, std::function<bool()> ready);

    // 有没有任何一路 ready。空 coordinator 给 false。
    bool AnyReady() const;

    // 源清单(诊断/测试用)。
    std::vector<std::string> SourceNames() const;

private:
    struct Source {
        std::string name;
        std::function<bool()> ready;
        Subscription::Impl* token = nullptr;  // 活订阅的回指(摘源时清)
    };

    // 摘源(token 的 reset 走这):按 name+指针回指摘自己那枚。
    void RemoveByName(const std::string& name, Subscription::Impl* token);

    mutable std::mutex mutex_;
    std::vector<Source> sources_;
};

}  // namespace lubancode::runtime
