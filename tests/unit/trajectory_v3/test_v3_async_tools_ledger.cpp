// 异步工具单 P0 账册测试(跨行层):用 v3 writer 生成合法账——job 生命
// 周期、跨 turn 欠账、投递 uncertain、能力快照——三类只读投影
//(FoldJobExecutions/ProjectProtocolObligations/FoldDeliveries)与能力闸
// 门(DecideAsyncModes)对正例断言;ValidateAsyncToolSequence 对非法前驱、
// 重复终态、乱序信封、未知身份引用的反例账全部拒。
// 合同与 fixture 已验,生产未接(单 §11 P0)。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/schema3.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;

namespace {

// ctest 注册循环默认注入 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0;本册写
// v3 账,显式开回 1(同 test_v3_goal_applied.cpp 口径)。
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

// 六键 artifactRef(§3.1)。
nlohmann::json ArtifactRef(const char* id) {
    return nlohmann::json::object({
        {"artifactId", id},
        {"kind", "result_metadata"},
        {"path", std::string("artifacts/") + id + ".json"},
        {"sha256", std::string(64, 'a')},
        {"bytes", 128},
        {"mediaType", "application/json"},
    });
}

struct LedgerHarness {
    std::filesystem::path dir;
    std::filesystem::path jsonl;
    std::optional<V3Writer> writer;

    explicit LedgerHarness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-v3-async-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        jsonl = dir / "s1.jsonl";
        auto started = V3Writer::Start(jsonl, "20260912-120000-AAAAAA", "run-000001",
                                       "system prompt");
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }

    void Close() { writer.reset(); }
    void Reopen() {
        auto cont = V3Writer::Continue(jsonl);
        REQUIRE_MESSAGE(cont.has_value(), cont.error_or(""));
        writer = std::move(*cont);
    }
    V3Ledger Read() {
        auto ledger = ReadV3Ledger(jsonl);
        REQUIRE_MESSAGE(ledger.has_value(), ledger.error_or(""));
        return *ledger;
    }

    WriteReceipt Event(EventDraft draft) {
        auto receipt = writer->AppendEvent(std::move(draft), Durability::ProcessCrash);
        REQUIRE_MESSAGE(receipt.status == WriteReceipt::Status::Committed, receipt.error_message);
        return receipt;
    }

    // 声明一枚工具调用的 assistant 消息(带 tool_calls)并接纳。
    WriteReceipt AppendAssistantWithCall(const char* call_id, const char* request_id) {
        MessageDraft draft;
        draft.turn_id = "turn-000001";
        draft.step_id = "step-000001";
        draft.request_id = request_id;
        draft.origin = MessageOrigin::SessionRuntime;
        draft.provider = "openai";
        draft.wire = "responses";
        draft.model = "gpt-6";
        draft.response_model = nlohmann::json("gpt-6");
        draft.usage = nlohmann::json::object({{"inputTokens", 10}, {"outputTokens", 5}});
        draft.message = nlohmann::json::object({
            {"role", "assistant"},
            {"content", "先查"},
            {"tool_calls", nlohmann::json::array({nlohmann::json::object({
                {"id", call_id},
                {"type", "function"},
                {"function", nlohmann::json::object({
                    {"name", "search"},
                    {"arguments", "{\"query\":\"资料\"}"},
                })},
            })})},
        });
        auto receipt = writer->AppendMessage(draft, Durability::PowerLoss);
        REQUIRE_MESSAGE(receipt.status == WriteReceipt::Status::Committed, receipt.error_message);
        auto admitted = writer->AdmitMessages({receipt.id});
        REQUIRE_MESSAGE(admitted.status == WriteReceipt::Status::Committed, admitted.error_message);
        return receipt;
    }

