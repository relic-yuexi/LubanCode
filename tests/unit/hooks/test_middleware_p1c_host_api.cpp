// LuaHook 单 P1-C 第 1/2 条:hook 的宿主能力面。
//   1) HTTP/Secret 以 hook 身份接入:自己的权限与预算(FakeHttpTransport 计
//      数),没授的口 capability_not_granted,tool 作用域照旧 not_hook_
//      context;
//   2) 受控文件(授权根/越界/字节帽/原子写/列目)、插件状态(按包隔离/
//      限额)、context 候选(折成效果)、结构化日志(截断)。
// 权限交集(§五):清单申请 ∩ 宿主授权,两侧各缺一头都不开。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"
#include "runtime/hook_host_services.hpp"
#include "runtime/plugin_lua_host.hpp"

using namespace lubancode;
using namespace lubancode::hooks::middleware;
using namespace lubancode::runtime;

namespace {

// ---- 装配小件 ---------------------------------------------------------------

MiddlewarePool::Options PoolWithCenter(HookHostServiceCenter& center) {
    MiddlewarePool::Options options;
    HookHostServiceCenter* center_ptr = &center;
    options.lua_factory = [center_ptr](const LuaHandlerSpec& spec, const HandlerLimits& limits) {
        return MakeLuaHookHandler(spec, limits, center_ptr);
    };
    return options;
}

// 清单驱动的 Lua 定义:capabilities 进清单,授权走中心 grants。
std::expected<void, ManifestError> AddLuaHook(MiddlewarePool& pool, const std::string& hook_json,
                                              const std::string& script) {
    return pool.AddManifest(nlohmann::json::parse(hook_json), SourceLayer::User,
                            "user ~/.lubancode/hooks", script);
}

nlohmann::json DispatchValue(HookPoint point, MiddlewarePool& pool,
                             const nlohmann::json& trigger_input = {{"prompt", "原文"}}) {
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    MiddlewareDispatcher dispatcher(std::move(*published));
    DispatchTrigger trigger;
    trigger.input = trigger_input;
    return dispatcher.Dispatch(point, trigger, [](const nlohmann::json& input) { return input; }).value;
}

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* tag) {
        path = std::filesystem::temp_directory_path() / ("lubancode-p1c-host-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// 假 resolver:计数 Describe,零真值(与 test_middleware_lua.cpp 同款)。
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

}  // namespace

// ---------------------------------------------------------------------------
// HTTP/Secret 以 hook 身份接入(第 1 条)
// ---------------------------------------------------------------------------

TEST_CASE("HTTP:hook 带自己的权限与预算真发一笔;没授的口 capability_not_granted") {
    FakeHttpTransport transport;
    transport.EnqueueResponse(HttpExchangeResponse{200, {}, "外部资料正文", "https://kb.example/v1"});

    HookHostServiceCenter::Grants grants;
    PluginHttpCallSpec http;
    http.transport = &transport;
    http.limits = EffectiveHttpLimits{};
    grants.http = http;
    HookHostServiceCenter center(std::move(grants));

    MiddlewarePool pool(PoolWithCenter(center));
    // 申请了 http:真发一笔,响应回 Lua。
    REQUIRE(AddLuaHook(pool,
                       R"({"schemaVersion":1,"id":"kb-recall","entry":"main.lua","hooks":[
                            {"hookPoint":"PostUser","name":"kb.recall","handler":"run",
                             "capabilities":["http"]}]})",
                       R"lua(return {
        run = function(ctx, input, next)
          local r, err = luban.http.request{method="GET", url="https://kb.example/v1"}
          if r == nil then return { output = { failed = true, code = err.code } } end
          return { output = { status = r.status, body = r.body } }
        end
      })lua")
                  .has_value());
    const nlohmann::json ok_value = DispatchValue(HookPoint::PostUser, pool);
    CHECK(ok_value.at("status") == 200);
    CHECK(ok_value.at("body") == "外部资料正文");
    REQUIRE(transport.call_count() == 1);

    // 没申请 http(清单不含):capability_not_granted,零网络。
    MiddlewarePool bare_pool(PoolWithCenter(center));
    REQUIRE(AddLuaHook(bare_pool,
                       R"({"schemaVersion":1,"id":"kb-bare","entry":"main.lua","hooks":[
                            {"hookPoint":"PostUser","name":"kb.bare","handler":"run"}]})",
                       R"lua(return {
        run = function(ctx, input, next)
          local r, err = luban.http.request{method="GET", url="https://kb.example/v1"}
          return { output = { sent = r ~= nil, code = err and err.code } }
        end
      })lua")
                  .has_value());
    const nlohmann::json bare_value = DispatchValue(HookPoint::PostUser, bare_pool);
    CHECK(bare_value.at("sent") == false);
    CHECK(bare_value.at("code") == "hook.capability_not_granted");
    CHECK(transport.call_count() == 1);  // 还是那一笔,没多

    // 宿主侧没授(grants 无 http):同样不开——交集两头都查。
    HookHostServiceCenter bare_center;
    MiddlewarePool host_bare_pool(PoolWithCenter(bare_center));
    REQUIRE(AddLuaHook(host_bare_pool,
                       R"({"schemaVersion":1,"id":"kb-hb","entry":"main.lua","hooks":[
                            {"hookPoint":"PostUser","name":"kb.hb","handler":"run",
                             "capabilities":["http"]}]})",
                       R"lua(return {
        run = function(ctx, input, next)
          local r, err = luban.http.request{method="GET", url="https://kb.example/v1"}
          return { output = { sent = r ~= nil, code = err and err.code } }
        end
      })lua")
                  .has_value());
    const nlohmann::json host_bare_value = DispatchValue(HookPoint::PostUser, host_bare_pool);
    CHECK(host_bare_value.at("code") == "hook.capability_not_granted");
}

