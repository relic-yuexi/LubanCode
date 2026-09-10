// 崩溃边界与恢复(LuaHook 单 P0-B,P0-B 第 3 条 + §7.3):在每个持久化边界
// 注入崩溃(测试内截断 sink——前 N 枚事件落账后断流,模拟 writer 在该边界
// 死掉),验证:补交(Replay/Resubmit)、暂停(PauseUnknown)、unknown、已
// 完成项不重跑(恢复计划里 completed 集合固定);恢复输入匹配最后合法链
// (工作版本);无重复注入(appends 判重底);只读 replay 零脚本执行。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"
#include "runtime/middleware_recovery.hpp"
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

// 崩溃注入(测试专用,不进生产):前 limit 枚事件透传给真 sink,之后断流
// ——账面停在边界上,模拟进程在那一笔提交后死掉。
class CrashAfterNSink final : public MiddlewareEventSink {
public:
    CrashAfterNSink(MiddlewareEventSink& real, int limit) : real_(real), limit_(limit) {}

    void OnDispatchRequested(const DispatchMeta& meta, const std::vector<HandlerSnapshot>& handlers) override {
        if (Gated()) return;
        real_.OnDispatchRequested(meta, handlers);
    }
    void OnSkipped(const DispatchMeta& meta, std::string_view reason) override {
        if (Gated()) return;
        real_.OnSkipped(meta, reason);
    }
    void OnInvocationStarted(const InvocationMeta& meta) override {
        if (Gated()) return;
        real_.OnInvocationStarted(meta);
    }
    void OnInvocationCompleted(const InvocationMeta& meta, std::optional<std::string> decision,
                               std::uint64_t duration_ms) override {
        if (Gated()) return;
        real_.OnInvocationCompleted(meta, decision, duration_ms);
    }
    void OnInvocationFailed(const InvocationMeta& meta, std::string_view error_code,
                            std::uint64_t duration_ms) override {
        if (Gated()) return;
        real_.OnInvocationFailed(meta, error_code, duration_ms);
    }
    void OnInvocationCancelled(const InvocationMeta& meta, std::string_view reason) override {
        if (Gated()) return;
        real_.OnInvocationCancelled(meta, reason);
    }
    void OnEffectSettled(const InvocationMeta& meta, std::string_view effect_type, bool applied,
                         std::string_view reason, const nlohmann::json& value) override {
        if (Gated()) return;
        real_.OnEffectSettled(meta, effect_type, applied, reason, value);
    }
    void OnContinuationConsumed(const InvocationMeta& meta) override {
        if (Gated()) return;
        real_.OnContinuationConsumed(meta);
    }
    void OnOutputProposed(const InvocationMeta& meta, std::string_view phase,
                          const nlohmann::json& candidate) override {
        if (Gated()) return;
        real_.OnOutputProposed(meta, phase, candidate);
    }

private:
    bool Gated() { return ++count_ > limit_; }
    MiddlewareEventSink& real_;
    int limit_;
    int count_ = 0;
};

struct RecoveryHarness {
    FixedClock clock;
    std::filesystem::path dir;
    std::filesystem::path jsonl;
    std::optional<V3Writer> writer;

