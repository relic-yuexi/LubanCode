// 异步工具单 P2 册:ResultDeliveryPlanner——请求边界选已提交结果(§7)。
// 完成通知只入 mailbox;冻结输入后到的留给下次;稳定提交次序;不跨目标
// 分支;prepared→acknowledged/uncertain 的投递账;恢复(原文已落仓、消息
// 未提交 → 补投递不重跑,单 §6 表)。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/result_delivery_planner.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode;
using namespace lubancode::runtime;
namespace v3 = lubancode::trajectory::v3;

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

CompletionNotice MakeNotice(const std::string& job_id, std::uint64_t ordinal,
                            const std::string& call_id = "call_a",
                            const std::string& branch = "20260916-120000-PLAN") {
    CompletionNotice notice;
    notice.job_id = job_id;
    notice.action_id = "action-job-" + job_id.substr(4);
    notice.mode = "native_deferred";
    notice.provider_call_id = call_id;
    notice.turn_id = "turn-000001";
    notice.step_id = "step-000001";
    notice.result_ref = "evt-result-" + job_id;
    notice.result_version = 1;
    notice.preview = "业务结果 " + job_id;
    notice.attempt = 2;
    notice.terminal_kind = 1;
    notice.commit_ordinal = ordinal;
    notice.branch = branch;
    return notice;
}

struct PlannerHarness {
    EnvGuard v3env{"LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1"};
    std::filesystem::path dir;
    std::optional<v3::V3Writer> writer;
    std::shared_ptr<std::recursive_mutex> writer_mutex = std::make_shared<std::recursive_mutex>();
    std::unique_ptr<runtime::ResultDeliveryPlannerImpl> planner;
    std::string evidence_event_id;
    std::function<std::optional<std::string>(const std::string&)> evidence_of;

    explicit PlannerHarness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-async-planner-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        auto started = v3::V3Writer::Start(dir / "s1.jsonl", "20260916-120000-PLAN", "run-000001",
                                           "system prompt");
        REQUIRE(started.has_value());
        writer = std::move(*started);
        // 账上先落一枚真事件(acknowledged 的 evidenceRef 要指到账上事件)。
        v3::EventDraft stub;
        stub.kind = v3::EventKindV3::ToolCapabilityRecorded;
        stub.payload = nlohmann::json::object({
            {"basis", nlohmann::json::object({{"provider", "openai"},
                                              {"wire", "responses"},
                                              {"model", "gpt-test"}})},
            {"verdicts", nlohmann::json::object({
                {"job_handle", nlohmann::json::object({{"status", "verified"}})},
            })},
        });
        auto receipt = writer->AppendEvent(std::move(stub), v3::Durability::ProcessCrash);
        REQUIRE(receipt.status == v3::WriteReceipt::Status::Committed);
        evidence_event_id = receipt.id;

        runtime::ResultDeliveryPlannerImpl::Hooks hooks;
        hooks.writer = &*writer;
        hooks.writer_mutex = writer_mutex;
        hooks.current_turn_id = [] { return std::string("turn-000001"); };
        evidence_of = [this](const std::string&) -> std::optional<std::string> {
            return evidence_event_id;
        };
        hooks.response_evidence = evidence_of;
        planner = std::make_unique<runtime::ResultDeliveryPlannerImpl>(std::move(hooks));
    }

    v3::V3Ledger Read() {
        auto ledger = v3::ReadV3Ledger(dir / "s1.jsonl");
        REQUIRE_MESSAGE(ledger.has_value(), ledger.error_or(""));
        return *ledger;
    }
};

std::string StateOf(const v3::V3Ledger& ledger, const std::string& delivery_id) {
    auto deliveries = v3::FoldDeliveries(ledger);
    const auto* found = v3::FindDelivery(deliveries, delivery_id);
    if (found == nullptr) {
        return "";
    }
    return found->state;
}

}  // namespace

TEST_CASE("mailbox:完成通知只入 mailbox;选取后到的不动已选,下次再取") {
    PlannerHarness h("mailbox");
    h.planner->NotifyCompletion(MakeNotice("job-000001", 1));
    CHECK(h.planner->mailbox_size() == 1);

    auto first = h.planner->SelectForRequestBoundary();
    REQUIRE(first.size() == 1);
    REQUIRE(first[0].content.size() == 1);
    const auto* block = std::get_if<api::ToolResultBlock>(&first[0].content[0]);
    REQUIRE(block != nullptr);
    CHECK(block->tool_use_id == "call_a");
    CHECK(block->content == "业务结果 job-000001");
    CHECK(h.planner->mailbox_size() == 0);  // 已选中出 mailbox

    // 本次选取之后到的完成通知:留给下次(冻结输入不动)。
    h.planner->NotifyCompletion(MakeNotice("job-000002", 2, "call_b"));
    CHECK(h.planner->mailbox_size() == 1);
    auto second = h.planner->SelectForRequestBoundary();
    REQUIRE(second.size() == 1);
    const auto* block_b = std::get_if<api::ToolResultBlock>(&second[0].content[0]);
    REQUIRE(block_b != nullptr);
    CHECK(block_b->tool_use_id == "call_b");
}

