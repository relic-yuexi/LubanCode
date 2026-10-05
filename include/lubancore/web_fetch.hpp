#pragma once

#include <lubancore/api.hpp>

#include <cstdint>
#include <string>

namespace lubancore::web_fetch::v1 {

// Owned capabilities of this SDK's built-in transport, read without networking.
// Failure to initialize/query is an error, never a false capability value.
struct Capabilities { bool gzip_decoding = false; };
LUBANCORE_API Result<Capabilities> DescribeCapabilities();

// Owned per-opening limits. Select "web_fetch" in builtin_tools to admit it.
// Model max_bytes can only narrow the returned body, never these host limits.
// Explicit options without selecting the tool are rejected before opening.
struct Options {
    std::string user_agent = "lubancore";
    std::int64_t connect_timeout_ms = 10'000;
    std::int64_t total_timeout_ms = 30'000;
    std::uint64_t max_header_bytes = 64 * 1024;
    std::uint64_t max_download_bytes = 4 * 1024 * 1024;
    std::uint64_t max_output_bytes = 100 * 1024;
    std::uint32_t max_redirects = 5;
};

} // namespace lubancore::web_fetch::v1
