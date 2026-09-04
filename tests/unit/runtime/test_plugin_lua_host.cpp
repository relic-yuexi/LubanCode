// Lua Host API 与动态调用上下文的单测(Lua 受控 HTTP 与 Secret 宿主能力
// 单·阶段 3)。对号设计单 §13.4 全案:
//   - 顶层 chunk 调 HTTP/Secret,假 transport/resolver 计数为 0(§九);
//   - handler 缺失/非函数整件拒挂(loader 层;挂载在阶段 4);
//   - Lua error/内存帽/指令帽/HTTP 错误各有唯一终态;
//   - 调用结束 context 清空,第二次调用不见上次 Secret/取消旗;
//   - 同 state 串行、不同插件可并行;
// 外加 §6.2/§6.3 的落法:请求全字段、响应全字段、err 表形状、禁写头双
// 侧一致、SecretRef 元方法锁死、auth.secret 语法糖、取消接线。
//
// 全程 FakeHttpTransport/FakeDnsResolver 一类假件,不碰公网;假 Key 一律
// FAKE_ 前缀。内存脚本(不走文件)直捣机制——正是阶段 3 验收的冒烟场。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/plugin_lua_host.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

// 假 resolver:计数 Resolve/Describe(顶层零副作用靠它钉死),按 id 发
// FAKE_ 前缀的假值;required 缺失按合同分流。
class CountingResolver final : public SecretResolver {
public:
    std::map<std::string, std::string> values;  // id -> FAKE_ 假值

    int resolve_count = 0;
    int describe_count = 0;

    std::expected<SecretValue, SecretResolveError> Resolve(const SecretDeclaration& declaration) override {
        ++resolve_count;
        const auto it = values.find(declaration.id);
        if (it == values.end()) {
            if (declaration.required) {
                SecretResolveError error;
                error.issue = SecretResolveIssue::Missing;
                error.message = "必需的 Secret 没找到: " + declaration.id;
                return std::unexpected(error);
            }
            return SecretValue(std::string());  // optional 缺失:空值,匿名降级
        }
        return SecretValue(std::string(it->second));
    }

    SecretStatus Describe(const SecretDeclaration& declaration) override {
        ++describe_count;
        SecretStatus status;
        status.id = declaration.id;
        status.env = declaration.env;
        status.required = declaration.required;
        status.available = values.find(declaration.id) != values.end();
        status.source = status.available ? SecretSource::HostEnv : SecretSource::None;
        return status;
    }
};

// 会睡的假 transport:验取消接线(在途置旗当口收口)与并发账(同 state
// 串行/不同插件并行)。FakeHttpTransport 是 final,组合不继承:睡完先查
// 取消,再走内嵌假件的记账路。
class BlockingFakeTransport final : public BoundedHttpTransport {
public:
    // 一笔睡眠的起止账(steady_clock)。判"真并行/真串行"看两笔睡眠
    // 区间是否重叠,不看墙钟总长:慢机的调度噪声吃得掉总时长差,吃
    // 不掉区间重叠与否——sleep_for 只会睡多不会睡少,每笔区间至少
    // block_ms 宽,重叠与否跟机器快慢不沾边。
    struct SleepWindow {
        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point finished;
    };

    std::chrono::milliseconds block_ms{150};
    FakeHttpTransport inner;
    int cancelled_observed = 0;  // 在途段亲眼看到旗子置位的次数
    // 每笔 Execute 一枚。两路线程 join 完再读;记账本身自带锁——串行
    // 册若 mutex 失守、两路真并睡了,记账也不许 UB。
    std::vector<SleepWindow> sleep_windows;

    std::expected<HttpExchangeResponse, HttpTransportError> Execute(
        const HttpExchangeRequest& request, const EffectiveHttpLimits& limits,
        const std::atomic<bool>* cancel) override {
        const auto started = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(block_ms);
        {
            const std::lock_guard<std::mutex> lock(windows_mutex_);
            sleep_windows.push_back({started, std::chrono::steady_clock::now()});
        }
        if (cancel != nullptr && cancel->load()) {
            ++cancelled_observed;  // 取消路不走 inner 的记账,自己记
            HttpTransportError error;
            error.code = LuaHostErrorCode::Cancelled;
            error.message = "已取消";
            return std::unexpected(error);
        }
        return inner.Execute(request, limits, cancel);
    }

private:
    std::mutex windows_mutex_;
};

// 两笔睡眠区间重叠 = 一路睡着的当口另一路也在睡:并行判据的语义本
// 体。证并行用它(CHECK),证串行用它的反(CHECK_FALSE),跟墙钟
// 精度、调度开销都不相干。
bool SleepWindowsOverlap(const BlockingFakeTransport::SleepWindow& a,
                         const BlockingFakeTransport::SleepWindow& b) {
    return a.started < b.finished && b.started < a.finished;
}

// 一份最小 manifest 账:api.anysearch.com 的 GET/POST + api_key(optional)。
PluginHttpCallSpec MakeSpec(BoundedHttpTransport* transport, SecretResolver* resolver,
                            const std::atomic<bool>* cancel) {
    PluginHttpCallSpec spec;
    spec.transport = transport;
    spec.secret_resolver = resolver;
    spec.cancel = cancel;
    SecretDeclaration secret;
    secret.id = "api_key";
    secret.env = "ANYSEARCH_API_KEY";
    secret.required = false;
    spec.secrets.push_back(secret);
    return spec;
}

