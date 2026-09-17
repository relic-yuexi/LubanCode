// LuaHook 单 P1-D 生命周期(§六):clear/exit 排空、迟到结果归属的基础
// 设施与资源回收次序。
//   ① in-flight 账:invocation 进出配对(MakeLuaHookHandler 的 Lua 路也记);
//   ② 排空窗口:BeginDrain 后新 invocation 不授 http/fs/tools(只留进程内
//      能力),在途束的工具桥对新的子执行回 hook.tool.drained;
//   ③ WaitForDrain:等在途归零(真线程里跑一只慢 handler,排空等它收口);
//   ④ 换场自动排空:BindMiddlewareSessionWriter 换写者前等 in-flight 归零
//      ——旧账封口前不被拆引用;
//   ⑤ 按需加载与资源回收的次序合同:per-invocation state 用完即弃、
//      服务束不跨调用(P1-C 已钉,这里补 drain 档的回归)。
// 热换(分代热换装/状态保留)不在本单——另见 todos/Lua插件分代热换装与
// 状态保留设计.todo。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/dispatcher.hpp"
#include "hooks/middleware.hpp"
#include "runtime/hook_host_services.hpp"
#include "runtime/middleware_runtime.hpp"
#include "runtime/middleware_v3_sink.hpp"
#include "runtime/plugin_lua_host.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode;
using namespace lubancode::hooks::middleware;
using namespace lubancode::runtime;
using namespace lubancode::trajectory::v3;

namespace {

class FixedClock final : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct V3Dir {
    FixedClock clock;
    std::filesystem::path dir;
    std::filesystem::path jsonl;
    std::optional<V3Writer> writer;

    explicit V3Dir(const char* tag) {
        dir = std::filesystem::temp_directory_path() / ("lubancode-mw-drain-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
        auto started = V3Writer::Start(jsonl, "20260917-100000-DR01", "run-000001", "你是 LubanCode。",
                                       nlohmann::json::object(), V3WriterOptions{}, &clock);
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }
};

class StubTool final : public tools::Tool {
public:
    std::string name() const override { return "stub.ping"; }
    std::string description() const override { return "stub"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++calls;
        return tools::Tool::Result::Text("pong");
    }
    int calls = 0;
};

MiddlewarePool::Options PoolWithCenter(HookHostServiceCenter& center) {
    MiddlewarePool::Options options;
    HookHostServiceCenter* center_ptr = &center;
    options.lua_factory = [center_ptr](const LuaHandlerSpec& spec, const HandlerLimits& limits) {
        return MakeLuaHookHandler(spec, limits, center_ptr);
    };
    return options;
}

std::expected<void, ManifestError> AddLuaHook(MiddlewarePool& pool, const std::string& hook_json,
                                              const std::string& script) {
    return pool.AddManifest(nlohmann::json::parse(hook_json), SourceLayer::User,
                            "user ~/.lubancode/hooks", script);
}

const char* kHttpHookJson = R"json({
  "schemaVersion": 1, "id": "drain-probe", "entry": "main.lua",
  "hooks": [{"hookPoint": "PostUser", "name": "probe.http", "handler": "run",
             "capabilities": ["http"]}]
})json";

// 造一只带服务中心的已发布 dispatcher(不带 v3 写者;drain 档不需要)。
struct WiredPool {
    std::shared_ptr<HookStateStore> state = std::make_shared<HookStateStore>();
    HookHostServiceCenter center;
    hooks::HookDispatcher wired;

    explicit WiredPool(const std::string& hook_json, const std::string& script,
                       HookHostServiceCenter::Grants extra_grants = {}) {
        HookHostServiceCenter::Grants grants;
        PluginHttpCallSpec http;  // transport 留空:只验证 granted 与否,不发真包
        grants.http = std::move(http);
        grants.state = state;
        grants.log = [](const std::string&, const nlohmann::json&) {};
        grants.tool_registry = extra_grants.tool_registry;
        grants.allow_tools = extra_grants.allow_tools;
        center.SetGrants(std::move(grants));
        MiddlewarePool pool(PoolWithCenter(center));
        REQUIRE(AddLuaHook(pool, hook_json, script).has_value());
        auto published = pool.Publish();
        REQUIRE(published.has_value());
        wired.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));
    }
};

}  // namespace

