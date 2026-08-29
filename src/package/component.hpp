// 组件解析(统一 Package 封装单阶段 2):ComponentSourceRoot 与六类组件
// loader。规矩一条:Package 层不复制任何组件的 schema——Skill frontmatter
// 归 tools::ParseSkillMarkdown,Workflow 归 workflow::ParseWorkflowYaml,
// Agent 归 agent::ParseAgentDefinitionYaml,plugin.json 归
// runtime::ParsePluginManifest;这里只把"包根 + 组件相对目录"递给原生
// parser,把原生错误透传出来,再按包的规矩补三样原生 parser 管不到的账:
//   1. local id 规矩(目录名/文件名是否小写 kebab-case,契约 packages.md §2);
//   2. 名字一致性(local id 与 frontmatter/manifest id 是否一致);
//   3. mcp.yaml——它是 Package 自己的格式(契约 §5),原生 parser 就是
//      本文件里的 ParseMcpComponentYaml,阶段 5 再映射到 McpServerConfig。
// Prompt Profile 无单一入口(目录整体即组件),这里的"原生规矩"是模块树
// 覆盖范围(core/features/platforms 可换,modes 不可,契约 agents.md §6.3)。
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/agent_definition.hpp"
#include "package/inventory.hpp"  // PackageScope
#include "runtime/plugin_contract.hpp"
#include "tools/skill_loader.hpp"
#include "workflow/definition.hpp"

namespace lubancode::package {

// 六类组件(契约 packages.md §2 的 ComponentKind)。
enum class ComponentKind { Agent, PromptProfile, Skill, Workflow, Plugin, McpServer };
std::string_view ComponentKindName(ComponentKind kind);
// doctor/清单展示用的目录名("agents"/"mcp"...)。
std::string_view ComponentKindDir(ComponentKind kind);

// 一件组件的来源描述(单子 §十一):包根 + 组件相对位置 + 归属。各组件
// loader 都收这个,不自己扫 Package;反向(Package 扫组件)只归 catalog。
struct ComponentSourceRoot {
    std::filesystem::path package_root;    // 包根(只读看待)
    std::filesystem::path component_path;  // 组件本体:入口文件或目录
    std::string rel_path;                  // 包内相对路径(UTF-8、'/' 分隔)
    std::string local_id;                  // 目录名 / 文件名去扩展
    ComponentKind kind = ComponentKind::Skill;
    PackageScope scope = PackageScope::User;
    std::string package_id;       // 解析失败时可能为空
    std::string package_version;  // 原文;没写为空
    std::string content_hash;     // 包内容哈希(阶段 1 盘点出的那份)
    bool trusted_for_code = false;  // 阶段 4 之前恒 false
};

// 一条组件级诊断:从顶层 Package 一路指到文件、行、字段(单子阶段 2 清
// 单)。path 是包内相对路径(或再往内一层),line/column 1 起、-1 = 拿不到
// (原生 parser 的位置透传)。
struct ComponentIssue {
    bool error = true;  // false = warning(不挡整包)
    std::string path;
    int line = -1;
    int column = -1;
    std::string field;
    std::string message;

    std::string Format() const;  // "agents/x.yaml:12:3 `tools.allow`: ..." 一句人话
};

// mcp.yaml(契约 §5)的解析产物。字段规矩全照契约表;阶段 5 把它映射成
// McpServerConfig 起服,这里只做静态形状与占位符检查,不起任何进程。
struct McpComponentDefinition {
    int schema = 1;
    std::string id;
    std::string description;
    std::string transport;  // 首版只认 "stdio"
    std::string command;    // runtime.command
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;  // 只许 "${env:NAME}" 占位
    int timeout_ms = 30000;
    bool network_allowed = false;  // permissions.network;首版只记账不执法
};

// mcp.yaml 解析错:字段路径 + 行 + 人话(与 ManifestError 同款口径)。
struct McpComponentError {
    std::string field;
    int line = 0;  // 1 起;0 = 拿不到
    std::string detail;
};

// 严格解析一份 mcp.yaml 文本。package_root 用于 ${package_dir} 展开后的
// 越界检查(契约 §5 占位符规矩:规范化后逃出包根即拒)。
std::expected<McpComponentDefinition, std::vector<McpComponentError>> ParseMcpComponentYaml(
    std::string_view yaml_text, const std::filesystem::path& package_root);

// 一件组件的解析结果。ok=false 时 definition 类字段全空,issues 把 error
// 与 warning 攒全——"任何组件坏,整包 invalid,但其余组件照样逐个诊断"
// (不因第一个错停摆)靠的就是这份逐件账。
struct ParsedComponent {
    ComponentKind kind = ComponentKind::Skill;
    std::string local_id;
    std::string canonical_id;  // <package-id>:<local-id>
    std::string rel_path;
    bool ok = false;
    std::vector<ComponentIssue> issues;

    // ---- 原生产物(ok 时才有值;引用解析吃这些,不再重读文件) ----
    std::optional<agent::AgentDefinition> agent;
    std::optional<workflow::WorkflowDefinition> workflow;
    std::optional<runtime::PluginManifest> plugin;
    std::optional<McpComponentDefinition> mcp;
    std::optional<tools::ParsedSkillFile> skill;
    std::vector<std::string> profile_files;  // Prompt Profile 的覆盖文件(包内相对)

    bool HasError() const;
};

// 读组件本体并调原生 parser。文件读不动折成一条 error issue(带路径),
// 不抛异常。整件只诊不执行:Plugin/MCP 的 command 一个都不起。
ParsedComponent ParsePackageComponent(const ComponentSourceRoot& source);

}  // namespace lubancode::package
