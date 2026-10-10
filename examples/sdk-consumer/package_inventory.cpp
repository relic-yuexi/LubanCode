#include "lubancore/packages.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace lubancore_consumer {
namespace {
namespace pkg = lubancore::packages::v1;
namespace fs = std::filesystem;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string Utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Check(file.is_open(), "cannot create consumer package fixture");
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    Check(!file.fail(), "cannot complete consumer package fixture");
}
pkg::Inventory Take(lubancore::Result<pkg::Inventory> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(*result);
}
} // namespace

void PackageInventory(const fs::path& base) {
    const auto root = base / "explicit-package";
    Check(fs::create_directory(root), "consumer package root must be fresh");
    struct Clean { fs::path root; ~Clean() { std::error_code ec; fs::remove_all(root, ec); } } clean{root};
    Write(root / "package.yaml", "schema: 1\nid: consumer.inventory\nversion: 1.0.0\nname: Inventory\ndescription: Installed host\ncompatibility:\n  platforms: [linux]\n");
    Write(root / "agents" / "demo.yaml", "unparsed agent");
    Write(root / "skills" / "audit" / "SKILL.md", "unparsed skill");
    Write(root / "plugins" / "tool" / "plugin.json", "unparsed plugin");
    Write(root / "assets" / "binary", std::string("\0\xff", 2));
    Write(root / "empty", "");
    fs::create_directories(root / "prompts" / "profiles" / "demo");
    pkg::Inventory saved;
    {
        pkg::InventoryInput input{Utf8(fs::canonical(root))};
        saved = Take(pkg::InventoryExplicitRoot(input));
        input.root.assign(input.root.size(), 'x');
    }
    Check(saved.manifest_analysis && saved.manifest_analysis->manifest, "owned consumer manifest did not parse");
    Check(saved.manifest_analysis->manifest->id == "consumer.inventory", "owned consumer identity changed");
    Check(saved.manifest_analysis->platform_compatibility == pkg::Compatibility::NotEvaluated, "inventory consulted the host platform");
    Check(saved.components.size() == 4 && saved.files.size() == 6, "consumer inventory did not discover the actual entry shapes");
    Check(saved.components[0].kind == pkg::ComponentKind::Agent && saved.components[1].kind == pkg::ComponentKind::PromptProfile,
          "consumer component order changed");
    Check(std::all_of(saved.components.begin(), saved.components.end(), [](const auto& component) { return component.canonical_id.has_value(); }),
          "consumer did not retain owned display identities");
    const auto empty = std::find_if(saved.files.begin(), saved.files.end(), [](const auto& file) { return file.relative_path == "empty"; });
    Check(empty != saved.files.end() && empty->bytes == 0 && empty->sha256 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "consumer empty file was discarded or hashed incorrectly");
    pkg::InventoryInput limited{Utf8(fs::canonical(root))}; limited.limits.total_bytes = 1;
    const auto rejected = pkg::InventoryExplicitRoot(limited);
    Check(!rejected && rejected.error().code == "sdk.package.limit_exceeded", "consumer total-byte cap did not reject real reads");
    Check(Take(pkg::InventoryExplicitRoot({Utf8(fs::canonical(root))})).content_sha256 == saved.content_sha256,
          "failed read changed a later observation");
    Check(fs::remove_all(root) > 0 && !fs::exists(root), "consumer package handles survived the call");
    Check(saved.manifest_analysis->manifest->id == "consumer.inventory" && saved.components[2].local_id == "audit" &&
          saved.content_sha256.size() == 64 && empty->sha256.size() == 64, "consumer owned values depended on deleted sources");
    std::cout << "[sdk-package-inventory-consumer] complete\n";
}
} // namespace lubancore_consumer
