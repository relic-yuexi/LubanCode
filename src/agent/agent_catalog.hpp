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
enum class AgentSourceLayer { Builtin, User, Project };
std::string ToString(AgentSourceLayer layer);

// Catalog 里的一条:一个名字的最终归属。definition 缺席(解析失败/重名
// 冲突)= 不可用,issues 带第一条错。
struct AgentCatalogEntry {
    std::string name;  // 解析失败时退回文件名去扩展名,名目还在、只是不可用
    std::optional<AgentDefinition> definition;
    AgentSourceLayer layer = AgentSourceLayer::Builtin;
    std::string file;  // UTF-8 来源路径;码内注册的写 "(builtin)"
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
// 与 SkillLoader 同款;路径会先做 lexically_normal 规范化)。
struct AgentCatalogScanRoots {
    std::optional<std::filesystem::path> builtin_dir;   // <embedded>/agents(可空)
    std::optional<std::filesystem::path> user_dir;      // ~/.lubancode/agents(可空)
    std::optional<std::filesystem::path> project_dir;   // <项目根>/.lubancode/agents(可空)
};

// 码内内置定义(单子 6.2:general-purpose 与 Explore 先登进 Catalog,行为
// 照旧——阶段 1 没有任何调用路径吃这份 Catalog)。
//   general-purpose:与 AgentTool 的 general-purpose 同性——全工具、多步任务。
//   Explore:只读代码搜索代理,allow 表如实记 ExploreAllows 那五枚工具。
AgentDefinition BuiltinGeneralPurposeDefinition();
AgentDefinition BuiltinExploreDefinition();

// 扫描三层并合并。总是先垫进码内两个内置定义(builtin 层),再叠磁盘层。
AgentCatalog LoadAgentCatalog(const AgentCatalogScanRoots& roots);

}  // namespace lubancode::agent