TEST_CASE("Secret:hook 作用域按自己的 http 授权开 available/ref;无授权零解析") {
    HookTestResolver resolver;  // test_middleware_lua.cpp 同款假 resolver
    HookHostServiceCenter::Grants grants;
    PluginHttpCallSpec http;
    SecretDeclaration declaration;
    declaration.id = "kb_token";
    http.secrets = {declaration};
    http.secret_resolver = &resolver;
    http.transport = nullptr;  // 本例不发请求
    grants.http = http;
    HookHostServiceCenter center(std::move(grants));

    MiddlewarePool pool(PoolWithCenter(center));
    REQUIRE(AddLuaHook(pool,
                       R"({"schemaVersion":1,"id":"sec-probe","entry":"main.lua","hooks":[
                            {"hookPoint":"PostUser","name":"sec.probe","handler":"run",
                             "capabilities":["http"]}]})",
                       R"lua(return {
        run = function(ctx, input, next)
          local ok = luban.secrets.available("kb_token")
          local ref = luban.secrets.ref("kb_token")
          return { output = { available = ok, ref = tostring(ref) } }
        end
      })lua")
                  .has_value());
    const nlohmann::json value = DispatchValue(HookPoint::PostUser, pool);
    CHECK(value.at("available") == true);
    CHECK(value.at("ref") == "<secret:kb_token>");  // opaque:只见 id,永不见值
    CHECK(resolver.describe_count == 1);
}

TEST_CASE("HTTP:hook 的取消旗贯通 transport(Esc 同一根真值)") {
    // 直接走 CallHook(执行核在帧边界就拦预置取消旗,轮内取消的语义归
    // wire 册;这里钉的是 Lua → HTTP seam 的旗贯通)。
    FakeHttpTransport transport;
    transport.EnqueueResponse(HttpExchangeResponse{200, {}, "正文", "https://kb.example/v1"});

    LuaHookServices services;
    services.http_granted = true;
    services.http.transport = &transport;
    services.http.limits = EffectiveHttpLimits{};

    auto state = LuaHostState::Load({
        .script = R"lua(return {
        run = function(ctx, input, next)
          local r, err = luban.http.request{method="GET", url="https://kb.example/v1"}
          return { output = { got = r ~= nil } }
        end
      })lua",
        .chunk_name = "hook.lua",
        .entries = {"run"},
        .profile = tools::LuaProfile::HookDefault(),
    });
    REQUIRE(state.has_value());

    std::atomic<bool> cancelled{true};
    LuaCallContext::HookIdentity identity;
    identity.dispatch_id = "hookmw_cancel";
    identity.invocation_id = "hookmw_cancel#0";
    identity.hook_id = "PostUser/cancel.probe";
    identity.hook_point = "PostUser";
    services.http.cancel = &cancelled;  // invocation 的取消旗 = HTTP 的取消旗
    LuaCallContext context = LuaCallContext::ForHook(std::move(identity), &services);

    LuaHostState::LuaHookCall call;
    call.ctx_meta = nlohmann::json{{"dispatchId", "hookmw_cancel"}};
    call.input = nlohmann::json{{"prompt", "原文"}};
    call.cancel = &cancelled;
    const auto result = (*state)->CallHook("run", call, context);
    REQUIRE(result.ok);
    CHECK(result.output.at("got") == true);
    // 旗真递到了传输 seam(cancel_observed = cancel 指针非空)。
    REQUIRE(transport.call_count() == 1);
    CHECK(transport.calls().at(0).cancel_observed);
}

