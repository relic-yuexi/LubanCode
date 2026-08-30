// checkpoint 与高水位恢复测试(P0-3 §10.4 末段):
//   - checkpoint 只是缓存:source_seq/source_event_hash/state_hash 三件套,
//     Journal 变了或版本不符即作废,从 Journal 重算;
//   - 高水位:多枚 checkpoint 取最大可用 seq;
//   - 续折等价:从 checkpoint 续折的最终 state hash 与从头整折相同
//    (checkpoint 不得改变折叠结果——确定性锚)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/replay.hpp"

using namespace lubancode::trajectory;

#ifndef LUBANCODE_SOURCE_DIR
#define LUBANCODE_SOURCE_DIR "."
#endif

namespace {

class FixedNsClock : public RecorderClock {
public:
    std::int64_t WallMs() const override { return 1759000000000LL; }
    std::int64_t MonotonicNs() const override { return 777777000LL; }
};

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::filesystem::path GoldenFixture() {
    return std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "trajectory" /
           "v1" / "golden_main.jsonl";
}

// 第 seq 枚事件的 hash(1 基)。
std::string HashAt(const std::filesystem::path& stream, std::uint64_t seq) {
    const auto lines = ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    REQUIRE(seq >= 1);
    REQUIRE(seq <= lines->size());
    const auto parsed = nlohmann::json::parse((*lines)[static_cast<std::size_t>(seq - 1)], nullptr,
                                              false);
    REQUIRE_FALSE(parsed.is_discarded());
    return parsed.at("event_hash").get<std::string>();
}

}  // namespace

TEST_CASE("checkpoint 高水位: 落盘、找回、Journal 变了即作废") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode-traj-ckpt";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto stream = dir / "main.jsonl";
    {
        const auto text = ReadFileText(GoldenFixture());
        REQUIRE(text.has_value());
        std::ofstream out(stream, std::ios::binary | std::ios::trunc);
        out << *text;
    }

    // 折到第 10 枚做一枚 checkpoint(折前缀:整折后截取也行,这里用
    // ContinueFoldFrom 的反向——整折再回填 folded_seq 之前的账不现实,
    // 直接用整折的账声明 seq=10 的 checkpoint:折叠账自洽校验会拒它。
    // 正路是"折到中途存账",故这里以 seq=19(全量)做 checkpoint)。
    const auto full = FoldStreamReplay(stream);
    REQUIRE(full.ok());
    ReplayCheckpoint checkpoint;
    checkpoint.stream_name = "main";
    checkpoint.source_seq = full.state.folded_seq;
    checkpoint.source_event_hash = full.state.integrity.last_event_hash;
    checkpoint.state_hash = ComputeReplayStateHash(full.state);
    checkpoint.folded = full.state;
    REQUIRE(WriteReplayCheckpoint(dir, checkpoint).has_value());
    CHECK(std::filesystem::exists(dir / "checkpoints" / "main-19.json"));

    // 找回:seq/hash/折叠账全对上。
    const auto found = FindLatestUsableCheckpoint(dir, "main", stream);
    REQUIRE(found.has_value());
    CHECK(found->source_seq == 19);
    CHECK(found->source_event_hash == checkpoint.source_event_hash);
    CHECK(found->state_hash == checkpoint.state_hash);

    // Journal 变了:手写一枚坏 checkpoint 在更大 seq,高水位选中它但
    // Journal 没那么长(hash 对不上),应作废并退回可用的 19。
    ReplayCheckpoint stale = checkpoint;
    stale.source_seq = 50;  // Journal 没这么长
    stale.source_event_hash = std::string(64, 'f');
    REQUIRE(WriteReplayCheckpoint(dir, stale).has_value());
    const auto found2 = FindLatestUsableCheckpoint(dir, "main", stream);
    REQUIRE(found2.has_value());
    CHECK(found2->source_seq == 19);  // 坏的作废,退回好的

    // 版本不符:同 seq 重写为旧 replay 版本,整枚作废(升版后旧 checkpoint
    // 全数作废,§10.4);再重写回当前版本又能找回(幂等)。
    ReplayCheckpoint old_version = checkpoint;
    old_version.replay_version = kReplayProjectionVersion - 1;
    REQUIRE(WriteReplayCheckpoint(dir, old_version).has_value());
    CHECK_FALSE(FindLatestUsableCheckpoint(dir, "main", stream).has_value());
    ReplayCheckpoint good = checkpoint;
    REQUIRE(WriteReplayCheckpoint(dir, good).has_value());
    const auto found3 = FindLatestUsableCheckpoint(dir, "main", stream);
    REQUIRE(found3.has_value());
    CHECK(found3->replay_version == kReplayProjectionVersion);
}

