// 工具执行链拆分(P1:只读工具并行与写入串行单)的行为钉子。
// RunOneTool 拆成四只可复用阶段(门禁审批 / execution_started / worker
// 执行 / 收口)后,这些册子钉住"行为不变"的口径:
//   1. execution_started 栅栏先于真实执行;finished 栅栏终态恰一份;
//   2. 审批链中间产物(PreToolUse 表态/审批类别)按 tool_use_id 记账,
//      两枚调用裁决不同不串槽——旧共享槽(pre_decision_slot/
//      approval_class_slot)正是 P1 拆掉的病根;
//   3. 拒绝(预裁定 Deny / 确认口回 false)的工具不执行,稳定码/终态
//      与拆链前一致;
//   4. Hook 改参后,权限预裁定与确认口拿的都是改写后的最终参数
//      (schema 复检册在 test_hooks.cpp,这里钉权限链那一半);
//   5. 取消旗执行中可见,单枚调用终态仍恰一份;
//   6. 结果捕获失败按 result_store_failed 收口,不冒充成功,原始
//      finished 栅栏已在捕获之前落账。
// P2 的并发断言(R,R,W 时序类)不属本册——那是并行调度落地后的账。

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "app/tool_call_scope.hpp"
#include "runtime/interaction.hpp"
#include "runtime/tool_trajectory_sink.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 需确认的靶工具:记录执行次数、收到的入参、执行当口 started 栅栏到没到、
// 取消旗真值。ApprovalClass 报真实类别(FileEdit),给串槽断言当指纹。
class GatedTool : public tools::Tool {
public:
    std::string name() const override { return "gated_write"; }
    std::string description() const override { return "需要确认的写靶"; }
    nlohmann::json input_schema() const override {
        return nlohmann::json{{"type", "object"},
                              {"properties", nlohmann::json{{"path", nlohmann::json{{"type", "string"}}}}},
                              {"required", nlohmann::json::array({"path"})}};
    }
    bool needs_confirm() const override { return true; }
    tools::ApprovalClass approval_class() const override { return tools::ApprovalClass::FileEdit; }

    tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
    tools::Tool::Result execute(const nlohmann::json& input, const tools::ToolExecutionContext&) override {
        ++executions;
        last_input = input;
        started_seen_at_execute = started_seen;
        return {"wrote " + input.value("path", std::string()), false};
    }

    int executions = 0;
    nlohmann::json last_input;
    bool started_seen = false;         // on_tool_trace 见到本枚 started 时置位
    bool started_seen_at_execute = false;
};

// 不需确认的只读靶:钉 started/finished/取消的朴素路径。
class ProbeTool : public tools::Tool {
public:
    std::string name() const override { return "probe_read"; }
    std::string description() const override { return "只读靶"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"不走旧口", true}; }
    tools::Tool::Result execute(const nlohmann::json&, const tools::ToolExecutionContext& context) override {
        ++executions;
        started_seen_at_execute = started_seen;
        cancel_value_at_execute = context.cancel != nullptr && context.cancel->load(std::memory_order_acquire);
        return {"ok", false};
    }
    int executions = 0;
    bool started_seen = false;
    bool started_seen_at_execute = false;
    bool cancel_value_at_execute = false;
};

// 收 trace 栅栏的壳(与 test_tool_trace.cpp 同款),另给靶工具递 started。
struct TraceCollector {
    std::vector<agent::ToolTraceEvent> events;
    GatedTool* gated = nullptr;
    ProbeTool* probe = nullptr;
    agent::TurnWiring Decorate(agent::TurnWiring wiring) {
        wiring.on_tool_trace = [this](const agent::ToolTraceEvent& event) {
            if (event.kind == agent::ToolTraceEventKind::ExecutionStarted) {
                if (gated != nullptr) {
                    gated->started_seen = true;
                }
                if (probe != nullptr) {
                    probe->started_seen = true;
                }
            }
            events.push_back(event);
        };
        return wiring;
    }
    std::size_t Count(agent::ToolTraceEventKind kind) const {
        std::size_t n = 0;
        for (const auto& event : events) {
            if (event.kind == kind) {
                ++n;
            }
        }
        return n;
    }
};

