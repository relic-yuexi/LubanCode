// SubagentScheduler(骨架拆解批三拆出;递归派工单 P0-2 改成纯 admission
// policy)。从前的"全局并发原子 + 前台深度原子"是两本野账:
//   active_           两只并行根任务互不相扰没错,但它复制了台账已有的
//                     "谁还在跑"——同一笔账两处记,迟早漂;
//   foreground_depth_ 算的是"此刻同步栈上有几层",两只并行根会互相抬数,
//                     后台任务压根不记——放开后台递归后这本账立刻失真
//                     (单子缺口 B)。
// 现在两者都删了。深度与活跃数只有一处真账:TaskLedger 的 lineage
// (parent/root/depth)与活态计数。本文件只剩判决函数:
//   EvaluateAdmission(request, stats, governance) -> allow/deny + 稳定文案
// 它不自存任何状态、不摸锁——吃调用方在台账锁内拍好的快照,只在注册
// 事务(TaskLedger::TryRegisterChild)的那一笔里用,不拿出去排队后再落账
// (admission 结果出了事务就作废)。
#pragma once

#include <cstddef>
#include <string>

namespace lubancode::tools {

// 派工治理配置(单子 §7.4/§14.1):前两枚是既有配置,后两枚给 0 = 不设
// (P1-2 才接配置解析,这里先把判决口备好)。
struct SubagentGovernance {
    int max_active = 8;             // 同时存活的子任务节点上限(含等孩子的父)
    int max_depth = 3;              // lineage 深度上限(main=0,子=1……)
    int max_children_per_task = 0;  // 一只父任务累计可派孩子数;0 = 不设
    int max_tree_nodes = 0;         // 一棵根树累计节点上限;0 = 不设
};

// admission 请求:显式 lineage(单子 §7.2)。requested_depth 由宿主按
// caller.depth + 1 算,模型不得自报。
struct AgentAdmissionRequest {
    int parent_task_id = 0;     // 0 = main 派出
    int requested_depth = 1;    // 必须等于 parent.depth + 1(事务里对账)
    int root_task_id = 0;       // 父的 root;根任务派出时为 0(分 id 后即自 root)
    bool parent_alive = true;   // 父任务存在且仍在运行(事务里验)
    bool coordinator_closing = false;  // 会话收场,拒收新派工
};

// 台账锁内快照:EvaluateAdmission 只吃这份,不回头查台账。
struct AgentLedgerStats {
    std::size_t alive_count = 0;           // 活任务数(Running/WaitingChildren/Completing)
    std::size_t parent_children_count = 0; // 该父累计已派孩子数(终态也算)
    std::size_t tree_nodes_count = 0;      // 该根树累计节点数(终态也算)
};

// 判决结果:allowed 为假时 message 是模型可见文案(含限额与可行去路),
// error_code 是稳定标识(测试与诊断用)。
struct AgentAdmission {
    bool allowed = false;
    std::string error_code;
    std::string message;

    static AgentAdmission Allow() { return AgentAdmission{true, std::string(), std::string()}; }
    static AgentAdmission Deny(std::string code, std::string message) {
        return AgentAdmission{false, std::move(code), std::move(message)};
    }
};

// 审查次序(单子 §7.2):closing -> 父活 -> 深度对账 -> 深度上限 -> 活跃
// 槽 -> 每父孩子数 -> 树节点数。任一门不过即拒;文案与旧调度器逐字兼容
// ("子代理并发槽已满…"、"已达子代理派工深度上限…"),既有测试与模型
// 引导不漂移。
AgentAdmission EvaluateAdmission(const AgentAdmissionRequest& request, const AgentLedgerStats& stats,
                                 const SubagentGovernance& governance);

}  // namespace lubancode::tools