    // 调用证据(pending+started),返回 pending 事件收据。
    WriteReceipt AppendCallEvidence(const char* action_id, const char* assistant_msg_id,
                                    const char* call_id) {
        EventDraft pending;
        pending.kind = EventKindV3::ToolExecutionPending;
        pending.status = OpStatus::Pending;
        pending.turn_id = "turn-000001";
        pending.step_id = "step-000001";
        pending.action_id = action_id;
        pending.payload = nlohmann::json::object({
            {"tool_call_id", action_id},
            {"attempt", 1},
            {"reason", "queued"},
            {"assistantMessageRef", assistant_msg_id},
            {"provider_tool_call_id", call_id},
        });
        auto pending_receipt = Event(std::move(pending));
        EventDraft started;
        started.kind = EventKindV3::ToolExecutionStarted;
        started.status = OpStatus::Running;
        started.turn_id = "turn-000001";
        started.step_id = "step-000001";
        started.action_id = action_id;
        started.payload = nlohmann::json::object({
            {"tool_call_id", action_id},
            {"attempt", 1},
            {"effectiveArgsRef", "args-000001"},
        });
        Event(std::move(started));
        return pending_receipt;
    }

    WriteReceipt AppendJobRegistered(const char* action_id, const char* job_id, const char* mode,
                                     const char* assistant_msg_id, const char* call_id,
                                     bool approval_required = false) {
        EventDraft draft;
        draft.kind = EventKindV3::ToolJobRegistered;
        draft.turn_id = "turn-000001";
        draft.step_id = "step-000001";
        draft.action_id = action_id;
        nlohmann::json payload = nlohmann::json::object({
            {"tool_call_id", action_id},
            {"attempt", 1},
            {"jobId", job_id},
            {"mode", mode},
            {"assistantMessageRef", assistant_msg_id},
        });
        if (std::string_view(mode) == "native_deferred") {
            payload["wireCallRef"] = nlohmann::json::object({
                {"provider", "openai"},
                {"wire", "responses"},
                {"callId", call_id},
                {"async", true},
            });
        }
        if (approval_required) {
            payload["approvalRequired"] = true;
        }
        draft.payload = std::move(payload);
        return Event(std::move(draft));
    }

    void AppendJobDispatched(const char* action_id, const char* job_id,
                             const char* owner_epoch = "epoch-1") {
        EventDraft draft;
        draft.kind = EventKindV3::ToolJobDispatched;
        draft.turn_id = "turn-000001";
        draft.step_id = "step-000001";
        draft.action_id = action_id;
        draft.payload = nlohmann::json::object({
            {"tool_call_id", action_id},
            {"attempt", 1},
            {"jobId", job_id},
            {"ownerEpoch", owner_epoch},
        });
        Event(std::move(draft));
    }

    WriteReceipt AppendJobObserved(const char* action_id, const char* job_id,
                                   const char* observed_status) {
        EventDraft draft;
        draft.kind = EventKindV3::ToolJobObserved;
        draft.turn_id = "turn-000001";
        draft.step_id = "step-000001";
        draft.action_id = action_id;
        draft.payload = nlohmann::json::object({
            {"tool_call_id", action_id},
            {"jobId", job_id},
            {"observedStatus", observed_status},
        });
        return Event(std::move(draft));
    }

    // 执行终态 + 结果持久化 + 选用(复用已有 tool.execution/tool.result)。
    WriteReceipt AppendResultChain(const char* action_id, const char* tool_message_content) {
        EventDraft finished;
        finished.kind = EventKindV3::ToolExecutionFinished;
        finished.status = OpStatus::Done;
        finished.turn_id = "turn-000001";
        finished.step_id = "step-000001";
        finished.action_id = action_id;
        finished.payload = nlohmann::json::object({
            {"tool_call_id", action_id},
            {"attempt", 1},
            {"exit_code", 0},
        });
        auto finished_receipt = Event(std::move(finished));
        EventDraft persisted;
        persisted.kind = EventKindV3::ToolResultPersisted;
        persisted.turn_id = "turn-000001";
        persisted.step_id = "step-000001";
        persisted.action_id = action_id;
        persisted.payload = nlohmann::json::object({
            {"tool_call_id", action_id},
            {"attempt", 1},
            {"result_ref", nlohmann::json::array({ArtifactRef("res-000001")})},
            {"executionEventRef", finished_receipt.id},
        });
        auto persisted_receipt = Event(std::move(persisted));
        EventDraft selected;
        selected.kind = EventKindV3::ToolResultSelected;
        selected.turn_id = "turn-000001";
        selected.step_id = "step-000001";
        selected.action_id = action_id;
        selected.payload = nlohmann::json::object({
            {"tool_call_id", action_id},
            {"sourceResultEventRefs", nlohmann::json::array({persisted_receipt.id})},
            {"hookEffectEventRefs", nlohmann::json::array()},
            {"effectiveOutcome", "done"},
        });
        auto selected_receipt = Event(std::move(selected));
        // 正式 tool 消息(回喂模型),带选用回执。
        MessageDraft tool_message;
        tool_message.turn_id = "turn-000001";
        tool_message.step_id = "step-000001";
        tool_message.action_id = action_id;
        tool_message.origin = MessageOrigin::SessionRuntime;
        tool_message.result_selection_ref = selected_receipt.id;
        tool_message.message = nlohmann::json::object({
            {"role", "tool"},
            {"tool_call_id", action_id},
            {"content", tool_message_content},
        });
        auto message_receipt = writer->AppendMessage(tool_message, Durability::PowerLoss);
        REQUIRE_MESSAGE(message_receipt.status == WriteReceipt::Status::Committed,
                        message_receipt.error_message);
        auto admitted = writer->AdmitMessages({message_receipt.id});
        REQUIRE_MESSAGE(admitted.status == WriteReceipt::Status::Committed, admitted.error_message);
        return persisted_receipt;
    }

