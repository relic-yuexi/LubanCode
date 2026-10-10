#include "sdk/operation_ledger.hpp"

#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>

#include "tools/path_utils.hpp"
#include "platform/bounded_read.hpp"
#include "platform/sha256.hpp"
#include "runtime/trajectory_session.hpp"

namespace lubancore::detail {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
Error Failure(std::string code, std::string message = {}) { return {std::move(code), std::move(message)}; }
bool ValidId(const std::string& value) {
    return !value.empty() && value.size() <= 200 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
} // namespace

Result<void> ValidateOperationLedger(const fs::path& session_dir) {
    // The shared live-query reader deliberately skips incomplete lines. Recovery
    // cannot do that: losing an accepted row also loses its dedupe key and the
    // operation counter, permitting both repeated effects and reused IDs.
    const auto path = session_dir / "operations.jsonl";
    const auto invalid = [](std::string reason) -> Result<void> {
        return std::unexpected(Failure("sdk.resume.operation_ledger_invalid", std::move(reason)));
    };
    enum class Stage { Accepted, Dispatched, Final };
    std::map<std::string, Stage> stages;
    const auto check_result_coverage = [&]() -> Result<void> {
        std::error_code result_error;
        const auto results = session_dir / "sdk-results";
        const bool exists = fs::exists(results, result_error);
        if (result_error) return invalid("cannot inspect SDK result directory");
        if (!exists) return {};
        fs::directory_iterator entries(results, result_error);
        if (result_error) return invalid("cannot read SDK result directory");
        for (; entries != fs::directory_iterator{}; entries.increment(result_error)) {
            if (result_error) return invalid("cannot read SDK result directory");
            if (entries->path().extension() != ".json") continue;
            const auto id = lubancode::tools::PathToUtf8(entries->path().stem());
            const auto fact = stages.find(id);
            // Result bytes are written before operation.final, so Dispatched is
            // valid during crash recovery. Missing acceptance/dispatch is not.
            if (fact == stages.end() || fact->second == Stage::Accepted) {
                return invalid("SDK result has no preceding operation dispatch");
            }
        }
        if (result_error) return invalid("cannot read SDK result directory");
        return {};
    };
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) return invalid("cannot inspect operation ledger");
    if (!exists) return check_result_coverage(); // new/empty sessions have a lazy ledger
    if (!fs::is_regular_file(path, ec) || ec) return invalid("operation ledger is not a regular file");
    std::ifstream input(path, std::ios::binary);
    if (!input) return invalid("cannot read operation ledger");
    std::set<std::string> keys;
    std::set<std::string> bound_turns;
    const auto is_string = [](const Json& row, const char* key) {
        return row.contains(key) && row.at(key).is_string();
    };
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || input.eof()) return invalid("incomplete operation ledger line");
        const auto row = Json::parse(line, nullptr, false);
        if (!row.is_object() || !row.contains("schemaVersion") || !row.at("schemaVersion").is_number_integer() ||
            (row.at("schemaVersion") != 1 && row.at("schemaVersion") != 2) ||
            !is_string(row, "kind") || !is_string(row, "operationId")) {
            return invalid("invalid operation fact shape");
        }
        const auto id = row.at("operationId").get<std::string>();
        const auto kind = row.at("kind").get<std::string>();
        if (!ValidId(id)) return invalid("invalid operation ID");
        const auto found = stages.find(id);
        if (kind == "operation.accepted") {
            if (found != stages.end() || !is_string(row, "inputId") || row.at("inputId") == "" ||
                !is_string(row, "clientOperationId") || !is_string(row, "payloadHash")) {
                return invalid("invalid or duplicate operation acceptance");
            }
            const auto key = row.at("clientOperationId").get<std::string>();
            const auto hash = row.at("payloadHash").get<std::string>();
            if ((!key.empty() && !keys.insert(key).second) || hash.size() != 64 ||
                hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
                return invalid("conflicting operation acceptance");
            }
            stages.emplace(id, Stage::Accepted);
        } else if (kind == "operation.dispatched") {
            if (found == stages.end() || found->second != Stage::Accepted) {
                return invalid("dispatch without one preceding acceptance");
            }
            found->second = Stage::Dispatched;
        } else if (kind == "operation.final") {
            if (found == stages.end() || found->second != Stage::Dispatched ||
                !is_string(row, "turnId") || !is_string(row, "executionStatus") ||
                row.at("executionStatus") == "" || !row.contains("finalMessageRefs") ||
                !row.at("finalMessageRefs").is_array() || !row.contains("usageReported") ||
                !row.at("usageReported").is_boolean()) {
                return invalid("invalid final or missing preceding dispatch");
            }
            for (const auto& ref : row.at("finalMessageRefs")) {
                if (!ref.is_string()) return invalid("invalid final message reference");
            }
            const auto turn = row.at("turnId").get<std::string>();
            if (!turn.empty() && (!ValidId(turn) || !bound_turns.insert(turn).second))
                return invalid("invalid turn identity or turn bound to multiple operations");
            found->second = Stage::Final;
        } else {
            return invalid("unknown operation fact kind");
        }
    }
    if (input.bad()) return invalid("failed while reading operation ledger");
    return check_result_coverage();
}

