// harness replay 测试(P0-3 §10.3/§16.5):
//   - RecordedModelBackend:指纹合 → 回录制输出;指纹不合/步子用尽 →
//     立即 divergence,不向后硬找;
//   - RecordedToolExecutor:call_id 不合、入参 hash 不合 → divergence;
//   - RunHarnessReplay:golden 流自洽走完;output 声明的 call 都有工具步。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/harness.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"

using namespace lubancode::trajectory;

#ifndef LUBANCODE_SOURCE_DIR
#define LUBANCODE_SOURCE_DIR "."
#endif

namespace {

std::filesystem::path GoldenFixture() {
    return std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "trajectory" /
           "v1" / "golden_main.jsonl";
}

}  // namespace

TEST_CASE("RecordedModelBackend: 指纹匹配置回录制输出,不匹配立即 divergence") {
    const auto report = FoldStreamReplay(GoldenFixture());
    REQUIRE(report.ok());
    RecordedModelBackend backend = RecordedModelBackend::FromReplay(report.state);
    CHECK(backend.steps() == 2);  // golden 有两次 completed 输出

    // 第一枚录制步:按录制 prepared 材料(model/parameters/message_refs)喂。
    const auto& first = report.state.requests[0];
    auto output = backend.NextRequest(first.model, first.parameters, first.message_refs);
    REQUIRE(output.has_value());
    CHECK(output->request_id == first.request_id);
    CHECK(output->stop_reason == "tool_use");
    CHECK(output->blocks == first.output_blocks);

    // 换个模型名再要:指纹不合,立即 divergence(expected/actual 都带)。
    auto wrong = backend.NextRequest("other-model", first.parameters, first.message_refs);
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().stage == "model");
    CHECK(wrong.error().reason == "model.fingerprint_mismatch");
    CHECK_FALSE(wrong.error().expected.empty());
    CHECK_FALSE(wrong.error().actual.empty());

    // 第二枚喂对材料,桩还认——divergence 不吃步子(不向后硬找,也不误吃)。
    const auto& second = report.state.requests[1];
    auto output2 = backend.NextRequest(second.model, second.parameters, second.message_refs);
    REQUIRE(output2.has_value());
    CHECK(output2->stop_reason == "end_turn");
    CHECK(backend.exhausted());

    // 步子用尽还来要:exhausted divergence。
    auto beyond = backend.NextRequest(second.model, second.parameters, second.message_refs);
    REQUIRE_FALSE(beyond.has_value());
    CHECK(beyond.error().reason == "model.exhausted");
}

TEST_CASE("RecordedToolExecutor: call 顺序与入参 hash 都要对") {
    const auto report = FoldStreamReplay(GoldenFixture());
    REQUIRE(report.ok());
    RecordedToolExecutor executor = RecordedToolExecutor::FromReplay(report.state);
    CHECK(executor.steps() == 1);  // golden 一道 started 工具

    // 错 call_id:立即 divergence。
    auto wrong_call = executor.NextCall("call-9999", "read_file",
                                        report.state.tools[0].effective_arguments_sha256);
    REQUIRE_FALSE(wrong_call.has_value());
    CHECK(wrong_call.error().reason == "tool.call_mismatch");
    CHECK(wrong_call.error().expected == "call-0001");

    // 对 call_id + 错 hash:input_hash_mismatch(golden 录的正是 64 个 '0',
    // 错值换 'a')。
    auto wrong_hash = executor.NextCall("call-0001", "read_file", std::string(64, 'a'));
    REQUIRE_FALSE(wrong_hash.has_value());
    CHECK(wrong_hash.error().reason == "tool.input_hash_mismatch");

    // 全对:回录制终态。
    auto outcome = executor.NextCall("call-0001", "read_file",
                                     report.state.tools[0].effective_arguments_sha256);
    REQUIRE(outcome.has_value());
    CHECK(outcome->terminal_kind == "tool.execution.finished");
    CHECK(outcome->outcome == "succeeded");
    CHECK(executor.exhausted());

    auto beyond = executor.NextCall("call-0001", "read_file", std::string(64, 'a'));
    REQUIRE_FALSE(beyond.has_value());
    CHECK(beyond.error().reason == "tool.exhausted");
}

TEST_CASE("RunHarnessReplay: golden 流自洽走完,指纹锚与 state hash 齐") {
    const auto report = RunHarnessReplay(GoldenFixture());
    REQUIRE(report.ok);
    CHECK(report.model_steps_consumed == 2);
    CHECK(report.tool_steps_consumed == 1);
    CHECK(IsHex64(report.replay_state_hash));
    // 桩的 state hash 与 exact replay 的折叠 hash 同源(§10.2 锚)。
    const auto fold = FoldStreamReplay(GoldenFixture());
    REQUIRE(fold.ok());
    CHECK(report.replay_state_hash == ComputeReplayStateHash(fold.state));
    CHECK(report.error_code.empty());
}

TEST_CASE("RunHarnessReplay: 坏账(链断)明报,不硬跑") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode-traj-harness-bad";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / "broken.jsonl";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "not-json\n";
    }
    const auto report = RunHarnessReplay(path);
    CHECK_FALSE(report.ok);
    CHECK(report.error_code == "replay.verify_failed");
}
