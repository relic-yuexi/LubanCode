#pragma once

#include <string>
#include <vector>

namespace lubancore::skills::v1 {

// Trusted, explicit local materials. No ambient discovery or script execution.
struct Selection {
    std::string root; // absolute UTF-8 directory
    std::vector<std::string> names; // exact standalone names; no package aliases
};

struct Entry {
    std::string name;
    std::string description;
    std::string directory;
    std::string content_sha256;
    std::vector<std::string> requires_tools;
    std::vector<std::string> missing_tools;
};

// A value snapshot. Paths are local trusted-host metadata, not remote exports.
// Close retains it without keeping an Agent, registry or live journal writer.
struct Snapshot {
    bool enabled = false;
    std::string session_id;
    std::string root;
    std::vector<Entry> entries;
    std::vector<std::string> tool_names;
    std::string prompt_segment;
    std::string plan_sha256; // integrity binding, not storage authentication
};

} // namespace lubancore::skills::v1
