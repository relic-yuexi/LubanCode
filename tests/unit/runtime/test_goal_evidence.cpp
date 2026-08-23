// goal 单合流接线:ToolTraceHub 事件 -> GoalEvidence 的采证适配器。
// 纯函数钉:只认 finished、无 goal 上下文不采、工具名分档、结果 hash 锚、
// 子代理 parent 边、写盘后旧验证翻 stale 的分档表。

#include <doctest/doctest.h>

#include <string>

#include "agent/tool_trace.hpp"
#include "runtime/goal_evidence.hpp"
#include "runtime/goal_types.hpp"

using lubancode::agent::EffectClass;
using lubancode::agent::ToolOutcome;
using lubancode::agent::ToolResultRef;
using lubancode::agent::ToolSourceKind;
using lubancode::agent::ToolTraceEvent;
using lubancode::agent::ToolTraceEventKind;
using lubancode::runtime::goal::EvidenceFromToolTrace;
using lubancode::runtime::goal::EvidenceKind;
using lubancode::runtime::goal::EvidenceKindForTool;
using lubancode::runtime::goal::EvidenceStalesOnWrite;
using lubancode::runtime::goal::GoalEvidenceContext;

namespace {

ToolTraceEvent FinishedEvent() {
    ToolTraceEvent event;
    event.kind = ToolTraceEventKind::ExecutionFinished;
    event.turn_id = "turn-1";
    event.execution_id = "item-9";
    event.tool_use_id = "toolu_1";
    event.tool_name = "run_command";
    event.outcome = ToolOutcome::Succeeded;
    event.duration_ms = 1200;
    event.result_ref.kind = ToolResultRef::Kind::Inline;
    event.result_ref.sha256 = "abc123";
    event.result_ref.bytes = 4096;
    return event;
}

GoalEvidenceContext Ctx() { return {"goal-1", "goal-1/iter-2", "turn-1"}; }

}  // namespace

TEST_CASE("只认 ExecutionFinished:started/scheduled 不产证据") {
    for (const ToolTraceEventKind kind :
         {ToolTraceEventKind::Scheduled, ToolTraceEventKind::ExecutionStarted,
          ToolTraceEventKind::ResultCommitted}) {
        ToolTraceEvent event = FinishedEvent();
        event.kind = kind;
        CHECK(EvidenceFromToolTrace(event, Ctx(), "ev-1") == std::nullopt);
    }
    CHECK(EvidenceFromToolTrace(FinishedEvent(), Ctx(), "ev-1").has_value());
}

TEST_CASE("无 goal 上下文不采(普通 turn 不喂证据账)") {
    GoalEvidenceContext no_goal;
    no_goal.iteration_id = "goal-1/iter-2";
    CHECK(EvidenceFromToolTrace(FinishedEvent(), no_goal, "ev-1") == std::nullopt);
    GoalEvidenceContext no_iter;
    no_iter.goal_id = "goal-1";
    CHECK(EvidenceFromToolTrace(FinishedEvent(), no_iter, "ev-1") == std::nullopt);
}

TEST_CASE("工具名分档:run_command 是 CommandExit,写盘是 FileDigest") {
    CHECK(EvidenceKindForTool("run_command", ToolSourceKind::Builtin) == EvidenceKind::CommandExit);
    CHECK(EvidenceKindForTool("write_file", ToolSourceKind::Builtin) == EvidenceKind::FileDigest);
    CHECK(EvidenceKindForTool("edit_file", ToolSourceKind::Builtin) == EvidenceKind::FileDigest);
    CHECK(EvidenceKindForTool("read_file", ToolSourceKind::Builtin) == EvidenceKind::ToolResult);
    CHECK(EvidenceKindForTool("deploy", ToolSourceKind::Mcp) == EvidenceKind::ToolResult);
}

TEST_CASE("采出的证据:身份/事实/内容锚各就各位") {
    const auto evidence = EvidenceFromToolTrace(FinishedEvent(), Ctx(), "ev-7");
    REQUIRE(evidence.has_value());
    CHECK(evidence->id == "ev-7");
    CHECK(evidence->goal_id == "goal-1");
    CHECK(evidence->iteration_id == "goal-1/iter-2");
    CHECK(evidence->tool_use_id == "toolu_1");
    CHECK(evidence->kind == EvidenceKind::CommandExit);
    CHECK(evidence->producer == "run_command");
    CHECK(evidence->content_sha256 == "abc123");
    CHECK(evidence->fresh);
    CHECK_FALSE(evidence->truncated);
    CHECK(evidence->facts.value("tool", std::string()) == "run_command");
    CHECK(evidence->facts.value("execution_id", std::string()) == "item-9");
    CHECK(evidence->facts.value("outcome", std::string()) == "succeeded");
    CHECK(evidence->facts.value("duration_ms", 0) == 1200);
    CHECK(evidence->facts.value("result_sha256", std::string()) == "abc123");
    CHECK(evidence->facts.value("result_bytes", 0) == 4096);
    CHECK_FALSE(evidence->facts.contains("result_artifact_id"));
}

TEST_CASE("子代理内层事件:producer 带 subagent 前缀,parent 边入事实") {
    ToolTraceEvent event = FinishedEvent();
    event.parent_execution_id = "item-3";
    event.tool_name = "read_file";
    event.effect_class = EffectClass::ReadOnlyLocal;
    const auto evidence = EvidenceFromToolTrace(event, Ctx(), "ev-8");
    REQUIRE(evidence.has_value());
    CHECK(evidence->producer == "subagent:read_file");
    CHECK(evidence->facts.value("parent_execution_id", std::string()) == "item-3");
}

TEST_CASE("结果引用 unavailable:truncated 标记立起,内容锚退 execution id") {
    ToolTraceEvent event = FinishedEvent();
    event.result_ref.kind = ToolResultRef::Kind::Unavailable;
    event.result_ref.sha256.clear();
    const auto evidence = EvidenceFromToolTrace(event, Ctx(), "ev-9");
    REQUIRE(evidence.has_value());
    CHECK(evidence->truncated);
    CHECK(evidence->content_sha256 == "exec:item-9");
    CHECK(evidence->facts.value("truncated", false) == true);
}

TEST_CASE("artifact 引用:大结果只带 id,不抄正文") {
    ToolTraceEvent event = FinishedEvent();
    event.result_ref.kind = ToolResultRef::Kind::Artifact;
    event.result_ref.artifact_id = "art-5";
    const auto evidence = EvidenceFromToolTrace(event, Ctx(), "ev-10");
    REQUIRE(evidence.has_value());
    CHECK(evidence->facts.value("result_artifact_id", std::string()) == "art-5");
    CHECK_FALSE(evidence->facts.contains("result_preview"));
}

TEST_CASE("写盘后的 stale 分档:旧验证标旧,改动事实与用户决定不动") {
    CHECK(EvidenceStalesOnWrite(EvidenceKind::CommandExit));
    CHECK(EvidenceStalesOnWrite(EvidenceKind::TestReport));
    CHECK_FALSE(EvidenceStalesOnWrite(EvidenceKind::FileDigest));
    CHECK_FALSE(EvidenceStalesOnWrite(EvidenceKind::ToolResult));
    CHECK_FALSE(EvidenceStalesOnWrite(EvidenceKind::UserDecision));
    CHECK_FALSE(EvidenceStalesOnWrite(EvidenceKind::RuntimeError));
}
