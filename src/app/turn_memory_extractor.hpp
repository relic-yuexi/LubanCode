// 回合记忆抽取的异步执行器(记忆回合总结异步化单):ExtractTurnMemory
// 的前置门(纯本地)留在前台,采样+解析丢这只的后台线程——回合收口即刻
// 还输入框,learn 开着的每一场不再同步等 cheap 路由的网络往返。先例:会话
// 起名原先也在收尾处同步等 cheap,实测问题 7 后搬发轮前、精炼异步跑
// (#148,SessionTitleRefiner);抽取的材料是回合终态(BuildTurnTranscript
// 要整轮结果),搬发轮前不可行——只能异步化。
//
// 材料全在起飞前拼好、值拷贝进闭包(转写/系统提示/分型),不引用会话
// 任何共享态;HTTP 走 RouteDetached 造的独占裸 backend(不与主会话共用
// client,不抢流式回调)。本地超时预算(看门狗)与会话拆除的外部取消
// 都走 SampleModel 的合并取消口。
//
// 单飞:同场在途至多一枚,上一枚没收走(在跑或结果待收)就拒——连续
// 快问快答不积压并发采样,让位的回合由调用方(ExtractTurnMemory)如实
// 记账,不冒充网络失败。
//
// 结果只经 TakeFinished 出去:usage 记账/候选入队/台账落袋全在主线程的
// 收货点(SettleTurnMemory),后台线程不碰会话共享态——除自持的 shared
// 槽与在闭包栈上自生灭的旁路桥(recorder 提交全程持锁,与主线程的写在
// 盘上串行)。session_generation 是起飞时的会话世代(/clear、/resume 翻
// 号):迟到结果由调用方对代丢弃——usage 仍照记,token 是真花了的。
//
// 退出兜底照 SessionTitleRefiner/AgentTool 析构的老方子:RequestCancel 拉
// 原子取消旗,析构取消 + 有界等待,等不到就 detach 放行——闭包自持
// shared 状态,晚归不悬垂,也不冻退出。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "agent/model_router.hpp"  // BackgroundCallAccounting(usage 出账)
#include "api/backend.hpp"

#include "app/memory_extract.hpp"  // MemoryExtraction/ExtractionError(结果合同)

namespace lubancode::runtime {
class TrajectorySessionLedger;
}

namespace lubancode::app {

class TurnMemoryExtractor {
public:
    struct Inputs {
        std::unique_ptr<lubancode::api::Backend> backend;  // 独占裸 client,线程内独享
        std::string model;
        std::string effort;          // cheap 路由的档位;空 = 请求不带
        std::string system_prompt;   // 前台拼好的抽取系统提示(冻结快照)
        std::string transcript;      // 前台拼好的本轮转写(冻结快照)
        std::string task_type;       // 前台分型结果(候选入队要挂)
        std::uint64_t session_generation = 0;  // 起飞时的会话世代,落地对代
        std::string turn_id;                   // 迟到收账对档(MemoryTurnLedger 悬账)
        // Token 账本单 A1(旁路落账):flag 开的会话递账本,抽取请求在
        // worker 线程自铸旁路桥落 Journal(purpose=memory_extract)。recorder
        // 提交全程持锁,后台线程与主线程的写在盘上串行;线程只持这只裸
        // 指针+值拷贝,不引用会话其它共享态。空 = 没接轨迹。
        lubancode::runtime::TrajectorySessionLedger* trajectory = nullptr;
        std::string trajectory_wire;  // 桥 identity 的渠道名(与主 turn 桥同源)
        std::string provider;         // 抽取路由的 provider(桥 identity)
    };
    struct Outcome {
        bool ok = false;                       // 采样成功且解析通过
        MemoryExtraction extraction;           // ok 时有效(总结+候选+检索词)
        ExtractionError error;                 // !ok 时有效(稳定码+诊断)
        lubancode::agent::BackgroundCallAccounting accounting;  // 失败半截也出账
        std::string model;                     // 实际用的模型(记账用)
        std::string task_type;                 // 原样带回(入队侧要挂)
        std::uint64_t session_generation = 0;  // 原样带回:迟到由调用方对代弃
        std::string turn_id;                   // 原样带回:悬账对档
        std::int64_t extract_wall_ms = 0;      // 发起到采样返回的墙钟
    };

    TurnMemoryExtractor() = default;
    ~TurnMemoryExtractor();
    TurnMemoryExtractor(const TurnMemoryExtractor&) = delete;
    TurnMemoryExtractor& operator=(const TurnMemoryExtractor&) = delete;
    TurnMemoryExtractor(TurnMemoryExtractor&&) = delete;
    TurnMemoryExtractor& operator=(TurnMemoryExtractor&&) = delete;

    // 起一枚抽取任务。单飞:上一枚还在跑或结果还没被收走就拒(false),
    // 不叠发。backend 为空同样拒(路由落空由调用方在起飞前自记零账)。
    bool Start(Inputs&& inputs);

    // 主线程收货:任务完工(成功/失败/取消都算)给 Outcome 并复位,可再
    // Start;没完工给空,绝不等待。
    std::optional<Outcome> TakeFinished();

    // 拉取消旗(换代 /clear、/resume、退出收尾)。只发信号不 join。
    void RequestCancel();

    // 有任务在跑或结果待收(还没被 TakeFinished 取走)。
    bool Busy() const;

    // 只读完工查询(空闲唤醒的条件):结果备好待收才 true。不 join、不
    // 取走、不清状态,真正的收货仍走 TakeFinished。运行中恒 false。
    bool Ready() const;

private:
    struct Shared {
        std::mutex mutex;
        std::optional<Outcome> outcome;   // 完工后等主线程收走
        std::atomic<bool> done{false};    // outcome 已写完的收讫旗
        std::atomic<bool> cancel{false};  // 取消链:看门狗与 RequestCancel 都拉它
        std::uint64_t session_generation = 0;
        std::string turn_id;
    };

    std::shared_ptr<Shared> shared_;
    std::thread worker_;
};

}  // namespace lubancode::app
