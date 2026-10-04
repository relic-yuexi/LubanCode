#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#endif

#include "lubancore/packages.hpp"
#include "package/inventory.hpp"
#include "package/inventory_snapshot.hpp"

namespace {
namespace pkg = lubancore::packages::v1;
namespace native = lubancode::package;
namespace fs = std::filesystem;

std::string Utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
fs::path Path(const std::string& text) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto candidate = fs::temp_directory_path() / ("sdk-package-inventory-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
            if (fs::create_directory(candidate)) { root = fs::canonical(candidate); break; }
        }
        REQUIRE_FALSE(root.empty());
    }
    ~Directory() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path Make(const char* name) const {
        const auto path = root / name;
        REQUIRE(fs::create_directory(path));
        return path;
    }
};
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    REQUIRE_FALSE(file.fail());
}
std::string Manifest() {
    return "schema: 1\nid: example.bundle\nversion: 1.2.3\nname: Bundle\ndescription: Inventory fixture\n";
}
pkg::InventoryInput Input(const fs::path& root) { return {Utf8(root)}; }
pkg::Inventory Take(lubancore::Result<pkg::Inventory> result) {
    const auto error = result ? std::string{} : result.error().code + ": " + result.error().message;
    REQUIRE_MESSAGE(result.has_value(), error);
    return std::move(*result);
}
void Rejected(pkg::InventoryInput input, const char* code) {
    const auto result = pkg::InventoryExplicitRoot(std::move(input));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
}
const pkg::InventoryFile& File(const pkg::Inventory& inventory, const std::string& path) {
    const auto found = std::find_if(inventory.files.begin(), inventory.files.end(), [&](const auto& file) { return file.relative_path == path; });
    REQUIRE(found != inventory.files.end());
    return *found;
}
bool Issue(const pkg::Inventory& inventory, const char* code) {
    return std::any_of(inventory.issues.begin(), inventory.issues.end(), [&](const auto& issue) { return issue.code == code; });
}
native::InventorySnapshotLimits NativeLimits(pkg::InventoryLimits limits = {}) {
    return {limits.entries, limits.depth, limits.path_bytes, limits.file_bytes, limits.total_bytes, pkg::kMaxManifestBytes};
}
void Full(const fs::path& root) {
    Write(root / "package.yaml", Manifest());
    Write(root / "agents" / "alpha.yaml", "not parsed as an agent\n");
    Write(root / "prompts" / "profiles" / "alpha" / "core" / "00.md", "profile");
    Write(root / "skills" / "alpha" / "SKILL.md", "not parsed as a skill\n");
    Write(root / "workflows" / "alpha" / "workflow.yaml", "not parsed as a workflow\n");
    Write(root / "plugins" / "alpha" / "plugin.json", "not parsed as a plugin\n");
    Write(root / "mcp" / "alpha" / "mcp.yaml", "not parsed as MCP\n");
    Write(root / "channels" / "alpha" / "channel.yaml", "not parsed as a channel\n");
    Write(root / "assets" / Path("月🌙.bin"), std::string("\0\xff", 2));
    Write(root / "docs" / "usage.md", "usage");
    Write(root / "README.md", "readme");
    Write(root / "empty", "");
}
void LinkDirectory(const fs::path& link, const fs::path& target) {
#ifdef _WIN32
    // Mount-point reparse data creates a real junction without symlink privilege.
    REQUIRE(fs::create_directory(link));
    struct Junction {
        DWORD tag;
        WORD data_length, reserved;
        WORD substitute_offset, substitute_length, print_offset, print_length;
        WCHAR path[2048];
    } data{};
    static_assert(offsetof(Junction, path) == 16);
    const auto print = target.native();
    const auto substitute = std::wstring(L"\\??\\") + print;
    const auto text = substitute + L'\0' + print + L'\0';
    REQUIRE(text.size() < std::size(data.path));
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.data_length = static_cast<WORD>(8 + text.size() * sizeof(WCHAR));
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(WCHAR));
    data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(WCHAR));
    data.print_length = static_cast<WORD>(print.size() * sizeof(WCHAR));
    std::copy(text.begin(), text.end(), data.path);
    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{handle};
    DWORD returned = 0;
    REQUIRE(DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(8 + data.data_length), nullptr, 0, &returned, nullptr) != FALSE);
    const auto attributes = GetFileAttributesW(link.c_str());
    REQUIRE(attributes != INVALID_FILE_ATTRIBUTES);
    REQUIRE((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
#else
    fs::create_directory_symlink(target, link);
    REQUIRE(fs::is_symlink(fs::symlink_status(link)));
#endif
}
struct Mutation final : native::InventoryObservation {
    native::InventoryObservationPoint point;
    std::function<void()> change;
    unsigned calls = 0, changes = 0;
    Mutation(native::InventoryObservationPoint point_value, std::function<void()> change_value)
        : point(point_value), change(std::move(change_value)) {}
    void After(native::InventoryObservationPoint actual, const fs::path&, std::string_view) override {
        ++calls;
        if (actual == point && changes == 0) { ++changes; change(); }
    }
};
} // namespace

