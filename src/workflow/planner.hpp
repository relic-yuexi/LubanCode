// 受约束动态图(自然语言编排单第 6 批):planner + GraphPatch。
//
// 单子"动态编排分两层做"第二层:Planner 运行中生受限子图。规矩(原文):
//   - planner 只能从定义列出的 node templates 与 tool allowlist 选积木。
//   - 它输出一份 GraphPatch,不是 shell、源码或任意 tool name。
//   - 宿主重跑全套图校验,查 max_nodes/max_depth/max_iterations 与预算。
//   - 新图越过预批权限、添副作用节点、换密钥或扩路径时,必须再问用户。
//   - GraphPatch 与批准结果写进 run journal,恢复时照旧图重放,不再问
//     模型"当时大概想了什么"。
//   - planner 不得修改已成功节点,只可添后继、收窄未跑分支或明确取消
//     待跑节点。
//
// 首版约束:patch 只许"append 后继节点 + 接边",不许删、不许改既有节点。

#pragma once

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "workflow/definition.hpp"
#include "workflow/validator.hpp"

namespace lubancode::workflow {

// planner 可用的积木:定义方在 WorkflowDefinition 里授权过的节点模板与
// 工具白名单(单子:只能从定义列出的 node templates 与 tool allowlist
// 选积木)。定义不授权,planner 就无米下锅。
struct PlannerAllowance {
    std::vector<WorkflowNode> node_templates;  // 可添的节点模板(kind 已定)
    std::vector<std::string> tool_allowlist;   // 模板里 tool 节点可用的工具名
    int max_patch_nodes = 8;                   // 一份 patch 至多添几只节点
};

// 从定义的 nodes 里抽 allowance:定义可以给节点加 label 前缀 "template:"
// 声明它是积木(不进执行图);没有声明时 allowance 为空(planner 关死)。
PlannerAllowance ExtractAllowance(const WorkflowDefinition& def);

// 一份补丁:只许 append。
struct GraphPatch {
    std::vector<WorkflowNode> append_nodes;
    std::vector<WorkflowEdge> append_edges;
    // 从哪只节点之后接(必须是已完成节点或 patch 自己添的节点)。
    std::string attach_after;
};

std::string SerializeGraphPatch(const GraphPatch& patch);
std::optional<GraphPatch> ParseGraphPatch(const std::string& text);

// 补丁应用(纯函数):返回新定义;原定义不动。
// 硬规矩:不许改/删既有节点;attach_after 必须是既有节点;patch 节点 id
// 不得与既有冲突;添的节点必须在 allowance 的模板集里(工具还得过
// allowlist);应用结果还要过全套 ValidateDefinition。
struct PatchRejection {
    std::string code;     // patch_forbidden_node / patch_unknown_attach / ...
    std::string message;
};

std::expected<WorkflowDefinition, PatchRejection> ApplyGraphPatch(
    const WorkflowDefinition& def, const GraphPatch& patch, const PlannerAllowance& allowance,
    const std::optional<CapabilityTable>& capabilities);

// 应用后的全套校验(含 max_nodes):单独给,方便宿主在"问用户"之前先看
// 会不会越帽。
ValidationResult ValidatePatchedDefinition(const WorkflowDefinition& patched, const WorkflowDefinition& original);

// patch 是否要再问用户(单子:越过预批权限、添副作用节点、换密钥或扩
// 路径时必须再问)。默认最窄。
struct PatchRisk {
    bool adds_side_effects = false;   // 添了副作用节点
    bool expands_paths = false;       // 引用了包外路径
    bool changes_scope = false;       // 添的节点种类超出原图用过的种类
    std::vector<std::string> reasons;
};

PatchRisk AssessPatchRisk(const WorkflowDefinition& patched, const WorkflowDefinition& original);

}  // namespace lubancode::workflow
