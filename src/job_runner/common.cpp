#include "job_runner/common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/secure_file.hpp"
#include "platform/sha256.hpp"

namespace lubancode::job_runner {
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
bool HexId(std::string_view s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool Identity(std::string_view s) {
    return !s.empty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    });
}
bool EqualSecret(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}
Json Failure(std::string_view code) { return Json{{"ok", false}, {"error", {{"code", code}}}}; }
Json ReadJson(const fs::path& path, std::size_t limit) {
    if (!platform::RejectReparsePoint(platform::FileIoPath(path))) throw Error("runner.state_is_link");
    std::error_code ec;
    const auto size = fs::file_size(platform::FileIoPath(path), ec);
    if (ec || size > limit) throw Error("runner.state_unreadable");
    std::ifstream input(platform::FileIoPath(path), std::ios::binary);
    if (!input) throw Error("runner.state_unreadable");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) throw Error("runner.state_unreadable");
    auto parsed = Json::parse(bytes, nullptr, false);
    if (parsed.is_discarded()) throw Error("runner.corrupt_state");
    return parsed;
}
void SaveJson(const fs::path& path, const Json& value) {
    const auto written = platform::AtomicWriteFile(path, value.dump(), platform::WriteDurability::ProcessCrashDurability);
    if (!written) throw Error("runner.store_failed");
}
void PublishJson(const fs::path& dir, const std::string& name, const Json& value) {
    const auto temporary = dir / (name + ".tmp-" + RandomHex());
    if (!platform::WriteNewSecureFile(platform::FileIoPath(temporary), value.dump())) {
        throw Error("runner.mailbox_write_failed");
    }
    std::error_code ec;
    fs::rename(platform::FileIoPath(temporary), platform::FileIoPath(dir / name), ec);
    if (ec) {
        fs::remove(platform::FileIoPath(temporary), ec);
        throw Error("runner.mailbox_write_failed");
    }
}
void PrivateDirectory(const fs::path& path) {
    if (!platform::CreateSecureDirectory(platform::FileIoPath(path))) throw Error("runner.state_directory_failed");
    VerifyPrivateDirectory(path);
}
void Keys(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) throw Error("runner.invalid_request");
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end()) throw Error("runner.invalid_request");
    }
}
Json NormalizeSpec(const Json& value) {
    Keys(value, {"argv", "cwd", "env_refs"});
    if (!value.contains("argv") || !value["argv"].is_array() || value["argv"].empty() ||
        value["argv"].size() > 256 || !value.contains("cwd") || !value["cwd"].is_string()) {
        throw Error("runner.invalid_spec");
    }
    Json spec = value;
    std::size_t bytes = 0;
    for (const auto& arg : spec["argv"]) {
        if (!arg.is_string()) throw Error("runner.invalid_spec");
        const auto text = arg.get<std::string>();
        if (text.find('\0') != std::string::npos) throw Error("runner.invalid_spec");
        bytes += text.size();
    }
    if (bytes > 32768) throw Error("runner.invalid_spec");
    const auto executable = platform::Utf8ToPath(spec["argv"][0].get<std::string>());
    const auto cwd = platform::Utf8ToPath(spec["cwd"].get<std::string>());
    if (!executable.is_absolute() || !cwd.is_absolute() || spec["cwd"].get<std::string>().find('\0') != std::string::npos) {
        throw Error("runner.absolute_path_required");
    }
    spec["argv"][0] = platform::PathToUtf8(executable.lexically_normal());
    spec["cwd"] = platform::PathToUtf8(cwd.lexically_normal());
    if (!spec.contains("env_refs")) spec["env_refs"] = Json::array();
    if (!spec["env_refs"].is_array() || spec["env_refs"].size() > 64) throw Error("runner.invalid_spec");
    std::set<std::string> names;
    for (const auto& item : spec["env_refs"]) {
        if (!item.is_string()) throw Error("runner.invalid_spec");
        const auto name = item.get<std::string>();
        if (name.empty() || name.size() > 128 ||
            !std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            }) || !names.insert(name).second) throw Error("runner.invalid_spec");
    }
    spec["env_refs"] = names;
    return spec;
}
std::vector<std::string> ResolveEnvironment(const Json& spec) {
    std::map<std::string, std::string> selected;
    for (const char* name : {"PATH", "HOME", "USERPROFILE", "SystemRoot", "TEMP", "TMP", "TMPDIR", "LANG",
                             "LC_ALL", "PATHEXT", "COMSPEC", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"}) {
        if (auto value = platform::GetEnvVarPresent(name)) selected[name] = *value;
    }
    for (const auto& ref : spec["env_refs"]) {
        const auto name = ref.get<std::string>();
        const auto value = platform::GetEnvVarPresent(name.c_str());
        if (!value) throw Error("runner.environment_reference_missing");
        // Resolved values never enter the persisted spec or a reply. argv is an
        // explicit persistent launch spec; callers must pass credentials by ref.
        selected[name] = *value;
    }
    std::vector<std::string> result;
    std::map<std::string, std::string> normalized;
    for (const auto& [name, value] : selected) {
        auto key = name;
#ifdef _WIN32
        for (auto& c : key) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
#endif
        normalized[key] = value;
    }
    for (const auto& [name, value] : normalized) result.push_back(name + "=" + value);
    return result;
}
std::string KeyFor(std::string_view session, std::string_view key) {
    return platform::Sha256Hex(std::string(session) + '\0' + std::string(key));
}
std::string Digest(const Json& value) { return platform::Sha256Hex(value.dump()); }
bool Terminal(std::string_view state) {
    return state == "succeeded" || state == "failed" || state == "cancelled" || state == "indeterminate";
}
}  // namespace lubancode::job_runner
