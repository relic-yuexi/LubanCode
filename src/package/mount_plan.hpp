// PackageMountPlan(统一 Package 封装单阶段 2):静态挂载计划——哪件组件
// 以什么 canonical id / wire 名挂到哪张表,依赖谁。只产计划,不执行:
// 不启动 Plugin 与 MCP,不注册任何工具,不写任何 Catalog(单子 §三
// "正路是 Package 先产出一张 Mount Plan,再把组件根目录交给原有解析器
// 和 Catalog"——交给那是阶段 3/5 的事)。
//
// 整包成整包败(单子 §十):前六步有一处错,plan 整张不出。broken 组件
// 不进 plan,但诊断照逐件给(见 PackageRecord)。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "package/component.hpp"

namespace lubancode::package {

// 挂载目标表(单子 §三的六张表;名字即给 doctor/审计看的去处)。
std::string MountTargetTable(ComponentKind kind);

// code 组件的信任门状态(阶段 4):NoCode = 包里没有 plugin/mcp;Trusted =
// 过了门(免审层放置即信任,或账上有这枚哈希);PendingTrust = 待信任
//(外来层未批,或文件动过哈希对不上)。信任的是那枚哈希,哈希变即失效。
enum class CodeTrustStatus { NoCode, Trusted, PendingTrust };
std::string_view CodeTrustStatusText(CodeTrustStatus status);

struct MountPlanTool {
    std::string short_name;   // manifest 里的工具短名
    std::string wire_name;    // plugin__<编码>__<tool>(发 provider 与权限账)
    std::string display_name; // plugin__<带点 canonical>__<tool>(给人看)
};

struct MountPlanEntry {
    ComponentKind kind = ComponentKind::Skill;
    std::string local_id;
    std::string canonical_id;
    std::string rel_path;         // 包内相对位置
    std::string target_table;     // 挂到哪张表(AgentCatalog / SkillCatalog / ...)
    std::string source_root;      // 组件根(包内相对;挂载时递给原生系统的位)
    std::string wire_component_id;  // 编码后的 <pkg>.<local>(plugin/mcp 有)
    std::vector<MountPlanTool> tools;  // plugin 的逐件工具(mcp 工具要握手才知道)
    std::vector<std::string> depends_on;  // 解析到的本包依赖(canonical id,去重保序)
    bool code_bearing = false;    // plugin/mcp:挂载要过信任门
    bool trusted = false;         // code 件的门禁(PendingTrust 时 false,内容件不参与)
};

struct PackageMountPlan {
    std::string package_id;
    std::string package_version;
    std::string content_hash;
    std::vector<MountPlanEntry> entries;  // 按六类目录序、local id 字节序,稳定
    CodeTrustStatus code_trust = CodeTrustStatus::NoCode;  // 整包门禁(阶段 4)

    std::size_t CountKind(ComponentKind kind) const;
    bool HasCodeBearing() const;
};

}  // namespace lubancode::package
