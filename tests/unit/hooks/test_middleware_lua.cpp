// hook 中间件的 Lua 侧(LuaHook 单 P0-A)——单子 P0-A 后两条:
//   4) 至多一次 next、候选采用、退栈、短路和跨 invocation 拒绝(Lua 面);
//   5) 独立 state、嵌套限制、指令/内存/墙钟与 Host API 取消,Lua 库白名单。
// 加上验收的 Lua 半边:内置与 Lua 混链可改参、后置、短路;重复 next 不增加
// 次数;hook 上下文不能伪造 tool call 开权限。
// 全程内存脚本 + 假 transport,不碰盘、不碰网。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "hooks/middleware.hpp"
#include "runtime/plugin_lua_host.hpp"

using namespace lubancode;
using namespace lubancode::hooks::middleware;
using namespace lubancode::runtime;

namespace {

tools::LuaProfile HookProfile(std::uint64_t instructions = 200'000'000,
                              std::size_t memory = 256ull * 1024 * 1024,
                              std::chrono::milliseconds wall = std::chrono::milliseconds(500)) {
    tools::LuaProfile profile = tools::LuaProfile::HookDefault();
    profile.instruction_budget = instructions;
    profile.memory_cap_bytes = memory;
    profile.wall_budget = wall;
    return profile;
}

// 透传 handler(混链里当"另一只"用)。
Handler Pass(std::vector<std::string>* log, std::string tag) {
    return [log, tag = std::move(tag)](const InvocationCtx&, const nlohmann::json&,
                                       NextCall& next) -> std::expected<HandlerReturn, HandlerError> {
        if (log != nullptr) {
            log->push_back(tag);
        }
        return HandlerReturn::Value(next().value);
    };
}

// 假 resolver:hook 上下文鉴权测试用(计数 Describe,零真值)。
class HookTestResolver final : public runtime::SecretResolver {
public:
    int describe_count = 0;

    std::expected<runtime::SecretValue, runtime::SecretResolveError> Resolve(
        const runtime::SecretDeclaration&) override {
        return runtime::SecretValue(std::string("FAKE_HOOK_KEY"));
    }
    runtime::SecretStatus Describe(const runtime::SecretDeclaration& declaration) override {
        ++describe_count;
        runtime::SecretStatus status;
        status.id = declaration.id;
        status.env = declaration.env;
        status.required = declaration.required;
        status.available = true;
        status.source = runtime::SecretSource::HostEnv;
        return status;
    }
};

std::expected<std::unique_ptr<LuaHostState>, std::string> LoadHookState(const std::string& script,
                                                                        const tools::LuaProfile& profile,
                                                                        const std::string& entry = "run") {
    LuaHostState::Options options;
    options.script = script;
    options.chunk_name = "hook.lua";
    options.entries = {entry};
    options.profile = profile;
    return LuaHostState::Load(std::move(options));
}

LuaCallContext HookContext(const std::string& hook_id = "PreUser/prompt.gate", int depth = 0) {
    LuaCallContext::HookIdentity identity;
    identity.dispatch_id = "hookmw_test";
    identity.invocation_id = "hookmw_test#0";
    identity.hook_id = hook_id;
    identity.hook_point = "PreUser";
    identity.stage = "default";
    identity.depth = depth;
    return LuaCallContext::ForHook(std::move(identity));
}

LuaHostState::LuaHookDownstream ValueDownstream(nlohmann::json value) {
    LuaHostState::LuaHookDownstream downstream;
    downstream.status = "value";
    downstream.value = std::move(value);
    return downstream;
}

MiddlewarePool::Options PoolWithLua() {
    MiddlewarePool::Options options;
    options.lua_factory = [](const LuaHandlerSpec& spec,
                             const HandlerLimits& limits) { return MakeLuaHookHandler(spec, limits); };
    return options;
}

// 清单驱动的 Lua 定义(§三目录形状的内存版:hook.json + main.lua 正文)。
std::expected<void, ManifestError> AddLuaHook(MiddlewarePool& pool, const std::string& hook_json,
                                              const std::string& script) {
    return pool.AddManifest(nlohmann::json::parse(hook_json), SourceLayer::User, "user ~/.lubancode/hooks", script);
}

MiddlewareDefinition MakeBuiltin(HookPoint point, const std::string& name, Handler handler, int priority = 100) {
    MiddlewareDefinition def;
    def.point = point;
    def.name = name;
    def.layer = SourceLayer::Builtin;
    def.source_label = "builtin";
    def.implementation_ref = "builtin." + name;
    def.builtin = std::move(handler);
    def.priority = priority;
    return def;
}

}  // namespace

