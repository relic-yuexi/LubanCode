// LuaHook 单 P1-C 第 4 条 + 验收:Lua handler 经宿主工具桥调已注册 MCP 工具
//(本地桩服务 = FakeTransport 的进程内 MCP server),召回外部资料并追加上
// 下文——每个外部调用(tool.execution.* + hook 联链)与采用效果(context
// 候选入账)都可追溯;无旁路 MCP Client、无裸文件写口。故障矩阵:超时/
// 取消/断连/已执行但响应丢失 -> unknown/cancelled + executionUncertain,
// 不自动重试副作用(桩服务恰收一笔 tools/call)。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"
#include "mcp/client.hpp"
#include "mcp/mcp_tool.hpp"
#include "runtime/hook_host_services.hpp"
#include "runtime/middleware_v3_sink.hpp"
#include "runtime/plugin_lua_host.hpp"
#include "tools/registry.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode;
using namespace lubancode::hooks::middleware;
using namespace lubancode::runtime;
using namespace lubancode::mcp;
using namespace lubancode::trajectory::v3;

namespace {

class FixedClock final : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

// 进程内假 MCP 服务(不依赖外网、不起子进程):transport 收请求,按编排
// 回应。Hang = 收下不回(慢服务/响应丢失);Kill = 传输判死(断连)。
class FakeMcpServer {
public:
    enum class Mode { Respond, Hang };

    class Transport final : public mcp::Transport {
    public:
        std::function<void(const std::string&)> on_write;
        std::atomic<bool> alive{true};

        bool WriteLine(const std::string& line) override {
            if (on_write) {
                on_write(line);
            }
            return true;
        }
        void Shutdown(int) override { alive = false; }
        bool IsAlive() const override { return alive; }
        std::string StderrTail() const override { return std::string(); }
    };

    explicit FakeMcpServer(Mode mode = Mode::Respond) : mode_(mode) {
        transport_.on_write = [this](const std::string& line) {
            const nlohmann::json request = nlohmann::json::parse(line);
            if (request.at("method") != "tools/call") {
                return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            call_requests_.push_back(request);
            if (mode_ == Mode::Respond) {
                RespondLocked(request);
            }
        };
    }

    // 按原 id 回 tools/call 成功响应(迟到补发也走这里)。
    void RespondToCall(const nlohmann::json& request) {
        const nlohmann::json response = {
            {"jsonrpc", "2.0"},
            {"id", request.at("id")},
            {"result", {{"content", nlohmann::json::array({{{"type", "text"}, {"text", response_text_}}})},
                        {"isError", false}}}};
        client_->OnLine(response.dump());
    }

    void Kill() { transport_.alive = false; }  // 断连:进程死/管道断
    void set_mode(Mode mode) { mode_ = mode; }
    void set_response_text(std::string text) { response_text_ = std::move(text); }

    std::size_t calls() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return call_requests_.size();
    }
    std::vector<nlohmann::json> call_requests() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return call_requests_;
    }

    void Bind(Client& client) {
        client_ = &client;
        client.AttachTransportForTest(&transport_);
    }

private:
    void RespondLocked(const nlohmann::json& request) { RespondToCall(request); }

    Mode mode_;
    Transport transport_;
    mutable std::mutex mutex_;
    std::vector<nlohmann::json> call_requests_;
    std::string response_text_ = "知识库命中:外部资料甲(来自桩服务)";
    Client* client_ = nullptr;
};

std::vector<nlohmann::json> ReadKind(const std::filesystem::path& path, const std::string& kind) {
    std::ifstream file(path, std::ios::binary);
    std::vector<nlohmann::json> events;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (parsed.value("type", "") == "event" && parsed.value("kind", "") == kind) {
            events.push_back(parsed);
        }
    }
    return events;
}

struct V3Dir {
    FixedClock clock;
    std::filesystem::path dir;
    std::filesystem::path jsonl;
    std::optional<V3Writer> writer;