std::expected<std::unique_ptr<LuaHostState>, std::string> LoadPlugin(const std::string& script,
                                                                     const std::vector<std::string>& entries = {"search"},
                                                                     const tools::LuaProfile& profile = tools::LuaProfile::PureDefault()) {
    LuaHostState::Options options;
    options.script = script;
    options.chunk_name = "test_plugin";
    options.entries = entries;
    options.profile = profile;
    return LuaHostState::Load(std::move(options));
}

// 200 + JSON 体 + 重复头(验数组形状)的标准应答。头按编排层收到的形状
// 给:已小写、已滤敏感字段(真传输在 transport 层做这两步,阶段 2 测过;
// 假件模拟它出仓的样子)。
HttpExchangeResponse MakeJsonResponse() {
    HttpExchangeResponse response;
    response.status = 200;
    response.headers.emplace_back("content-type", "application/json");
    response.headers.emplace_back("x-dup", "first");
    response.headers.emplace_back("x-dup", "second");
    response.body = "{\"query\":\"hello\",\"n\":5}";
    response.final_url = "https://api.anysearch.com/v1/search";
    return response;
}

}  // namespace

// ---------------------------------------------------------------------------
// §6.1 模块形状:只描述分派,不抄第二份 schema
// ---------------------------------------------------------------------------

TEST_CASE("模块注册:luban 只有 http/secrets 两张子表,各就各位") {
    FakeHttpTransport transport;
    CountingResolver resolver;
    auto plugin = LoadPlugin(R"lua(
      local keys = {}
      for k, v in pairs(luban) do keys[#keys + 1] = k .. "=" .. type(v) end
      table.sort(keys)
      shape = table.concat(keys, ",")
      return { search = function(input) return shape end }
    )lua");
    REQUIRE(plugin.has_value());
    CHECK(CurrentLuaCallContext((*plugin)->lua()) == nullptr);  // 加载完:context 真空

    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CHECK_FALSE(result.is_error);
    // 没有第二份 schema/工具表——manifest 是唯一账本(阶段 4 接线)。
    CHECK(result.content == "http=table,secrets=table");
}

// ---------------------------------------------------------------------------
// §九/§13.4:顶层零副作用(计数器钉死为 0)
// ---------------------------------------------------------------------------

TEST_CASE("顶层 chunk 调 HTTP/Secret:假计数器为 0,拿 no_active_tool_call") {
    FakeHttpTransport transport;
    CountingResolver resolver;
    resolver.values["api_key"] = "FAKE_TOP_LEVEL_KEY";
    auto plugin = LoadPlugin(R"lua(
      -- 恶意顶层:想联网、想摸 Secret,全都得手不了。
      local r, e = luban.http.request({ method = "GET", url = "https://api.anysearch.com/steal" })
      top_http = e and e.code
      top_http_retryable = e and e.retryable
      local a, aerr = luban.secrets.available("api_key")
      top_available = a
      top_available_code = aerr and aerr.code
      local ref, rerr = luban.secrets.ref("api_key")
      top_ref = ref
      top_ref_code = rerr and rerr.code
      return { search = function(input)
        return table.concat({ tostring(top_http), tostring(top_http_retryable),
                              tostring(top_available), tostring(top_available_code),
                              tostring(top_ref), tostring(top_ref_code) }, "|")
      end }
    )lua");
    REQUIRE(plugin.has_value());

    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CHECK_FALSE(result.is_error);
    CHECK(result.content ==
          "no_active_tool_call|false|nil|no_active_tool_call|nil|no_active_tool_call");
    // 机关的证据:顶层翻完全套,一根毛都没碰着。
    CHECK(transport.call_count() == 0);
    CHECK(resolver.resolve_count == 0);
    CHECK(resolver.describe_count == 0);
}

TEST_CASE("顶层 chunk 硬翻(把 err 直接 error 出来):整件拒挂,计数仍为 0") {
    FakeHttpTransport transport;
    CountingResolver resolver;
    auto plugin = LoadPlugin(R"lua(
      local r, e = luban.http.request({ method = "GET", url = "https://api.anysearch.com/x" })
      if e ~= nil then error("顶层想联网被拦: " .. e.code) end
      return { search = function(input) return "ok" end }
    )lua");
    REQUIRE_FALSE(plugin.has_value());
    CHECK(plugin.error().find("顶层想联网被拦") != std::string::npos);
    CHECK(transport.call_count() == 0);
    CHECK(resolver.resolve_count == 0);
}

// ---------------------------------------------------------------------------
// §6.1/§13.4:loader 层对账(挂载在阶段 4)
// ---------------------------------------------------------------------------

TEST_CASE("handler 对账:缺 handler/非函数/entry 重复/返回非表,整件拒挂") {
    auto missing = LoadPlugin("return { other = function(input) return 'x' end }", {"search"});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("search") != std::string::npos);
    CHECK(missing.error().find("handler") != std::string::npos);

    auto not_function = LoadPlugin("return { search = 'not a function' }");
    REQUIRE_FALSE(not_function.has_value());
    CHECK(not_function.error().find("function handler") != std::string::npos);

    auto duplicated = LoadPlugin("return { search = function(i) return 'x' end }", {"search", "search"});
    REQUIRE_FALSE(duplicated.has_value());
    CHECK(duplicated.error().find("entry 重复") != std::string::npos);

    auto not_table = LoadPlugin("return 42");
    REQUIRE_FALSE(not_table.has_value());
    CHECK(not_table.error().find("不是表") != std::string::npos);

    // 多一枚未声明 function:留着,不挂成工具,不拒载(§6.1)。
    auto extra = LoadPlugin("return { search = function(i) return 'x' end, helper = function() return 1 end }");
    REQUIRE(extra.has_value());
    CHECK((*extra)->entries().size() == 1);
}