api::ToolUseBlock MakeCall(const std::string& id, const std::string& path = "a.txt") {
    api::ToolUseBlock call;
    call.id = id;
    call.name = "gated_write";
    call.input = nlohmann::json{{"path", path}};
    return call;
}

}  // namespace

// ---------------------------------------------------------------------------
// started 先于执行;终态恰一份
// ---------------------------------------------------------------------------

TEST_CASE("拆链:execution_started 先于真实执行,finished 恰一份") {
    tools::ToolRegistry registry;
    auto gated = std::make_unique<GatedTool>();
    GatedTool* gated_ptr = gated.get();
    registry.Register(std::move(gated));

    TraceCollector collector;
    collector.gated = gated_ptr;
    agent::TurnWiring wiring = collector.Decorate({});
    // 预裁定直接 Allow:不走确认口,直奔执行。
    wiring.on_permission_evaluate = [](const std::string&, const std::string&, tools::ApprovalClass,
                                       const nlohmann::json&,
                                       const runtime::ToolHookDecision&) -> runtime::PermissionVerdict {
        runtime::PermissionVerdict verdict;
        verdict.action = runtime::PermissionVerdict::Action::Allow;
        return verdict;
    };

    agent::ToolTraceContext ctx;
    ctx.execution_id = "e_chain_1";
    ctx.batch_id = "b_chain";
    ctx.sequence_in_batch = 0;

    const tools::Tool::Result result = agent::RunOneTool(registry, MakeCall("u_chain_1"), wiring,
                                                         /*tool_filter=*/nullptr, std::string(), &ctx);
    CHECK_FALSE(result.is_error);
    REQUIRE(gated_ptr->executions == 1);
    // 工具执行当口,started 栅栏已经发过(先于真实执行的持久证据)。
    CHECK(gated_ptr->started_seen_at_execute);
    // 恰一份 started、恰一份 finished。
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionStarted) == 1);
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionFinished) == 1);
    // finished 带本枚身份与成功终态。
    const auto& finished = collector.events.back();
    CHECK(finished.tool_use_id == "u_chain_1");
    CHECK(finished.execution_id == "e_chain_1");
    CHECK(finished.outcome == agent::ToolOutcome::Succeeded);
}

TEST_CASE("拆链:不需要确认的工具同款——started 先行、终态一份") {
    tools::ToolRegistry registry;
    auto probe = std::make_unique<ProbeTool>();
    ProbeTool* probe_ptr = probe.get();
    registry.Register(std::move(probe));

    TraceCollector collector;
    collector.probe = probe_ptr;
    agent::TurnWiring wiring = collector.Decorate({});
    agent::ToolTraceContext ctx;
    ctx.execution_id = "e_probe_1";

    api::ToolUseBlock call;
    call.id = "u_probe_1";
    call.name = "probe_read";
    const tools::Tool::Result result =
        agent::RunOneTool(registry, call, wiring, nullptr, std::string(), &ctx);
    CHECK_FALSE(result.is_error);
    REQUIRE(probe_ptr->executions == 1);
    CHECK(probe_ptr->started_seen_at_execute);
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionStarted) == 1);
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionFinished) == 1);
}

// ---------------------------------------------------------------------------
// 拒绝的工具不执行
// ---------------------------------------------------------------------------