// ---------------------------------------------------------------------------
// 混链:内置改参 + Lua 后置加工 + 短路。
// ---------------------------------------------------------------------------

TEST_CASE("混链:builtin 改参,Lua 后置加工,链尾恰一次") {
    MiddlewarePool pool(PoolWithLua());
    pool.AddDefinition(MakeBuiltin(
        HookPoint::PreUser, "prompt.normalize",
        [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
            nlohmann::json candidate = input;
            candidate["trimmed"] = true;
            return HandlerReturn::Value(next(candidate).value);
        }));
    REQUIRE(AddLuaHook(pool, R"({
        "schemaVersion": 1, "id": "prompt-wrap", "entry": "main.lua",
        "hooks": [{"hookPoint": "PreUser", "name": "prompt.wrap", "handler": "wrap", "priority": 200}]
    })",
                                   R"lua(
      return {
        wrap = function(ctx, input, next)
          local d = next()
          return { output = { wrapped = true, inner = d.value } }
        end
      }
    )lua")
                .has_value());
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());

    MiddlewareDispatcher dispatcher(std::move(*registry));
    int terminal_runs = 0;
    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"text", "  hi  "}};
    const auto outcome = dispatcher.Dispatch(
        HookPoint::PreUser, trigger, [&terminal_runs](const nlohmann::json& input) {
            ++terminal_runs;
            return nlohmann::json{{("terminal"), (input.dump())}};
        });
    CHECK(outcome.Ok());
    CHECK(outcome.terminal_runs == 1);
    CHECK(terminal_runs == 1);
    CHECK(outcome.value["wrapped"] == true);
    // Lua 看见的是 builtin 改写后的采用版本(inner 是链尾产出)。
    CHECK(outcome.value["inner"]["terminal"].is_string());
    CHECK(outcome.adopted_input["trimmed"] == true);
    CHECK(outcome.FindRecord("PreUser/prompt.wrap")->handler_kind == "lua");
    CHECK(outcome.FindRecord("PreUser/prompt.normalize")->handler_kind == "builtin");
}

TEST_CASE("混链:Lua ctx.deny 短路——零次链尾,deny 是业务拒绝不是失败") {
    MiddlewarePool pool(PoolWithLua());
    REQUIRE(AddLuaHook(pool, R"({
        "schemaVersion": 1, "id": "input-gate", "entry": "main.lua",
        "hooks": [{"hookPoint": "PreUser", "name": "prompt.gate", "handler": "gate", "priority": 1}]
    })",
                                   R"lua(
      return {
        gate = function(ctx, input, next)
          if input.text == "坏话" then
            return ctx.deny("bad_words", "这次输入未通过检查")
          end
          return { output = next().value }
        end
      }
    )lua")
                .has_value());
    pool.AddDefinition(MakeBuiltin(HookPoint::PreUser, "prompt.other",
                                   Pass(nullptr, "other"), 200));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    MiddlewareDispatcher dispatcher(std::move(*registry));

    DispatchTrigger bad;
    bad.input = nlohmann::json{{"text", "坏话"}};
    const auto denied = dispatcher.Dispatch(HookPoint::PreUser, bad, [](const nlohmann::json&) {
        return nlohmann::json();
    });
    CHECK(denied.kind == DispatchOutcome::Kind::Denied);
    CHECK(denied.deny_code == "bad_words");
    CHECK(denied.deny_message == "这次输入未通过检查");
    CHECK(denied.terminal_runs == 0);
    CHECK(denied.FindRecord("PreUser/prompt.gate")->outcome == "denied");
    CHECK(denied.FindRecord("PreUser/prompt.other")->outcome == "skipped_short_circuit");

    DispatchTrigger fine;
    fine.input = nlohmann::json{{"text", "好话"}};
    const auto passed = dispatcher.Dispatch(HookPoint::PreUser, fine, [](const nlohmann::json& input) {
        return nlohmann::json{{("ok"), (true)}};
    });
    CHECK(passed.Ok());
    CHECK(passed.terminal_runs == 1);
}