TEST_CASE("编译失败与顶层执行失败分开报") {
    auto syntax = LoadPlugin("return {{{");
    REQUIRE_FALSE(syntax.has_value());
    CHECK(syntax.error().find("编译失败") != std::string::npos);

    auto runtime_fail = LoadPlugin("error('加载期炸了') return { search = function(i) return 'x' end }");
    REQUIRE_FALSE(runtime_fail.has_value());
    CHECK(runtime_fail.error().find("执行失败") != std::string::npos);
    CHECK(runtime_fail.error().find("加载期炸了") != std::string::npos);
}

// ---------------------------------------------------------------------------
// §6.2 http.request:全字段落法
// ---------------------------------------------------------------------------

TEST_CASE("http.request:入参全字段 -> 出参全字段(含重复头数组与响应过滤)") {
    FakeHttpTransport transport;
    transport.EnqueueResponse(MakeJsonResponse());
    CountingResolver resolver;
    resolver.values["api_key"] = "FAKE_BEARER_TOKEN_12345";

    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local r, e = luban.http.request({
          method = "post",  -- 小写:宿主规范化成 POST
          url = "https://api.anysearch.com/v1/search",
          headers = { ["X-Anysearch-Client"] = "lubancode-lua/0.1.0" },
          json = input,
          auth = { type = "bearer", secret = "api_key", optional = true },
          timeout_ms = 5000,
        })
        if e ~= nil then return "ERR:" .. e.code end
        return table.concat({
          tostring(r.status), r.body, r.url, tostring(r.bytes),
          tostring(r.json and r.json.query), tostring(r.json and r.json.n),
          tostring(r.headers["content-type"]),
          tostring(r.headers["accept"]),   -- 没有的头:Lua 侧就是 nil
          tostring(r.headers["x-dup"][2]), -- 重复头按数组保留
        }, "|")
      end }
    )lua");
    REQUIRE(plugin.has_value());

    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json{{"query", "hello"}}, context);
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content ==
          "200|{\"query\":\"hello\",\"n\":5}|https://api.anysearch.com/v1/search|23|hello|5|"
          "application/json|nil|second");

    // 假 transport 看到的最终请求:C++ 侧的全部落法。
    REQUIRE(transport.call_count() == 1);
    const auto& call = transport.calls()[0];
    CHECK(call.request.method == "POST");  // 小写被规范化
    CHECK(call.request.url == "https://api.anysearch.com/v1/search");
    CHECK(call.request.body == "{\"query\":\"hello\"}");
    CHECK(call.limits.timeout_ms == 5000);  // 只降不升
    CHECK(call.cancel_observed);
    bool saw_content_type = false;
    bool saw_client = false;
    bool saw_authorization = false;
    for (const auto& [name, value] : call.request.headers) {
        if (name == "Content-Type") {
            saw_content_type = true;
            CHECK(value == "application/json");  // json 自动补
        } else if (name == "X-Anysearch-Client") {
            saw_client = true;
            CHECK(value == "lubancode-lua/0.1.0");
        } else if (name == "Authorization") {
            saw_authorization = true;
            CHECK(value == "Bearer FAKE_BEARER_TOKEN_12345");  // 宿主代填
        }
    }
    CHECK(saw_content_type);
    CHECK(saw_client);
    CHECK(saw_authorization);
    // Secret 原文只活在宿主侧:Lua 的返回里没有它。
    CHECK(result.content.find("FAKE_") == std::string::npos);
    CHECK(resolver.resolve_count == 1);
}

TEST_CASE("http.request:body 原文直发、GET 不带体、timeout_ms 越顶只取帽") {
    FakeHttpTransport transport;
    HttpExchangeResponse response;
    response.status = 200;
    response.body = "plain";
    transport.EnqueueResponse(response);
    CountingResolver resolver;

    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local r1, e1 = luban.http.request({
          method = "POST", url = "https://api.anysearch.com/v1/sub",
          headers = { ["Accept"] = "text/plain" }, body = "raw-bytes",
          timeout_ms = 999999,
        })
        if e1 ~= nil then return "ERR1:" .. e1.code end
        -- GET 带 body:宿主拦(§6.2)。
        local r2, e2 = luban.http.request({
          method = "GET", url = "https://api.anysearch.com/v1/sub", body = "raw-bytes",
        })
        return r1.body .. "|" .. tostring(r1.json) .. "|" .. (e2 and e2.code or "NO-ERR")
      end }
    )lua");
    REQUIRE(plugin.has_value());
    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "plain|nil|invalid_request");  // 非 JSON Content-Type:无 json 字段

    REQUIRE(transport.call_count() == 1);  // GET 那笔没发出去
    const auto& call = transport.calls()[0];
    CHECK(call.request.method == "POST");
    CHECK(call.request.body == "raw-bytes");  // body 原文直发,不经 JSON
    // timeout 只降不升:999999 被钳回生效帽(缺省 30s)。
    CHECK(call.limits.timeout_ms == kHttpTimeoutDefaultMs);
}

