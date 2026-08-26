// SubagentScheduler(骨架拆解批三·病十四:AgentTool 六职拆分之调度件)。
// 派工治理(规格"递归派工不能再靠拿掉工具解决")的并发槽与前台深度账:
//   max_active  全局并发槽:同时跑着的子代理任务(前台 + 后台)上限,
//               超过就明报"等一项收尾",不再每层各算各的;
//   max_depth   前台派工嵌套深度上限(main=0,子=1,孙=2……),超过明报。
// 两者都来自配置(subagent.max_active / subagent.max_depth),默认值公开
// (config.hpp 的 kDefaultSubagentMaxActive/MaxDepth)。
// 计数是 RAII:出入各一笔,拒绝路径也照退。递归失控不靠"子表拿掉 agent
// 工具"防——子表挂的是 AgentDispatchTool 转发壳,真闸在这里。
#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace lubancode::tools {

class SubagentScheduler {
public:
    // 占住的槽,RAII:析构时把占过的账各退一笔(异常路径也退)。
    class Slot {
    public:
        ~Slot();
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

    private:
        friend class SubagentScheduler;
        Slot(std::atomic<int>* active, std::atomic<int>* depth);
        std::atomic<int>* active_ = nullptr;
        std::atomic<int>* depth_ = nullptr;
    };

    // 会话层从配置灌(SetDispatchGovernance 的旧口转这);非正值按 1 收。
    void SetGovernance(int max_active, int max_depth);

    int max_active() const { return max_active_; }

    // 占槽:全局并发先占上(前台 + 后台都算),满了明报等收尾;foreground
    // 再记一层嵌套深度,超限明报。成功返回 RAII 守卫;超限返回 nullptr 并把
    // 模型可见的错误文案写进 error_out(空指针 = 只报 nullptr,不拼文案)。
    // 两笔账都是先占后查——拒绝路径靠守卫析构照退,不漏账。
    std::unique_ptr<Slot> Enter(bool foreground, std::string* error_out = nullptr);

private:
    int max_active_ = 8;  // 与 kDefaultSubagentMaxActive 同值;会话层从配置灌
    int max_depth_ = 3;   // 与 kDefaultSubagentMaxDepth 同值;1 = 子代理不再往下派
    std::atomic<int> active_{0};        // 当前跑着的任务总数(前台+后台)
    std::atomic<int> foreground_depth_{0};  // 前台嵌套深度(同步栈上的一层算一层)
};

}  // namespace lubancode::tools
