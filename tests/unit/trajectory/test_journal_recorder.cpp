// TrajectoryRecorder 状态机硬约束测试(§6.2 十八条逐条)+ hash 链确定性 +
// blob offload + 拒写语义。每条约束给稳定 error_code;先写后说不行。
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"

using namespace lubancode::trajectory;

namespace {

class FixedClock : public RecorderClock {
public:
    std::int64_t WallMs() const override { return 1759000000000LL; }
    std::int64_t MonotonicNs() const override { return 777777000LL; }
};

struct Harness {
    FixedClock clock;
    std::filesystem::path dir;
    std::optional<TrajectoryRecorder> recorder;

    explicit Harness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-traj-rec-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "artifacts", ec);
        auto started = TrajectoryRecorder::Start(dir / "main.jsonl", dir / "artifacts",
                                                 BaseScope(), RecorderOptions{}, &clock);
        REQUIRE(started.has_value());
        recorder = std::move(*started);
    }

    static EventScope BaseScope() {
        EventScope scope;
        scope.workspace_key = "ws-000000000000";
        scope.session_id = "20260830-031522-7K4M2P";
        scope.run_id = "main-0001";
        scope.run_kind = RunKind::MainSession;
        scope.visibility = {Visibility::HostOnly};
        return scope;
    }

    static EventScope TurnScope(std::string turn, std::optional<std::string> request = std::nullopt,
                                std::optional<std::string> call = std::nullopt) {
        EventScope scope = BaseScope();
        scope.turn_id = std::move(turn);
        scope.request_id = std::move(request);
        scope.call_id = std::move(call);
        return scope;
    }

    RecordReceipt Put(EventKind kind, EventScope scope, nlohmann::json payload,
                      Durability durability = Durability::ProcessCrash) {
        RecordRequest request;
        request.kind = kind;
        request.scope = std::move(scope);
        request.payload = std::move(payload);
        return recorder->Record(std::move(request), durability);
    }

    // 开场:run.started + turn.started + input.received。
    RecordReceipt OpenTurn(const std::string& turn) {
        auto started = recorder->WriteRunStarted(nlohmann::json{{"start_reason", "process_launch"}},
                                                 Durability::PowerLoss);
        REQUIRE(started.status == RecordReceipt::Status::Committed);
        auto receipt = Put(EventKind::TurnStarted, TurnScope(turn),
                           nlohmann::json{{"trigger", "external_user"}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        EventScope input_scope = TurnScope(turn);
        input_scope.actor = Actor::User;
        input_scope.origin = Origin::ExternalUser;
        input_scope.visibility = {Visibility::UserVisible, Visibility::ModelInput};
        input_scope.training_policy = TrainingPolicy::Include;
        receipt = Put(EventKind::InputReceived, input_scope,
                      nlohmann::json{{"input_id", "input-0001"},
                                     {"content", nlohmann::json::array({"text"})},
                                     {"channel", "terminal"},
                                     {"sender", nlohmann::json{{"kind", "local_user"}}}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        return receipt;
    }

    // 一份带 tool_call 的模型往返:prepared -> sent -> output(声明 call)。
    RecordReceipt ModelExchange(const std::string& turn, const std::string& request_id,
                                const std::string& call_id, bool with_tool_call,
                                std::string* prepared_event_id) {
        EventScope prep = TurnScope(turn, request_id);
        prep.actor = Actor::Model;
        prep.origin = Origin::ProviderModel;
        auto prepared = Put(EventKind::ModelRequestPrepared, prep,
                            nlohmann::json{{"model", "m"},
                                           {"provider", "p"},
                                           {"wire", "responses"},
                                           {"message_refs", nlohmann::json::array()}});
        REQUIRE(prepared.status == RecordReceipt::Status::Committed);
        *prepared_event_id = prepared.event_id;
        auto sent = Put(EventKind::ModelRequestSent, TurnScope(turn, request_id),
                        nlohmann::json{{"prepared_event_id", prepared.event_id}});
        REQUIRE(sent.status == RecordReceipt::Status::Committed);
        nlohmann::json blocks = nlohmann::json::array({nlohmann::json{{"type", "text"},
                                                                      {"text", "先查文件"}}});
        if (with_tool_call) {
            blocks.push_back(nlohmann::json{{"type", "tool_call"},
                                            {"call_id", call_id},
                                            {"name", "read_file"},
                                            {"arguments", nlohmann::json{{"path", "src/a.cpp"}}}});
        }
        return Put(EventKind::ModelOutputCompleted, TurnScope(turn, request_id),
                   nlohmann::json{{"output_id", "output-" + request_id},
                                  {"blocks", blocks},
                                  {"stop_reason", with_tool_call ? "tool_use" : "end_turn"}});
    }

    // 一道完整工具流水:planned -> effective -> started -> finished。
    RecordReceipt RunTool(const std::string& turn, const std::string& request_id,
                          const std::string& call_id, const std::string& effect_class) {
        auto receipt = Put(EventKind::ToolExecutionPlanned, TurnScope(turn, request_id, call_id),
                           nlohmann::json{{"call_id", call_id}, {"tool_name", "read_file"}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        nlohmann::json effective = nlohmann::json{{"call_id", call_id},
                                                 {"tool_name", "read_file"},
                                                 {"source_kind", "builtin"},
                                                 {"effect_class", effect_class},
                                                 {"effective_arguments", nlohmann::json::object()},
                                                 {"effective_arguments_sha256", std::string(64, '0')}};
        receipt = Put(EventKind::ToolInputEffective, TurnScope(turn, request_id, call_id),
                      std::move(effective));
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        receipt = Put(EventKind::ToolExecutionStarted, TurnScope(turn, request_id, call_id),
                      nlohmann::json{{"call_id", call_id}}, Durability::ProcessCrash);
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        receipt = Put(EventKind::ToolExecutionFinished, TurnScope(turn, request_id, call_id),
                      nlohmann::json{{"outcome", "succeeded"}, {"duration_ms", 5}});
        REQUIRE(receipt.status == RecordReceipt::Status::Committed);
        return Put(EventKind::ToolResultCommitted, TurnScope(turn, request_id, call_id),
                   nlohmann::json{{"call_id", call_id},
                                  {"content", nlohmann::json::array({"text"})},
                                  {"is_error", false}});
    }
};

const char* CodeOf(const RecordReceipt& receipt) { return receipt.error_code.c_str(); }

}  // namespace

// ---------------------------------------------------------------------------
// §6.2 硬约束逐条
// ---------------------------------------------------------------------------

TEST_CASE("约束1: run.started 只能一枚") {
    Harness h("c1");
    auto first = h.recorder->WriteRunStarted(nlohmann::json::object(), Durability::PowerLoss);
    CHECK(first.status == RecordReceipt::Status::Committed);
    auto second = h.recorder->WriteRunStarted(nlohmann::json::object(), Durability::PowerLoss);
    CHECK(second.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(second)) == "state.run_started_duplicate");
}

TEST_CASE("约束1反面: 首事件不是 run.started 拒") {
    Harness h("c1b");
    auto receipt = h.Put(EventKind::TurnStarted, Harness::TurnScope("turn-0001"),
                         nlohmann::json{{"trigger", "external_user"}});
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "state.run_not_started");
}

TEST_CASE("约束2: turn 终态前必须有 turn.started") {
    Harness h("c2");
    h.OpenTurn("turn-0001");
    auto receipt = h.Put(EventKind::TurnCompleted, Harness::TurnScope("turn-0002"),
                         nlohmann::json{{"outcome", "succeeded"}});
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "state.turn_not_started");
}

TEST_CASE("约束3: input.received 先于本 turn 首次 sent") {
    Harness h("c3");
    auto started = h.recorder->WriteRunStarted(nlohmann::json::object(), Durability::PowerLoss);
    REQUIRE(started.status == RecordReceipt::Status::Committed);
    auto turn = h.Put(EventKind::TurnStarted, Harness::TurnScope("turn-0001"),
                      nlohmann::json{{"trigger", "external_user"}});
    REQUIRE(turn.status == RecordReceipt::Status::Committed);
    // 直接 prepared(不落 input)再 sent。
    EventScope prep = Harness::TurnScope("turn-0001", "req-0001");
    prep.actor = Actor::Model;
    prep.origin = Origin::ProviderModel;
    auto prepared = h.Put(EventKind::ModelRequestPrepared, prep,
                          nlohmann::json{{"model", "m"},
                                         {"provider", "p"},
                                         {"wire", "w"},
                                         {"message_refs", nlohmann::json::array()}});
    REQUIRE(prepared.status == RecordReceipt::Status::Committed);
    auto sent = h.Put(EventKind::ModelRequestSent, Harness::TurnScope("turn-0001", "req-0001"),
                      nlohmann::json{{"prepared_event_id", prepared.event_id}});
    CHECK(sent.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(sent)) == "state.turn_missing_input");
}

