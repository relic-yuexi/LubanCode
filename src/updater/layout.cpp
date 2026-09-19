// layout.hpp 的实现。语义对齐 scripts/updater.py(layout_paths/detect_layout/
// read_current/write_install_state),严格读口径对齐 src/app/launcher.cpp。
#include "updater/layout.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "updater/manifest.hpp"

namespace lubancode::updater {

namespace {

#ifdef _WIN32
constexpr const char* kExeName = "lubancode.exe";
#else
constexpr const char* kExeName = "lubancode";
#endif

// 读文件全文(二进制)。打不开/读失败返回 nullopt。
std::optional<std::string> ReadFileBytes(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return bytes;
}

// 拿到已 contains 校验过的字符串字段的值;类型不对给缺省。
std::string StringField(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || !json[key].is_string()) return std::string();
    return json[key].get<std::string>();
}

}  // namespace

// ---------------------------------------------------------------------------
// 布局与时间
// ---------------------------------------------------------------------------

LayoutPaths MakeLayoutPaths(const std::filesystem::path& root) {
    LayoutPaths paths;
    paths.root = root;
    paths.versions = root / "versions";
    paths.current = root / "current.json";
    paths.state = root / "install-state.json";
    paths.staging = root / "staging";
    paths.backups = root / "backups";
    paths.updates = root / "updates";
    paths.exe = root / kExeName;
    paths.updater = root / "updater";
    return paths;
}

LayoutKind DetectLayout(const std::filesystem::path& root) {
    const LayoutPaths paths = MakeLayoutPaths(root);
    std::error_code ec;
    if (std::filesystem::is_regular_file(paths.current, ec) && !ec) {
        return LayoutKind::Versioned;
    }
    ec.clear();
    if ((std::filesystem::is_regular_file(paths.exe, ec) && !ec) ||
        (std::filesystem::is_regular_file(paths.state, ec) && !ec)) {
        return LayoutKind::Flat;
    }
    return LayoutKind::Empty;
}

std::optional<nlohmann::json> ReadJsonFileTolerant(const std::filesystem::path& file) {
    const auto bytes = ReadFileBytes(file);
    if (!bytes.has_value()) return std::nullopt;
    std::string_view text = *bytes;
    // utf-8-sig 容错:剥开头 BOM(PowerShell 5.1 写文件爱带)。
    constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
    if (text.size() >= 3 && text.substr(0, 3) == kUtf8Bom) {
        text.remove_prefix(3);
    }
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) return std::nullopt;
    return parsed;
}