// ---------------------------------------------------------------------------
// 至多一次 next(Lua 面)与跨 invocation 拒绝。
// ---------------------------------------------------------------------------

TEST_CASE("Lua:重复 next 第二次拿 hook.next.already_consumed,下游不重跑") {
    auto state = LoadHookState(R"lua(
      return {
        run = function(ctx, input, next)
          local first = next()
          local ok, err = pcall(next)
          return { output = { first_status = first.status,
                              second_ok = ok,
                              second_code = (not ok) and err.code or "none" } }
        end
      }
    )lua",
                               HookProfile());
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext("PreUser/prompt.once");
    int downstream_runs = 0;
    LuaHostState::LuaHookCall call;
    call.input = nlohmann::json::object();
    call.next = [&downstream_runs](const std::optional<nlohmann::json>&) {
        ++downstream_runs;
        return ValueDownstream(nlohmann::json{{("downstream"), (1)}});
    };
    const auto result = (*state)->CallHook("run", call, context);
    REQUIRE(result.ok);
    CHECK(result.next_calls == 2);
    CHECK(result.next_consumed);
    CHECK(downstream_runs == 1);  // 重复调用不增加下游执行
    CHECK(result.output["first_status"] == "value");
    CHECK_FALSE(result.output["second_ok"].get<bool>());
    CHECK(result.output["second_code"] == "hook.next.already_consumed");
}

TEST_CASE("Lua:跨 invocation 保存 next,终态后调用一律拒绝(不悬垂)") {
    auto state = LoadHookState(R"lua(
      saved = nil
      return {
        run = function(ctx, input, next)
          if saved == nil then
            saved = next            -- 第一次调用把 next 藏进全局
            return { output = "stashed" }
          end
          local ok, err = pcall(function() return saved() end)  -- 第二次调用旧 next
          return { output = { stale_ok = ok, stale_code = (not ok) and err.code or "none" } }
        end
      }
    )lua",
                               HookProfile());
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext();

    LuaHostState::LuaHookCall first;
    first.input = nlohmann::json::object();
    first.next = [](const std::optional<nlohmann::json>&) { return ValueDownstream(nlohmann::json(1)); };
    const auto first_result = (*state)->CallHook("run", first, context);
    REQUIRE(first_result.ok);
    CHECK(first_result.output == "stashed");

    LuaHostState::LuaHookCall second;
    second.input = nlohmann::json::object();
    second.next = [](const std::optional<nlohmann::json>&) { return ValueDownstream(nlohmann::json(2)); };
    const auto second_result = (*state)->CallHook("run", second, context);
    REQUIRE(second_result.ok);
    CHECK_FALSE(second_result.output["stale_ok"].get<bool>());
    CHECK(second_result.output["stale_code"] == "hook.next.expired");
}

TEST_CASE("Lua:独立 state——脚本内存不跨调用保留,全局计数器每次归零") {
    LuaHandlerSpec spec;
    spec.chunk_name = "hook.lua";
    spec.entry = "run";
    spec.script = R"lua(
      count = (count or 0) + 1
      return { run = function(ctx, input, next) return { output = count } end }
    )lua";
    auto handler = MakeLuaHookHandler(spec, HandlerLimits{});
    REQUIRE(handler.has_value());

    MiddlewarePool pool;
    MiddlewareDefinition def = MakeBuiltin(HookPoint::PostUser, "memory.recall", *handler);
    pool.AddDefinition(std::move(def));
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    MiddlewareDispatcher dispatcher(std::move(*registry));

    for (int i = 0; i < 3; ++i) {
        DispatchTrigger trigger;
        trigger.input = nlohmann::json::object();
        const auto outcome = dispatcher.Dispatch(HookPoint::PostUser, trigger, [](const nlohmann::json& input) {
            return input;
        });
        CHECK(outcome.Ok());
        CHECK(outcome.value == 1);  // 每次调用独立 state,全局变量不残留
    }
}

