// GoalEvidence 适配器实现(见 goal_evidence.hpp 的合同注释)。

#include "runtime/goal_evidence.hpp"

#include <utility>

namespace lubancode::runtime::goal {

EvidenceKind EvidenceKindForTool(const std::string& tool_name, agent::ToolSourceKind source) {
    if (tool_name == "run_command") {
        return EvidenceKind::CommandExit;
    }
    if (tool_name == "write_file" || tool_name == "edit_file") {
        return EvidenceKind::FileDigest;
    }
    if (source == agent::ToolSourceKind::Mcp) {
        return EvidenceKind::ToolResult;
    }
    return EvidenceKind::ToolResult;
}

bool EvidenceStalesOnWrite(EvidenceKind kind) {
    // 写盘之后旧验证不再可信(单子"证据涉及改动后,旧 validation 要按
    // 影响范围翻 stale"):CommandExit/TestReport 标旧;FileDigest 是改动
    // 事实本身、UserDecision/RuntimeError 与工作区无关,不动。
    return kind == EvidenceKind::CommandExit || kind == EvidenceKind::TestReport;
}

std::optional<GoalEvidence> EvidenceFromToolTrace(const agent::ToolTraceEvent& event,
                                                 const GoalEvidenceContext& context,
                                                 const std::string& evidence_id) {
    // 只认 finished:结果事实落齐了才算证据(started 阶段采出来是空壳)。
    if (event.kind != agent::ToolTraceEventKind::ExecutionFinished) {
        return std::nullopt;
    }
    if (context.goal_id.empty() || context.iteration_id.empty()) {
        return std::nullopt;  // 不在 goal turn:不采
    }
    GoalEvidence evidence;
    evidence.id = evidence_id;
    evidence.kind = EvidenceKindForTool(event.tool_name, event.source_kind);
    evidence.goal_id = context.goal_id;
    evidence.iteration_id = context.iteration_id;
    evidence.tool_use_id = event.tool_use_id;
    evidence.producer = event.parent_execution_id.empty() ? event.tool_name
                                                          : "subagent:" + event.tool_name;

    // 结构化事实:按单子的分档记,不抄整段正文进 goal 事件。
    nlohmann::json& facts = evidence.facts;
    facts["tool"] = event.tool_name;
    facts["execution_id"] = event.execution_id;
    if (!event.parent_execution_id.empty()) {
        facts["parent_execution_id"] = event.parent_execution_id;  // 子代理归属边
    }
    facts["outcome"] = agent::ToString(event.outcome);
    if (!event.error_code.empty()) {
        facts["error_code"] = event.error_code;
    }
    facts["duration_ms"] = event.duration_ms;
    facts["source"] = agent::ToString(event.source_kind);
    if (!event.source_instance.empty()) {
        facts["source_instance"] = event.source_instance;  // MCP server/LSP 名
    }
    // 结果引用:hash/字节/预览摘要——不复制正文(单子:"command stdout 不
    // 整段复制到 goal event;存 digest、必要摘要、artifact ref")。
    facts["result_sha256"] = event.result_ref.sha256;
    facts["result_bytes"] = event.result_ref.bytes;
    if (!event.result_ref.artifact_id.empty()) {
        facts["result_artifact_id"] = event.result_ref.artifact_id;
    }
    facts["truncated"] = event.result_ref.kind == agent::ToolResultRef::Kind::Unavailable;

    // 内容锚:结果 hash 当证据的 content_sha256(新鲜度与防伪对账用它;
    // unavailable 时退 execution_id 的串,不冒充空 hash)。
    evidence.content_sha256 = !event.result_ref.sha256.empty()
                                  ? event.result_ref.sha256
                                  : "exec:" + event.execution_id;
    evidence.observed_at_ms = 0;  // 装配层落账时补墙钟
    evidence.fresh = true;
    evidence.truncated = event.result_ref.kind == agent::ToolResultRef::Kind::Unavailable;
    return evidence;
}

}  // namespace lubancode::runtime::goal