TEST_CASE("Package inventory owns seven discovered entry kinds after source deletion") {
    Directory directory;
    const auto root = directory.Make("bundle");
    Full(root);
    auto input = Input(root);
    const auto saved = Take(pkg::InventoryExplicitRoot(input));
    input.root.assign(input.root.size(), 'x');
    REQUIRE(saved.manifest_analysis.has_value());
    REQUIRE(saved.manifest_analysis->manifest.has_value());
    CHECK(saved.root == Utf8(root));
    CHECK(saved.canonical_root == Utf8(fs::canonical(root)));
    CHECK(saved.manifest_analysis->manifest->id == "example.bundle");
    REQUIRE(saved.components.size() == 7);
    const std::array<pkg::ComponentKind, 7> kinds{pkg::ComponentKind::Agent, pkg::ComponentKind::PromptProfile,
        pkg::ComponentKind::Skill, pkg::ComponentKind::Workflow, pkg::ComponentKind::Plugin,
        pkg::ComponentKind::McpServer, pkg::ComponentKind::Channel};
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        CHECK(saved.components[i].kind == kinds[i]);
        CHECK(saved.components[i].local_id == "alpha");
        REQUIRE(saved.components[i].canonical_id.has_value());
        CHECK(*saved.components[i].canonical_id == "example.bundle:alpha");
    }
    REQUIRE(saved.files.size() == 12);
    CHECK(File(saved, "empty").bytes == 0);
    CHECK(File(saved, "empty").sha256 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(File(saved, "assets/月🌙.bin").bytes == 2);
    CHECK(File(saved, "assets/月🌙.bin").sha256 == "06eb7d6a69ee19e5fbdf749018d3d2abfa04bcbd1365db312eb86dc7169389b8");
    CHECK(File(saved, "plugins/alpha/plugin.json").code_bearing);
    CHECK(File(saved, "mcp/alpha/mcp.yaml").code_bearing);
    CHECK(File(saved, "channels/alpha/channel.yaml").code_bearing);
    CHECK_FALSE(File(saved, "assets/月🌙.bin").code_bearing);
    CHECK(std::is_sorted(saved.files.begin(), saved.files.end(), [](const auto& a, const auto& b) { return a.relative_path < b.relative_path; }));
    CHECK(std::is_sorted(saved.directories.begin(), saved.directories.end()));
    std::size_t bytes = 0;
    for (const auto& file : saved.files) bytes += file.bytes;
    CHECK(saved.total_bytes == bytes);
    REQUIRE(fs::remove_all(root) > 0);
    CHECK_FALSE(fs::exists(root));
    CHECK(saved.components[0].relative_path == "agents/alpha.yaml");
    CHECK(saved.content_sha256.size() == 64);
    CHECK(File(saved, "empty").sha256.size() == 64);
    std::cout << "[sdk-package-inventory-path] owned\n";
}

