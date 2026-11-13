#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

#include "trajectory/v3/reader.hpp"
#include "api/model_input_snapshot.hpp"
#include "hooks/middleware_builtins.hpp"

namespace lubancore_consumer {
void NamedResultsCase(const std::filesystem::path&, const std::filesystem::path&, const std::string&,
                      const std::function<void(const std::filesystem::path&, const std::string&)>&);
}
namespace {
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
void InspectSummary(const fs::path& directory, const std::string& action) {
    const auto path = directory / (directory.filename().string() + ".jsonl");
    const auto ledger = v3::ReadV3Ledger(path);
    REQUIRE_MESSAGE(ledger.has_value(), (ledger ? std::string() : ledger.error()));
    const v3::EventLine* summary = nullptr;
    unsigned raw = 0, formal = 0, mirrored = 0;
    for (const auto& event : ledger->events) {
        if (event.action_id != action) continue;
        if (event.kind == v3::EventKindV3::ToolResultSummaryFinished && event.payload.value("state", "") == "accepted") {
            REQUIRE(summary == nullptr); summary = &event;
        }
        if (event.kind != v3::EventKindV3::ToolResultPersisted) continue;
        for (const auto& ref : event.payload.at("result_ref")) {
            if (ref.at("kind") == "result_metadata") {
                const auto id = ref.at("artifactId").get<std::string>();
                raw += id.starts_with("capture-"); formal += id.starts_with("res-");
            }
            mirrored += fs::exists(directory / fs::u8path(ref.at("path").get<std::string>()));
        }
    }
    REQUIRE(summary != nullptr); REQUIRE(raw == 1); REQUIRE(formal == 1); REQUIRE(mirrored == 0);
    const auto revision = summary->payload.at("sourceContextRevision").get<std::uint64_t>();
    REQUIRE(ledger->revision_chains.contains(revision));
    REQUIRE(summary->payload.at("modelCalls").get<unsigned>() > 0);
    bool selected = false, subsequent_request = false;
    unsigned summary_prepared = 0;
    std::size_t candidates = 0;
    for (const auto& message : ledger->messages) {
        if (message.action_id != action || message.message.value("role", "") != "tool") continue;
        const auto projection = v3::ProjectResultPreview(*ledger, message.message_id);
        if (projection.summary_event_ref != summary->event_id) continue;
        REQUIRE(projection.complete); REQUIRE(projection.summary_valid);
        REQUIRE_FALSE(projection.summary_candidate_refs.empty());
        REQUIRE_FALSE(projection.source_result_event_refs.empty());
        REQUIRE(projection.result_preview.find("host-result://") != std::string::npos);
        selected = true; candidates = projection.summary_candidate_refs.size();
    }
    for (const auto& event : ledger->events) {
        if (event.kind != v3::EventKindV3::ModelRequestPrepared) continue;
        if (event.payload.value("purpose", "") == "action_summary" && event.payload.value("sourceActionId", "") == action) {
            ++summary_prepared; const auto& input = event.payload.at("modelInputSnapshot");
            REQUIRE(event.payload.at("modelInputSnapshotScope") == lubancode::api::kSdkModelRequestInputScope);
            REQUIRE(event.payload.at("outputLimitScope") == lubancode::api::kSdkGenerateOutputLimitScope);
            REQUIRE(input.at("tools").empty()); REQUIRE(input.at("messages").size() == 1);
            const auto* system = ledger->FindMessage(event.payload.at("systemMessageRef").get<std::string>());
            REQUIRE(system != nullptr); REQUIRE(input.at("system") == system->message.at("content"));
            REQUIRE(event.payload.at("inputMessageRefs").size() == 1);
            const auto* prompt = ledger->FindMessage(event.payload.at("inputMessageRefs")[0].get<std::string>());
            REQUIRE(prompt != nullptr); REQUIRE(input.at("messages")[0].at("text") == prompt->message.at("content"));
            const auto estimate = lubancode::hooks::middleware::ComputeUtf8BytesDiv4Estimate(input);
            REQUIRE(event.payload.at("tokenEstimate").at("inputUtf8Bytes") == estimate.at("inputUtf8Bytes"));
            REQUIRE(event.payload.at("tokenEstimate").at("estimatedInputTokens") == estimate.at("estimatedInputTokens"));
            REQUIRE(event.payload.at("outputReserveTokens") == 1024);
        }
        if (event.seq <= summary->seq) continue;
        REQUIRE(event.payload.at("modelInputSnapshotScope") == lubancode::api::kSdkModelRequestInputScope);
        REQUIRE(event.payload.at("outputLimitScope") == lubancode::api::kSdkGenerateOutputLimitScope);
        REQUIRE(event.payload.at("modelInputSnapshotSha256").get<std::string>().size() == 64);
        REQUIRE(event.payload.at("modelInputSnapshotUtf8Bytes").get<std::size_t>() > 0);
        REQUIRE(event.payload.at("modelInputSnapshotFingerprintAlgorithm") == "sha256-compact_json_utf8_v1");
        REQUIRE(v3::CheckPreparedAgainstChain(*ledger, event.event_id).empty()); subsequent_request = true;
    }
    REQUIRE(summary_prepared == summary->payload.at("modelCalls").get<unsigned>());
    REQUIRE(selected); REQUIRE(subsequent_request);
    std::cout << "[sdk-named-results-summary] " << nlohmann::json{
        {"session_id", ledger->session_id}, {"tool_call_id", action}, {"summary_event_id", summary->event_id},
        {"source_revision", revision}, {"model_calls", summary->payload.at("modelCalls")},
        {"raw_results", raw}, {"formal_results", formal}, {"local_named_mirrors", mirrored},
        {"summary_candidates", candidates}, {"selected", selected}, {"subsequent_request_verified", subsequent_request}}.dump() << '\n';
}
void Run(const char* path) {
    const auto probe = fs::u8path(LUBANCORE_TEST_COMMAND_LIMITS_PROBE);
    REQUIRE(probe.is_absolute()); REQUIRE(fs::is_regular_file(probe));
    lubancore_consumer::NamedResultsCase(fs::temp_directory_path(), probe, path,
        std::string(path) == "summary" ? InspectSummary : std::function<void(const fs::path&, const std::string&)>{});
}
} // namespace

