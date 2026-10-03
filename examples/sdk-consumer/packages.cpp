#include "lubancore/packages.hpp"

#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace lubancore_consumer {
namespace {
namespace pkg = lubancore::packages::v1;
void Check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
pkg::Analysis Analyze(pkg::Input input) {
    auto result = pkg::AnalyzeManifest(std::move(input));
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(*result);
}
std::string Root() {
    return "schema: 1\nid: consumer.root\nversion: 1.0.0\nname: true\ndescription: 42\nauthors:\n  - name: '作者🌙'\n    url: https://example.test\n";
}
} // namespace

void Packages() {
    pkg::Analysis kept;
    {
        pkg::Input input{Root()};
        kept = Analyze(input);
        input.yaml.assign(input.yaml.size(), 'x');
    }
    Check(kept.manifest.has_value() && kept.issues.empty(), "owned root did not parse");
    Check(kept.manifest->id == "consumer.root" && kept.manifest->version == "1.0.0", "root identity changed");
    Check(kept.manifest->name == "true" && kept.manifest->description == "42", "CLI scalar acceptance changed");
    Check(kept.manifest->authors.size() == 1 && kept.manifest->authors.front().name == "作者🌙" &&
          kept.manifest->authors.front().url == "https://example.test", "owned author changed");
    Check(kept.version_compatibility == pkg::Compatibility::NotDeclared &&
          kept.platform_compatibility == pkg::Compatibility::NotDeclared, "undeclared constraints were invented");
    Check(kept.input_sha256.size() == 64 && kept.input_sha256 == Analyze({Root()}).input_sha256, "input identity not retained");
    Check(kept.input_sha256 != Analyze({Root() + "# different input\n"}).input_sha256, "hash is not exact input bytes");

    const auto yaml = Root() + "compatibility:\n  lubancode: '>=1.0.0-9223372036854775808 <2.0.0'\n  platforms: [linux, macos]\n";
    const auto omitted = Analyze({yaml});
    Check(omitted.manifest.has_value() && omitted.version_compatibility == pkg::Compatibility::NotEvaluated &&
          omitted.platform_compatibility == pkg::Compatibility::NotEvaluated, "omitted host values consulted the environment");
    const auto matches = Analyze({yaml, "1.0.0-9223372036854775809", "linux"});
    Check(matches.version_compatibility == pkg::Compatibility::Compatible &&
          matches.platform_compatibility == pkg::Compatibility::Compatible, "explicit match failed");
    const auto denied = Analyze({yaml, "1.0.0-9223372036854775807", "windows"});
    Check(denied.version_compatibility == pkg::Compatibility::Incompatible &&
          denied.platform_compatibility == pkg::Compatibility::Incompatible, "explicit incompatibility was hidden");
    const auto empty_platforms = Analyze({Root() + "compatibility:\n  platforms: []\n", {}, "windows"});
    Check(empty_platforms.manifest.has_value() && empty_platforms.platform_compatibility == pkg::Compatibility::NotDeclared,
          "empty platform list changed original unconstrained semantics");

    const auto bad = Analyze({"schema: 1\nid: a.b\nversion: 0.1.0\nname: n\nextra: 1"});
    Check(!bad.manifest && bad.issues.size() == 1 && bad.issues.front().field == "extra" &&
          bad.issues.front().line == 5 && !bad.issues.front().detail.empty(), "parser diagnostic changed");
    Check(bad.input_sha256 == "a3a9fdd3726e61172a83eb5eaededdc0a212c2056dc3baef1a825c3c1d3dc096", "exact input SHA-256 changed");
    Check(bad.version_compatibility == pkg::Compatibility::NotEvaluated &&
          bad.platform_compatibility == pkg::Compatibility::NotEvaluated, "parse failure claimed no constraints");
    const auto empty = Analyze({""});
    Check(!empty.manifest && empty.issues.size() == 1, "empty YAML became a package");
    for (auto input : {pkg::Input{Root() + std::string(1, '\0')},
                       pkg::Input{Root() + std::string(1, static_cast<char>(0xff))},
                       pkg::Input{std::string(pkg::kMaxManifestBytes + 1, 'x')}}) {
        const auto rejected = pkg::AnalyzeManifest(std::move(input));
        Check(!rejected && rejected.error().code == "sdk.package.invalid_input", "unsafe input passed the public entry gate");
    }
    for (auto input : {pkg::Input{Root(), ""}, pkg::Input{Root(), std::string(pkg::kMaxHostVersionBytes + 1, '9')},
                       pkg::Input{Root(), {}, "Linux"}, pkg::Input{Root(), {}, std::string("linux\0", 6)}}) {
        const auto rejected = pkg::AnalyzeManifest(std::move(input));
        Check(!rejected && rejected.error().code == "sdk.package.invalid_host", "invalid host option was ignored");
    }
    std::string exact = Root() + "#";
    exact.resize(pkg::kMaxManifestBytes, 'x');
    Check(Analyze({exact}).manifest.has_value(), "exact YAML cap was rejected");
    std::string version = "1.0.0-";
    version.resize(pkg::kMaxHostVersionBytes, '9');
    Check(Analyze({Root(), version, "macos"}).manifest.has_value(), "exact host version cap was rejected");
}
} // namespace lubancore_consumer
