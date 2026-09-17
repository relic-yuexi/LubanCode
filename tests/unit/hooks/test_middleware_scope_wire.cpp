// LuaHook 单 P0-B 遗留最后一笔(P1-D 补验):fork/btw 的钩子面行为。
// steer/followup、compact 切槽、首行 system、Esc interrupted、输出预留五笔
// 已钉在 test_middleware_runtime_wire.cpp;这里补第六笔——
//   ① purpose 隔离:purpose=btw/title 的派发不命中 interactive 专属钩子
//      (记 skipped_no_match),通用钩子照常;消息钩子按真实 origin/purpose
//      触发,不扫描 wire role 猜来源(§7.2:btw/title/compact 各走各的
//      purpose,不混跑);
//   ② fork 形状:新 session 的 v3 文件行 1 仍是完整 system;换场后 hook
//      事件只落新文件,旧账一行不多;
//   ③ 迟到结果归属:子执行账钉在 invocation 起点那只写者上——工具执行
//      中途换绑写者,tool.execution.* 终态仍落发起会话,新 session 不沾。
// 注:§4.57/§4.58 的 fork/btw 会话运行时(旁支排队/检查点复制)在 v3 父单
// 余项里,尚未实现;本册钉的是钩子层已冻结的隔离与归属合同,不是宣称
// /fork、/btw 已能用。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/dispatcher.hpp"
#include "hooks/middleware.hpp"
#include "runtime/hook_host_services.hpp"
#include "runtime/middleware_runtime.hpp"
#include "runtime/middleware_v3_sink.hpp"
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

    explicit V3Dir(const char* tag, const std::string& system = "你是 LubanCode。") {
        dir = std::filesystem::temp_directory_path() / ("lubancode-mw-scope-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
        auto started = V3Writer::Start(jsonl, "20260917-090000-SC01", "run-000001", system,
                                       nlohmann::json::object(), V3WriterOptions{}, &clock);
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }
};

std::vector<nlohmann::json> ReadEvents(const std::filesystem::path& path,
                                       const std::string& kind_prefix = std::string()) {
    std::ifstream file(path, std::ios::binary);
    std::vector<nlohmann::json> events;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (parsed.value("type", "") != "event") continue;
        const std::string kind = parsed.value("kind", "");
        if (kind.rfind(kind_prefix, 0) == 0) {
            events.push_back(parsed);
        }
    }
    return events;
}

MiddlewareDefinition PassDef(HookPoint point, const std::string& name) {
    MiddlewareDefinition def;
    def.point = point;
    def.name = name;
    def.layer = SourceLayer::Builtin;
    def.source_label = "builtin";
    def.implementation_ref = "builtin." + name;
    def.builtin = [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
        return HandlerReturn::Value(next().value);
    };
    return def;
}

// 中途换绑写者的 fake 工具:execute() 里先回调(把服务中心的子执行写者
// 切到新 session),再返回——模拟"执行跨过换场点,终态迟到"。
class SwitchingFakeTool final : public tools::Tool {
public:
    std::function<void()> on_execute;
    std::string name() const override { return "kb.search"; }
    std::string description() const override { return "fixture"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&, const tools::ToolExecutionContext&) override {
        if (on_execute) {
            on_execute();
        }
        return tools::Tool::Result::Text("迟到的检索结果");
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// ① purpose 隔离:btw/title 不命中 interactive 专属钩子
// ---------------------------------------------------------------------------

TEST_CASE("btw:purpose=btw 的派发不混跑 interactive 专属钩子,通用钩子照常") {
    V3Dir v3("btw");
    MiddlewarePool pool;
    {
        MiddlewareDefinition interactive_only = PassDef(HookPoint::PreUser, "memory.inject_interactive");
        interactive_only.match.purpose = "interactive";
        MiddlewareDefinition generic = PassDef(HookPoint::PreUser, "audit.generic");
        pool.AddDefinition(std::move(interactive_only));
        pool.AddDefinition(std::move(generic));
    }
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    hooks::HookDispatcher dispatcher;
    dispatcher.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));
    BindMiddlewareSessionWriter(&dispatcher, nullptr, &*v3.writer);

    MiddlewareHookContext context;
    context.origin = "human";
    context.purpose = "btw";  // 旁问:自己的 purpose,不借 interactive 的钩子
    const PreUserGate gate = RunPreUserMiddleware(&dispatcher, "主线忙时旁问一句", context);
    REQUIRE(gate.dispatched);
    REQUIRE(gate.outcome.Ok());
    CHECK(gate.outcome.FindRecord("PreUser/memory.inject_interactive")->outcome == "skipped_no_match");
    CHECK(gate.outcome.FindRecord("PreUser/audit.generic")->outcome == "completed");
    CHECK(gate.prompt == "主线忙时旁问一句");  // 无改写,旁支材料不进 main

    // title 同口径:生成标题的内部请求不触发 interactive 注入。
    MiddlewareHookContext title_context;
    title_context.origin = "session_runtime";
    title_context.purpose = "title";
    const PreUserGate title_gate = RunPreUserMiddleware(&dispatcher, "给这场起个标题", title_context);
    REQUIRE(title_gate.dispatched);
    CHECK(title_gate.outcome.FindRecord("PreUser/memory.inject_interactive")->outcome == "skipped_no_match");
    CHECK(title_gate.outcome.FindRecord("PreUser/audit.generic")->outcome == "completed");

