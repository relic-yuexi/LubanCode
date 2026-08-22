// EmbeddedLuaRuntime 的实现:目录扫描 + 在册台账 + 取消链分发。
#include "runtime/plugin_lua.hpp"

#include <algorithm>
#include <cctype>

#include "platform/paths.hpp"

namespace lubancode::runtime {

namespace {

std::string PathToUtf8Local(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace

std::vector<std::string> EmbeddedLuaRuntime::LoadDirectory(const std::filesystem::path& dir,
                                                           const tools::LuaProfile& profile) {
    std::vector<std::string> warnings;
    if (loaded_) {
        return warnings;  // 幂等:第二遍是给别的 registry 造 adapter 用的,不重扫
    }
    loaded_ = true;
    auto scan = tools::LoadLuaPlugins(dir, profile);
    warnings = scan.warnings;
    for (auto& tool : scan.tools) {
        LuaPluginRecord record;
        record.id = tool->stem();
        record.version = "legacy";
        record.tool_name = tool->name();
        record.tool = tool.get();
        records_.push_back(record);
        tools_.push_back(std::move(tool));
    }
    return warnings;
}

std::vector<std::unique_ptr<LuaToolAdapter>> EmbeddedLuaRuntime::MakeAdapters() const {
    std::vector<std::unique_ptr<LuaToolAdapter>> out;
    out.reserve(tools_.size());
    for (const auto& tool : tools_) {
        out.push_back(std::make_unique<LuaToolAdapter>(*tool));
    }
    return out;
}

void EmbeddedLuaRuntime::SetCancel(const std::atomic<bool>* cancel) {
    for (auto& tool : tools_) {
        tool->SetCancel(cancel);
    }
}

}  // namespace lubancode::runtime