    explicit RecoveryHarness(const char* tag) {
        dir = std::filesystem::temp_directory_path() / ("lubancode-mw-recover-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
        auto started = V3Writer::Start(jsonl, "20260910-140000-MWRC", "run-000001",
                                        "你是 LubanCode。", nlohmann::json::object(),
                                        V3WriterOptions{}, &clock);
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }

    std::optional<HookDispatchView> FoldOne() const {
        auto ledger = ReadV3Ledger(jsonl);
        if (!ledger.has_value()) {
            return std::nullopt;
        }
        const auto dispatches = FoldHookDispatches(*ledger);
        if (dispatches.empty()) {
            return std::nullopt;
        }
        return dispatches.front();
    }
};

// 生产形状的两枚链:A(rewriter,改写 + 追加上下文)→ B(观察透传)。
// A 改写后 next 里跑 B:B 先 completed,A 后 completed(洋葱序)。
std::shared_ptr<const FrozenRegistry> PublishChain(int* a_runs, int* b_runs) {
    MiddlewarePool pool;
    MiddlewareDefinition a;
    a.point = HookPoint::PreUser;
    a.name = "prompt.normalize";
    a.layer = SourceLayer::Builtin;
    a.source_label = "builtin";
    a.implementation_ref = "builtin.prompt_normalize_v1";
    a.priority = 50;
    a.builtin = [a_runs](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
        if (a_runs != nullptr) ++*a_runs;
        nlohmann::json candidate = input;
        candidate["prompt"] = "工作版本";
        HandlerReturn out = HandlerReturn::Value(next(candidate).value);
        out.effects.push_back(Effect{EffectType::ContextAppend, nlohmann::json{{"text", "追加的一段"}}});
        return out;
    };
    MiddlewareDefinition b;
    b.point = HookPoint::PreUser;
    b.name = "prompt.audit";
    b.layer = SourceLayer::Builtin;
    b.source_label = "builtin";
    b.implementation_ref = "builtin.prompt_audit_v1";
    b.priority = 100;
    b.builtin = [b_runs](const InvocationCtx&, const nlohmann::json& input, NextCall& next) {
        if (b_runs != nullptr) ++*b_runs;
        return HandlerReturn::Value(next().value);
    };
    pool.AddDefinition(std::move(a));
    pool.AddDefinition(std::move(b));
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    return std::move(*published);
}

// 在第 limit 枚事件后崩溃,返回恢复判例。
HookResumePlan CrashAndPlan(RecoveryHarness& harness, int limit) {
    V3MiddlewareEventSink real_sink(*harness.writer);
    CrashAfterNSink crash_sink(real_sink, limit);
    MiddlewareDispatcher dispatcher(PublishChain(nullptr, nullptr));
    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"prompt", "原话"}};
    trigger.turn_id = "turn-000001";
    dispatcher.Dispatch(HookPoint::PreUser, trigger, [](const nlohmann::json& in) { return in; },
                        &crash_sink);
    const auto view = harness.FoldOne();
    REQUIRE(view.has_value());
    return PlanHookResume(*view);
}

}  // namespace

// 边界序(两枚链,完整事件序;效果先于 completed 落账——RecordEffect 在
// invocation 收口之前):requested(1) → started_A(2) → proposed_A(3) →
// applied_A(4) → consumed_A(5) → started_B(6) → consumed_B(7) →
// proposed_B(8) → completed_B(9) → settled_A(append)(10) → proposed_A2
// (11) → completed_A(12)。

TEST_CASE("边界 1:requested 落账、started 之前崩——按原输入推进") {
    RecoveryHarness harness("b1");
    const HookResumePlan plan = CrashAndPlan(harness, 1);
    CHECK(plan.action == HookResumeAction::ProceedFromInput);
    CHECK(plan.completed_invocations.empty());
    CHECK(!plan.executes_scripts);
}

TEST_CASE("边界 2:started_A 落账、候选之前崩——只推进待执行项") {
    RecoveryHarness harness("b2");
    const HookResumePlan plan = CrashAndPlan(harness, 2);
    CHECK(plan.action == HookResumeAction::AdvancePendingOnly);
    CHECK(plan.completed_invocations.empty());   // 没有已完成项
    REQUIRE(plan.pending_invocations.size() == 1);
}

TEST_CASE("边界 3:候选已存、效果未提交——补提交,不悄悄改内存") {
    RecoveryHarness harness("b3");
    const HookResumePlan plan = CrashAndPlan(harness, 3);
    CHECK(plan.action == HookResumeAction::ReplayAdoptedEffects);
    // 核验材料:proposed 候选(工作版本)随行。
    const auto view = harness.FoldOne();
    REQUIRE(view.has_value());
    CHECK(!view->has_adopted_working_input);  // applied 未落账:恢复不冒充已采用
}