TEST_CASE("约束4: sent 必须引用已提交的 prepared event") {
    Harness h("c4");
    h.OpenTurn("turn-0001");
    auto receipt = h.Put(EventKind::ModelRequestSent, Harness::TurnScope("turn-0001", "req-0001"),
                         nlohmann::json{{"prepared_event_id", "main-0001:evt-00000099"}});
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "state.request_not_prepared");
}

TEST_CASE("约束5: output 必须引用已发送 request") {
    Harness h("c5");
    h.OpenTurn("turn-0001");
    auto receipt = h.Put(
        EventKind::ModelOutputCompleted, Harness::TurnScope("turn-0001", "req-0001"),
        nlohmann::json{{"output_id", "o"}, {"blocks", nlohmann::json::array()},
                       {"stop_reason", "end_turn"}});
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "state.request_not_sent");
}

TEST_CASE("约束6: call_id 在 run 内唯一(两份 output 声明同一 call 撞号)") {
    Harness h("c6");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    auto out1 = h.ModelExchange("turn-0001", "req-0001", "call-0001", true, &prepared_id);
    REQUIRE(out1.status == RecordReceipt::Status::Committed);
    // 第一枚 call 走完整流水(带 result),下一份请求才发得出去。
    h.RunTool("turn-0001", "req-0001", "call-0001", "read_only_local");
    auto out2 = h.ModelExchange("turn-0001", "req-0002", "call-0001", true, &prepared_id);
    CHECK(out2.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(out2)) == "state.call_id_duplicate");
}

