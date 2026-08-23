// 工具注册表:agent 启动时把所有工具注册进去,运行时按名字查找、
// 把全部工具的定义(name/description/input_schema)导出来供拼请求用。
//
// 逐枚追踪单:注册处除 unique_ptr<Tool> 外可带一份 ToolRegistration 元
// 数据(来源/实例/版本摘要)。诊断层(trace、/trace、恢复账)认这份
// 账,不用 dynamic_cast 猜 MCP/Lua/Plugin——日后每加一种 runtime 都不
// 用再漏一次。没带元数据的注册按 builtin 记(旧调用方一行不改)。

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tools/tool.hpp"

namespace lubancode::tools {

// 工具来源。与 agent::ToolSourceKind 一一对应(那边做持久化字符串映射,
// 这边零 agent 依赖)。
enum class ToolSourceKind { Builtin, Mcp, Lsp, PluginLua, PluginNative, Agent, Ptc, Deferred };

// 一枚注册的全部元数据(单子"引入注册元数据"那张表)。
struct ToolRegistration {
    std::unique_ptr<Tool> tool;
    ToolSourceKind source_kind = ToolSourceKind::Builtin;
    std::string source_instance;       // MCP server 名 / plugin id / lsp server;可空
    std::string version_or_digest;     // server 版本 / 文件 digest;可空
    EffectClass effect_class = EffectClass::InProcessUnknown;
    Idempotency idempotency = Idempotency::Unknown;
    RecoveryCapability recovery = RecoveryCapability::None;
};

class ToolRegistry {
public:
    // 旧门:只带工具,按 builtin 记(带 conservative 默认元数据)。
    void Register(std::unique_ptr<Tool> tool);
    // 新门:带完整元数据。tool 为空的注册被丢弃。
    void Register(ToolRegistration registration);

    // 按名字查找,找不到返回 nullptr。所有权还在注册表手里,调用方不用管释放。
    Tool* Find(const std::string& name) const;

    // 注册进来的全部工具,按注册顺序排列。
    const std::vector<std::unique_ptr<Tool>>& All() const { return tools_; }

    // 按名字查注册元数据。没查到(理论上 Find 得到就该查到)给 builtin
    // 默认档——诊断层拿到什么都能画,不许因缺账崩。
    const ToolRegistration* RegistrationOf(const std::string& name) const;

private:
    std::vector<ToolRegistration> registrations_;
    std::vector<std::unique_ptr<Tool>> tools_;
    std::vector<std::string> names_;  // 注册序的名字缓存(表与账同源的对账键)
};

}  // namespace lubancode::tools
