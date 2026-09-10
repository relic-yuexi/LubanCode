// hook 包目录发现实现(LuaHook 单 P0-B,§三)。
#include "hooks/middleware_loader.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <system_error>

namespace lubancode::hooks::middleware {

namespace {

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
        return std::nullopt;
    }
    return content;
}

// entry 解析到包内:拒绝绝对路径与越界(../ 逃出包根),不认当前工作目录。
std::expected<std::filesystem::path, std::string> ResolveEntryWithinPackage(
    const std::filesystem::path& package_root, const std::string& entry) {
    if (entry.empty()) {
        return std::unexpected("entry 为空");
    }
    const std::filesystem::path entry_path(entry);
    if (entry_path.is_absolute()) {
        return std::unexpected("entry 须是包内相对路径,不收绝对路径: " + entry);
    }
    std::filesystem::path resolved = package_root;
    for (const auto& part : entry_path) {
        const std::string name = part.string();
        if (name == "..") {
            return std::unexpected("entry 越出包根: " + entry);
        }
        // "." 段跳过;其余正常拼接(分隔符段为空也跳过)。
        if (name == "." || name.empty() || name == "/") {
            continue;
        }
        resolved /= part;
    }
    return resolved;
}

}  // namespace

std::expected<void, std::string> LoadHookPackage(MiddlewarePool& pool,
                                                 const std::filesystem::path& package_dir, SourceLayer layer,
                                                 const std::string& source_label) {
    const std::filesystem::path manifest_path = package_dir / "hook.json";
    const auto manifest_text = ReadTextFile(manifest_path);
    if (!manifest_text.has_value()) {
        return std::unexpected("hook.json 读不到: " + manifest_path.string());
    }
    nlohmann::json manifest = nlohmann::json::parse(*manifest_text, nullptr, /*allow_exceptions=*/false);
    if (manifest.is_discarded()) {
        return std::unexpected("hook.json 不是合法 JSON: " + manifest_path.string());
    }
    const std::string entry = manifest.value("entry", std::string());
    if (entry.empty()) {
        return std::unexpected("清单缺 entry 字段: " + manifest_path.string());
    }
    auto entry_path = ResolveEntryWithinPackage(package_dir, entry);
    if (!entry_path.has_value()) {
        return std::unexpected(entry_path.error());
    }
    const auto script = ReadTextFile(*entry_path);
    if (!script.has_value()) {
        return std::unexpected("entry 脚本读不到: " + entry_path->string());
    }
    auto added = pool.AddManifest(manifest, layer, source_label, *script);
    if (!added.has_value()) {
        return std::unexpected(added.error().code + ": " + added.error().message);
    }
    return {};
}

HookPackageLoadReport LoadHookPackages(MiddlewarePool& pool, const std::filesystem::path& hooks_root,
                                       SourceLayer layer, const std::string& source_label) {
    HookPackageLoadReport report;
    std::error_code ec;
    if (!std::filesystem::exists(hooks_root, ec)) {
        return report;  // 没配 hooks 的常态:零包零错
    }
    // 稳定序:目录名排序,装载结果不随文件系统枚举序漂。
    std::vector<std::filesystem::path> packages;
    for (const auto& entry : std::filesystem::directory_iterator(hooks_root, ec)) {
        if (entry.is_directory()) {
            packages.push_back(entry.path());
        }
    }
    if (ec) {
        report.errors.push_back("hooks 目录扫描失败: " + hooks_root.string() + " (" + ec.message() + ")");
        return report;
    }
    std::sort(packages.begin(), packages.end());
    for (const auto& package_dir : packages) {
        // 目录发现仅读清单:没有 hook.json 的目录不是 hook 包,静默跳过
        //(fixtures/ 等随包资料不装)。
        std::error_code exists_ec;
        if (!std::filesystem::exists(package_dir / "hook.json", exists_ec)) {
            continue;
        }
        auto loaded = LoadHookPackage(pool, package_dir, layer, source_label);
        if (loaded.has_value()) {
            ++report.packages_loaded;
        } else {
            report.errors.push_back("[" + package_dir.filename().string() + "] " + loaded.error());
        }
    }
    return report;
}

}  // namespace lubancode::hooks::middleware
