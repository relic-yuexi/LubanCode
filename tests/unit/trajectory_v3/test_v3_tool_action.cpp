// v3 工具操作账测试(P1 其余,§4.14-4.21):生命周期三段、全局身份、
// attempt 重试链、幂等键、每尝试一个终态、结果选用与 tool 消息配对。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/result_store.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;

namespace {

class FixedClock : public V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct Harness {
    FixedClock clock;
    std::filesystem::path dir;
    std::filesystem::path jsonl;

    explicit Harness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-v3-tool-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "session.jsonl";
    }

    std::optional<V3Writer> Start() {
        auto writer = V3Writer::Start(jsonl, "20260910-130000-TOOL01", "run-000001",
                                      "你是 LubanCode。", nlohmann::json::object(),
                                      V3WriterOptions{}, &clock);
        if (!writer.has_value()) {
            return std::nullopt;
        }
        return std::move(*writer);
    }
};

std::vector<nlohmann::json> ReadJson(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<nlohmann::json> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(nlohmann::json::parse(line));
        }
    }
    return lines;
}

ToolIdentity SampleIdentity(std::string name = "run_command") {
    return ToolIdentity{std::move(name), "builtin", "1.0.0", "cwd=/repo"};
}

}  // namespace

TEST_CASE("同一 turn 两次同名同参调用:两枚 Action,不因 args 相同而合并") {
    Harness harness("two-actions");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    std::string turn = writer->NewTurnId();
    std::string step = writer->NewStepId();

    std::string first_id = writer->NewActionId();
    std::string second_id = writer->NewActionId();
    CHECK(first_id != second_id);

    auto first = ToolActionSession::Admit(*writer, turn, step, first_id, "queued", "msg-1",
                                          "call_aaa");
    auto second = ToolActionSession::Admit(*writer, turn, step, second_id, "queued", "msg-1",
                                           "call_bbb");
    REQUIRE(first.Start(*writer, "args-ref-1", SampleIdentity()).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(second.Start(*writer, "args-ref-1", SampleIdentity()).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(first.Finish(*writer, 0, 120).status == WriteReceipt::Status::Committed);
    REQUIRE(second.Finish(*writer, 0, 130).status == WriteReceipt::Status::Committed);

    // 幂等键:同 args 不同 action → 不同 key(§4.15:两次新声明不合并)。
    std::string key_first =
        ComputeToolIdempotencyKey(first_id, SampleIdentity(), "args-hash", "scope-hash");
    std::string key_second =
        ComputeToolIdempotencyKey(second_id, SampleIdentity(), "args-hash", "scope-hash");
    CHECK(key_first != key_second);
    CHECK(key_first.size() == 64);

    auto lines = ReadJson(harness.jsonl);
    int executions = 0;
    for (const auto& line : lines) {
        if (line.value("kind", "") == "tool.execution.finished") {
            ++executions;
        }
    }
    CHECK(executions == 2);  // 不因 args hash 相同而漏执行
}

TEST_CASE("幂等键:同 action 传输重试复用;不含 attempt") {
    const std::string key = ComputeToolIdempotencyKey(
        "action-000001", SampleIdentity(), "args-hash", "scope-hash");
    const std::string same = ComputeToolIdempotencyKey(
        "action-000001", SampleIdentity(), "args-hash", "scope-hash");
    CHECK(key == same);  // 确定性
    // 改参数指纹 → 意图变了,key 不得复用(§4.15)。
    const std::string changed = ComputeToolIdempotencyKey(
        "action-000001", SampleIdentity(), "args-hash-2", "scope-hash");
    CHECK(key != changed);
    // 改执行目标 → 不复用原意图。
    const std::string other_scope = ComputeToolIdempotencyKey(
        "action-000001", SampleIdentity(), "args-hash", "scope-hash-2");
    CHECK(key != other_scope);
}

TEST_CASE("attempt 重试链:先记上一 attempt 终态,再对 attempt+1 记 pending") {
    Harness harness("retry-chain");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto session = ToolActionSession::Admit(*writer, "turn-000001", "step-000001",
                                            "action-000001", "queued", std::nullopt,
                                            std::nullopt);
    CHECK(session.attempt() == 1);
    REQUIRE(session.Start(*writer, "args-ref", SampleIdentity()).status ==
            WriteReceipt::Status::Committed);
    // attempt 1 失败收口。
    REQUIRE(session.Fail(*writer, "executor_timeout").status == WriteReceipt::Status::Committed);
    CHECK(session.terminal() == ToolActionSession::Terminal::Failed);
    // 未终态就开下一 attempt:拒收(§4.14)——此处已终态,应放行。
    REQUIRE(session.BeginNextAttempt(*writer, "backoff").status ==
            WriteReceipt::Status::Committed);
    CHECK(session.attempt() == 2);
    REQUIRE(session.Start(*writer, "args-ref", SampleIdentity()).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(session.Finish(*writer, 0, 200).status == WriteReceipt::Status::Committed);

    auto lines = ReadJson(harness.jsonl);
    std::vector<std::pair<std::string, int>> lifecycle;
    for (const auto& line : lines) {
        std::string kind = line.value("kind", "");
        if (kind.rfind("tool.execution.", 0) == 0) {
            lifecycle.emplace_back(kind, line["payload"]["attempt"].get<int>());
        }
    }
    // 顺序固定:pending(1) started(1) failed(1) pending(2) started(2) finished(2)。
    REQUIRE(lifecycle.size() == 6);
    CHECK(lifecycle[0] == std::make_pair(std::string("tool.execution.pending"), 1));
    CHECK(lifecycle[2] == std::make_pair(std::string("tool.execution.failed"), 1));
    CHECK(lifecycle[3] == std::make_pair(std::string("tool.execution.pending"), 2));
    CHECK(lifecycle[5] == std::make_pair(std::string("tool.execution.finished"), 2));
    // 旧 attempt 的 pending 不被覆盖(§4.14:不把已结束的尝试改回 pending)。
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("每次尝试最多一个执行终态:第二次终态拒收") {
    Harness harness("one-terminal");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto session = ToolActionSession::Admit(*writer, "turn-000001", "step-000001",
                                            "action-000001", "queued", std::nullopt, std::nullopt);
    REQUIRE(session.Start(*writer, "args-ref", SampleIdentity()).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(session.Finish(*writer, 0, 10).status == WriteReceipt::Status::Committed);
    // 迟到失败不得改旧终态(§4.14)。
    WriteReceipt late = session.Fail(*writer, "late_error");
    CHECK(late.status == WriteReceipt::Status::Rejected);
    CHECK(late.error_code == "v3tool.already_terminal");
    WriteReceipt late_unknown = session.MarkUnknown(*writer, "late");
    CHECK(late_unknown.status == WriteReceipt::Status::Rejected);
}

TEST_CASE("等待与恢复:reason + waitRef;快工具不虚造 waiting") {
    Harness harness("waiting");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto session = ToolActionSession::Admit(*writer, "turn-000001", "step-000001",
                                            "action-000001", "approval", "msg-000002",
                                            std::nullopt);
    // 未 started 不得 waiting。
    CHECK(session.Wait(*writer, "approval", "approval-1").status == WriteReceipt::Status::Rejected);
    REQUIRE(session.Start(*writer, "args-ref", SampleIdentity()).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(session.Wait(*writer, "approval", "approval-1").status ==
            WriteReceipt::Status::Committed);
    REQUIRE(session.Resume(*writer).status == WriteReceipt::Status::Committed);
    // 没有第二次等待就没有第二次 resumed。
    CHECK(session.Resume(*writer).status == WriteReceipt::Status::Rejected);
    REQUIRE(session.Finish(*writer, 0, 50).status == WriteReceipt::Status::Committed);

    auto lines = ReadJson(harness.jsonl);
    bool saw_waiting = false;
    for (const auto& line : lines) {
        if (line.value("kind", "") == "tool.execution.waiting") {
            saw_waiting = true;
            CHECK(line["status"] == "pending");
            CHECK(line["payload"]["reason"] == "approval");
            CHECK(line["payload"]["waitRef"] == "approval-1");  // 可恢复等待引用
        }
    }
    CHECK(saw_waiting);
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("快工具:不虚造 waiting;rejected 没有执行") {
    Harness harness("fast-reject");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    // 快工具:接纳后直接 started(§4.14)。
    auto fast = ToolActionSession::Admit(*writer, "turn-000001", "step-000001",
                                         "action-000001", "queued", std::nullopt, std::nullopt);
    REQUIRE(fast.Start(*writer, "args-ref", SampleIdentity()).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(fast.Finish(*writer, 0, 5).status == WriteReceipt::Status::Committed);

    // 拒绝:参数/权限/准入,没有 started(§4.14/§4.20)。
    auto denied = ToolActionSession::Admit(*writer, "turn-000001", "step-000001",
                                           "action-000002", "queued", std::nullopt, std::nullopt);
    REQUIRE(denied.Reject(*writer, "permission_denied").status == WriteReceipt::Status::Committed);
    CHECK(denied.terminal() == ToolActionSession::Terminal::Rejected);

    auto lines = ReadJson(harness.jsonl);
    bool saw_started_for_denied = false;
    for (const auto& line : lines) {
        if (line.value("actionId", "") != "action-000002") {
            continue;
        }
        if (line.value("kind", "") == "tool.execution.rejected") {
            CHECK(line["status"] == "rejected");
            CHECK(line["payload"]["reason"] == "permission_denied");
            CHECK(!line["payload"].contains("attempt"));  // 未执行,不必带 attempt
        }
        if (line.value("kind", "") == "tool.execution.started") {
            saw_started_for_denied = true;  // 拒绝的调用不得有 started(§4.20)
        }
    }
    CHECK(!saw_started_for_denied);
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("payload.tool_call_id 须等于信封 actionId:错配拒收") {
    Harness harness("id-mismatch");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    EventDraft draft;
    draft.kind = EventKindV3::ToolExecutionPending;
    draft.status = OpStatus::Pending;
    draft.turn_id = "turn-000001";
    draft.step_id = "step-000001";
    draft.action_id = "action-000001";
    draft.payload = nlohmann::json::object({{"tool_call_id", "action-000099"},
                                            {"attempt", 1},
                                            {"reason", "queued"}});
    WriteReceipt receipt = writer->AppendEvent(std::move(draft));
    CHECK(receipt.status == WriteReceipt::Status::Rejected);
    CHECK(receipt.error_code == "schema3.tool_call_id_mismatch");
}

TEST_CASE("结果链:persisted → selected → tool 消息配对并接纳") {
    Harness harness("result-chain");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto session = ToolActionSession::Admit(*writer, "turn-000001", "step-000001",
                                            "action-000001", "queued", std::nullopt, std::nullopt);
    REQUIRE(session.Start(*writer, "args-ref", SampleIdentity()).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(session.Finish(*writer, 0, 80).status == WriteReceipt::Status::Committed);

    // 结果落稳(§4.19 形状):result_ref 数组只有一份也 [ref]。
    nlohmann::json result_ref = nlohmann::json::array(
        {MakeArtifactRef("res-000001", "result_metadata", "artifacts/res-000001.json",
                         std::string(64, '1'), 412, "application/json")});
    WriteReceipt persisted =
        session.PersistedResult(*writer, result_ref.get<std::vector<nlohmann::json>>(),
                                session.last_event_id());
    REQUIRE(persisted.status == WriteReceipt::Status::Committed);

    // 选用:无改写也明确选择原结果(§4.23)。
    WriteReceipt selected = session.SelectResult(*writer, {persisted.id}, {}, "done");
    REQUIRE(selected.status == WriteReceipt::Status::Committed);

    // 最终 tool 消息:配对 actionId,正文为模型预览;落稳即接纳(§4.18)。
    std::size_t chain_before = writer->context().chain.size();
    WriteReceipt message = session.AppendToolMessage(*writer, "exit_code: 0\nok",
                                                     session.selected_event_id());
    REQUIRE(message.status == WriteReceipt::Status::Committed);
    CHECK(writer->context().chain.size() == chain_before + 1);

    auto lines = ReadJson(harness.jsonl);
    const nlohmann::json* tool_message = nullptr;
    for (const auto& line : lines) {
        if (line.value("type", "") == "message" && line["message"]["role"] == "tool") {
            tool_message = &line;
        }
    }
    REQUIRE(tool_message != nullptr);
    CHECK((*tool_message)["actionId"] == "action-000001");
    CHECK((*tool_message)["message"]["tool_call_id"] == "action-000001");
    CHECK((*tool_message)["resultSelectionRef"] == selected.id);
    CHECK(VerifyV3File(harness.jsonl).ok);
}

TEST_CASE("执行成功而结果保存失败:保留 done,另报 persist_failed") {
    Harness harness("persist-failed");
    auto writer = harness.Start();
    REQUIRE(writer.has_value());
    auto session = ToolActionSession::Admit(*writer, "turn-000001", "step-000001",
                                            "action-000001", "queued", std::nullopt, std::nullopt);
    REQUIRE(session.Start(*writer, "args-ref", SampleIdentity()).status ==
            WriteReceipt::Status::Committed);
    REQUIRE(session.Finish(*writer, 0, 30).status == WriteReceipt::Status::Committed);
    WriteReceipt failed = session.PersistFailed(*writer, "disk_full");
    REQUIRE(failed.status == WriteReceipt::Status::Committed);

    auto lines = ReadJson(harness.jsonl);
    bool has_done = false;
    bool has_persist_failed = false;
    for (const auto& line : lines) {
        if (line.value("kind", "") == "tool.execution.finished") {
            has_done = true;  // 保留成功执行事实,不改称未执行(§4.18)
        }
        if (line.value("kind", "") == "tool.result.persist_failed") {
            has_persist_failed = true;
            CHECK(line["payload"]["reason"] == "disk_full");
        }
    }
    CHECK(has_done);
    CHECK(has_persist_failed);
    CHECK(VerifyV3File(harness.jsonl).ok);
}
