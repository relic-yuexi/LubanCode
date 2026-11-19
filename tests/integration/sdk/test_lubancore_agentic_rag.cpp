#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace lubancore_consumer {
void AgenticRagCase(const std::string&, const std::filesystem::path&);
}
namespace {
struct Directory {
    std::filesystem::path path;
    Directory() {
        static std::atomic<unsigned> next{0};
        path = std::filesystem::temp_directory_path() / ("sdk-agentic-rag-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(next++));
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
void Run(const std::string& name) {
    Directory directory;
    REQUIRE_NOTHROW(lubancore_consumer::AgenticRagCase(name, directory.path));
}
}
TEST_CASE("SDK Agentic RAG uses actual retrieval replies to choose query count and answer") { Run("retrieval"); }
TEST_CASE("SDK Agentic RAG keeps owned sources rejects bad query and missing evidence") { Run("sources"); }
TEST_CASE("SDK Agentic RAG preserves default preview and denies Node full escalation") { Run("preview"); }
TEST_CASE("SDK Agentic RAG isolates overlapping same-project corpus approvals and cancellation") { Run("isolation"); }
TEST_CASE("SDK Agentic RAG restores same-ID actual tool history without replaying retrieval") { Run("recovery"); }
TEST_CASE("SDK Agentic RAG joins in-flight retrieval before closing and destroying owners") { Run("lifetime"); }