TEST_CASE("Package inventory hashes actual sorted bytes with the frozen CLI material") {
    Directory directory;
    const auto root = directory.Make("first");
    Write(root / "z.txt", "z");
    Write(root / "package.yaml", Manifest());
    Write(root / "assets" / "data.bin", std::string("\0\xff", 2));
    const auto before = Take(pkg::InventoryExplicitRoot(Input(root)));
    CHECK(before.content_sha256 == "15f945a7e7e998dff5bb8268bdc096602ac2dcd28966caa7ef592f06fd421b6b");
    CHECK(before.total_bytes == 91);
    native::PackageCandidate candidate;
    candidate.package_root = root;
    candidate.dir_name = "first";
    const auto cli = native::BuildPackageInventory(candidate);
    CHECK(cli.content_hash == before.content_sha256);
    CHECK(cli.total_file_count == before.files.size());
    const auto second = directory.Make("second");
    Write(second / "assets" / "data.bin", std::string("\0\xff", 2));
    Write(second / "package.yaml", Manifest());
    Write(second / "z.txt", "z");
    CHECK(Take(pkg::InventoryExplicitRoot(Input(second))).content_sha256 == before.content_sha256);
    Write(root / "z.txt", "Z");
    const auto modified = Take(pkg::InventoryExplicitRoot(Input(root)));
    CHECK(modified.content_sha256 != before.content_sha256);
    fs::rename(root / "z.txt", root / "renamed.txt");
    CHECK(Take(pkg::InventoryExplicitRoot(Input(root))).content_sha256 != modified.content_sha256);
    CHECK(before.files.back().relative_path == "z.txt");
    CHECK(File(before, "z.txt").bytes == 1);
    std::cout << "[sdk-package-inventory-path] fingerprint\n";
}

TEST_CASE("Package inventory distinguishes missing empty invalid manifests and unparsed entry shapes") {
    Directory directory;
    const auto root = directory.Make("bundle");
    Write(root / "skills" / "not-valid" / "SKILL.md", "invalid frontmatter, still discovered");
    fs::create_directories(root / "skills" / "wrong-type" / "SKILL.md");
    Write(root / "agents", "wrong top-level type");
    fs::create_directory(root / "skill");
    auto missing = Take(pkg::InventoryExplicitRoot(Input(root)));
    CHECK_FALSE(missing.manifest_analysis.has_value());
    CHECK(Issue(missing, "missing_manifest"));
    CHECK(Issue(missing, "missing_component_entry"));
    CHECK(Issue(missing, "invalid_top_level_type"));
    CHECK(Issue(missing, "near_miss_directory"));
    REQUIRE(missing.components.size() == 1);
    CHECK_FALSE(missing.components[0].canonical_id.has_value());
    Write(root / "package.yaml", "");
    const auto empty = Take(pkg::InventoryExplicitRoot(Input(root)));
    REQUIRE(empty.manifest_analysis.has_value());
    CHECK_FALSE(empty.manifest_analysis->manifest.has_value());
    CHECK_FALSE(empty.manifest_analysis->issues.empty());
    CHECK(empty.manifest_analysis->input_sha256 == File(empty, "package.yaml").sha256);
    Write(root / "package.yaml", Manifest() + "extra: 1\n");
    const auto invalid = Take(pkg::InventoryExplicitRoot(Input(root)));
    REQUIRE(invalid.manifest_analysis.has_value());
    REQUIRE(invalid.manifest_analysis->issues.size() == 1);
    CHECK(invalid.manifest_analysis->issues[0].field == "extra");
    CHECK(invalid.manifest_analysis->issues[0].line == 6);
    CHECK_FALSE(invalid.components[0].canonical_id.has_value());
    Write(root / "package.yaml", Manifest() + "compatibility:\n  lubancode: '>=1.0.0 <2.0.0'\n  platforms: [linux]\n");
    const auto omitted = Take(pkg::InventoryExplicitRoot(Input(root)));
    REQUIRE(omitted.manifest_analysis.has_value());
    CHECK(omitted.manifest_analysis->version_compatibility == pkg::Compatibility::NotEvaluated);
    CHECK(omitted.manifest_analysis->platform_compatibility == pkg::Compatibility::NotEvaluated);
    auto input = Input(root); input.lubancode_version = "1.2.3"; input.platform = "linux";
    const auto compatible = Take(pkg::InventoryExplicitRoot(input));
    CHECK(compatible.manifest_analysis->version_compatibility == pkg::Compatibility::Compatible);
    CHECK(compatible.manifest_analysis->platform_compatibility == pkg::Compatibility::Compatible);
    input.lubancode_version = "2.0.0"; input.platform = "windows";
    const auto incompatible = Take(pkg::InventoryExplicitRoot(input));
    CHECK(incompatible.manifest_analysis->version_compatibility == pkg::Compatibility::Incompatible);
    CHECK(incompatible.manifest_analysis->platform_compatibility == pkg::Compatibility::Incompatible);
    Write(root / "package.yaml", Manifest() + std::string(1, '\0'));
    Rejected(Input(root), "sdk.package.invalid_input");
    std::cout << "[sdk-package-inventory-path] manifest\n";
}

