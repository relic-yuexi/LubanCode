// 自然语言编译器(自然语言编排单第 5 批):人话 -> Workflow Draft -> 图。
//
// 五步(单子"自然语言编译器"原文):
//   1. 提取意图:模型把描述归成 WorkflowDraft(JSON Schema 约束的候选
//      AST);缺字段列 unknowns。
//   2. 查能力:宿主从 ToolRegistry/SkillCatalog/WorkflowCatalog 投只读
//      能力表(CapabilityTable,validator 同一份)。模型不能凭空杜撰
//      google_scholar_search——先决条件是宿主只喂真实存在的名字。
//   3. 最少追问:只问会改变输入、产物、失败语义、权限或图形的岔路;
//      描述里已有的直接填,不照表全问。
//   4. 编译与校验:模型输出只是候选 AST;宿主过全套 validator(schema/
//      引用/图/类型/能力/预算/权限/路径)。错误落到点路径。
//   5. 预览与安装:摘要 + ASCII/Mermaid 图 + 将写文件的 diff 与警告;
//      确认后原子落盘(staging + rename,半截包不进可用清单)并刷新
//      catalog。
//
// 模型侧的意图提取由宿主装配(CompileFn 注入,通常是一次 llm 调用);
// 这里给的是宿主侧的骨架:Draft 校验、追问计算、预览文本、原子安装。

#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "workflow/catalog.hpp"
#include "workflow/definition.hpp"
#include "workflow/validator.hpp"

namespace lubancode::workflow {

// ---------------------------------------------------------------------------
// Draft(模型的候选 AST,宿主侧形状)
// ---------------------------------------------------------------------------

// 编译缺口:一句话 + 指到哪个字段(给前端排问话次序用)。
struct DraftUnknown {
    std::string field;    // "inputs" / "outputs" / "sources" / "failure_policy" ...
    std::string question; // 人话:"你想拿什么作输入?"
    bool blocking = true; // 不问清楚就编不出图
};

struct WorkflowDraft {
    WorkflowDefinition definition;  // 半成品图(可以缺节点)
    std::vector<DraftUnknown> unknowns;
    std::vector<std::string> warnings;  // 能力缺失(alias 撞名等)不拦编译
    bool complete = false;              // unknowns 空 且 图过校验
};

// 意图提取函数:用户描述 + 能力表 -> 候选 Draft(JSON)。宿主注入(通常
// 走 llm);输出永远只是候选,宿主还要过校验。
using IntentCompiler =
    std::function<std::expected<nlohmann::json, std::string>(const std::string& description,
                                                              const CapabilityTable& capabilities)>;

// 模型输出的候选 JSON -> WorkflowDraft(纯函数):字段搬进强类型 AST,
// 搬不动的记 unknowns/warnings。别名/id/alias 合法性、能力对账在这里做
// 第一道;图的完整性归 validator。
std::expected<WorkflowDraft, std::vector<ValidationIssue>> ParseDraftJson(const nlohmann::json& candidate,
                                                                          const CapabilityTable& capabilities);

// Draft -> 追问清单:只列 blocking 的缺口,按"会改变图"的优先级排。
std::vector<DraftUnknown> ComputeClarifications(const WorkflowDraft& draft);

// ---------------------------------------------------------------------------
// 预览
// ---------------------------------------------------------------------------

struct PreviewOptions {
    WorkflowScope scope = WorkflowScope::Project;
    std::filesystem::path install_dir;  // 目标 workflow 目录(打印将写哪些文件)
};

// 预览文本:摘要(输入/产物/工具/并行/失败规矩/审批/预算/alias)、
// ASCII 图、将写文件清单、警告。确认前给人看的那一屏。
std::string BuildPreviewText(const WorkflowDraft& draft, const PreviewOptions& options);

// ---------------------------------------------------------------------------
// 安装(原子:staging + rename)
// ---------------------------------------------------------------------------

struct InstallOptions {
    WorkflowScope scope = WorkflowScope::Project;
    bool overwrite = false;  // 同 id 已在且 hash 不同时,默认拒
};

struct InstallResult {
    std::filesystem::path dir;
    std::string content_hash;
    std::vector<std::string> written_files;
};

// 落盘:staging 目录写全(workflow.yaml + prompts),rename 进
// <root>/.lubancode/workflows/<id>。校验不过/同 id 冲突一律报错,不动
// 已有目录。prompts 参数:相对路径 -> 正文(prompt 文件的内容账)。
std::expected<InstallResult, std::string> InstallWorkflow(
    const WorkflowDefinition& definition, const std::optional<std::filesystem::path>& project_root,
    const std::optional<std::filesystem::path>& user_root, const InstallOptions& options,
    const std::map<std::string, std::string>& prompt_files = {});

// 编辑:改完的定义原子换目录里那份;业务 version 至少补 patch(调用方
// 自己已改),留 .bak 旧定义可回滚(单子"改 Workflow")。
std::expected<InstallResult, std::string> UpdateWorkflow(
    const WorkflowDefinition& definition, const std::filesystem::path& existing_dir,
    const std::map<std::string, std::string>& prompt_files = {});

// enable/disable:catalog 仍列,直呼 alias 不响应(定义里 enabled 字段)。
std::expected<void, std::string> SetWorkflowEnabled(const std::filesystem::path& workflow_dir, bool enabled);

// 移除:默认只删定义目录,运行账(workflow-runs)另设命令(单子:已有
// 运行历史,默认只移定义,不顺手抹运行账)。
std::expected<void, std::string> RemoveWorkflow(const std::filesystem::path& workflow_dir);

}  // namespace lubancode::workflow
