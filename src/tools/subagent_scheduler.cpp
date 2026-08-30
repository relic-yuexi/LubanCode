// subagent_scheduler.hpp 的实现:纯判决函数,无状态、无锁。
#include "tools/subagent_scheduler.hpp"

#include <utility>

namespace lubancode::tools {

AgentAdmission EvaluateAdmission(const AgentAdmissionRequest& request, const AgentLedgerStats& stats,
                                 const SubagentGovernance& governance) {
    if (request.coordinator_closing) {
        return AgentAdmission::Deny(
            "session_closing",
            "会话正在收场,不再接受新的子代理派工。请直接在当前对话里收尾。");
    }
    if (request.parent_task_id != 0 && !request.parent_alive) {
        return AgentAdmission::Deny(
            "parent_finished",
            "父任务已结束,不能再替它派子任务。请由当前代理直接完成,或重新派一项新任务。");
    }
    // 深度对账:requested_depth 必须是 parent.depth + 1(事务里由台账按父
    // 记录再验一遍,这里的对账覆盖"根任务必须 depth=1"的下界)。
    if (request.requested_depth < 1) {
        return AgentAdmission::Deny("bad_depth", "派工深度不合法:深度至少为 1(main=0,子=1)。");
    }
    if (request.requested_depth > governance.max_depth) {
        return AgentAdmission::Deny(
            "depth_limit",
            "已达子代理派工深度上限(" + std::to_string(governance.max_depth) +
                " 层,subagent.max_depth 可调):请把任务拆平后再派,或由当前代理直接完成。");
    }
    if (stats.alive_count >= static_cast<std::size_t>(governance.max_active)) {
        return AgentAdmission::Deny(
            "active_limit",
            "子代理并发槽已满(" + std::to_string(governance.max_active) +
                " 路同时在跑,前台后台合计):请等一项收尾,或调大 subagent.max_active。");
    }
    if (governance.max_children_per_task > 0 &&
        stats.parent_children_count >= static_cast<std::size_t>(governance.max_children_per_task)) {
        return AgentAdmission::Deny(
            "children_limit",
            "这只任务已派满 " + std::to_string(governance.max_children_per_task) +
                " 个子任务(subagent.max_children_per_task):请由当前代理直接完成,或整合现有结果。");
    }
    if (governance.max_tree_nodes > 0 &&
        stats.tree_nodes_count >= static_cast<std::size_t>(governance.max_tree_nodes)) {
        return AgentAdmission::Deny(
            "tree_nodes_limit",
            "这棵任务树已满 " + std::to_string(governance.max_tree_nodes) +
                " 个节点(subagent.max_tree_nodes):请由当前代理直接完成,或整合现有结果。");
    }
    return AgentAdmission::Allow();
}

}  // namespace lubancode::tools
