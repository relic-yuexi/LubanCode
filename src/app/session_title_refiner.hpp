// 会话标题的异步精炼器(实测问题 7):两层标题的第二层。
//
// Start 在首问建档后立刻起一枚后台线程:独占裸 backend(ModelRouterService
// 的 RouteDetached 造,不与主会话共用 client,不抢流式回调)、只发一次
// cheap 采样——首问截段 600 字节、max_tokens=24、5 秒看门狗、无工具。
// 主回合照跑,轮末提示符照还,谁也不等它。
//
// 结果只经 TakeFinished 出去:主线程在会话循环顶非阻塞收货,记账/落盘/
// 上屏全在主线程——后台线程不碰会话任何共享态,除自己的 shared 槽外只
// 引用自持的值。generation 是起飞时的标题代数:人工 /title、/clear、
// /resume 都会翻代,迟到的结果由调用方对代丢弃(usage 仍照记,账是真的)。
//
// 退出兜底照子代理的老方子(见 AgentTool 析构):RequestCancel 拉原子
// 取消旗,析构取消 + 有界等待,等不到就 detach 放行——闭包自持 shared
// 状态,晚归不悬垂,也不冻退出。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "agent/model_router.hpp"  // BackgroundCallAccounting
#include "api/backend.hpp"

namespace lubancode::app {

class SessionTitleRefiner {
public:
    struct Inputs {
        std::unique_ptr<lubancode::api::Backend> backend;  // 独占裸 client,线程内独享
        std::string model;
        std::string effort;         // 路由档位;空 = 精炼请求自带最低档
        std::string first_query;    // 首问原文(线程内截 600 字节)
        std::uint64_t generation = 0;  // 起飞时的标题代数,落地对代
    };
    struct Outcome {
        bool ok = false;         // 采样成功且清洗后非空
        std::string title;      // ok 时非空
        std::string model;      // 实际用的模型(记账用)
        std::uint64_t generation = 0;
        lubancode::agent::BackgroundCallAccounting accounting;  // 失败半截也出账
    };

    SessionTitleRefiner() = default;
    ~SessionTitleRefiner();
    SessionTitleRefiner(const SessionTitleRefiner&) = delete;
    SessionTitleRefiner& operator=(const SessionTitleRefiner&) = delete;
    SessionTitleRefiner(SessionTitleRefiner&&) = delete;
    SessionTitleRefiner& operator=(SessionTitleRefiner&&) = delete;

    // 起一枚精炼任务。单飞:上一枚还在跑或结果还没被收走就拒(false),
    // 不叠发。backend 为空或模型为空同样拒。
    bool Start(Inputs&& inputs);

    // 主线程收货:任务完工(成功/失败/取消都算)给 Outcome 并复位,可再
    // Start;没完工给空,绝不等待。
    std::optional<Outcome> TakeFinished();

    // 拉取消旗(人工 /title 抢先、/clear 翻场、退出收尾)。只发信号不 join。
    void RequestCancel();

    // 有任务在跑或结果待收(还没被 TakeFinished 取走)。
    bool Busy() const;

private:
    struct Shared {
        std::mutex mutex;
        std::optional<Outcome> outcome;   // 完工后等主线程收走
        std::atomic<bool> done{false};    // outcome 已写完的收讫旗
        std::atomic<bool> cancel{false};  // 取消链:看门狗与 RequestCancel 都拉它
        std::uint64_t generation = 0;
    };

    std::shared_ptr<Shared> shared_;
    std::thread worker_;
};

}  // namespace lubancode::app
