#include "sdk/lua.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>
#include <system_error>
#include <utility>

#include "platform/bounded_read.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "tools/lua_tool.hpp"

namespace lubancore::detail {
namespace {
namespace fs = std::filesystem;
namespace platform = lubancode::platform;
using Json = nlohmann::json;
constexpr std::size_t kMaxPlan = 256 * 1024;
constexpr std::size_t kMaxScript = 1024 * 1024;
constexpr std::size_t kMaxTotalScripts = 16 * 1024 * 1024;
constexpr std::size_t kMaxScripts = 128;
Error Fail(std::string code, std::string text = {}) { return {std::move(code), std::move(text)}; }
bool Text(const std::string& value, std::size_t cap, bool empty = false) {
    return (empty || !value.empty()) && value.size() <= cap &&
        value.find('\0') == std::string::npos && platform::IsValidUtf8(value);
}
bool Name(const std::string& value) {
    return Text(value, 200) && value.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos;
}
bool Relative(const fs::path& value) {
    if (value.empty() || value.is_absolute() || value.has_root_name() || value.has_root_directory()) return false;
    for (const auto& part : value) if (part.empty() || part == "." || part == "..") return false;
    return value.lexically_normal() == value;
}
bool Within(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    return Relative(relative);
}
} // namespace

Result<void> SessionLua::Load(const lua::v1::Selection& selection) {
    const auto max_wall = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::duration::max()).count() / 2;
    if (!Text(selection.root, 4096) || selection.scripts.empty() || selection.scripts.size() > kMaxScripts ||
        selection.instruction_budget == 0 || selection.memory_cap_bytes == 0 ||
        selection.wall_budget.count() <= 0 || selection.wall_budget.count() > max_wall)
        return std::unexpected(Fail("sdk.lua.invalid_selection", "absolute root, selected scripts and positive representable budgets are required"));
    const auto root = platform::Utf8ToPath(selection.root);
    if (!root.is_absolute() || root.lexically_normal() != root)
        return std::unexpected(Fail("sdk.lua.invalid_selection", "root must be an absolute normalized directory"));
    std::error_code ec;
    const auto root_status = fs::symlink_status(root, ec);
    if (ec || fs::is_symlink(root_status) || !fs::is_directory(root_status) || fs::canonical(root, ec) != root || ec)
        return std::unexpected(Fail("sdk.lua.read_failed", "root is linked, non-directory or unavailable"));
    std::set<std::string> selected_names, selected_paths;
    std::vector<fs::path> files;
    std::size_t used = 0;
    auto profile = lubancode::tools::LuaProfile::HookDefault();
    profile.instruction_budget = selection.instruction_budget;
    profile.memory_cap_bytes = selection.memory_cap_bytes;
    profile.wall_budget = selection.wall_budget;
    profile.allow_print = false;
    profile.allow_error_catching = false;
    for (const auto& script : selection.scripts) {
        if (!Text(script.path, 4096) || !Name(script.tool_name) ||
            !selected_names.insert(script.tool_name).second || !selected_paths.insert(script.path).second)
            return std::unexpected(Fail("sdk.lua.invalid_selection", "invalid or duplicate script declaration"));
        const auto relative = platform::Utf8ToPath(script.path);
        if (!Relative(relative) || relative.extension() != ".lua")
            return std::unexpected(Fail("sdk.lua.invalid_selection", "standalone script needs an exact relative .lua path"));
        auto path = root;
        for (auto part = relative.begin(); part != relative.end(); ++part) {
            path /= *part;
            const auto status = fs::symlink_status(path, ec);
            const auto next = std::next(part);
            if (ec || fs::is_symlink(status) ||
                (next == relative.end() ? !fs::is_regular_file(status) : !fs::is_directory(status)))
                return std::unexpected(Fail("sdk.lua.read_failed", "selected path is linked, nonregular or unavailable"));
        }
        if (fs::canonical(path, ec) != path || ec || !Within(root, path))
            return std::unexpected(Fail("sdk.lua.read_failed", "selected path mapping changed"));
        for (const auto& previous : files) {
            const bool same = fs::equivalent(previous, path, ec);
            if (ec || same) return std::unexpected(Fail("sdk.lua.invalid_selection", "selected script aliases another entry"));
        }
        files.push_back(path);
        auto bytes = platform::ReadBoundedRegularFile(path, (std::min)(kMaxScript, kMaxTotalScripts - used));
        if (!bytes || !Text(*bytes, kMaxScript))
            return std::unexpected(Fail("sdk.lua.read_failed", bytes ? "script has invalid UTF-8 or NUL" : bytes.error()));
        used += bytes->size();
        auto tool = lubancode::tools::LuaTool::LoadFromScript(*bytes, platform::PathToUtf8(relative.stem()), profile);
        if (!tool) return std::unexpected(Fail("sdk.lua.load_failed", tool.error()));
        const auto actual_name = (*tool)->name();
        const auto description = (*tool)->description();
        const auto schema = (*tool)->input_schema();
        if (actual_name != script.tool_name || !Name(actual_name) || !Text(description, kMaxPlan, true) ||
            !schema.is_object() || schema.value("type", Json()) != "object")
            return std::unexpected(Fail("sdk.lua.invalid_definition", "actual name, description or object schema differs"));
        const auto schema_bytes = schema.dump(-1, ' ', false, Json::error_handler_t::replace);
        if (!Text(schema_bytes, kMaxPlan) || Json::parse(schema_bytes, nullptr, false) != schema)
            return std::unexpected(Fail("sdk.lua.invalid_definition", "schema contains invalid text or exceeds plan cap"));
        snapshot_.entries.push_back({script.path, actual_name, description, platform::Sha256Hex(*bytes), schema_bytes});
        tools_.push_back(std::move(*tool));
    }
    snapshot_.root = selection.root;
    snapshot_.instruction_budget = selection.instruction_budget;
    snapshot_.memory_cap_bytes = selection.memory_cap_bytes;
    snapshot_.wall_budget = selection.wall_budget;
    return {};
}


} // namespace lubancore::detail