    // 投递各步(单 §6:pending → prepared → sent → acknowledged;uncertain
    // 是回执丢失)。按需落 prepared/sent/uncertain/acknowledged;返回
    // acknowledged 用的证据事件 id。
    std::string AppendDeliveryLifecycle(const char* action_id, const char* delivery_id,
                                        const char* request_id, const std::string& result_event_id,
                                        bool with_prepared, bool with_sent, bool with_acknowledged,
                                        const char* uncertain_reason = nullptr) {
        if (with_prepared) {
            EventDraft draft;
            draft.kind = EventKindV3::ToolDeliveryPrepared;
            draft.turn_id = "turn-000001";
            draft.step_id = "step-000001";
            draft.action_id = action_id;
            draft.request_id = request_id;
            draft.payload = nlohmann::json::object({
                {"tool_call_id", action_id},
                {"deliveryId", delivery_id},
                {"resultRef", result_event_id},
                {"resultVersion", 1},
            });
            Event(std::move(draft));
        }
        if (with_sent) {
            EventDraft sent;
            sent.kind = EventKindV3::ModelRequestSent;
            sent.status = OpStatus::Done;
            sent.request_id = request_id;
            sent.turn_id = "turn-000001";
            sent.step_id = "step-000001";
            sent.payload = nlohmann::json::object({{"deliveryScope", "local_transport"}});
            Event(std::move(sent));
        }
        if (uncertain_reason != nullptr) {
            EventDraft uncertain;
            uncertain.kind = EventKindV3::ToolDeliveryUncertain;
            uncertain.turn_id = "turn-000001";
            uncertain.step_id = "step-000001";
            uncertain.action_id = action_id;
            uncertain.request_id = request_id;
            uncertain.payload = nlohmann::json::object({
                {"tool_call_id", action_id},
                {"deliveryId", delivery_id},
                {"reason", uncertain_reason},
            });
            Event(std::move(uncertain));
        }
        std::string evidence_id;
        if (with_acknowledged) {
            EventDraft completed;
            completed.kind = EventKindV3::ModelResponseCompleted;
            completed.status = OpStatus::Done;
            completed.request_id = request_id;
            completed.turn_id = "turn-000001";
            completed.step_id = "step-000001";
            completed.payload = nlohmann::json::object({{"requestId", request_id}});
            evidence_id = Event(std::move(completed)).id;
            EventDraft ack;
            ack.kind = EventKindV3::ToolDeliveryAcknowledged;
            ack.turn_id = "turn-000001";
            ack.step_id = "step-000001";
            ack.action_id = action_id;
            ack.request_id = request_id;
            ack.payload = nlohmann::json::object({
                {"tool_call_id", action_id},
                {"deliveryId", delivery_id},
                {"evidenceRef", evidence_id},
            });
            Event(std::move(ack));
        }
        return evidence_id;
    }