TEST_CASE("约束6: planned 引用 output 声明过的 call_id") {
    Harness h("c6b");
    h.OpenTurn("turn-0001");
    auto receipt = h.Put(EventKind::ToolExecutionPlanned,
                         Harness::TurnScope("turn-0001", "req-0001", "call-ghost"),
                         nlohmann::json{{"call_id", "call-ghost"}, {"tool_name", "t"}});
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "state.call_not_declared");
}

TEST_CASE("约束7: tool started 前必须有 effective input") {
    Harness h("c7");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    auto out = h.ModelExchange("turn-0001", "req-0001", "call-0001", true, &prepared_id);
    REQUIRE(out.status == RecordReceipt::Status::Committed);
    auto receipt = h.Put(EventKind::ToolExecutionStarted,
                         Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                         nlohmann::json{{"call_id", "call-0001"}});
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "state.tool_missing_effective_input");
}

TEST_CASE("约束8落法: 副作用工具 started 给低档不拒,提档执行") {
    Harness h("c8");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    auto out = h.ModelExchange("turn-0001", "req-0001", "call-0001", true, &prepared_id);
    REQUIRE(out.status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ToolExecutionPlanned,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"call_id", "call-0001"}, {"tool_name", "write_file"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ToolInputEffective,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"call_id", "call-0001"},
                                 {"tool_name", "write_file"},
                                 {"source_kind", "builtin"},
                                 {"effect_class", "external_write"},
                                 {"effective_arguments", nlohmann::json::object()},
                                 {"effective_arguments_sha256", std::string(64, '0')}})
                .status == RecordReceipt::Status::Committed);
    // 副作用 started 即便给 Buffered 也不拒:提档 PowerLoss 是 recorder 的
    // 职责,调用方给低档不构成绕栅栏。
    auto receipt = h.Put(EventKind::ToolExecutionStarted,
                         Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                         nlohmann::json{{"call_id", "call-0001"}}, Durability::Buffered);
    CHECK(receipt.status == RecordReceipt::Status::Committed);
}

TEST_CASE("约束9: tool 终态四选一,至多一枚") {
    Harness h("c9");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    h.ModelExchange("turn-0001", "req-0001", "call-0001", true, &prepared_id);
    h.RunTool("turn-0001", "req-0001", "call-0001", "read_only_local");
    auto again = h.Put(EventKind::ToolExecutionFinished,
                       Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                       nlohmann::json{{"outcome", "succeeded"}, {"duration_ms", 1}});
    CHECK(again.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(again)) == "state.tool_terminal_duplicate");
}

