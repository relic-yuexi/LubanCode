#include "trajectory/named_result_opening.hpp"

#include <system_error>
#include <utility>

#include "platform/bounded_read.hpp"
#include "platform/owned_file_path.hpp"
#include "platform/paths.hpp"
#include "platform/secure_file.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancode::trajectory {
namespace {
using Json = nlohmann::json;
constexpr const char* kPlan = "sdk-named-results-plan.json";
const Json* Binding(const v3::MessageLine& message) {
    if (!message.system_meta || !message.system_meta->is_object()) return nullptr;
    const auto host = message.system_meta->find("hostBindings");
    if (host == message.system_meta->end() || !host->is_object()) return nullptr;
    const auto item = host->find("namedResults");
    return item == host->end() ? nullptr : &*item;
}
std::expected<std::optional<std::string>, std::string> ReadPlan(const std::filesystem::path& directory) {
    const auto path = directory / kPlan;
    if (!platform::IsUnlinkedOwnedPath(directory, path))
        return std::unexpected("named_result.plan_path_rejected");
    std::error_code error;
    const auto status = std::filesystem::symlink_status(platform::FileIoPath(path), error);
    if (error == std::errc::no_such_file_or_directory || (!error && status.type() == std::filesystem::file_type::not_found))
        return std::optional<std::string>{};
    if (error || !std::filesystem::is_regular_file(status)) return std::unexpected("named_result.plan_invalid");
    auto bytes = platform::ReadBoundedRegularFile(path, 16384);
    if (!bytes) return std::unexpected("named_result.plan_unreadable");
    return std::optional<std::string>{std::move(*bytes)};
}
std::expected<void, std::string> CheckBinding(const v3::V3Ledger& source,
    const CasScope& scope, const Json& expected) {
    const auto matches = [&](const v3::MessageLine* message) {
        const auto* binding = message ? Binding(*message) : nullptr;
        return message && message->session_id == scope.session_id && message->run_id == source.run_id &&
            message->message.value("role", Json()) == "system" && binding && *binding == expected;
    };
    if (source.session_id != scope.session_id || source.run_id.empty() || source.messages.empty() || source.messages.front().seq != 1 ||
        !matches(&source.messages.front())) return std::unexpected("named_result.initial_binding_mismatch");
    bool adopted = false;
    unsigned starts = 0;
    for (const auto& event : source.events) {
        if (event.kind == v3::EventKindV3::ResumeSourceAttached)
            return std::unexpected("named_result.cross_session_unsupported");
        if (event.kind != v3::EventKindV3::SessionStarted) continue;
        if (++starts != 1 || event.session_id != scope.session_id || event.run_id != source.run_id)
            return std::unexpected("named_result.initial_owner_mismatch");
        const auto context = event.payload.find("context");
        if (context == event.payload.end() || !context->is_object()) continue;
        const auto chain = context->find("contextChain");
        adopted |= chain != context->end() && chain->is_array() && !chain->empty() &&
            chain->front().value("messageRef", Json()) == source.messages.front().message_id;
    }
    if (!adopted || starts != 1) return std::unexpected("named_result.initial_binding_unadopted");
    for (const auto& [revision, chain] : source.revision_chains) {
        (void)revision;
        if (!matches(source.FindMessage(chain.first))) return std::unexpected("named_result.binding_drift");
    }
    if (!matches(source.FindMessage(source.context.system_message_ref))) return std::unexpected("named_result.binding_drift");
    return {};
}
} // namespace

bool HasExternalNamedResultBinding(const v3::V3Ledger& source) {
    for (const auto& message : source.messages) if (Binding(message)) return true;
    return false;
}

std::expected<NamedResultOpening, std::string> OpenLockedNamedResults(const CasScope& scope,
    const std::filesystem::path& directory, const std::shared_ptr<NamedResultFactory>& factory,
    const v3::V3Ledger* source) {
    if (InNamedResultProvider()) return std::unexpected("named_result.reentrant");
    try {
        if (factory) {
            const auto& binding = factory->binding_id();
            if (binding.empty() || binding.size() > 200 || binding == "file-v1" ||
                binding.find_first_of("\r\n") != std::string::npos || binding.find('\0') != std::string::npos ||
                !platform::IsValidUtf8(binding)) return std::unexpected("named_result.invalid_binding");
        }
        auto saved = ReadPlan(directory);
        if (!saved) return std::unexpected(saved.error());
        NamedResultOpening result;
        if (!factory) {
            if (saved->has_value() || (source && HasExternalNamedResultBinding(*source)))
                return std::unexpected("named_result.provider_required");
        } else {
            const Json plan{{"schemaVersion", 1}, {"workspaceKey", scope.workspace_key},
                {"sessionId", scope.session_id}, {"bindingId", factory->binding_id()}};
            const auto bytes = plan.dump();
            if (bytes.size() > 16384) return std::unexpected("named_result.plan_too_large");
            result.binding = Json{{"schemaVersion", 1}, {"sha256", platform::Sha256Hex(bytes)}};
            if (source) {
                if (!*saved || **saved != bytes) return std::unexpected("named_result.plan_mismatch");
                auto checked = CheckBinding(*source, scope, *result.binding);
                if (!checked) return std::unexpected(checked.error());
            } else {
                if (saved->has_value()) return std::unexpected("named_result.preexisting_plan");
                const auto receipt = platform::CreateImmutableFileDetailed(directory / kPlan, bytes,
                    platform::WriteDurability::ProcessCrashDurability);
                if (!receipt.ok() || receipt.outcome != platform::WriteOutcome::CommittedDurable)
                    return std::unexpected("named_result.plan_publish_failed:" + receipt.error_code + ":outcome=" +
                        std::to_string(static_cast<int>(receipt.outcome)));
            }
        }
        auto lease = OpenNamedResultCapability(scope, directory, factory, source != nullptr);
        if (!lease) return std::unexpected(lease.error().code + ": " + lease.error().message);
        result.lease = std::move(*lease);
        return result;
    } catch (...) { return std::unexpected("named_result.opening_failed"); }
}

std::expected<void, std::string> MergeNamedResultBinding(Json& metadata, const std::optional<Json>& binding) {
    if (!metadata.is_object()) return std::unexpected("named_result.invalid_opening_metadata");
    if (metadata.contains("hostBindings") && !metadata["hostBindings"].is_object())
        return std::unexpected("named_result.invalid_host_bindings");
    if (metadata.contains("hostBindings") && metadata["hostBindings"].contains("namedResults"))
        return std::unexpected("named_result.reserved_host_binding");
    if (binding) metadata["hostBindings"]["namedResults"] = *binding;
    return {};
}

} // namespace lubancode::trajectory