    // 账上只有真正跑过的 invocation:requested 的 matchedHandlers 不含未命中项。
    const auto requested = ReadEvents(v3.jsonl, "hook.dispatch.requested");
    REQUIRE_FALSE(requested.empty());
    for (const auto& event : requested) {
        for (const auto& matched : event.at("payload").at("matchedHandlers")) {
            CHECK(matched.at("hookId") != "PreUser/memory.inject_interactive");
        }
    }
}

// ---------------------------------------------------------------------------
// ② fork 形状:新 session 首行完整 system;换场后事件只落新账
// ---------------------------------------------------------------------------

TEST_CASE("fork 形状:新 session 行 1 是完整 system,换场后 hook 事件只落新文件") {
    V3Dir v3_main("fork-main");
    MiddlewarePool pool;
    pool.AddDefinition(PassDef(HookPoint::PostUser, "ctx.observe"));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    hooks::HookDispatcher dispatcher;
    dispatcher.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));
    BindMiddlewareSessionWriter(&dispatcher, nullptr, &*v3_main.writer);

    MiddlewareHookContext context;
    const PostUserAppend first = RunPostUserMiddleware(&dispatcher, "源会话的一句", context);
    REQUIRE(first.dispatched);
    const std::size_t main_events_after_first = ReadEvents(v3_main.jsonl, "hook.").size();
    REQUIRE(main_events_after_first > 0);

    // fork 开新卷:首行仍是完整 system(不是半截,不带旧链)。
    V3Dir v3_fork("fork-branch", "分叉后的完整系统提示,一字不少。");
    {
        std::ifstream file(v3_fork.jsonl, std::ios::binary);
        std::string first_line;
        std::getline(file, first_line);
        const nlohmann::json parsed = nlohmann::json::parse(first_line);
        REQUIRE(parsed.value("type", "") == "message");
        CHECK(parsed.at("message").at("role") == "system");
        CHECK(parsed.at("message").at("content") == "分叉后的完整系统提示,一字不少。");
    }

    // 换场:事件落新文件;源会话的账一行不多(晚到结果不写进新 session,
    // 新 session 的事件也不回流旧账)。
    BindMiddlewareSessionWriter(&dispatcher, nullptr, &*v3_fork.writer);
    const PostUserAppend fork_turn = RunPostUserMiddleware(&dispatcher, "新分支的第一句", context);
    REQUIRE(fork_turn.dispatched);
    CHECK(ReadEvents(v3_fork.jsonl, "hook.").size() > 0);
    CHECK(ReadEvents(v3_main.jsonl, "hook.").size() == main_events_after_first);
    CHECK(VerifyV3File(v3_main.jsonl).ok);
    CHECK(VerifyV3File(v3_fork.jsonl).ok);
}

// ---------------------------------------------------------------------------
// ③ 迟到结果归属:子执行终态落发起会话
// ---------------------------------------------------------------------------

TEST_CASE("迟到归属:工具执行中途换绑写者,tool.execution.* 终态仍落发起会话") {
    V3Dir v3_main("late-main");
    V3Dir v3_next("late-next");
    HookHostServiceCenter center;

    SwitchingFakeTool tool;
    tool.on_execute = [&center, &v3_next] {
        // 执行中途换场:服务中心的子执行写者切到新 session(生产里这是
        // clear/resume 的换绑;这里直接逼出竞态窗口)。
        center.SetSubExecutionWriter(&*v3_next.writer);
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<SwitchingFakeTool>(std::move(tool)));
    HookHostServiceCenter::Grants grants;
    grants.tool_registry = &registry;
    grants.allow_tools = {"kb.search"};
    center.SetGrants(std::move(grants));
    center.SetSubExecutionWriter(&*v3_main.writer);

    InvocationCtx ctx;
    ctx.dispatch_id = "dispatch-late-1";
    ctx.invocation_id = "dispatch-late-1#0";
    ctx.hook_id = "PostUser/knowledge.recall";
    ctx.point = HookPoint::PostUser;
    auto services = center.Build({std::string(kHookCapTools)}, ctx, "knowledge-recall");
    REQUIRE(services != nullptr);
    REQUIRE(services->tools != nullptr);

    const auto result = services->tools->Call("kb.search", nlohmann::json{{"query", "分代热换"}},
                                              "PostUser/knowledge.recall", "dispatch-late-1",
                                              "dispatch-late-1#0", std::nullopt, "turn-000001",
                                              "step-000001", nullptr);
    REQUIRE(result.admitted);
    CHECK(result.status == "finished");

    // 终态全数落发起会话(pending/started/finished 都在 v3_main);
    // 新 session 的文件里只有它自己的首行 system + session.started。
    const auto sub_events = ReadEvents(v3_main.jsonl, "tool.execution.");
    bool saw_started = false;
    bool saw_finished = false;
    for (const auto& event : sub_events) {
        const std::string kind = event.at("kind");
        if (kind == "tool.execution.started") saw_started = true;
        if (kind == "tool.execution.finished") saw_finished = true;
    }
    CHECK(saw_started);
    CHECK(saw_finished);
    CHECK(ReadEvents(v3_next.jsonl, "tool.execution.").empty());
    CHECK(ReadEvents(v3_next.jsonl, "hook.").empty());
    CHECK(VerifyV3File(v3_main.jsonl).ok);
    CHECK(VerifyV3File(v3_next.jsonl).ok);
}