TEST_CASE("mailbox:重复/乱序通知按 result_version 去重,同 job 只投一次") {
    PlannerHarness h("dedup");
    h.planner->NotifyCompletion(MakeNotice("job-000001", 1));
    h.planner->NotifyCompletion(MakeNotice("job-000001", 1));  // 重复
    CHECK(h.planner->mailbox_size() == 1);
    auto first = h.planner->SelectForRequestBoundary();
    REQUIRE(first.size() == 1);
    // 投递中的 job:迟到通知不重复投。
    h.planner->NotifyCompletion(MakeNotice("job-000001", 1));
    CHECK(h.planner->pending_native_count() == 0);
    CHECK(h.planner->mailbox_size() == 0);
}

TEST_CASE("mailbox:不跨目标分支——他分支的通知不投") {
    PlannerHarness h("branch");
    CompletionNotice foreign = MakeNotice("job-000001", 1);
    foreign.branch = "20260916-999999-OTHER";
    h.planner->NotifyCompletion(std::move(foreign));
    CHECK(h.planner->SelectForRequestBoundary().empty());
    CHECK(h.planner->mailbox_size() == 1);  // 留在 mailbox,不投也不丢
}

TEST_CASE("投递账:prepared → acknowledged(evidenceRef 指账上事件)") {
    PlannerHarness h("delivery");
    h.planner->NotifyCompletion(MakeNotice("job-000001", 1));
    auto selected = h.planner->SelectForRequestBoundary();
    REQUIRE(selected.size() == 1);

    h.planner->NoteRequestPrepared("request-000002");
    {
        v3::V3Ledger ledger = h.Read();
        CHECK(StateOf(ledger, "delivery-job-000001-v1") == "prepared");
        for (const auto& error : v3::ValidateAsyncToolSequence(ledger)) {
            FAIL_CHECK(error.code << ": " << error.message);
        }
    }
    h.planner->NoteResponseOutcome("request-000002", true);
    v3::V3Ledger ledger = h.Read();
    CHECK(StateOf(ledger, "delivery-job-000001-v1") == "acknowledged");
    for (const auto& error : v3::ValidateAsyncToolSequence(ledger)) {
        FAIL_CHECK(error.code << ": " << error.message);
    }
}

TEST_CASE("投递账:回执丢失落 uncertain;失败路同款") {
    PlannerHarness h("uncertain");
    runtime::ResultDeliveryPlannerImpl::Hooks hooks;
    hooks.writer = &*h.writer;
    hooks.writer_mutex = h.writer_mutex;
    hooks.current_turn_id = [] { return std::string("turn-000001"); };
    hooks.response_evidence = [](const std::string&) -> std::optional<std::string> {
        return std::nullopt;  // 没证据不宣称接纳
    };
    runtime::ResultDeliveryPlannerImpl planner_no_evidence(std::move(hooks));
    planner_no_evidence.NotifyCompletion(MakeNotice("job-000001", 1));
    REQUIRE(planner_no_evidence.SelectForRequestBoundary().size() == 1);
    planner_no_evidence.NoteRequestPrepared("request-000002");
    planner_no_evidence.NoteResponseOutcome("request-000002", true);
    v3::V3Ledger ledger = h.Read();
    CHECK(StateOf(ledger, "delivery-job-000001-v1") == "uncertain");

    planner_no_evidence.NotifyCompletion(MakeNotice("job-000002", 2, "call_b"));
    REQUIRE(planner_no_evidence.SelectForRequestBoundary().size() == 1);
    planner_no_evidence.NoteRequestPrepared("request-000003");
    planner_no_evidence.NoteResponseOutcome("request-000003", false);
    v3::V3Ledger ledger2 = h.Read();
    CHECK(StateOf(ledger2, "delivery-job-000002-v1") == "uncertain");
    for (const auto& error : v3::ValidateAsyncToolSequence(ledger2)) {
        FAIL_CHECK(error.code << ": " << error.message);
    }
}

