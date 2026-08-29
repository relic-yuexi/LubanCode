// AgentCatalog(自定义 Agent 与 Prompt Profile 单·阶段 1):扫描、分层、
// 去重、列举。三层(单子"加载范围与信任"):
//   builtin  码内注册(general-purpose/Explore)+ <embedded resources>/agents/*.yaml
//   user     ~/.lubancode/agents/*.yaml
//   project  <项目根>/.lubancode/agents/*.yaml(项目根复用 instruction 发现
//            规则——Git 根,config::FindProjectRoot,不各自猜 cwd)
// 加载优先级 project > user > builtin;同层重名报冲突,跨层同名属显式覆盖,
// 被盖住的来源记进 shadowed_sources(inspect/doctor 摆覆盖链)。
//
// 性子与 SkillLoader 同款:单个定义坏了只坏那一个——解析失败的定义登成
// unavailable 条目(带全部诊断),绝不炸整个 Catalog。首版不做热重载,
// 启动(或命令触发)时加载(单子"加载范围与信任")。
//
// 目录路径由调用方算好递进来(App 层知道家目录与项目根),这层不摸环境
// 变量,单测好造三层假目录。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "agent/agent_definition.hpp"

namespace lubancode::agent {

// 来源层,从低到高。ToString 给 /agents、doctor 的来源标签用。
// Package 层(统一 Package 封装单阶段 3)定死在 user 之下、builtin 之上:
//   project > user > package > builtin
// 契约(packages.md §7/agents.md §11)只说包内 Agent 以 canonical id
// (<包id>:<名>)登记、不撞 standalone 裸名,没给包层在四层里的座次——
// 本仓定夺:用户/项目是本地主人自家的话,理论撞名(实际因 canonical 命名
// 空间不相交而不发生)也永远盖过第三方包带来的;builtin 仍是地板。
enum class AgentSourceLayer { Builtin, Package, User, Project };
std::string ToString(AgentSourceLayer layer);

// 一件已解析的 packaged Agent(阶段 3 挂载):Package 层在 AnalyzePackage
// 里已过原生 parser 并把包内短引用(prompt.profile / skills.preload)折成
// canonical,这里只收成品,不重读文件、不重扫包(单子 §十一:loader 收
// ComponentSourceRoot,不反过来扫 Package)。
struct PackagedAgentEntry {
    std::string canonical_name;              // 挂载名 <包id>:<local>(Catalog 的键)
    std::string package_id;                  // 来源包(展示"/agents 带来源包")
    AgentDefinition definition;              // name 保持 local 人话;引用已折 canonical
    std::string file_utf8;                   // agents/<local>.yaml 的 UTF-8 全路径
};

// Catalog 里的一条:一个名字的最终归属。definition 缺席(解析失败/重名
// 冲突)= 不可用,issues 带第一条错。
struct AgentCatalogEntry {
    std::string name;  // 解析失败时退回文件名去扩展名,名目还在、只是不可用
    std::optional<AgentDefinition> definition;
    AgentSourceLayer layer = AgentSourceLayer::Builtin;
    std::string file;  // UTF-8 来源路径;码内注册的写 "(builtin)"
    std::string package_id;  // Package 层非空(阶段 3);standalone 为空
    bool available = true;
    std::vector<AgentDefinitionIssue> issues;  // 解析诊断 + 重名冲突 + 名不符警告
    // 被这一条盖住的低层来源(优先级从高到低列),跨层覆盖的账,inspect 用。
    std::vector<std::string> shadowed_sources;

    // 第一条 error 的单行摘要;没有错返回空串(/agents 的"不可用则写第一条
    // 原因"用)。
    std::string FirstError() const;
};

// 一份加载完的 Catalog。entries 按名字节序稳定排序——同一批文件在任何
// 文件系统枚举顺序下,加载结果完全一致(单子测试账"扫描次序确定")。
struct AgentCatalog {
    std::vector<AgentCatalogEntry> entries;
    // 加载级问题(同层重名冲突、目录读不了),每条一行人话,带来源层。
    std::vector<std::string> load_errors;

    const AgentCatalogEntry* Find(const std::string& name) const;
    std::vector<const AgentCatalogEntry*> Available() const;
};

// 三层扫描根,哪层不要就给 nullopt(目录不存在按"没有那层"静默跳过,
// 与 SkillLoader 同款;路径会先做 lexically_normal 规范化)。packaged 是
// 阶段 3 的包层成品件(空 = 没有包,行为与旧签名一致)。
struct AgentCatalogScanRoots {
    std::optional<std::filesystem::path> builtin_dir;   // <embedded>/agents(可空)
    std::optional<std::filesystem::path> user_dir;      // ~/.lubancode/agents(可空)
    std::optional<std::filesystem::path> project_dir;   // <项目根>/.lubancode/agents(可空)
    std::vector<PackagedAgentEntry> packaged;           // 包内 Agent(会话钉快照折的)
};

// 码内内置定义(单子 6.2:general-purpose 与 Explore 先登进 Catalog,行为
// 照旧——阶段 1 没有任何调用路径吃这份 Catalog)。
//   general-purpose:与 AgentTool 的 general-purpose 同性——全工具、多步任务。
//   Explore:只读代码搜索代理,allow 表如实记 ExploreAllows 那五枚工具。
AgentDefinition BuiltinGeneralPurposeDefinition();
AgentDefinition BuiltinExploreDefinition();

// 扫描三层并合并。总是先垫进码内两个内置定义(builtin 层),再叠磁盘层,
// 最后并入包层成品件(阶段 3:canonical 名与裸名两套命名空间不相交,并入
// 只添行不遮行)。
AgentCatalog LoadAgentCatalog(const AgentCatalogScanRoots& roots);

}  // namespace lubancode::agent
