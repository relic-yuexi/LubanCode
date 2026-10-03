#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lubancore::lua::v1 {

struct Script {
    std::string path; // Exact UTF-8 path relative to root; standalone .lua only.
    std::string tool_name; // Full plugin__<stem>__<script name>, never renamed.
};

// Explicit trusted local scripts. All three budgets must be positive. Admission
// does not grant filesystem/process/network APIs, Package, or child tools.
// Hook errors remain catchable by protected Lua calls. Scripts must cooperate
// with cancellation; Close waits for actual return rather than forcing exit.
struct Selection {
    std::string root; // Absolute UTF-8 ordinary directory, with no ambient fallback.
    std::vector<Script> scripts;
    std::uint64_t instruction_budget = 0;
    std::size_t memory_cap_bytes = 0;
    std::chrono::milliseconds wall_budget{0};
};

struct Entry {
    std::string path;
    std::string tool_name;
    std::string description;
    std::string content_sha256;
    std::string input_schema_json;
};

// Owned local metadata. Close retains this value without keeping a VM, registry,
// or live writer. Fingerprints are integrity checks, not authentication. A resume
// constructs new VMs from matching source; Lua globals/closures are not restored.
struct Snapshot {
    bool enabled = false;
    std::string session_id;
    std::string root;
    std::vector<Entry> entries;
    std::uint64_t instruction_budget = 0;
    std::size_t memory_cap_bytes = 0;
    std::chrono::milliseconds wall_budget{0};
    std::string plan_sha256;
};

} // namespace lubancore::lua::v1