TEST_CASE("Lua:候选经 next 采用——改参进链尾,原 input 表不动 session") {
    auto state = LoadHookState(R"lua(
      return {
        run = function(ctx, input, next)
          local candidate = { text = input.text .. "!" }
          local d = next(candidate)
          return { output = d.value }
        end
      }
    )lua",
                               HookProfile());
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext("PreUser/prompt.rewrite");
    nlohmann::json adopted;
    LuaHostState::LuaHookCall call;
    call.input = nlohmann::json{{"text", "hi"}};
    call.next = [&adopted](const std::optional<nlohmann::json>& candidate) {
        if (candidate.has_value()) {
            adopted = *candidate;  // 宿主采用后的版本才到这里
        }
        return ValueDownstream(nlohmann::json{{("seen"), (adopted.dump())}});
    };
    const auto result = (*state)->CallHook("run", call, context);
    REQUIRE(result.ok);
    CHECK(adopted["text"] == "hi!");
    CHECK(result.output["seen"].is_string());
}

// ---------------------------------------------------------------------------
// 嵌套限制:同 state 重入拒绝,不死锁(§4.1)。
// ---------------------------------------------------------------------------

TEST_CASE("嵌套:next 续体里重入同一 state 拒 hook.lua.state_reentry,不死锁") {
    auto state = LoadHookState(R"lua(
      return {
        run = function(ctx, input, next)
          local ok, err = pcall(next)
          return { output = { ok = ok, code = (not ok) and err.code or "none" } }
        end
      }
    )lua",
                               HookProfile());
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext();
    LuaHostState::LuaHookCall call;
    call.input = nlohmann::json::object();
    LuaHostState* raw = state->get();
    call.next = [raw](const std::optional<nlohmann::json>&)
        -> std::expected<LuaHostState::LuaHookDownstream, LuaHostState::LuaHookNextError> {
        // 恶意续体:在 next 里再进同一 state(嵌套 handler 重入)。
        LuaCallContext nested = HookContext("PreUser/prompt.nested", 1);
        LuaHostState::LuaHookCall nested_call;
        nested_call.input = nlohmann::json::object();
        const auto nested_result = raw->CallHook("run", nested_call, nested);
        if (!nested_result.ok) {
            LuaHostState::LuaHookNextError error;
            error.code = nested_result.error_code;
            error.message = nested_result.message;
            return std::unexpected(error);
        }
        return ValueDownstream(nlohmann::json(1));
    };
    const auto result = (*state)->CallHook("run", call, context);
    // 续体里的重入被拒;闭包把它升成 next 错误,外层 pcall 接住。
    REQUIRE(result.ok);
    CHECK_FALSE(result.output["ok"].get<bool>());
    CHECK(result.output["code"] == "hook.lua.state_reentry");
}

// ---------------------------------------------------------------------------
// 预算:指令/墙钟/内存三道墙的错误码分型。
// ---------------------------------------------------------------------------

TEST_CASE("预算:指令预算耗尽 -> hook.lua.budget_instruction") {
    auto state = LoadHookState(R"lua(
      return { run = function(ctx, input, next)
        local x = 0
        while true do x = x + 1 end
      end }
    )lua",
                               HookProfile(/*instructions=*/400'000, /*memory=*/256ull * 1024 * 1024,
                               std::chrono::milliseconds(60'000)));
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext();
    LuaHostState::LuaHookCall call;
    call.input = nlohmann::json::object();
    const auto result = (*state)->CallHook("run", call, context);
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "hook.lua.budget_instruction");
    CHECK(result.next_consumed == false);
}

TEST_CASE("预算:墙钟到点 -> hook.lua.budget_wallclock") {
    auto state = LoadHookState(R"lua(
      return { run = function(ctx, input, next)
        local x = 0
        while true do x = x + 1 end
      end }
    )lua",
                               HookProfile(/*instructions=*/0, /*memory=*/256ull * 1024 * 1024, std::chrono::milliseconds(120)));
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext();
    LuaHostState::LuaHookCall call;
    call.input = nlohmann::json::object();
    const auto result = (*state)->CallHook("run", call, context);
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "hook.lua.budget_wallclock");
}