    // 能力快照事件。
    WriteReceipt AppendCapability(const char* native_status, const char* job_handle_status) {
        EventDraft draft;
        draft.kind = EventKindV3::ToolCapabilityRecorded;
        nlohmann::json verdicts = nlohmann::json::object();
        if (native_status != nullptr) {
            verdicts["native_deferred"] =
                nlohmann::json::object({{"status", native_status}, {"evidence", "fixture"}});
        }
        if (job_handle_status != nullptr) {
            verdicts["job_handle"] =
                nlohmann::json::object({{"status", job_handle_status}});
        }
        draft.payload = nlohmann::json::object({
            {"basis", nlohmann::json::object({
                {"provider", "openai"},
                {"wire", "responses"},
                {"model", "gpt-6"},
                {"endpoint", "https://api.example.com/v1"},
            })},
            {"verdicts", std::move(verdicts)},
        });
        return Event(std::move(draft));
    }
};

bool HasAsyncCode(const std::vector<Schema3Error>& errors, const char* code) {
    for (const auto& error : errors) {
        if (error.code == code) {
            return true;
        }
    }
    return false;
}

// 拷贝返回:投影 vector 是局部量,不能外泄指针。
std::optional<ProtocolObligationView> ObligationFor(const V3Ledger& ledger,
                                                    const char* action_id) {
    auto obligations = ProjectProtocolObligations(ledger);
    const auto* found = FindProtocolObligation(obligations, action_id);
    if (found == nullptr) {
        return std::nullopt;
    }
    return *found;
}

}  // namespace

// ---------------------------------------------------------------------------
// native_deferred 全链:job 生命周期 + 跨 turn 欠账 + 投递 acknowledged
// ---------------------------------------------------------------------------

TEST_CASE("native_deferred:原调用跨 turn 欠账,结果只配原 call,投递到账") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    LedgerHarness h("native");
    V3Writer& w = *h.writer;
    auto assistant = h.AppendAssistantWithCall("call_A1", "request-000001");
    h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_A1");
    h.AppendJobRegistered("action-000001", "job-000001", "native_deferred", assistant.id.c_str(),
                          "call_A1");
    h.AppendJobDispatched("action-000001", "job-000001");
    // turn 1 收口,原调用仍欠模型一份结果;turn 2 新请求不带 A 的结果。
    auto prepared = w.PrepareRequest("request-000002", "turn-000002", "step-000002", "turn",
                                     w.context().system_message_ref, {assistant.id},
                                     nlohmann::json::object({{"provider", "openai"}}));
    REQUIRE_MESSAGE(prepared.status == WriteReceipt::Status::Committed, prepared.error_message);

    // 中段读取:欠账在案(单 §1"哪枚调用还欠模型一份结果")。
    h.Close();
    {
        V3Ledger mid = h.Read();
        const auto obligation = ObligationFor(mid, "action-000001");
        REQUIRE(obligation.has_value());
        CHECK(obligation->mode == "native_deferred");
        CHECK(obligation->job_id == "job-000001");
        CHECK(obligation->async_call);
        CHECK(obligation->provider_call_id == "call_A1");
        CHECK_FALSE(obligation->paired);  // 跨 turn 欠账
        CHECK(obligation->pairing_message_id.empty());
        auto jobs = FoldJobExecutions(mid);
        const auto* job = FindJobExecution(jobs, "job-000001");
        REQUIRE(job != nullptr);
        CHECK(job->state == "running");  // 已派发、无终态
        CHECK(job->dispatched);
        CHECK(job->owner_epoch == "epoch-1");
        CHECK(job->mode == "native_deferred");
    }
    h.Reopen();

    // 后半:job succeeded → 结果链复用 tool.execution/tool.result → 正式
    // tool 消息 → 投递 prepared/sent/acknowledged。
    h.AppendJobObserved("action-000002", "job-000001", "running");
    h.AppendJobObserved("action-000002", "job-000001", "succeeded");
    auto persisted = h.AppendResultChain("action-000001", "资料结果预览");
    h.AppendDeliveryLifecycle("action-000001", "delivery-000001", "request-000003", persisted.id,
                              /*with_prepared=*/true, /*with_sent=*/true,
                              /*with_acknowledged=*/true);

    h.Close();
    V3Ledger ledger = h.Read();
    // 执行投影:终态。
    auto jobs = FoldJobExecutions(ledger);
    const auto* job = FindJobExecution(jobs, "job-000001");
    REQUIRE(job != nullptr);
    CHECK(job->state == "succeeded");
    // 欠账清偿:tool 消息落账且在当前链上;协议欠账归零。
    const auto obligation = ObligationFor(ledger, "action-000001");
    REQUIRE(obligation.has_value());
    CHECK(obligation->paired);
    CHECK(obligation->on_current_context);
    CHECK_FALSE(obligation->pairing_message_id.empty());
    // 投递投影:acknowledged,证据在账。
    auto deliveries = FoldDeliveries(ledger);
    const auto* delivery = FindDelivery(deliveries, "delivery-000001");
    REQUIRE(delivery != nullptr);
    CHECK(delivery->state == "acknowledged");
    CHECK(delivery->target_request_id == "request-000003");
    CHECK(delivery->result_version == 1);
    REQUIRE(delivery->evidence_ref.has_value());
    CHECK(ledger.FindEvent(*delivery->evidence_ref) != nullptr);
    CHECK(delivery->result_ref_id == persisted.id);
    // 整账跨行合同:零错。
    CHECK(ValidateAsyncToolSequence(ledger).empty());
}

