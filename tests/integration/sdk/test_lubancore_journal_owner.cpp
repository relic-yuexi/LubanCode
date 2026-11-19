#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace lubancore_consumer {
std::string JournalOwnerCase(const std::string&, const std::filesystem::path&);
}
namespace {
struct Directory {
    std::filesystem::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() / ("sdk-journal-public-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        REQUIRE(std::filesystem::create_directory(root));
    }
    ~Directory() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};
void Run(const char* path) {
    Directory directory; REQUIRE_NOTHROW(lubancore_consumer::JournalOwnerCase(path, directory.root));
}
}
TEST_CASE("SDK Journal owner: a new host completes Close and a fresh Runtime same-ID turn") { Run("roundtrip"); }
TEST_CASE("SDK Journal owner: closed result reader cannot retain or replace the live owner") { Run("close-read"); }
TEST_CASE("SDK Journal owner: same project Sessions retain separate history and execution") { Run("isolation"); }
