// LuaHook 单 P1-C 第 3 条:统一工具执行服务(hook 子执行路)。准入(名单/
// 注册表现存/schema 复验)、子执行 v3 记账(tool.execution.* 全链 + hook
// 联链)、递归与重复治理、终态分型(finished/failed/cancelled/unknown)、
// 不自动重试副作用。模型 Action 路(agent::RunOneTool)同底座不同身份——
// 这里钉的是 hook 这半边的合同(§5.1/§4.49)。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/hook_host_services.hpp"
#include "tools/registry.hpp"
#include "tools/schema_check.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode;
using namespace lubancode::runtime;
using namespace lubancode::trajectory::v3;

namespace {

class FixedClock final : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct LedgerDir {
    FixedClock clock;
    std::filesystem::path dir;
    std::filesystem::path jsonl;
    std::optional<V3Writer> writer;

    explicit LedgerDir(const char* tag) {
        dir = std::filesystem::temp_directory_path() / ("lubancode-hooktool-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
        auto started = V3Writer::Start(jsonl, "20260911-120000-HT01", "run-000001", "你是 LubanCode。",
                                       nlohmann::json::object(), V3WriterOptions{}, &clock);
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }
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

// 测试用工具:脚本化结果(计数调用次数,可返回指定 outcome/error_code)。
class StubTool final : public tools::Tool {
public:
    struct Behavior {
        int call_limit = 1;
        tools::Tool::Result result = tools::Tool::Result::Text("ok");
    };

    StubTool(std::string tool_name, Behavior behavior)
        : name_(std::move(tool_name)), behavior_(std::move(behavior)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "测试桩:" + name_; }
    nlohmann::json input_schema() const override {
        return nlohmann::json::parse(R"({"type":"object","properties":{"q":{"type":"string"}}})");
    }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++calls;
        if (calls > behavior_.call_limit) {
            return behavior_.result;  // 超限的调用(本测试里出现即"又跑了一次")
        }
        return behavior_.result;
    }

    int calls = 0;

private:
    std::string name_;
    Behavior behavior_;
};

// 终态五型 + 记账的公共装配。
struct BridgeHarness {
    tools::ToolRegistry registry;
    LedgerDir ledger;
    std::shared_ptr<V3HookSubExecutionLedger> v3_ledger;
    HookToolExecutionService::Options options;
    std::unique_ptr<HookToolExecutionService> service;
    StubTool* tool = nullptr;  // 注册进表的那枚(数调用次数)

    explicit BridgeHarness(const char* tag, StubTool::Behavior behavior = {})
        : ledger(tag), v3_ledger(std::make_shared<V3HookSubExecutionLedger>(*ledger.writer)) {
        auto stub = std::make_unique<StubTool>("kb_lookup", std::move(behavior));
        tool = stub.get();
        registry.Register(tools::ToolRegistration{std::move(stub), tools::ToolSourceKind::Builtin,
                                                  "builtin", "v1"});
        options.registry = &registry;
        options.allow_tools = {"kb_lookup"};
        options.ledger = v3_ledger;
        service = std::make_unique<HookToolExecutionService>(options);
    }

