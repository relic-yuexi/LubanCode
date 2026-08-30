// ReplayState 折叠测试(P0-3 §10.1/§10.2/§16.5):
//   - 折叠形状:run/turn/request/tool/evidence/control 各账从事件投影;
//   - effective conversation:user/assistant/tool 三角色 + source event id,
//     宿主注入不冒充 user;
//   - 确定性:同一 Journal 折两次 state hash 相同;不同 Journal 不同 hash;
//   - v1 golden fixture 旧读:state hash 稳定不随 reader 版本漂;
//   - 尾行截断:已验证前缀照折,truncated_tail 明标;
//   - 链断:replay.verify_failed 不折;
//   - ToJson/FromJson round-trip;
//   - 悬空工具三道账分档。
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

// 折叠里一条 conversation 的 role 名,断言方便。
std::string RoleName(const ReplayMessage& message) {
    switch (message.role) {
        case ReplayMessage::Role::User:
            return "user";
        case ReplayMessage::Role::Assistant:
            return "assistant";
        case ReplayMessage::Role::Tool:
            return "tool";
    }
    return "?";
}

}  // namespace

TEST_CASE("折叠形状: golden fixture 的 run/turn/request/tool/evidence 各账") {
    const auto report = FoldStreamReplay(GoldenFixture());
    REQUIRE(report.ok());
    const ReplayState& state = report.state;

    CHECK(state.run_id == "main-0001");
    CHECK(state.run_kind == RunKind::MainSession);
    CHECK(state.start_reason == "process_launch");
    CHECK(state.run_terminal_state == "run.completed");
    CHECK(state.session_end_state == "ended");
    CHECK(state.integrity.events_folded == 19);
    CHECK_FALSE(state.integrity.last_event_hash.empty());

    // turn 账:一场外层轮,completed + outcome。
    REQUIRE(state.turns.size() == 1);
    CHECK(state.turns[0].turn_id == "turn-0001");
    CHECK(state.turns[0].trigger == "external_user");
    CHECK(state.turns[0].terminal_state == "turn.completed");
    CHECK(state.turns[0].outcome == "succeeded");  // outcome.assessed 落到 turn

    // request 账:两次请求都 completed;prepared 材料齐全(harness 桩口粮)。
    REQUIRE(state.requests.size() == 2);
    CHECK(state.requests[0].request_id == "req-0001");
    CHECK(state.requests[0].model == "demo-model");
    CHECK(state.requests[0].provider == "demo");
    CHECK(state.requests[0].sent);
    CHECK(state.requests[0].output_state == "completed");
    CHECK(state.requests[0].stop_reason == "tool_use");
    CHECK(state.requests[0].message_refs.size() == 1);
    CHECK(state.requests[1].output_state == "completed");
    CHECK(state.requests[1].stop_reason == "end_turn");

    // 工具台账:一道调用六事件全。
    REQUIRE(state.tools.size() == 1);
    const ReplayToolEntry& tool = state.tools[0];
    CHECK(tool.call_id == "call-0001");
    CHECK(tool.tool_name == "read_file");
    CHECK(tool.planned);
    CHECK(tool.effective);
    CHECK(tool.started);
    CHECK(tool.terminal);
    CHECK(tool.terminal_kind == "tool.execution.finished");
    CHECK(tool.outcome == "succeeded");
    CHECK(tool.result_committed);
    CHECK_FALSE(tool.effective_arguments_sha256.empty());

    // 证据账:一枚 verification.recorded。
    REQUIRE(state.evidence.size() == 1);
    CHECK(state.evidence[0].verification_id == "verify-0001");
    CHECK(state.evidence[0].passed);
    CHECK_FALSE(state.evidence[0].invalidated);

    // 悬空:0(全收口)。
    CHECK(state.integrity.dangling_tools == 0);
    CHECK(CollectDanglingTools(state).empty());
}

TEST_CASE("effective conversation: 三角色投影 + source event id,注入不冒充 user") {
    const auto report = FoldStreamReplay(GoldenFixture());
    REQUIRE(report.ok());
    const std::vector<ReplayMessage>& conversation = report.state.effective_conversation;

    // user 输入 → 一次带 tool_call 的 assistant → tool result → 收尾 assistant。
    REQUIRE(conversation.size() == 4);
    CHECK(RoleName(conversation[0]) == "user");
    CHECK(conversation[0].origin == "external_user");
    CHECK(conversation[1].role == ReplayMessage::Role::Assistant);
    // golden 的 output 事件由宿主代落(actor=host/recovery_runtime),折叠照
    // 信封记 origin,不猜。
    CHECK(conversation[1].origin == "recovery_runtime");
    // assistant blocks 里带 tool_call(call 由模型输出定义,§6.1)。
    bool has_tool_call = false;
    for (const auto& block : conversation[1].blocks) {
        if (block.value("type", std::string()) == "tool_call") {
            has_tool_call = block.at("call_id").get<std::string>() == "call-0001";
        }
    }
    CHECK(has_tool_call);
    CHECK(RoleName(conversation[2]) == "tool");
    CHECK(conversation[2].call_id.has_value());
    CHECK(*conversation[2].call_id == "call-0001");
    CHECK(RoleName(conversation[3]) == "assistant");

    // 每条都带 source event id(§15.4 HistoryItem)。
    for (const auto& message : conversation) {
        CHECK_FALSE(message.source_event_id.empty());
        CHECK_FALSE(message.source_event_hash.empty());
    }
}

