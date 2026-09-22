// 工具来源与副作用等级的共同领域合同(架构审查 AR-08)。原先 tools 与
// agent 各持一份同值域枚举、loop 再写三段逐项同义转换——新增一档就得改
// 两份 enum 加映射,漏一格停在最危险默认档,诊断失真。现下沉到这枚中立
// 头:谁都能 include,不牵 agent/tools 层级(先例:approval_mode.hpp)。
//
// 边界与承诺:
//   - 本头零项目依赖,只引标准库(AR-08 时是纯枚举;尾巴收敛单起把来源
//     字符串编解码也收进来,照 approval_mode.hpp 先例带 inline 函数);
//     不得反过来 include runtime/app(单子验收明令)。
//   - 线上账本是字符串不是数字——来源标签的编解码唯一真源在本头
//     (ToolSourceKindName/ParseToolSourceKindName),agent/tool_trace.cpp
//     的 ToString/Parse 改薄委托,不按 enum 数值迁移历史。
//   - EffectClass 的字符串编解码没有第二份拷贝,仍钉在 agent/
//     tool_trace.cpp,不入本头。
//   - tools:: 与 agent:: 两命名空间短期用 using 别名指向这里,旧消费者
//     (switch/比较/函数签名)一行不改。
//   - PlanToolOrigin(runtime/plan_mode.hpp)原意不同(含 Unknown、缺
//     Deferred),不并入本枚举,投影照旧在装配层显式写。

#pragma once

#include <string_view>

namespace lubancode {

// 工具来源。注册时明写(ToolRegistration.source_kind),诊断层不靠
// RTTI 猜 MCP/Lua/Plugin——每种 runtime 挂上来时自己报账。
enum class ToolSourceKind { Builtin, Mcp, Lsp, PluginLua, PluginNative, Agent, Ptc, Deferred };

// 来源 <-> 稳定字符串的唯一真源(AR-08 尾巴收敛单)。原先 tool_search 与
// prompt audit 各手抄一份同表,漏改一格就静默漂移——现收编到这里,全仓
// 只此一张表。标签即线上格式(tool_trace JSONL 的 source 字段、
// /prompt audit 的 tools 快照、tool_search 命中结果共用),一字不动;
// 新增来源只许在这里加一格,别处不许再抄。
constexpr const char* ToolSourceKindName(ToolSourceKind kind) {
    switch (kind) {
        case ToolSourceKind::Builtin: return "builtin";
        case ToolSourceKind::Mcp: return "mcp";
        case ToolSourceKind::Lsp: return "lsp";
        case ToolSourceKind::PluginLua: return "plugin-lua";
        case ToolSourceKind::PluginNative: return "plugin-native";
        case ToolSourceKind::Agent: return "agent";
        case ToolSourceKind::Ptc: return "ptc";
        case ToolSourceKind::Deferred: return "deferred";
    }
    return "builtin";
}

// 严格解析:认不得给 false;默认档由调用方定,这里不猜。
constexpr bool ParseToolSourceKindName(std::string_view value, ToolSourceKind& out) {
    if (value == "builtin") { out = ToolSourceKind::Builtin; return true; }
    if (value == "mcp") { out = ToolSourceKind::Mcp; return true; }
    if (value == "lsp") { out = ToolSourceKind::Lsp; return true; }
    if (value == "plugin-lua") { out = ToolSourceKind::PluginLua; return true; }
    if (value == "plugin-native") { out = ToolSourceKind::PluginNative; return true; }
    if (value == "agent") { out = ToolSourceKind::Agent; return true; }
    if (value == "ptc") { out = ToolSourceKind::Ptc; return true; }
    if (value == "deferred") { out = ToolSourceKind::Deferred; return true; }
    return false;
}

// 副作用等级(逐枚追踪单"Effect class 与恢复策略")。声明是保守承诺:
// 只影响崩溃后的恢复建议,不越过权限确认;未声明的注册按最危险档
// InProcessUnknown。
enum class EffectClass {
    ReadOnlyLocal,       // read_file/search:可建议重试
    ReadOnlyRemote,      // web_fetch/只读 MCP:不自动重试(费用/限流)
    LocalReversible,     // write_file/edit_file:查 undo token 再询问
    LocalProcessUnknown, // run_command:unknown,先核验
    RemoteIdempotent,    // 带 idempotency key:按 key 查,不直接重发
    RemoteCompensatable, // 支持 delete/cancel:可提补偿(另一枚可见调用)
    RemoteIrreversible,  // 发信/付款/发布:只告警与人工核验
    InProcessUnknown,    // 未声明的 native/Lua:按未知副作用处理
};

}  // namespace lubancode
