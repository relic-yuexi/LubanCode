// 五路只读 adapter 的实现(输入口径见 adapters.hpp 头注释)。全部纯函数:
// 只读入参、只产观察;账本里没有的字段不编,消息正文与思考原文一处不读。

#include "evolution/adapters.hpp"

#include <map>
#include <set>
#include <utility>

#include "platform/paths.hpp"  // PathToUtf8:路径进观察账一律走 UTF-8 窄边界

namespace lubancode::evolution {

namespace {

std::string GetStr(const nlohmann::json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j.at(key).is_string()) {
        return j.at(key).get<std::string>();
    }
    return std::string();
}

// 连续同名折叠的序列(工具名/节点名):"a a b a" -> "a,b,a"。重复不删净,
// 只折相邻——次数与顺序都是"形状"的一部分。
std::vector<std::string> FoldConsecutive(const std::vector<std::string>& names) {
    std::vector<std::string> folded;
    for (const std::string& name : names) {
        if (name.empty()) continue;
        if (!folded.empty() && folded.back() == name) continue;
        folded.push_back(name);
    }
    return folded;
}

std::string JoinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) out.push_back(',');
        out += name;
    }
    return out;
}

// 一枚观察的证据引用与来源指回。
EvidenceRef MakeRef(std::string ref, std::string note) {
    EvidenceRef evidence;
    evidence.ref = std::move(ref);
    evidence.note = std::move(note);
    return evidence;
}

}  // namespace

// ---------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------

