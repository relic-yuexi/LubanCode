// Focused unit tests for the shared automatic memory learning owner
// (`agent::memory_learning_owner`) and CLI settlement/dispatch parity.

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/memory_learning_owner.hpp"
#include "app/commands/memory_commands.hpp"
#include "app/memory_extract.hpp"
#include "app/turn_memory_extractor.hpp"
#include "cli/theme.hpp"
#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;
namespace owner = lubancode::agent::memory_learning_owner;
namespace gates = lubancode::agent::memory_learning_gates;
namespace extract = lubancode::agent::memory_extraction;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    ("lmb-memowner-" + std::to_string(run_id % 100000) + "-" + name + "-" +
                     std::to_string(++sequence));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path;
}

api::Message MakeTextMessage(api::Role role, const std::string& text) {
    api::Message msg;
    msg.role = role;
    msg.content.push_back(api::TextBlock{text});
    return msg;
}

extract::ProposedCandidate MakeProposed(const std::string& kind,
                                        const std::string& title,
                                        const std::string& confidence,
                                        std::vector<std::string> paths = {},
                                        std::vector<std::string> keywords = {}) {
    extract::ProposedCandidate p;
    p.kind = kind;
    p.title = title;
    p.summary = title + " summary";
    p.content = title + " detailed content.";
    p.confidence = confidence;
    p.paths = std::move(paths);
    p.keywords = std::move(keywords);
    p.occurred_at = "2026-11-13";
    return p;
}

class RecordingSettlementSink final : public owner::LearningSettlementSink {
public:
    std::vector<std::string> hints;
    std::vector<std::size_t> auto_indices;
    std::vector<memory::SaveRequest> save_requests;
    std::vector<std::size_t> review_indices;
    std::vector<bool> review_fallbacks;
    std::size_t cancel_after_candidates = 999;
    std::size_t handled_count = 0;
    bool fallback_next_auto = false;
    bool abort_next_auto = false;
    bool abort_next_review = false;

    void ApplyRetrievalHints(std::vector<std::string> next_hints) override {
        hints = std::move(next_hints);
    }

    bool IsCancelled() const override { return handled_count >= cancel_after_candidates; }

    owner::AutoWriteAttemptOutcome TryAutoWrite(
        std::size_t candidate_index,
        const extract::ProposedCandidate& /*proposed*/,
        const memory::SaveRequest& request) override {
        ++handled_count;
        if (abort_next_auto) {
            abort_next_auto = false;
            return owner::AutoWriteAttemptOutcome::Aborted;
        }
        if (fallback_next_auto) {
            fallback_next_auto = false;
            return owner::AutoWriteAttemptOutcome::FallbackToReview;
        }
        auto_indices.push_back(candidate_index);
        save_requests.push_back(request);
        return owner::AutoWriteAttemptOutcome::Handled;
    }

    owner::SettlementActionStatus AddReviewCandidate(
        std::size_t candidate_index,
        const extract::ProposedCandidate& /*proposed*/,
        memory::MemoryCandidate /*candidate*/,
        bool /*auto_write_eligible*/,
        bool fallback_from_auto_write) override {
        ++handled_count;
        if (abort_next_review) {
            abort_next_review = false;
            return owner::SettlementActionStatus::Aborted;
        }
        review_indices.push_back(candidate_index);
        review_fallbacks.push_back(fallback_from_auto_write);
        return owner::SettlementActionStatus::Completed;
    }
};

struct RecordingWriteReceiptSink final : memory::MemoryWriteReceiptSink {
    std::vector<memory::MemoryWriteReceipt> receipts;
    void OnMemoryWriteReceipt(const memory::MemoryWriteReceipt& receipt) override {
        receipts.push_back(receipt);
    }
};

}  // namespace

TEST_CASE("memory_learning_owner: ExtractTurnSliceFacts captures assistant prose and tool evidence") {
    api::Message user = MakeTextMessage(api::Role::User, "run tests and check CMakeLists.txt");
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ToolUseBlock{"tool-1", "shell", nlohmann::json::object()});
    assistant.content.push_back(api::TextBlock{"Tests pass with ninja build."});
    api::Message tool_result;
    tool_result.role = api::Role::User;
    tool_result.content.push_back(api::ToolResultBlock{"tool-1", "ok", false});

    const std::vector<api::Message> slice{user, assistant, tool_result};
    const owner::TurnSliceFacts facts = owner::ExtractTurnSliceFacts(slice);
    CHECK(facts.has_assistant_message);
    CHECK(facts.has_tool_evidence);
    REQUIRE(facts.tool_names.size() == 1);
    CHECK(facts.tool_names[0] == "shell");
    CHECK(facts.assistant_text == "Tests pass with ninja build.\n");
}