TEST_CASE("排空窗口:BeginDrain 后新 invocation 不授 http,工具桥拒新子执行") {
    // 脚本探针:prompt=probe:http 调 luban.http.request,把结局 code 透出去;
    // probe:state 试进程内能力是否还开。
    const std::string script = R"lua(
return {
  run = function(ctx, input, next)
    if input.prompt == "probe:http" then
      local resp, err = luban.http.request({method = "GET", url = "https://example.local/"})
      if resp == nil then
        return {output = {code = tostring(err.code)}}
      end
      return {output = {code = "no_error"}}
    end
    if input.prompt == "probe:state" then
      luban.state.set("seen", true)
      return {output = {code = "no_error"}}
    end
    return next(input)
  end,
}
)lua";
    WiredPool wired(kHttpHookJson, script);

    // 排空前:http 授权在(transport 未接线,报 network_failed 而不是
    // capability_not_granted——证明门口认了这份授权);state 写得进。
    {
        MiddlewareHookContext context;
        const PostUserAppend before = RunPostUserMiddleware(&wired.wired, "probe:http", context);
        REQUIRE(before.dispatched);
        REQUIRE(before.outcome.Ok());
        CHECK(before.outcome.value.at("code") == "network_failed");
        const PostUserAppend state_probe = RunPostUserMiddleware(&wired.wired, "probe:state", context);
        REQUIRE(state_probe.dispatched);
        CHECK(wired.state->Get("drain-probe", "seen").has_value());
    }

    // 排空窗口:新 invocation 的 http 一律 capability_not_granted;进程内的
    // state/log/context 留着(§六"取消后只准清理")。
    wired.center.BeginDrain("test_clear");
    CHECK(wired.center.draining());
    {
        MiddlewareHookContext context;
        const PostUserAppend drained = RunPostUserMiddleware(&wired.wired, "probe:http", context);
        REQUIRE(drained.dispatched);
        REQUIRE(drained.outcome.Ok());
        CHECK(drained.outcome.value.at("code") == "capability_not_granted");
        const InvocationRecord* record = drained.outcome.FindRecord("PostUser/probe.http");
        REQUIRE(record != nullptr);
        // handler 正常完成(探针不调 next,直接回值 = 短路;能力拒绝是
        // Host API 的错,不是 dispatch 失败)。
        CHECK(record->outcome == "completed_short_circuit");
        // state 仍可用(进程内能力排空窗口保留)。
        const PostUserAppend state_probe = RunPostUserMiddleware(&wired.wired, "probe:state", context);
        REQUIRE(state_probe.dispatched);
        CHECK(state_probe.outcome.value.at("code") == "no_error");
    }
    wired.center.EndDrain();
    CHECK_FALSE(wired.center.draining());
}

TEST_CASE("工具桥:排空旗置位后新的子执行回 hook.tool.drained,在途不受影响") {
    auto tool = std::make_unique<StubTool>();
    StubTool* tool_ptr = tool.get();
    tools::ToolRegistry registry;
    registry.Register(std::move(tool));
    HookToolExecutionService::Options options;
    options.registry = &registry;
    options.allow_tools = {"stub.ping"};
    std::atomic<bool> drain{false};
    options.drain = &drain;
    HookToolExecutionService service(std::move(options));

    const auto before = service.Call("stub.ping", nlohmann::json::object(), "h", "d", "i", std::nullopt,
                                     std::nullopt, std::nullopt, nullptr);
    REQUIRE(before.admitted);
    CHECK(before.status == "finished");

    drain.store(true);
    const auto after = service.Call("stub.ping", nlohmann::json::object(), "h", "d", "i", std::nullopt,
                                     std::nullopt, std::nullopt, nullptr);
    REQUIRE_FALSE(after.admitted);
    CHECK(after.status == "rejected");
    CHECK(after.error_code == "hook.tool.drained");
    CHECK(tool_ptr->calls == 1);  // 拒掉的没执行
}