// ---------------------------------------------------------------------------
// 恢复:原文已落仓、tool 消息未提交 → 补投递不重跑(单 §6 表)
// ---------------------------------------------------------------------------
TEST_CASE("恢复:终态已落未配的 native 欠账,重建 mailbox 补投递不重跑") {
    PlannerHarness h("restore");
    // 手造账:assistant 声明 → 调用证据 pending → registered(native)→
    // dispatched → started → finished → persisted → observed(终态)。
    // 没有 selected/tool 消息——"原文已落仓、消息未提交"的崩溃边界。
    v3::MessageDraft assistant;
    assistant.turn_id = "turn-000001";
    assistant.step_id = "step-000001";
    assistant.request_id = "request-000001";
    assistant.origin = v3::MessageOrigin::SessionRuntime;
    assistant.provider = "openai";
    assistant.wire = "responses";
    assistant.model = "gpt-test";
    assistant.response_model = nlohmann::json("gpt-test");
    assistant.usage = nlohmann::json::object({{"inputTokens", 10}, {"outputTokens", 5}});
    assistant.message = nlohmann::json::object({
        {"role", "assistant"},
        {"content", "先查"},
        {"tool_calls", nlohmann::json::array({nlohmann::json::object({
            {"id", "call_a"},
            {"type", "function"},
            {"function", nlohmann::json::object({{"name", "native_search"},
                                                 {"arguments", "{\"query\":\"资料\"}"}})},
        })})},
    });
    auto assistant_receipt = h.writer->AppendMessage(assistant, v3::Durability::PowerLoss);
    REQUIRE(assistant_receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(h.writer->AdmitMessages({assistant_receipt.id}).status ==
            v3::WriteReceipt::Status::Committed);

    const char* action = "action-job-000001";
    auto emit = [&](v3::EventKindV3 kind, std::optional<v3::OpStatus> status, nlohmann::json payload) {
        v3::EventDraft draft;
        draft.kind = kind;
        draft.status = status;
        draft.turn_id = "turn-000001";
        draft.step_id = "step-000001";
        draft.action_id = action;
        draft.payload = std::move(payload);
        auto receipt = h.writer->AppendEvent(std::move(draft), v3::Durability::PowerLoss);
        REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
        return receipt;
    };
    emit(v3::EventKindV3::ToolExecutionPending, v3::OpStatus::Pending,
         nlohmann::json{{"tool_call_id", action},
                        {"attempt", 1},
                        {"reason", "queued"},
                        {"assistantMessageRef", assistant_receipt.id},
                        {"provider_tool_call_id", "call_a"}});
    emit(v3::EventKindV3::ToolJobRegistered, std::nullopt,
         nlohmann::json{{"tool_call_id", action},
                        {"attempt", 1},
                        {"jobId", "job-000001"},
                        {"mode", "native_deferred"},
                        {"assistantMessageRef", assistant_receipt.id},
                        {"wireCallRef", nlohmann::json::object({{"provider", "openai"},
                                                                {"wire", "responses"},
                                                                {"callId", "call_a"},
                                                                {"async", true}})}});
    emit(v3::EventKindV3::ToolJobDispatched, std::nullopt,
         nlohmann::json{{"tool_call_id", action},
                        {"jobId", "job-000001"},
                        {"ownerEpoch", "epoch-1"}});
    emit(v3::EventKindV3::ToolExecutionStarted, v3::OpStatus::Running,
         nlohmann::json{{"tool_call_id", action}, {"attempt", 2}, {"effectiveArgsRef", "args-1"}});
    auto finished =
        emit(v3::EventKindV3::ToolExecutionFinished, v3::OpStatus::Done,
             nlohmann::json{{"tool_call_id", action}, {"attempt", 2}, {"exit_code", 0}});
    auto persisted =
        emit(v3::EventKindV3::ToolResultPersisted, std::nullopt,
             nlohmann::json{{"tool_call_id", action},
                            {"attempt", 2},
                            {"result_ref", nlohmann::json::array({nlohmann::json::object({
                                {"artifactId", "res-000001"},
                                {"kind", "result_metadata"},
                                {"path", "artifacts/res-000001.json"},
                                {"sha256", std::string(64, 'a')},
                                {"bytes", 64},
                                {"mediaType", "application/json"},
                            })})},
                            {"executionEventRef", finished.id}});
    emit(v3::EventKindV3::ToolJobObserved, std::nullopt,
         nlohmann::json{{"tool_call_id", action},
                        {"jobId", "job-000001"},
                        {"observedStatus", "succeeded"},
                        {"resultRef", persisted.id},
                        {"resultVersion", 1}});

    // 重建前:义务未配。
    {
        v3::V3Ledger ledger = h.Read();
        for (const auto& error : v3::ValidateAsyncToolSequence(ledger)) {
            FAIL_CHECK(error.code << ": " << error.message);
        }
        auto obligations = v3::ProjectProtocolObligations(ledger);
        const auto* obligation = v3::FindProtocolObligation(obligations, action);
        REQUIRE(obligation != nullptr);
        CHECK_FALSE(obligation->paired);
    }

    // 恢复:账态注入 → mailbox 重建 → 下次请求边界补投递(不重跑)。
    CHECK(h.planner->RestoreFromLedger(h.Read()) == 1);
    auto selected = h.planner->SelectForRequestBoundary();
    REQUIRE(selected.size() == 1);
    const auto* block = std::get_if<api::ToolResultBlock>(&selected[0].content[0]);
    REQUIRE(block != nullptr);
    CHECK(block->tool_use_id == "call_a");

    v3::V3Ledger ledger = h.Read();
    for (const auto& error : v3::ValidateAsyncToolSequence(ledger)) {
        FAIL_CHECK(error.code << ": " << error.message);
    }
    auto obligations = v3::ProjectProtocolObligations(ledger);
    const auto* obligation = v3::FindProtocolObligation(obligations, action);
    REQUIRE(obligation != nullptr);
    CHECK(obligation->paired);  // 补投递配齐,原文仓未动(不重跑)
    // 恢复重建是幂等的:再来一遍不重复入 mailbox。
    CHECK(h.planner->RestoreFromLedger(h.Read()) == 0);
}