TEST_CASE("拆链:预裁定 Deny 的工具不执行,终态 permission.no_prompt_denied 且无 started") {
    tools::ToolRegistry registry;
    auto gated = std::make_unique<GatedTool>();
    GatedTool* gated_ptr = gated.get();
    registry.Register(std::move(gated));

    TraceCollector collector;
    agent::TurnWiring wiring = collector.Decorate({});
    wiring.on_permission_evaluate = [](const std::string&, const std::string&, tools::ApprovalClass,
                                       const nlohmann::json&,
                                       const runtime::ToolHookDecision&) -> runtime::PermissionVerdict {
        runtime::PermissionVerdict verdict;
        verdict.action = runtime::PermissionVerdict::Action::Deny;
        return verdict;
    };

    agent::ToolTraceContext ctx;
    ctx.execution_id = "e_deny_1";
    const tools::Tool::Result result =
        agent::RunOneTool(registry, MakeCall("u_deny_1"), wiring, nullptr, std::string(), &ctx);
    REQUIRE(result.is_error);
    CHECK(result.error_code == agent::kErrPermissionNoPromptDenied);
    CHECK(result.outcome == agent::ToString(agent::ToolOutcome::PermissionDeclined));
    CHECK(gated_ptr->executions == 0);  // 拒绝 = 不执行
    // 没越过执行边界:无 started;终态 finished 恰一份(闸前收口)。
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionStarted) == 0);
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionFinished) == 1);
    CHECK(collector.events.back().outcome == agent::ToolOutcome::PermissionDeclined);
}

TEST_CASE("拆链:确认口回 false(用户拒绝)同样不执行") {
    tools::ToolRegistry registry;
    auto gated = std::make_unique<GatedTool>();
    GatedTool* gated_ptr = gated.get();
    registry.Register(std::move(gated));

    TraceCollector collector;
    agent::TurnWiring wiring = collector.Decorate({});
    wiring.on_tool_confirm = [](const std::string&, const std::string&, const nlohmann::json&) {
        return false;
    };

    agent::ToolTraceContext ctx;
    ctx.execution_id = "e_decline_1";
    const tools::Tool::Result result =
        agent::RunOneTool(registry, MakeCall("u_decline_1"), wiring, nullptr, std::string(), &ctx);
    REQUIRE(result.is_error);
    CHECK(result.error_code == agent::kErrPermissionDeclined);
    CHECK(gated_ptr->executions == 0);
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionStarted) == 0);
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionFinished) == 1);
}

// ---------------------------------------------------------------------------
// 逐调用审批账:两枚调用裁决不同不串槽
// ---------------------------------------------------------------------------

TEST_CASE("逐调用账:两枚调用各取各的,谁也不顶谁") {
    app::ToolCallScopeTable table;
    runtime::ToolHookDecision deny_a;
    deny_a.decision = runtime::ToolHookDecision::Decision::Deny;
    deny_a.reason = "deny-A";
    table.RecordPre("toolu_A", deny_a);
    table.RecordApprovalClass("toolu_B", tools::ApprovalClass::FileEdit);

    // B 的审批类别没把 A 的表态顶掉,A 的表态也没渗进 B。
    const app::ToolCallScope scope_b = table.Take("toolu_B");
    CHECK(scope_b.pre.decision == runtime::ToolHookDecision::Decision::None);
    CHECK(scope_b.approval_class == tools::ApprovalClass::FileEdit);
    const app::ToolCallScope scope_a = table.Take("toolu_A");
    CHECK(scope_a.pre.reason == "deny-A");
    CHECK(scope_a.approval_class == tools::ApprovalClass::None);

    // 取走即清:再来取同名 id,回缺省(不残留、不冒充)。
    const app::ToolCallScope scope_b_again = table.Take("toolu_B");
    CHECK(scope_b_again.pre.decision == runtime::ToolHookDecision::Decision::None);
    CHECK(scope_b_again.approval_class == tools::ApprovalClass::None);
}