TEST_CASE("边界 4:applied 落账、consumed 之前崩——工作版本已可恢复") {
    RecoveryHarness harness("b4");
    const HookResumePlan plan = CrashAndPlan(harness, 4);
    // A 还 running(有候选、consumed 未落)→ 按候选补提交。
    CHECK(plan.action == HookResumeAction::ReplayAdoptedEffects);
    const auto view = harness.FoldOne();
    REQUIRE(view.has_value());
    REQUIRE(view->has_adopted_working_input);
    CHECK(view->adopted_working_input["prompt"] == "工作版本");  // 恢复输入匹配最后合法链
    CHECK(plan.adopted_working_input.has_value());
    CHECK((*plan.adopted_working_input)["prompt"] == "工作版本");
}

TEST_CASE("边界 5:consumed_A 落账、B 未 started——下游结果不明,暂停不重调") {
    RecoveryHarness harness("b5");
    const HookResumePlan plan = CrashAndPlan(harness, 5);
    // A running + consumed + 无更晚 started → 行 3:unknown/暂停。
    CHECK(plan.action == HookResumeAction::PauseUnknown);
    CHECK(!plan.executes_scripts);
}

TEST_CASE("边界 6:B 已完成、A 后置未返回——保留下游结果,不重跑") {
    RecoveryHarness harness("b6");
    const HookResumePlan plan = CrashAndPlan(harness, 9);
    // A running + consumed + 有更晚 started 的 B 已 completed → 行 4。
    CHECK(plan.action == HookResumeAction::KeepDownstreamResult);
    REQUIRE(plan.completed_invocations.size() == 1);  // B 已收口,不重跑
    CHECK(!plan.executes_scripts);
}

TEST_CASE("边界 7:效果已落、A 未收口——仍是行 4,已完成项不重跑") {
    RecoveryHarness harness("b7");
    // settled(append) 是第 10 枚:效果在账、completed_A 缺 → A 还 running,
    // 下游 B 已完成 → 行 4(保留下游结果);append 已在账不重复注入。
    const HookResumePlan plan = CrashAndPlan(harness, 10);
    CHECK(plan.action == HookResumeAction::KeepDownstreamResult);
    REQUIRE(plan.completed_invocations.size() == 1);
    const auto view = harness.FoldOne();
    REQUIRE(view.has_value());
    REQUIRE(view->adopted_context_appends.size() == 1);  // 效果已提交:判重底在
}