// ---------------------------------------------------------------------------
// job_handle:start 接单即配齐;wait/observe 是新调用
// ---------------------------------------------------------------------------

TEST_CASE("job_handle:start 的接单结果即配齐调用,job 观测走新 Action") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    LedgerHarness h("handle");
    auto assistant = h.AppendAssistantWithCall("call_B1", "request-000001");
    h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_B1");
    h.AppendJobRegistered("action-000001", "job-000002", "job_handle", assistant.id.c_str(),
                          "call_B1");
    h.AppendJobDispatched("action-000001", "job-000002");
    h.AppendJobObserved("action-000002", "job-000002", "queued");
    // start 动作收口:接单结果 {jobId, status:queued} 就是给模型的答复。
    h.AppendResultChain("action-000001", "{\"jobId\":\"job-000002\",\"status\":\"queued\"}");

    V3Ledger ledger = h.Read();
    const auto obligation = ObligationFor(ledger, "action-000001");
    REQUIRE(obligation.has_value());
    CHECK(obligation->mode == "job_handle");
    CHECK(obligation->paired);  // start 回接单结果即配齐(单 §4)
    CHECK(obligation->on_current_context);
    auto jobs = FoldJobExecutions(ledger);
    const auto* job = FindJobExecution(jobs, "job-000002");
    REQUIRE(job != nullptr);
    // observed(queued) 在 dispatched(running) 之后:陈旧观测不回退状态。
    CHECK(job->state == "running");
    CHECK(job->origin_action_id == "action-000001");
    CHECK(ValidateAsyncToolSequence(ledger).empty());
}

// ---------------------------------------------------------------------------
// 执行投影:awaiting_approval / unknown / 取消竞态
// ---------------------------------------------------------------------------

TEST_CASE("job 执行投影:审批未过不派发;running 查不明为 unknown") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SUBCASE("审批未过停 awaiting_approval,不派发") {
        LedgerHarness h("approval");
        auto assistant = h.AppendAssistantWithCall("call_A1", "request-000001");
        h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_A1");
        h.AppendJobRegistered("action-000001", "job-000001", "job_handle", assistant.id.c_str(),
                              "call_A1", /*approval_required=*/true);
        auto jobs = FoldJobExecutions(h.Read());
        const auto* job = FindJobExecution(jobs, "job-000001");
        REQUIRE(job != nullptr);
        CHECK(job->state == "awaiting_approval");
        CHECK_FALSE(job->dispatched);
        CHECK(job->approval_required);
    }
    SUBCASE("dispatched 后观测查不明:unknown,不合成假终态") {
        LedgerHarness h("unknown");
        auto assistant = h.AppendAssistantWithCall("call_A1", "request-000001");
        h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_A1");
        h.AppendJobRegistered("action-000001", "job-000001", "job_handle", assistant.id.c_str(),
                              "call_A1");
        h.AppendJobDispatched("action-000001", "job-000001");
        h.AppendJobObserved("action-000002", "job-000001", "unknown");
        auto jobs = FoldJobExecutions(h.Read());
        const auto* job = FindJobExecution(jobs, "job-000001");
        REQUIRE(job != nullptr);
        CHECK(job->state == "unknown");  // 单 §6:running 查不明则 unknown
    }
    SUBCASE("取消请求在账不算终态;唯一终态是 cancelled") {
        LedgerHarness h("cancel");
        auto assistant = h.AppendAssistantWithCall("call_A1", "request-000001");
        h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_A1");
        h.AppendJobRegistered("action-000001", "job-000001", "job_handle", assistant.id.c_str(),
                              "call_A1");
        h.AppendJobDispatched("action-000001", "job-000001");
        EventDraft cancel;
        cancel.kind = EventKindV3::ToolJobCancelRequested;
        cancel.turn_id = "turn-000001";
        cancel.step_id = "step-000001";
        cancel.action_id = "action-000009";
        cancel.payload = nlohmann::json::object({
            {"tool_call_id", "action-000009"},
            {"jobId", "job-000001"},
            {"reason", "user_escape"},
        });
        h.Event(std::move(cancel));
        h.AppendJobObserved("action-000010", "job-000001", "cancelled");
        auto jobs = FoldJobExecutions(h.Read());
        const auto* job = FindJobExecution(jobs, "job-000001");
        REQUIRE(job != nullptr);
        CHECK(job->cancel_requested);  // 取消请求≠成功取消(单 §8)
        CHECK(job->state == "cancelled");
        CHECK(ValidateAsyncToolSequence(h.Read()).empty());
    }
}

