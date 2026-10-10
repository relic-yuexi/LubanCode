#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

#include "app/tool_assembly_plan.hpp"
#include "platform/paths.hpp"

namespace lubancode::test_support {

// Every root is explicit. This fixture never changes HOME, USERPROFILE or cwd.
struct ToolAssemblyFixture {
    std::filesystem::path root;
    ToolAssemblyFixture() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() /
            ("tool assembly " + std::to_string(std::chrono::steady_clock::now()
                 .time_since_epoch().count()) + " " + std::to_string(++serial));
        for (const auto& path : {Project(), Plugins(), root / "state"}) {
            std::filesystem::create_directories(path);
        }
    }
    ~ToolAssemblyFixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    ToolAssemblyFixture(const ToolAssemblyFixture&) = delete;
    ToolAssemblyFixture& operator=(const ToolAssemblyFixture&) = delete;

    std::filesystem::path Project() const { return root / "project with spaces"; }
    std::filesystem::path Plugins() const { return root / "user plugins"; }
    app::ToolAssemblyPlan Plan() const {
        app::ToolAssemblyPlan plan;
        plan.cwd_utf8 = platform::PathToUtf8(Project());
        plan.user_plugins_dir = Plugins();
        plan.plugin_trust_path = root / "state" / "plugin-trust.json";
        plan.package_data_root = root / "state" / "package-data";
        plan.ptc_profile_store_path = root / "state" / "ptc-profile.json";
        return plan;
    }
};

}  // namespace lubancode::test_support