TEST_CASE("SDK named results: real tool bytes, projection, Close and fresh Runtime same-ID restoration") { REQUIRE_NOTHROW(Run("roundtrip")); }
TEST_CASE("SDK named results: foreground Job foreground share numbering and passive restored preview") { REQUIRE_NOTHROW(Run("jobs")); }
TEST_CASE("SDK named results: ordinary batch budget reads external evidence for adopted automatic summary") { REQUIRE_NOTHROW(Run("summary")); }
TEST_CASE("SDK named results: actual partial publication, exception and invalid claims never retry") { REQUIRE_NOTHROW(Run("publication")); }
TEST_CASE("SDK named results: strict reads reject changed missing and foreign material") { REQUIRE_NOTHROW(Run("identity")); }
TEST_CASE("SDK named results: same-project and separate-project scenes retain independent stores") { REQUIRE_NOTHROW(Run("isolation")); }
TEST_CASE("SDK named results: ongoing reader survives Close and real Session directory move") { REQUIRE_NOTHROW(Run("close-read")); }
TEST_CASE("SDK named results: cancellation bypasses held publishing callback and Close waits for it") { REQUIRE_NOTHROW(Run("close-write")); }
TEST_CASE("SDK named results: actual concurrent openings Shutdown exceptions and provider reentry retire once") { REQUIRE_NOTHROW(Run("opening")); }
TEST_CASE("SDK named results: frozen namespace and missing provider reject before Open or source mutation") { REQUIRE_NOTHROW(Run("bindings")); }