// ---------------------------------------------------------------------------
// 受控文件(第 2 条:fs)
// ---------------------------------------------------------------------------

TEST_CASE("fs:授权根内读/写/列,原子写真落盘;越界与超帽拒绝") {
    TempDir dir("fs");
    std::filesystem::create_directories(dir.path / "pkg");
    std::filesystem::create_directories(dir.path / "pkg" / "data");
    { std::ofstream(dir.path / "pkg" / "note.txt", std::ios::binary) << "第一版"; }

    HookHostServiceCenter::Grants grants;
    HookFsGrant fs;
    fs.base = dir.path / "pkg";
    fs.read_roots = {""};  // 整包可读
    fs.write_roots = {"data"};
    grants.fs = fs;
    HookStateStore::Limits state_limits;
    grants.state = std::make_shared<HookStateStore>(state_limits);
    HookHostServiceCenter center(std::move(grants));

    MiddlewarePool pool(PoolWithCenter(center));
    REQUIRE(AddLuaHook(pool,
                       R"({"schemaVersion":1,"id":"fs-probe","entry":"main.lua","hooks":[
                            {"hookPoint":"PostUser","name":"fs.probe","handler":"run",
                             "capabilities":["fs.read","fs.write"]}]})",
                       R"lua(return {
        run = function(ctx, input, next)
          local read_ok, read_err = luban.fs.read("note.txt")
          local write_ok, write_err = luban.fs.write("data/out.bin", "第二版")
          local listed, list_err = luban.fs.list("data")
          local escape_ok, escape_err = luban.fs.read("../outside.txt")
          local root_write_ok, root_write_err = luban.fs.write("top.txt", "越权")
          return { output = {
            read = read_ok, write = write_ok, listed = listed and #listed or -1,
            escape = escape_ok ~= nil, escape_code = escape_err and escape_err.code,
            root_write = root_write_ok ~= nil, root_write_code = root_write_err and root_write_err.code,
          } }
        end
      })lua")
                  .has_value());
    const nlohmann::json value = DispatchValue(HookPoint::PostUser, pool);
    CHECK(value.at("read") == "第一版");
    CHECK(value.at("write") == true);
    CHECK(value.at("listed") == 1);  // data/ 里 out.bin 一枚
    // .. 逃出包根:拒绝;写根不含包顶层:拒绝。
    CHECK(value.at("escape") == false);
    CHECK(value.at("escape_code") == "hook.fs.path_denied");
    CHECK(value.at("root_write") == false);
    CHECK(value.at("root_write_code") == "hook.fs.path_denied");
    // 落盘事实:原子写顶替了 data/out.bin;包顶层没冒出 top.txt。
    std::error_code ec;
    CHECK(std::filesystem::file_size(dir.path / "pkg" / "data" / "out.bin", ec) == 9);   // "第二版" 9 字节 UTF-8
    CHECK_FALSE(std::filesystem::exists(dir.path / "pkg" / "top.txt", ec));
    CHECK(std::filesystem::exists(dir.path / "pkg" / "note.txt", ec));  // 只读申请没动它
}

TEST_CASE("fs:字节帽拦大文件;只授 fs.read 时 write 拒绝") {
    TempDir dir("fscap");
    std::filesystem::create_directories(dir.path / "pkg");
    { std::ofstream(dir.path / "pkg" / "big.txt", std::ios::binary) << std::string(300 * 1024, 'x'); }

    HookHostServiceCenter::Grants grants;
    HookFsGrant fs;
    fs.base = dir.path / "pkg";
    fs.read_roots = {""};
    fs.write_roots = {""};
    fs.max_read_bytes = 4096;  // 收紧读帽
    grants.fs = fs;
    grants.state = std::make_shared<HookStateStore>();
    HookHostServiceCenter center(std::move(grants));

    // 只申请 fs.read:write_roots 被裁,write 拒。
    MiddlewarePool pool(PoolWithCenter(center));
    REQUIRE(AddLuaHook(pool,
                       R"({"schemaVersion":1,"id":"fs-cap","entry":"main.lua","hooks":[
                            {"hookPoint":"PostUser","name":"fs.cap","handler":"run",
                             "capabilities":["fs.read"]}]})",
                       R"lua(return {
        run = function(ctx, input, next)
          local big, big_err = luban.fs.read("big.txt")
          local w, w_err = luban.fs.write("any.txt", "x")
          return { output = {
            big_code = big == nil and big_err.code,
            write_code = w == nil and w_err.code,
          } }
        end
      })lua")
                  .has_value());
    const nlohmann::json value = DispatchValue(HookPoint::PostUser, pool);
    CHECK(value.at("big_code") == "hook.fs.too_large");
    CHECK(value.at("write_code") == "hook.capability_not_granted");
}