TEST_CASE("http.request:禁写 header 在 Lua 侧也拒(与 C++ 侧一致)") {
    FakeHttpTransport transport;
    CountingResolver resolver;
    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local tried = {}
        for _, h in ipairs({ "Authorization", "authorization", "Cookie", "Host", "Content-Length",
                             "Proxy-Authorization" }) do
          local r, e = luban.http.request({
            method = "GET", url = "https://api.anysearch.com/v1/x",
            headers = { [h] = "lua-self-written" },
          })
          tried[#tried + 1] = e and e.code or "NO-ERR"
        end
        return table.concat(tried, ",")
      end }
    )lua");
    REQUIRE(plugin.has_value());
    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content ==
          "invalid_request,invalid_request,invalid_request,invalid_request,invalid_request,invalid_request");
    CHECK(transport.call_count() == 0);  // 一个包都没发出去
}

TEST_CASE("http.request:json 与 body 同填/坏形状按 invalid_request 拒") {
    FakeHttpTransport transport;
    CountingResolver resolver;
    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local r1, e1 = luban.http.request({ method = "POST", url = "https://api.anysearch.com/v1/x",
          json = { a = 1 }, body = "both" })
        local r2, e2 = luban.http.request({ method = "GET" })  -- 缺 url
        local r3, e3 = luban.http.request("not a table")
        local r4, e4 = luban.http.request({ method = "POST", url = "https://api.anysearch.com/v1/x",
          json = { bad = function() end } })  -- 函数进不了 JSON
        return table.concat({ e1.code, e2.code, e3.code, e4.code }, ",")
      end }
    )lua");
    REQUIRE(plugin.has_value());
    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "invalid_request,invalid_request,invalid_request,invalid_request");
    CHECK(transport.call_count() == 0);
}

TEST_CASE("http.request:错误表形状 {code,message,status,retryable} 全带") {
    FakeHttpTransport transport;
    HttpTransportError error;
    error.code = LuaHostErrorCode::ResponseTooLarge;
    error.message = "响应体超过 4194304 字节,已中止";
    transport.EnqueueError(error);
    CountingResolver resolver;

    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local r, e = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x" })
        if r ~= nil then return "UNEXPECTED-OK" end
        return table.concat({ e.code, tostring(e.status), tostring(e.retryable),
                              tostring(e.message ~= nil and #e.message > 0) }, "|")
      end }
    )lua");
    REQUIRE(plugin.has_value());
    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "response_too_large|0|false|true");
}

// ---------------------------------------------------------------------------
// §6.3 Secret API:available/ref 与 SecretRef 元方法锁死
// ---------------------------------------------------------------------------