TEST_CASE("memory_learning_owner: EvaluateTurnLearningDispatch enforces shared gate order") {
    const std::string durable_user = "以后统一用 pnpm 装依赖，别再用 npm 了";
    const std::vector<api::Message> valid_messages{
        MakeTextMessage(api::Role::User, durable_user),
        MakeTextMessage(api::Role::Assistant, "明白，后续构建和安装都会统一使用 pnpm。"),
    };
    const std::vector<std::string> tools{"shell"};

    {
        owner::TurnLearningDispatchInput input;
        input.enabled = false;
        const auto res = owner::EvaluateTurnLearningDispatch(input);
        CHECK_FALSE(res.ready);
        REQUIRE(res.skip_reason.has_value());
        CHECK(*res.skip_reason == gates::ExtractionSkipReason::Disabled);
        CHECK_FALSE(res.history_grew);
    }
    {
        owner::TurnLearningDispatchInput input;
        input.enabled = true;
        input.has_new_history = false;
        const auto res = owner::EvaluateTurnLearningDispatch(input);
        CHECK_FALSE(res.ready);
        REQUIRE(res.skip_reason.has_value());
        CHECK(*res.skip_reason == gates::ExtractionSkipReason::NoNewHistory);
        CHECK_FALSE(res.history_grew);
    }
    {
        owner::TurnLearningDispatchInput input;
        input.enabled = true;
        input.has_new_history = true;
        input.turn_mutated = true;
        input.has_tool_evidence = true;
        input.user_text = durable_user;
        const auto res = owner::EvaluateTurnLearningDispatch(input);
        CHECK_FALSE(res.ready);
        REQUIRE(res.skip_reason.has_value());
        CHECK(*res.skip_reason == gates::ExtractionSkipReason::AlreadyMutated);
        CHECK(res.history_grew);
        CHECK(res.has_tool_evidence);
        CHECK_FALSE(res.durable_signals_evaluated);
    }
    {
        owner::TurnLearningDispatchInput input;
        input.enabled = true;
        input.has_new_history = true;
        input.user_text = "/help";
        input.turn_messages = &valid_messages;
        const auto res = owner::EvaluateTurnLearningDispatch(input);
        CHECK_FALSE(res.ready);
        REQUIRE(res.skip_reason.has_value());
        CHECK(*res.skip_reason == gates::ExtractionSkipReason::SlashCommandOnly);
        CHECK(res.durable_signals_evaluated);
    }
    {
        owner::TurnLearningDispatchInput input;
        input.enabled = true;
        input.has_new_history = true;
        input.user_text = "ok";
        input.turn_messages = &valid_messages;
        const auto res = owner::EvaluateTurnLearningDispatch(input);
        CHECK_FALSE(res.ready);
        REQUIRE(res.skip_reason.has_value());
        CHECK(*res.skip_reason == gates::ExtractionSkipReason::AcknowledgementOnly);
    }
    {
        owner::TurnLearningDispatchInput input;
        input.enabled = true;
        input.has_new_history = true;
        input.user_text = "今天天气真不错我们来聊聊日常安排吧";
        input.assistant_text = "好的，今天安排比较轻松。";
        input.turn_messages = &valid_messages;
        const auto res = owner::EvaluateTurnLearningDispatch(input);
        CHECK_FALSE(res.ready);
        REQUIRE(res.skip_reason.has_value());
        CHECK(*res.skip_reason == gates::ExtractionSkipReason::NoDurableSignal);
    }
    {
        api::Message thinking;
        thinking.role = api::Role::Assistant;
        thinking.content.push_back(api::ThinkingBlock{"internal reasoning only"});
        const std::vector<api::Message> thinking_only{thinking};

        owner::TurnLearningDispatchInput input;
        input.enabled = true;
        input.has_new_history = true;
        input.user_text = durable_user;
        input.assistant_text = "";
        input.turn_messages = &thinking_only;
        const auto res = owner::EvaluateTurnLearningDispatch(input);
        CHECK_FALSE(res.ready);
        REQUIRE(res.skip_reason.has_value());
        CHECK(*res.skip_reason == gates::ExtractionSkipReason::EmptyTranscript);
    }
    {
        owner::TurnLearningDispatchInput input;
        input.enabled = true;
        input.has_new_history = true;
        input.has_tool_evidence = true;
        input.user_text = durable_user;
        input.assistant_text = "已记录包管理器偏好。";
        input.turn_messages = &valid_messages;
        input.tool_names = &tools;
        input.max_transcript_bytes = 8 * 1024;
        const auto res = owner::EvaluateTurnLearningDispatch(input);
        CHECK(res.ready);
        CHECK_FALSE(res.skip_reason.has_value());
        CHECK_FALSE(res.durable_signals.empty());
        CHECK_FALSE(res.task_type.empty());
        CHECK_FALSE(res.system_prompt.empty());
        CHECK_FALSE(res.transcript.empty());
    }
}