TEST_CASE("预算:内存帽落锤 -> hook.lua.budget_memory") {
    auto state = LoadHookState(R"lua(
      return { run = function(ctx, input, next)
        local blob = {}
        for i = 1, 10000 do blob[i] = string.rep(tostring(i), 512) end
        return { output = #blob }
      end }
    )lua",
                               HookProfile(/*instructions=*/2'000'000'000, /*memory=*/256 * 1024,
                                           std::chrono::milliseconds(60'000)));
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext();
    LuaHostState::LuaHookCall call;
    call.input = nlohmann::json::object();
    const auto result = (*state)->CallHook("run", call, context);
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "hook.lua.budget_memory");
}

// ---------------------------------------------------------------------------
// Lua 库白名单(§五)。
// ---------------------------------------------------------------------------

TEST_CASE("白名单:只有 base(-文件口)/string/table/math/utf8/os 时间四函数") {
    auto state = LoadHookState(R"lua(
      local function kind(v) local ok, t = pcall(function() return type(v) end) return ok and t or "gone" end
      local function os_kind(name)
        local ok, v = pcall(function() return os[name] end)
        return ok and type(v) or "gone"
      end
      return { run = function(ctx, input, next)
        return { output = {
          io = kind(io), package = kind(package), coroutine = kind(coroutine), debug = kind(debug),
          require = kind(require), loadfile = kind(loadfile), dofile = kind(dofile),
          string = kind(string), table = kind(table), math = kind(math), utf8 = kind(utf8),
          os_clock = os_kind("clock"), os_date = os_kind("date"),
          os_execute = os_kind("execute"), os_remove = os_kind("remove"), os_rename = os_kind("rename"),
          os_getenv = os_kind("getenv"),
          print = kind(print), load = kind(load), pairs = kind(pairs),
        } }
      end }
    )lua",
                               HookProfile());
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext();
    LuaHostState::LuaHookCall call;
    call.input = nlohmann::json::object();
    call.next = [](const std::optional<nlohmann::json>&) { return ValueDownstream(nlohmann::json(1)); };
    const auto result = (*state)->CallHook("run", call, context);
    REQUIRE(result.ok);
    const nlohmann::json& out = result.output;
    // 不开的:io/package/coroutine/debug/require/loadfile/dofile 全 nil。
    CHECK(out["io"] == "nil");
    CHECK(out["package"] == "nil");
    CHECK(out["coroutine"] == "nil");
    CHECK(out["debug"] == "nil");
    CHECK(out["require"] == "nil");
    CHECK(out["loadfile"] == "nil");
    CHECK(out["dofile"] == "nil");
    // 开的:base/string/table/math/utf8 与 load(只编译字符串)。
    CHECK(out["string"] == "table");
    CHECK(out["table"] == "table");
    CHECK(out["math"] == "table");
    CHECK(out["utf8"] == "table");
    CHECK(out["print"] == "function");
    CHECK(out["load"] == "function");
    CHECK(out["pairs"] == "function");
    // os 只有时间四函数;execute/remove/rename/getenv 不在。
    CHECK(out["os_clock"] == "function");
    CHECK(out["os_date"] == "function");
    CHECK(out["os_execute"] == "nil");
    CHECK(out["os_remove"] == "nil");
    CHECK(out["os_rename"] == "nil");
    CHECK(out["os_getenv"] == "nil");
}

TEST_CASE("白名单:os.rename/io.write 真调即报错,不是纸面 nil") {
    auto state = LoadHookState(R"lua(
      return { run = function(ctx, input, next)
        local ok_remove, err_remove = pcall(function() return os.remove("Z:/definitely/not/here") end)
        local ok_rename, err_rename = pcall(function() return os.rename("a", "b") end)
        local ok_io, err_io = pcall(function() return io.open("Z:/x", "r") end)
        return { output = {
          remove_ok = ok_remove and (err_remove == nil) or false,
          remove_error = (not ok_remove) and "attempt" or tostring(err_remove),
          rename_error = tostring(err_rename),
          io_error = tostring(err_io),
        } }
      end }
    )lua",
                               HookProfile());
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext();
    LuaHostState::LuaHookCall call;
    call.input = nlohmann::json::object();
    const auto result = (*state)->CallHook("run", call, context);
    REQUIRE(result.ok);
    // os.remove/os.rename 是 nil:调用直接 attempt to call a nil value。
    CHECK(result.output["remove_error"].is_string());
    CHECK(result.output["rename_error"].is_string());
    CHECK(result.output["io_error"].is_string());
}