TEST_CASE("SecretRef:元方法锁死清单(tostring/拼接/索引/锁表/比较/转 JSON)") {
    FakeHttpTransport transport;
    transport.EnqueueResponse(MakeJsonResponse());
    CountingResolver resolver;
    resolver.values["api_key"] = "FAKE_REF_LEAK_PROBE";

    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local ref = luban.secrets.ref("api_key")
        local notes = {}
        notes[#notes + 1] = tostring(ref)                    -- <secret:api_key>
        local ok1, err1 = pcall(function() return ref .. "-x" end)
        notes[#notes + 1] = tostring(ok1) .. ":" .. (ok1 and "" or (err1 and err1:match("concatenat") ~= nil and "concat" or "?"))
        local ok2, err2 = pcall(function() return ref.value end)
        notes[#notes + 1] = tostring(ok2) .. ":" .. (ok2 and "" or (err2 and err2:match("index") ~= nil and "index" or "?"))
        local ok3, err3 = pcall(function() return ref[1] end)
        notes[#notes + 1] = tostring(ok3) .. ":" .. (ok3 and "" or (err3 and err3:match("index") ~= nil and "index" or "?"))
        notes[#notes + 1] = type(getmetatable(ref))           -- 锁表:只得串
        local ok4, err4 = pcall(function() return ref:method() end)  -- 冒号调用也是索引
        notes[#notes + 1] = tostring(ok4)
        notes[#notes + 1] = tostring(ref == luban.secrets.ref("api_key"))  -- 同一性比较:false
        notes[#notes + 1] = tostring(ref == ref)              -- 自己比自己:true
        notes[#notes + 1] = tostring(luban.secrets.available("api_key"))
        return table.concat(notes, ",")
      end }
    )lua");
    REQUIRE(plugin.has_value());
    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content ==
          "<secret:api_key>,false:concat,false:index,false:index,string,false,false,true,true");
    // 原文从未进入 Lua:返回里只有 <secret:api_key>。
    CHECK(result.content.find("FAKE_") == std::string::npos);
    // available 走的是 Describe(状态口),不是 Resolve(取值口)。
    CHECK(resolver.describe_count == 1);
    CHECK(resolver.resolve_count == 0);
}

TEST_CASE("SecretRef 转 JSON 被拒:handler 想把 ref 回给模型,终态是错") {
    FakeHttpTransport transport;
    CountingResolver resolver;
    resolver.values["api_key"] = "FAKE_JSON_LEAK_PROBE";

    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        return { leaked = luban.secrets.ref("api_key") }  -- 想混出模型上下文
      end }
    )lua");
    REQUIRE(plugin.has_value());
    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CHECK(result.is_error);
    CHECK(result.content.find("没法转成 JSON") != std::string::npos);
    CHECK(result.content.find("userdata") != std::string::npos);
}

TEST_CASE("secrets.available/ref:未声明报 secret_not_declared;missing 分流") {
    FakeHttpTransport transport;
    transport.EnqueueResponse(MakeJsonResponse());
    transport.EnqueueResponse(MakeJsonResponse());
    transport.EnqueueResponse(MakeJsonResponse());
    CountingResolver resolver;  // 没灌任何值:api_key optional,匿名路

    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local a1, e1 = luban.secrets.available("not_declared")
        local r1, e2 = luban.secrets.ref("not_declared")
        local a2 = luban.secrets.available("api_key")       -- optional 且缺失:false
        -- required 缺失走 auth 注入链:secret_missing
        local r3, e3 = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x",
          auth = { type = "bearer", secret = "api_key" } })  -- optional 缺省 false
        -- optional=true 缺失:匿名降级,请求照发
        local r4, e4 = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x",
          auth = { type = "bearer", secret = "api_key", optional = true } })
        return table.concat({ e1 and e1.code, e2 and e2.code, tostring(a2),
                              e3 and e3.code, e4 and e4.code or "OK:" .. r4.status }, ",")
      end }
    )lua");
    REQUIRE(plugin.has_value());
    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "secret_not_declared,secret_not_declared,false,secret_missing,OK:200");
    // 匿名降级那一笔:没有 Authorization 头。
    REQUIRE(transport.call_count() == 1);  // required 那笔没发包
    bool saw_authorization = false;
    for (const auto& [name, value] : transport.calls()[0].request.headers) {
        if (name == "Authorization") {
            saw_authorization = true;
        }
    }
    CHECK_FALSE(saw_authorization);
}

TEST_CASE("auth.secret 语法糖与 SecretRef 走同一条注入链") {
    FakeHttpTransport transport;
    transport.EnqueueResponse(MakeJsonResponse());
    transport.EnqueueResponse(MakeJsonResponse());
    CountingResolver resolver;
    resolver.values["api_key"] = "FAKE_SAME_CHAIN_TOKEN";

    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        -- 一枚字符串糖、一枚 SecretRef:结果必须一字不差。
        local r1, e1 = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x",
          auth = { type = "bearer", secret = "api_key", optional = true } })
        local r2, e2 = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x",
          auth = { type = "bearer", secret = luban.secrets.ref("api_key"), optional = true } })
        -- auth.secret 给个数字:形状错。
        local r3, e3 = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x",
          auth = { type = "bearer", secret = 12345 } })
        return table.concat({ r1.status, r2.status, e3.code }, ",")
      end }
    )lua");
    REQUIRE(plugin.has_value());
    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "200,200,invalid_request");

    REQUIRE(transport.call_count() == 2);
    for (const auto& call : transport.calls()) {
        bool saw = false;
        for (const auto& [name, value] : call.request.headers) {
            if (name == "Authorization") {
                saw = true;
                CHECK(value == "Bearer FAKE_SAME_CHAIN_TOKEN");
            }
        }
        CHECK(saw);
    }
    CHECK(resolver.resolve_count == 2);  // 两笔各解析一次,同一注入链
}

TEST_CASE("auth.type=header:三类规范名 + prefix,bearer 不收 name/prefix") {
    FakeHttpTransport transport;
    transport.EnqueueResponse(MakeJsonResponse());
    transport.EnqueueResponse(MakeJsonResponse());
    transport.EnqueueResponse(MakeJsonResponse());
    CountingResolver resolver;
    resolver.values["api_key"] = "FAKE_HEADER_AUTH_TOKEN";

    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local r1, e1 = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x",
          auth = { type = "header", name = "X-Api-Key", secret = "api_key", optional = true } })
        local r2, e2 = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x",
          auth = { type = "header", name = "X-Custom-Business-Field", secret = "api_key",
                   optional = true } })  -- 非规范名:拒
        local r3, e3 = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x",
          auth = { type = "bearer", secret = "api_key", name = "X-Api-Key",
                   optional = true } })  -- bearer 不收 name
        return table.concat({ r1.status, e2.code, e3.code }, ",")
      end }
    )lua");
    REQUIRE(plugin.has_value());
    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "200,invalid_request,invalid_request");
    REQUIRE(transport.call_count() == 1);
    bool saw = false;
    for (const auto& [name, value] : transport.calls()[0].request.headers) {
        if (name == "X-Api-Key") {
            saw = true;
            CHECK(value == "FAKE_HEADER_AUTH_TOKEN");
        }
    }
    CHECK(saw);
}

// ---------------------------------------------------------------------------
// §8.4 取消接线:同一枚旗,hook 与 HTTP 回调两路
// ---------------------------------------------------------------------------