    explicit V3Dir(const char* tag) {
        dir = std::filesystem::temp_directory_path() / ("lubancode-hookmcp-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
        auto started = V3Writer::Start(jsonl, "20260911-130000-MB01", "run-000001", "你是 LubanCode。",
                                       nlohmann::json::object(), V3WriterOptions{}, &clock);
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }
};

// Lua 桥接脚本:调 mcp__kb__search,finished 且非错时 context.append 召回
// 材料;status 原样带出。
const char* kBridgeScript = R"lua(return {
      run = function(ctx, input, next)
        local r, err = luban.tools.call("mcp__kb__search", { query = "外部资料" })
        if r == nil then
          return { output = { rejected = true, code = err.code } }
        end
        local appended = true
        if r.status == "finished" and not r.isError then
          appended = select(1, luban.context.append("召回材料: " .. r.content, "kb"))
        end
        return { output = { status = r.status, is_error = r.isError, exec = r.executionId,
                            appended = appended } }
      end
    })lua";

const char* kBridgeManifest = R"({"schemaVersion":1,"id":"kb-hook","entry":"main.lua","hooks":[
    {"hookPoint":"PostUser","name":"kb.recall","handler":"run",
     "capabilities":["tools","context"]}]})";

// 端到端装配:Lua hook(tools+context)→ 工具桥 → McpTool over 假服务,
// 子执行账绑 v3 主写者。
struct McpBridgeHarness {
    FakeMcpServer server;
    Client client{"kb"};
    tools::ToolRegistry registry;
    V3Dir v3;
    HookHostServiceCenter center;

    explicit McpBridgeHarness(const char* tag, FakeMcpServer::Mode mode = FakeMcpServer::Mode::Respond)
        : server(mode), v3(tag) {
        server.Bind(client);
        client.SetTimeoutsForTest(/*default_timeout_ms=*/200, /*tool_call_timeout_ms=*/250);

        ToolInfo info;
        info.name = "search";
        info.description = "知识库检索";
        info.input_schema = nlohmann::json::parse(
            R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})");
        tools::ToolRegistration registration;
        registration.tool = std::make_unique<McpTool>(client, "kb", info);
        registration.source_kind = tools::ToolSourceKind::Mcp;
        registration.source_instance = "kb";
        registration.version_or_digest = "stub-1";
        registry.Register(std::move(registration));

        HookHostServiceCenter::Grants grants;
        grants.tool_registry = &registry;
        grants.allow_tools = {"mcp__kb__search"};
        grants.state = std::make_shared<HookStateStore>();
        center.SetGrants(std::move(grants));
        center.SetSubExecutionWriter(&*v3.writer);
    }

    DispatchOutcome DispatchPostUser(const std::atomic<bool>* cancel = nullptr) {
        MiddlewarePool::Options options;
        HookHostServiceCenter* center_ptr = &center;
        options.lua_factory = [center_ptr](const LuaHandlerSpec& spec, const HandlerLimits& limits) {
            return MakeLuaHookHandler(spec, limits, center_ptr);
        };
        MiddlewarePool pool(std::move(options));
        REQUIRE(pool.AddManifest(nlohmann::json::parse(kBridgeManifest), SourceLayer::User,
                                 "user ~/.lubancode/hooks", kBridgeScript)
                    .has_value());
        auto published = pool.Publish();
        REQUIRE(published.has_value());
        // 与生产同款通电:核挂 HookDispatcher,sink 槽绑本场 v3 主写者
        //(一个 writer,§7.1)。
        host.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));
        BindMiddlewareSessionWriter(&host, nullptr, &*v3.writer);
        MiddlewareDispatcher* core = host.middleware();
        REQUIRE(core != nullptr);
        DispatchTrigger trigger;
        trigger.input = {{"prompt", "查一下外部资料"}};
        trigger.origin = "human";
        trigger.purpose = "interactive";
        trigger.cancel = cancel;
        // Run*Middleware 的同款取槽:显式递 sink 槽位(事件账通电)。
        return core->Dispatch(HookPoint::PostUser, trigger,
                              [](const nlohmann::json& input) { return input; },
                              host.middleware_sink());
    }

    hooks::HookDispatcher host;  // 生产同款容器:sink 槽 + 中间件核
};

}  // namespace

