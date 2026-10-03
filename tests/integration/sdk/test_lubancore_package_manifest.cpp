#include <doctest/doctest.h>

#include <array>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "lubancore/packages.hpp"

namespace {
namespace pkg = lubancore::packages::v1;
std::string Minimal() {
    return "schema: 1\nid: example.root\nversion: 1.2.3-rc.1+build\nname: Root\ndescription: Manifest only\n";
}
std::string Constrained() {
    return Minimal() + "compatibility:\n  lubancode: '>=1.0.0-9223372036854775808 <2.0.0'\n  platforms: [linux, macos]\n";
}
void Rejected(pkg::Input input, const char* code) {
    const auto result = pkg::AnalyzeManifest(std::move(input));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
}
} // namespace

TEST_CASE("Package public analysis owns all values after input destruction") {
    pkg::Analysis saved;
    {
        pkg::Input input{Minimal() + "authors:\n  - name: Author\n    url: https://author.example\nlicense: MIT\nhomepage: https://home.example\nrepository: https://repo.example\n"};
        const auto result = pkg::AnalyzeManifest(input);
        REQUIRE(result.has_value());
        saved = *result;
        input.yaml.assign(input.yaml.size(), 'x');
    }
    REQUIRE(saved.manifest.has_value());
    const auto& manifest = *saved.manifest;
    CHECK(manifest.schema == 1);
    CHECK(manifest.id == "example.root");
    CHECK(manifest.version == "1.2.3-rc.1+build");
    CHECK(manifest.name == "Root");
    CHECK(manifest.description == "Manifest only");
    REQUIRE(manifest.authors.size() == 1);
    CHECK(manifest.authors.front().name == "Author");
    CHECK(manifest.authors.front().url == "https://author.example");
    CHECK(manifest.license == "MIT");
    CHECK(manifest.homepage == "https://home.example");
    CHECK(manifest.repository == "https://repo.example");
    CHECK(saved.input_sha256.size() == 64);
    const auto again = pkg::AnalyzeManifest({Minimal() + "authors:\n  - name: Author\n    url: https://author.example\nlicense: MIT\nhomepage: https://home.example\nrepository: https://repo.example\n"});
    REQUIRE(again.has_value());
    CHECK(saved.input_sha256 == again->input_sha256);
}

TEST_CASE("Package public parser failures retain field line and undecided compatibility") {
    const auto result = pkg::AnalyzeManifest({"schema: 1\nid: a.b\nversion: 0.1.0\nname: n\nextra: 1"});
    REQUIRE(result.has_value());
    CHECK_FALSE(result->manifest.has_value());
    REQUIRE(result->issues.size() == 1);
    CHECK(result->issues.front().field == "extra");
    CHECK(result->issues.front().line == 5);
    CHECK_FALSE(result->issues.front().detail.empty());
    CHECK(result->version_compatibility == pkg::Compatibility::NotEvaluated);
    CHECK(result->platform_compatibility == pkg::Compatibility::NotEvaluated);
    CHECK(result->input_sha256.size() == 64);
    CHECK(result->input_sha256 == "a3a9fdd3726e61172a83eb5eaededdc0a212c2056dc3baef1a825c3c1d3dc096");
    for (const auto& yaml : std::array<std::string, 3>{"", "id: [unclosed", "- item\n"}) {
        const auto failed = pkg::AnalyzeManifest({yaml});
        REQUIRE(failed.has_value());
        CHECK_FALSE(failed->manifest.has_value());
        REQUIRE(failed->issues.size() == 1);
        CHECK(failed->issues.front().field == "(yaml)");
    }
}

TEST_CASE("Package public analysis keeps original scalar and Unicode acceptance") {
    const auto scalar = pkg::AnalyzeManifest({"schema: 1\nid: scalar.root\nversion: 1.0.0\nname: true\ndescription: 42\nlicense: false\n"});
    REQUIRE(scalar.has_value());
    REQUIRE(scalar->manifest.has_value());
    CHECK(scalar->manifest->name == "true");
    CHECK(scalar->manifest->description == "42");
    CHECK(scalar->manifest->license == "false");
    const auto unicode = pkg::AnalyzeManifest({Minimal() + "authors:\n  - name: '作者🌙'\n"});
    REQUIRE(unicode.has_value());
    REQUIRE(unicode->manifest.has_value());
    REQUIRE(unicode->manifest->authors.size() == 1);
    CHECK(unicode->manifest->authors.front().name == "作者🌙");
}