TEST_CASE("取消接线:调用前已置位,instruction hook 当场掐,零网络") {
    FakeHttpTransport transport;
    CountingResolver resolver;
    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local n = 0
        for i = 1, 3000000 do n = n + 1 end  -- 走够 hook 步长,给取消落锤的机会
        return "never:" .. n
      end }
    )lua");
    REQUIRE(plugin.has_value());

    std::atomic<bool> cancel{true};  // 调用前就置位
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CHECK(result.is_error);
    CHECK(result.outcome == "plugin_exception");
    CHECK(result.error_code == "plugin.lua_error");
    CHECK(result.content.find("用户取消") != std::string::npos);
    CHECK(result.content.find("never") == std::string::npos);
    CHECK(transport.call_count() == 0);
    CHECK(CurrentLuaCallContext((*plugin)->lua()) == nullptr);  // 取消路径也清空
}

TEST_CASE("取消接线:在途 HTTP 中段置旗,transport 收口,Lua 拿 cancelled") {
    BlockingFakeTransport transport;
    CountingResolver resolver;
    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local r, e = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x",
          timeout_ms = 30000 })
        if e ~= nil then return "CANCELLED:" .. e.code .. ":" .. tostring(e.retryable) end
        return "ok"
      end }
    )lua");
    REQUIRE(plugin.has_value());

    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 睡进在途段再置旗
        cancel.store(true);
    });
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    canceller.join();
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);  // handler 把 cancelled 翻成业务返回
    CHECK(result.content == "CANCELLED:cancelled:false");
    CHECK(transport.cancelled_observed == 1);  // 同一枚旗递到了 transport 回调
}

// ---------------------------------------------------------------------------
// §13.4:唯一终态四件
// ---------------------------------------------------------------------------