TEST_CASE("确定性: 同一 Journal 折两次 state hash 相同,不同 Journal 不同") {
    const auto first = FoldStreamReplay(GoldenFixture());
    const auto second = FoldStreamReplay(GoldenFixture());
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    const std::string hash_first = ComputeReplayStateHash(first.state);
    const std::string hash_second = ComputeReplayStateHash(second.state);
    CHECK(hash_first == hash_second);
    CHECK(IsHex64(hash_first));

    // 不同 Journal:多一枚事件,hash 必须变。
    const auto dir = std::filesystem::temp_directory_path() / "lubancode-traj-replay-det";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "artifacts", ec);
    const auto golden_text = ReadFileText(GoldenFixture());
    REQUIRE(golden_text.has_value());
    {
        std::ofstream out(dir / "copy.jsonl", std::ios::binary | std::ios::trunc);
        out << *golden_text;
        // golden 的最后一枚是 session.ended,不能再追加(封链后拒写);
        // 换个办法:去掉最后一行再折,hash 必不同。
    }
    std::vector<std::string> lines;
    std::istringstream stream(*golden_text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    {
        std::ofstream out(dir / "shorter.jsonl", std::ios::binary | std::ios::trunc);
        for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
            out << lines[i] << "\n";
        }
    }
    const auto shorter = FoldStreamReplay(dir / "shorter.jsonl");
    REQUIRE(shorter.ok());
    CHECK(ComputeReplayStateHash(shorter.state) != hash_first);
}

TEST_CASE("ToJson/FromJson round-trip: 折叠账整份过桥不丢") {
    const auto report = FoldStreamReplay(GoldenFixture());
    REQUIRE(report.ok());
    const nlohmann::json json = report.state.ToJson();
    const auto back = ReplayState::FromJson(json);
    REQUIRE(back.has_value());
    CHECK(ComputeReplayStateHash(*back) == ComputeReplayStateHash(report.state));
    CHECK(back->effective_conversation.size() == report.state.effective_conversation.size());
    CHECK(back->tools.size() == report.state.tools.size());
    CHECK(back->requests.size() == report.state.requests.size());
    CHECK(back->integrity.last_event_hash == report.state.integrity.last_event_hash);
    CHECK(back->folded_seq == report.state.folded_seq);
}

TEST_CASE("尾行截断: 已验证前缀照折,truncated_tail 明标,不伪造终态") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode-traj-replay-trunc";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto golden_text = ReadFileText(GoldenFixture());
    REQUIRE(golden_text.has_value());
    {
        std::ofstream out(dir / "truncated.jsonl", std::ios::binary | std::ios::trunc);
        out.write(golden_text->data(), static_cast<std::streamsize>(golden_text->size() - 1));
    }
    const auto report = FoldStreamReplay(dir / "truncated.jsonl");
    REQUIRE(report.ok());
    CHECK(report.state.integrity.truncated_tail);
    // 已验证前缀 19 枚全折(尾行只缺 '\n',事件本身完整)。
    CHECK(report.state.integrity.events_folded == 19);
}

TEST_CASE("链断: replay.verify_failed,不折") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode-traj-replay-broken";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto golden_text = ReadFileText(GoldenFixture());
    REQUIRE(golden_text.has_value());
    std::vector<std::string> lines;
    std::istringstream stream(*golden_text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    // 抽走中段一枚:seq 跳号 + 链断。
    {
        std::ofstream out(dir / "gapped.jsonl", std::ios::binary | std::ios::trunc);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i != 9) {
                out << lines[i] << "\n";
            }
        }
    }
    const auto report = FoldStreamReplay(dir / "gapped.jsonl");
    CHECK_FALSE(report.ok());
    CHECK(report.error_code == "replay.verify_failed");
}

