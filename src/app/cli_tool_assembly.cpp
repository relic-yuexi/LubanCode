#include "app/cli_tool_assembly.hpp"

#include <cstdlib>
#include <utility>

#include "app/commands/agent_commands.hpp"
#include "app/tool_runtime.hpp"
#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "config/config.hpp"
#include "tools/path_utils.hpp"
#include "tools/subagent_env_appendix.hpp"

namespace lubancode::app {

ToolAssemblyPlan ResolveCliToolAssemblyPlan(const std::string& cwd_utf8) {
    ToolAssemblyPlan plan;
    plan.cwd_utf8 = cwd_utf8;
    if (const auto home = config::HomeLubancodeDir()) {
        const auto root = tools::Utf8ToPath(*home);
        plan.user_plugins_dir = root / "plugins";
        // Profiles are user configuration, not mutable runtime data.
        plan.ptc_profile_store_path = root / "ptc_profiles.json";
    }
    if (const auto state = config::StateRootDir()) {
        const auto root = tools::Utf8ToPath(*state);
        plan.package_data_root = root / "package-data";
        plan.plugin_trust_path = root / "plugin-trust.json";
    }
#ifdef _WIN32
    plan.ptc_sandbox_exempt = true;
#else
    const char* exempt = std::getenv("LUBANCODE_PTC_ALLOW_NO_SANDBOX");
    plan.ptc_sandbox_exempt = exempt != nullptr && exempt[0] != '\0';
#endif
    // These are intentional CLI live-state suppliers. Each dispatch observes
    // the current workspace and permission mode, as before this extraction.
    plan.agent_scan_roots = [] { return ComputeAgentScanRoots(); };
    plan.parent_permission = [] { return cli::ToApprovalMode(cli::CurrentConfirmMode()); };
    plan.env_appendix_probe = [cwd_utf8] {
        try {
            const auto repo_root = cli::FindRepositoryRoot(tools::Utf8ToPath(cwd_utf8));
            return repo_root ? tools::ComposeSubagentEnvAppendix(tools::DetectSubagentEnvFacts(*repo_root))
                             : std::string();
        } catch (...) {
            return std::string();
        }
    };
    return plan;
}

ToolAssemblyDiagnosticSink MakeCliToolAssemblyDiagnosticSink(cli::Theme theme, std::ostream& output) {
    return [theme = std::move(theme), &output](const ToolAssemblyDiagnostic& diagnostic) {
        std::string text;
        if (diagnostic.code == "plugin.warning" || diagnostic.code == "plugin.trust_load_failed" ||
            diagnostic.code == "package.mount_failed") {
            if (!diagnostic.arguments.empty()) text = diagnostic.arguments.front();
        } else if (diagnostic.code == "package.note") {
            text = "[package] " + (diagnostic.arguments.empty() ? std::string() : diagnostic.arguments.front());
        } else {
            text = cli::TrFormat(diagnostic.code, diagnostic.arguments);
        }
        switch (diagnostic.severity) {
            case ToolAssemblyDiagnosticSeverity::Info: output << text << "\n"; break;
            case ToolAssemblyDiagnosticSeverity::Notice: output << theme.stats << text << theme.reset << "\n"; break;
            case ToolAssemblyDiagnosticSeverity::Warning: output << theme.error << text << theme.reset << "\n"; break;
        }
    };
}

std::optional<tools::CustomAgentMaterial> ResolveCustomAgentMaterial(
    const std::vector<tools::SkillMeta>& skills, const package::PackageSnapshot* snapshot,
    const std::string& name) {
    return ResolveCustomAgentMaterial(skills, snapshot, name, ComputeAgentScanRoots());
}

}  // namespace lubancode::app
