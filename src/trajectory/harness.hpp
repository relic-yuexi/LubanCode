// Harness replay(P0 新轨迹记录单 §10.3):把边界换成录制桩。
//
//   ProviderBackend -> RecordedModelBackend   模型输出走录制值
//   ToolExecutor    -> RecordedToolExecutor   工具结果走录制值
//
// 宿主照常跑 turn 状态机。每一步只消费下一枚匹配事件:名字、参数 hash、
// 因果 id 不合,立即报 divergence,不向后硬找(§10.3)。这条路用来稳定
// 复现"某次模型输出后,宿主为何走错"——不是真调模型(那是 live rerun)。
//
// 依赖铁律:trajectory 纯库,不 include app/cli/runtime/api。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/replay.hpp"

namespace lubancode::trajectory {

// 一次 divergence(§16.5:遇错立即报,不向后硬找)。
struct HarnessDivergence {
    std::string stage;   // model | tool
    std::string step_id; // request_id / call_id
    std::string reason;  // 稳定码:model.fingerprint_mismatch / tool.call_mismatch /
                         // tool.input_hash_mismatch / model.exhausted / tool.exhausted /
                         // tool.call_unknown
    std::string expected;
    std::string actual;

    nlohmann::json ToJson() const;
};

// 录制桩回的模型输出(规范 blocks,同 model.output.completed.payload)。
struct RecordedModelOutput {
    std::string request_id;
    nlohmann::json blocks = nlohmann::json::array();
    std::string stop_reason;
    std::string output_event_id;
};

// 录制桩回的工具结果(执行终态,同 tool.execution.*.payload)。
struct RecordedToolOutcome {
    std::string call_id;
    std::string terminal_kind;  // finished|failed|cancelled|unknown
    std::string outcome;        // succeeded|…|reason
    nlohmann::json result_payload;
};

// 模型边界桩:宿主每发一次请求,拿请求指纹换下一枚录制输出。指纹不合或
// 步子用尽,立即 divergence。
class RecordedModelBackend {
public:
    RecordedModelBackend() = default;
    // 从折叠账造桩(步序 = 折叠序)。
    static RecordedModelBackend FromReplay(const ReplayState& state);

    // 指纹:模型名 + 参数 canonical hash + 输入事件引用串。指纹只用来对
    // 桩——不重算 hash chain(那是 verify 的活)。
    static std::string RequestFingerprint(const std::string& model, const nlohmann::json& parameters,
                                          const std::vector<std::string>& message_refs);

    // 喂"宿主现在要发的请求";匹配下一枚录制步回录制输出,不匹配 divergence。
    std::expected<RecordedModelOutput, HarnessDivergence> NextRequest(
        const std::string& model, const nlohmann::json& parameters,
        const std::vector<std::string>& message_refs);

    bool exhausted() const { return cursor_ >= steps_.size(); }
    std::size_t steps() const { return steps_.size(); }

private:
    struct Step {
        std::string request_id;
        nlohmann::json blocks;
        std::string stop_reason;
        std::string output_event_id;
        std::string fingerprint;  // 造桩时按折叠账里的 prepared payload 算
    };
    std::vector<Step> steps_;
    std::size_t cursor_ = 0;
};

// 工具边界桩:宿主每执行一道工具,拿 call_id + 有效入参 hash 换下一枚
// 录制结果。
class RecordedToolExecutor {
public:
    RecordedToolExecutor() = default;
    static RecordedToolExecutor FromReplay(const ReplayState& state);

    // 喂"宿主现在要执行的工具";call 顺序不合或入参 hash 不合,divergence。
    std::expected<RecordedToolOutcome, HarnessDivergence> NextCall(const std::string& call_id,
                                                                   const std::string& tool_name,
                                                                   const std::string& arguments_sha256);

    bool exhausted() const { return cursor_ >= steps_.size(); }
    std::size_t steps() const { return steps_.size(); }

private:
    struct Step {
        std::string call_id;
        std::string tool_name;
        std::string arguments_sha256;
        std::string terminal_kind;
        std::string outcome;
    };
    std::vector<Step> steps_;  // 按 started 折叠序
    std::size_t cursor_ = 0;
};

// 完整 harness 一趟(§10.3"宿主照常跑状态机"的最小替身):按折叠账逐步
// 消费两枚桩,验因果 id 与配对(output 声明的 call 都有工具步、工具步都
// 有结果)。任何一步不合立即 divergence。
struct HarnessReplayReport {
    bool ok = false;
    std::optional<HarnessDivergence> divergence;
    std::uint64_t model_steps_consumed = 0;
    std::uint64_t tool_steps_consumed = 0;
    std::string replay_state_hash;  // 折叠账的规范状态 hash(§10.2 锚)
    std::string error_code;         // 空 = 过;harness.* / replay.* 前缀
    std::string message;
};

HarnessReplayReport RunHarnessReplay(const std::filesystem::path& stream_path);

}  // namespace lubancode::trajectory