TEST_CASE("Package inventory rejects unsafe explicit roots hosts and limit declarations") {
    Directory directory;
    const auto root = directory.Make("bundle");
    for (const auto& path : std::array<std::string, 5>{"", "relative", Utf8(root / ".." / "bundle"), Utf8(root) + std::string(1, '\0'), std::string(1, static_cast<char>(0xff))})
        Rejected({path}, "sdk.package.invalid_input");
    Rejected(Input(root / "missing"), "sdk.package.read_failed");
    auto host = Input(root / "missing"); host.lubancode_version = "1.2";
    Rejected(host, "sdk.package.invalid_host");
    host.lubancode_version = "1.2.3"; host.platform = "Linux";
    Rejected(host, "sdk.package.invalid_host");
    using Member = std::size_t pkg::InventoryLimits::*;
    const std::array<Member, 5> members{&pkg::InventoryLimits::entries, &pkg::InventoryLimits::depth,
        &pkg::InventoryLimits::path_bytes, &pkg::InventoryLimits::file_bytes, &pkg::InventoryLimits::total_bytes};
    for (const auto member : members) {
        auto input = Input(root);
        input.limits.*member = 0;
        Rejected(input, "sdk.package.invalid_limits");
        input = Input(root);
        ++(input.limits.*member);
        Rejected(input, "sdk.package.invalid_limits");
    }
    const pkg::InventoryLimits defaults;
    CHECK(defaults.entries == 4096);
    CHECK(defaults.depth == 32);
    CHECK(defaults.path_bytes == 4096);
    CHECK(defaults.file_bytes == 16 * 1024 * 1024);
    CHECK(defaults.total_bytes == 64 * 1024 * 1024);
#ifndef _WIN32
    for (const auto& name : std::array<std::string, 4>{"bad\tname", "bad\nname", "bad\\name", "bad" + std::string(1, static_cast<char>(0xff))}) {
        Write(root / name, "");
        Rejected(Input(root), "sdk.package.invalid_input");
        REQUIRE(fs::remove(root / name));
    }
#endif
    CHECK(fs::is_empty(root));
    std::cout << "[sdk-package-inventory-path] input\n";
}

