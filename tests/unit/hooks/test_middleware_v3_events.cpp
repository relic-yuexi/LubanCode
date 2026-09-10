// hook 事件账(LuaHook 单 P0-B,§7.1):MiddlewareEventSink -> V3Writer 适配。
// 一个 writer;requested/started/proposed/consumed/settled/completed 全序落
// 账;洋葱嵌套(handler A 的 next 里跑 handler B)事件良嵌套;skipped 汇总;
// 文件过 VerifyV3File;FoldHookDispatches 折回恢复视图。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"
#include "runtime/middleware_v3_sink.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode;
using namespace lubancode::hooks::middleware;
using namespace lubancode::runtime;
using namespace lubancode::trajectory::v3;

namespace {

class FixedClock : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct LedgerHarness {
    FixedClock clock;
    std::filesystem::path dir;
    std::filesystem::path jsonl;
    std::optional<V3Writer> writer;

    explicit LedgerHarness(const char* tag) {
        dir = std::filesystem::temp_directory_path() / ("lubancode-mw-v3-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
        auto started = V3Writer::Start(jsonl, "20260910-140000-MW01", "run-000001",
                                        "你是 LubanCode。", nlohmann::json::object(),
                                        V3WriterOptions{}, &clock);
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

}  // namespace

TEST_CASE("两枚改写链:事件全序落账,洋葱嵌套良序,验卷通过") {
    LedgerHarness harness("chain");
    V3MiddlewareEventSink sink(*harness.writer);

    MiddlewarePool pool;
    // A 改写后经 next 跑 B(嵌套:B 的 started/completed 落在 A 的收口之前)。
    MiddlewareDefinition a;
    a.point = HookPoint::PreUser;
    a.name = "prompt.normalize";
    a.layer = SourceLayer::Builtin;
    a.source_label = "builtin";
    a.implementation_ref = "builtin.prompt_normalize_v1";
    a.priority = 50;
    a.builtin = [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
        nlohmann::json candidate = input;
        candidate["prompt"] = "规范化后";
        return HandlerReturn::Value(next(candidate).value);
    };
    MiddlewareDefinition b;
    b.point = HookPoint::PreUser;
    b.name = "prompt.audit";
    b.layer = SourceLayer::Builtin;
    b.source_label = "builtin";
    b.implementation_ref = "builtin.prompt_audit_v1";
    b.priority = 100;
    b.builtin = [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
        return HandlerReturn::Value(next().value);
    };
    pool.AddDefinition(std::move(a));
    pool.AddDefinition(std::move(b));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    MiddlewareDispatcher dispatcher(std::move(*published));

    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"prompt", "原话"}};
    trigger.turn_id = "turn-000001";
    DispatchOutcome outcome = dispatcher.Dispatch(HookPoint::PreUser, trigger,
                                                  [](const nlohmann::json& in) { return in; }, &sink);
    REQUIRE(outcome.Ok());
    REQUIRE(sink.recent_errors().empty());

    // 事件序(requested 携 matchedHandlers 快照;嵌套:started_A → proposed
    // → consumed → started_B → completed_B → completed_A)。
    const auto events = ReadEvents(harness.jsonl, "hook.");
    std::vector<std::string> order;
    for (const auto& event : events) {
        const std::string kind = event["kind"];
        if (kind == "hook.started" || kind == "hook.completed") {
            order.push_back(kind + ":" + event["payload"]["hookId"].get<std::string>());
        } else if (kind == "hook.output.proposed") {
            order.push_back(kind + ":" + event["payload"]["phase"].get<std::string>());
        } else if (kind == "hook.continuation.consumed") {
            order.push_back(kind);
        } else if (kind == "hook.effects.applied") {
            order.push_back(kind + ":" + event["payload"]["effectType"].get<std::string>());
        }
    }
    REQUIRE(order.size() == 10);
    CHECK(order[0] == "hook.started:PreUser/prompt.normalize");
    CHECK(order[1] == "hook.output.proposed:before_next");        // 候选先存(§7.1)
    CHECK(order[2] == "hook.effects.applied:input.rewrite");      // 验证后采用
    CHECK(order[3] == "hook.continuation.consumed");              // A 的一次性执行权
    CHECK(order[4] == "hook.started:PreUser/prompt.audit");       // 嵌套:B 在 A 收口前跑
    CHECK(order[5] == "hook.continuation.consumed");              // B 的执行权
    CHECK(order[6] == "hook.output.proposed:after_next");         // B 的后置产出
    CHECK(order[7] == "hook.completed:PreUser/prompt.audit");
    CHECK(order[8] == "hook.output.proposed:after_next");         // A 的后置产出
    CHECK(order[9] == "hook.completed:PreUser/prompt.normalize");

    // adopted 效果带采用值(工作版本可从账恢复)。
    bool saw_rewrite_value = false;
    for (const auto& event : ReadEvents(harness.jsonl, "hook.effects.applied")) {
        if (event["payload"]["effectType"] == "input.rewrite" &&
            event["payload"]["appliedValueRef"]["prompt"] == "规范化后") {
            saw_rewrite_value = true;
        }
    }
    CHECK(saw_rewrite_value);