// ---------------------------------------------------------------------------
// 投递投影:prepared → sent → uncertain →(迟到证据)acknowledged
// ---------------------------------------------------------------------------

TEST_CASE("投递投影:uncertain 标回执丢失,迟到证据可解除不降级") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    LedgerHarness h("delivery");
    auto assistant = h.AppendAssistantWithCall("call_A1", "request-000001");
    h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_A1");
    auto persisted = h.AppendResultChain("action-000001", "结果预览");

    auto state_of = [&](const char* delivery_id) {
        auto deliveries = FoldDeliveries(h.Read());
        const auto* delivery = FindDelivery(deliveries, delivery_id);
        REQUIRE(delivery != nullptr);
        return delivery->state;
    };

    h.AppendDeliveryLifecycle("action-000001", "delivery-000001", "request-000002", persisted.id,
                              /*with_prepared=*/true, /*with_sent=*/false,
                              /*with_acknowledged=*/false);
    CHECK(state_of("delivery-000001") == "prepared");
    // 只补 sent(同 requestId 的 model.request.sent):prepared → sent。
    {
        EventDraft sent;
        sent.kind = EventKindV3::ModelRequestSent;
        sent.status = OpStatus::Done;
        sent.request_id = "request-000002";
        sent.turn_id = "turn-000001";
        sent.step_id = "step-000001";
        sent.payload = nlohmann::json::object({{"deliveryScope", "local_transport"}});
        h.Event(std::move(sent));
    }
    CHECK(state_of("delivery-000001") == "sent");
    // 回执丢失:uncertain,带原因(单 §6 验收矩阵"回包丢失")。
    h.AppendDeliveryLifecycle("action-000001", "delivery-000001", "request-000002", persisted.id,
                              /*with_prepared=*/false, /*with_sent=*/false,
                              /*with_acknowledged=*/false,
                              /*uncertain_reason=*/"response_receipt_lost");
    {
        auto deliveries = FoldDeliveries(h.Read());
        const auto* delivery = FindDelivery(deliveries, "delivery-000001");
        REQUIRE(delivery != nullptr);
        CHECK(delivery->state == "uncertain");
        CHECK(delivery->uncertain_reason == "response_receipt_lost");
    }
    // 迟到证据:acknowledged 解除 uncertain,不降级。
    h.AppendDeliveryLifecycle("action-000001", "delivery-000001", "request-000002", persisted.id,
                              /*with_prepared=*/false, /*with_sent=*/false,
                              /*with_acknowledged=*/true);
    CHECK(state_of("delivery-000001") == "acknowledged");
    CHECK(ValidateAsyncToolSequence(h.Read()).empty());
}

// ---------------------------------------------------------------------------
// 能力快照与模式闸门(§4 三态;不接真探针)
// ---------------------------------------------------------------------------

