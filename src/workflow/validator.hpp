// Workflow 校验器(自然语言编排单第 1 批):结构、引用、可达性、环上限、
// 类型与能力,开跑前全查一遍。
//
// 校验错误必须落到人看得懂的位置(单子"自然语言编译器"):
//   nodes.arxiv.input.limit 这种点路径,不只吐 "invalid workflow"。
//
// 分两类输入:
//   - 定义自身查得动的:形状自洽、边/节点互指、可达性、环上限、outcome
//     歧义、${...} 引用形状、alias/id 合法、retry 与副作用幂等约束。
//   - 要问外界的:tool/skill/transform/subflow 存不存在——经 CapabilityTable
//     注入只读能力表(单子编译器第 2 步"查能力"),validator 不 include
//     tools/*,依赖仍旧单向。

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "workflow/definition.hpp"

namespace lubancode::workflow {

// 校验错误:带点路径 + 人话 + (可选)稳定 code。
struct ValidationIssue {
    std::string code;      // 如 "dangling_edge" / "unknown_tool" / "unbounded_loop"
    std::string path;      // nodes.arxiv.input.limit
    std::string message;   // 人话
};

// 只读能力表(单子"查能力"):ToolRegistry、SkillCatalog、WorkflowCatalog
// 的投影。宿主装配;validator 拿它对账,模型/编译器也拿同一份——模型不能
// 凭空杜撰 google_scholar_search,先决条件是宿主只喂真实存在的名字。
struct CapabilityTable {
    std::vector<std::string> tools;      // 稳定 tool id(plugin__papers__arxiv_search 一类)
    std::vector<std::string> skills;     // skill 名
    std::vector<std::string> transforms; // 宿主注册的纯数据变换名
    std::vector<std::string> workflows;  // 可作 subflow 的 workflow id(不含自身)
    std::vector<std::string> agent_roles; // 现有子代理角色名(空表 = 不限制)
};

// 校验结果:issues 空 = 过。
struct ValidationResult {
    std::vector<ValidationIssue> issues;

    bool ok() const { return issues.empty(); }

    // 首条错误的 code(给 slash 命令的即时回执)。
    std::string first_code() const { return issues.empty() ? std::string() : issues.front().code; }
};

// ${...} 引用的静态检查所需:节点执行序的保守估计(拓扑层级)。环上的
// 节点由可达性/环检查另行报错;这里只给"引用不指向未来节点"用。
std::map<std::string, int> EstimateTopoOrder(const WorkflowDefinition& def);

// 全量校验。capabilities 可为 nullopt(纯结构校验,创建向导早期用)。
ValidationResult ValidateDefinition(const WorkflowDefinition& def,
                                    const std::optional<CapabilityTable>& capabilities);

}  // namespace lubancode::workflow