TEST_CASE("约束10: tool result 必须引用已知 ToolCall 与执行终态") {
    Harness h("c10");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    h.ModelExchange("turn-0001", "req-0001", "call-0001", true, &prepared_id);
    // 未知 call。
    auto ghost = h.Put(EventKind::ToolResultCommitted,
                       Harness::TurnScope("turn-0001", "req-0001", "call-ghost"),
                       nlohmann::json{{"call_id", "call-ghost"},
                                      {"content", nlohmann::json::array()},
                                      {"is_error", false}});
    CHECK(ghost.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(ghost)) == "state.call_unknown");
    // 已声明未跑完。
    auto early = h.Put(EventKind::ToolResultCommitted,
                       Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                       nlohmann::json{{"call_id", "call-0001"},
                                      {"content", nlohmann::json::array()},
                                      {"is_error", false}});
    CHECK(early.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(early)) == "state.tool_not_terminal");
}

TEST_CASE("约束11: 下一 request 前本批 ToolCall 都要有 ToolResult") {
    Harness h("c11");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    h.ModelExchange("turn-0001", "req-0001", "call-0001", true, &prepared_id);
    // planned -> effective -> started -> finished,但不落 result。
    REQUIRE(h.Put(EventKind::ToolExecutionPlanned,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"call_id", "call-0001"}, {"tool_name", "t"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ToolInputEffective,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"call_id", "call-0001"},
                                 {"tool_name", "t"},
                                 {"source_kind", "builtin"},
                                 {"effect_class", "read_only_local"},
                                 {"effective_arguments", nlohmann::json::object()},
                                 {"effective_arguments_sha256", std::string(64, '0')}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ToolExecutionStarted,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"call_id", "call-0001"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ToolExecutionFinished,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"outcome", "succeeded"}, {"duration_ms", 1}})
                .status == RecordReceipt::Status::Committed);
    EventScope prep = Harness::TurnScope("turn-0001", "req-0002");
    prep.actor = Actor::Model;
    prep.origin = Origin::ProviderModel;
    auto next = h.Put(EventKind::ModelRequestPrepared, prep,
                      nlohmann::json{{"model", "m"},
                                     {"provider", "p"},
                                     {"wire", "w"},
                                     {"message_refs", nlohmann::json::array()}});
    CHECK(next.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(next)) == "state.tool_results_pending");
}

TEST_CASE("约束12: turn completed 不留悬空 request/call/result") {
    Harness h("c12");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    h.ModelExchange("turn-0001", "req-0001", "call-0001", true, &prepared_id);
    // 声明了 call 却没执行也没回喂。
    auto receipt = h.Put(EventKind::TurnCompleted, Harness::TurnScope("turn-0001"),
                         nlohmann::json{{"outcome", "succeeded"}});
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "state.turn_dangling_call");
}

TEST_CASE("约束13: run terminal 至多一枚;活动 turn 未收齐先拒") {
    Harness h("c13");
    h.OpenTurn("turn-0001");
    auto premature = h.recorder->FinishRun(EventKind::RunCompleted, "", Durability::PowerLoss);
    CHECK(premature.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(premature)) == "state.run_active_turn");
    REQUIRE(h.Put(EventKind::TurnCompleted, Harness::TurnScope("turn-0001"),
                  nlohmann::json{{"outcome", "succeeded"}})
                .status == RecordReceipt::Status::Committed);
    auto first = h.recorder->FinishRun(EventKind::RunCompleted, "", Durability::PowerLoss);
    CHECK(first.status == RecordReceipt::Status::Committed);
    auto second = h.recorder->FinishRun(EventKind::RunCompleted, "", Durability::PowerLoss);
    CHECK(second.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(second)) == "state.run_terminal_duplicate");
}

TEST_CASE("约束15: 未决审批不许 started;policy hash 可豁免") {
    Harness h("c15");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    h.ModelExchange("turn-0001", "req-0001", "call-0001", true, &prepared_id);
    REQUIRE(h.Put(EventKind::ControlApprovalRequested,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"approval_id", "appr-0001"}, {"call_id", "call-0001"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ToolExecutionPlanned,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"call_id", "call-0001"}, {"tool_name", "t"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ToolInputEffective,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"call_id", "call-0001"},
                                 {"tool_name", "t"},
                                 {"source_kind", "builtin"},
                                 {"effect_class", "external_write"},
                                 {"effective_arguments", nlohmann::json::object()},
                                 {"effective_arguments_sha256", std::string(64, '0')}})
                .status == RecordReceipt::Status::Committed);
    auto pending = h.Put(EventKind::ToolExecutionStarted,
                         Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                         nlohmann::json{{"call_id", "call-0001"}});
    CHECK(pending.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(pending)) == "state.approval_pending");

    // allow 后放行。
    REQUIRE(h.Put(EventKind::ControlApprovalResolved,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"approval_id", "appr-0001"}, {"decision", "allow"}})
                .status == RecordReceipt::Status::Committed);
    auto allowed = h.Put(EventKind::ToolExecutionStarted,
                         Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                         nlohmann::json{{"call_id", "call-0001"}});
    CHECK(allowed.status == RecordReceipt::Status::Committed);
}

