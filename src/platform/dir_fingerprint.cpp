// 目录内容指纹共用底座的实现(从 runtime/plugin_tool.cpp 的
// ComputePluginContentHash 与 package/inventory.cpp 的盘点材料各抽一半,
// 行为一字不动——两本信任账上的旧指纹都不许失效)。
#include "platform/dir_fingerprint.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <system_error>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"

namespace lubancode::platform {

namespace {

// 空文件与读不动必须分得开:前者是合法内容(sha256 of ""),后者是信任
// 材料的窟窿。返回 nullopt = 打不开/不是常规文件。
std::optional<std::string> ReadFileBytes(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string RelUtf8Native(const std::filesystem::path& root, const std::filesystem::path& file) {
    std::error_code ec;
    // 枚举自 root,前缀必然匹配,relative() 不会走偏;分隔符按平台原生
    // 渲染(Plugin v1 冻结的口径)。
    return PathToUtf8(std::filesystem::relative(file, root, ec));
}

}  // namespace

std::expected<std::vector<std::string>, std::string> ListRegularFilesUtf8Sorted(
    const std::filesystem::path& dir, bool follow_symlinks) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return std::unexpected("目录不存在: " + PathToUtf8(dir));
    }
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::none, ec)) {
        if (ec) {
            break;
        }
        std::error_code file_ec;
        // follow_symlinks = true:按目标状态认(symlink 指到常规文件就收,
        // Plugin 旧账的规矩);false:先看链接本身,symlink 一律不收。
        if (follow_symlinks) {
            if (!entry.is_regular_file(file_ec) || file_ec) continue;
        } else {
            const std::filesystem::file_status status = entry.symlink_status(file_ec);
            if (file_ec || status.type() != std::filesystem::file_type::regular) continue;
        }
        files.push_back(RelUtf8Native(dir, entry.path()));
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::expected<std::string, std::string> PluginDirFingerprintV1(const std::filesystem::path& dir) {
    const auto files = ListRegularFilesUtf8Sorted(dir, /*follow_symlinks=*/true);
    if (!files.has_value()) {
        return std::unexpected("插件目录不存在: " + PathToUtf8(dir));
    }
    // 相对路径 + 字节全部喂进同一口锅:改名/改内容/添删文件都变指纹。
    std::string material;
    for (const std::string& rel : *files) {
        material += rel;
        material += '\0';
        const auto bytes = ReadFileBytes(dir / Utf8ToPath(rel));
        if (!bytes.has_value()) {
            return std::unexpected("读不到文件: " + PathToUtf8(dir / Utf8ToPath(rel)));
        }
        material += *bytes;
        material += '\0';
    }
    return hooks::Sha256Hex(material);
}

std::string PackageLedgerFingerprintV1(const std::vector<LedgerFile>& files) {
    std::ostringstream material;
    material << "luban-package-v1\n";
    for (const LedgerFile& file : files) {
        material << file.rel_utf8 << '\t' << file.size << '\t' << file.sha256 << '\n';
    }
    return hooks::Sha256Hex(material.str());
}

std::string FileSha256Hex(const std::filesystem::path& path) {
    const auto bytes = ReadFileBytes(path);
    if (!bytes.has_value()) {
        return std::string();  // 读不动:空串,调用方记账,这里不装看见
    }
    return hooks::Sha256Hex(*bytes);
}

}  // namespace lubancode::platform
