// Harness replay 实现(§10.3)。合同见 harness.hpp。
//
// 桩的匹配规则:
//   - 模型步按"下一枚录制步"消费:指纹(模型名+参数 hash+输入引用串)与
//     录制 prepared 不合 → model.fingerprint_mismatch,不向后找;
//   - 工具步按 started 折叠序消费:call_id 不合 → tool.call_mismatch,
//     入参 hash 不合 → tool.input_hash_mismatch;
//   - 步子用尽还来要 → exhausted(宿主多做了录制里没有的事,也是 divergence)。

#include "trajectory/harness.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

#include "hooks/hash.hpp"
#include "trajectory/canonical_json.hpp"

namespace lubancode::trajectory {
namespace {

std::string CanonicalText(const nlohmann::json& value) {
    const auto canonical = CanonicalJsonDump(value);
    return canonical.value_or(value.dump());
}

// 一份 output blocks 里声明的 tool_call id(按块序;§6.1 call 由模型输出定义)。
std::vector<std::string> CallIdsInBlocks(const nlohmann::json& blocks) {
    std::vector<std::string> ids;
    if (!blocks.is_array()) {
        return ids;
    }
    for (const auto& block : blocks) {
        if (block.is_object() && block.value("type", std::string()) == "tool_call" &&
            block.contains("call_id")) {
            ids.push_back(block["call_id"].get<std::string>());
        }
    }
    return ids;
}

}  // namespace

nlohmann::json HarnessDivergence::ToJson() const {
    return nlohmann::json{{"stage", stage}, {"step_id", step_id},       {"reason", reason},
                          {"expected", expected}, {"actual", actual}};
}

// ---------------------------------------------------------------------------
// RecordedModelBackend
// ---------------------------------------------------------------------------

std::string RecordedModelBackend::RequestFingerprint(const std::string& model,
                                                     const nlohmann::json& parameters,
                                                     const std::vector<std::string>& message_refs) {
    nlohmann::json material = nlohmann::json::object();
    material["model"] = model;
    material["parameters"] = parameters.is_object() ? parameters : nlohmann::json::object();
    material["message_refs"] = message_refs;
    return hooks::Sha256Hex(CanonicalText(material));
}

RecordedModelBackend RecordedModelBackend::FromReplay(const ReplayState& state) {
    RecordedModelBackend backend;
    for (const auto& step : state.requests) {
        if (step.output_state != "completed") {
            continue;  // 失败/取消步没有可回喂的输出;宿主要它时自然 mismatch
        }
        Step recorded;
        recorded.request_id = step.request_id;
        recorded.blocks = step.output_blocks;
        recorded.stop_reason = step.stop_reason;
        recorded.output_event_id = step.output_event_id;
        // 指纹按录制 prepared payload 算(model+parameters+message_refs,
        // §5.3 可重放核心)。
        recorded.fingerprint = RequestFingerprint(step.model, step.parameters, step.message_refs);
        backend.steps_.push_back(std::move(recorded));
    }
    return backend;
}

std::expected<RecordedModelOutput, HarnessDivergence> RecordedModelBackend::NextRequest(
    const std::string& model, const nlohmann::json& parameters,
    const std::vector<std::string>& message_refs) {
    if (cursor_ >= steps_.size()) {
        HarnessDivergence divergence;
        divergence.stage = "model";
        divergence.reason = "model.exhausted";
        divergence.expected = std::to_string(steps_.size()) + " 枚录制步";
        divergence.actual = "第 " + std::to_string(cursor_ + 1) + " 次请求";
        return std::unexpected(std::move(divergence));
    }
    const Step& step = steps_[cursor_];
    const std::string fingerprint = RequestFingerprint(model, parameters, message_refs);
    if (fingerprint != step.fingerprint) {
        HarnessDivergence divergence;
        divergence.stage = "model";
        divergence.step_id = step.request_id;
        divergence.reason = "model.fingerprint_mismatch";
        divergence.expected = step.fingerprint;
        divergence.actual = fingerprint;
        return std::unexpected(std::move(divergence));
    }
    ++cursor_;
    RecordedModelOutput output;
    output.request_id = step.request_id;
    output.blocks = step.blocks;
    output.stop_reason = step.stop_reason;
    output.output_event_id = step.output_event_id;
    return output;
}

// ---------------------------------------------------------------------------
// RecordedToolExecutor
// ---------------------------------------------------------------------------

RecordedToolExecutor RecordedToolExecutor::FromReplay(const ReplayState& state) {
    RecordedToolExecutor executor;
    for (const auto& tool : state.tools) {
        if (!tool.started) {
            continue;  // 没越过 started 边界的调用,宿主不该来要结果
        }
        Step step;
        step.call_id = tool.call_id;
        step.tool_name = tool.tool_name;
        step.arguments_sha256 = tool.effective_arguments_sha256;
        step.terminal_kind = tool.terminal_kind;
        step.outcome = tool.outcome;
        executor.steps_.push_back(std::move(step));
    }
    return executor;
}

std::expected<RecordedToolOutcome, HarnessDivergence> RecordedToolExecutor::NextCall(
    const std::string& call_id, const std::string& tool_name,
    const std::string& arguments_sha256) {
    if (cursor_ >= steps_.size()) {
        HarnessDivergence divergence;
        divergence.stage = "tool";
        divergence.step_id = call_id;
        divergence.reason = "tool.exhausted";
        divergence.expected = std::to_string(steps_.size()) + " 枚录制步";
        divergence.actual = "第 " + std::to_string(cursor_ + 1) + " 次调用";
        return std::unexpected(std::move(divergence));
    }
    const Step& step = steps_[cursor_];
    if (call_id != step.call_id) {
        HarnessDivergence divergence;
        divergence.stage = "tool";
        divergence.step_id = call_id;
        divergence.reason = "tool.call_mismatch";
        divergence.expected = step.call_id;
        divergence.actual = call_id;
        return std::unexpected(std::move(divergence));
    }
    if (!arguments_sha256.empty() && !step.arguments_sha256.empty() &&
        arguments_sha256 != step.arguments_sha256) {
        HarnessDivergence divergence;
        divergence.stage = "tool";
        divergence.step_id = call_id;
        divergence.reason = "tool.input_hash_mismatch";
        divergence.expected = step.arguments_sha256;
        divergence.actual = arguments_sha256;
        return std::unexpected(std::move(divergence));
    }
    (void)tool_name;  // 名字不符会在 call_id/指纹层先暴露;桩不重复判
    ++cursor_;
    RecordedToolOutcome outcome;
    outcome.call_id = step.call_id;
    outcome.terminal_kind = step.terminal_kind;
    outcome.outcome = step.outcome;
    if (step.terminal_kind == "finished") {
        outcome.result_payload = nlohmann::json{{"outcome", step.outcome}};
    } else {
        outcome.result_payload = nlohmann::json{{"reason", step.outcome}};
    }
    return outcome;
}

// ---------------------------------------------------------------------------
// RunHarnessReplay
// ---------------------------------------------------------------------------

HarnessReplayReport RunHarnessReplay(const std::filesystem::path& stream_path) {
    HarnessReplayReport report;
    const auto fold = FoldStreamReplay(stream_path);
    if (!fold.ok()) {
        report.error_code = fold.error_code;
        report.message = fold.message;
        return report;
    }
    report.replay_state_hash = ComputeReplayStateHash(fold.state);

    RecordedModelBackend model = RecordedModelBackend::FromReplay(fold.state);
    RecordedToolExecutor tools = RecordedToolExecutor::FromReplay(fold.state);

    // 宿主状态机的最小替身:逐枚 request 步按录制 prepared 材料喂模型桩;
    // 输出里声明的每枚 tool_call 依 started 序喂工具桩;两桩全数消费完才
    // 算走完。外部宿主(或单测)喂别的指纹时,桩立即报 divergence——那
    // 才是这条路的主用途(§10.3"某次模型输出后,宿主为何走错")。
    for (const auto& step : fold.state.requests) {
        if (step.output_state != "completed") {
            continue;
        }
        auto output = model.NextRequest(step.model, step.parameters, step.message_refs);
        if (!output.has_value()) {
            report.divergence = std::move(output).error();
            report.error_code = "harness.divergence";
            report.message = report.divergence->reason + " @ " + report.divergence->step_id;
            return report;
        }
        // 输出声明的 tool_call 逐枚对工具桩。
        for (const std::string& call_id : CallIdsInBlocks(step.output_blocks)) {
            std::string arguments_sha256;
            std::string tool_name;
            for (const auto& tool : fold.state.tools) {
                if (tool.call_id == call_id) {
                    arguments_sha256 = tool.effective_arguments_sha256;
                    tool_name = tool.tool_name;
                    break;
                }
            }
            auto outcome = tools.NextCall(call_id, tool_name, arguments_sha256);
            if (!outcome.has_value()) {
                report.divergence = std::move(outcome).error();
                report.error_code = "harness.divergence";
                report.message = report.divergence->reason + " @ " + report.divergence->step_id;
                return report;
            }
            ++report.tool_steps_consumed;
        }
        ++report.model_steps_consumed;
    }
    // 声明外多出的 started 工具步(无 output 声明却执行了):宿主多做=divergence。
    if (!tools.exhausted()) {
        HarnessDivergence divergence;
        divergence.stage = "tool";
        divergence.reason = "tool.excess_started";
        divergence.expected = std::to_string(tools.steps()) + " 枚录制步";
        divergence.actual = "只消费 " + std::to_string(report.tool_steps_consumed) + " 枚";
        report.divergence = std::move(divergence);
        report.error_code = "harness.divergence";
        report.message = "tool.excess_started";
        return report;
    }
    report.ok = true;
    return report;
}

}  // namespace lubancode::trajectory
