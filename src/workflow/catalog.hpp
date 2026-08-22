// WorkflowCatalog(自然语言编排单第 1 批):项目级/用户级扫描、来源与冲突。
//
// 规矩(单子"文件与安装范围"与"Slash alias 与冲突规矩"):
//   - 项目级 <project>/.lubancode/workflows/<id>/workflow.yaml;
//     用户级 <home>/.lubancode/workflows/<id>/...。
//   - 项目级同 id 胜用户级;list/show 标来源,不静默遮住。
//   - 定义、prompt 与 fixture 一并算内容 hash(这里的 hash 只算 workflow.yaml
//     本体;包内文件变化经 BumpContentHashWithFiles 并入,安装/编辑时调用)。
//   - 跨类撞名(alias 撞内建 slash、撞 Skill、撞另一 Workflow)不靠阴影顺序
//     猜:发现即禁用直呼 alias,只留 /workflow run <id>,doctor 报明白。
//
// 目录不存在 = 空 catalog,不是错误(没装过 workflow 的机器是常态)。

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "workflow/definition.hpp"
#include "workflow/parser.hpp"

namespace lubancode::workflow {

enum class WorkflowScope { Project, User };

std::string ToString(WorkflowScope scope);

// 一份已装载的定义条目。
struct CatalogEntry {
    WorkflowDefinition definition;
    WorkflowScope scope = WorkflowScope::Project;
    std::filesystem::path dir;   // workflow 目录(含 workflow.yaml)
    std::string content_hash;    // 定义的 SHA-256
    // 解析失败的定义也进 catalog(状态标出来,不算装死):definition.id 空。
    bool broken = false;
    std::vector<ParseIssue> issues;  // broken 时的原因
};

// 一处撞名:id 或 alias 在两份(或两类)之间重了。
struct AliasConflict {
    std::string alias;
    std::string kind;     // "workflow-project" / "workflow-user" / "skill" / "builtin"
    std::string owner;    // workflow id 或 skill 名
};

// 扫描结果:entries 按定义顺序(project 先、同 scope 按 id 字典序);
// conflicts 是跨条目/跨类撞名,由调用方喂入 Skill 名单与内建 slash 名单。
struct Catalog {
    std::vector<CatalogEntry> entries;
    std::vector<AliasConflict> conflicts;
    std::map<std::string, std::string> disabled_aliases;  // alias -> 原因

    // id -> 条目下标(项目级遮用户级的那份只在 entries 里留一份,这里指它)。
    const CatalogEntry* Find(const std::string& id) const;
    // alias 直呼(撞名禁用的不在内);同 alias 项目级胜用户级。
    const CatalogEntry* FindByAlias(const std::string& alias) const;
};

// 撞名检查:workflows 之间的 id/alias 互撞、alias 撞 skill_names、alias 撞
// builtin_slash_names(带斜杠或不带都认)。任何命中都进 conflicts,并把
// 相应 alias 记进 disabled_aliases——"更稳的做法是禁用直呼,只留正门"。
void DetectAliasConflicts(Catalog& catalog, const std::vector<std::string>& skill_names,
                          const std::vector<std::string>& builtin_slash_names);

// alias 合法性(单子"Slash alias 与冲突规矩"):只认 Unicode 字母、数字、
// -、_ 与中文等正常文字;拒绝空白、斜杠、控制符和路径字符。UTF-8 逐码点
// 检查,ASCII 区之外的码点按"正常文字"放行。
bool IsValidAlias(const std::string& alias);

// workflow id 合法性:ASCII 小写字母数字与 '-',首字符须字母;目录名安全。
bool IsValidWorkflowId(const std::string& id);

// 扫两级目录。project_root 传空 optional = 只扫用户级。目录不存在给空表。
Catalog LoadCatalog(const std::optional<std::filesystem::path>& project_root,
                    const std::optional<std::filesystem::path>& user_root);

}  // namespace lubancode::workflow
