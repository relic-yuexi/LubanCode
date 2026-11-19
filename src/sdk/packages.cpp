#include "lubancore/packages.hpp"

#include <algorithm>
#include <utility>

#include "package/manifest.hpp"
#include "package/inventory_snapshot.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"

namespace lubancore::packages::v1 {
namespace {
bool SafeText(const std::string& text, std::size_t cap) {
    return text.size() <= cap && text.find('\0') == std::string::npos &&
           lubancode::platform::IsValidUtf8(text);
}
Result<std::optional<lubancode::package::SemVer>> Host(
    const std::optional<std::string>& version, const std::optional<std::string>& platform) {
    std::optional<lubancode::package::SemVer> host_version;
    if (version) {
        if (!SafeText(*version, kMaxHostVersionBytes) ||
            !(host_version = lubancode::package::ParseSemVer(*version)))
            return std::unexpected(Error{"sdk.package.invalid_host", "Explicit LubanCode version is invalid or exceeds 4096 bytes"});
    }
    if (platform && (!SafeText(*platform, kMaxPlatformBytes) ||
        (*platform != "windows" && *platform != "linux" && *platform != "macos")))
        return std::unexpected(Error{"sdk.package.invalid_host", "Explicit platform must be windows, linux or macos"});
    return host_version;
}
} // namespace

Result<Analysis> AnalyzeManifest(Input input) {
    if (!SafeText(input.yaml, kMaxManifestBytes))
        return std::unexpected(Error{"sdk.package.invalid_input", "Manifest must be UTF-8 without NUL and at most 256 KiB"});
    auto host = Host(input.lubancode_version, input.platform);
    if (!host) return std::unexpected(std::move(host.error()));
    const auto& host_version = *host;

    Analysis analysis;
    analysis.input_sha256 = lubancode::platform::Sha256Hex(input.yaml);
    auto parsed = lubancode::package::ParsePackageManifest(input.yaml);
    if (!parsed) {
        auto& issue = parsed.error();
        analysis.issues.push_back({std::move(issue.field), issue.line, std::move(issue.detail)});
        return analysis;
    }
    const auto& source = *parsed;
    if (!source.compatibility_lubancode) analysis.version_compatibility = Compatibility::NotDeclared;
    else if (host_version) analysis.version_compatibility =
        lubancode::package::VersionSatisfies(*host_version, *source.compatibility_lubancode)
            ? Compatibility::Compatible : Compatibility::Incompatible;
    if (source.compatibility_platforms.empty()) analysis.platform_compatibility = Compatibility::NotDeclared;
    else if (input.platform) analysis.platform_compatibility =
        std::find(source.compatibility_platforms.begin(), source.compatibility_platforms.end(), *input.platform) !=
            source.compatibility_platforms.end() ? Compatibility::Compatible : Compatibility::Incompatible;

    Manifest manifest;
    manifest.schema = source.schema;
    manifest.id = std::move(parsed->id);
    manifest.version = std::move(parsed->version.text);
    manifest.name = std::move(parsed->name);
    manifest.description = std::move(parsed->description);
    for (auto& author : parsed->authors)
        manifest.authors.push_back({std::move(author.name), std::move(author.url)});
    manifest.license = std::move(parsed->license);
    manifest.homepage = std::move(parsed->homepage);
    manifest.repository = std::move(parsed->repository);
    if (parsed->compatibility_lubancode)
        manifest.compatibility_lubancode = std::move(parsed->compatibility_lubancode->text);
    manifest.compatibility_platforms = std::move(parsed->compatibility_platforms);
    analysis.manifest = std::move(manifest);
    return analysis;
}

Result<Inventory> InventoryExplicitRoot(InventoryInput input) {
    const auto& limits = input.limits;
    if (!limits.entries || limits.entries > kMaxInventoryEntries || !limits.depth || limits.depth > kMaxInventoryDepth ||
        !limits.path_bytes || limits.path_bytes > kMaxInventoryPathBytes ||
        !limits.file_bytes || limits.file_bytes > kMaxInventoryFileBytes ||
        !limits.total_bytes || limits.total_bytes > kMaxInventoryTotalBytes)
        return std::unexpected(Error{"sdk.package.invalid_limits", "Inventory limits must be positive and no larger than their defaults"});
    if (input.root.empty() || !SafeText(input.root, limits.path_bytes))
        return std::unexpected(Error{"sdk.package.invalid_input", "Package root must be nonempty UTF-8 without NUL within its path limit"});
    auto host = Host(input.lubancode_version, input.platform);
    if (!host) return std::unexpected(std::move(host.error()));
    const auto root = lubancode::platform::Utf8ToPath(input.root);
    auto captured = lubancode::package::CaptureInventoryRoot(root, {
        limits.entries, limits.depth, limits.path_bytes, limits.file_bytes, limits.total_bytes, kMaxManifestBytes});
    if (!captured) return std::unexpected(Error{std::move(captured.error().code), std::move(captured.error().detail)});
    Inventory inventory;
    inventory.root = std::move(input.root);
    inventory.canonical_root = lubancode::platform::PathToUtf8(captured->canonical_root);
    inventory.directories = std::move(captured->directories);
    inventory.content_sha256 = std::move(captured->content_sha256);
    inventory.total_bytes = captured->total_bytes;
    if (captured->manifest_bytes) {
        auto analysis = AnalyzeManifest({std::move(*captured->manifest_bytes), std::move(input.lubancode_version), std::move(input.platform)});
        if (!analysis) return std::unexpected(std::move(analysis.error()));
        inventory.manifest_analysis = std::move(*analysis);
    } else {
        inventory.issues.push_back({InventoryIssueKind::Error, "package.yaml", "missing_manifest", "Root package.yaml is missing or is not a regular file"});
    }
    for (auto& file : captured->files)
        inventory.files.push_back({std::move(file.relative_path), file.bytes, std::move(file.sha256), file.code_bearing});
    for (auto& component : captured->components) {
        ComponentKind kind;
        using NativeKind = lubancode::package::InventoryComponentKind;
        switch (component.kind) {
            case NativeKind::Agent: kind = ComponentKind::Agent; break;
            case NativeKind::PromptProfile: kind = ComponentKind::PromptProfile; break;
            case NativeKind::Skill: kind = ComponentKind::Skill; break;
            case NativeKind::Workflow: kind = ComponentKind::Workflow; break;
            case NativeKind::Plugin: kind = ComponentKind::Plugin; break;
            case NativeKind::McpServer: kind = ComponentKind::McpServer; break;
            case NativeKind::Channel: kind = ComponentKind::Channel; break;
            default: return std::unexpected(Error{"sdk.package.invalid_inventory", "Unknown internal component kind"});
        }
        std::optional<std::string> canonical;
        if (inventory.manifest_analysis && inventory.manifest_analysis->manifest)
            canonical = inventory.manifest_analysis->manifest->id + ":" + component.local_id;
        inventory.components.push_back({kind, std::move(component.local_id), std::move(component.relative_path), std::move(canonical)});
    }
    for (auto& issue : captured->issues) {
        InventoryIssueKind kind;
        using NativeKind = lubancode::package::InventorySnapshotIssueKind;
        switch (issue.kind) {
            case NativeKind::Info: kind = InventoryIssueKind::Info; break;
            case NativeKind::Warning: kind = InventoryIssueKind::Warning; break;
            case NativeKind::Error: kind = InventoryIssueKind::Error; break;
            default: return std::unexpected(Error{"sdk.package.invalid_inventory", "Unknown internal issue kind"});
        }
        inventory.issues.push_back({kind, std::move(issue.path), std::move(issue.code), std::move(issue.detail)});
    }
    return inventory;
}

} // namespace lubancore::packages::v1
