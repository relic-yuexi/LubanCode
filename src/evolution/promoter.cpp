// VersionStore 的实现(自进化闭环阶段 4)。规矩:
//   - 唯一写口仍是 EvolutionCoordinator——本模块的 Install/SetCanary/
//     PromoteToActive/RollbackTo 只做 store 侧文件与指针账,不经它谁也不
//     调这里;
//   - 版本一枚不删:回滚只切指针,已装版本、install-log 一行不抹;
//   - 哈希三处对账:staging 复算(停晋升)、canary/promote/rollback 前复算
//     (手改过 store 即拒切)、装配快照现算(拒挂并指路)。
#include "evolution/promoter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "evolution/candidate.hpp"   // ComputeCandidateContentHash
#include "evolution/eval.hpp"        // RunStaticGate(静态门复用,不另写)
#include "package/manifest.hpp"      // ParsePackageManifest(版本号取根清单)
#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "platform/paths.hpp"

namespace lubancode::evolution {

namespace {

using platform::PathToUtf8;
using platform::Utf8ToPath;

std::string IsoNowUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

bool WriteFileBytes(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    return file.good();
}

bool AppendFileLine(const std::filesystem::path& path, const std::string& line) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file.is_open()) {
        return false;
    }
    file << line << "\n";
    return file.good();
}

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// 版本号当目录名:单段、无分隔、无越界段。别的形状一律拒之门外。
bool VersionIsSafeDirName(const std::string& version) {
    if (version.empty() || version == "." || version == ".." ||
        version.find('/') != std::string::npos || version.find('\\') != std::string::npos) {
        return false;
    }
    return true;
}

// 递归复制(staging 用):文件一枚一枚搬,任何一枚搬不动即失败——不留
// 半截拷贝给 rename。symlink/junction 不跟随不复制(候选包本就该没有;
// 有也不该混进正式 store)。
std::optional<std::string> CopyTree(const std::filesystem::path& from,
                                    const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    if (ec) {
        return "建 staging 目录失败: " + PathToUtf8(to) + ": " + ec.message();
    }
    for (auto it = std::filesystem::recursive_directory_iterator(from, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            return "枚举候选包失败: " + PathToUtf8(from) + ": " + ec.message();
        }
        const std::filesystem::path& src = it->path();
        const std::filesystem::path dst = to / src.lexically_relative(from);
        std::error_code link_ec;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(src, link_ec)) ||
            link_ec) {
            return "候选包含软链/junction,不进正式 store: " + PathToUtf8(src);
        }
        if (it->is_directory()) {
            std::filesystem::create_directories(dst, ec);
            if (ec) {
                return "建 staging 子目录失败: " + PathToUtf8(dst) + ": " + ec.message();
            }
            continue;
        }
        if (!it->is_regular_file()) {
            continue;  // 奇怪条目(设备/套接字一类)安静跳过,不进 store
        }
        std::error_code copy_ec;
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::none, copy_ec);
        if (copy_ec) {
            return "复制失败: " + PathToUtf8(src) + " -> " + PathToUtf8(dst) + ": " +
                   copy_ec.message();
        }
    }
    return std::nullopt;
}

StoreVersionInfo VersionInfoFromJson(const nlohmann::json& json) {
    StoreVersionInfo info;
    if (json.is_object()) {
        if (const auto it = json.find("version"); it != json.end() && it->is_string()) {
            info.version = it->get<std::string>();
        }
        if (const auto it = json.find("content_hash"); it != json.end() && it->is_string()) {
            info.content_hash = it->get<std::string>();
        }
        if (const auto it = json.find("candidate_id"); it != json.end() && it->is_string()) {
            info.candidate_id = it->get<std::string>();
        }
        if (const auto it = json.find("installed_at"); it != json.end() && it->is_string()) {
            info.installed_at = it->get<std::string>();
        }
    }
    return info;
}