TEST_CASE("唯一终态:Lua error / 内存帽 / 指令帽 / HTTP 错误各归各位") {
    FakeHttpTransport transport;
    CountingResolver resolver;
    std::atomic<bool> cancel{false};

    SUBCASE("Lua 普通 error") {
        auto plugin = LoadPlugin("return { search = function(i) error('handler 炸了') end }");
        REQUIRE(plugin.has_value());
        LuaCallContext context;
        context.http = MakeSpec(&transport, &resolver, &cancel);
        const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
        CHECK(result.is_error);
        CHECK(result.outcome == "plugin_exception");
        CHECK(result.error_code == "plugin.lua_error");
        CHECK(result.content.find("handler 炸了") != std::string::npos);
        CHECK(CurrentLuaCallContext((*plugin)->lua()) == nullptr);  // 异常路径也清空
    }

    SUBCASE("内存帽:OOM 落 luaL_error,宿主堆不破") {
        tools::LuaProfile profile = tools::LuaProfile::PureDefault();
        profile.memory_cap_bytes = 8 * 1024 * 1024;
        auto plugin = LoadPlugin(
            "return { search = function(i) local c = {} for n = 1, 1000 do c[n] = string.rep('x', 1024 * 1024) end return 'ate it' end }",
            {"search"}, profile);
        REQUIRE(plugin.has_value());
        LuaCallContext context;
        context.http = MakeSpec(&transport, &resolver, &cancel);
        const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
        CHECK(result.is_error);
        CHECK(result.error_code == "plugin.lua_error");
        CHECK(result.content.find("not enough memory") != std::string::npos);
    }

    SUBCASE("指令帽:死循环在预算内掐断") {
        tools::LuaProfile profile = tools::LuaProfile::PureDefault();
        profile.instruction_budget = 2'000'000;
        auto plugin = LoadPlugin("return { search = function(i) while true do end end }", {"search"}, profile);
        REQUIRE(plugin.has_value());
        LuaCallContext context;
        context.http = MakeSpec(&transport, &resolver, &cancel);
        const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
        CHECK(result.is_error);
        CHECK(result.error_code == "plugin.lua_error");
        CHECK(result.content.find("cpu 指令预算耗尽") != std::string::npos);
    }

    SUBCASE("HTTP 错误:不炸 state,handler 自决(可翻业务返回)") {
        HttpTransportError error;
        error.code = LuaHostErrorCode::Timeout;
        error.message = "宿主墙钟到点";
        transport.EnqueueError(error);
        auto plugin = LoadPlugin(R"lua(
          return { search = function(i)
            local r, e = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x" })
            if e ~= nil then return "HTTP-ERR:" .. e.code .. ":" .. tostring(e.retryable) end
            return "ok"
          end }
        )lua");
        REQUIRE(plugin.has_value());
        LuaCallContext context;
        context.http = MakeSpec(&transport, &resolver, &cancel);
        const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
        CHECK_FALSE(result.is_error);  // 工具层不是错:HTTP 错由 Lua 翻译
        CHECK(result.content == "HTTP-ERR:timeout:true");
        // state 活着:第二笔照常。
        transport.EnqueueResponse(MakeJsonResponse());
        LuaCallContext second_context;
        second_context.http = MakeSpec(&transport, &resolver, &cancel);
        const auto again = (*plugin)->Call("search", nlohmann::json::object(), second_context);
        CHECK_FALSE(again.is_error);
        CHECK(again.content.find("HTTP-ERR") == std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// §13.4:调用结束 context 清空;第二次调用不见上次 Secret/取消旗
// ---------------------------------------------------------------------------

TEST_CASE("context 清空:第二次调用不见上次的 Secret 与取消旗") {
    FakeHttpTransport transport;
    transport.EnqueueResponse(MakeJsonResponse());
    transport.EnqueueResponse(MakeJsonResponse());

    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local n = 0
        for i = 1, 400000 do n = n + 1 end  -- 走过 hook:陈旧的取消旗会在这里现形
        local r, e = luban.http.request({
          method = "GET", url = "https://api.anysearch.com/v1/x",
          auth = { type = "bearer", secret = "api_key", optional = true },
        })
        if e ~= nil then return "ERR:" .. e.code end
        return "OK:" .. r.status
      end }
    )lua");
    REQUIRE(plugin.has_value());
    CHECK(CurrentLuaCallContext((*plugin)->lua()) == nullptr);

    // 第一调:有钥匙,取消旗未置。
    CountingResolver first_resolver;
    first_resolver.values["api_key"] = "FAKE_FIRST_CALL_TOKEN";
    std::atomic<bool> first_cancel{false};
    {
        LuaCallContext context;
        context.http = MakeSpec(&transport, &first_resolver, &first_cancel);
        const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
        CHECK_FALSE(result.is_error);
        CHECK(result.content == "OK:200");
    }
    CHECK(CurrentLuaCallContext((*plugin)->lua()) == nullptr);  // 调用后:清空
    first_cancel.store(true);  // 调用结束才置位——不许串进第二调

    // 第二调:没钥匙(匿名),新取消旗。旧旗若残留,hook 会当场掐;旧
    // Secret 若缓存,Authorization 会带旧值。
    CountingResolver second_resolver;  // 空:api_key optional,匿名
    std::atomic<bool> second_cancel{false};
    {
        LuaCallContext context;
        context.http = MakeSpec(&transport, &second_resolver, &second_cancel);
        const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
        CHECK_FALSE(result.is_error);
        CHECK(result.content == "OK:200");
    }
    CHECK(CurrentLuaCallContext((*plugin)->lua()) == nullptr);

    REQUIRE(transport.call_count() == 2);
    bool first_has_auth = false;
    bool second_has_auth = false;
    for (const auto& [name, value] : transport.calls()[0].request.headers) {
        if (name == "Authorization") {
            first_has_auth = true;
            CHECK(value == "Bearer FAKE_FIRST_CALL_TOKEN");
        }
    }
    for (const auto& [name, value] : transport.calls()[1].request.headers) {
        if (name == "Authorization") {
            second_has_auth = true;
            CHECK(value.find("FAKE_FIRST_CALL_TOKEN") != std::string::npos);  // 这行不该走到
        }
    }
    CHECK(first_has_auth);
    CHECK_FALSE(second_has_auth);  // 上次的 Secret 不见
}

TEST_CASE("context 清空:Lua error 收场也不留旧 context") {
    FakeHttpTransport transport;
    transport.EnqueueResponse(MakeJsonResponse());
    CountingResolver resolver;
    auto plugin = LoadPlugin(R"lua(
      return { search = function(i)
        local r, e = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x" })
        error("半路炸")
      end }
    )lua");
    REQUIRE(plugin.has_value());
    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result = (*plugin)->Call("search", nlohmann::json::object(), context);
    CHECK(result.is_error);
    CHECK(CurrentLuaCallContext((*plugin)->lua()) == nullptr);
}

// ---------------------------------------------------------------------------
// §13.4/§8.5:同 state 串行,不同插件并行
// ---------------------------------------------------------------------------

TEST_CASE("同 state 串行:两路并发调用被 mutex 排队,不串结果") {
    BlockingFakeTransport transport;
    CountingResolver resolver;
    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local r, e = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x" })
        if e ~= nil then return "ERR:" .. e.code end
        return "OK:" .. input.tag
      end }
    )lua");
    REQUIRE(plugin.has_value());

    HttpExchangeResponse response;
    response.status = 200;
    transport.inner.EnqueueResponse(response);
    transport.inner.EnqueueResponse(response);

    std::atomic<bool> cancel{false};
    const auto started = std::chrono::steady_clock::now();
    std::thread a([&] {
        LuaCallContext context;
        context.http = MakeSpec(&transport, &resolver, &cancel);
        const auto result = (*plugin)->Call("search", nlohmann::json{{"tag", "A"}}, context);
        CHECK(result.content == "OK:A");
    });
    std::thread b([&] {
        LuaCallContext context;
        context.http = MakeSpec(&transport, &resolver, &cancel);
        const auto result = (*plugin)->Call("search", nlohmann::json{{"tag", "B"}}, context);
        CHECK(result.content == "OK:B");
    });
    a.join();
    b.join();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    CAPTURE(elapsed_ms);  // 诊断留影:串行该在 300ms 上下,不作断言(墙钟扛不住慢机噪声)
    // 证串行不靠总时长(那要拿墙钟精度扛调度开销):两笔睡眠区间不重
    // 叠,才是"mutex 把两路排队"的语义本身。区间重叠即 mutex 失守。
    REQUIRE(transport.sleep_windows.size() == 2);
    CHECK_FALSE(SleepWindowsOverlap(transport.sleep_windows[0], transport.sleep_windows[1]));
    CHECK(transport.inner.call_count() == 2);
}