TEST_CASE("能力快照:unknown fail-closed,verified 放行,unsupported 禁用") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    LedgerHarness h("capability");
    h.AppendCapability("unknown", "verified");      // 首测:未验证
    h.AppendCapability("verified", "unsupported");  // 复测:验证过但宿主禁 job

    auto snapshots = FoldCapabilitySnapshots(h.Read());
    REQUIRE(snapshots.size() == 2);
    CHECK(snapshots[0].provider == "openai");
    CHECK(snapshots[0].wire == "responses");
    CHECK(snapshots[0].model == "gpt-6");
    const auto* verdict = FindCapabilityVerdict(snapshots[0], "native_deferred");
    REQUIRE(verdict != nullptr);
    CHECK(verdict->status == "unknown");

    // unknown 默认不用 native_deferred(单 §4);job_handle 是宿主侧行为。
    AsyncModeDecision first = DecideAsyncModes(snapshots[0]);
    CHECK_FALSE(first.native_deferred_allowed);
    CHECK_FALSE(first.native_deferred_reason.empty());
    CHECK(first.job_handle_allowed);

    AsyncModeDecision second = DecideAsyncModes(snapshots[1]);
    CHECK(second.native_deferred_allowed);
    CHECK(second.job_handle_reason.empty());
    CHECK_FALSE(second.job_handle_allowed);  // 明示 unsupported 才禁

    // 缺项按 unknown 处理(fail-closed),不静默放行。
    ToolCapabilitySnapshotView partial;
    partial.provider = "openai";
    partial.wire = "responses";
    partial.model = "gpt-6";
    partial.verdicts.push_back(ToolCapabilityVerdict{"job_handle", "verified", ""});
    AsyncModeDecision missing = DecideAsyncModes(partial);
    CHECK_FALSE(missing.native_deferred_allowed);
    CHECK_FALSE(missing.native_deferred_reason.empty());
}

// ---------------------------------------------------------------------------
// 跨行合同校验器:反例账全拒
// ---------------------------------------------------------------------------