TEST_CASE("memory_learning_owner: ClassifyLearnedCandidate and BuildAutoWriteSaveRequest match contract") {
    const auto fact_ok = MakeProposed("fact", "CMake entry", "verified", {"CMakeLists.txt"}, {"cmake"});
    const auto fact_no_paths = MakeProposed("fact", "No paths", "verified", {});
    const auto fact_user_stated = MakeProposed("fact", "Unverified fact", "user-stated", {"CMakeLists.txt"});
    const auto feedback_ok = MakeProposed("feedback", "No global npm", "user-stated");
    const auto feedback_inferred = MakeProposed("feedback", "Guessed style", "inferred");
    const auto pref_verified = MakeProposed("preference", "Use Ninja", "verified");
    const auto pref_user = MakeProposed("preference", "Use pnpm", "user-stated");
    const auto pref_inferred = MakeProposed("preference", "Guessed pref", "inferred");
    const auto bogus = MakeProposed("mystery", "Invalid kind", "verified");

    CHECK_FALSE(owner::ClassifyLearnedCandidate(memory::LearnMode::Auto, bogus).has_value());

    // Review mode never auto-writes any candidate.
    for (const auto& cand : {fact_ok, feedback_ok, pref_verified, pref_user}) {
        const auto cls = owner::ClassifyLearnedCandidate(memory::LearnMode::Review, cand);
        REQUIRE(cls.has_value());
        CHECK_FALSE(cls->auto_writable);
    }

    // Auto mode durability rules.
    CHECK(owner::ClassifyLearnedCandidate(memory::LearnMode::Auto, fact_ok)->auto_writable);
    CHECK_FALSE(owner::ClassifyLearnedCandidate(memory::LearnMode::Auto, fact_no_paths)->auto_writable);
    CHECK_FALSE(owner::ClassifyLearnedCandidate(memory::LearnMode::Auto, fact_user_stated)->auto_writable);
    CHECK(owner::ClassifyLearnedCandidate(memory::LearnMode::Auto, feedback_ok)->auto_writable);
    CHECK_FALSE(owner::ClassifyLearnedCandidate(memory::LearnMode::Auto, feedback_inferred)->auto_writable);
    CHECK(owner::ClassifyLearnedCandidate(memory::LearnMode::Auto, pref_verified)->auto_writable);
    CHECK(owner::ClassifyLearnedCandidate(memory::LearnMode::Auto, pref_user)->auto_writable);
    CHECK_FALSE(owner::ClassifyLearnedCandidate(memory::LearnMode::Auto, pref_inferred)->auto_writable);

    const memory::SaveRequest req =
        owner::BuildAutoWriteSaveRequest(memory::MemoryKind::Fact, fact_ok, "sess-42");
    CHECK(req.kind == memory::MemoryKind::Fact);
    CHECK(req.title == fact_ok.title);
    CHECK(req.summary == fact_ok.summary);
    CHECK(req.content == fact_ok.content);
    CHECK(req.keywords == fact_ok.keywords);
    CHECK(req.paths == fact_ok.paths);
    CHECK(req.confidence == "verified");
    CHECK(req.source_session == "sess-42");
    CHECK(req.occurred_at == "2026-11-13");
    CHECK(req.scope.level == "project");
    CHECK(req.scope.kind == "project");
    REQUIRE(req.evidence.size() == 1);
    CHECK(req.evidence[0].path == "CMakeLists.txt");
}