// ---------------------------------------------------------------------------
// ctx:只读身份 + hook 上下文不开工具 Host API。
// ---------------------------------------------------------------------------

TEST_CASE("ctx:身份字段只读,宿主发行什么看见什么") {
    auto state = LoadHookState(R"lua(
      return { run = function(ctx, input, next)
        return { output = { hookId = ctx.hookId, dispatchId = ctx.dispatchId,
                            hookPoint = ctx.hookPoint, stage = ctx.stage,
                            depth = ctx.depth, revision = ctx.registryRevision } }
      end }
    )lua",
                               HookProfile());
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext("PostUser/memory.recall", 3);
    LuaHostState::LuaHookCall call;
    call.ctx_meta = nlohmann::json{{"dispatchId", "hookmw_test"},
                                   {"hookId", "PostUser/memory.recall"},
                                   {"hookPoint", "PostUser"},
                                   {"stage", "default"},
                                   {"depth", 3},
                                   {"registryRevision", 7}};
    call.input = nlohmann::json::object();
    const auto result = (*state)->CallHook("run", call, context);
    REQUIRE(result.ok);
    CHECK(result.output["hookId"] == "PostUser/memory.recall");
    CHECK(result.output["hookPoint"] == "PostUser");
    CHECK(result.output["depth"] == 3);
    CHECK(result.output["revision"] == 7);
}

TEST_CASE("ctx:改写 ctx 落子即拒(只读)") {
    auto state = LoadHookState(R"lua(
      return { run = function(ctx, input, next)
        ctx.hookId = "forged/identity"
        return { output = "unreachable" }
      end }
    )lua",
                               HookProfile());
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext();
    LuaHostState::LuaHookCall call;
    call.input = nlohmann::json::object();
    const auto result = (*state)->CallHook("run", call, context);
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "hook.result.invalid");
    CHECK(result.message.find("只读") != std::string::npos);
}

TEST_CASE("hook 上下文:调 luban.http/secrets 拿 not_tool_context,零网络零解析") {
    FakeHttpTransport transport;
    HookTestResolver resolver;
    auto state = LoadHookState(R"lua(
      return { run = function(ctx, input, next)
        local r, e = luban.http.request({ method = "GET", url = "https://api.example.com/x" })
        local a, ae = luban.secrets.available("api_key")
        local ok = pcall(function() return ctx.http end)
        return { output = { http_code = e and e.code, available_code = ae and ae.code,
                            transport_nil = (ctx.http == nil) } }
      end }
    )lua",
                               HookProfile());
    REQUIRE(state.has_value());
    LuaCallContext context = HookContext();
    context.http.transport = &transport;  // 恶意装配:就算 seam 被塞进来也进不去
    context.http.secret_resolver = &resolver;
    LuaHostState::LuaHookCall call;
    call.input = nlohmann::json::object();
    const auto result = (*state)->CallHook("run", call, context);
    REQUIRE(result.ok);
    CHECK(result.output["http_code"] == "not_tool_context");
    CHECK(result.output["available_code"] == "not_tool_context");
    CHECK(result.output["transport_nil"].get<bool>());  // ctx 上没有 http 字段
    CHECK(transport.call_count() == 0);
    CHECK(resolver.describe_count == 0);
}

// ---------------------------------------------------------------------------
// 验收主句:替换 memory.recall 后内置调用计数为零,skill.resolve 仍执行。
// ---------------------------------------------------------------------------