// ---------------------------------------------------------------------------
// 验收场景:召回外部资料并追加上下文,全链可追溯
// ---------------------------------------------------------------------------

TEST_CASE("验收:Lua 调 MCP 召回并 context.append,调用与采用效果都可追溯") {
    McpBridgeHarness harness("accept");
    const DispatchOutcome outcome = harness.DispatchPostUser();

    REQUIRE(outcome.Ok());
    CHECK(outcome.value.at("status") == "finished");
    CHECK(outcome.value.at("appended") == true);
    // 采用效果:PostUser 的 context.append 恰一条(候选来自工具结果正文)。
    REQUIRE(outcome.context_appends.size() == 1);
    CHECK(outcome.context_appends.at(0).rfind("召回材料: 知识库命中", 0) == 0);

    // 外部调用可追溯:tool.execution.pending/started/finished 同 actionId,
    // linkage 带 hookInvocationId/hookDispatchId/logicalTool/backend=mcp:kb。
    const auto pending = ReadKind(harness.v3.jsonl, "tool.execution.pending");
    const auto started = ReadKind(harness.v3.jsonl, "tool.execution.started");
    const auto finished = ReadKind(harness.v3.jsonl, "tool.execution.finished");
    REQUIRE(pending.size() == 1);
    REQUIRE(started.size() == 1);
    REQUIRE(finished.size() == 1);
    const nlohmann::json& linkage = pending.at(0).at("payload");
    CHECK(linkage.at("hookInvocationId") != "");
    CHECK(linkage.at("hookDispatchId") != "");
    CHECK(linkage.at("hookId") == "PostUser/kb.recall");
    CHECK(linkage.at("logicalTool") == "mcp__kb__search");
    CHECK(linkage.at("backend") == "mcp:kb");
    CHECK(linkage.at("reason") == "hook_subexecution");
    CHECK(pending.at(0).at("actionId") == started.at(0).at("actionId"));
    CHECK(pending.at(0).at("actionId") == finished.at(0).at("actionId"));

    // 无旁路:桩服务恰收一笔 tools/call,转发的是 MCP 原始工具名与参数
    //(Lua 没有第二条直连 mcp::Client 的路——它只有 luban.tools.call)。
    const std::vector<nlohmann::json> mcp_requests = harness.server.call_requests();
    REQUIRE(mcp_requests.size() == 1);
    const nlohmann::json& mcp_request = mcp_requests.at(0);
    CHECK(mcp_request.at("params").at("name") == "search");
    CHECK(mcp_request.at("params").at("arguments").at("query") == "外部资料");

    // 采用效果可追溯:hook.effects.applied(context.append)落在同一份 v3
    // 账上,带 hookInvocationId——外部调用与采用效果两笔账可联查。
    const auto applied = ReadKind(harness.v3.jsonl, "hook.effects.applied");
    REQUIRE(applied.size() == 1);
    CHECK(applied.at(0).at("payload").at("effectType") == "context.append");
    CHECK(applied.at(0).at("payload").at("hookInvocationId") ==
          linkage.at("hookInvocationId"));
}

// ---------------------------------------------------------------------------
// 故障矩阵(第 4 条)
// ---------------------------------------------------------------------------

TEST_CASE("超时:服务不响应 -> unknown + uncertain,不自动重试(恰一笔请求)") {
    McpBridgeHarness harness("timeout", FakeMcpServer::Mode::Hang);
    const DispatchOutcome outcome = harness.DispatchPostUser();
    REQUIRE(outcome.Ok());
    CHECK(outcome.value.at("status") == "unknown");
    // 不自动重试:桩服务恰收一笔 tools/call;结果未明不追加上下文。
    CHECK(harness.server.calls() == 1);
    CHECK(outcome.context_appends.empty());
    // 账:pending + started + unknown(缺响应不证明未执行,§5.1)。
    CHECK(ReadKind(harness.v3.jsonl, "tool.execution.unknown").size() == 1);
    CHECK(ReadKind(harness.v3.jsonl, "tool.execution.pending").size() == 1);
    CHECK(ReadKind(harness.v3.jsonl, "tool.execution.started").size() == 1);
}