std::string UtcNowIso8601() {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

namespace {

std::string CallNow(const UtcNowFn& now) {
    return now ? now() : UtcNowIso8601();
}

// 落盘(原子 + ProcessCrashDurability);失败抛(账写不进是硬错)。
void AtomicWriteOrThrow(const std::filesystem::path& target, const std::string& bytes) {
    const auto result =
        platform::AtomicWriteFile(target, bytes, platform::WriteDurability::ProcessCrashDurability);
    if (!result.has_value()) {
        throw std::runtime_error("账落盘失败 " + platform::PathToUtf8(target) + ": " +
                                 result.error().code + " " + result.error().message);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// current.json
// ---------------------------------------------------------------------------

bool IsValidVersionDirname(std::string_view name) {
    return !name.empty() && name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos && name != "." && name != "..";
}

std::optional<CurrentPointer> ReadCurrent(const LayoutPaths& paths) {
    const auto data = ReadJsonFileTolerant(paths.current);
    if (!data.has_value() || !data->is_object()) return std::nullopt;
    // nlohmann const operator[] 查缺键是 UB:一律 contains()+is_* 先行。
    if (!data->contains("schema") || !(*data)["schema"].is_number_integer() ||
        (*data)["schema"].get<int>() != kCurrentSchema) {
        return std::nullopt;
    }
    if (!data->contains("current") || !(*data)["current"].is_string()) return std::nullopt;
    const std::string current = (*data)["current"].get<std::string>();
    // 目录名单段名,别的形状一律不信(launcher 严格口径)。
    if (!IsValidVersionDirname(current)) return std::nullopt;
    CurrentPointer pointer;
    pointer.current = current;
    if (data->contains("previous") && (*data)["previous"].is_string()) {
        const std::string previous = (*data)["previous"].get<std::string>();
        if (!previous.empty()) pointer.previous = previous;
    }
    return pointer;
}

void WriteCurrent(const LayoutPaths& paths, const std::string& current,
                  const std::optional<std::string>& previous, const std::string& transaction,
                  const UtcNowFn& now) {
    if (!IsValidVersionDirname(current)) {
        throw std::runtime_error("current 指针不是合格单段目录名: " + current);
    }
    if (previous.has_value() && !IsValidVersionDirname(*previous)) {
        throw std::runtime_error("previous 指针不是合格单段目录名: " + *previous);
    }
    nlohmann::json pointer = nlohmann::json::object();
    pointer["schema"] = kCurrentSchema;
    pointer["current"] = current;
    pointer["previous"] = previous.has_value() ? nlohmann::json(*previous) : nlohmann::json();
    pointer["updated_at_utc"] = CallNow(now);
    pointer["transaction"] = transaction;
    AtomicWriteOrThrow(paths.current, CanonicalJsonDump(pointer));
}

// ---------------------------------------------------------------------------
// install-state.json
// ---------------------------------------------------------------------------

std::optional<InstallState> ReadInstallState(const LayoutPaths& paths) {
    const auto data = ReadJsonFileTolerant(paths.state);
    if (!data.has_value() || !data->is_object()) return std::nullopt;
    if (!data->contains("schema") || !(*data)["schema"].is_number_integer() ||
        (*data)["schema"].get<int>() != kInstallStateSchema) {
        return std::nullopt;
    }
    InstallState state;
    state.installed_at_utc = StringField(*data, "installed_at_utc");
    state.version = StringField(*data, "version");
    state.channel = StringField(*data, "channel");
    state.installer = StringField(*data, "installer");
    state.transaction = StringField(*data, "transaction");
    if (data->contains("platform") && (*data)["platform"].is_string()) {
        state.platform = (*data)["platform"].get<std::string>();
    }
    if (data->contains("rolled_back_at_utc") && (*data)["rolled_back_at_utc"].is_string()) {
        state.rolled_back_at_utc = (*data)["rolled_back_at_utc"].get<std::string>();
    }
    if (data->contains("manifest")) {
        state.manifest = (*data)["manifest"];
    }
    // source 子对象:七件套逐个容错。
    if (data->contains("source") && (*data)["source"].is_object()) {
        const nlohmann::json& source = (*data)["source"];
        if (source.contains("repo") && source["repo"].is_string()) {
            state.source_repo = source["repo"].get<std::string>();
        }
        if (source.contains("release_tag") && source["release_tag"].is_string()) {
            state.source_release_tag = source["release_tag"].get<std::string>();
        }
        if (source.contains("asset_name") && source["asset_name"].is_string()) {
            state.source_asset_name = source["asset_name"].get<std::string>();
        }
        if (source.contains("download_url") && source["download_url"].is_string()) {
            state.source_download_url = source["download_url"].get<std::string>();
        }
        if (source.contains("release_id") && source["release_id"].is_number_integer()) {
            state.source_release_id = source["release_id"].get<std::int64_t>();
        }
        if (source.contains("asset_id") && source["asset_id"].is_number_integer()) {
            state.source_asset_id = source["asset_id"].get<std::int64_t>();
        }
        state.source_asset_digest = StringField(source, "asset_digest");
    }
    return state;
}

void WriteInstallState(const LayoutPaths& paths, const InstallState& state, const UtcNowFn& now) {
    nlohmann::json source = nlohmann::json::object();
    source["repo"] = state.source_repo.has_value() ? nlohmann::json(*state.source_repo) : nlohmann::json();
    source["release_id"] = state.source_release_id.has_value()
                               ? nlohmann::json(*state.source_release_id)
                               : nlohmann::json();
    source["release_tag"] = state.source_release_tag.has_value()
                                ? nlohmann::json(*state.source_release_tag)
                                : nlohmann::json();
    source["asset_id"] = state.source_asset_id.has_value() ? nlohmann::json(*state.source_asset_id)
                                                           : nlohmann::json();
    source["asset_name"] = state.source_asset_name.has_value()
                               ? nlohmann::json(*state.source_asset_name)
                               : nlohmann::json();
    source["asset_digest"] = state.source_asset_digest;
    source["download_url"] = state.source_download_url.has_value()
                                 ? nlohmann::json(*state.source_download_url)
                                 : nlohmann::json();

    nlohmann::json out = nlohmann::json::object();
    out["schema"] = kInstallStateSchema;
    out["layout"] = "versioned";
    out["installed_at_utc"] = CallNow(now);
    out["version"] = state.version;
    out["platform"] = state.platform.has_value() ? nlohmann::json(*state.platform) : nlohmann::json();
    out["channel"] = state.channel;
    out["source"] = std::move(source);
    out["installer"] = state.installer;
    out["transaction"] = state.transaction;
    out["manifest_provenance"] = "official-package";
    out["manifest"] = state.manifest;
    out["pending_conflicts"] = nlohmann::json::array();
    if (state.rolled_back_at_utc.has_value()) {
        out["rolled_back_at_utc"] = *state.rolled_back_at_utc;
    }
    AtomicWriteOrThrow(paths.state, CanonicalJsonDump(out));
}

}  // namespace lubancode::updater
