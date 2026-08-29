// 会话钉快照与内容组件挂载(统一 Package 封装单阶段 3)。
//
// 首版语义(单子 §十二"会话钉快照",契约 packages.md §9):启动时扫四层、
// 逐包 AnalyzePackage 一次,把 valid 包的内容组件折成四张表的挂载材料;
// 运行中目录变化不热生效——下回启动才见。不做热重载,不监听目录。
//
// 只挂内容组件(单子 §9.1):Skill、Workflow、Agent、Prompt Profile。带
// Plugin/MCP 的包照样挂它的内容组件,代码组件一件不挂不起(阶段 4/5 的
// 信任门与挂载事务),账上标 code_pending_trust。
//
// 挂载路数(单子 §三"正路"):Package 先产分析账(AnalyzePackage),这里
// 把组件根与成品定义交给原有加载器/Catalog——SkillCatalog、AgentCatalog、
// WorkflowCatalog、PromptProfileResolver。加载器不反过来扫 Package;这里
// 也不复制任何组件 schema。
//
// canonical 折名:packaged 组件一律 <包id>:<名> 登册;包内短引用
// (agent 的 prompt.profile / skills.preload,workflow 的 agent/skill/
// subflow)在本包有同名组件时折成 canonical 再交出去,外部裸引用与显式
// 全名原样保留(契约 §6"Resolver 先在本包里找")。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "agent/agent_catalog.hpp"      // PackagedAgentEntry
#include "agent/prompt_assembler.hpp"   // PackageProfileRoot
#include "package/catalog.hpp"          // AnalyzePackage 的产物
#include "tools/skill_loader.hpp"       // PackagedSkillRoot
#include "workflow/catalog.hpp"         // PackagedWorkflowSource

namespace lubancode::package {

// ---------------------------------------------------------------------------
// PackageMount:一场会话钉住的挂载快照。
// ---------------------------------------------------------------------------
struct PackageMountEntry {
    std::string package_id;
    std::string version_text;
    PackageScope scope = PackageScope::User;
    std::filesystem::path package_root;
    std::string content_hash;
    // 内容组件的 canonical id 清账(按 ComponentKind 目录序、local id 字节序)。
    std::vector<std::string> mounted_canonical_ids;
    bool code_pending_trust = false;  // 包里有 plugin/mcp,代码组件待信任门(阶段 4/5)
};

struct PackageMount {
    std::vector<PackageMountEntry> entries;  // 按 package_id 字节序;只收 valid 胜者
    // 各包的完整分析账(挂载材料从这折;也供 /package 命令对账)。会话钉
    // 住的就是这一份——运行中目录怎么变,这里的账不变。
    std::vector<PackageRecord> records;

    bool empty() const { return entries.empty(); }
    const PackageMountEntry* Find(const std::string& package_id) const;
};

// 挂载输入:扫描四层的根 + 包外短引用的兜底账(standalone 技能名、
// config.json 的 mcpServers 键、builtin Agent 名——有就喂,与 /package
// doctor 的 BuildExternalNamespaces 同一口径;不喂则指向它们的短引用按
// 悬空判,整包 invalid)。
//
// store_candidates(自进化闭环阶段 4):evolution version store 的选中版本
// (active/canary 指针指到的那一枚)折成的现成候选,scope=Store。哈希验
// 完好没有由 evolution 侧负责——对不上(手改过 store 内文件)的根本不
// 递进来,这里照单全收。同 id 优先级:dev > project > store > user >
// official(store 压手装的用户层拷贝,让显式调试层)。
struct PackageMountInput {
    ScanOptions scan;
    ExternalNamespaces external;
    std::vector<PackageCandidate> store_candidates;
};

// 启动装配一次。次序:四层扫描 -> 同 id 按优先级定胜者(dev > project >
// store > user > official,被遮的不挂) -> 逐胜者 AnalyzePackage -> valid
// 的收进挂载账(invalid 一件不挂——整包成整包败)。
PackageMount BuildPackageMount(const PackageMountInput& input);

// ---------------------------------------------------------------------------
// 四张表的挂载材料:各自的原生系统收这些,不收 PackageMount 本体。
// 顺序稳定:按包 id、包内目录序;同一份 mount 折两遍,结果一致。
// ---------------------------------------------------------------------------

// Skill loader 的包根(<包根>/skills;source_level 带 scope 标签)。
std::vector<tools::PackagedSkillRoot> MountSkillRoots(const PackageMount& mount);

// AgentCatalog 的包层成品件(定义里的包内短引用已折 canonical)。
std::vector<agent::PackagedAgentEntry> MountAgentEntries(const PackageMount& mount);

// WorkflowCatalog 的包层成品件(id 换 canonical;包内 agent/skill/subflow
// 短引用已折 canonical;task/prompt/template 文件引用原样,dir 指包内)。
std::vector<workflow::PackagedWorkflowSource> MountWorkflowSources(const PackageMount& mount);

// Prompt Profile 的包层根(<包根>/prompts/profiles,canonical 名在此解析)。
std::vector<agent::PackageProfileRoot> MountProfileRoots(const PackageMount& mount);

}  // namespace lubancode::package