TEST_CASE("不同插件并行:各有 state,各睡各的,睡眠区间真重叠") {
    BlockingFakeTransport transport_a;
    BlockingFakeTransport transport_b;
    CountingResolver resolver;
    const std::string script = R"lua(
      return { search = function(input)
        local r, e = luban.http.request({ method = "GET", url = "https://api.anysearch.com/v1/x" })
        if e ~= nil then return "ERR:" .. e.code end
        return "OK"
      end }
    )lua";
    auto plugin_a = LoadPlugin(script);
    auto plugin_b = LoadPlugin(script);
    REQUIRE(plugin_a.has_value());
    REQUIRE(plugin_b.has_value());

    HttpExchangeResponse response;
    response.status = 200;
    transport_a.inner.EnqueueResponse(response);
    transport_b.inner.EnqueueResponse(response);

    std::atomic<bool> cancel{false};
    const auto started = std::chrono::steady_clock::now();
    std::thread a([&] {
        LuaCallContext context;
        context.http = MakeSpec(&transport_a, &resolver, &cancel);
        CHECK((*plugin_a)->Call("search", nlohmann::json::object(), context).content == "OK");
    });
    std::thread b([&] {
        LuaCallContext context;
        context.http = MakeSpec(&transport_b, &resolver, &cancel);
        CHECK((*plugin_b)->Call("search", nlohmann::json::object(), context).content == "OK");
    });
    a.join();
    b.join();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    CAPTURE(elapsed_ms);  // 诊断留影:并行该在 150ms 上下,不作断言(墙钟扛不住慢机噪声)
    // 证并行不靠总时长:CI macOS 慢机(run 33920363083)上调度开销吃
    // 掉并行红利,< 290 这类墙钟帽实翻过。换区间重叠检测——两笔睡眠
    // 区间真重叠(a_start < b_end && b_start < a_end),才是"各有
    // state、各睡各的"的语义本体。每笔区间至少 150ms 宽,只有一路
    // 拖 150ms 以上才进不了重叠:那是真没并行,不是慢机误判。
    REQUIRE(transport_a.sleep_windows.size() == 1);
    REQUIRE(transport_b.sleep_windows.size() == 1);
    CHECK(SleepWindowsOverlap(transport_a.sleep_windows[0], transport_b.sleep_windows[0]));
    CHECK(transport_a.inner.call_count() == 1);
    CHECK(transport_b.inner.call_count() == 1);
}

// ---------------------------------------------------------------------------
// 阶段 3 验收冒烟:内存脚本调假 HTTPS 拿结构化 JSON;原文不进 Lua
// ---------------------------------------------------------------------------

TEST_CASE("冒烟:内存 Lua 脚本调假 HTTPS,拿结构化 JSON 交模型") {
    FakeHttpTransport transport;
    HttpExchangeResponse response;
    response.status = 200;
    response.headers.emplace_back("content-type", "application/json");
    response.body = "{\"results\":[{\"title\":\"LubanCode\",\"url\":\"https://example.com/luban\"},"
                    "{\"title\":\"受控 HTTP\",\"url\":\"https://example.com/http\"}]}";
    response.final_url = "https://api.anysearch.com/v1/search";
    transport.EnqueueResponse(response);
    CountingResolver resolver;
    resolver.values["api_key"] = "FAKE_SMOKE_BEARER";

    auto plugin = LoadPlugin(R"lua(
      return { search = function(input)
        local response, err = luban.http.request({
          method = "POST",
          url = "https://api.anysearch.com/v1/search",
          headers = { ["Content-Type"] = "application/json", ["X-Anysearch-Client"] = "lubancode-lua/0.1.0" },
          json = input,
          auth = { type = "bearer", secret = "api_key", optional = true },
          timeout_ms = 20000,
        })
        if err ~= nil then
          error(err.code .. ": " .. err.message)
        end
        -- 插件自己整形给模型:挑标题拼一段。
        local titles = {}
        for _, item in ipairs(response.json.results) do
          titles[#titles + 1] = item.title .. " <" .. item.url .. ">"
        end
        return table.concat(titles, "\n")
      end }
    )lua");
    REQUIRE(plugin.has_value());

    std::atomic<bool> cancel{false};
    LuaCallContext context;
    context.http = MakeSpec(&transport, &resolver, &cancel);
    const auto result =
        (*plugin)->Call("search", nlohmann::json{{"query", "lubancode 受控 http"}}, context);
    CAPTURE(result.content);
    CHECK_FALSE(result.is_error);
    CHECK(result.content ==
          "LubanCode <https://example.com/luban>\n受控 HTTP <https://example.com/http>");
    // 全链复查:Secret 原文只进过宿主侧的最终头表,Lua 的任何输出没有它。
    CHECK(result.content.find("FAKE_") == std::string::npos);
    bool auth_in_wire = false;
    for (const auto& [name, value] : transport.calls()[0].request.headers) {
        if (name == "Authorization") {
            auth_in_wire = true;
            CHECK(value == "Bearer FAKE_SMOKE_BEARER");
        }
    }
    CHECK(auth_in_wire);
}
