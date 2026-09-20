// 审批通道(按代理状态投影单 P2:确认菜单改"提交审批请求→UI 显示→
// 独立响应通道返回决策";P3 补目标绑定收口)。
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
// 目标绑定(P3,§六"审批请求绑定 request ID/owner key/turn ID,确认只解
// 一笔,任务退场/超时/取消后旧审批按钮失效"):
//   - request id:每笔 Submit 发号,Resolve 只按号解一笔——答 A 不误答 B;
//   - owner:别页的请求取不走;owner 退场(任务取消/清条目)经
//     DenyPendingForOwner 按拒收口,旧按钮(菜单)自然失效;
//   - turn id:主轮回合收口经 DenyPendingForTurn 把本轮回合的悬账按拒
//     收口——回合死了,future 不悬死工具线程;
//   - session generation:/clear、/resume 换代经 DenyStaleGenerations 把
//     旧世代的悬账整批拒收——任务号即使重用,旧世代的审批也串不进新会话
//     (任务号本进程单调不重用,这是第二道保险)。
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
    // 跑 presenter(旧路,一字不差)。session_generation/turn_id 是 P3 的
    // 目标绑定(见文件头);0/空串 = 不绑定(单测与旧调用方)。
    std::optional<std::future<bool>> Submit(int owner_task_id, std::string tool_name,
                                            std::function<bool()> presenter,
                                            std::uint64_t session_generation = 0,
                                            std::string turn_id = std::string());

    // 监听线程:取"当前查看页"的最早一笔(先到先答);没有则 nullopt。
    // 取走即从待决表摘除——同一笔不会被问两遍;resolve 由监听线程随后调。
    std::optional<Pending> TakeForViewer(int viewed_task_id);

    // 监听线程:把裁定送回工具线程的 future。id 陌生(已撤)时 no-op。
    void Resolve(std::uint64_t id, bool allowed);

    // 悬着的请求里有没有"属于 task_id 之外"的页——底栏通知位用(其他页
    // 待审批时标出来,不抢当前页)。
    bool HasPendingOutside(int task_id) const;

    // 通知位的页标签:悬着的请求里不属于 viewed_task_id 的页各有谁
    // ("main" / "#3")。按出现次序去重,给底栏拼"XX 待审批"用。
    std::vector<std::string> PendingOwnerLabelsOutside(int viewed_task_id) const;

    // 悬着的请求总数(通知位/诊断)。
    std::size_t PendingCount() const;

    // 打断收口(ESC/interrupt_turn):全部悬着的按拒绝 resolve——轮要收
    // 场,future 不能悬死工具线程。
    void DenyAllPending();

    // 目标绑定的收口(P3):owner 退场(面板 x 停止/清条目)把该页悬着的
    // 审批按拒收口——旧审批按钮随任务退场失效,不等人来答一笔死账。
    void DenyPendingForOwner(int owner_task_id);

    // 回合收口:本轮 turn_id 悬着的按拒收口(RunTurn 尾部的保险)。
    void DenyPendingForTurn(const std::string& turn_id);

    // 会话换代收口:世代对不上 current 的悬账(绑定过世代的)整批按拒
    // 收口——/clear、/resume 之后旧会话的审批串不进新会话。
    void DenyStaleGenerations(std::uint64_t current_generation);

    // 监听线程起跑/停表时登记/注销服务者。Submit 看"服务者在场"决定
    // 挂起还是让调用方就地问(单发/管道没有监听线程)。
    void RegisterServer();
    void ClearServer();

private:
    struct Slot {
        std::uint64_t id = 0;
        int owner_task_id = 0;
        std::uint64_t session_generation = 0;  // 0 = 未绑定(单测/旧路)
        std::string turn_id;                   // 空 = 未绑定
        std::string tool_name;
        std::function<bool()> presenter;
        std::shared_ptr<std::promise<bool>> decision;
    };

    // 把命中谓词的悬账整批按拒收口(promise 锁外 resolve,同 DenyAll 的
    // 纪律:小锁内挑账,锁外送信)。
    void DenyWhere(const std::function<bool(const Slot&)>& matches);

    mutable std::mutex mutex_;
    std::vector<Slot> pending_;
    std::uint64_t next_id_ = 1;
    bool server_present_ = false;
};

// 进程内一只(交互会话与工具线程/监听线程三方共用;单发场景天然无并发)。
ApprovalChannel& SessionApprovalChannel();

}  // namespace lubancode::cli