TEST_CASE("Package public compatibility distinguishes absent and unevaluated constraints") {
    const auto plain = pkg::AnalyzeManifest({Minimal()});
    REQUIRE(plain.has_value());
    REQUIRE(plain->manifest.has_value());
    CHECK(plain->version_compatibility == pkg::Compatibility::NotDeclared);
    CHECK(plain->platform_compatibility == pkg::Compatibility::NotDeclared);
    const auto constrained = pkg::AnalyzeManifest({Constrained()});
    REQUIRE(constrained.has_value());
    REQUIRE(constrained->manifest.has_value());
    CHECK(constrained->version_compatibility == pkg::Compatibility::NotEvaluated);
    CHECK(constrained->platform_compatibility == pkg::Compatibility::NotEvaluated);
    REQUIRE(constrained->manifest->compatibility_lubancode.has_value());
    CHECK(*constrained->manifest->compatibility_lubancode == ">=1.0.0-9223372036854775808 <2.0.0");
    const std::vector<std::string> platforms{"linux", "macos"};
    CHECK(constrained->manifest->compatibility_platforms == platforms);
    const auto empty_platforms = pkg::AnalyzeManifest({Minimal() + "compatibility:\n  platforms: []\n", {}, "windows"});
    REQUIRE(empty_platforms.has_value());
    REQUIRE(empty_platforms->manifest.has_value());
    CHECK(empty_platforms->platform_compatibility == pkg::Compatibility::NotDeclared);
}

TEST_CASE("Package public explicit matching uses the original unbounded numeric prerelease comparator") {
    const auto accepted = pkg::AnalyzeManifest({Constrained(), "1.0.0-9223372036854775809", "linux"});
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->manifest.has_value());
    CHECK(accepted->version_compatibility == pkg::Compatibility::Compatible);
    CHECK(accepted->platform_compatibility == pkg::Compatibility::Compatible);
    const auto denied = pkg::AnalyzeManifest({Constrained(), "1.0.0-9223372036854775807", "windows"});
    REQUIRE(denied.has_value());
    CHECK(denied->version_compatibility == pkg::Compatibility::Incompatible);
    CHECK(denied->platform_compatibility == pkg::Compatibility::Incompatible);
    const auto equal = pkg::AnalyzeManifest({Minimal() + "compatibility:\n  lubancode: '=1.0.0-999999999999999999999999999999'\n", "1.0.0-999999999999999999999999999999+different"});
    REQUIRE(equal.has_value());
    CHECK(equal->version_compatibility == pkg::Compatibility::Compatible);
}

TEST_CASE("Package public input and explicit host options reject unsafe bytes") {
    Rejected({Minimal() + std::string(1, '\0')}, "sdk.package.invalid_input");
    Rejected({Minimal() + std::string(1, static_cast<char>(0xff))}, "sdk.package.invalid_input");
    for (const auto& version : std::array<std::string, 5>{"", "1.2", std::string("1.0.0\0x", 7), std::string(1, static_cast<char>(0xff)), std::string(pkg::kMaxHostVersionBytes + 1, '1')})
        Rejected({Minimal(), version}, "sdk.package.invalid_host");
    for (const auto& platform : std::array<std::string, 5>{"", "Linux", "windows95", std::string("linux\0", 6), std::string(pkg::kMaxPlatformBytes + 1, 'x')})
        Rejected({Minimal(), {}, platform}, "sdk.package.invalid_host");
}

TEST_CASE("Package public fixed byte caps include exact boundaries") {
    auto yaml = Minimal();
    yaml += "#";
    yaml.resize(pkg::kMaxManifestBytes, 'x');
    const auto exact = pkg::AnalyzeManifest({yaml});
    REQUIRE(exact.has_value());
    REQUIRE(exact->manifest.has_value());
    yaml.push_back('x');
    Rejected({yaml}, "sdk.package.invalid_input");
    std::string version = "1.0.0-";
    version.resize(pkg::kMaxHostVersionBytes, '9');
    const auto host_boundary = pkg::AnalyzeManifest({Minimal(), version, "macos"});
    REQUIRE(host_boundary.has_value());
    REQUIRE(host_boundary->manifest.has_value());
    version.push_back('9');
    Rejected({Minimal(), version}, "sdk.package.invalid_host");
}

TEST_CASE("Package public analyses keep concurrent owned results apart") {
    auto left = std::async(std::launch::async, [] { return pkg::AnalyzeManifest({Constrained(), "1.0.0-9223372036854775809", "linux"}); });
    auto right = std::async(std::launch::async, [] { return pkg::AnalyzeManifest({Minimal() + "unknown: true\n", {}, "windows"}); });
    const auto good = left.get();
    const auto bad = right.get();
    REQUIRE(good.has_value());
    REQUIRE(bad.has_value());
    REQUIRE(good->manifest.has_value());
    CHECK(good->version_compatibility == pkg::Compatibility::Compatible);
    CHECK(good->issues.empty());
    CHECK_FALSE(bad->manifest.has_value());
    REQUIRE(bad->issues.size() == 1);
    CHECK(bad->issues.front().field == "unknown");
    CHECK(bad->version_compatibility == pkg::Compatibility::NotEvaluated);
    CHECK(good->input_sha256 != bad->input_sha256);
}