TEST_CASE("ValidateAsyncToolSequence:非法前驱/重复终态/乱序/未知身份引用全拒") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SUBCASE("派发前无注册(非法前驱)") {
        LedgerHarness h("v-unknown-job");
        h.AppendJobDispatched("action-000001", "job-000001");
        CHECK(HasAsyncCode(ValidateAsyncToolSequence(h.Read()), "async.unknown_job"));
    }
    SUBCASE("注册先于调用证据(幽灵 job)") {
        LedgerHarness h("v-no-evidence");
        h.AppendJobRegistered("action-000001", "job-000001", "job_handle", "msg-000002",
                              "call_A1");
        CHECK(HasAsyncCode(ValidateAsyncToolSequence(h.Read()), "async.job_without_call_evidence"));
    }
    SUBCASE("同一 jobId 二次注册") {
        LedgerHarness h("v-dup-reg");
        auto assistant = h.AppendAssistantWithCall("call_A1", "request-000001");
        h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_A1");
        h.AppendJobRegistered("action-000001", "job-000001", "job_handle", assistant.id.c_str(),
                              "call_A1");
        h.AppendJobRegistered("action-000001", "job-000001", "job_handle", assistant.id.c_str(),
                              "call_A1");
        CHECK(HasAsyncCode(ValidateAsyncToolSequence(h.Read()), "async.duplicate_job_registration"));
    }
    SUBCASE("完成与取消竞态:第二枚终态观测拒(合法终态只接纳一次)") {
        LedgerHarness h("v-dup-terminal");
        auto assistant = h.AppendAssistantWithCall("call_A1", "request-000001");
        h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_A1");
        h.AppendJobRegistered("action-000001", "job-000001", "job_handle", assistant.id.c_str(),
                              "call_A1");
        h.AppendJobDispatched("action-000001", "job-000001");
        h.AppendJobObserved("action-000002", "job-000001", "succeeded");
        h.AppendJobObserved("action-000003", "job-000001", "cancelled");
        CHECK(HasAsyncCode(ValidateAsyncToolSequence(h.Read()),
                           "async.duplicate_terminal_observation"));
    }
    SUBCASE("acknowledged 先于 prepared(乱序信封)") {
        LedgerHarness h("v-ack-first");
        auto assistant = h.AppendAssistantWithCall("call_A1", "request-000001");
        auto evidence = h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_A1");
        EventDraft ack;
        ack.kind = EventKindV3::ToolDeliveryAcknowledged;
        ack.turn_id = "turn-000001";
        ack.step_id = "step-000001";
        ack.action_id = "action-000001";
        ack.request_id = "request-000002";
        ack.payload = nlohmann::json::object({
            {"tool_call_id", "action-000001"},
            {"deliveryId", "delivery-000001"},
            {"evidenceRef", evidence.id},
        });
        h.Event(std::move(ack));
        CHECK(HasAsyncCode(ValidateAsyncToolSequence(h.Read()), "async.delivery_without_prepared"));
    }
    SUBCASE("prepared 引用无工具域账的 Action(未知身份引用)") {
        LedgerHarness h("v-ghost-action");
        EventDraft prepared;
        prepared.kind = EventKindV3::ToolDeliveryPrepared;
        prepared.turn_id = "turn-000001";
        prepared.step_id = "step-000001";
        prepared.action_id = "action-000099";
        prepared.request_id = "request-000002";
        prepared.payload = nlohmann::json::object({
            {"tool_call_id", "action-000099"},
            {"deliveryId", "delivery-000001"},
            {"resultRef", "evt-999999"},
            {"resultVersion", 1},
        });
        h.Event(std::move(prepared));
        CHECK(HasAsyncCode(ValidateAsyncToolSequence(h.Read()), "async.unknown_action"));
    }
    SUBCASE("prepared.resultRef 指不到 tool.result.persisted(未知身份引用)") {
        LedgerHarness h("v-ghost-result");
        auto assistant = h.AppendAssistantWithCall("call_A1", "request-000001");
        h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_A1");
        h.AppendDeliveryLifecycle("action-000001", "delivery-000001", "request-000002",
                                  "evt-999999",
                                  /*with_prepared=*/true, /*with_sent=*/false,
                                  /*with_acknowledged=*/false);
        CHECK(HasAsyncCode(ValidateAsyncToolSequence(h.Read()), "async.unknown_result"));
    }
    SUBCASE("同 (deliveryId,requestId) 二次 prepared(重试须换 targetRequestId)") {
        LedgerHarness h("v-dup-prepare");
        auto assistant = h.AppendAssistantWithCall("call_A1", "request-000001");
        h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_A1");
        auto persisted = h.AppendResultChain("action-000001", "结果预览");
        h.AppendDeliveryLifecycle("action-000001", "delivery-000001", "request-000002",
                                  persisted.id, /*with_prepared=*/true, /*with_sent=*/false,
                                  /*with_acknowledged=*/false);
        h.AppendDeliveryLifecycle("action-000001", "delivery-000001", "request-000002",
                                  persisted.id, /*with_prepared=*/true, /*with_sent=*/false,
                                  /*with_acknowledged=*/false);
        CHECK(HasAsyncCode(ValidateAsyncToolSequence(h.Read()), "async.duplicate_delivery_attempt"));
    }
    SUBCASE("acknowledged 证据指不到账上事件(没证据不宣称接纳)") {
        LedgerHarness h("v-ghost-evidence");
        auto assistant = h.AppendAssistantWithCall("call_A1", "request-000001");
        h.AppendCallEvidence("action-000001", assistant.id.c_str(), "call_A1");
        auto persisted = h.AppendResultChain("action-000001", "结果预览");
        // 先补一条合法 prepared,再让 acknowledged 的证据指向不存在的事件。
        h.AppendDeliveryLifecycle("action-000001", "delivery-000001", "request-000002",
                                  persisted.id, /*with_prepared=*/true, /*with_sent=*/false,
                                  /*with_acknowledged=*/false);
        EventDraft ack;
        ack.kind = EventKindV3::ToolDeliveryAcknowledged;
        ack.turn_id = "turn-000001";
        ack.step_id = "step-000001";
        ack.action_id = "action-000001";
        ack.request_id = "request-000002";
        ack.payload = nlohmann::json::object({
            {"tool_call_id", "action-000001"},
            {"deliveryId", "delivery-000001"},
            {"evidenceRef", "evt-999999"},
        });
        h.Event(std::move(ack));
        CHECK(HasAsyncCode(ValidateAsyncToolSequence(h.Read()), "async.unknown_evidence"));
    }
}
