#include "lubancore/packages.hpp"

#include <algorithm>
#include <utility>

#include "package/manifest.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"

namespace lubancore::packages::v1 {
namespace {
bool SafeText(const std::string& text, std::size_t cap) {
    return text.size() <= cap && text.find('\0') == std::string::npos &&
           lubancode::platform::IsValidUtf8(text);
}
} // namespace

Result<Analysis> AnalyzeManifest(Input input) {
    if (!SafeText(input.yaml, kMaxManifestBytes))
        return std::unexpected(Error{"sdk.package.invalid_input", "Manifest must be UTF-8 without NUL and at most 256 KiB"});
    std::optional<lubancode::package::SemVer> host_version;
    if (input.lubancode_version) {
        if (!SafeText(*input.lubancode_version, kMaxHostVersionBytes) ||
            !(host_version = lubancode::package::ParseSemVer(*input.lubancode_version)))
            return std::unexpected(Error{"sdk.package.invalid_host", "Explicit LubanCode version is invalid or exceeds 4096 bytes"});
    }
    if (input.platform && (!SafeText(*input.platform, kMaxPlatformBytes) ||
        (*input.platform != "windows" && *input.platform != "linux" && *input.platform != "macos")))
        return std::unexpected(Error{"sdk.package.invalid_host", "Explicit platform must be windows, linux or macos"});

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

} // namespace lubancore::packages::v1
