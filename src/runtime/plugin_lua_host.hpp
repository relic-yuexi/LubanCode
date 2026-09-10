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

#include <atomic>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"     // LuaHook 单 P0-A:中间件 handler 桥
#include "runtime/plugin_http.hpp"  // PluginHttpCallSpec(宿主能力一揽子)
#include "tools/lua_tool.hpp"       // LuaProfile/LuaGuard/Pure 画像与互转件

struct lua_State;

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// LuaCallContext:一次调用的动态作用域(§九第 5/6 步)。LuaHook 单 P0-A 起
// 区分 tool/hook 两类(§二:不能伪造 tool call 来开权限):
//   Kind::Tool —— 原语义,Host API(HTTP/Secret)只在这类作用域开放;
//   Kind::Hook —— hook handler 的调用作用域:宿主只发行只读身份与取消
//     旗,http seam 一律为空;调 luban.http.*/luban.secrets.* 只会拿
//     not_tool_context。hook 自己的能力合同(受控文件/插件状态/工具桥)
//     是 P1-C 的事,届时再挂。
// 缺省构造 = Tool(既有装配与测试零改动);ForHook 造 hook 形状。
//
// 寿命规矩:调用方(阶段 4 的 owner / 中间件工厂 / 本阶段测试)在 Call 外
// 造好,活到 Call 返回;期内指针被 registry 与 guard 同时引用,不搬家。
// ---------------------------------------------------------------------------
struct LuaCallContext {
    enum class Kind { Tool, Hook };

    Kind kind = Kind::Tool;
    PluginHttpCallSpec http;  // Tool 形状的全部宿主 seam(Hook 形状恒空)

    // Hook 形状的只读身份(宿主发行,Lua 只引用;§四:ctx 携带 dispatch/
    // invocation 身份与链位置)。
    struct HookIdentity {
        std::string dispatch_id;
        std::string invocation_id;
        std::string hook_id;     // "Point/name"
        std::string hook_point;  // "PreUser" 等
        std::string stage;       // "mutate"/"estimate"/"capacity"/"default"
        int depth = 0;           // 链上位置
    };
    std::optional<HookIdentity> hook;

    static LuaCallContext ForTool(PluginHttpCallSpec spec) {
        LuaCallContext context;
        context.kind = Kind::Tool;
        context.http = std::move(spec);
        return context;
    }
    static LuaCallContext ForHook(HookIdentity identity) {
        LuaCallContext context;
        context.kind = Kind::Hook;
        context.hook = std::move(identity);
        return context;
    }
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

    // ---- LuaHook 单 P0-A:hook 中间件调用合同 ------------------------------
    // handler(ctx, input, next) 的三参调用。next 是宿主续体:候选(空 =
    // 原样)先过宿主校验,采用后同步跑下游,把下游结果表回给 Lua。合同:
    //   - next 至多一次;第二次调用 lua error(hook.next.already_consumed),
    //     下游执行次数不增加;终态后/跨 invocation 调用 hook.next.expired
    //     (槽位过期;state 若被复用,旧闭包拿到的也只是拒绝,不是悬垂)。
    //   - handler 返回表:{ output = <json>, effects = { {type=..., ...}, ... } };
    //     ctx.deny(code, message) 返回的拒绝表直接 return 即短路拒绝。
    //     nil/标量返回 = 纯输出(无效果)。
    //   - 同 state 嵌套重入(CallHook 在 next 续体里再进同一 state)拒绝
    //     hook.lua.state_reentry(§4.1:嵌套 handler 不重入同一 state)——
    //     不拿互斥递归实现 next,mutex 恒非递归。
    //   - 指令/内存/墙钟预算落 LuaGuard;超限的 error_code 分型见
    //     LuaHookCallResult(§六:预算/失败与关闭)。
    // next 续体的下游结果表:{ status="value|denied|failed", value, code, message }。
    struct LuaHookNextError {
        std::string code;     // 协议违规码(hook.next.*)
        std::string message;
    };
    struct LuaHookDownstream {
        std::string status = "value";  // value/denied/failed
        nlohmann::json value;
        std::string code, message;
    };
    struct LuaHookCall {
        nlohmann::json ctx_meta = nlohmann::json::object();  // 只读身份字段(dispatchId 等)
        nlohmann::json input;                                // 挂点输入候选副本
        std::function<std::expected<LuaHookDownstream, LuaHookNextError>(
            const std::optional<nlohmann::json>& candidate)>
            next;                                  // 宿主续体(可空 = 无下游)
        const std::atomic<bool>* cancel = nullptr;  // 取消旗(灌 guard)
    };
    struct LuaHookCallResult {
        bool ok = false;             // pcall 成功且返回形状可解释
        std::string error_code;      // hook.lua.* / hook.next.* / hook.result.invalid
        std::string message;
        bool next_consumed = false;
        int next_calls = 0;          // next 被调次数(>1 的调用被拒,只计真实调用)
        nlohmann::json output;       // 返回表的 output(nil = 透传下游)
        std::vector<nlohmann::json> effects;  // 原始效果表(宿主再按挂点合同验)
        bool deny = false;
        std::string deny_code, deny_message;
    };
    LuaHookCallResult CallHook(const std::string& entry, const LuaHookCall& call, LuaCallContext& context);

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
    std::mutex call_mutex_;                 // 同 state 串行(§8.5;恒非递归)
    std::map<std::string, int> entry_refs_;  // entry -> registry ref
    std::vector<std::string> entries_;
    // CallHook 重入探针(原子,无锁快查):嵌套重入在摸 mutex 之前就拒——
    // mutex 恒非递归,重入若先锁就成死锁,拒绝才是合同(§4.1)。
    std::atomic<bool> hook_in_flight_{false};
};

// ---------------------------------------------------------------------------
// LuaHook 单 P0-A:Lua 声明 -> hooks::middleware::Handler 的工厂。
// 每次 invocation 独立建 state(§4.1:活动 invocation 用独立 Lua state,
// 脚本内存不跨调用保留;已校验源码在定义里缓存,不缓存业务结果)。预算
// 从定义的 limits 折成 LuaProfile(白名单库 + 指令/内存/墙钟三道墙)。
// 给 MiddlewarePool::Options::lua_factory 用;P0-B 的装配层与测试直接拿。
// ---------------------------------------------------------------------------
std::expected<hooks::middleware::Handler, std::string> MakeLuaHookHandler(
    const hooks::middleware::LuaHandlerSpec& spec, const hooks::middleware::HandlerLimits& limits);

}  // namespace lubancode::runtime
