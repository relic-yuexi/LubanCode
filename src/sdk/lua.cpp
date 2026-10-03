#include "sdk/lua.hpp"
#include "sdk/plan_write.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

#include "platform/bounded_read.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "tools/lua_tool.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/index.hpp"

namespace lubancore::detail {
namespace {
namespace fs = std::filesystem;
namespace platform = lubancode::platform;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
constexpr std::size_t kMaxPlan = 256 * 1024;
constexpr std::size_t kMaxScript = 1024 * 1024;
constexpr std::size_t kMaxTotalScripts = 16 * 1024 * 1024;
constexpr std::size_t kMaxScripts = 128;
constexpr const char* kPlanFile = "sdk-lua-plan.json";

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
Result<void> DirectoryUnder(const fs::path& root, const fs::path& directory) {
    const auto normalized = directory.lexically_normal();
    if (!Within(root, normalized)) return std::unexpected(Fail("sdk.lua.plan_invalid", "session directory escapes owned root"));
    std::error_code ec;
    auto cursor = root;
    for (const auto& part : normalized.lexically_relative(root)) {
        cursor /= part;
        const auto status = fs::symlink_status(cursor, ec);
        if (ec || fs::is_symlink(status) || !fs::is_directory(status))
            return std::unexpected(Fail("sdk.lua.plan_invalid", "session directory is linked or unavailable"));
    }
    if (fs::canonical(normalized, ec) != normalized || ec)
        return std::unexpected(Fail("sdk.lua.plan_invalid", "session directory mapping changed"));
    return {};
}
Result<std::optional<std::string>> ReadPlan(const fs::path& root, const fs::path& directory) {
    auto owned = DirectoryUnder(root, directory);
    if (!owned) return std::unexpected(owned.error());
    const auto path = directory / kPlanFile;
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
        return std::optional<std::string>{};
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
        return std::unexpected(Fail("sdk.lua.plan_invalid", "plan is linked, nonregular or unavailable"));
    auto bytes = platform::ReadBoundedRegularFile(path, kMaxPlan);
    if (!bytes || !Text(*bytes, kMaxPlan))
        return std::unexpected(Fail("sdk.lua.plan_invalid", bytes ? "plan text is invalid" : bytes.error()));
    return std::optional<std::string>{std::move(*bytes)};
}
Result<std::uint64_t> Positive(const Json& value) {
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > 0) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() > 0)
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    return std::unexpected(Fail("sdk.lua.plan_invalid", "budget is not a positive integer"));
}
Result<lua::v1::Selection> SavedSelection(const Json& plan) {
    if (!plan.contains("root") || !plan["root"].is_string() || !plan.contains("entries") ||
        !plan["entries"].is_array() || !plan.contains("limits") || !plan["limits"].is_object())
        return std::unexpected(Fail("sdk.lua.plan_invalid", "saved Lua declaration is incomplete"));
    const auto& limits = plan["limits"];
    auto instructions = Positive(limits.value("instructions", Json()));
    auto memory = Positive(limits.value("memoryBytes", Json()));
    auto wall = Positive(limits.value("wallMilliseconds", Json()));
    if (!instructions || !memory || !wall || *memory > (std::numeric_limits<std::size_t>::max)() ||
        *wall > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        return std::unexpected(Fail("sdk.lua.plan_invalid", "saved budgets are invalid"));
    lua::v1::Selection selection;
    selection.root = plan["root"].get<std::string>();
    selection.instruction_budget = *instructions;
    selection.memory_cap_bytes = static_cast<std::size_t>(*memory);
    selection.wall_budget = std::chrono::milliseconds(static_cast<std::int64_t>(*wall));
    for (const auto& item : plan["entries"]) {
        if (!item.is_object() || !item.contains("path") || !item["path"].is_string() ||
            !item.contains("toolName") || !item["toolName"].is_string())
            return std::unexpected(Fail("sdk.lua.plan_invalid", "saved script declaration is invalid"));
        selection.scripts.push_back({item["path"].get<std::string>(), item["toolName"].get<std::string>()});
    }
    return selection;
}
const Json* Binding(const v3::MessageLine& message) {
    if (!message.system_meta || !message.system_meta->is_object()) return nullptr;
    const auto hosts = message.system_meta->find("hostBindings");
    if (hosts == message.system_meta->end() || !hosts->is_object()) return nullptr;
    const auto value = hosts->find("lua");
    return value == hosts->end() ? nullptr : &*value;
}
} // namespace