TEST_CASE("in-flight:统一入口进出配对,WaitForMiddlewareDrain 等它归零") {
    // 慢 handler:门口放行,函数体里睡到旗放。经 Run* 统一入口派发——
    // in-flight 账记在 dispatcher(生产缝),直接调 Dispatch 的测试路不记。
    std::atomic<bool> release{false};
    MiddlewareDefinition slow;
    slow.point = HookPoint::PreUser;
    slow.name = "prompt.slow";
    slow.layer = SourceLayer::Builtin;
    slow.source_label = "builtin";
    slow.implementation_ref = "builtin.prompt_slow_v1";
    slow.builtin = [&release](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return HandlerReturn::Value(next().value);
    };
    MiddlewarePool pool;
    pool.AddDefinition(std::move(slow));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    hooks::HookDispatcher wired;
    wired.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));
    CHECK(wired.middleware_in_flight() == 0);

    std::thread worker([&] {
        MiddlewareHookContext context;
        (void)RunPreUserMiddleware(&wired, "慢一句", context);
    });
    // 等 dispatch 真进场(有界自旋,不赌瞬时)。
    for (int i = 0; i < 5000 && wired.middleware_in_flight() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(wired.middleware_in_flight() == 1);

    // 排空不等强拆:到点未归零回 false。
    CHECK_FALSE(wired.WaitForMiddlewareDrain(std::chrono::milliseconds(50)));
    release.store(true);
    worker.join();
    CHECK(wired.middleware_in_flight() == 0);
    CHECK(wired.WaitForMiddlewareDrain(std::chrono::milliseconds(50)));
}

TEST_CASE("服务中心账:Enter/Leave 配对,WaitForDrain 等归零") {
    HookHostServiceCenter center;
    center.EnterInvocation();
    center.EnterInvocation();
    CHECK(center.in_flight() == 2);
    CHECK_FALSE(center.WaitForDrain(std::chrono::milliseconds(20)));
    center.LeaveInvocation();
    CHECK(center.in_flight() == 1);
    center.LeaveInvocation();
    CHECK(center.in_flight() == 0);
    CHECK(center.WaitForDrain(std::chrono::milliseconds(20)));
    center.LeaveInvocation();  // 多退不越零
    CHECK(center.in_flight() == 0);
}

TEST_CASE("换场自动排空:BindMiddlewareSessionWriter 换写者前等 in-flight 归零") {
    V3Dir v3_a("switch-a");
    V3Dir v3_b("switch-b");
    HookHostServiceCenter center;

    std::atomic<bool> release{false};
    MiddlewareDefinition slow;
    slow.point = HookPoint::PreUser;
    slow.name = "prompt.slow";
    slow.layer = SourceLayer::Builtin;
    slow.source_label = "builtin";
    slow.implementation_ref = "builtin.prompt_slow_v1";
    slow.builtin = [&release](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return HandlerReturn::Value(next().value);
    };
    MiddlewarePool pool;
    pool.AddDefinition(std::move(slow));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    hooks::HookDispatcher wired;
    wired.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));
    BindMiddlewareSessionWriter(&wired, &center, &*v3_a.writer);

    std::thread worker([&] {
        MiddlewareHookContext context;
        const PreUserGate gate = RunPreUserMiddleware(&wired, "跨换场的一句", context);
        (void)gate;
    });
    for (int i = 0; i < 5000 && wired.middleware_in_flight() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(wired.middleware_in_flight() == 1);

    // 换绑在另一线程发起:排空没等到归零前不许换(在途 dispatch 持旧 sink
    // 的裸指针,不许拆它的引用)。
    std::atomic<bool> rebound{false};
    std::thread switcher([&] {
        BindMiddlewareSessionWriter(&wired, &center, &*v3_b.writer);
        rebound.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK_FALSE(rebound.load());  // 还在排空:慢 handler 没收口,写者没换
    release.store(true);
    worker.join();
    switcher.join();
    CHECK(rebound.load());  // 归零后换绑完成
    CHECK(wired.middleware_in_flight() == 0);

    // 事件归属:跨场 dispatch 的事件落旧写者(hook.started 在 a 不在 b)。
    {
        std::ifstream file_a(v3_a.jsonl, std::ios::binary);
        std::string line;
        int started_a = 0;
        while (std::getline(file_a, line)) {
            if (line.find("\"hook.started\"") != std::string::npos) ++started_a;
        }
        CHECK(started_a >= 1);
        std::ifstream file_b(v3_b.jsonl, std::ios::binary);
        int started_b = 0;
        while (std::getline(file_b, line)) {
            if (line.find("\"hook.started\"") != std::string::npos) ++started_b;
        }
        CHECK(started_b == 0);
    }
}
