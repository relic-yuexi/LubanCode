// Lua Host API 与动态调用上下文(Lua 受控 HTTP 与 Secret 宿主能力单·阶段 3)。
//
// 这一层把阶段 0-2 冻结的合同接进 Lua state:
//   - luban.http.request / luban.secrets.available / luban.secrets.ref 三枚
//     Host 函数(§六):Lua 只描述请求形状,分派全走宿主;工具定义仍只认
//     manifest,Lua 不抄第二份 schema。
//   - LuaCallContext(§九):一次工具 execute 的动态作用域。加载/顶层执行
//     期 context 为空——此时调 HTTP/Secret 一律 no_active_tool_call,零网络、
//     零 Secret 解析。RAII(ScopedLuaCallContext)把 context 写进 registry,
//     lua_pcall 返回/异常/取消路径都清空。
//   - SecretRef opaque userdata(§6.3):tostring 只得 <secret:id>;拼接/索引/
//     转 JSON 全被元方法锁死;唯一 sink 是 luban.http.request 的 auth.secret。
//   - 取消接线(§8.4):ToolExecutionContext.cancel -> LuaCallContext ->
//     transport;instruction hook 与 HTTP 回调共用同一枚旗。
//   - LuaHostState:manifest-backed Lua 插件的机制件——建 state(三道墙与
//     tools::LuaTool 同款)、注入 luban 模块、顶层零副作用加载、handler 表
//     与 manifest entry 对账、按 entry 的动态作用域调用。阶段 4 的
//     ManifestLuaRuntime owner 持它接 manifest/挂载;裸 .lua 不经这里
//     (§一断语 1:裸 Lua 不开 Host API)。
//
// 头文件不 include lua 头(与 tools/lua_tool.hpp 同一条规矩),lua_State
// 只前向声明;宿主侧实现见 plugin_lua_host.cpp。
#pragma once

#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/plugin_http.hpp"  // PluginHttpCallSpec(宿主能力一揽子)
#include "tools/lua_tool.hpp"       // LuaProfile/LuaGuard/Pure 画像与互转件

struct lua_State;

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// LuaCallContext:一次工具调用的动态作用域(§九第 5/6 步)。宿主能力全挂
// 在这里:HTTP/Secret 的 seam、生效帽、取消旗。离开作用域即失效——Lua 想
// 在调用外(顶层加载、挂起的 coroutine 事后恢复)用 Host API,拿到的只
// 会是 no_active_tool_call。
//
// 寿命规矩:调用方(阶段 4 的 owner / 本阶段测试)在 Call 外造好,活到
// Call 返回;期内指针被 registry 与 guard 同时引用,不搬家。
// ---------------------------------------------------------------------------
struct LuaCallContext {
    PluginHttpCallSpec http;  // 全部宿主 seam:secrets 账/resolver/transport/limits/cancel
};

// 往 state 注册 luban 模块(luban.http.request、luban.secrets.available/ref)。
// 在建 state、开 Pure 库之后、执行 chunk 之前调(§九第 1 步)。可重复调
// 用(整表重造,旧的被覆盖)。不绑任何 context——函数体运行期才从
// registry 取,取不到就是 no_active_tool_call。
void RegisterLuaHostModule(lua_State* L);

// 当前动态调用上下文;顶层加载期/调用外为 nullptr。机制件的观察口
// (loader/owner 与测试用;Lua 侧看不见它)。
LuaCallContext* CurrentLuaCallContext(lua_State* L);

// §九第 5/6 步的 RAII:构造时把 context 写进 registry,析构时清空(nil)。
// lua_pcall 返回、Lua error(longjmp 走 pcall 接住)、取消、宿主异常展开,
// 都经析构清空——不留第二次调用的旧指针。
class ScopedLuaCallContext {
public:
    ScopedLuaCallContext(lua_State* L, LuaCallContext* context);
    ~ScopedLuaCallContext();

    ScopedLuaCallContext(const ScopedLuaCallContext&) = delete;
    ScopedLuaCallContext& operator=(const ScopedLuaCallContext&) = delete;

private:
    lua_State* lua_;
};

// ---------------------------------------------------------------------------
// LuaHostState:manifest-backed Lua 插件的 state 与调用机制(阶段 3)。
//
// 加载(§九六步):建带三道墙的 state -> 开 Pure 库 -> 注册 luban 模块 ->
// 顶层零副作用执行 chunk(context 为空)-> 验返回表是 table -> 逐枚
// manifest entry 对账 handler(缺 handler/非 function/entry 重复,整件拒挂,
// state 关闭)。多出的未声明 function 留着,不挂成工具(§6.1)。
//
// 调用:同 state 由 per-state mutex 串行(§8.5),不同插件可并行;每次
// Call 重置指令账、灌取消旗(instruction hook 与 HTTP 回调同一枚)、RAII
// 绑 context;返回值字符串化规矩与 tools::LuaTool 一致(字符串原样、数字/
// 布尔转文本、表转 JSON、nil 算错)。
// ---------------------------------------------------------------------------
class LuaHostState {
public:
    // 加载材料。
    struct Options {
        std::string script;                                    // Lua 源码(内存,不走盘)
        std::string chunk_name = "plugin";                     // 报错与 trace 用
        std::vector<std::string> entries;                      // manifest tools[].entry 全表
        tools::LuaProfile profile = tools::LuaProfile::PureDefault();  // 缺省 Pure
    };

    // 加载并验 handler。失败(编译/顶层执行/返回非表/handler 对账不过/
    // entry 重复)返回人话,不带半个 state。
    static std::expected<std::unique_ptr<LuaHostState>, std::string> Load(Options options);

    ~LuaHostState();
    LuaHostState(const LuaHostState&) = delete;
    LuaHostState& operator=(const LuaHostState&) = delete;

    // 一次调用的终态(与 tools::LuaTool 的 Result 字段对齐,阶段 4 的
    // adapter 折成 Tool::Result 不用再猜)。
    struct CallResult {
        std::string content;
        bool is_error = false;
        std::string outcome;     // "plugin_exception"(Lua error 路)
        std::string error_code;  // "plugin.lua_error" 等
    };

    // 调一枚 entry:input(JSON)转 lua 表;context 活到 Call 返回。返回后
    // context 清空(第二次调用不见上次的 Secret/取消旗)。
    CallResult Call(const std::string& entry, const nlohmann::json& input, LuaCallContext& context);

    // owner/测试的观察口:state 与对过账的 entry 表。阶段 4 的
    // ManifestLuaRuntime 往 state 里再塞东西/做诊断时用;别处别拿去直跑
    // Lua——动态作用域外的调用一律该走 Call。
    lua_State* lua() const { return lua_; }
    const std::vector<std::string>& entries() const { return entries_; }

private:
    LuaHostState() = default;

    lua_State* lua_ = nullptr;
    std::unique_ptr<tools::LuaGuard> guard_;
    tools::LuaProfile profile_;
    std::mutex call_mutex_;                 // 同 state 串行(§8.5)
    std::map<std::string, int> entry_refs_;  // entry -> registry ref
    std::vector<std::string> entries_;
};

}  // namespace lubancode::runtime