TEST_CASE("取消:Esc 旗进工具桥 -> MCP notifications/cancelled + cancelled 终态,不重试") {
    // 服务级直调(执行核在帧边界就拦预置取消旗,轮内取消链路归 wire 册;
    // 这里钉 Lua 工具桥 → McpTool → Client 的取消贯通)。
    McpBridgeHarness harness("cancel", FakeMcpServer::Mode::Hang);
    HookToolExecutionService::Options tool_options;
    tool_options.registry = &harness.registry;
    tool_options.allow_tools = {"mcp__kb__search"};
    tool_options.ledger = std::make_shared<V3HookSubExecutionLedger>(*harness.v3.writer);
    HookToolExecutionService service(std::move(tool_options));

    std::atomic<bool> cancelled{true};  // Esc 已按
    const auto result = service.Call("mcp__kb__search", {{"query", "外部资料"}}, "PostUser/kb.recall",
                                     "hookmw_c", "hookmw_c#0", std::nullopt, std::nullopt,
                                     std::nullopt, &cancelled);
    REQUIRE(result.admitted);
    CHECK(result.status == "cancelled");
    CHECK(result.record.at("executionUncertain") == true);
    CHECK(harness.server.calls() == 1);  // 恰一笔;取消不是重发
    CHECK(ReadKind(harness.v3.jsonl, "tool.execution.cancelled").size() == 1);
}

TEST_CASE("断连:传输判死 -> unknown(请求可能已到远端),不重试") {
    McpBridgeHarness harness("dead", FakeMcpServer::Mode::Hang);
    harness.server.Kill();  // 写请求前传输已死:进程崩/管道断的形状
    const DispatchOutcome outcome = harness.DispatchPostUser();
    REQUIRE(outcome.Ok());
    CHECK(outcome.value.at("status") == "unknown");
    CHECK(harness.server.calls() == 1);  // WriteLine 仍记账一笔(请求已出门)
    CHECK(ReadKind(harness.v3.jsonl, "tool.execution.unknown").size() == 1);
}

TEST_CASE("已执行但响应丢失:迟到响应不翻案;下一笔照常配对") {
    McpBridgeHarness harness("late", FakeMcpServer::Mode::Hang);
    // 第一笔:挂起 -> 超时收口 unknown(远端可能已执行)。
    const DispatchOutcome first = harness.DispatchPostUser();
    REQUIRE(first.Ok());
    CHECK(first.value.at("status") == "unknown");
    REQUIRE(harness.server.call_requests().size() == 1);

    // 迟到响应(原 id)补进传输:不得翻案(不新增/不改写 tool.execution.*),
    // 也不得串进下一笔。
    harness.server.RespondToCall(harness.server.call_requests().at(0));
    CHECK(ReadKind(harness.v3.jsonl, "tool.execution.unknown").size() == 1);
    CHECK(ReadKind(harness.v3.jsonl, "tool.execution.finished").empty());

    // 第二笔:服务改应答,新 id 正位配对 -> finished;两笔互不污染。
    harness.server.set_mode(FakeMcpServer::Mode::Respond);
    const DispatchOutcome second = harness.DispatchPostUser();
    REQUIRE(second.Ok());
    CHECK(second.value.at("status") == "finished");
    CHECK(harness.server.calls() == 2);
    CHECK(ReadKind(harness.v3.jsonl, "tool.execution.finished").size() == 1);
    CHECK(ReadKind(harness.v3.jsonl, "tool.execution.unknown").size() == 1);
}
