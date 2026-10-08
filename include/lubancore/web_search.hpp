#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lubancore::web_search::v1 {
enum class Provider { Tavily, Brave, Serper };
// Explicit host-owned per-opening values. Credentials are not persisted and
// must be supplied again on resume. No ambient CLI config/environment lookup.
struct Options {
    Provider provider = Provider::Tavily;
    std::string api_key;
    // Empty selects the provider's built-in HTTPS endpoint. An override must
    // be HTTPS, or HTTP at a literal loopback address; no userinfo/query/fragment.
    std::string endpoint;
    std::int64_t connect_timeout_ms = 10000;
    std::int64_t total_timeout_ms = 30000;
    std::uint64_t max_header_bytes = 64 * 1024;
    std::uint64_t max_response_bytes = 4 * 1024 * 1024;
    std::uint64_t max_output_bytes = 100 * 1024;
    std::size_t max_query_bytes = 4096;
    int max_results = 10;
};
} // namespace lubancore::web_search::v1