TEST_CASE("行 5(handler 返回已保存、效果尚缺):补提交原返回,不重跑") {
    // 现行执行核的效果先于 completed 落账,单 dispatch 内造不出"completed
    // 在、效果缺"的断档;按合同形状手工落账(异步/未来执行核的合法断点),
    // 判例按 §7.3 行 5 收口。
    RecoveryHarness harness("row5");
    HookHandlerSpec spec;
    spec.hook_id = "PreUser/prompt.normalize";
    spec.definition_hash = "abc123";
    spec.handler_kind = "builtin";
    spec.definition_order = 0;
    spec.failure_policy = "block";
    NestedHookDispatchSession session = NestedHookDispatchSession::Open(
        *harness.writer, "hookdispatch-000001", "PreUser", "turn-000001", std::nullopt, std::nullopt,
        {spec}, std::nullopt);
    REQUIRE(session.BeginInvocation(*harness.writer, "hookdispatch-000001#0", spec).status ==
            WriteReceipt::Status::Committed);
    // 提案带效果清单(context.append),settled 缺;completed 已落。
    REQUIRE(session.OutputProposed(*harness.writer, "hookdispatch-000001#0", "after_next",
                                   nlohmann::json{{"output", nlohmann::json::object()},
                                                   {"deny", false},
                                                   {"effects", nlohmann::json::array({nlohmann::json{
                                                       {"type", "context.append"},
                                                       {"payload", nlohmann::json{{"text", "漏交的一段"}}}}})}})
                .status == WriteReceipt::Status::Committed);
    REQUIRE(session.CompleteInvocation(*harness.writer, "hookdispatch-000001#0", spec.hook_id,
                                       std::nullopt, std::nullopt, 5)
                .status == WriteReceipt::Status::Committed);

    const auto view = harness.FoldOne();
    REQUIRE(view.has_value());
    const HookResumePlan plan = PlanHookResume(*view);
    CHECK(plan.action == HookResumeAction::ResubmitHandlerReturn);
    CHECK(plan.reason.find("效果尚缺") != std::string::npos);
    CHECK(!plan.executes_scripts);
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("边界 8:全账落齐——依预留身份补交一次(appends 判重底随行)") {
    RecoveryHarness harness("b8");
    const HookResumePlan plan = CrashAndPlan(harness, 999);
    CHECK(plan.action == HookResumeAction::ResubmitFinalEffect);
    REQUIRE(plan.completed_invocations.size() == 2);
    REQUIRE(plan.adopted_context_appends.size() == 1);
    CHECK(plan.adopted_context_appends[0] == "追加的一段");  // 已在账的不再注入
}

TEST_CASE("恢复后重放:已完成项不重跑,只读 replay 零脚本执行") {
    RecoveryHarness harness("replay");
    // 完整跑一遍(账全落),统计两枚 handler 的真实执行次数。
    int a_runs = 0;
    int b_runs = 0;
    {
        V3MiddlewareEventSink real_sink(*harness.writer);
        MiddlewareDispatcher dispatcher(PublishChain(&a_runs, &b_runs));
        DispatchTrigger trigger;
        trigger.input = nlohmann::json{{"prompt", "原话"}};
        dispatcher.Dispatch(HookPoint::PreUser, trigger, [](const nlohmann::json& in) { return in; },
                            &real_sink);
    }
    REQUIRE(a_runs == 1);
    REQUIRE(b_runs == 1);

    // 恢复:折账 + 判例。只读——不重新执行 handler;判例只允许补提交/补交,
    // 不允许把 completed 项再跑一遍(§7.3"已完成 hook 不重跑")。
    const auto view = harness.FoldOne();
    REQUIRE(view.has_value());
    const HookResumePlan plan = PlanHookResume(*view);
    CHECK(plan.action == HookResumeAction::ResubmitFinalEffect);
    REQUIRE(plan.completed_invocations.size() == 2);
    CHECK(plan.pending_invocations.empty());
    CHECK(!plan.executes_scripts);  // 只读 replay 零脚本执行的合同位

    // 再跑一遍"恢复后的 dispatch"(拿判例当续跑依据):同一注册表重放不
    // 增加执行次数——执行次数只由真 dispatch 增加,恢复读账不碰 handler。
    const int a_before = a_runs;
    const int b_before = b_runs;
    const HookResumePlan replay_plan = PlanHookResume(harness.FoldOne().value());
    CHECK(replay_plan.completed_invocations.size() == 2);
    CHECK(a_runs == a_before);
    CHECK(b_runs == b_before);
}

TEST_CASE("定义缺失/hash 不符:明报缺口,不用今天脚本猜昨天产物") {
    RecoveryHarness harness("gap");
    V3MiddlewareEventSink real_sink(*harness.writer);
    MiddlewareDispatcher dispatcher(PublishChain(nullptr, nullptr));
    DispatchTrigger trigger;
    trigger.input = nlohmann::json{{"prompt", "原话"}};
    dispatcher.Dispatch(HookPoint::PreUser, trigger, [](const nlohmann::json& in) { return in; },
                        &real_sink);

    const auto view = harness.FoldOne();
    REQUIRE(view.has_value());
    // 当前注册表给一份对不上的 hash 集 → ReportGap。
    const HookResumePlan plan = PlanHookResume(*view, {"deadbeef00000000"});
    CHECK(plan.action == HookResumeAction::ReportGap);
    // 不做 hash 核验(空集)时不拦:调用方自己放弃核验。
    const HookResumePlan lenient = PlanHookResume(*view);
    CHECK(lenient.action != HookResumeAction::ReportGap);
}
