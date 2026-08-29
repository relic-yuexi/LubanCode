// PackageCatalog 侧的分析入口(统一 Package 封装单阶段 2):在阶段 1 的
// inventory 之上跑契约 §八 的第 3-6 步——
//   3. 逐件调原生 parser(任何组件坏,整包 invalid,但逐件诊断不停摆);
//   4. 解析 package-local 短引用与 canonical 全名引用(悬空报结构化错);
//   5. 工具 wire 名检查(编码、长度帽);
//   6. 产 PackageMountPlan(只读计划,不挂载)。
//
// 引用规矩(单子 §七):包内引用写短名,Resolver 先在本包里找;再认显式
// canonical id(<包id>:<local名>),全名跨包引用须指向已存在包(四层扫描
// 账里有的);短名出包不猜——本包没有、外部命名空间(standalone skill、
// config.json 的 mcpServers 键、builtin agent 等,由调用方喂进来)也没有,
// 就是悬空,报结构化错。
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "package/component.hpp"
#include "package/inventory.hpp"
#include "package/mount_plan.hpp"

namespace lubancode::package {

// ---------------------------------------------------------------------------
// 跨包组件索引:全名引用的"已存在包"账。轻扫目录(不算哈希),从扫描
// 候选建;manifest 解析失败的包不进(引用它必悬空,错误里有的说)。
// ---------------------------------------------------------------------------
struct PackageComponentSet {
    std::string package_id;
    std::filesystem::path package_root;
    std::set<std::string> agents;
    std::set<std::string> prompt_profiles;
    std::set<std::string> skills;
    std::set<std::string> workflows;
    std::set<std::string> plugins;
    std::set<std::string> mcp_servers;

    bool Has(ComponentKind kind, const std::string& local_id) const;
};

struct PackageRefIndex {
    std::map<std::string, PackageComponentSet> packages;  // key = package id

    const PackageComponentSet* Find(const std::string& package_id) const;
};

// 从扫描候选建索引。每只候选只读目录认组件,不读组件内容、不算哈希。
PackageRefIndex BuildPackageRefIndex(const std::vector<PackageCandidate>& candidates);

// ---------------------------------------------------------------------------
// 外部命名空间:包外可以合法短引的既有名字(各自的原生账)。调用方有就
// 喂,没有留空(引用会按悬空报,文案里指明短名出包须写全名)。
// ---------------------------------------------------------------------------
struct ExternalNamespaces {
    std::set<std::string> skills;       // standalone Skill(官方/用户/项目各层)
    std::set<std::string> agents;       // builtin + user/project 自定义 Agent
    std::set<std::string> workflows;    // standalone Workflow id
    std::set<std::string> mcp_servers;  // 用户/项目 config.json 的 mcpServers 键
    std::set<std::string> tools;        // 已注册工具名(workflow tool 节点兜底)
};

// ---------------------------------------------------------------------------
// 一条引用的解析账。
// ---------------------------------------------------------------------------
struct ComponentRef {
    std::string from;        // 引用方 canonical id(展示用)
    ComponentKind from_kind = ComponentKind::Skill;  // 归属要 kind+local 一起看:
    std::string from_local_id;                        // canonical id 不带 kind 段,同名
                                                      // Agent 与 Profile 不算撞名(§六)
    std::string field;       // 字段路径,如 "prompt.profile"、"nodes.verify.agent"
    std::string raw;         // 原文写的引用
    bool is_canonical = false;  // 写的是 <包id>:<local> 全名
    bool is_file_ref = false;   // workflow 的 prompts/*.md 一类文件引用
    bool resolved = false;
    bool in_package = false;    // 解析到本包组件
    std::string target;         // 解析到的 canonical id / 外部裸名 / 文件路径
    std::string message;        // 悬空或需要说明时的人话

    std::string Format() const;  // doctor 一行
};

// ---------------------------------------------------------------------------
// 分析结果:一份"解析、信任、诊断状态"的整包账(单子 §十一 PackageRecord)。
// ---------------------------------------------------------------------------
struct PackageRecord {
    PackageInventory inventory;                 // 阶段 1 的静态账
    std::vector<ParsedComponent> components;    // 六类逐件(坏的也在,诊断全给)
    std::vector<ComponentRef> references;       // 引用解析全账
    std::optional<PackageMountPlan> mount_plan;  // valid 才有;invalid 时 nullopt
    bool valid = false;  // manifest 干净 + 无 Error 诊断 + 引用全闭合
    // code 组件的信任门状态(阶段 4):mount_plan 里的 code 件 trusted 随
    // 它定,mounting 的连坐账(依赖未信任 code 的 Agent/Workflow 不可用)
    // 也吃它。invalid 包恒 NoCode(没有 plan 可言)。
    CodeTrustStatus code_trust = CodeTrustStatus::NoCode;

    const ParsedComponent* FindComponent(ComponentKind kind, const std::string& local_id) const;
};

// 分析一只包。options 只为复用 BuildPackageInventory 的口径(版本/平台检
// 查)。ref_index 给全名跨包引用对账;external 给包外短名兜底。trust 给
// 信任账的只读视图(阶段 4):nullptr / 默认 = 谁都没批,code 件一律
// PendingTrust(免审层除外——user/official 放置即信任)。
struct PackageTrustSnapshot;
PackageRecord AnalyzePackage(const PackageCandidate& candidate, const ScanOptions& options,
                             const PackageRefIndex& ref_index,
                             const ExternalNamespaces& external = ExternalNamespaces{},
                             const PackageTrustSnapshot* trust = nullptr);

}  // namespace lubancode::package
