// 会话标题的异步精炼器(实测问题 7):两层标题的第二层。
//
// Start 由会话在首个主回合收口后的空闲边界调(ec8b22df 起:旁路小 turn
// 与主 turn 不能同流并存,一 stream 一 open turn,回合里发必撞车):独占
// 裸 backend(ModelRouterService 的 RouteDetached 造,不与主会话共用
// client,不抢流式回调)、只发一次 cheap 采样——首问截段 600 字节、
// max_tokens=24、5 秒看门狗、无工具。起飞后谁也不等它,提示符照还。
//
// 完工的叫醒:Ready()(只读、线程安全)在结果备好待收时翻真,装配层把
// 它挂进 IdleWakeCoordinator——空闲 composer 的 100ms 拍一看真,ReadLine
// 以空串让位,主循环的收货点当场记账上屏,不等用户再敲一行。Busy() 与
// Ready() 的分别:Busy 在"完成待取"时也真(槽还占着,单飞防叠发),
// 拿它当唤醒条件会起飞即醒、空转到收货——唤醒只认 Ready。
//
// 结果只经 TakeFinished 出去:主线程记账/落盘/上屏全在主线程——后台线程
// 不碰会话任何共享态,除自己的 shared 槽外只引用自持的值。generation 是
// 起飞时的标题代数:人工 /title、/clear、/resume 都会翻代,迟到的结果由
// 调用方对代丢弃(usage 仍照记,账是真的)。
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

namespace lubancode::runtime {
class TrajectorySessionLedger;
}

namespace lubancode::app {

class SessionTitleRefiner {
public:
    struct Inputs {
        std::unique_ptr<lubancode::api::Backend> backend;  // 独占裸 client,线程内独享
        std::string model;
        std::string effort;         // 路由档位;空 = 精炼请求自带最低档
        std::string first_query;    // 首问原文(线程内截 600 字节)
        std::uint64_t generation = 0;  // 起飞时的标题代数,落地对代
        // Token 账本单 A1(旁路落账):flag 开的会话递账本,精炼请求在
        // worker 线程自铸旁路桥落 Journal(purpose=title_refine)。
        // recorder 提交全程持锁,后台线程与主线程的写在盘上串行;线程
        // 只持这只裸指针+值拷贝,不引用会话其它共享态。空 = 没接轨迹。
        lubancode::runtime::TrajectorySessionLedger* trajectory = nullptr;
        std::string trajectory_wire;  // 桥 identity 的渠道名(与主 turn 桥同源)
        std::string provider;         // 精炼路由的 provider(桥 identity)
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

    // 只读完工查询(空闲唤醒的条件):结果备好待收才 true。不 join、不
    // 取走、不清状态,真正的收货仍走 TakeFinished。运行中恒 false;
    // Busy() 在完成待取时也是 true——它管单飞,不管唤醒,别混用。
    bool Ready() const;

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