Result<std::shared_ptr<SessionLua>> SessionLua::Prepare(
    const std::optional<lua::v1::Selection>& selection, fs::path owned_root,
    std::string workspace_key, std::string resume_id) {
    auto owner = std::shared_ptr<SessionLua>(new SessionLua);
    owner->owned_root_ = std::move(owned_root);
    owner->resume_id_ = std::move(resume_id);
    auto effective = selection;
    if (!owner->resume_id_.empty()) {
        const auto directory = lubancode::workspace::index::ResolveDirByWorkspaceKey(
            owner->owned_root_ / "workspaces", workspace_key);
        if (!directory) return std::unexpected(Fail("sdk.session.open_failed", "resume session is unavailable"));
        owner->resume_dir_ = *directory / "sessions" / owner->resume_id_;
        std::error_code ec;
        const auto session_status = fs::symlink_status(owner->resume_dir_, ec);
        if (ec == std::errc::no_such_file_or_directory ||
            (!ec && session_status.type() == fs::file_type::not_found))
            return std::unexpected(Fail("sdk.session.open_failed", "resume session is unavailable"));
        // Only true absence keeps the established SDK code. Existing linked,
        // unreadable or malformed directories continue into strict validation.
        auto bytes = ReadPlan(owner->owned_root_, owner->resume_dir_);
        if (!bytes) return std::unexpected(bytes.error());
        if (*bytes) {
            owner->saved_bytes_ = **bytes;
            owner->saved_plan_ = Json::parse(owner->saved_bytes_, nullptr, false);
            const auto& plan = owner->saved_plan_;
            if (!plan.is_object() || plan.value("schemaVersion", Json()) != 1 ||
                plan.value("sessionId", Json()) != owner->resume_id_ ||
                !plan.contains("enabled") || !plan["enabled"].is_boolean() || plan.dump() != owner->saved_bytes_)
                return std::unexpected(Fail("sdk.lua.plan_invalid", "saved plan is invalid or noncanonical"));
            owner->snapshot_.enabled = plan["enabled"].get<bool>();
            owner->snapshot_.plan_sha256 = platform::Sha256Hex(owner->saved_bytes_);
            if (selection && !owner->snapshot_.enabled)
                return std::unexpected(Fail("sdk.lua.resume_mismatch", "saved session did not enable Lua"));
            if (owner->snapshot_.enabled) {
                auto saved = SavedSelection(plan);
                if (!saved) return std::unexpected(saved.error());
                if (selection) {
                    bool same = selection->root == saved->root && selection->instruction_budget == saved->instruction_budget &&
                        selection->memory_cap_bytes == saved->memory_cap_bytes && selection->wall_budget == saved->wall_budget &&
                        selection->scripts.size() == saved->scripts.size();
                    if (same) for (std::size_t index = 0; index < saved->scripts.size(); ++index)
                        same = same && selection->scripts[index].path == saved->scripts[index].path &&
                            selection->scripts[index].tool_name == saved->scripts[index].tool_name;
                    if (!same) return std::unexpected(Fail("sdk.lua.resume_mismatch", "explicit Lua declaration changed"));
                } else {
                    effective = std::move(*saved);
                }
            }
        } else {
            if (selection) return std::unexpected(Fail("sdk.lua.resume_mismatch", "legacy session did not admit Lua"));
            owner->legacy_ = true;
        }
        auto stream = v3::FindV3SessionStream(owner->resume_dir_);
        if (!stream) return std::unexpected(Fail("sdk.lua.plan_invalid", "saved V3 stream is unavailable"));
        auto ledger = v3::ReadV3Ledger(*stream);
        if (!ledger) return std::unexpected(Fail("sdk.lua.plan_invalid", ledger.error()));
        auto binding = owner->CheckBinding(*ledger);
        if (!binding) return std::unexpected(binding.error());
    }
    owner->snapshot_.enabled = effective.has_value();
    if (effective) {
        auto loaded = owner->Load(*effective);
        if (!loaded) return std::unexpected(loaded.error());
    }
    const auto candidate = owner->Plan(owner->resume_id_);
    if (candidate.dump().size() > kMaxPlan) return std::unexpected(Fail("sdk.lua.plan_invalid", "plan exceeds 256 KiB"));
    if (!owner->resume_id_.empty() && !owner->legacy_ && candidate != owner->saved_plan_)
        return std::unexpected(Fail("sdk.lua.resume_mismatch", "selection, source, schema, name or budget changed"));
    return owner;
}

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

SessionLua::Json SessionLua::Plan(const std::string& id) const {
    Json entries = Json::array();
    for (const auto& item : snapshot_.entries) entries.push_back({{"path", item.path}, {"toolName", item.tool_name},
        {"description", item.description}, {"contentSha256", item.content_sha256},
        {"inputSchema", Json::parse(item.input_schema_json)}});
    return Json{{"schemaVersion", 1}, {"sessionId", id}, {"enabled", snapshot_.enabled},
        {"profile", "Whitelisted"}, {"stdio", false}, {"root", snapshot_.root}, {"entries", std::move(entries)},
        {"limits", {{"instructions", snapshot_.instruction_budget}, {"memoryBytes", snapshot_.memory_cap_bytes},
                    {"wallMilliseconds", snapshot_.wall_budget.count()}}}};
}
std::vector<std::string> SessionLua::Names() const {
    std::vector<std::string> names;
    for (const auto& entry : snapshot_.entries) names.push_back(entry.tool_name);
    return names;
}
Result<std::vector<std::unique_ptr<lubancode::tools::Tool>>> SessionLua::TakeTools() {
    if (tools_taken_) return std::unexpected(Fail("sdk.lua.open_failed", "tools were already transferred"));
    tools_taken_ = true;
    return std::move(tools_);
}