TEST_CASE("Package inventory enforces actual entry depth path file total and manifest byte boundaries") {
    Directory directory;
    const auto root = directory.Make("limits");
    Write(root / "a", "1234"); Write(root / "b", "5678"); Write(root / "empty", "");
    fs::create_directory(root / "directory");
    auto input = Input(root); input.limits.entries = 4; input.limits.depth = 1; input.limits.file_bytes = 4; input.limits.total_bytes = 8;
    const auto exact = Take(pkg::InventoryExplicitRoot(input));
    CHECK(exact.files.size() == 3);
    CHECK(exact.directories.size() == 1);
    CHECK(exact.total_bytes == 8);
    auto below = input; below.limits.entries = 3; Rejected(below, "sdk.package.limit_exceeded");
    below = input; below.limits.file_bytes = 3; Rejected(below, "sdk.package.limit_exceeded");
    below = input; below.limits.total_bytes = 7; Rejected(below, "sdk.package.limit_exceeded");
    fs::create_directory(root / "directory" / "nested");
    below = input; below.limits.entries = 5; Rejected(below, "sdk.package.limit_exceeded");
    below.limits.depth = 2; CHECK(Take(pkg::InventoryExplicitRoot(below)).directories.size() == 2);
    const auto paths = directory.Make("paths");
    const std::string name(Utf8(paths).size() + 1, 'p');
    REQUIRE(name.size() < 240);
    Write(paths / name, "");
    auto path_input = Input(paths); path_input.limits.path_bytes = name.size();
    CHECK(Take(pkg::InventoryExplicitRoot(path_input)).files[0].relative_path == name);
    fs::rename(paths / name, paths / (name + "p"));
    Rejected(path_input, "sdk.package.limit_exceeded");
    const auto manifests = directory.Make("manifest-limit");
    auto yaml = Manifest() + "#"; yaml.resize(pkg::kMaxManifestBytes, 'x');
    Write(manifests / "package.yaml", yaml);
    CHECK(Take(pkg::InventoryExplicitRoot(Input(manifests))).manifest_analysis->manifest.has_value());
    yaml.push_back('x'); Write(manifests / "package.yaml", yaml);
    Rejected(Input(manifests), "sdk.package.limit_exceeded");
    const auto moved = directory.root / "released-limit-tree";
    fs::rename(root, moved);
    REQUIRE(fs::remove_all(moved) > 0);
    std::cout << "[sdk-package-inventory-path] limits\n";
}

TEST_CASE("Package inventory rejects real links junction roots and nonregular entries without traversal") {
    Directory directory;
    const auto root = directory.Make("bundle");
    const auto outside = directory.Make("outside");
    Write(outside / "secret", "must not traverse");
    LinkDirectory(root / "linked", outside);
    Rejected(Input(root), "sdk.package.linked_source");
    REQUIRE(fs::remove(root / "linked"));
    LinkDirectory(directory.root / "linked-root", root);
    Rejected(Input(directory.root / "linked-root"), "sdk.package.linked_source");
    Rejected(Input((directory.root / "linked-root") / fs::path{}), "sdk.package.linked_source");
    REQUIRE(fs::remove(directory.root / "linked-root"));
    CHECK(Take(pkg::InventoryExplicitRoot(Input(root / fs::path{}))).canonical_root == Utf8(fs::canonical(root)));
#ifndef _WIN32
    fs::create_symlink(outside / "secret", root / "package.yaml");
    REQUIRE(fs::is_symlink(fs::symlink_status(root / "package.yaml")));
    Rejected(Input(root), "sdk.package.linked_source");
    REQUIRE(fs::remove(root / "package.yaml"));
    REQUIRE(::mkfifo((root / "pipe").c_str(), 0600) == 0);
    Rejected(Input(root), "sdk.package.nonregular_source");
    REQUIRE(fs::remove(root / "pipe"));
#endif
    CHECK(fs::is_empty(root));
    CHECK(fs::file_size(outside / "secret") == 17);
    const auto after = Take(pkg::InventoryExplicitRoot(Input(root)));
    CHECK(after.files.empty());
    CHECK(after.directories.empty());
    CHECK(after.content_sha256.size() == 64);
    std::cout << "[sdk-package-inventory-path] links\n";
}