TEST_CASE("悬空工具三道账: started 无终态 / 终态无 result 分档,unknown 不算成功") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode-traj-replay-dangling";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "artifacts", ec);

    EventScope scope;
    scope.workspace_key = "demo-000000000000";
    scope.session_id = "20260830-031522-7K4M2P";
    scope.run_id = "main-0001";
    scope.run_kind = RunKind::MainSession;
    scope.visibility = {Visibility::HostOnly};
    FixedNsClock clock;
    auto recorder =
        TrajectoryRecorder::Start(dir / "main.jsonl", dir / "artifacts", scope, RecorderOptions{}, &clock);
    REQUIRE(recorder.has_value());
    REQUIRE(recorder
                ->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"},
                                                 {"start_reason", "dangling"}},
                                  Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);

    const auto put_as = [&](EventKind kind, EventScope event_scope, nlohmann::json payload,
                            Durability durability = Durability::ProcessCrash) {
        RecordRequest req;
        req.kind = kind;
        req.scope = std::move(event_scope);
        req.payload = std::move(payload);
        return recorder->Record(std::move(req), durability);
    };
    const auto put_tool = [&](EventKind kind, nlohmann::json payload,
                              Durability durability = Durability::ProcessCrash) {
        EventScope tool_scope = scope;
        tool_scope.turn_id = "turn-0001";
        tool_scope.request_id = "req-0001";
        tool_scope.call_id = "call-dangle";
        tool_scope.actor = Actor::Tool;
        tool_scope.origin = Origin::BuiltinTool;
        return put_as(kind, std::move(tool_scope), std::move(payload), durability);
    };

    // 一道 started 未终态的工具(悬空账留在账上:run 不收口,文件不封)。
    {
        EventScope turn_scope = scope;
        turn_scope.turn_id = "turn-0001";
        turn_scope.actor = Actor::User;
        turn_scope.origin = Origin::ExternalUser;
        REQUIRE(put_as(EventKind::TurnStarted, turn_scope,
                       nlohmann::json{{"trigger", "external_user"}})
                    .status == RecordReceipt::Status::Committed);
        REQUIRE(put_as(EventKind::InputReceived, turn_scope,
                       nlohmann::json{{"input_id", "input-0001"},
                                      {"content", nlohmann::json::array({"跑一下构建"})},
                                      {"channel", "terminal"},
                                      {"sender", nlohmann::json{{"kind", "local_user"}}}})
                    .status == RecordReceipt::Status::Committed);
    }
    // 模型问答回合:planned 引用 output 声明过的 call_id(§6.1)。
    {
        EventScope model_scope = scope;
        model_scope.turn_id = "turn-0001";
        model_scope.request_id = "req-0001";
        model_scope.actor = Actor::Model;
        model_scope.origin = Origin::ProviderModel;
        const auto prepared =
            put_as(EventKind::ModelRequestPrepared, model_scope,
                   nlohmann::json{{"model", "demo-model"},
                                  {"provider", "demo"},
                                  {"wire", "responses"},
                                  {"message_refs", nlohmann::json::array({"evt-00000002"})}});
        REQUIRE(prepared.status == RecordReceipt::Status::Committed);
        REQUIRE(put_as(EventKind::ModelRequestSent, model_scope,
                       nlohmann::json{{"prepared_event_id", prepared.event_id}})
                    .status == RecordReceipt::Status::Committed);
        REQUIRE(put_as(EventKind::ModelOutputCompleted, model_scope,
                       nlohmann::json{{"output_id", "output-0001"},
                                      {"blocks",
                                       nlohmann::json::array({nlohmann::json{
                                           {"type", "tool_call"}, {"call_id", "call-dangle"},
                                           {"name", "run_command"},
                                           {"arguments", nlohmann::json{{"cmd", "make"}}}}})},
                                      {"stop_reason", "tool_use"}})
                    .status == RecordReceipt::Status::Committed);
    }
    REQUIRE(put_tool(EventKind::ToolExecutionPlanned,
                     nlohmann::json{{"call_id", "call-dangle"}, {"tool_name", "run_command"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(put_tool(EventKind::ToolInputEffective,
                     nlohmann::json{{"call_id", "call-dangle"},
                                    {"tool_name", "run_command"},
                                    {"source_kind", "builtin"},
                                    {"effect_class", "external_write"},
                                    {"effective_arguments", nlohmann::json::object()},
                                    {"effective_arguments_sha256", std::string(64, 'a')}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(put_tool(EventKind::ToolExecutionStarted, nlohmann::json{{"call_id", "call-dangle"}},
                     Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);

    const auto report = FoldStreamReplay(dir / "main.jsonl");
    REQUIRE(report.ok());
    const auto dangling = CollectDanglingTools(report.state);
    REQUIRE(dangling.size() == 1);
    CHECK(dangling[0].call_id == "call-dangle");
    CHECK(dangling[0].state == "started_no_terminal");
    CHECK_FALSE(dangling[0].unknown_side_effect);  // 没到终态,不算 unknown
    CHECK(report.state.integrity.dangling_tools == 1);
    CHECK(report.state.run_terminal_state.empty());  // 不伪造终态

    // unknown 终态:副作用不明,integrity 明标(unknown 不算 success)。
    REQUIRE(put_tool(EventKind::ToolExecutionUnknown, nlohmann::json{{"reason", "process_killed"}})
                .status == RecordReceipt::Status::Committed);
    const auto report2 = FoldStreamReplay(dir / "main.jsonl");
    REQUIRE(report2.ok());
    CHECK(report2.state.integrity.unknown_side_effects);
    const auto dangling2 = CollectDanglingTools(report2.state);
    REQUIRE(dangling2.size() == 1);
    CHECK(dangling2[0].unknown_side_effect);
    // 终态无 result:分档变 terminal_no_result。
    CHECK(dangling2[0].state == "terminal_no_result");
}
