// 工具来源与副作用等级的共同领域合同(架构审查 AR-08)。原先 tools 与
// agent 各持一份同值域枚举、loop 再写三段逐项同义转换——新增一档就得改
// 两份 enum 加映射,漏一格停在最危险默认档,诊断失真。现下沉到这枚中立
// 头:谁都能 include,不牵 agent/tools 层级(先例:approval_mode.hpp)。
//
// 边界与承诺:
//   - 本头零依赖(纯枚举,不带函数、不 include 任何东西);不得反过来
//     include runtime/app(单子验收明令)。
//   - 线上账本是字符串不是数字——编解码与落盘标签仍在
//     agent/tool_trace.cpp(ToString/Parse 那套),不按 enum 数值迁移历史。
//   - tools:: 与 agent:: 两命名空间短期用 using 别名指向这里,旧消费者
//     (switch/比较/函数签名)一行不改。
//   - PlanToolOrigin(runtime/plan_mode.hpp)原意不同(含 Unknown、缺
//     Deferred),不并入本枚举,投影照旧在装配层显式写。

#pragma once

namespace lubancode {

// 工具来源。注册时明写(ToolRegistration.source_kind),诊断层不靠
// RTTI 猜 MCP/Lua/Plugin——每种 runtime 挂上来时自己报账。
enum class ToolSourceKind { Builtin, Mcp, Lsp, PluginLua, PluginNative, Agent, Ptc, Deferred };

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