    HookToolExecutionService::Result Call(const char* tool_name = "kb_lookup",
                                          const nlohmann::json& input = {{"q", "外部资料"}}) {
        return service->Call(tool_name, input, "PostUser/kb.recall", "hookmw_d1", "hookmw_d1#0",
                             std::nullopt, std::nullopt, std::nullopt, nullptr);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 准入
// ---------------------------------------------------------------------------

TEST_CASE("准入:未接注册表/未注册/不在名单/参数不合 schema 都拒,且入账 rejected") {
    // 未接注册表。
    {
        HookToolExecutionService::Options options;
        options.registry = nullptr;
        HookToolExecutionService service(options);
        const auto result = service.Call("x", nlohmann::json::object(), "h", "d", "i", std::nullopt,
                                         std::nullopt, std::nullopt, nullptr);
        REQUIRE_FALSE(result.admitted);
        CHECK(result.status == "rejected");
        CHECK(result.error_code == "hook.tool.registry_missing");
    }
    LedgerDir ledger("admit");
    tools::ToolRegistry registry;
    auto stub = std::make_unique<StubTool>("kb_lookup", StubTool::Behavior{});
    StubTool* stub_ptr = stub.get();
    registry.Register(std::move(stub));

    // 未在名单(名单空 = 一律拒):发现不等于授权。
    {
        HookToolExecutionService::Options opts;
        opts.registry = &registry;
        opts.ledger = std::make_shared<V3HookSubExecutionLedger>(*ledger.writer);
        HookToolExecutionService service(opts);
        const auto result = service.Call("kb_lookup", nlohmann::json::object(), "h", "d", "i",
                                         std::nullopt, std::nullopt, std::nullopt, nullptr);
        REQUIRE_FALSE(result.admitted);
        CHECK(result.error_code == "hook.tool.not_allowed");
        CHECK(stub_ptr->calls == 0);
    }
    // 名单在而工具未注册。
    {
        HookToolExecutionService::Options opts;
        opts.registry = &registry;
        opts.allow_tools = {"ghost"};
        opts.ledger = std::make_shared<V3HookSubExecutionLedger>(*ledger.writer);
        HookToolExecutionService service(opts);
        const auto result = service.Call("ghost", nlohmann::json::object(), "h", "d", "i",
                                         std::nullopt, std::nullopt, std::nullopt, nullptr);
        REQUIRE_FALSE(result.admitted);
        CHECK(result.error_code == "hook.tool.unknown");
    }
    // 名单在而参数不合 schema(q 须是字符串)。
    {
        HookToolExecutionService::Options opts;
        opts.registry = &registry;
        opts.allow_tools = {"kb_lookup"};
        opts.ledger = std::make_shared<V3HookSubExecutionLedger>(*ledger.writer);
        HookToolExecutionService service(opts);
        const auto result = service.Call("kb_lookup", nlohmann::json{{"q", 42}}, "h", "d", "i",
                                         std::nullopt, std::nullopt, std::nullopt, nullptr);
        REQUIRE_FALSE(result.admitted);
        CHECK(result.error_code == "hook.tool.invalid_arguments");
        CHECK(stub_ptr->calls == 0);
    }
    // rejected 也入账:pending(reason=hook_subexecution)+ rejected,无 started。
    const auto pending = ReadKind(ledger.jsonl, "tool.execution.pending");
    const auto rejected = ReadKind(ledger.jsonl, "tool.execution.rejected");
    const auto started = ReadKind(ledger.jsonl, "tool.execution.started");
    REQUIRE(rejected.size() >= 3);  // not_allowed + unknown + invalid_arguments
    CHECK(pending.size() == rejected.size());
    CHECK(started.empty());
    CHECK(pending.at(0).at("payload").at("reason") == "hook_subexecution");
}

// ---------------------------------------------------------------------------
// 子执行记账(v3 §4.49)
// ---------------------------------------------------------------------------

TEST_CASE("记账:finished 全链 pending→started→finished,linkage 带 hook 身份") {
    BridgeHarness harness("record");
    const auto result = harness.Call();
    REQUIRE(result.admitted);
    CHECK(result.status == "finished");
    REQUIRE_FALSE(result.execution_id.empty());

    const auto pending = ReadKind(harness.ledger.jsonl, "tool.execution.pending");
    const auto started = ReadKind(harness.ledger.jsonl, "tool.execution.started");
    const auto finished = ReadKind(harness.ledger.jsonl, "tool.execution.finished");
    REQUIRE(pending.size() == 1);
    REQUIRE(started.size() == 1);
    REQUIRE(finished.size() == 1);
    // 独立 executionId:三者同 actionId,且与返回给 Lua 的 executionId 一致。
    CHECK(pending.at(0).at("actionId") == result.execution_id);
    CHECK(started.at(0).at("actionId") == result.execution_id);
    // linkage(§4.49):parentHookInvocationId/hookDispatchId/实际工具与后端。
    const nlohmann::json& payload = pending.at(0).at("payload");
    CHECK(payload.at("hookInvocationId") == "hookmw_d1#0");
    CHECK(payload.at("hookDispatchId") == "hookmw_d1");
    CHECK(payload.at("hookId") == "PostUser/kb.recall");
    CHECK(payload.at("logicalTool") == "kb_lookup");
    CHECK(payload.at("backend") == "builtin");
    CHECK(payload.at("source") == "hook_subexecution");
    // 等待与参数:started 带 effectiveArgsRef;参数进了 record。
    CHECK(started.at(0).at("payload").contains("effectiveArgsRef"));
    CHECK(result.record.at("input").at("q") == "外部资料");
    CHECK(result.record.at("terminal") == "finished");
    CHECK(result.record.at("executionId") == result.execution_id);
    // 结果引用:正文摘要进 record(全文不进事件账)。
    CHECK(result.record.at("resultSummary").at("content") == "ok");
}

// ---------------------------------------------------------------------------
// 终态分型(§5.1:超时/断连/取消不证明远端未执行 -> unknown;不自动重试)
// ---------------------------------------------------------------------------

TEST_CASE("终态:timeout -> unknown + uncertain,不自动重试(执行恰一次)") {
    StubTool::Behavior behavior;
    behavior.result = tools::Tool::Result::Error("等待超时");
    behavior.result.outcome = "timed_out";
    behavior.result.error_code = "mcp.timeout";
    BridgeHarness harness("timeout", std::move(behavior));
    const auto first = harness.Call();
    const auto second = harness.Call();  // 再调同名工具:_repeat_cap 治理,不是重试
    REQUIRE(first.admitted);
    CHECK(first.status == "unknown");
    CHECK(first.record.at("executionUncertain") == true);
    CHECK(first.record.at("terminal") == "unknown");
    CHECK(first.error_code == "mcp.timeout");
    // 执行恰一次:第二次是新的调用决定(repeat cap 放行 2<3),不是宿主重试。
    CHECK(harness.tool->calls == 2);
    const auto unknown = ReadKind(harness.ledger.jsonl, "tool.execution.unknown");
    REQUIRE(unknown.size() == 2);
    CHECK(unknown.at(0).at("payload").at("reason") == "mcp.timeout");
}

TEST_CASE("终型:cancelled -> cancelled + uncertain;tool_error -> finished(执行已发生)") {
    // 取消旗观察路:旗置位 + is_error。
    {
        StubTool::Behavior behavior;
        behavior.result = tools::Tool::Result::Error("已取消");
        behavior.result.outcome = "cancelled_during_run";
        behavior.result.error_code = "mcp.cancelled";
        BridgeHarness harness("cancelled", std::move(behavior));
        std::atomic<bool> cancel{true};
        const auto result =
            harness.service->Call("kb_lookup", nlohmann::json::object(), "h", "d", "i", std::nullopt,
                                  std::nullopt, std::nullopt, &cancel);
        REQUIRE(result.admitted);
        CHECK(result.status == "cancelled");
        CHECK(result.record.at("executionUncertain") == true);
        const auto cancelled = ReadKind(harness.ledger.jsonl, "tool.execution.cancelled");
        REQUIRE(cancelled.size() == 1);
    }
    // 工具自报错:执行已发生、结果已知 -> finished(is_error 原样),不是 unknown。
    {
        StubTool::Behavior behavior;
        behavior.result = tools::Tool::Result::Error("远端说参数不对");
        behavior.result.outcome = "tool_error";
        behavior.result.error_code = "kb.bad_query";
        BridgeHarness harness("toolerr", std::move(behavior));
        const auto result = harness.Call();
        REQUIRE(result.admitted);
        CHECK(result.status == "finished");
        CHECK(result.is_error);
        CHECK(result.record.at("terminal") == "finished");
        CHECK(result.record.at("executionUncertain") == false);
        const auto finished = ReadKind(harness.ledger.jsonl, "tool.execution.finished");
        REQUIRE(finished.size() == 1);
    }
    // 本地执行抛异常:failed(执行态已知)。
    {
        StubTool::Behavior behavior;
        behavior.result = tools::Tool::Result::Error("工具执行抛异常: 演练");
        behavior.result.outcome = "tool_exception";
        BridgeHarness harness("threw", std::move(behavior));
        const auto result = harness.Call();
        REQUIRE(result.admitted);
        CHECK(result.status == "failed");
        const auto failed = ReadKind(harness.ledger.jsonl, "tool.execution.failed");
        REQUIRE(failed.size() == 1);
    }
}

// ---------------------------------------------------------------------------
// 递归与次数治理(§5.1:拒绝无界递归;不因避递归跳过权限)
// ---------------------------------------------------------------------------

TEST_CASE("治理:同名重复帽与总次数帽;超帽是 rejected(准入账),不是静默失败") {
    StubTool::Behavior behavior;
    BridgeHarness harness("caps", std::move(behavior));
    harness.service = std::make_unique<HookToolExecutionService>([&]() {
        HookToolExecutionService::Options options = harness.options;
        options.max_calls_per_invocation = 2;   // 总帽先到:第三次按 call_cap 拒
        options.max_same_tool_calls = 5;
        return options;
    }());
    CHECK(harness.Call().admitted);
    CHECK(harness.Call().admitted);
    const auto third = harness.Call();
    REQUIRE_FALSE(third.admitted);
    CHECK(third.error_code == "hook.tool.call_cap");
    // 名单外(权限)先于次数帽:换了名字也进不来(权限不因治理跳过)。
    const auto other = harness.service->Call("other", nlohmann::json::object(), "h", "d", "i",
                                             std::nullopt, std::nullopt, std::nullopt, nullptr);
    CHECK_FALSE(other.admitted);
    CHECK(other.error_code == "hook.tool.unknown");

    // 同名重复帽单独验:总帽放宽,同名第三次被 repeat_cap 拒——朴素重试环
    // 治理(副作用不自动重试,§5.1)。
    StubTool::Behavior behavior2;
    BridgeHarness harness2("caps2", std::move(behavior2));
    harness2.service = std::make_unique<HookToolExecutionService>([&]() {
        HookToolExecutionService::Options options = harness2.options;
        options.max_calls_per_invocation = 8;
        options.max_same_tool_calls = 2;
        return options;
    }());
    CHECK(harness2.Call().admitted);
    CHECK(harness2.Call().admitted);
    const auto repeat = harness2.Call();
    REQUIRE_FALSE(repeat.admitted);
    CHECK(repeat.error_code == "hook.tool.repeat_cap");
    CHECK(repeat.record.at("terminal") == "rejected");
}

namespace {

// 嵌套递归:工具执行里再经服务调工具(模拟"工具又触发 hook 又调工具")。
class RecursiveTool final : public tools::Tool {
public:
    HookToolExecutionService* service = nullptr;
    int depth_limit = 1;
    int entered = 0;

    std::string name() const override { return "recursive_probe"; }
    std::string description() const override { return "递归探针"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++entered;
        if (service == nullptr) {
            return tools::Tool::Result::Text("no service");
        }
        const auto nested = service->Call("recursive_probe", nlohmann::json::object(), "h", "d",
                                          "i", std::nullopt, std::nullopt, std::nullopt, nullptr);
        if (!nested.admitted) {
            return tools::Tool::Result::Text("nested rejected: " + nested.error_code);
        }
        return tools::Tool::Result::Text("nested: " + nested.content);
    }
};

}  // namespace

TEST_CASE("治理:子执行嵌套深度帽(工具里再调工具),拒绝无界递归") {
    tools::ToolRegistry registry;
    auto probe = std::make_unique<RecursiveTool>();
    RecursiveTool* probe_ptr = probe.get();
    HookToolExecutionService::Options options;
    options.allow_tools = {"recursive_probe"};
    options.max_recursion_depth = 2;  // 在途两层:顶层 + 一次嵌套
    registry.Register(std::move(probe));
    options.registry = &registry;
    HookToolExecutionService service(options);
    probe_ptr->service = &service;

    const auto result = service.Call("recursive_probe", nlohmann::json::object(), "h", "d", "i",
                                     std::nullopt, std::nullopt, std::nullopt, nullptr);
    REQUIRE(result.admitted);
    // 第二层嵌套被拒(在途已达帽),第三层不存在——无界递归到此为止。
    CHECK(result.content.rfind("nested: nested rejected: hook.tool.recursion_depth", 0) == 0);
    CHECK(probe_ptr->entered == 2);
}

// ---------------------------------------------------------------------------
// 结果引用与截断
// ---------------------------------------------------------------------------

TEST_CASE("结果:正文超 preview 帽截断并标 truncated;record 摘要同口径") {
    StubTool::Behavior behavior;
    behavior.result = tools::Tool::Result::Text(std::string(40 * 1024, 'z'));
    BridgeHarness harness("truncate", std::move(behavior));
    const auto result = harness.Call();
    REQUIRE(result.admitted);
    CHECK(result.content.size() == 32 * 1024);
    CHECK(result.content_truncated);
    CHECK(result.record.at("resultSummary").at("contentTruncated") == true);
    CHECK(result.record.at("resultSummary").at("contentBytes") == 40 * 1024);
}
