// 部署档(deployment profile schema 1)的生产解析器——工业化多协议接入单
// P1(冻结合同 docs/reference/capability-contract.md §2/§3)。
//
// P0 把这套语义冻结在 tests/unit/capability/test_capability_contract.cpp 的
// 测试侧校验器里;P1 起生产链路(cl_app 的 --app-server-profile)要真吃
// 这份档,解析器落到生产侧,语义与测试册逐条对齐:
//   - schemaVersion 只认 1;未知功能名/未知 mode/矛盾组合一律配置错误,
//     整档拒绝,不忽略后落成全工具(合同 §2.1 铁律);
//   - 依赖解释是校验的一部分(合同 §3):tools.allow 的 mcp:<server>:<tool>
//     要求 components.mcpServers 含 <server> 且 features 放行 mcp;解释
//     不全的档不许采用;
//   - 零工具面(none / only+空 allow)配 exposure=deferred 是配置错误
//     (没有可延迟暴露的东西)。
//
// 本文件是纯数据解析:不读 stdin/stdout、不起进程、不碰盘(读文件入口
// LoadHarnessDeploymentFile 只读一份 JSON)。解析结果交给
// session_assembly.hpp 的装配路——先解析允许组件,再启动。
#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::app_server {

// 功能名表(冻结合同 §4,十六枚)。名单值必须全在表内——"同一功能两枚
// 不同键"在这里拦。
bool IsFrozenFeatureName(std::string_view name);

// ToolPolicySpec(冻结合同 §2)。生产侧只消费新 schema(部署档必带
// mode);旧 AgentToolRules 的换算归 P2 统一解析,不在这里混读。
struct HarnessToolPolicy {
    enum class Mode { Inherit, Only, None };
    Mode mode = Mode::Inherit;
    // canonical 名(mcp:<server>:<tool>;P2 起另收内置 skill 工具的裸名
    // "skill",须 features 放行 skills——见解析器)。mode=Inherit 必空;
    // mode=Only 必填(空数组合法=零工具);mode=None 必空。
    std::vector<std::string> allow;
    std::vector<std::string> deny;
};

// 一份部署档的解析结果(纯数据)。
struct HarnessProfile {
    std::string name;
    std::string agent_ref;  // canonical Agent 引用(记账;解析归 Agent 目录)
    // features 面:default=enabled 时全开、disabled 名单收窄;
    // default=disabled 时只有 enabled 名单开。与 P0 测试册同一条语义。
    bool features_default_enabled = false;
    std::set<std::string> features_enabled;
    std::set<std::string> features_disabled;
    // components.mcpServers:本场点名要挂的 MCP 服务(canonical 名)。
    std::vector<std::string> mcp_servers;
    // components.plugins:本场点名要挂的 Lua/process 插件(canonical 名,
    // 即 manifest.id)。P5 起 v2 embedded-lua 件进入真装载(发现根扫描->
    // 信任账 ->挂载,见 session_assembly 步骤 0);点名 process/native 件
    // 仍按 component_unavailable 整场明拒(单子 §7.2"不支持即拒绝"的
    // 收窄面;错误码冻结见 docs/reference/capability-contract.md §13.4)。
    std::vector<std::string> plugins;
    HarnessToolPolicy tools;
    // exposure.default:direct|deferred|host_only。P1 直连表只走 direct;
    // 值仍解析与校验(零工具配 deferred 在解析层就拒)。
    std::string exposure;
    // limits.stepsPerInput;0 = 档未设(吃配置轴 max_steps_per_turn)。
    int steps_per_input = 0;

    // 功能开关判定(default 与两份名单折算)。
    bool FeatureEnabled(std::string_view feature) const;
    // tools.allow 里引用的 MCP 服务名集合(依赖解释的另一半边)。
    std::set<std::string> ReferencedMcpServers() const;
};

struct HarnessParseResult {
    std::optional<HarnessProfile> profile;  // 空 = 拒绝(error 有人话)
    std::string error;
};

// 解析整份部署文档,取指定档名的档。profile 名须在 harnessProfiles 里;
// service.defaultProfile 只是文档字段,选哪档由调用方定(cli 旗标点名,
// 不给则取 defaultProfile)。
HarnessParseResult ParseHarnessDeployment(const nlohmann::json& deployment,
                                           const std::string& profile_name);

// 便捷入口:取 service.defaultProfile 指的档。
HarnessParseResult ParseHarnessDeploymentDefault(const nlohmann::json& deployment);

// 读文件 + 解析。文件打不开/坏 JSON 同样明败(error 有人话),不静默。
// profile_name 空 = 取 service.defaultProfile 指的档。
HarnessParseResult LoadHarnessDeploymentFile(const std::filesystem::path& path,
                                              std::string profile_name);

}  // namespace lubancode::app_server