namespace {
namespace rt = lubancode::runtime;
namespace v3 = lubancode::trajectory::v3;
Result<std::vector<rt::SessionService::OperationFact>> ReadAnchorOperations(const fs::path& dir) {
    auto bytes = lubancode::platform::ReadBoundedRegularFile(dir / "operations.jsonl", 64 * 1024 * 1024);
    if (!bytes) return std::unexpected(Failure("sdk.resume.operation_ledger_invalid", bytes.error()));
    auto facts = rt::SessionService::ReadOperationFactsOwned(*bytes);
    if (!facts) return std::unexpected(Failure("sdk.resume.operation_ledger_invalid", facts.error()));
    return std::move(*facts);
}
} // namespace

MainOperationTurnStart BeginMainOperationTurn(rt::SessionService& service,
    const std::function<void(const v3::OperationTurnBindingFacts&)>& publish) {
    MainOperationTurnStart out;
    out.error = Failure("sdk.operation_turn.unconfirmed"); // Fallback allocated before taking the input.
    auto* trajectory = service.trajectory();
    auto* writer = trajectory ? trajectory->v3_main_writer() : nullptr;
    if (!writer || writer->closed() || writer->broken()) {
        out.error = Failure("sdk.operation_turn.writer_unavailable");
        return out;
    }
    try {
        auto popped = service.PopPendingInput(); // The only Pop on this producer path.
        if (popped.status != rt::SessionService::PendingPop::Status::Ok) {
            out.error = Failure(popped.status == rt::SessionService::PendingPop::Status::Empty
                ? "sdk.operation_turn.empty" : "sdk.operation_turn.dispatch_unconfirmed");
            return out;
        }
        out.knowledge = OperationTurnKnowledge::ConsumedNoAnchor;
        out.input = std::move(popped.input);
        const auto source = ReadAnchorOperations(trajectory->session_dir());
        if (!source) { out.error = source.error(); return out; }
        const rt::SessionService::OperationFact* accepted = nullptr;
        bool dispatched = false, final = false;
        for (const auto& fact : *source) {
            if (fact.operation_id != out.input->operation_id) continue;
            if (fact.kind == "operation.accepted") accepted = &fact;
            if (fact.kind == "operation.dispatched") dispatched = true;
            if (fact.kind == "operation.final") final = true;
        }
        if (!accepted || !dispatched || final || !ValidId(accepted->input_id) ||
            accepted->payload_hash != lubancode::platform::Sha256Hex(rt::SessionService::CanonicalInputPayload(
                {{}, out.input->text, out.input->images}))) {
            out.error = Failure("sdk.operation_turn.invalid_source");
            return out;
        }
        out.turn_id = writer->NewTurnId();
        v3::OperationTurnBindingFacts facts;
        facts.session_id = writer->session_id(); facts.run_id = writer->run_id(); facts.turn_id = out.turn_id;
        facts.operation_id = accepted->operation_id; facts.input_id = accepted->input_id;
        facts.payload_hash = accepted->payload_hash;
        v3::EventDraft event;
        event.kind = v3::EventKindV3::SdkOperationTurnBound;
        event.turn_id = out.turn_id;
        event.payload = {{"layout", std::string(v3::kSdkMainOperationTurnLayout)}, {"version", 1},
            {"operationId", facts.operation_id}, {"inputId", facts.input_id}, {"payloadHash", facts.payload_hash}};
        out.knowledge = OperationTurnKnowledge::AppendUnconfirmed;
        out.receipt = writer->AppendEvent(std::move(event), v3::Durability::PowerLoss);
        if (out.receipt->status != v3::WriteReceipt::Status::Committed || writer->broken()) {
            out.error = Failure(out.receipt->error_code.empty() ? "sdk.operation_turn.append_unconfirmed" : out.receipt->error_code,
                                out.receipt->error_message);
            return out;
        }
        out.knowledge = OperationTurnKnowledge::CommittedPublicationGap;
        facts.event_id = out.receipt->id; facts.seq = out.receipt->seq; facts.line_hash = out.receipt->line_hash;
        out.facts = std::move(facts);
        if (publish) publish(*out.facts);
        out.knowledge = OperationTurnKnowledge::Bound;
        out.error = {};
    } catch (const std::exception& error) {
        try { out.error = Failure("sdk.operation_turn.unconfirmed", error.what()); } catch (...) {}
    } catch (...) {
        // Keep stage/first receipt; an opaque exception cannot prove zero write.
    }
    return out;
}