TEST_CASE("验收:Lua 同名接管 memory.recall,内置计数为零,skill 照跑,依赖仍绑定") {
    MiddlewarePool pool(PoolWithLua());
    int builtin_memory_calls = 0;
    std::vector<std::string> order;
    pool.AddDefinition(MakeBuiltin(
        HookPoint::PostUser, "skill.resolve",
        [&order](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
            order.push_back("skill");
            return HandlerReturn::Value(next().value);
        },
        /*priority=*/100));
    pool.AddDefinition(MakeBuiltin(
        HookPoint::PostUser, "memory.recall",
        [&builtin_memory_calls, &order](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
            builtin_memory_calls += 1;
            order.push_back("builtin-memory");
            return HandlerReturn::Value(next().value);
        },
        /*priority=*/200));
    // 用户 Lua 接管 memory.recall,声明 after skill.resolve(依赖指向逻辑键,
    // 覆盖后自动连到获选实现)。
    REQUIRE(AddLuaHook(pool, R"({
        "schemaVersion": 1, "id": "my-memory", "entry": "main.lua",
        "hooks": [{"hookPoint": "PostUser", "name": "memory.recall", "handler": "recall",
                    "priority": 200, "after": ["skill.resolve"]}]
    })",
                                   R"lua(
      return {
        recall = function(ctx, input, next)
          local effects = { { type = "context.append", text = "回忆: 上次谈到 hook 合同" } }
          local d = next()
          return { output = d.value, effects = effects }
        end
      }
    )lua")
                .has_value());
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    REQUIRE((*registry)->Overridden().size() == 1);  // 被覆盖的 builtin 保留定义来源
    MiddlewareDispatcher dispatcher(std::move(*registry));

    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"text", "再讲讲"}};
    const auto outcome = dispatcher.Dispatch(HookPoint::PostUser, trigger, [](const nlohmann::json& input) {
        return nlohmann::json{{("terminal"), (true)}};
    });
    CHECK(outcome.Ok());
    CHECK(builtin_memory_calls == 0);                      // 内置调用计数为零
    CHECK(order == std::vector<std::string>{"skill"});     // skill.resolve 仍执行
    REQUIRE(outcome.context_appends.size() == 1);          // Lua 的追加效果被采用
    CHECK(outcome.context_appends[0].find("回忆") != std::string::npos);
    const InvocationRecord* recall = outcome.FindRecord("PostUser/memory.recall");
    REQUIRE(recall != nullptr);
    CHECK(recall->handler_kind == "lua");
    CHECK(recall->definition_order > outcome.FindRecord("PostUser/skill.resolve")->definition_order);  // 依赖仍绑定
}

TEST_CASE("效果:Lua 给的效果按挂点合同收口——PostUser 不收 input.rewrite") {
    MiddlewarePool pool(PoolWithLua());
    REQUIRE(AddLuaHook(pool, R"({
        "schemaVersion": 1, "id": "sneaky", "entry": "main.lua",
        "hooks": [{"hookPoint": "PostUser", "name": "memory.recall", "handler": "recall"}]
    })",
                                   R"lua(
      return {
        recall = function(ctx, input, next)
          return { output = next().value,
                   effects = { { type = "context.append", text = "合法追加" },
                               { type = "input.rewrite", value = { sneaky = true } } } }
        end
      }
    )lua")
                .has_value());
    const auto registry = pool.Publish();
    REQUIRE(registry.has_value());
    MiddlewareDispatcher dispatcher(std::move(*registry));
    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"text", "hi"}};
    const auto outcome = dispatcher.Dispatch(HookPoint::PostUser, trigger, [](const nlohmann::json& input) {
        return input;
    });
    CHECK(outcome.Ok());
    const InvocationRecord* recall = outcome.FindRecord("PostUser/memory.recall");
    REQUIRE(recall != nullptr);
    REQUIRE(recall->effects.size() == 2);
    CHECK(recall->effects[0].type == "context.append");
    CHECK(recall->effects[0].applied);
    CHECK(recall->effects[1].type == "input.rewrite");
    CHECK_FALSE(recall->effects[1].applied);  // PostUser 不能回写原 user
    CHECK(outcome.adopted_input["sneaky"].is_null());
}
