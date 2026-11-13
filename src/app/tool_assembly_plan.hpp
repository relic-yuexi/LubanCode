#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "agent/agent_catalog.hpp"
#include "approval_mode.hpp"

namespace lubancode::app {

enum class ToolAssemblyDiagnosticSeverity { Info, Notice, Warning };
enum class ToolAssemblyDiagnosticScope { Shared, Main, Sub };

// Values, not rendered terminal lines. Codes identify the outcome; stage and
// component identify its source. Arguments retain the original diagnostic data.
struct ToolAssemblyDiagnostic {
    std::string code;
    ToolAssemblyDiagnosticSeverity severity = ToolAssemblyDiagnosticSeverity::Info;
    std::string stage;
    std::string component;
    std::vector<std::string> arguments;
    ToolAssemblyDiagnosticScope scope = ToolAssemblyDiagnosticScope::Shared;
};

// Presentation only, called synchronously during construction and never kept by
// ToolRuntime. Repeated sub-registry reports are stored but not presented twice.
using ToolAssemblyDiagnosticSink = std::function<void(const ToolAssemblyDiagnostic&)>;
using ToolAssemblyDiagnosticReporter = std::function<void(ToolAssemblyDiagnostic, bool announce)>;

// Host-interpreted assembly input. Missing roots disable that source; missing
// suppliers never fall back to process cwd, home, or CLI permission globals.
// Supplied filesystem paths must be absolute and valid UTF-8; they need not
// exist yet. Validation precedes process launch and filesystem mutation.
// Suppliers are retained by tools and must own their captures, or borrow host
// state that outlives ToolRuntime. The cwd is frozen for startup resources such
// as MCP processes; later SetPluginCwd does not move an existing MCP process.
struct ToolAssemblyPlan {
    std::string cwd_utf8;
    std::optional<std::filesystem::path> user_plugins_dir;
    std::optional<std::filesystem::path> plugin_trust_path;
    std::optional<std::filesystem::path> package_data_root;
    std::optional<std::filesystem::path> ptc_profile_store_path;
    bool ptc_sandbox_exempt = false;
    // Returned directory roots are checked on every use, before scanning; a
    // supplier may change roots but may not introduce cwd-relative discovery.
    std::function<agent::AgentCatalogScanRoots()> agent_scan_roots;
    std::function<ApprovalMode()> parent_permission;
    std::function<std::string()> env_appendix_probe;
};

}  // namespace lubancode::app