TEST_CASE("memory_learning_owner: SettleLearnedCandidates enforces caps, hints, fallbacks, and cancel") {
    extract::MemoryExtraction ext;
    ext.task_type = "code";
    ext.summary = "Summary";
    ext.retrieval_terms = {"ninja"};
    ext.candidates = {
        MakeProposed("fact", "Fact verified", "verified", {"CMakeLists.txt"}, {"cmake"}),
        MakeProposed("feedback", "Feedback inferred", "inferred", {}, {"lint"}),
        MakeProposed("preference", "Pref user", "user-stated", {}, {"pnpm"}),
        MakeProposed("preference", "Over cap", "user-stated", {}, {"extra"}),
    };

    // SDK-style hint isolation + fallback + max_candidates = 3.
    {
        RecordingSettlementSink sink;
        sink.fallback_next_auto = true;  // First auto candidate falls back to review.
        owner::SettlementOptions opts;
        opts.mode = memory::LearnMode::Auto;
        opts.task_type = "code";
        opts.source_session = "sess-sdk";
        opts.max_candidates = 3;
        opts.include_candidate_keywords_in_hints = false;

        const auto summary = owner::SettleLearnedCandidates(ext, opts, sink);
        CHECK_FALSE(summary.cancelled);
        CHECK_FALSE(summary.aborted);
        CHECK(summary.processed_candidates == 3);
        CHECK(sink.hints == std::vector<std::string>{"ninja"});
        REQUIRE(sink.review_indices == std::vector<std::size_t>{0, 1});
        CHECK(sink.review_fallbacks == std::vector<bool>{true, false});
        CHECK(sink.auto_indices == std::vector<std::size_t>{2});
        REQUIRE(sink.save_requests.size() == 1);
        CHECK(sink.save_requests[0].source_session == "sess-sdk");
    }

    // Cancellation mid-settlement stops before subsequent candidates.
    {
        RecordingSettlementSink sink;
        sink.cancel_after_candidates = 1;
        owner::SettlementOptions opts;
        opts.mode = memory::LearnMode::Auto;
        opts.max_candidates = 3;
        opts.include_candidate_keywords_in_hints = true;

        const auto summary = owner::SettleLearnedCandidates(ext, opts, sink);
        CHECK(summary.cancelled);
        CHECK(summary.processed_candidates == 1);
        CHECK(sink.hints == std::vector<std::string>{"ninja", "cmake", "lint", "pnpm", "extra"});
    }
}

TEST_CASE("memory_learning_owner: CLI SettleTurnMemory parity in LearnMode::Auto") {
    const fs::path root = TempRoot("cli-auto-settle");
    const fs::path repo = root / "repo";
    const fs::path home = root / "home";
    fs::create_directories(repo);
    fs::create_directories(home);

    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());

    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.learn = memory::LearnMode::Auto;
    options.learn_ceiling = memory::LearnMode::Auto;

    RecordingWriteReceiptSink receipts;
    auto store = std::make_shared<memory::ProjectMemory>(std::move(*identity), home, options);
    store->set_write_receipt_sink(&receipts);

    runtime::TrajectorySessionLedger::Options ledger_options;
    ledger_options.workspaces_root = root / "workspaces";
    ledger_options.workspace_root = repo;
    ledger_options.workspace_identity = workspace::MakeFallbackIdentity(repo);
    ledger_options.lubancode_version = "test";
    auto session = runtime::TrajectorySessionLedger::Open(ledger_options);
    REQUIRE(session.has_value());

    app::MemoryTurnLedger ledger(&*session);
    ledger.BeginTurn(session->session_id(), "turn-auto", "remember verified build setup");
    ledger.NoteExtractionCalled();
    ledger.SuspendTurn();

    app::TurnMemoryExtractor::Outcome outcome;
    outcome.ok = true;
    outcome.extraction_invoked = true;
    outcome.turn_id = "turn-auto";
    outcome.task_type = "code";
    outcome.extraction.task_type = "code";
    outcome.extraction.summary = "Auto settlement parity test";
    outcome.extraction.retrieval_terms = {"ctest"};
    outcome.extraction.candidates = {
        MakeProposed("fact", "Verified fact", "verified", {"CMakeLists.txt"}, {"cmake"}),
        MakeProposed("fact", "Unanchored verified fact", "verified", {}, {"missing-path"}),
        MakeProposed("feedback", "Inferred feedback", "inferred", {}, {"style"}),
        MakeProposed("preference", "User preference", "user-stated", {}, {"ninja"}),
    };

    const cli::Theme theme;
    app::SessionTailContext tail;
    tail.project_memory = store.get();
    tail.theme = &theme;
    tail.memory_turns = &ledger;
    app::SettleTurnMemory(tail, outcome, /*tail_wall_ms=*/12);

    // Eligible Auto candidates enqueued with AutoExtraction receipt; ineligible routed to review.
    CHECK(receipts.receipts.size() == 2);
    for (const auto& receipt : receipts.receipts) {
        CHECK(receipt.source == memory::MemoryWriteSource::AutoExtraction);
        CHECK(receipt.outcome == memory::MemoryWriteReceiptOutcome::Queued);
    }
    const auto candidates = store->ListCandidates();
    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0].title == "Unanchored verified fact");
    CHECK(candidates[1].title == "Inferred feedback");
    CHECK(candidates[0].task_type == "code");

    CHECK(ledger.funnel().review_candidates_queued == 2);
    CHECK(ledger.funnel().auto_writes_queued == 2);
}