OperationTurnMaterialCheck CheckMainOperationTurnBindings(const fs::path& session_dir,
    const v3::V3Ledger& ledger) {
    OperationTurnMaterialCheck out;
    const auto reject = [&](Error error) {
        out.state = OperationTurnMaterialState::Rejected; out.error = std::move(error); return out;
    };
    auto anchors = v3::ReadOperationTurnBindings(ledger);
    if (!anchors) return reject(Failure("sdk.resume.operation_turn_invalid", anchors.error()));
    if (anchors->empty()) return out; // Old, unbound material keeps its established reader semantics.
    std::error_code ec;
    if (!ValidId(ledger.session_id))
        return reject(Failure("sdk.resume.operation_turn_invalid", "invalid main session identity"));
    const auto expected = fs::canonical(session_dir / (ledger.session_id + ".jsonl"), ec);
    if (ec) return reject(Failure("sdk.resume.operation_turn_invalid", "main journal unavailable"));
    const auto parent = fs::canonical(session_dir, ec);
    if (ec || expected.parent_path() != parent || !fs::equivalent(expected, ledger.path, ec) || ec)
        return reject(Failure("sdk.resume.operation_turn_invalid", "journal belongs to another session directory"));
    auto operations = ReadAnchorOperations(session_dir);
    if (!operations) return reject(operations.error());
    out.facts = std::move(*anchors);
    out.state = OperationTurnMaterialState::Validated;
    for (const auto& bound : out.facts) {
        const rt::SessionService::OperationFact* accepted = nullptr;
        const rt::SessionService::OperationFact* final = nullptr;
        bool dispatched = false;
        for (const auto& fact : *operations) {
            if (fact.kind == "operation.final" && fact.turn_id == bound.turn_id &&
                fact.operation_id != bound.operation_id)
                return reject(Failure("sdk.resume.operation_turn_invalid", "turn finalized by another operation"));
            if (fact.operation_id != bound.operation_id) continue;
            if (fact.kind == "operation.accepted") accepted = &fact;
            if (fact.kind == "operation.dispatched") dispatched = true;
            if (fact.kind == "operation.final") final = &fact;
        }
        if (!accepted || !dispatched || accepted->input_id != bound.input_id ||
            accepted->payload_hash != bound.payload_hash || (final && final->turn_id != bound.turn_id))
            return reject(Failure("sdk.resume.operation_turn_invalid", "accepted/dispatched/final relationship differs"));
        if (!final) out.state = OperationTurnMaterialState::Incomplete;
    }
    return out;
}
} // namespace lubancore::detail