TEST_CASE("约束16: deny/expired 后不得 started") {
    Harness h("c16");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    h.ModelExchange("turn-0001", "req-0001", "call-0001", true, &prepared_id);
    REQUIRE(h.Put(EventKind::ControlApprovalRequested,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"approval_id", "appr-0001"}, {"call_id", "call-0001"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ControlApprovalResolved,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"approval_id", "appr-0001"}, {"decision", "deny"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ToolExecutionPlanned,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"call_id", "call-0001"}, {"tool_name", "t"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ToolInputEffective,
                  Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                  nlohmann::json{{"call_id", "call-0001"},
                                 {"tool_name", "t"},
                                 {"source_kind", "builtin"},
                                 {"effect_class", "external_write"},
                                 {"effective_arguments", nlohmann::json::object()},
                                 {"effective_arguments_sha256", std::string(64, '0')}})
                .status == RecordReceipt::Status::Committed);
    auto denied = h.Put(EventKind::ToolExecutionStarted,
                        Harness::TurnScope("turn-0001", "req-0001", "call-0001"),
                        nlohmann::json{{"call_id", "call-0001"}});
    CHECK(denied.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(denied)) == "state.approval_denied");
}

TEST_CASE("约束17: queue item 三选一,至多一枚") {
    Harness h("c17");
    h.OpenTurn("turn-0001");
    REQUIRE(h.Put(EventKind::ControlQueueItemEnqueued, Harness::BaseScope(),
                  nlohmann::json{{"item_id", "q-1"}, {"input_id", "input-0009"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ControlQueueItemDequeued, Harness::BaseScope(),
                  nlohmann::json{{"item_id", "q-1"}, {"input_id", "input-0009"}})
                .status == RecordReceipt::Status::Committed);
    auto again = h.Put(EventKind::ControlQueueItemCancelled, Harness::BaseScope(),
                       nlohmann::json{{"item_id", "q-1"}, {"reason", "clear"}});
    CHECK(again.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(again)) == "state.queue_terminal_duplicate");
}

TEST_CASE("约束18: dequeued 的 input_id 必须与 turn/input 对上") {
    Harness h("c18");
    h.OpenTurn("turn-0001");
    REQUIRE(h.Put(EventKind::TurnCompleted, Harness::TurnScope("turn-0001"),
                  nlohmann::json{{"outcome", "succeeded"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.Put(EventKind::ControlQueueItemEnqueued, Harness::BaseScope(),
                  nlohmann::json{{"item_id", "q-1"}, {"input_id", "input-0009"}})
                .status == RecordReceipt::Status::Committed);
    // 未 dequeued 便拿它触发 turn。
    auto early = h.Put(EventKind::TurnStarted, Harness::TurnScope("turn-0002"),
                       nlohmann::json{{"trigger", "queued_user"},
                                      {"queue_item_input_id", "input-0009"}});
    CHECK(early.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(early)) == "state.queue_item_not_dequeued");

    REQUIRE(h.Put(EventKind::ControlQueueItemDequeued, Harness::BaseScope(),
                  nlohmann::json{{"item_id", "q-1"}, {"input_id", "input-0009"}})
                .status == RecordReceipt::Status::Committed);
    auto turn = h.Put(EventKind::TurnStarted, Harness::TurnScope("turn-0002"),
                      nlohmann::json{{"trigger", "queued_user"},
                                     {"queue_item_input_id", "input-0009"}});
    REQUIRE(turn.status == RecordReceipt::Status::Committed);

    // dequeued 之后另造 input id,拒绝。
    EventScope wrong_scope = Harness::TurnScope("turn-0002");
    wrong_scope.actor = Actor::User;
    wrong_scope.origin = Origin::QueuedUser;
    auto wrong = h.Put(EventKind::InputReceived, wrong_scope,
                       nlohmann::json{{"input_id", "input-0424"},
                                      {"content", nlohmann::json::array({"t"})},
                                      {"channel", "terminal"},
                                      {"sender", nlohmann::json{{"kind", "local_user"}}}});
    CHECK(wrong.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(wrong)) == "state.queue_input_mismatch");

    // 对上号则过。
    EventScope right_scope = Harness::TurnScope("turn-0002");
    right_scope.actor = Actor::User;
    right_scope.origin = Origin::QueuedUser;
    auto right = h.Put(EventKind::InputReceived, right_scope,
                       nlohmann::json{{"input_id", "input-0009"},
                                      {"content", nlohmann::json::array({"t"})},
                                      {"channel", "terminal"},
                                      {"sender", nlohmann::json{{"kind", "local_user"}}}});
    CHECK(right.status == RecordReceipt::Status::Committed);
}

// ---------------------------------------------------------------------------
// 链、封口、拒写语义
// ---------------------------------------------------------------------------

TEST_CASE("hash 链确定性: 同一串事件两遍录制,event_hash 序列与整本字节一致") {
    const auto run_once = [](const std::filesystem::path& dir) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "artifacts", ec);
        FixedClock clock;
        auto recorder = TrajectoryRecorder::Start(dir / "main.jsonl", dir / "artifacts",
                                                  Harness::BaseScope(), RecorderOptions{}, &clock);
        REQUIRE(recorder.has_value());
        recorder->WriteRunStarted(nlohmann::json{{"start_reason", "process_launch"}},
                                  Durability::PowerLoss);
        EventScope turn_scope = Harness::TurnScope("turn-0001");
        turn_scope.actor = Actor::User;
        turn_scope.origin = Origin::ExternalUser;
        RecordRequest turn_request;
        turn_request.kind = EventKind::TurnStarted;
        turn_request.scope = turn_scope;
        turn_request.payload = nlohmann::json{{"trigger", "external_user"}};
        REQUIRE(recorder->Record(std::move(turn_request), Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);
        RecordRequest input_request;
        input_request.kind = EventKind::InputReceived;
        input_request.scope = turn_scope;
        input_request.payload = nlohmann::json{
            {"input_id", "input-0001"},
            {"content", nlohmann::json::array({nlohmann::json{{"type", "text"},
                                                              {"text", "问一声"}}})},
            {"channel", "terminal"},
            {"sender", nlohmann::json{{"kind", "local_user"}}}};
        REQUIRE(recorder->Record(std::move(input_request), Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);
        RecordRequest close_request;
        close_request.kind = EventKind::TurnCompleted;
        close_request.scope = turn_scope;
        close_request.payload = nlohmann::json{{"outcome", "succeeded"}};
        REQUIRE(recorder->Record(std::move(close_request), Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);
        REQUIRE(recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss).status ==
                RecordReceipt::Status::Committed);
        const auto report = VerifyJournalFile(dir / "main.jsonl");
        REQUIRE(report.ok);
        return *recorder->Close();
    };
    const auto a = run_once(std::filesystem::temp_directory_path() / "lubancode-traj-det-a");
    const auto b = run_once(std::filesystem::temp_directory_path() / "lubancode-traj-det-b");
    CHECK(a == b);  // 整本 journal_sha256 一致 => 字节一致
    CHECK(IsHex64(a));
}

TEST_CASE("seq 连发号;拒绝不消耗号") {
    Harness h("seq");
    h.OpenTurn("turn-0001");
    CHECK(h.recorder->next_seq() == 4);  // run.started + turn.started + input.received
    auto rejected = h.Put(EventKind::TurnCompleted, Harness::TurnScope("turn-0002"),
                          nlohmann::json{{"outcome", "succeeded"}});
    REQUIRE(rejected.status == RecordReceipt::Status::Rejected);
    CHECK(h.recorder->next_seq() == 4);  // 拒绝不前移
    auto ok = h.Put(EventKind::ControlTitleChanged, Harness::BaseScope(),
                    nlohmann::json{{"title", "新标题"}});
    REQUIRE(ok.status == RecordReceipt::Status::Committed);
    CHECK(ok.seq == 4);
    CHECK(ok.event_id == "main-0001:evt-00000004");
    CHECK(IsHex64(ok.event_hash));
}

TEST_CASE("终态封口四件套落在 run terminal") {
    Harness h("seal");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    h.ModelExchange("turn-0001", "req-0001", "call-0001", false, &prepared_id);
    REQUIRE(h.Put(EventKind::TurnCompleted, Harness::TurnScope("turn-0001"),
                  nlohmann::json{{"outcome", "succeeded"}})
                .status == RecordReceipt::Status::Committed);
    auto seal = h.recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss);
    REQUIRE(seal.status == RecordReceipt::Status::Committed);
    // 落盘行核对封口字段。
    const auto lines = ReadJournalLines(h.dir / "main.jsonl");
    REQUIRE(lines.has_value());
    REQUIRE_FALSE(lines->empty());
    const auto last = nlohmann::json::parse(lines->back());
    CHECK(last.at("kind") == "run.completed");
    CHECK(last.at("payload").contains("first_event_hash"));
    CHECK(last.at("payload").contains("event_count_before_terminal"));
    CHECK(last.at("payload").at("schema_version") == 1);
    CHECK(last.at("payload").contains("recorder_version"));
    CHECK(last.at("seq") == lines->size());
}

TEST_CASE("session.ended 收链,之后一概拒写;关柄后 journal_sha256 可算") {
    Harness h("ended");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    h.ModelExchange("turn-0001", "req-0001", "call-0001", false, &prepared_id);
    REQUIRE(h.Put(EventKind::TurnCompleted, Harness::TurnScope("turn-0001"),
                  nlohmann::json{{"outcome", "succeeded"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.recorder->FinishRun(EventKind::RunCompleted, "", Durability::PowerLoss).status ==
            RecordReceipt::Status::Committed);
    auto ended = h.recorder->EndSession("exit", std::nullopt, "clean", Durability::PowerLoss);
    REQUIRE(ended.status == RecordReceipt::Status::Committed);
    auto after = h.Put(EventKind::ControlTitleChanged, Harness::BaseScope(),
                       nlohmann::json{{"title", "x"}});
    CHECK(after.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(after)) == "state.session_ended");

    const auto journal_sha = h.recorder->Close();
    REQUIRE(journal_sha.has_value());
    CHECK(IsHex64(*journal_sha));
    auto closed = h.Put(EventKind::ControlTitleChanged, Harness::BaseScope(),
                        nlohmann::json{{"title", "y"}});
    CHECK(closed.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(closed)) == "io.closed");
}

TEST_CASE("run terminal 后写业务事件拒绝(session.ended 除外)") {
    Harness h("runclosed");
    h.OpenTurn("turn-0001");
    REQUIRE(h.Put(EventKind::TurnCancelled, Harness::TurnScope("turn-0001"),
                  nlohmann::json{{"reason", "esc"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(h.recorder->FinishRun(EventKind::RunCancelled, "esc", Durability::PowerLoss).status ==
            RecordReceipt::Status::Committed);
    auto receipt = h.Put(EventKind::ControlTitleChanged, Harness::BaseScope(),
                         nlohmann::json{{"title", "x"}});
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "state.run_closed");
}

TEST_CASE("session.* 只许 main stream") {
    Harness h("notmain");
    // 把 base 换成 subagent 再开一只 recorder。
    auto dir2 = h.dir / "sub";
    std::error_code ec;
    std::filesystem::create_directories(dir2 / "artifacts", ec);
    EventScope scope = Harness::BaseScope();
    scope.run_id = "agent-0002";
    scope.run_kind = RunKind::Subagent;
    auto sub = TrajectoryRecorder::Start(dir2 / "agent-0002.jsonl", dir2 / "artifacts", scope,
                                         RecorderOptions{}, &h.clock);
    REQUIRE(sub.has_value());
    REQUIRE(sub->WriteRunStarted(nlohmann::json{{"run_kind", "subagent"},
                                                {"agent_run_id", "agent-0002"},
                                                {"owner_run_id", "main-0001"}},
                                 Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    RecordRequest request;
    request.kind = EventKind::SessionClearRequested;
    request.scope = scope;
    request.payload = nlohmann::json{{"next_session_id", "20260830-999999-ZZZZZZ"}};
    auto receipt = sub->Record(std::move(request), Durability::PowerLoss);
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "state.session_event_not_main");
}

// 单发轨迹断档单:one_shot 的 main stream 同放行 session.*(单发一场也是
// 进程的 main stream;不放开的话 Close 的 session.ended 落不进去)。
TEST_CASE("session.* 在 one_shot main stream 照放行") {
    Harness h("oneshotmain");
    auto dir2 = h.dir / "oneshot";
    std::error_code ec;
    std::filesystem::create_directories(dir2 / "artifacts", ec);
    EventScope scope = Harness::BaseScope();
    scope.run_kind = RunKind::OneShot;
    auto once = TrajectoryRecorder::Start(dir2 / "main.jsonl", dir2 / "artifacts", scope,
                                          RecorderOptions{}, &h.clock);
    REQUIRE(once.has_value());
    REQUIRE(once->WriteRunStarted(nlohmann::json::object(), Durability::PowerLoss).status ==
            RecordReceipt::Status::Committed);
    // run.started 的 payload run_kind 与信封同源(WriteRunStarted 从 base 取)。
    const auto events = [&]() {
        const auto lines = ReadJournalLines(dir2 / "main.jsonl");
        REQUIRE(lines.has_value());
        return nlohmann::json::parse(lines->front());
    }();
    CHECK(events.at("run_kind").get<std::string>() == "one_shot");
    CHECK(events.at("payload").at("run_kind").get<std::string>() == "one_shot");

    REQUIRE(once->FinishRun(EventKind::RunCompleted, "exit", Durability::PowerLoss).status ==
            RecordReceipt::Status::Committed);
    auto ended = once->EndSession("exit", std::nullopt, "clean", Durability::PowerLoss);
    CHECK(ended.status == RecordReceipt::Status::Committed);
}

TEST_CASE("scope 恒等:换 workspace/session/run/kind 拒绝") {
    Harness h("scope");
    h.recorder->WriteRunStarted(nlohmann::json::object(), Durability::PowerLoss);
    EventScope scope = Harness::BaseScope();
    scope.run_id = "other-run";
    RecordRequest request;
    request.kind = EventKind::ControlTitleChanged;
    request.scope = scope;
    request.payload = nlohmann::json{{"title", "x"}};
    auto receipt = h.recorder->Record(std::move(request), Durability::Buffered);
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(receipt.error_code == "scope.mismatch");
}

TEST_CASE("相同正文不同 id 照常落;不因内容去重") {
    Harness h("dedup");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    auto out = h.ModelExchange("turn-0001", "req-0001", "call-0001", false, &prepared_id);
    REQUIRE(out.status == RecordReceipt::Status::Committed);
    auto out2 = h.ModelExchange("turn-0001", "req-0002", "call-0002", false, &prepared_id);
    CHECK(out2.status == RecordReceipt::Status::Committed);
}

TEST_CASE("超限正文 offload 成 BlobRef,内联不超限") {
    Harness h("offload");
    h.OpenTurn("turn-0001");
    const std::string big(40000, 'Q');  // 40KB,超 32KiB 上限
    EventScope scope = Harness::TurnScope("turn-0001");
    scope.actor = Actor::User;
    scope.origin = Origin::ExternalUser;
    auto receipt = h.Put(EventKind::InputReceived, scope,
                         nlohmann::json{{"input_id", "input-big"},
                                        {"content", nlohmann::json::array({nlohmann::json{
                                            {"type", "text"}, {"text", big}}})},
                                        {"channel", "terminal"},
                                        {"sender", nlohmann::json{{"kind", "local_user"}}}});
    REQUIRE(receipt.status == RecordReceipt::Status::Committed);
    const auto lines = ReadJournalLines(h.dir / "main.jsonl");
    REQUIRE(lines.has_value());
    const auto last = nlohmann::json::parse(lines->back());
    const auto& text = last.at("payload").at("content").at(0).at("text");
    REQUIRE(text.is_object());
    CHECK(text.at("size") == big.size());
    CHECK(BlobRef::MatchesShape(text));
    // blob 落盘且读回一致。
    const auto ref = BlobRef::FromJson(text);
    REQUIRE(ref.has_value());
    BlobStore store(h.dir / "artifacts");
    const auto back = store.ReadVerified(*ref);
    REQUIRE(back.has_value());
    CHECK(*back == big);
}

TEST_CASE("durability 三档都能写,字节不受档位影响") {
    Harness h("dur");
    h.OpenTurn("turn-0001");
    std::string prepared_id;
    auto a = h.ModelExchange("turn-0001", "req-0001", "call-0001", false, &prepared_id);
    REQUIRE(a.status == RecordReceipt::Status::Committed);
    // Buffered / ProcessCrash / PowerLoss 各写一枚 control 事件。
    for (const Durability durability :
         {Durability::Buffered, Durability::ProcessCrash, Durability::PowerLoss}) {
        auto receipt = h.Put(EventKind::ControlContextWindowChanged, Harness::TurnScope("turn-0001"),
                             nlohmann::json{{"context_window", "128k"}}, durability);
        CHECK(receipt.status == RecordReceipt::Status::Committed);
    }
}
