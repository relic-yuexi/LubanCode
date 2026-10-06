#include "runtime/managed_operation_provenance.hpp"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/session_recovery_view.hpp"
#include "trajectory/managed_session_reservation.hpp"
#include "trajectory/v3/reader.hpp"
#include "runtime/operation_result.hpp"

namespace lubancode::runtime {
namespace {
using Json = nlohmann::json;
using Owner = trajectory::ManagedSessionOwnership;
constexpr char kInvalid[] = "managed.operation.invalid_materials";
bool Opaque(const std::string& value) {
    return !value.empty() && value.size() <= 512 && platform::IsValidUtf8(value) &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool Id(const std::string& value) {
    return !value.empty() && value.size() <= 200 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
bool Hash(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool ValidOwner(const Owner& owner) {
    return Opaque(owner.tenant_id) && Opaque(owner.project_id) && Opaque(owner.workspace_key) &&
        Id(owner.session_id) && owner.binding_version != 0;
}
bool ValidAdmission(const ManagedOperationAdmission& value, const Owner& expected) {
    const auto& s = value.subject;
    return ValidOwner(expected) && value.owner == expected && s.tenant_id == expected.tenant_id &&
        Opaque(s.user_id) && Opaque(s.credential_id) &&
        (s.actor_kind == "user" || s.actor_kind == "agent" || s.actor_kind == "service") &&
        value.policy_revision != 0 && value.allowed_capabilities == std::vector<std::string>{"RequestModel"};
}
bool Exact(const Json& object, std::initializer_list<const char*> keys) {
    if (!object.is_object() || object.size() != keys.size()) return false;
    return std::all_of(keys.begin(), keys.end(), [&](const char* key) { return object.contains(key); });
}
bool String(const Json& object, const char* key) { return object.contains(key) && object[key].is_string(); }
bool Unsigned(const Json& object, const char* key, std::uint64_t& out) {
    if (!object.contains(key)) return false;
    const auto& value = object[key];
    if (value.is_number_unsigned()) { out = value.get<std::uint64_t>(); return true; }
    if (!value.is_number_integer()) return false;
    const auto n = value.get<std::int64_t>();
    if (n < 0) return false;
    out = static_cast<std::uint64_t>(n); return true;
}
bool Timestamp(const Json& object, const char* key, std::int64_t& out) {
    std::uint64_t n = 0;
    if (!Unsigned(object, key, n) || n > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return false;
    out = static_cast<std::int64_t>(n); return true;
}
Json SubjectJson(const ManagedOperationSubject& s) {
    return {{"tenantId", s.tenant_id}, {"userId", s.user_id}, {"actorKind", s.actor_kind}, {"credentialId", s.credential_id}};
}
Json OwnerJson(const Owner& o) {
    return {{"tenantId", o.tenant_id}, {"projectId", o.project_id}, {"workspaceKey", o.workspace_key}, {"sessionId", o.session_id}};
}
std::expected<std::string, std::string> Canonical(const Json& value) {
    return trajectory::CanonicalJsonDump(value);
}
std::expected<std::string, std::string> Digest(const char* domain, const Json& value) {
    auto bytes = Canonical(value);
    if (!bytes) return std::unexpected(bytes.error());
    return platform::Sha256Hex(std::string(domain) + "\n" + *bytes);
}
std::expected<std::string, std::string> InputHash(const std::string& text) {
    if (text.find('\0') != std::string::npos || !platform::IsValidUtf8(text))
        return std::unexpected("managed.operation.invalid_input");
    return Digest("lubancode.managed.operation.input.v2", {{"schemaVersion", 2}, {"text", text}, {"images", Json::array()}});
}
Json ProvenanceJson(const ManagedOperationProvenance& p) {
    return {{"schemaVersion", 1}, {"initiatingSubject", SubjectJson(p.admission.subject)},
        {"owner", OwnerJson(p.admission.owner)}, {"bindingVersion", p.admission.owner.binding_version},
        {"allowedCapabilities", p.admission.allowed_capabilities}, {"intentKind", "managed.text_submit.v1"},
        {"admissionPolicyRevision", p.admission.policy_revision}, {"operationId", p.operation_id},
        {"inputId", p.input_id}, {"executionId", p.execution_id}, {"runId", p.run_id},
        {"inputHash", p.input_hash}, {"intentHash", p.intent_hash}};
}
std::expected<ManagedOperationProvenance, std::string> ParseProvenance(const Json& j,
    const Owner& expected, const std::string& run_id, const std::string& digest) {
    if (!Exact(j, {"schemaVersion", "initiatingSubject", "owner", "bindingVersion", "allowedCapabilities",
        "intentKind", "admissionPolicyRevision", "operationId", "inputId", "executionId", "runId", "inputHash", "intentHash"}) ||
        !j["schemaVersion"].is_number_integer() || j["schemaVersion"] != 1 ||
        !String(j, "intentKind") || j["intentKind"] != "managed.text_submit.v1" ||
        !Exact(j["owner"], {"tenantId", "projectId", "workspaceKey", "sessionId"}) ||
        !Exact(j["initiatingSubject"], {"tenantId", "userId", "actorKind", "credentialId"}))
        return std::unexpected(kInvalid);
    for (const auto* key : {"tenantId", "projectId", "workspaceKey", "sessionId"})
        if (!String(j["owner"], key)) return std::unexpected(kInvalid);
    for (const auto* key : {"tenantId", "userId", "actorKind", "credentialId"})
        if (!String(j["initiatingSubject"], key)) return std::unexpected(kInvalid);
    for (const auto* key : {"operationId", "inputId", "executionId", "runId", "inputHash", "intentHash"})
        if (!String(j, key)) return std::unexpected(kInvalid);
    ManagedOperationProvenance p;
    const auto& o = j["owner"]; const auto& s = j["initiatingSubject"];
    p.admission.owner = {o["tenantId"].get<std::string>(), o["projectId"].get<std::string>(),
        o["workspaceKey"].get<std::string>(), o["sessionId"].get<std::string>(), 0};
    p.admission.subject = {s["tenantId"].get<std::string>(), s["userId"].get<std::string>(),
        s["actorKind"].get<std::string>(), s["credentialId"].get<std::string>()};
    if (!Unsigned(j, "bindingVersion", p.admission.owner.binding_version) ||
        !Unsigned(j, "admissionPolicyRevision", p.admission.policy_revision) ||
        !j["allowedCapabilities"].is_array()) return std::unexpected(kInvalid);
    for (const auto& capability : j["allowedCapabilities"]) {
        if (!capability.is_string()) return std::unexpected(kInvalid);
        p.admission.allowed_capabilities.push_back(capability.get<std::string>());
    }
    p.operation_id = j["operationId"].get<std::string>(); p.input_id = j["inputId"].get<std::string>();
    p.execution_id = j["executionId"].get<std::string>(); p.run_id = j["runId"].get<std::string>();
    p.input_hash = j["inputHash"].get<std::string>(); p.intent_hash = j["intentHash"].get<std::string>();
    p.provenance_hash = digest;
    if (!ValidAdmission(p.admission, expected) || !Id(p.operation_id) || !Id(p.input_id) ||
        p.execution_id != p.operation_id || !Id(run_id) || p.run_id != run_id ||
        !Hash(p.input_hash) || !Hash(p.intent_hash) || !Hash(digest)) return std::unexpected(kInvalid);
    auto actual = Digest("lubancode.managed.operation.provenance.v1", ProvenanceJson(p));
    if (!actual || *actual != digest) return std::unexpected(kInvalid);
    return p;
}
bool ValidRejection(const std::string& status, const std::string& reason) {
    return (status == "rejected" || status == "cancelled") && !reason.empty() && reason.size() <= 128 &&
        reason.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == std::string::npos;
}
} // namespace

std::expected<std::string, std::string> ManagedOperationIntentHash(const ManagedOperationAdmission& value,
    const Owner& expected, const std::string& text) {
    if (!ValidAdmission(value, expected)) return std::unexpected("managed.operation.admission_invalid");
    auto input = InputHash(text);
    if (!input) return std::unexpected(input.error());
    return Digest("lubancode.managed.operation.intent.v1", {{"schemaVersion", 1},
        {"intentKind", "managed.text_submit.v1"}, {"initiatingSubject", SubjectJson(value.subject)},
        {"owner", OwnerJson(value.owner)}, {"bindingVersion", value.owner.binding_version},
        {"allowedCapabilities", value.allowed_capabilities}, {"inputHash", *input}});
}

std::expected<PreparedManagedOperation, std::string> PrepareManagedOperation(const ManagedOperationAdmission& admission,
    const Owner& expected, const std::string& run_id, const std::string& key, const std::string& text,
    const std::string& operation_id, const std::string& input_id, std::int64_t received_at_ms) {
    if (!Opaque(key) || !Id(operation_id) || !Id(input_id) || !Id(run_id) || received_at_ms < 0)
        return std::unexpected("managed.operation.invalid_input");
    auto intent = ManagedOperationIntentHash(admission, expected, text);
    if (!intent) return std::unexpected(intent.error());
    auto input_hash = InputHash(text);
    if (!input_hash) return std::unexpected(input_hash.error());
    PreparedManagedOperation out;
    auto& stored = out.operation;
    stored.provenance = {admission, operation_id, input_id, operation_id, run_id, *input_hash, *intent, {}};
    auto provenance_hash = Digest("lubancode.managed.operation.provenance.v1", ProvenanceJson(stored.provenance));
    if (!provenance_hash) return std::unexpected(provenance_hash.error());
    stored.provenance.provenance_hash = std::move(*provenance_hash);
    stored.client_operation_id = key; stored.text = text; stored.received_at_ms = received_at_ms;
    stored.input_ref = "operations-inputs/" + operation_id + ".json";
    auto artifact = Canonical({{"schemaVersion", 2}, {"operationId", operation_id}, {"inputId", input_id},
        {"clientOperationId", key}, {"text", text}, {"images", Json::array()},
        {"provenance", ProvenanceJson(stored.provenance)}, {"provenanceHash", stored.provenance.provenance_hash}});
    if (!artifact) return std::unexpected(artifact.error());
    if (artifact->size() > kManagedOperationInputBytes) return std::unexpected("managed.operation.input_limit");
    out.input_bytes = std::move(*artifact); stored.input_bytes = out.input_bytes.size();
    stored.input_sha256 = platform::Sha256Hex(out.input_bytes);
    auto accepted = Canonical({{"schemaVersion", 3}, {"kind", "operation.accepted"}, {"operationId", operation_id},
        {"inputId", input_id}, {"clientOperationId", key}, {"payloadHash", stored.provenance.input_hash},
        {"inputRef", stored.input_ref}, {"inputSha256", stored.input_sha256}, {"inputBytes", stored.input_bytes},
        {"provenance", ProvenanceJson(stored.provenance)}, {"provenanceHash", stored.provenance.provenance_hash},
        {"receivedAtMs", received_at_ms}});
    if (!accepted) return std::unexpected(accepted.error());
    out.accepted_line = std::move(*accepted); return out;
}

std::expected<std::string, std::string> PrepareManagedOperationRejection(const ManagedStoredOperation& operation,
    const std::string& status, const std::string& reason, std::uint64_t revision, std::int64_t rejected_at_ms) {
    if (operation.state != ManagedStoredOperation::State::Accepted || !ValidRejection(status, reason) ||
        (status == "rejected" && revision == 0) ||
        rejected_at_ms < operation.received_at_ms || !Hash(operation.provenance.provenance_hash))
        return std::unexpected("managed.operation.invalid_rejection");
    return Canonical({{"schemaVersion", 3}, {"kind", "operation.rejected"},
        {"operationId", operation.provenance.operation_id}, {"provenanceHash", operation.provenance.provenance_hash},
        {"terminalStatus", status}, {"reasonCode", reason}, {"decisionPolicyRevision", revision},
        {"rejectedAtMs", rejected_at_ms}});
}

namespace {
std::expected<std::vector<ManagedStoredOperation>, std::string> ReadLedger(
    const std::string& bytes, const Owner& expected, const std::string& run_id, bool execution) {
    if (!ValidOwner(expected) || !Id(run_id) || bytes.find('\0') != std::string::npos || !platform::IsValidUtf8(bytes))
        return std::unexpected(kInvalid);
    auto lines = trajectory::RecoveryStreamLines(bytes, trajectory::RecoveryStreamReadLimits{
        kManagedOperationLedgerBytes, kManagedOperationLedgerLines, kManagedOperationLedgerLineBytes});
    if (!lines) return std::unexpected(kInvalid);
    std::vector<ManagedStoredOperation> out;
    std::map<std::string, std::size_t> operations;
    std::set<std::string> keys, input_ids;
    std::size_t total_input = 0;
    for (const auto& line : *lines) {
        const auto row = Json::parse(line, nullptr, false);
        if (!row.is_object() || !row.contains("schemaVersion") || !row["schemaVersion"].is_number_integer() ||
            (row["schemaVersion"] != 3 && (!execution || row["schemaVersion"] != 4)) ||
            !String(row, "kind") || !String(row, "operationId"))
            return std::unexpected(kInvalid);
        auto canonical = Canonical(row);
        if (!canonical || *canonical != line) return std::unexpected(kInvalid);
        const auto id = row["operationId"].get<std::string>();
        if (!Id(id)) return std::unexpected(kInvalid);
        const auto prior = operations.find(id);
        if (row["kind"] == "operation.accepted") {
            if (row["schemaVersion"] != 3) return std::unexpected(kInvalid);
            if (!Exact(row, {"schemaVersion", "kind", "operationId", "inputId", "clientOperationId", "payloadHash",
                "inputRef", "inputSha256", "inputBytes", "provenance", "provenanceHash", "receivedAtMs"}) || prior != operations.end())
                return std::unexpected(kInvalid);
            for (const auto* field : {"inputId", "clientOperationId", "payloadHash", "inputRef", "inputSha256", "provenanceHash"})
                if (!String(row, field)) return std::unexpected(kInvalid);
            ManagedStoredOperation op;
            auto provenance = ParseProvenance(row["provenance"], expected, run_id, row["provenanceHash"].get<std::string>());
            if (!provenance) return std::unexpected(provenance.error());
            op.provenance = std::move(*provenance); op.client_operation_id = row["clientOperationId"].get<std::string>();
            op.input_ref = row["inputRef"].get<std::string>(); op.input_sha256 = row["inputSha256"].get<std::string>();
            std::uint64_t count = 0;
            if (op.provenance.operation_id != id || op.provenance.input_id != row["inputId"] ||
                op.provenance.input_hash != row["payloadHash"] || !Opaque(op.client_operation_id) || !Hash(op.input_sha256) ||
                op.input_ref != "operations-inputs/" + id + ".json" || !Unsigned(row, "inputBytes", count) ||
                count == 0 || count > kManagedOperationInputBytes || !Timestamp(row, "receivedAtMs", op.received_at_ms) ||
                !keys.insert(op.client_operation_id).second || !input_ids.insert(op.provenance.input_id).second ||
                out.size() >= kManagedOperationInputEntries || count > kManagedOperationInputsTotalBytes - total_input)
                return std::unexpected(kInvalid);
            op.input_bytes = static_cast<std::size_t>(count); total_input += op.input_bytes;
            operations.emplace(id, out.size()); out.push_back(std::move(op));
        } else if (row["kind"] == "operation.rejected") {
            if (row["schemaVersion"] != 3) return std::unexpected(kInvalid);
            if (!Exact(row, {"schemaVersion", "kind", "operationId", "provenanceHash", "terminalStatus", "reasonCode",
                "decisionPolicyRevision", "rejectedAtMs"}) || prior == operations.end() ||
                !String(row, "provenanceHash") || !String(row, "terminalStatus") || !String(row, "reasonCode"))
                return std::unexpected(kInvalid);
            auto& op = out[prior->second];
            if (op.state != ManagedStoredOperation::State::Accepted || row["provenanceHash"] != op.provenance.provenance_hash ||
                !ValidRejection(row["terminalStatus"].get<std::string>(), row["reasonCode"].get<std::string>()) ||
                !Unsigned(row, "decisionPolicyRevision", op.terminal_policy_revision) ||
                (row["terminalStatus"] == "rejected" && op.terminal_policy_revision == 0) ||
                !Timestamp(row, "rejectedAtMs", op.rejected_at_ms) || op.rejected_at_ms < op.received_at_ms)
                return std::unexpected(kInvalid);
            op.state = ManagedStoredOperation::State::RejectedBeforeDispatch;
            op.terminal_status = row["terminalStatus"].get<std::string>(); op.reason_code = row["reasonCode"].get<std::string>();
        } else if (execution && row["kind"] == "operation.dispatched") {
            if (row["schemaVersion"] != 4 || prior == operations.end() ||
                !Exact(row, {"schemaVersion", "kind", "operationId", "provenanceHash", "runId", "turnId",
                    "dispatchPolicyRevision", "dispatchedAtMs"}) || !String(row, "provenanceHash") ||
                !String(row, "runId") || !String(row, "turnId")) return std::unexpected(kInvalid);
            auto& op = out[prior->second];
            if (op.state != ManagedStoredOperation::State::Accepted || row["provenanceHash"] != op.provenance.provenance_hash ||
                row["runId"] != op.provenance.run_id || !Id(row["turnId"].get<std::string>()) ||
                !Unsigned(row, "dispatchPolicyRevision", op.dispatch_policy_revision) || !op.dispatch_policy_revision ||
                !Timestamp(row, "dispatchedAtMs", op.dispatched_at_ms) || op.dispatched_at_ms < op.received_at_ms)
                return std::unexpected(kInvalid);
            op.turn_id = row["turnId"].get<std::string>();
            for (const auto& other : out) if (&other != &op && other.turn_id == op.turn_id) return std::unexpected(kInvalid);
            op.state = ManagedStoredOperation::State::Dispatched;
        } else if (execution && row["kind"] == "operation.final") {
            if (row["schemaVersion"] != 4 || prior == operations.end() ||
                !Exact(row, {"schemaVersion", "kind", "operationId", "provenanceHash", "runId", "turnId",
                    "bindingEventId", "bindingSeq", "bindingHash", "executionStatus", "finalMessageRefs", "usageReported",
                    "resultRef", "resultSha256", "resultBytes", "complete", "finalizedAtMs"})) return std::unexpected(kInvalid);
            for (const auto* key : {"provenanceHash", "runId", "turnId", "bindingEventId", "bindingHash", "executionStatus",
                                    "resultRef", "resultSha256"}) if (!String(row, key)) return std::unexpected(kInvalid);
            auto& op = out[prior->second];
            std::uint64_t bytes_count = 0;
            if (op.state != ManagedStoredOperation::State::Dispatched || row["provenanceHash"] != op.provenance.provenance_hash ||
                row["runId"] != op.provenance.run_id || row["turnId"] != op.turn_id ||
                !Id(row["bindingEventId"].get<std::string>()) || !Hash(row["bindingHash"].get<std::string>()) ||
                !Unsigned(row, "bindingSeq", op.binding_seq) || !op.binding_seq ||
                !row["finalMessageRefs"].is_array() || !row["usageReported"].is_boolean() || !row["complete"].is_boolean() ||
                !Hash(row["resultSha256"].get<std::string>()) || row["resultRef"] != "sdk-results/" + id + ".json" ||
                !Unsigned(row, "resultBytes", bytes_count) || !bytes_count || bytes_count > kManagedOperationResultBytes ||
                !Timestamp(row, "finalizedAtMs", op.finalized_at_ms) || op.finalized_at_ms < op.dispatched_at_ms)
                return std::unexpected(kInvalid);
            op.execution_status = row["executionStatus"].get<std::string>();
            if (op.execution_status != "success" && op.execution_status != "error" &&
                op.execution_status != "cancelled" && op.execution_status != "interrupted") return std::unexpected(kInvalid);
            std::set<std::string> refs;
            for (const auto& ref : row["finalMessageRefs"]) {
                if (!ref.is_string() || !Id(ref.get<std::string>()) || !refs.insert(ref.get<std::string>()).second)
                    return std::unexpected(kInvalid);
                op.final_message_refs.push_back(ref.get<std::string>());
            }
            op.binding_event_id = row["bindingEventId"].get<std::string>(); op.binding_hash = row["bindingHash"].get<std::string>();
            op.result_ref = row["resultRef"].get<std::string>(); op.result_sha256 = row["resultSha256"].get<std::string>();
            op.result_bytes = static_cast<std::size_t>(bytes_count); op.usage_reported = row["usageReported"].get<bool>();
            op.complete = row["complete"].get<bool>(); op.state = ManagedStoredOperation::State::Final;
        } else return std::unexpected(kInvalid); // Storage-only explicitly rejects every schema4 row.
    }
    return out;
}

std::expected<std::vector<ManagedStoredOperation>, std::string> ReadInputs(const ManagedOperationMaterials& materials, bool execution) {
    auto operations = ReadLedger(materials.operations, materials.owner, materials.run_id, execution);
    if (!operations) return std::unexpected(operations.error());
    if (materials.inputs.size() != operations->size()) return std::unexpected(kInvalid);
    std::size_t total = materials.operations.size();
    for (auto& op : *operations) {
        const auto found = materials.inputs.find(op.provenance.operation_id);
        if (found == materials.inputs.end() || found->second.size() != op.input_bytes ||
            found->second.size() > kManagedOperationViewBytes - total ||
            platform::Sha256Hex(found->second) != op.input_sha256) return std::unexpected(kInvalid);
        total += found->second.size();
        const auto input = Json::parse(found->second, nullptr, false);
        if (!Exact(input, {"schemaVersion", "operationId", "inputId", "clientOperationId", "text", "images", "provenance", "provenanceHash"}) ||
            !input["schemaVersion"].is_number_integer() || input["schemaVersion"] != 2 ||
            !String(input, "operationId") || !String(input, "inputId") || !String(input, "clientOperationId") ||
            !String(input, "text") || !String(input, "provenanceHash") || !input["images"].is_array() || !input["images"].empty() ||
            input["operationId"] != op.provenance.operation_id || input["inputId"] != op.provenance.input_id ||
            input["clientOperationId"] != op.client_operation_id || input["provenanceHash"] != op.provenance.provenance_hash)
            return std::unexpected(kInvalid);
        auto provenance = ParseProvenance(input["provenance"], materials.owner, materials.run_id, op.provenance.provenance_hash);
        if (!provenance || *provenance != op.provenance) return std::unexpected(kInvalid);
        op.text = input["text"].get<std::string>();
        auto input_hash = InputHash(op.text);
        auto intent = ManagedOperationIntentHash(op.provenance.admission, materials.owner, op.text);
        if (!input_hash || *input_hash != op.provenance.input_hash || !intent || *intent != op.provenance.intent_hash)
            return std::unexpected(kInvalid);
        auto canonical = Canonical(input);
        if (!canonical || *canonical != found->second) return std::unexpected(kInvalid);
    }
    return operations;
}
} // namespace

std::expected<std::vector<ManagedStoredOperation>, std::string> ReadManagedOperationLedgerOwned(
    const std::string& bytes, const Owner& expected, const std::string& run_id) {
    return ReadLedger(bytes, expected, run_id, false);
}
std::expected<std::vector<ManagedStoredOperation>, std::string> ReadManagedExecutionLedgerOwned(
    const std::string& bytes, const Owner& expected, const std::string& run_id) {
    return ReadLedger(bytes, expected, run_id, true);
}
std::expected<std::vector<ManagedStoredOperation>, std::string> ReadManagedOperationsOwned(const ManagedOperationMaterials& materials) {
    return ReadInputs(materials, false);
}

std::expected<std::string, std::string> PrepareManagedOperationDispatch(const ManagedStoredOperation& op,
    const std::string& turn_id, std::uint64_t revision, std::int64_t at) {
    if (op.state != ManagedStoredOperation::State::Accepted || !Id(turn_id) || !revision ||
        at < op.received_at_ms || !Hash(op.provenance.provenance_hash)) return std::unexpected("managed.operation.invalid_dispatch");
    return Canonical({{"schemaVersion", 4}, {"kind", "operation.dispatched"}, {"operationId", op.provenance.operation_id},
        {"provenanceHash", op.provenance.provenance_hash}, {"runId", op.provenance.run_id}, {"turnId", turn_id},
        {"dispatchPolicyRevision", revision}, {"dispatchedAtMs", at}});
}

std::expected<PreparedManagedFinal, std::string> PrepareManagedOperationFinal(const ManagedStoredOperation& op,
    const ManagedOperationResult& result, std::int64_t at) {
    if (op.state != ManagedStoredOperation::State::Dispatched || !Id(op.turn_id) || !Id(op.binding_event_id) ||
        !op.binding_seq || !Hash(op.binding_hash) || at < op.dispatched_at_ms ||
        (result.execution_status != "success" && result.execution_status != "error" && result.execution_status != "cancelled" &&
         result.execution_status != "interrupted") || !platform::IsValidUtf8(result.final_text) || !platform::IsValidUtf8(result.error) ||
        result.final_text.find('\0') != std::string::npos || result.error.find('\0') != std::string::npos)
        return std::unexpected("managed.operation.invalid_final");
    std::set<std::string> refs;
    for (const auto& ref : result.final_message_refs) if (!Id(ref) || !refs.insert(ref).second)
        return std::unexpected("managed.operation.invalid_final");
    PreparedManagedFinal out; out.operation = op;
    out.result_bytes = MakeSdkOperationResultPayload(op.provenance.operation_id, op.turn_id,
        result.final_text, result.error, result.complete).dump();
    if (out.result_bytes.size() > kManagedOperationResultBytes) return std::unexpected("managed.operation.result_limit");
    auto& final = out.operation; final.state = ManagedStoredOperation::State::Final; final.finalized_at_ms = at;
    final.result_ref = "sdk-results/" + op.provenance.operation_id + ".json";
    final.result_sha256 = platform::Sha256Hex(out.result_bytes); final.result_bytes = out.result_bytes.size();
    final.execution_status = result.execution_status; final.final_message_refs = result.final_message_refs;
    final.usage_reported = result.usage_reported; final.complete = result.complete;
    auto line = Canonical({{"schemaVersion", 4}, {"kind", "operation.final"}, {"operationId", op.provenance.operation_id},
        {"provenanceHash", op.provenance.provenance_hash}, {"runId", op.provenance.run_id}, {"turnId", op.turn_id},
        {"bindingEventId", op.binding_event_id}, {"bindingSeq", op.binding_seq}, {"bindingHash", op.binding_hash},
        {"executionStatus", final.execution_status}, {"finalMessageRefs", final.final_message_refs}, {"usageReported", final.usage_reported},
        {"resultRef", final.result_ref}, {"resultSha256", final.result_sha256}, {"resultBytes", final.result_bytes},
        {"complete", final.complete}, {"finalizedAtMs", at}});
    if (!line || line->size() > kManagedOperationLedgerLineBytes) return std::unexpected("managed.operation.final_limit");
    out.final_line = std::move(*line); return out;
}

std::expected<std::vector<ManagedStoredOperation>, std::string> ReadManagedExecutionOwned(const ManagedOperationMaterials& materials) {
    auto operations = ReadInputs(materials, true);
    if (!operations) return std::unexpected(operations.error());
    auto lines = trajectory::RecoveryStreamLines(materials.main,
        {kManagedOperationMainBytes, 262144, 8 * 1024 * 1024});
    if (!lines) return std::unexpected(kInvalid);
    auto ledger = trajectory::v3::ReadV3LedgerOwned({}, *lines);
    if (!ledger || ledger->session_id != materials.owner.session_id || ledger->run_id != materials.run_id ||
        ledger->messages.empty() || !ledger->messages.front().system_meta) return std::unexpected(kInvalid);
    const auto& meta = *ledger->messages.front().system_meta;
    if (!meta.contains("managedSession") || !meta["managedSession"].is_object() ||
        !meta["managedSession"].contains("executionProfile") ||
        !trajectory::IsManagedTextSessionProfile(meta["managedSession"]["executionProfile"])) return std::unexpected(kInvalid);
    const auto& owner = meta["managedSession"];
    if (!Exact(owner, {"schemaVersion", "mode", "tenantId", "projectId", "workspaceKey", "sessionId", "bindingVersion",
                     "creationSubject", "openingPolicyRevision", "executionProfile"})) return std::unexpected(kInvalid);
    for (const auto* key : {"tenantId", "projectId", "workspaceKey", "sessionId", "mode"})
        if (!String(owner, key)) return std::unexpected(kInvalid);
    std::uint64_t opening_revision = 0;
    if (!owner["schemaVersion"].is_number_integer() || owner["schemaVersion"] != 1 || owner["mode"] != "Managed" ||
        !Unsigned(owner, "openingPolicyRevision", opening_revision) || !opening_revision ||
        !Exact(owner["creationSubject"], {"tenantId", "userId", "actorKind", "credentialId"})) return std::unexpected(kInvalid);
    for (const auto* key : {"tenantId", "userId", "actorKind", "credentialId"})
        if (!String(owner["creationSubject"], key) || !Opaque(owner["creationSubject"][key].get<std::string>())) return std::unexpected(kInvalid);
    if (owner["creationSubject"]["tenantId"] != materials.owner.tenant_id ||
        (owner["creationSubject"]["actorKind"] != "user" && owner["creationSubject"]["actorKind"] != "agent" &&
         owner["creationSubject"]["actorKind"] != "service")) return std::unexpected(kInvalid);
    if (owner.value("tenantId", std::string()) != materials.owner.tenant_id ||
        owner.value("projectId", std::string()) != materials.owner.project_id ||
        owner.value("workspaceKey", std::string()) != materials.owner.workspace_key ||
        owner.value("sessionId", std::string()) != materials.owner.session_id ||
        !owner.contains("bindingVersion") || !owner["bindingVersion"].is_number_integer() ||
        owner["bindingVersion"] != materials.owner.binding_version) return std::unexpected(kInvalid);
    auto bindings = trajectory::v3::ReadOperationTurnBindings(*ledger);
    if (!bindings) return std::unexpected(kInvalid);
    std::map<std::string, trajectory::v3::OperationTurnBindingFacts> anchors;
    for (auto& binding : *bindings) {
        if (binding.provenance_hash.empty() || binding.run_id != materials.run_id ||
            !anchors.emplace(binding.operation_id, std::move(binding)).second) return std::unexpected(kInvalid);
    }
    std::set<std::string> results;
    std::size_t result_total = 0, total = materials.operations.size() + materials.main.size();
    for (const auto& [id, bytes] : materials.inputs) { (void)id; total += bytes.size(); }
    if (total > kManagedOperationViewBytes) return std::unexpected(kInvalid);
    for (auto& op : *operations) {
        const auto binding = anchors.find(op.provenance.operation_id);
        const bool dispatched = op.state == ManagedStoredOperation::State::Dispatched || op.state == ManagedStoredOperation::State::Final;
        if (!dispatched) { if (binding != anchors.end()) return std::unexpected(kInvalid); continue; }
        if (binding == anchors.end()) {
            if (op.state == ManagedStoredOperation::State::Final || materials.completion_known) return std::unexpected(kInvalid);
        } else {
            const auto& b = binding->second;
            if (b.input_id != op.provenance.input_id || b.payload_hash != op.provenance.input_hash ||
                b.provenance_hash != op.provenance.provenance_hash || b.turn_id != op.turn_id) return std::unexpected(kInvalid);
            if (op.state == ManagedStoredOperation::State::Final &&
                (op.binding_event_id != b.event_id || op.binding_seq != b.seq || op.binding_hash != b.line_hash)) return std::unexpected(kInvalid);
            op.binding_event_id = b.event_id; op.binding_seq = b.seq; op.binding_hash = b.line_hash;
            anchors.erase(binding);
        }
        const auto result = materials.results.find(op.provenance.operation_id);
        if (op.state != ManagedStoredOperation::State::Final) {
            // Result may have committed before an unconfirmed final append. It
            // is retained as an incomplete original, never promoted to Final.
            if (result == materials.results.end()) continue;
            if (materials.completion_known) return std::unexpected(kInvalid);
        } else if (result == materials.results.end()) return std::unexpected(kInvalid);
        if (op.binding_event_id.empty()) return std::unexpected(kInvalid);
        if (result->second.empty() || result->second.size() > kManagedOperationResultBytes ||
            result->second.size() > kManagedOperationResultsTotalBytes - result_total ||
            result->second.size() > kManagedOperationViewBytes - total) return std::unexpected(kInvalid);
        result_total += result->second.size(); total += result->second.size(); results.insert(op.provenance.operation_id);
        const auto payload = Json::parse(result->second, nullptr, false);
        if (!Exact(payload, {"operationId", "turnId", "finalText", "error", "complete"}) ||
            !String(payload, "operationId") || !String(payload, "turnId") || !String(payload, "finalText") || !String(payload, "error") ||
            !payload["complete"].is_boolean() || payload["operationId"] != op.provenance.operation_id || payload["turnId"] != op.turn_id ||
            payload.dump() != result->second || !platform::IsValidUtf8(payload["finalText"].get<std::string>()) ||
            !platform::IsValidUtf8(payload["error"].get<std::string>())) return std::unexpected(kInvalid);
        if (op.state == ManagedStoredOperation::State::Final) {
            if (op.result_bytes != result->second.size() || platform::Sha256Hex(result->second) != op.result_sha256 ||
                payload["complete"] != op.complete) return std::unexpected(kInvalid);
            for (const auto& ref : op.final_message_refs) {
                const auto* message = ledger->FindMessage(ref);
                if (!message || message->session_id != materials.owner.session_id || message->run_id != materials.run_id ||
                    !message->turn_id || *message->turn_id != op.turn_id || message->seq <= op.binding_seq)
                    return std::unexpected(kInvalid);
            }
        }
    }
    if (!anchors.empty() || results.size() != materials.results.size()) return std::unexpected(kInvalid);
    return operations;
}

} // namespace lubancode::runtime