// ---------------------------------------------------------------------------
// 插件状态(第 2 条:state)
// ---------------------------------------------------------------------------

TEST_CASE("state:按包隔离、键值校验、限额;跨 invocation 可见(宿主持有)") {
    HookHostServiceCenter::Grants grants;
    HookStateStore::Limits limits;
    limits.max_keys_per_package = 2;
    limits.max_value_bytes = 64;
    grants.state = std::make_shared<HookStateStore>(limits);
    HookHostServiceCenter center(std::move(grants));

    const std::string script = R"lua(return {
      run = function(ctx, input, next)
        local code
        if input.prompt == "set" then
          local ok, err = luban.state.set(input.key, input.value)
          code = err and err.code or "ok"
        elseif input.prompt == "get" then
          local v = luban.state.get(input.key)
          return { output = { value = v, missing = (v == nil) } }
        elseif input.prompt == "keys" then
          local ks = luban.state.keys()
          return { output = { count = #ks, first = ks[1] } }
        end
        return { output = { code = code } }
      end
    })lua";

    // 包 A:两个键放行,第三个撞键数帽;坏键名拒。
    MiddlewarePool pool_a(PoolWithCenter(center));
    REQUIRE(AddLuaHook(pool_a,
                       R"({"schemaVersion":1,"id":"pkg-a","entry":"main.lua","hooks":[
                            {"hookPoint":"PostUser","name":"a.state","handler":"run",
                             "capabilities":["state"]}]})",
                       script)
                  .has_value());
    CHECK(DispatchValue(HookPoint::PostUser, pool_a, {{"prompt", "set"}, {"key", "k1"}, {"value", 1}})
              .at("code") == "ok");
    CHECK(DispatchValue(HookPoint::PostUser, pool_a, {{"prompt", "set"}, {"key", "k2"}, {"value", 2}})
              .at("code") == "ok");
    CHECK(DispatchValue(HookPoint::PostUser, pool_a,
                        {{"prompt", "set"}, {"key", "k3"}, {"value", 3}})
              .at("code") == "hook.state.quota_exceeded");
    CHECK(DispatchValue(HookPoint::PostUser, pool_a,
                        {{"prompt", "set"}, {"key", "bad key!"}, {"value", 3}})
              .at("code") == "hook.state.bad_key");
    const std::string big(128, 'v');
    CHECK(DispatchValue(HookPoint::PostUser, pool_a,
                        {{"prompt", "set"}, {"key", "k1"}, {"value", big}})
              .at("code") == "hook.state.quota_exceeded");

    // 包 B:同一只 store,但命名空间不同——A 的键看不见。
    MiddlewarePool pool_b(PoolWithCenter(center));
    REQUIRE(AddLuaHook(pool_b,
                       R"({"schemaVersion":1,"id":"pkg-b","entry":"main.lua","hooks":[
                            {"hookPoint":"PostUser","name":"b.state","handler":"run",
                             "capabilities":["state"]}]})",
                       script)
                  .has_value());
    const nlohmann::json got =
        DispatchValue(HookPoint::PostUser, pool_b, {{"prompt", "get"}, {"key", "k1"}});
    CHECK(got.at("missing") == true);  // B 的 k1 空:A 的 k1 不串门
    CHECK(DispatchValue(HookPoint::PostUser, pool_b, {{"prompt", "keys"}}).at("count") == 0);

    // A 再看:跨 invocation 持久(同一只 store,宿主生命周期)。
    CHECK(DispatchValue(HookPoint::PostUser, pool_a, {{"prompt", "get"}, {"key", "k1"}})
              .at("value") == 1);
    CHECK(DispatchValue(HookPoint::PostUser, pool_a, {{"prompt", "keys"}}).at("first") == "k1");
}

// ---------------------------------------------------------------------------
// context 候选(第 2 条:context)
// ---------------------------------------------------------------------------