TEST_CASE("续折等价: 从前缀 checkpoint 续折的结果与从头整折 state hash 相同") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode-traj-ckpt-cont";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "artifacts", ec);
    const auto stream = dir / "main.jsonl";

    // 造一份没封 session 的活账(能续折):run.started + turn + input +
    // 一道完整工具 + 一枚收尾 assistant 输出;不落 run terminal。
    EventScope scope;
    scope.workspace_key = "demo-000000000000";
    scope.session_id = "20260830-031522-7K4M2P";
    scope.run_id = "main-0001";
    scope.run_kind = RunKind::MainSession;
    scope.visibility = {Visibility::HostOnly};
    FixedNsClock clock;
    auto recorder =
        TrajectoryRecorder::Start(stream, dir / "artifacts", scope, RecorderOptions{}, &clock);
    REQUIRE(recorder.has_value());
    REQUIRE(recorder
                ->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"},
                                                 {"start_reason", "process_launch"}},
                                  Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    const auto put = [&](EventKind kind, const EventScope& s, nlohmann::json payload,
                         Durability durability = Durability::ProcessCrash) {
        RecordRequest req;
        req.kind = kind;
        req.scope = s;
        req.payload = std::move(payload);
        return recorder->Record(std::move(req), durability);
    };
    EventScope turn_scope = scope;
    turn_scope.turn_id = "turn-0001";
    turn_scope.actor = Actor::User;
    turn_scope.origin = Origin::ExternalUser;
    REQUIRE(put(EventKind::TurnStarted, turn_scope, nlohmann::json{{"trigger", "external_user"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(put(EventKind::InputReceived, turn_scope,
                nlohmann::json{{"input_id", "input-0001"},
                               {"content", nlohmann::json::array({"看看目录"})},
                               {"channel", "terminal"},
                               {"sender", nlohmann::json{{"kind", "local_user"}}}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(recorder->Close().has_value());  // 关柄(不落 terminal,活账)

    // 前 3 枚处做 checkpoint(折到 seq=3)。
    const auto prefix = FoldStreamReplay(stream);
    REQUIRE(prefix.ok());
    REQUIRE(prefix.state.folded_seq >= 3);
    // 手工把账截到 seq=3 的形状:FromJson 重建后把 folded_seq/last hash 回填,
    // 再验 FindLatestUsable 的自洽口。整账截取:去掉多余向量尾部等价性由
    // 续折测试覆盖,这里直接用整账 seq=3 做法不可行(folded_seq 是全量)。
    // 换路:重折前缀的正规姿势是 ContinueFoldFrom,但前缀账本身要先有——
    // 用"截短 Journal"造前缀。
    const auto lines = ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    {
        std::ofstream out(dir / "prefix.jsonl", std::ios::binary | std::ios::trunc);
        for (std::size_t i = 0; i < 3; ++i) {
            out << (*lines)[i] << "\n";
        }
    }
    const auto prefix_fold = FoldStreamReplay(dir / "prefix.jsonl");
    REQUIRE(prefix_fold.ok());
    REQUIRE(prefix_fold.state.folded_seq == 3);
    ReplayCheckpoint checkpoint;
    checkpoint.stream_name = "main";
    checkpoint.source_seq = 3;
    checkpoint.source_event_hash = HashAt(stream, 3);
    checkpoint.state_hash = ComputeReplayStateHash(prefix_fold.state);
    checkpoint.folded = prefix_fold.state;
    REQUIRE(WriteReplayCheckpoint(dir, checkpoint).has_value());

    // 找回后续折到完整账。
    const auto found = FindLatestUsableCheckpoint(dir, "main", stream);
    REQUIRE(found.has_value());
    CHECK(found->source_seq == 3);
    ReplayState continued = found->folded;
    std::string error;
    REQUIRE(ContinueFoldFrom(stream, &continued, &error));

    // 续折 == 整折(确定性锚:checkpoint 只是缓存,不得改变结果)。
    const std::string continued_hash = ComputeReplayStateHash(continued);
    CHECK(continued_hash == ComputeReplayStateHash(prefix.state));
    CHECK(continued.folded_seq == prefix.state.folded_seq);
    CHECK(continued.effective_conversation.size() == prefix.state.effective_conversation.size());
    CHECK(continued.integrity.last_event_hash == prefix.state.integrity.last_event_hash);
}