std::vector<EvolutionObservation> ObservationsFromRecording(const RecordingMaterial& material) {
    std::vector<EvolutionObservation> out;
    bool finished = false;
    std::string goal;
    std::vector<std::string> variables;      // 变量名,不是值(口径:口述里"哪些值每回都会变")
    std::string acceptance;
    std::string verification;
    bool has_verification = false;
    std::vector<std::string> tool_names;
    std::size_t tool_error_count = 0;
    std::int64_t last_ts_seq = 0;
    std::string created_at;

    for (const skills::RecordEvent& event : material.events) {
        if (event.seq > last_ts_seq) {
            last_ts_seq = event.seq;
            if (!event.ts.empty()) created_at = event.ts;
        }
        if (event.type == skills::kEventRecordStart) {
            goal = GetStr(event.data, "goal");
            acceptance = GetStr(event.data, "acceptance");
            if (event.data.contains("variables") && event.data.at("variables").is_array()) {
                for (const auto& item : event.data.at("variables")) {
                    if (item.is_string()) variables.push_back(item.get<std::string>());
                }
            }
        } else if (event.type == skills::kEventToolCall) {
            tool_names.push_back(GetStr(event.data, "tool"));
        } else if (event.type == skills::kEventToolResult) {
            if (event.data.contains("ok") && event.data.at("ok").is_boolean() &&
                !event.data.at("ok").get<bool>()) {
                ++tool_error_count;
            }
        } else if (event.type == skills::kEventVerification) {
            verification = GetStr(event.data, "text");
            has_verification = true;
        } else if (event.type == skills::kEventRecordStop) {
            finished = true;
        }
    }
    if (!finished) {
        return out;  // 半截录制不是完整示范;draft 都装不进 skills,观察也不收
    }

    EvolutionObservation observation;
    observation.source = ObservationSource::Recording;
    observation.source_id = material.status.id;
    observation.source_ref = lubancode::platform::PathToUtf8(material.status.dir);
    // 没有验证口述的"成功"不收(契约:没有产物证据的成功不收)——outcome 落
    // unknown,材料仍留账,起草阶段再由人判。
    observation.outcome = has_verification ? ObservationOutcome::Success
                                           : ObservationOutcome::Unknown;

    const std::vector<std::string> folded_tools = FoldConsecutive(tool_names);
    nlohmann::json details;
    details["name"] = material.status.name;
    details["goal"] = SanitizeObservationText(goal, 400);
    if (!variables.empty()) {
        details["variables"] = variables;
    }
    if (!acceptance.empty()) {
        details["acceptance"] = SanitizeObservationText(acceptance, 200);
    }
    if (has_verification) {
        details["verification"] = SanitizeObservationText(verification, 200);
    }
    details["tools"] = folded_tools;
    details["tool_call_count"] = tool_names.size();
    details["tool_error_count"] = tool_error_count;
    observation.details = SanitizeObservationJson(details);

    observation.summary = SanitizeObservationText(
        "录制 " + material.status.name + ":" + goal.substr(0, 80) + ";工具 " +
            std::to_string(tool_names.size()) + " 步" +
            (has_verification ? ";已验证" : ";未验证"),
        200);
    observation.fingerprint = ComputeFingerprint(
        ObservationSource::Recording,
        NormalizeShapeText(goal) + "\n" + NormalizeShapeText(acceptance) + "\n" +
            JoinNames(folded_tools));
    observation.id = MakeObservationId(observation.source, observation.source_id);
    observation.created_at = created_at;
    observation.evidence.push_back(
        MakeRef(lubancode::platform::PathToUtf8(material.status.dir) + "/events.jsonl", "录制事件流(goal/验收/验证/工具)"));
    out.push_back(std::move(observation));
    return out;
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

std::vector<EvolutionObservation> ObservationsFromRun(const workflow::RunStatus& run,
                                                      const std::vector<workflow::JournalEvent>& events) {
    std::vector<EvolutionObservation> out;
    std::vector<std::string> node_names;
    std::size_t retry_count = 0;
    std::size_t checkpoint_count = 0;
    for (const workflow::JournalEvent& event : events) {
        if (event.type == workflow::kEventNodeStarted && !event.node_id.empty()) {
            node_names.push_back(event.node_id);
        } else if (event.type == workflow::kEventNodeRetrying) {
            ++retry_count;
        } else if (event.type == workflow::kEventCheckpointSaved) {
            ++checkpoint_count;
        }
    }

    EvolutionObservation observation;
    observation.source = ObservationSource::Run;
    observation.source_id = run.run_id;
    observation.source_ref = lubancode::platform::PathToUtf8(run.dir);
    // 终态分档:succeeded=success;failed=failure;空(没跑完)=unknown;
    // 其余(cancelled/interrupted/budget_exhausted 一类)=partial。
    if (run.final_state == "succeeded") {
        observation.outcome = ObservationOutcome::Success;
    } else if (run.final_state == "failed") {
        observation.outcome = ObservationOutcome::Failure;
    } else if (run.final_state.empty()) {
        observation.outcome = ObservationOutcome::Unknown;
    } else {
        observation.outcome = ObservationOutcome::Partial;
    }

    const std::vector<std::string> folded_nodes = FoldConsecutive(node_names);
    nlohmann::json details;
    details["workflow_id"] = run.workflow_id;
    if (!run.workflow_version.empty()) {
        details["workflow_version"] = run.workflow_version;
    }
    if (!run.content_hash.empty()) {
        details["content_hash"] = run.content_hash;
    }
    details["final_state"] = run.final_state.empty() ? "(未跑完)" : run.final_state;
    details["nodes"] = folded_nodes;
    details["node_count"] = node_names.size();
    details["retry_count"] = retry_count;
    details["checkpoint_count"] = checkpoint_count;
    observation.details = SanitizeObservationJson(details);

    observation.summary = SanitizeObservationText(
        "Workflow run " + run.workflow_id + "(" + run.workflow_version + ")" +
            (run.final_state.empty() ? " 未收场" : " " + run.final_state) + ",节点 " +
            std::to_string(node_names.size()) + " 个" +
            (retry_count > 0 ? ",重试 " + std::to_string(retry_count) + " 次" : ""),
        200);
    observation.fingerprint = ComputeFingerprint(
        ObservationSource::Run,
        run.workflow_id + "|" + (run.final_state.empty() ? "(open)" : run.final_state) + "|" +
            JoinNames(folded_nodes));
    observation.id = MakeObservationId(observation.source, observation.source_id);
    observation.created_at = run.started_at;
    observation.evidence.push_back(MakeRef(lubancode::platform::PathToUtf8(run.dir) + "/manifest.json", "run 簿记与终态"));
    observation.evidence.push_back(MakeRef(lubancode::platform::PathToUtf8(run.dir) + "/events.jsonl", "run 事件流"));
    out.push_back(std::move(observation));
    return out;
}

// ---------------------------------------------------------------------------
// goal
// ---------------------------------------------------------------------------

namespace {

// 一只 goal 的折叠账(事件流 -> 单条观察的中间态)。
struct GoalFold {
    std::string goal_id;
    std::string objective;
    int last_revision = 0;
    std::set<std::string> iterations;
    std::set<std::string> evidence_kinds;
    std::size_t evidence_count = 0;
    std::string last_decision;
    std::string last_verdict;
    bool overridden_achieved = false;
    std::string terminal_event;  // blocked / budget_exhausted / cleared(可空)
    std::int64_t last_ts_ms = 0;
};

}  // namespace

std::vector<EvolutionObservation> ObservationsFromGoalEvents(
    const std::string& session_file_utf8, const std::vector<sessions::GoalSessionEvent>& events) {
    std::vector<EvolutionObservation> out;
    std::map<std::string, GoalFold> folds;
    for (const sessions::GoalSessionEvent& event : events) {
        GoalFold& fold = folds[event.goal_id];
        fold.goal_id = event.goal_id;
        if (event.timestamp_ms > fold.last_ts_ms) {
            fold.last_ts_ms = event.timestamp_ms;
        }
        if (event.revision > fold.last_revision) {
            fold.last_revision = event.revision;
        }
        if (event.type == "goal_v1" && (event.event == "created" || event.event == "edited")) {
            // created/edited 各带完整 objective;按事件序,后者(更高 revision)胜。
            const std::string objective = GetStr(event.payload, "objective");
            if (!objective.empty()) {
                fold.objective = objective;
            }
        } else if (event.type == "goal_iteration_v1") {
            if (!event.iteration_id.empty()) {
                fold.iterations.insert(event.iteration_id);
            }
        } else if (event.type == "goal_evidence_v1") {
            ++fold.evidence_count;
            std::string kind = GetStr(event.payload, "kind");
            if (kind.empty()) {
                kind = "(unknown)";
            }
            fold.evidence_kinds.insert(kind);
        } else if (event.type == "goal_evaluation_v1") {
            if (event.payload.contains("evaluation") && event.payload.at("evaluation").is_object()) {
                const nlohmann::json& evaluation = event.payload.at("evaluation");
                const std::string decision = GetStr(evaluation, "decision");
                if (!decision.empty()) {
                    fold.last_decision = decision;
                }
                const std::string summary = GetStr(evaluation, "summary");
                if (!summary.empty()) {
                    fold.last_verdict = summary;
                }
                if (evaluation.contains("overridden_achieved") &&
                    evaluation.at("overridden_achieved").is_boolean()) {
                    fold.overridden_achieved =
                        evaluation.at("overridden_achieved").get<bool>() || fold.overridden_achieved;
                }
            }
        } else if (event.type == "goal_v1" &&
                   (event.event == "blocked" || event.event == "budget_exhausted" ||
                    event.event == "cleared")) {
            fold.terminal_event = event.event;
        }
    }

    for (const auto& [goal_id, fold] : folds) {
        EvolutionObservation observation;
        observation.source = ObservationSource::Goal;
        observation.source_id = goal_id;
        observation.source_ref = session_file_utf8;
        // 判词分档:achieved=success;blocked/budget_exhausted=failure;
        // continue/needs_user=partial;从未评估且无终态=unknown。
        // "终点判词"是 evaluator 的结构化 decision(+一行 summary),单收这一
        // 句;模型执行轮的思考原文不在此路(它根本不在 goal 事件里)。
        if (fold.last_decision == "achieved") {
            observation.outcome = ObservationOutcome::Success;
        } else if (fold.last_decision == "blocked" ||
                   fold.terminal_event == "budget_exhausted") {
            observation.outcome = ObservationOutcome::Failure;
        } else if (fold.last_decision == "continue" || fold.last_decision == "needs_user") {
            observation.outcome = ObservationOutcome::Partial;
        } else if (fold.terminal_event == "cleared") {
            observation.outcome = ObservationOutcome::Partial;  // 用户主动摘掉,不是失败
        } else {
            observation.outcome = ObservationOutcome::Unknown;
        }

        nlohmann::json details;
        details["objective"] = SanitizeObservationText(fold.objective, 400);
        details["revision"] = fold.last_revision;
        details["iteration_count"] = fold.iterations.size();
        details["evidence_count"] = fold.evidence_count;
        details["evidence_kinds"] = std::vector<std::string>(fold.evidence_kinds.begin(),
                                                             fold.evidence_kinds.end());
        if (!fold.last_decision.empty()) {
            details["last_decision"] = fold.last_decision;
        }
        if (!fold.last_verdict.empty()) {
            details["verdict"] = SanitizeObservationText(fold.last_verdict, 200);
        }
        if (fold.overridden_achieved) {
            details["overridden_achieved"] = true;  // evaluator 说成了、硬门槛不够——如实留痕
        }
        if (!fold.terminal_event.empty()) {
            details["terminal_event"] = fold.terminal_event;
        }
        observation.details = SanitizeObservationJson(details);

        observation.summary = SanitizeObservationText(
            "goal " + goal_id + ":" + fold.objective.substr(0, 80) + ";迭代 " +
                std::to_string(fold.iterations.size()) + " 轮,证据 " +
                std::to_string(fold.evidence_count) + " 条" +
                (fold.last_decision.empty() ? "" : ",判词 " + fold.last_decision),
            200);
        observation.fingerprint = ComputeFingerprint(
            ObservationSource::Goal,
            NormalizeShapeText(fold.objective) + "|" +
                (fold.last_decision.empty() ? "(open)" : fold.last_decision));
        observation.id = MakeObservationId(observation.source, observation.source_id);
        observation.created_at = FormatEpochMsLocal(fold.last_ts_ms);
        observation.evidence.push_back(
            MakeRef(session_file_utf8, "goal_v1 事件族(objective/iteration/evidence/判词)"));
        out.push_back(std::move(observation));
    }
    return out;
}

// ---------------------------------------------------------------------------
// tooltrace
// ---------------------------------------------------------------------------

std::vector<EvolutionObservation> ObservationsFromToolTrace(
    const std::string& session_file_utf8, const agent::ToolExecutionLedger& ledger) {
    std::vector<EvolutionObservation> out;
    for (const agent::ToolExecutionRecord& record : ledger.executions()) {
        if (!record.has_finished || record.outcome == agent::ToolOutcome::Succeeded) {
            continue;  // 只收明确失败;没跑完的execution连结论都没有
        }
        if (record.outcome == agent::ToolOutcome::CancelledDuringRun) {
            continue;  // 用户收掉的是会话事实,不是可复用的失败路
        }
        EvolutionObservation observation;
        observation.source = ObservationSource::ToolTrace;
        observation.source_id = record.execution_id;
        observation.source_ref = session_file_utf8;
        observation.outcome = ObservationOutcome::Failure;

        nlohmann::json details;
        details["tool"] = record.tool_name;
        details["outcome"] = agent::ToString(record.outcome);
        if (!record.error_code.empty()) {
            details["error_code"] = record.error_code;
        }
        details["duration_ms"] = record.duration_ms;
        details["result_bytes"] = record.result_ref.bytes;
        if (!record.turn_id.empty()) {
            details["turn_id"] = record.turn_id;
        }
        observation.details = SanitizeObservationJson(details);

        observation.summary = SanitizeObservationText(
            "工具 " + record.tool_name + " " + agent::ToString(record.outcome) +
                (record.error_code.empty() ? "" : " (" + record.error_code + ")"),
            200);
        // 指纹不收 effective_input_sha256:它是内容指纹,同形不同值的调用对
        // 不上号,同类聚合就失效了(口径见 observation.hpp)。
        observation.fingerprint = ComputeFingerprint(
            ObservationSource::ToolTrace,
            record.tool_name + "|" +
                (record.error_code.empty() ? agent::ToString(record.outcome) : record.error_code));
        observation.id = MakeObservationId(observation.source, observation.source_id);
        observation.created_at = std::string();  // 折叠账不留时间戳;追根看 session 档
        observation.evidence.push_back(MakeRef(session_file_utf8,
                                               "tool_trace_v1 折叠账 execution " + record.execution_id));
        out.push_back(std::move(observation));
    }
    return out;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

std::vector<EvolutionObservation> ObservationsFromMemory(const std::vector<memory::MemoryEntry>& entries,
                                                         const std::string& layer_label,
                                                         const std::string& dir_utf8) {
    std::vector<EvolutionObservation> out;
    for (const memory::MemoryEntry& entry : entries) {
        if (entry.status != "active") {
            continue;  // archived/conflict 不收;候选审阅箱压根不进这里(口径见头注释)
        }
        EvolutionObservation observation;
        observation.source = ObservationSource::Memory;
        observation.source_id = entry.id;
        observation.source_ref = dir_utf8;
        // 记忆条目不是任务,没有成败——unknown 是如实,不冒充 success。
        observation.outcome = ObservationOutcome::Unknown;

        nlohmann::json details;
        details["kind"] = memory::MemoryKindName(entry.kind);
        details["title"] = SanitizeObservationText(entry.title, 120);
        if (!entry.summary.empty()) {
            details["summary"] = SanitizeObservationText(entry.summary, 200);
        }
        if (!entry.confidence.empty()) {
            details["confidence"] = entry.confidence;
        }
        if (!entry.keywords.empty()) {
            details["keywords"] = entry.keywords;
        }
        if (!entry.scope.kind.empty()) {
            details["scope"] = entry.scope.kind;
        }
        details["layer"] = layer_label;
        observation.details = SanitizeObservationJson(details);

        observation.summary = SanitizeObservationText(
            "memory(" + layer_label + ")" + memory::MemoryKindName(entry.kind) + ":" + entry.title,
            200);
        observation.fingerprint = ComputeFingerprint(
            ObservationSource::Memory,
            memory::MemoryKindName(entry.kind) + "|" + NormalizeShapeText(entry.title));
        observation.id = MakeObservationId(observation.source, observation.source_id);
        observation.created_at = entry.updated_at;
        if (!entry.file.empty()) {
            observation.evidence.push_back(
                MakeRef(dir_utf8 + "/" + entry.file, "memory 主题文件(正文在此,观察只带摘要)"));
        }
        out.push_back(std::move(observation));
    }
    return out;
}

}  // namespace lubancode::evolution