TEST_CASE("拆链:两枚调用裁决不同,确认口各拿各的表态与类别(不串槽)") {
    tools::ToolRegistry registry;
    auto gated = std::make_unique<GatedTool>();
    GatedTool* gated_ptr = gated.get();
    registry.Register(std::move(gated));

    // 照 turn_runner 的装配法:PreToolUse/预裁定写逐调用账,确认口取自己
    // 那枚。两枚调用给不同的钩子表态理由,确认口只认自己 id 的那份。
    auto table = std::make_shared<app::ToolCallScopeTable>();
    agent::TurnWiring wiring;
    wiring.on_pre_tool_use_hook = [table](const std::string& tool_use_id, const std::string&,
                                          const nlohmann::json&) -> runtime::ToolHookDecision {
        runtime::ToolHookDecision decision;
        decision.decision = runtime::ToolHookDecision::Decision::Ask;
        decision.reason = "ask-" + tool_use_id;
        table->RecordPre(tool_use_id, decision);
        return decision;
    };
    wiring.on_permission_evaluate = [table](const std::string& tool_use_id, const std::string&,
                                            tools::ApprovalClass approval_class, const nlohmann::json&,
                                            const runtime::ToolHookDecision&) -> runtime::PermissionVerdict {
        table->RecordApprovalClass(tool_use_id, approval_class);
        runtime::PermissionVerdict verdict;
        verdict.action = runtime::PermissionVerdict::Action::Ask;
        return verdict;
    };
    wiring.on_tool_confirm = [table](const std::string& tool_use_id, const std::string&,
                                     const nlohmann::json&) -> bool {
        const app::ToolCallScope scope = table->Take(tool_use_id);
        // 串槽即假:理由必须是自己 id 的那份,类别必须是本工具真报的。
        return scope.pre.reason == "ask-" + tool_use_id &&
               scope.approval_class == tools::ApprovalClass::FileEdit;
    };

    const tools::Tool::Result first =
        agent::RunOneTool(registry, MakeCall("toolu_first"), wiring, nullptr);
    CHECK_FALSE(first.is_error);
    const tools::Tool::Result second =
        agent::RunOneTool(registry, MakeCall("toolu_second", "b.txt"), wiring, nullptr);
    CHECK_FALSE(second.is_error);
    REQUIRE(gated_ptr->executions == 2);  // 两枚的确认都凭自己的账放行
}

TEST_CASE("拆链:没跑过钩子的调用,确认口拿缺省表态(workflow 借来的确认口同款)") {
    app::ToolCallScopeTable table;
    const app::ToolCallScope scope = table.Take("never_seen");
    CHECK(scope.pre.decision == runtime::ToolHookDecision::Decision::None);
    CHECK(scope.pre.updated_input.has_value() == false);
    CHECK(scope.approval_class == tools::ApprovalClass::None);
}

// ---------------------------------------------------------------------------
// Hook 改参后,权限链拿最终参数
// ---------------------------------------------------------------------------

TEST_CASE("拆链:PreToolUse 改参后,预裁定与确认口拿的都是改写后的最终参数") {
    tools::ToolRegistry registry;
    auto gated = std::make_unique<GatedTool>();
    GatedTool* gated_ptr = gated.get();
    registry.Register(std::move(gated));

    nlohmann::json evaluated_input;
    nlohmann::json confirmed_input;
    agent::TurnWiring wiring;
    wiring.on_pre_tool_use_hook = [](const std::string&, const std::string&,
                                     const nlohmann::json&) -> runtime::ToolHookDecision {
        runtime::ToolHookDecision decision;
        decision.decision = runtime::ToolHookDecision::Decision::Allow;
        decision.updated_input = nlohmann::json{{"path", "rewritten.txt"}};
        return decision;
    };
    wiring.on_permission_evaluate = [&evaluated_input](const std::string&, const std::string&,
                                                       tools::ApprovalClass, const nlohmann::json& input,
                                                       const runtime::ToolHookDecision&)
                                       -> runtime::PermissionVerdict {
        evaluated_input = input;
        runtime::PermissionVerdict verdict;
        verdict.action = runtime::PermissionVerdict::Action::Ask;  // 真要问,好捕获确认口参数
        return verdict;
    };
    wiring.on_tool_confirm = [&confirmed_input](const std::string&, const std::string&,
                                                const nlohmann::json& input) -> bool {
        confirmed_input = input;
        return true;
    };

    const tools::Tool::Result result =
        agent::RunOneTool(registry, MakeCall("u_rewrite_1", "original.txt"), wiring, nullptr);
    CHECK_FALSE(result.is_error);
    REQUIRE(gated_ptr->executions == 1);
    // 三个口拿的都是改写后的最终参数,不是原始入参。
    CHECK(evaluated_input == nlohmann::json{{"path", "rewritten.txt"}});
    CHECK(confirmed_input == nlohmann::json{{"path", "rewritten.txt"}});
    CHECK(gated_ptr->last_input == nlohmann::json{{"path", "rewritten.txt"}});
    CHECK(result.content == "wrote rewritten.txt");
}