TEST_CASE("context.append:候选折成 ContextAppend 效果,经宿主校验采用,带来源") {
    HookHostServiceCenter center;  // context 不需要宿主 grants(采用走效果管道)
    MiddlewarePool pool(PoolWithCenter(center));
    REQUIRE(AddLuaHook(pool,
                       R"({"schemaVersion":1,"id":"ctx-append","entry":"main.lua","hooks":[
                            {"hookPoint":"PostUser","name":"ctx.append","handler":"run",
                             "capabilities":["context"]}]})",
                       R"lua(return {
      run = function(ctx, input, next)
        luban.context.append("召回到的外部资料", "kb")
        return { output = { appended = true } }
      end
    })lua")
                  .has_value());
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    MiddlewareDispatcher dispatcher(std::move(*published));
    DispatchTrigger trigger;
    trigger.input = {{"prompt", "原文"}};
    const DispatchOutcome outcome =
        dispatcher.Dispatch(HookPoint::PostUser, trigger, [](const nlohmann::json& input) { return input; });
    REQUIRE(outcome.Ok());
    // PostUser 允许 context.append(挂点合同):恰好一条,带来源与文本。
    REQUIRE(outcome.context_appends.size() == 1);
    CHECK(outcome.context_appends.at(0) == "召回到的外部资料");
    const InvocationRecord* record = outcome.FindRecord("PostUser/ctx.append");
    REQUIRE(record != nullptr);
    bool saw_effect = false;
    for (const EffectRecord& effect : record->effects) {
        if (effect.type == "context.append" && effect.applied) {
            saw_effect = true;
            CHECK(effect.payload.at("source") == "hook:ctx-append/kb");
        }
    }
    CHECK(saw_effect);
    // 挂点不收的位置(PreAssistant 只准入):同款 append 被拒,不进账。
    MiddlewarePool wrong_point_pool(PoolWithCenter(center));
    REQUIRE(AddLuaHook(wrong_point_pool,
                       R"({"schemaVersion":1,"id":"ctx-wrong","entry":"main.lua","hooks":[
                            {"hookPoint":"PreAssistant","name":"ctx.wrong","handler":"run",
                             "capabilities":["context"]}]})",
                       R"lua(return {
      run = function(ctx, input, next)
        luban.context.append("不该进来的", nil)
        return { output = {} }
      end
    })lua")
                  .has_value());
    auto wrong_published = wrong_point_pool.Publish();
    REQUIRE(wrong_published.has_value());
    MiddlewareDispatcher wrong_dispatcher(std::move(*wrong_published));
    const DispatchOutcome wrong_outcome =
        wrong_dispatcher.Dispatch(HookPoint::PreAssistant, trigger,
                                  [](const nlohmann::json& input) { return input; });
    CHECK(wrong_outcome.context_appends.empty());
}

// ---------------------------------------------------------------------------
// 结构化日志(第 2 条:log)
// ---------------------------------------------------------------------------

TEST_CASE("log.event:结构化字段进宿主 sink,长值截断;非法 level/fields 拒绝") {
    std::vector<nlohmann::json> received;
    HookHostServiceCenter::Grants grants;
    grants.log = [&received](const std::string& level, const nlohmann::json& fields) {
        received.push_back(nlohmann::json{{"level", level}, {"fields", fields}});
    };
    grants.state = std::make_shared<HookStateStore>();
    HookHostServiceCenter center(std::move(grants));

    MiddlewarePool pool(PoolWithCenter(center));
    REQUIRE(AddLuaHook(pool,
                       R"({"schemaVersion":1,"id":"log-probe","entry":"main.lua","hooks":[
                            {"hookPoint":"PostUser","name":"log.probe","handler":"run",
                             "capabilities":["log"]}]})",
                       R"lua(return {
      run = function(ctx, input, next)
        luban.log.event("warn", { note = "召回失败", blob = string.rep("x", 5000) })
        local bad, bad_err = luban.log.event("verbose", {})
        return { output = { bad = bad ~= nil, bad_code = bad_err and bad_err.code } }
      end
    })lua")
                  .has_value());
    DispatchValue(HookPoint::PostUser, pool);
    REQUIRE(received.size() == 1);
    CHECK(received.at(0).at("level") == "warn");
    const nlohmann::json& fields = received.at(0).at("fields");
    CHECK(fields.at("note") == "召回失败");
    CHECK(fields.at("package") == "log-probe");
    // 长值截断:2 KiB 帽 + truncated 标。
    CHECK(fields.at("blob").at("truncated") == true);
    CHECK(fields.at("blob").at("text").get<std::string>().size() == 2048);
}