std::optional<StoreChannelPointer> PointerFromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    StoreChannelPointer pointer;
    if (const auto it = json.find("version"); it != json.end() && it->is_string()) {
        pointer.version = it->get<std::string>();
    }
    if (const auto it = json.find("content_hash"); it != json.end() && it->is_string()) {
        pointer.content_hash = it->get<std::string>();
    }
    if (const auto it = json.find("candidate_id"); it != json.end() && it->is_string()) {
        pointer.candidate_id = it->get<std::string>();
    }
    if (const auto it = json.find("set_at"); it != json.end() && it->is_string()) {
        pointer.set_at = it->get<std::string>();
    }
    if (const auto it = json.find("via"); it != json.end() && it->is_string()) {
        pointer.via = it->get<std::string>();
    }
    if (pointer.version.empty()) {
        return std::nullopt;
    }
    return pointer;
}

}  // namespace

// ---------------------------------------------------------------------------
// channels.json 序列化
// ---------------------------------------------------------------------------

std::string SerializeStoreChannels(const StoreChannels& channels) {
    nlohmann::json out;
    out["schema"] = channels.schema;
    out["package_id"] = channels.package_id;
    nlohmann::json versions = nlohmann::json::object();
    for (const auto& [version, info] : channels.versions) {
        versions[version] = {{"version", info.version},
                             {"content_hash", info.content_hash},
                             {"candidate_id", info.candidate_id},
                             {"installed_at", info.installed_at}};
    }
    out["versions"] = versions;
    const auto pointer_json = [](const std::optional<StoreChannelPointer>& pointer) {
        if (!pointer.has_value()) {
            return nlohmann::json(nullptr);
        }
        return nlohmann::json{{"version", pointer->version},
                              {"content_hash", pointer->content_hash},
                              {"candidate_id", pointer->candidate_id},
                              {"set_at", pointer->set_at},
                              {"via", pointer->via}};
    };
    out["active"] = pointer_json(channels.active);
    out["canary"] = pointer_json(channels.canary);
    return out.dump(2) + "\n";
}