TEST_CASE("Package inventory detects actual tree changes at internal capture observation points") {
    Directory directory;
    using Point = native::InventoryObservationPoint;
    for (unsigned mode = 0; mode < 7; ++mode) {
        const auto root = directory.Make(("change-" + std::to_string(mode)).c_str());
        Write(root / "data", "12");
        const auto public_before = Take(pkg::InventoryExplicitRoot(Input(root)));
        Mutation steady(Point::AfterEnumeration, [] {});
        const auto observed = native::CaptureInventoryRoot(root, NativeLimits(), &steady);
        REQUIRE(observed.has_value());
        CHECK(observed->content_sha256 == public_before.content_sha256);
        CHECK(steady.changes == 1);
        CHECK(steady.calls == 3);
        const auto point = mode == 5 ? Point::AfterFileRead : mode == 6 ? Point::BeforeFinalEnumeration : Point::AfterEnumeration;
        Mutation mutation(point, [&] {
            if (mode == 0) Write(root / "new", "new");
            else if (mode == 1) REQUIRE(fs::remove(root / "data"));
            else if (mode == 2) fs::rename(root / "data", root / "renamed");
            else if (mode == 3) Write(root / "data", "grown");
            else if (mode == 4) {
                const auto before = fs::last_write_time(root / "data");
                Write(root / "data", "XX");
                fs::last_write_time(root / "data", before + std::chrono::seconds(2));
            } else if (mode == 5) Write(root / "new-after-read", "new");
            else fs::rename(root, directory.root / "renamed-root");
        });
        const auto changed = native::CaptureInventoryRoot(root, NativeLimits(), &mutation);
        REQUIRE_FALSE(changed.has_value());
        CHECK(changed.error().code == "sdk.package.source_changed");
        CHECK(mutation.changes == 1);
        CHECK_FALSE(changed.error().detail.empty());
    }
    std::cout << "[sdk-package-inventory-path] changed\n";
}

TEST_CASE("Package inventory keeps concurrent roots values and failed-read resources apart") {
    Directory directory;
    const auto left = directory.Make("left"), right = directory.Make("right");
    Write(left / "package.yaml", Manifest());
    Write(left / "assets" / "a", "left");
    Write(right / "package.yaml", Manifest() + "extra: 1\n");
    Write(right / "assets" / "b", "right");
    auto first = std::async(std::launch::async, [&] { return pkg::InventoryExplicitRoot(Input(left)); });
    auto second = std::async(std::launch::async, [&] { return pkg::InventoryExplicitRoot(Input(right)); });
    const auto a = Take(first.get()), b = Take(second.get());
    REQUIRE(a.manifest_analysis.has_value());
    REQUIRE(b.manifest_analysis.has_value());
    CHECK(a.manifest_analysis->manifest.has_value());
    CHECK_FALSE(b.manifest_analysis->manifest.has_value());
    CHECK(a.content_sha256 != b.content_sha256);
    CHECK(File(a, "assets/a").bytes == 4);
    CHECK(File(b, "assets/b").bytes == 5);
    auto limited = Input(left); limited.limits.total_bytes = 1;
    Rejected(limited, "sdk.package.limit_exceeded");
    CHECK(Take(pkg::InventoryExplicitRoot(Input(left))).content_sha256 == a.content_sha256);
    REQUIRE(fs::remove_all(left) > 0);
    REQUIRE(fs::remove_all(right) > 0);
    CHECK(a.root == Utf8(left));
    CHECK(b.root == Utf8(right));
    CHECK(File(a, "assets/a").sha256.size() == 64);
    CHECK(File(b, "assets/b").sha256.size() == 64);
    std::cout << "[sdk-package-inventory-path] isolation\n";
}