    // 验卷:事件全部过 schema(kind/status/payload 合同)。
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("短路:proposed(short_circuit) 落账,未进入项不伪造执行") {
    LedgerHarness harness("shortcircuit");
    V3MiddlewareEventSink sink(*harness.writer);

    MiddlewarePool pool;
    MiddlewareDefinition gate;
    gate.point = HookPoint::PreUser;
    gate.name = "prompt.gate";
    gate.layer = SourceLayer::Builtin;
    gate.source_label = "builtin";
    gate.implementation_ref = "builtin.prompt_gate_v1";
    gate.builtin = [](const InvocationCtx&, const nlohmann::json&, NextCall&) {
        return HandlerReturn::Denied("input_rejected", "不许发");
    };
    pool.AddDefinition(std::move(gate));
    pool.AddDefinition(PassDef(HookPoint::PreUser, "prompt.later"));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    MiddlewareDispatcher dispatcher(std::move(*published));

    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"prompt", "坏话"}};
    DispatchOutcome outcome = dispatcher.Dispatch(HookPoint::PreUser, trigger,
                                                  [](const nlohmann::json& in) { return in; }, &sink);
    REQUIRE(outcome.kind == DispatchOutcome::Kind::Denied);
    REQUIRE(sink.recent_errors().empty());

    const auto proposed = ReadEvents(harness.jsonl, "hook.output.proposed");
    REQUIRE(proposed.size() == 1);
    CHECK(proposed[0]["payload"]["phase"] == "short_circuit");
    // deny 的 completed 带 decision=deny;后续 handler 无 started。
    const auto started = ReadEvents(harness.jsonl, "hook.started");
    REQUIRE(started.size() == 1);
    CHECK(started[0]["payload"]["hookId"] == "PreUser/prompt.gate");
    const auto completed = ReadEvents(harness.jsonl, "hook.completed");
    REQUIRE(completed.size() == 1);
    CHECK(completed[0]["payload"]["decision"] == "deny");
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("无匹配链项:skipped 汇总一条,不刷屏不伪造") {
    LedgerHarness harness("skip");
    V3MiddlewareEventSink sink(*harness.writer);

    MiddlewarePool pool;  // 空池 → 零定义
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    MiddlewareDispatcher dispatcher(std::move(*published));

    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"prompt", "话"}};
    dispatcher.Dispatch(HookPoint::PostUser, trigger, nullptr, &sink);
    REQUIRE(sink.recent_errors().empty());

    const auto skipped = ReadEvents(harness.jsonl, "hook.skipped");
    REQUIRE(skipped.size() == 1);
    CHECK(skipped[0]["payload"]["reason"] == "no_handlers");
    CHECK(ReadEvents(harness.jsonl, "hook.started").empty());
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("折回:FoldHookDispatches 恢复工作版本与已采用 appends") {
    LedgerHarness harness("fold");
    V3MiddlewareEventSink sink(*harness.writer);

    MiddlewarePool pool;
    MiddlewareDefinition rewriter;
    rewriter.point = HookPoint::PreUser;
    rewriter.name = "prompt.normalize";
    rewriter.layer = SourceLayer::Builtin;
    rewriter.source_label = "builtin";
    rewriter.implementation_ref = "builtin.prompt_normalize_v1";
    rewriter.builtin = [](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
        nlohmann::json candidate = input;
        candidate["prompt"] = "工作版本";
        HandlerReturn out = HandlerReturn::Value(next(candidate).value);
        out.effects.push_back(Effect{EffectType::ContextAppend, nlohmann::json{{"text", "追加的一段"}}});
        return out;
    };
    pool.AddDefinition(std::move(rewriter));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    MiddlewareDispatcher dispatcher(std::move(*published));

    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"prompt", "原话"}};
    trigger.turn_id = "turn-000009";
    trigger.step_id = "step-000001";
    dispatcher.Dispatch(HookPoint::PreUser, trigger, [](const nlohmann::json& in) { return in; }, &sink);
    REQUIRE(sink.recent_errors().empty());

    auto ledger = ReadV3Ledger(harness.jsonl);
    REQUIRE(ledger.has_value());
    const auto dispatches = FoldHookDispatches(*ledger);
    REQUIRE(dispatches.size() == 1);
    const HookDispatchView& view = dispatches[0];
    CHECK(view.hook_point == "PreUser");
    CHECK(view.folded_status == "completed");
    CHECK(*view.turn_id == "turn-000009");
    REQUIRE(view.invocations.size() == 1);
    CHECK(view.invocations[0].status == "completed");
    CHECK(view.invocations[0].continuation_consumed);
    // 工作版本:最后采用的 input.rewrite 候选。
    REQUIRE(view.has_adopted_working_input);
    CHECK(view.adopted_working_input["prompt"] == "工作版本");
    REQUIRE(view.adopted_context_appends.size() == 1);
    CHECK(view.adopted_context_appends[0] == "追加的一段");
}