std::optional<StoreChannels> ParseStoreChannels(const std::string& text) {
    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (!root.is_object()) {
            return std::nullopt;
        }
        StoreChannels channels;
        if (const auto it = root.find("schema");
            it == root.end() || !it->is_number_integer() || it->get<int>() != 1) {
            return std::nullopt;
        }
        channels.schema = 1;
        if (const auto it = root.find("package_id"); it != root.end() && it->is_string()) {
            channels.package_id = it->get<std::string>();
        }
        if (const auto it = root.find("versions"); it != root.end() && it->is_object()) {
            for (auto version_it = it->begin(); version_it != it->end(); ++version_it) {
                StoreVersionInfo info = VersionInfoFromJson(version_it.value());
                if (info.version.empty()) {
                    info.version = version_it.key();
                }
                channels.versions[version_it.key()] = std::move(info);
            }
        }
        if (const auto it = root.find("active"); it != root.end()) {
            channels.active = PointerFromJson(*it);
        }
        if (const auto it = root.find("canary"); it != root.end()) {
            channels.canary = PointerFromJson(*it);
        }
        if (channels.package_id.empty()) {
            return std::nullopt;
        }
        return channels;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// install-log.jsonl
// ---------------------------------------------------------------------------

std::string SerializeStoreLogEvent(const StoreLogEvent& event) {
    nlohmann::json out;
    out["schema"] = event.schema;
    out["seq"] = event.seq;
    out["package_id"] = event.package_id;
    out["event"] = event.event;
    out["version"] = event.version;
    out["content_hash"] = event.content_hash;
    out["candidate_id"] = event.candidate_id;
    out["from_version"] = event.from_version.has_value()
                                  ? nlohmann::json(*event.from_version)
                                  : nlohmann::json(nullptr);
    out["from_channel"] = event.from_channel.has_value()
                                  ? nlohmann::json(*event.from_channel)
                                  : nlohmann::json(nullptr);
    out["reason"] = event.reason;
    out["at"] = event.at;
    return out.dump();
}

std::optional<StoreLogEvent> ParseStoreLogEvent(const std::string& line) {
    try {
        const nlohmann::json root = nlohmann::json::parse(line);
        if (!root.is_object()) {
            return std::nullopt;
        }
        StoreLogEvent event;
        if (const auto it = root.find("schema");
            it == root.end() || !it->is_number_integer() || it->get<int>() != 1) {
            return std::nullopt;
        }
        if (const auto it = root.find("seq"); it != root.end() && it->is_number_integer()) {
            event.seq = it->get<std::int64_t>();
        }
        auto take_string = [&root](const char* key, std::string& into) {
            if (const auto it = root.find(key); it != root.end() && it->is_string()) {
                into = it->get<std::string>();
            }
        };
        take_string("package_id", event.package_id);
        take_string("event", event.event);
        take_string("version", event.version);
        take_string("content_hash", event.content_hash);
        take_string("candidate_id", event.candidate_id);
        take_string("reason", event.reason);
        take_string("at", event.at);
        if (const auto it = root.find("from_version"); it != root.end() && it->is_string()) {
            event.from_version = it->get<std::string>();
        }
        if (const auto it = root.find("from_channel"); it != root.end() && it->is_string()) {
            event.from_channel = it->get<std::string>();
        }
        if (event.event.empty() || event.package_id.empty()) {
            return std::nullopt;
        }
        return event;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::vector<StoreLogEvent> LoadStoreLog(const std::filesystem::path& log_file) {
    std::vector<StoreLogEvent> out;
    if (const auto text = ReadFileText(log_file); text.has_value()) {
        std::istringstream stream(*text);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            if (const auto event = ParseStoreLogEvent(line); event.has_value()) {
                out.push_back(std::move(*event));
            }
            // 坏行/半截行跳过,不废整账(与观察账/评测账同规矩)。
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// VersionStore:目录与指针
// ---------------------------------------------------------------------------

std::filesystem::path VersionStore::PackageDir(const std::string& package_id) const {
    return root_ / Utf8ToPath(package_id);
}

std::filesystem::path VersionStore::VersionDir(const std::string& package_id,
                                               const std::string& version) const {
    return PackageDir(package_id) / Utf8ToPath(version);
}

std::vector<std::string> VersionStore::ListPackages() const {
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec) || ec) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root_, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        if (ReadFileText(entry.path() / "channels.json").has_value()) {
            out.push_back(PathToUtf8(entry.path().filename()));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<StoreChannels> VersionStore::LoadChannels(const std::string& package_id) const {
    const auto text = ReadFileText(PackageDir(package_id) / "channels.json");
    if (!text.has_value()) {
        return std::nullopt;
    }
    return ParseStoreChannels(*text);
}

std::expected<StoreChannels, std::string> VersionStore::LoadOrInitChannels(
    const std::string& package_id) {
    if (auto channels = LoadChannels(package_id); channels.has_value()) {
        return std::move(*channels);
    }
    StoreChannels fresh;
    fresh.schema = 1;
    fresh.package_id = package_id;
    return fresh;
}

bool VersionStore::WriteChannelsAtomic(const std::filesystem::path& package_dir,
                                       const StoreChannels& channels) {
    // 统一原子写(审计 P1):唯一临时名 + 平台原子替换,要么整份新的在,
    // 要么整份旧的在,没有半截 channels.json。
    const std::filesystem::path final_path = package_dir / "channels.json";
    return platform::AtomicWriteFile(final_path, SerializeStoreChannels(channels)).has_value();
}

void VersionStore::AppendLog(const std::filesystem::path& package_dir, StoreLogEvent event) {
    std::int64_t seq = 0;
    if (const auto text = ReadFileText(package_dir / "install-log.jsonl"); text.has_value()) {
        for (const char c : *text) {
            if (c == '\n') {
                ++seq;
            }
        }
    }
    event.seq = seq + 1;
    if (event.at.empty()) {
        event.at = IsoNowUtc();
    }
    AppendFileLine(package_dir / "install-log.jsonl", SerializeStoreLogEvent(event));
}

// ---------------------------------------------------------------------------
// Install:staging -> 复算哈希 -> 静态门 -> 原子落
// ---------------------------------------------------------------------------

std::expected<VersionStore::InstallOutcome, std::string> VersionStore::Install(
    const std::filesystem::path& package_dir, const std::string& candidate_id,
    const std::string& expected_hash) {
    // ---- 身份:版本号取根清单(候选瞄准的稳定版号) ----
    const auto manifest_text = ReadFileText(package_dir / "package.yaml");
    if (!manifest_text.has_value()) {
        return std::unexpected("候选包缺 package.yaml: " + PathToUtf8(package_dir));
    }
    const auto manifest = lubancode::package::ParsePackageManifest(*manifest_text);
    if (!manifest.has_value()) {
        return std::unexpected("候选包根清单解析不过: " + manifest.error().Format());
    }
    const std::string& package_id = manifest->id;
    const std::string& version = manifest->version.text;
    if (!VersionIsSafeDirName(version)) {
        return std::unexpected("版本号不能当目录名: \"" + version + "\"");
    }

    const std::filesystem::path package_root = PackageDir(package_id);
    const std::filesystem::path staging = package_root / ".staging" / Utf8ToPath(version);
    const std::filesystem::path final_dir = package_root / Utf8ToPath(version);

    // ---- 幂等:同版本同哈希已装,不重装(重批场景)。目录被手删则重装
    //      (账里旧记录被新账覆盖);盘上有别的内容则明拒——不原地改。 ----
    if (auto channels = LoadChannels(package_id); channels.has_value()) {
        const auto installed = channels->versions.find(version);
        if (installed != channels->versions.end()) {
            const std::string on_disk = ComputeCandidateContentHash(final_dir);
            if (on_disk == installed->second.content_hash && on_disk == expected_hash) {
                InstallOutcome outcome;
                outcome.package_id = package_id;
                outcome.version = version;
                outcome.content_hash = on_disk;
                outcome.version_dir = final_dir;
                outcome.already_present = true;
                return outcome;
            }
            if (!on_disk.empty()) {
                return std::unexpected(
                    "正式 Package 不原地改:store 里 " + package_id + "@" + version + " 已装(账上 " +
                    installed->second.content_hash.substr(0, 19) + "),本次要装的是 " +
                    expected_hash.substr(0, 19) +
                    ",内容不同;换版本号重做候选,或先清走旧版");
            }
            // on_disk 为空:版本目录不在盘上,按下落重装处理。
        }
    }

    // ---- 第 1 步:复制到 staging(写一半失败,正式 store 不变) ----
    {
        std::error_code ec;
        std::filesystem::remove_all(staging, ec);  // 上回失败留下的残骸,清了重来
        if (const auto failure = CopyTree(package_dir, staging); failure.has_value()) {
            std::filesystem::remove_all(staging, ec);
            return std::unexpected(*failure + "(staging 失败,正式 store 未动)");
        }
    }

    // ---- 第 2 步:复算哈希,与批准绑定的对账 ----
    const std::string staged_hash = ComputeCandidateContentHash(staging);
    if (staged_hash.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(staging, ec);
        return std::unexpected("staging 复算整包哈希失败(正式 store 未动)");
    }
    if (staged_hash != expected_hash) {
        std::error_code ec;
        std::filesystem::remove_all(staging, ec);
        return std::unexpected(
            "staging 复算哈希与批准绑定的对不上:批的是 " + expected_hash.substr(0, 19) +
            ",复算是 " + staged_hash.substr(0, 19) +
            "(内容变过,旧批准作废;停晋升,重做候选)");
    }

    // ---- 第 3 步:再过一遍静态门(AnalyzePackage + 密钥/绝对路径扫描) ----
    {
        const StaticGateResult gate = RunStaticGate(staging);
        if (!gate.pass()) {
            std::error_code ec;
            std::filesystem::remove_all(staging, ec);
            std::string detail = "staging 静态门未过,停晋升(正式 store 未动)";
            if (!gate.errors.empty()) {
                detail += ": " + gate.errors.front();
            } else if (!gate.findings.empty()) {
                detail += ": [" + gate.findings.front().kind + "] " + gate.findings.front().path;
            }
            return std::unexpected(detail);
        }
    }

    // ---- 第 4 步:原子落(staging rename 成正式版本目录) ----
    {
        std::error_code ec;
        if (std::filesystem::exists(final_dir, ec) && !ec) {
            std::filesystem::remove_all(staging, ec);
            return std::unexpected("正式版本目录已存在,不覆盖: " + PathToUtf8(final_dir));
        }
        std::filesystem::rename(staging, final_dir, ec);
        if (ec) {
            std::filesystem::remove_all(staging, ec);
            return std::unexpected("版本目录落位失败(正式 store 未动): " + PathToUtf8(final_dir) +
                                   ": " + ec.message());
        }
    }

    // ---- 第 5 步:记账(channels.versions + install-log),clear 掉 staging 空壳 ----
    auto channels = LoadOrInitChannels(package_id);
    if (!channels.has_value()) {
        return std::unexpected(channels.error());
    }
    StoreVersionInfo info;
    info.version = version;
    info.content_hash = staged_hash;
    info.candidate_id = candidate_id;
    info.installed_at = IsoNowUtc();
    channels->versions[version] = info;
    if (!WriteChannelsAtomic(package_root, *channels)) {
        return std::unexpected("写 channels.json 失败: " + PathToUtf8(package_root));
    }
    {
        StoreLogEvent event;
        event.package_id = package_id;
        event.event = "install";
        event.version = version;
        event.content_hash = staged_hash;
        event.candidate_id = candidate_id;
        event.reason = "staged:复算哈希一致,静态门过,原子落 store";
        AppendLog(package_root, std::move(event));
        std::error_code ec;
        std::filesystem::remove(package_root / ".staging", ec);  // 空壳顺手收走
    }

    InstallOutcome outcome;
    outcome.package_id = package_id;
    outcome.version = version;
    outcome.content_hash = staged_hash;
    outcome.version_dir = final_dir;
    return outcome;
}

// ---------------------------------------------------------------------------
// 指针切换:canary / promote / rollback
// ---------------------------------------------------------------------------

std::expected<StoreChannelPointer, std::string> VersionStore::SetCanary(
    const std::string& package_id, const std::string& version) {
    auto channels = LoadChannels(package_id);
    if (!channels.has_value()) {
        return std::unexpected("store 里没有包 \"" + package_id + "\" 的账(先 /evolve approve)");
    }
    const auto installed = channels->versions.find(version);
    if (installed == channels->versions.end()) {
        return std::unexpected("版本 " + version + " 不在 store 的已装账里: " + package_id);
    }
    // 哈希对账:store 内文件被手改,即拒切 canary(指路)。
    const std::string on_disk = ComputeCandidateContentHash(VersionDir(package_id, version));
    if (on_disk != installed->second.content_hash) {
        return std::unexpected("store 内文件被改过:账上 " +
                               installed->second.content_hash.substr(0, 19) + ",盘上是 " +
                               on_disk.substr(0, 19) + ";先修 store(重装该版本)再启用");
    }
    StoreChannelPointer pointer;
    pointer.version = version;
    pointer.content_hash = installed->second.content_hash;
    pointer.candidate_id = installed->second.candidate_id;
    pointer.set_at = IsoNowUtc();
    pointer.via = "canary";
    channels->canary = pointer;
    if (!WriteChannelsAtomic(PackageDir(package_id), *channels)) {
        return std::unexpected("写 channels.json 失败: " +
                               PathToUtf8(PackageDir(package_id) / "channels.json"));
    }
    StoreLogEvent event;
    event.package_id = package_id;
    event.event = "canary";
    event.version = version;
    event.content_hash = pointer.content_hash;
    event.candidate_id = pointer.candidate_id;
    event.reason = "点名 canary(新会话生效,旧任务钉旧快照)";
    AppendLog(PackageDir(package_id), std::move(event));
    return pointer;
}

std::expected<StoreChannelPointer, std::string> VersionStore::PromoteToActive(
    const std::string& package_id) {
    auto channels = LoadChannels(package_id);
    if (!channels.has_value()) {
        return std::unexpected("store 里没有包 \"" + package_id + "\" 的账");
    }
    if (!channels->canary.has_value()) {
        return std::unexpected("包 \"" + package_id + "\" 没有 canary 版本,无从晋升(先 /evolve use)");
    }
    const StoreChannelPointer canary = *channels->canary;
    const auto installed = channels->versions.find(canary.version);
    if (installed == channels->versions.end()) {
        return std::unexpected("canary 指的版本不在已装账里: " + canary.version);
    }
    const std::string on_disk = ComputeCandidateContentHash(VersionDir(package_id, canary.version));
    if (on_disk != installed->second.content_hash) {
        return std::unexpected("store 内文件被改过:账上 " +
                               installed->second.content_hash.substr(0, 19) + ",盘上是 " +
                               on_disk.substr(0, 19) + ";先修 store(重装该版本)再晋升");
    }
    StoreChannelPointer active = canary;
    active.set_at = IsoNowUtc();
    active.via = "promote";
    channels->active = active;
    channels->canary = std::nullopt;
    if (!WriteChannelsAtomic(PackageDir(package_id), *channels)) {
        return std::unexpected("写 channels.json 失败: " +
                               PathToUtf8(PackageDir(package_id) / "channels.json"));
    }
    StoreLogEvent event;
    event.package_id = package_id;
    event.event = "promote";
    event.version = active.version;
    event.content_hash = active.content_hash;
    event.candidate_id = active.candidate_id;
    event.from_channel = "canary";
    event.reason = "canary -> active(新会话起用新版;旧任务照旧)";
    AppendLog(PackageDir(package_id), std::move(event));
    return active;
}

std::expected<std::optional<StoreChannelPointer>, std::string> VersionStore::RollbackTo(
    const std::string& package_id, const std::string& version, const std::string& reason) {
    auto channels = LoadChannels(package_id);
    if (!channels.has_value()) {
        return std::unexpected("store 里没有包 \"" + package_id + "\" 的账");
    }
    std::optional<StoreChannelPointer> target;
    if (!version.empty()) {
        const auto installed = channels->versions.find(version);
        if (installed == channels->versions.end()) {
            return std::unexpected("版本 " + version + " 不在 store 的已装账里: " + package_id +
                                   "(/evolve show 看已装版本)");
        }
        const std::string on_disk =
            ComputeCandidateContentHash(VersionDir(package_id, version));
        if (on_disk != installed->second.content_hash) {
            return std::unexpected("store 内文件被改过:账上 " +
                                   installed->second.content_hash.substr(0, 19) + ",盘上是 " +
                                   on_disk.substr(0, 19) + ";先修 store(重装该版本)再回滚");
        }
        StoreChannelPointer pointer;
        pointer.version = version;
        pointer.content_hash = installed->second.content_hash;
        pointer.candidate_id = installed->second.candidate_id;
        pointer.set_at = IsoNowUtc();
        pointer.via = "rollback";
        target = pointer;
    }
    const std::optional<std::string> from_version =
        channels->active.has_value() ? std::optional<std::string>(channels->active->version)
                                     : std::nullopt;
    const std::string from_candidate =
        channels->active.has_value() ? channels->active->candidate_id
                                     : (channels->canary.has_value() ? channels->canary->candidate_id
                                                                     : std::string());
    const std::string from_hash =
        channels->active.has_value() ? channels->active->content_hash
                                     : (channels->canary.has_value() ? channels->canary->content_hash
                                                                     : std::string());
    channels->active = target;
    channels->canary = std::nullopt;  // 回滚把灰度一并收走;版本与账一枚不删
    if (!WriteChannelsAtomic(PackageDir(package_id), *channels)) {
        return std::unexpected("写 channels.json 失败: " +
                               PathToUtf8(PackageDir(package_id) / "channels.json"));
    }
    StoreLogEvent event;
    event.package_id = package_id;
    event.event = "rollback";
    event.version = version.empty() ? std::string("(撤下)") : version;
    event.content_hash = from_hash;
    event.candidate_id = from_candidate;
    event.from_version = from_version;
    event.from_channel = from_version.has_value() ? std::optional<std::string>("active")
                                                  : std::optional<std::string>("canary");
    event.reason = reason.empty() ? "回滚:切回旧版,不删版本不抹账" : reason;
    AppendLog(PackageDir(package_id), std::move(event));
    return target;
}

// ---------------------------------------------------------------------------
// PackageSnapshot:装配折算(路径 + 期望哈希 + 现算哈希)
// ---------------------------------------------------------------------------

const VersionStore::SnapshotEntry* VersionStore::Snapshot::Find(
    const std::string& package_id) const {
    for (const auto& entry : entries) {
        if (entry.package_id == package_id) {
            return &entry;
        }
    }
    return nullptr;
}

VersionStore::Snapshot VersionStore::BuildSnapshot() const {
    Snapshot snapshot;
    for (const std::string& package_id : ListPackages()) {
        const auto channels = LoadChannels(package_id);
        if (!channels.has_value()) {
            continue;  // channels.json 坏了:这只包不折快照,也不挂
        }
        // 选中:canary 遮 active(点名灰度——新会话拿 canary)。
        const StoreChannelPointer* selection = nullptr;
        std::string channel;
        if (channels->canary.has_value()) {
            selection = &*channels->canary;
            channel = "canary";
        } else if (channels->active.has_value()) {
            selection = &*channels->active;
            channel = "active";
        }
        if (selection == nullptr) {
            continue;  // 只装架未启用(staged):不进快照
        }
        SnapshotEntry entry;
        entry.package_id = package_id;
        entry.version = selection->version;
        entry.channel = channel;
        entry.package_root = VersionDir(package_id, selection->version);
        entry.expected_hash = selection->content_hash;
        entry.actual_hash = ComputeCandidateContentHash(entry.package_root);
        if (!entry.actual_hash.empty() && entry.actual_hash == entry.expected_hash) {
            entry.intact = true;
        } else {
            entry.note = "store 内文件与安装账对不上(期望 " + entry.expected_hash.substr(0, 19) +
                         ",盘上 " +
                         (entry.actual_hash.empty() ? std::string("(读不动)")
                                                    : entry.actual_hash.substr(0, 19)) +
                         ");拒挂。修法:重装该版本,或 /evolve rollback " + package_id +
                         " 切回旧版";
        }
        if (entry.intact) {
            snapshot.entries.push_back(std::move(entry));
        } else {
            snapshot.rejected.push_back(std::move(entry));
        }
    }
    return snapshot;
}

std::vector<lubancode::package::PackageCandidate> VersionStore::ScanSelectedCandidates() const {
    std::vector<lubancode::package::PackageCandidate> out;
    const Snapshot snapshot = BuildSnapshot();
    const auto take = [&](const SnapshotEntry& item) {
        lubancode::package::PackageCandidate candidate;
        candidate.scope = lubancode::package::PackageScope::Store;
        candidate.layer_root = PackageDir(item.package_id);
        candidate.package_root = item.package_root;
        candidate.dir_name = item.package_id;
        if (const auto text = ReadFileText(item.package_root / "package.yaml"); text.has_value()) {
            if (auto parsed = lubancode::package::ParsePackageManifest(*text); parsed.has_value()) {
                candidate.manifest = std::move(*parsed);
            }
        }
        out.push_back(std::move(candidate));
    };
    for (const SnapshotEntry& item : snapshot.entries) {
        take(item);
    }
    for (const SnapshotEntry& item : snapshot.rejected) {
        take(item);  // tamper 的也进发现账(/package list 看得见);挂载侧才拒
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.dir_name < b.dir_name;
    });
    return out;
}

}  // namespace lubancode::evolution