// ---------------------------------------------------------------------------
// 取消:执行中可见,终态恰一份
// ---------------------------------------------------------------------------

TEST_CASE("拆链:执行中取消旗可见,单枚调用终态仍恰一份") {
    tools::ToolRegistry registry;
    auto probe = std::make_unique<ProbeTool>();
    ProbeTool* probe_ptr = probe.get();
    registry.Register(std::move(probe));

    TraceCollector collector;
    collector.probe = probe_ptr;
    agent::TurnWiring wiring = collector.Decorate({});
    // 旗在调用前就置位:RunOneTool 当口不查旗(取消在批次层逐枚查),工具
    // 在执行里看见真值并按能力收口——这是拆链前的既有行为,钉住不动。
    std::atomic<bool> cancel_flag{true};
    agent::ToolTraceContext ctx;
    ctx.execution_id = "e_cancel_1";

    api::ToolUseBlock call;
    call.id = "u_cancel_1";
    call.name = "probe_read";
    const tools::Tool::Result result =
        agent::RunOneTool(registry, call, wiring, nullptr, std::string(), &ctx, &cancel_flag);
    CHECK_FALSE(result.is_error);
    REQUIRE(probe_ptr->executions == 1);
    CHECK(probe_ptr->cancel_value_at_execute);
    // 取消不双收口:started/finished 仍各恰一份。
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionStarted) == 1);
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionFinished) == 1);
}

// ---------------------------------------------------------------------------
// 结果捕获失败:不冒充成功,原始终态先落账
// ---------------------------------------------------------------------------

TEST_CASE("拆链:结果捕获失败按 result_store_failed 收口,finished 先落一份") {
    tools::ToolRegistry registry;
    auto gated = std::make_unique<GatedTool>();
    GatedTool* gated_ptr = gated.get();
    registry.Register(std::move(gated));

    TraceCollector collector;
    agent::TurnWiring wiring = collector.Decorate({});
    wiring.on_permission_evaluate = [](const std::string&, const std::string&, tools::ApprovalClass,
                                       const nlohmann::json&,
                                       const runtime::ToolHookDecision&) -> runtime::PermissionVerdict {
        runtime::PermissionVerdict verdict;
        verdict.action = runtime::PermissionVerdict::Action::Allow;
        return verdict;
    };
    wiring.capture_tool_result = [](const api::ToolResultBlock&) -> runtime::ToolResultsCommitReceipt {
        runtime::ToolResultsCommitReceipt receipt;
        receipt.status = runtime::ToolResultsCommitReceipt::Status::Failed;
        receipt.error_code = "tool.result.store_unavailable";
        return receipt;
    };

    agent::ToolTraceContext ctx;
    ctx.execution_id = "e_capture_1";
    const tools::Tool::Result result =
        agent::RunOneTool(registry, MakeCall("u_capture_1"), wiring, nullptr, std::string(), &ctx);
    REQUIRE(result.is_error);  // 落不住就明败,不拿内存里的结果冒充已提交
    CHECK(result.error_code == "tool.result.store_unavailable");
    CHECK(result.outcome == agent::ToString(agent::ToolOutcome::ResultStoreFailed));
    REQUIRE(gated_ptr->executions == 1);
    // 工具确实执行过:原始 finished(成功终态)在捕获失败之前已落账,
    // 恰一份;捕获失败不补第二枚 finished。
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionStarted) == 1);
    CHECK(collector.Count(agent::ToolTraceEventKind::ExecutionFinished) == 1);
    CHECK(collector.events.back().outcome == agent::ToolOutcome::Succeeded);
}
