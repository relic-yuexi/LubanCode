// v3 hook 事件账测试(P1 其余,§4.22-4.24):dispatch/invocation 分层、
// completed≠改写已采用、效果 applied/rejected、无匹配 skipped、deny 也算
// completed、子执行派生账带父链。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/hooks.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;

namespace {

class FixedClock : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct Harness {
    FixedClock clock;
    std::filesystem::path dir;
    std::filesystem::path jsonl;

    explicit Harness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-v3-hook-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
    }

    std::optional<V3Writer> Start() {
        auto writer = V3Writer::Start(jsonl, "20260910-140000-HOOK01", "run-000001",
                                      "你是 LubanCode。", nlohmann::json::object(),
                                      V3WriterOptions{}, &clock);
        if (!writer.has_value()) {
            return std::nullopt;
        }
        return std::move(*writer);
    }
};

std::vector<nlohmann::json> ReadJson(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<nlohmann::json> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(nlohmann::json::parse(line));
        }
    }
    return lines;
}

HookHandlerSpec Spec(std::string hook_id = "hook-estimator") {
    return HookHandlerSpec{std::move(hook_id), "abc123def456", "builtin", 1, "block"};
}

}  // namespace

TEST_CASE("无 handler:skipped 留原因,不伪造执行") {
    Harness harness("skipped");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    WriteReceipt skipped = HookDispatchSession::Skip(
        *writer, writer->NewHookDispatchId(), "PreAction", "no_matching_handlers",
        "turn-000001", "step-000001", "action-000001");
    REQUIRE(skipped.status == WriteReceipt::Status::Committed);
    auto lines = ReadJson(harness.jsonl);
    const nlohmann::json* event = nullptr;
    for (const auto& line : lines) {
        if (line.value("kind", "") == "hook.skipped") {
            event = &line;
        }
    }
    REQUIRE(event != nullptr);
    CHECK(!(*event).contains("status"));  // skipped 不携带 status(§2.2)
    CHECK((*event)["payload"]["reason"] == "no_matching_handlers");
    CHECK((*event)["payload"]["hookPoint"] == "PreAction");
    // 不伪造 started/completed。
    for (const auto& line : lines) {
        CHECK(line.value("kind", "") != "hook.started");
        CHECK(line.value("kind", "") != "hook.completed");
    }
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("dispatch/invocation 分层:requested → started → completed,身份各自齐全") {
    Harness harness("lifecycle");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto session = HookDispatchSession::Dispatch(
        *writer, writer->NewHookDispatchId(), "PreAction", "turn-000001", "step-000001",
        "action-000001", {Spec()}, nlohmann::json("input-ref-1"));
    REQUIRE(session.BeginInvocation(*writer, "hookinv-000001", Spec()).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(session.CompleteInvocation(*writer, "allow", nlohmann::json("output-ref-1"), 12)
                .status == WriteReceipt::Status::Committed);

    auto lines = ReadJson(harness.jsonl);
    int dispatch_events = 0;
    const nlohmann::json* started = nullptr;
    const nlohmann::json* completed = nullptr;
    for (const auto& line : lines) {
        const std::string kind = line.value("kind", "");
        if (kind == "hook.dispatch.requested") {
            ++dispatch_events;
            CHECK(line["payload"]["hookPoint"] == "PreAction");
            CHECK(line["payload"]["matchedHandlers"].size() == 1);
            CHECK(line["payload"]["matchedHandlers"][0]["definitionHash"] == "abc123def456");
            CHECK(line["payload"]["matchedHandlers"][0]["failurePolicy"] == "block");
        }
        if (kind == "hook.started") {
            started = &line;
        }
        if (kind == "hook.completed") {
            completed = &line;
        }
    }
    CHECK(dispatch_events == 1);
    REQUIRE(started != nullptr);
    REQUIRE(completed != nullptr);
    CHECK((*started)["status"] == "running");
    CHECK((*started)["payload"]["hookInvocationId"] == "hookinv-000001");
    CHECK((*started)["payload"]["hookId"] == "hook-estimator");
    CHECK((*started)["payload"]["handlerKind"] == "builtin");
    CHECK((*completed)["status"] == "done");
    CHECK((*completed)["payload"]["decision"] == "allow");
    CHECK((*completed)["payload"]["outputRef"] == "output-ref-1");
    CHECK((*completed)["payload"]["durationMs"] == 12);
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("completed ≠ 改写已采用:效果另记 hook.effects.applied") {
    Harness harness("effects");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto session = HookDispatchSession::Dispatch(
        *writer, writer->NewHookDispatchId(), "PostAction", "turn-000001", "step-000001",
        "action-000001", {Spec()}, std::nullopt);
    REQUIRE(session.BeginInvocation(*writer, "hookinv-000001", Spec()).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(session.CompleteInvocation(*writer, "rewrite", nlohmann::json("hook-out-1"),
                                       30)
                .status == WriteReceipt::Status::Committed);
    // handler 已返回 ≠ 效果已采用(§4.22):runtime 验证后才 applied。
    WriteReceipt applied = session.ApplyEffect(
        *writer, "result_replace", nlohmann::json("tool-out-1"),
        nlohmann::json("hook-out-1"), nlohmann::json("res-000002"), "passed");
    REQUIRE(applied.status == WriteReceipt::Status::Committed);

    auto lines = ReadJson(harness.jsonl);
    const nlohmann::json* effect = nullptr;
    for (const auto& line : lines) {
        if (line.value("kind", "") == "hook.effects.applied") {
            effect = &line;
        }
    }
    REQUIRE(effect != nullptr);
    CHECK((*effect)["payload"]["effectType"] == "result_replace");
    CHECK((*effect)["payload"]["hookInvocationId"] == "hookinv-000001");
    CHECK((*effect)["payload"]["inputRef"] == "tool-out-1");
    CHECK((*effect)["payload"]["outputRef"] == "hook-out-1");
    CHECK((*effect)["payload"]["appliedValueRef"] == "res-000002");
    CHECK((*effect)["payload"]["validation"] == "passed");
    // applied 不携带 status(§2.2)。
    CHECK(!(*effect).contains("status"));
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("候选效果被拒:hook.effects.rejected 带原因,原内容留档") {
    Harness harness("rejected");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto session = HookDispatchSession::Dispatch(
        *writer, writer->NewHookDispatchId(), "PreAction", "turn-000001", "step-000001",
        "action-000001", {Spec()}, std::nullopt);
    REQUIRE(session.BeginInvocation(*writer, "hookinv-000001", Spec()).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(session.CompleteInvocation(*writer, "rewrite", std::nullopt, 8).status ==
            WriteReceipt::Status::Committed);
    WriteReceipt rejected =
        session.RejectEffect(*writer, "args_rewrite", "schema_validation_failed");
    REQUIRE(rejected.status == WriteReceipt::Status::Committed);
    auto lines = ReadJson(harness.jsonl);
    for (const auto& line : lines) {
        if (line.value("kind", "") == "hook.effects.rejected") {
            CHECK(line["payload"]["effectType"] == "args_rewrite");
            CHECK(line["payload"]["reason"] == "schema_validation_failed");
        }
    }
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("deny 也是 completed:决策与执行失败分清") {
    Harness harness("deny");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto session = HookDispatchSession::Dispatch(
        *writer, writer->NewHookDispatchId(), "PreAction", "turn-000001", "step-000001",
        "action-000001", {Spec()}, std::nullopt);
    REQUIRE(session.BeginInvocation(*writer, "hookinv-000001", Spec()).status ==
            WriteReceipt::Status::Committed);
    // handler 正常返回 deny:completed,不是 failed(§4.24)。
    WriteReceipt completed = session.CompleteInvocation(*writer, "deny", std::nullopt, 4);
    REQUIRE(completed.status == WriteReceipt::Status::Committed);
    auto lines = ReadJson(harness.jsonl);
    for (const auto& line : lines) {
        CHECK(line.value("kind", "") != "hook.failed");
        if (line.value("kind", "") == "hook.completed") {
            CHECK(line["status"] == "done");
            CHECK(line["payload"]["decision"] == "deny");
        }
    }
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("invocation 一次一枚:未收口不得再开,收口后可再来") {
    Harness harness("serial");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto session = HookDispatchSession::Dispatch(
        *writer, writer->NewHookDispatchId(), "PreAction", "turn-000001", "step-000001",
        "action-000001", {Spec(), Spec("hook-guard")}, std::nullopt);
    REQUIRE(session.BeginInvocation(*writer, "hookinv-000001", Spec()).status ==
            WriteReceipt::Status::Committed);
    // 上一枚未收口:拒收(§4.48 串行,§4.22)。
    WriteReceipt overlap = session.BeginInvocation(*writer, "hookinv-000002", Spec("hook-guard"));
    CHECK(overlap.status == WriteReceipt::Status::Rejected);
    CHECK(overlap.error_code == "v3hook.invocation_open");
    REQUIRE(session.CompleteInvocation(*writer, "allow", std::nullopt, 3).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(session.BeginInvocation(*writer, "hookinv-000002", Spec("hook-guard")).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(session.CompleteInvocation(*writer, "allow", std::nullopt, 5).status ==
            WriteReceipt::Status::Committed);
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("handler 失败与取消:状态与原因可恢复") {
    Harness harness("fail-cancel");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    // 失败链。
    {
        auto session = HookDispatchSession::Dispatch(
            *writer, writer->NewHookDispatchId(), "PreAction", std::nullopt, std::nullopt,
            std::nullopt, {Spec()}, std::nullopt);
        REQUIRE(session.BeginInvocation(*writer, "hookinv-000001", Spec()).status ==
                WriteReceipt::Status::Committed);
        WriteReceipt failed = session.FailInvocation(*writer, "handler_crash", 66);
        REQUIRE(failed.status == WriteReceipt::Status::Committed);
    }
    // 取消链。
    {
        auto session = HookDispatchSession::Dispatch(
            *writer, writer->NewHookDispatchId(), "PreAction", std::nullopt, std::nullopt,
            std::nullopt, {Spec()}, std::nullopt);
        REQUIRE(session.BeginInvocation(*writer, "hookinv-000002", Spec()).status ==
                WriteReceipt::Status::Committed);
        WriteReceipt cancelled = session.CancelInvocation(*writer, "user_abort");
        REQUIRE(cancelled.status == WriteReceipt::Status::Committed);
    }
    auto lines = ReadJson(harness.jsonl);
    for (const auto& line : lines) {
        const std::string kind = line.value("kind", "");
        if (kind == "hook.failed") {
            CHECK(line["status"] == "failed");
            CHECK(line["payload"]["error_code"] == "handler_crash");
        }
        if (kind == "hook.cancelled") {
            CHECK(line["status"] == "cancelled");
            CHECK(line["payload"]["reason"] == "user_abort");
        }
    }
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("PostAction 子执行:独立 actionId,payload 带父链与后端") {
    Harness harness("subexec");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    const std::string parent_action = "action-000001";
    auto session = HookDispatchSession::Dispatch(
        *writer, writer->NewHookDispatchId(), "PostAction", "turn-000001", "step-000001",
        parent_action, {Spec("hook-fallback-search")}, std::nullopt);
    REQUIRE(session.BeginInvocation(*writer, "hookinv-000001", Spec("hook-fallback-search"))
                .status == WriteReceipt::Status::Committed);
    // hook 发起的底层调用:宿主派生执行(§4.23)。
    ToolIdentity sub_identity{"search", "builtin", "2.0.0", "remote=backend-b"};
    ToolActionSession sub =
        session.BeginSubExecution(*writer, sub_identity, "backend-b");
    CHECK(sub.action_id() != parent_action);  // 不冒充同一调用
    REQUIRE(sub.Start(*writer, "args-sub-ref", sub_identity, std::nullopt,
                      nlohmann::json::object({{"backend", "backend-b"}}))
                .status == WriteReceipt::Status::Committed);
    REQUIRE(sub.Finish(*writer, 0, 210).status == WriteReceipt::Status::Committed);

    auto lines = ReadJson(harness.jsonl);
    const nlohmann::json* pending = nullptr;
    for (const auto& line : lines) {
        if (line.value("kind", "") == "tool.execution.pending" &&
            line.value("actionId", "") == sub.action_id()) {
            pending = &line;
        }
    }
    REQUIRE(pending != nullptr);
    CHECK((*pending)["payload"]["parentActionId"] == parent_action);
    CHECK((*pending)["payload"]["hookInvocationId"] == "hookinv-000001");
    CHECK((*pending)["payload"]["backend"] == "backend-b");
    CHECK((*pending)["payload"]["logicalTool"] == "search");
    CHECK(VerifyV3File(harness.jsonl).ok);
}
