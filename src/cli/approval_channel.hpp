// 审批通道(按代理状态投影单 P2:确认菜单改"提交审批请求→UI 显示→
// 独立响应通道返回决策")。
//
// 工具线程不再自己占屏问话:ConfirmToolUse 的"真要问用户"那半边打包成
// presenter 提交到这里,工具线程在独立响应通道(future)上等裁定;监听
// 线程(流式期间的键盘所有者)每拍取走"当前查看页"的待决请求、跑
// presenter 出菜单,答完把裁定 resolve 回 future。
//
// 页归属(单子 §三/§六):请求带 owner task id(0 = main)。当前查看页
// 不是 owner 时请求不显屏——底栏的固定通知位标"待审批",用户切回那页
// 的下一拍菜单才开。通知不追加进任何代理的正文。
//
// 线程纪律:工具线程 Submit/等 future(业务线程可等审批结果,单子 §五
// 原话);监听线程 Take/跑 presenter/Resolve。ESC 打断当前轮时
// DenyAllPending 把悬着的请求全部按拒绝收口——轮要死了,future 不能
// 悬成死锁。全部小锁内完成,presenter 在锁外跑。
#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::cli {

class ApprovalChannel {
public:
    // 一笔待决审批:presenter 在监听线程上跑(铺详情/出菜单/收画面),
    // 返回裁定(true = 允许)。
    struct Pending {
        std::uint64_t id = 0;
        int owner_task_id = 0;     // 0 = main;请求归属哪页
        std::string tool_name;     // 通知位文案用
        std::function<bool()> presenter;
    };

    // 工具线程:提交问话。监听侧在场(RegisterServer 已调)则挂起等裁定,
    // 返回 future;不在场(单发/管道/旧装配)返回 nullopt——调用方当场
    // 跑 presenter(旧路,一字不差)。
    std::optional<std::future<bool>> Submit(int owner_task_id, std::string tool_name,
                                            std::function<bool()> presenter);

    // 监听线程:取"当前查看页"的最早一笔(先到先答);没有则 nullopt。
    // 取走即从待决表摘除——同一笔不会被问两遍;resolve 由监听线程随后调。
    std::optional<Pending> TakeForViewer(int viewed_task_id);

    // 监听线程:把裁定送回工具线程的 future。id 陌生(已撤)时 no-op。
    void Resolve(std::uint64_t id, bool allowed);

    // 悬着的请求里有没有"属于 task_id 之外"的页——底栏通知位用(其他页
    // 待审批时标出来,不抢当前页)。
    bool HasPendingOutside(int task_id) const;

    // 悬着的请求总数(通知位/诊断)。
    std::size_t PendingCount() const;

    // 打断收口(ESC/interrupt_turn):全部悬着的按拒绝 resolve——轮要收
    // 场,future 不能悬死工具线程。
    void DenyAllPending();

    // 监听线程起跑/停表时登记/注销服务者。Submit 看"服务者在场"决定
    // 挂起还是让调用方就地问(单发/管道没有监听线程)。
    void RegisterServer();
    void ClearServer();

private:
    struct Slot {
        std::uint64_t id = 0;
        int owner_task_id = 0;
        std::string tool_name;
        std::function<bool()> presenter;
        std::shared_ptr<std::promise<bool>> decision;
    };

    mutable std::mutex mutex_;
    std::vector<Slot> pending_;
    std::uint64_t next_id_ = 1;
    bool server_present_ = false;
};

// 进程内一只(交互会话与工具线程/监听线程三方共用;单发场景天然无并发)。
ApprovalChannel& SessionApprovalChannel();

}  // namespace lubancode::cli