Result<void> SessionLua::CheckBinding(const v3::V3Ledger& ledger) const {
    if (ledger.session_id != resume_id_) return std::unexpected(Fail("sdk.lua.plan_invalid", "journal identity mismatch"));
    if (legacy_) {
        for (const auto& message : ledger.messages) if (Binding(message))
            return std::unexpected(Fail("sdk.lua.plan_invalid", "journal binds a missing plan"));
        return {};
    }
    const Json expected{{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}};
    const auto matches = [&](const v3::MessageLine* message) {
        const auto* binding = message ? Binding(*message) : nullptr;
        return binding && *binding == expected && message->session_id == resume_id_ &&
            message->message.value("role", Json()) == "system";
    };
    if (ledger.messages.empty() || ledger.messages.front().seq != 1 || !matches(&ledger.messages.front()))
        return std::unexpected(Fail("sdk.lua.plan_invalid", "initial system binding differs"));
    bool started = false;
    for (const auto& event : ledger.events) {
        if (event.kind != v3::EventKindV3::SessionStarted) continue;
        if (event.session_id != resume_id_)
            return std::unexpected(Fail("sdk.lua.plan_invalid", "initial adoption belongs to another session"));
        const auto context = event.payload.find("context");
        if (context != event.payload.end() && context->is_object()) {
            const auto chain = context->find("contextChain");
            started = chain != context->end() && chain->is_array() && !chain->empty() &&
                chain->front().value("messageRef", Json()) == ledger.messages.front().message_id;
        }
        break;
    }
    if (!started) return std::unexpected(Fail("sdk.lua.plan_invalid", "initial system was not adopted"));
    for (const auto& [revision, chain] : ledger.revision_chains) {
        (void)revision;
        if (!matches(ledger.FindMessage(chain.first)))
            return std::unexpected(Fail("sdk.lua.plan_invalid", "adopted system lost its Lua binding"));
    }
    if (!matches(ledger.FindMessage(ledger.context.system_message_ref)))
        return std::unexpected(Fail("sdk.lua.plan_invalid", "effective system lost its Lua binding"));
    return {};
}
std::expected<SessionLua::Json, std::string> SessionLua::Open(const lubancode::trajectory::V3OpeningContext& context) {
    const auto fail = [](const Error& error) -> std::expected<Json, std::string> {
        return std::unexpected(error.code + ": " + error.message);
    };
    if (snapshot_.enabled && !tools_taken_) return std::unexpected("sdk.lua.open_failed: tools were not transferred");
    auto bytes = ReadPlan(owned_root_, context.session_dir);
    if (!bytes) return fail(bytes.error());
    if (context.source) {
        if (context.session_id != resume_id_ || context.session_dir != resume_dir_)
            return std::unexpected("sdk.lua.resume_mismatch: locked identity changed");
        if (legacy_ ? bytes->has_value() : !bytes->has_value() || **bytes != saved_bytes_)
            return std::unexpected("sdk.lua.plan_invalid: plan changed before locked opening");
        auto valid = CheckBinding(*context.source);
        if (!valid) return fail(valid.error());
    } else {
        if (!resume_id_.empty()) return std::unexpected("sdk.lua.resume_mismatch: resume became new");
        if (bytes->has_value()) return std::unexpected("sdk.lua.plan_invalid: new plan already exists");
        const auto text = Plan(context.session_id).dump();
        if (text.size() > kMaxPlan) return std::unexpected("sdk.lua.plan_invalid: plan exceeds 256 KiB");
        auto written = WriteFrozenPlan(context.session_dir / kPlanFile, text);
        if (!written || written->outcome != platform::WriteOutcome::CommittedDurable) {
            const auto outcome = written ? PlanWriteOutcomeName(written->outcome) : PlanWriteOutcomeName(written.error().outcome);
            return std::unexpected("sdk.lua.plan_write_failed: " + Json{{"outcome", outcome},
                {"atomicCode", written ? "" : written.error().code},
                {"message", written ? "plan receipt was not confirmed durable" : written.error().message}}
                .dump(-1, ' ', false, Json::error_handler_t::replace));
        }
        snapshot_.plan_sha256 = platform::Sha256Hex(text);
    }
    snapshot_.session_id = context.session_id;
    return legacy_ ? Json::object() : Json{{"hostBindings", {{"lua", {{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}}}}}};
}
lubancode::trajectory::V3OpeningParticipant SessionLua::OpeningParticipant() {
    return [owner = shared_from_this()](const auto& context) { return owner->Open(context); };
}

} // namespace lubancore::detail
